use super::*;
use lyflow_client::Frame;
use sha2::{Digest, Sha256};
use std::fs::{File, OpenOptions};
use std::io::Read;
use std::sync::Weak;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

mod text;
mod wire;

const MAX_INDEX: u64 = 8 * 1024 * 1024;
const DEFAULT_BYTES: u64 = 1024 * 1024 * 1024;

fn active() -> &'static Mutex<Vec<Weak<AtomicBool>>> {
    static ACTIVE: OnceLock<Mutex<Vec<Weak<AtomicBool>>>> = OnceLock::new();
    ACTIVE.get_or_init(|| Mutex::new(Vec::new()))
}

pub(super) fn cancel_active() -> bool {
    let mut any = false;
    if let Ok(mut flags) = active().lock() {
        flags.retain(|weak| {
            if let Some(flag) = weak.upgrade() {
                flag.store(true, Ordering::SeqCst);
                any = true;
                true
            } else {
                false
            }
        });
    }
    any
}

struct Cancellation {
    flag: Arc<AtomicBool>,
    group: Arc<RunGroup>,
}
impl Cancellation {
    fn new() -> Self {
        console::install();
        let flag = Arc::new(AtomicBool::new(false));
        active()
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .push(Arc::downgrade(&flag));
        Self {
            flag,
            group: Arc::new(RunGroup::default()),
        }
    }
    fn cancelled(&self) -> bool {
        self.flag.load(Ordering::SeqCst) || self.group.cancelled()
    }
    fn check(&self) -> Result<(), String> {
        if self.cancelled() {
            Err("cancelled".into())
        } else {
            Ok(())
        }
    }
    fn wait(&self, duration: Duration) -> Result<(), String> {
        let end = Instant::now() + duration;
        while Instant::now() < end {
            self.check()?;
            std::thread::sleep(
                end.saturating_duration_since(Instant::now())
                    .min(Duration::from_millis(20)),
            );
        }
        self.check()
    }
}

fn now_ns() -> Result<u64, String> {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_err(|e| e.to_string())?
        .as_nanos()
        .try_into()
        .map_err(|_| "host timestamp exceeds u64".into())
}
fn integer(parsed: &Parsed, name: &str, default: u64, max: u64) -> Result<u64, String> {
    let value = parsed
        .one(name)
        .map(str::parse::<u64>)
        .transpose()
        .map_err(|_| format!("--{name} must be an unsigned integer"))?
        .unwrap_or(default);
    if value > max {
        Err(format!("--{name} exceeds {max}"))
    } else {
        Ok(value)
    }
}
fn max_bytes(parsed: &Parsed) -> Result<u64, String> {
    let n = integer(parsed, "max-bytes", DEFAULT_BYTES, 2 * DEFAULT_BYTES)?;
    if n == 0 {
        Err("--max-bytes must be positive".into())
    } else {
        Ok(n)
    }
}
fn required<'a>(parsed: &'a Parsed, name: &str) -> Result<&'a str, String> {
    parsed
        .one(name)
        .filter(|s| !s.is_empty())
        .ok_or_else(|| format!("--{name} is required"))
}
fn write_new(path: &Path, bytes: &[u8]) -> Result<(), String> {
    let mut file = OpenOptions::new()
        .create_new(true)
        .write(true)
        .open(path)
        .map_err(|e| format!("{}: {e}", path.display()))?;
    let result = file
        .write_all(bytes)
        .and_then(|_| file.sync_all())
        .map_err(|e| e.to_string());
    drop(file);
    if result.is_err() {
        let _ = std::fs::remove_file(path);
    }
    result
}
fn read_small(path: &Path, max: u64) -> Result<Vec<u8>, String> {
    let mut bytes = Vec::new();
    File::open(path)
        .map_err(|e| e.to_string())?
        .take(max + 1)
        .read_to_end(&mut bytes)
        .map_err(|e| e.to_string())?;
    if bytes.len() as u64 > max {
        Err(format!("{} exceeds {max} bytes", path.display()))
    } else {
        Ok(bytes)
    }
}
fn parse_json(bytes: &[u8]) -> Result<Value, String> {
    serde_json::from_slice(bytes).map_err(|e| e.to_string())
}

fn run_errors(result: &RunResult) -> Value {
    Value::Array(
        result
            .events
            .iter()
            .filter(|event| event.get("error").is_some() || event.get("diagnostics").is_some())
            .take(8)
            .cloned()
            .collect(),
    )
}
fn hash_file(path: &Path, max: u64, cancel: &Cancellation) -> Result<(u64, String), String> {
    let mut file = File::open(path).map_err(|e| e.to_string())?;
    let mut digest = Sha256::new();
    let mut buf = [0u8; 65536];
    let mut bytes = 0u64;
    loop {
        cancel.check()?;
        let n = file.read(&mut buf).map_err(|e| e.to_string())?;
        if n == 0 {
            break;
        }
        bytes = bytes.checked_add(n as u64).ok_or("file size overflow")?;
        if bytes > max {
            return Err("archive exceeds max-bytes".into());
        }
        digest.update(&buf[..n]);
    }
    Ok((bytes, format!("{:x}", digest.finalize())))
}

pub(super) fn command(command: &str, parsed: &Parsed, out: &Sink, err: &Sink) -> i32 {
    let cancel = Cancellation::new();
    let result = match command {
        "record" => record(parsed, out, err, &cancel),
        "replay" => replay(parsed, out, err, &cancel),
        "publish" => publish(parsed, out, &cancel),
        "log" => log(parsed, out, &cancel),
        _ => unreachable!(),
    };
    match result {
        Ok(()) => EXIT_OK,
        Err(e) => fail(
            err,
            &e,
            if cancel.cancelled() || e == "cancelled" {
                EXIT_CANCELLED
            } else {
                EXIT_FAILED
            },
        ),
    }
}

fn record(parsed: &Parsed, out: &Sink, err: &Sink, cancel: &Cancellation) -> Result<(), String> {
    let path = parsed.positional.first().ok_or("record requires a graph")?;
    let dir = PathBuf::from(required(parsed, "recording")?);
    let count = integer(parsed, "count", 1, 10_000)?;
    if count == 0 {
        return Err("--count must be positive".into());
    }
    let interval = integer(parsed, "interval-ms", 0, 3_600_000)?;
    if count != 1 && parsed.one("timestamp-ns").is_some() {
        return Err(
            "--timestamp-ns requires --count 1; repeated acquisitions use host_capture timestamps"
                .into(),
        );
    }
    let limit = max_bytes(parsed)?;
    let total_limit = integer(
        parsed,
        "max-total-bytes",
        4 * DEFAULT_BYTES,
        64 * DEFAULT_BYTES,
    )?;
    if total_limit == 0 {
        return Err("--max-total-bytes must be positive".into());
    }
    let core = core()?;
    let loaded = load_graph(parsed, path, err)?;
    let inputs = load_inputs(&core, parsed)?;
    let diagnostics: Vec<_> = diagnostics_of(&core, &loaded)?
        .into_iter()
        .filter(|d| {
            !(d["code"] == "missing_input"
                && inputs
                    .iter()
                    .any(|input| d["nodeId"] == input.node_id && d["portName"] == input.port))
        })
        .collect();
    if has_errors(&diagnostics) {
        return Err(format!("graph validation failed: {}", json!(diagnostics)));
    }
    let plan = parse_json(
        core.plan_with_inputs(
            &loaded.json,
            &loaded.base_dir,
            parsed.many("to"),
            None,
            &inputs,
            &[],
        )
        .map_err(|e| e.to_string())?
        .as_bytes(),
    )?;
    let sources: Vec<String> = plan
        .as_array()
        .ok_or("core plan must be an array")?
        .iter()
        .filter(|n| n["level"] == 0)
        .map(|n| {
            n["nodeId"]
                .as_str()
                .map(str::to_owned)
                .ok_or("plan source has no nodeId".to_string())
        })
        .collect::<Result<_, _>>()?;
    if sources.is_empty() {
        return Err(
            "no provable level-zero source nodes; record requires a nonempty execution plan".into(),
        );
    }
    let effective = core
        .effective_params_with_inputs(&loaded.json, &loaded.base_dir, None, &inputs, &[])
        .map_err(|e| e.to_string())?;
    if parse_json(effective.as_bytes())?.is_array() {
        return Err(format!(
            "effective parameters could not be resolved: {effective}"
        ));
    }
    std::fs::create_dir(&dir)
        .map_err(|e| format!("recording must be a new directory {}: {e}", dir.display()))?;
    write_new(&dir.join("graph.lyflow.json"), loaded.json.as_bytes())?;
    write_new(&dir.join("effective-params.json"), effective.as_bytes())?;
    let mut index = json!({"type":"lyflow.Recording.v1","schemaVersion":1,"complete":false,"graph":"graph.lyflow.json","baseDir":loaded.base_dir,"effectiveParams":"effective-params.json","sourceNodes":sources,"request":{"targets":parsed.many("to"),"inputs":parsed.many("input"),"count":count,"intervalMs":interval},"frames":[]});
    index["graphSha256"] = json!(format!("{:x}", Sha256::digest(loaded.json.as_bytes())));
    index["effectiveParamsSha256"] = json!(format!("{:x}", Sha256::digest(effective.as_bytes())));
    checkpoint(&dir, &index)?;
    let parallel = parallel_of(parsed)?;
    let mut total_bytes = 0u64;
    for sequence in 0..count {
        cancel.check()?;
        if sequence > 0 {
            cancel.wait(Duration::from_millis(interval))?;
        }
        let result = execute(
            &core,
            RunRequest {
                graph_json: &loaded.json,
                base_dir: &loaded.base_dir,
                targets: parsed.many("to"),
                parallel,
                preview_points: 0,
                preview: false,
                no_cache: parsed.has("no-cache"),
                stream: None,
                params_json: None,
                inputs: &inputs,
                group: Some(&cancel.group),
                on_event: None,
            },
        )?;
        cancel.check()?;
        if result.status != "ok" {
            return Err(format!(
                "recording run {sequence} finished {}: {}",
                result.status,
                run_errors(&result)
            ));
        }
        let timestamp = integer(parsed, "timestamp-ns", now_ns()?, u64::MAX)?;
        let metadata = json!({"type":"lyflow.Frame.v1","schemaVersion":1,"frameId":ulid::new(),"timestampNs":timestamp.to_string(),"frame":parsed.one("frame").unwrap_or(""),"calibrationVersion":parsed.one("calibration-version"),"context":{"timestampKind":if parsed.one("timestamp-ns").is_some(){"explicit"}else{"host_capture"},"sourceNodes":sources,"recordingSequence":sequence.to_string()}});
        let frame = core
            .capture_frame(
                result.run_id(),
                &metadata.to_string(),
                &json!({"maxBytes":limit}).to_string(),
            )
            .map_err(|e| e.to_string())?;
        let name = format!("frame-{sequence:06}.lyframe");
        let archive = dir.join(&name);
        frame
            .save_cancellable(&archive, false, &|| cancel.cancelled())
            .map_err(|e| e.to_string())?;
        let (bytes, sha) = hash_file(&archive, limit, cancel)?;
        if bytes > total_limit.saturating_sub(total_bytes) {
            std::fs::remove_file(&archive).map_err(|e| e.to_string())?;
            return Err(
                "recording exceeds max-total-bytes; prior indexed frames remain valid".into(),
            );
        }
        total_bytes += bytes;
        let actual = parse_json(frame.metadata().map_err(|e| e.to_string())?.as_bytes())?;
        let row = json!({"sequence":sequence.to_string(),"file":name,"bytes":bytes.to_string(),"sha256":sha,"metadata":actual});
        index["frames"].as_array_mut().unwrap().push(row.clone());
        checkpoint(&dir, &index)?;
        json_line(
            out,
            &json!({"type":"lyflow.Recorded.v1","recording":dir,"frame":row}),
        );
    }
    index["complete"] = json!(true);
    checkpoint(&dir, &index)
}

fn checkpoint(dir: &Path, value: &Value) -> Result<(), String> {
    let bytes = serde_json::to_vec_pretty(value).map_err(|e| e.to_string())?;
    if bytes.len() as u64 > MAX_INDEX {
        return Err("recording index exceeds 8 MiB".into());
    }
    let temp = dir.join(format!("index-{}.tmp", ulid::new()));
    write_new(&temp, &bytes)?;
    let destination = dir.join("index.json");
    let result = replace_index(&temp, &destination);
    if result.is_err() {
        let _ = std::fs::remove_file(&temp);
    }
    result
}

#[cfg(not(windows))]
fn replace_index(source: &Path, target: &Path) -> Result<(), String> {
    std::fs::rename(source, target).map_err(|e| e.to_string())
}
#[cfg(windows)]
fn replace_index(source: &Path, target: &Path) -> Result<(), String> {
    use std::os::windows::ffi::OsStrExt;
    #[link(name = "kernel32")]
    extern "system" {
        fn MoveFileExW(source: *const u16, target: *const u16, flags: u32) -> i32;
    }
    let source: Vec<u16> = source.as_os_str().encode_wide().chain(Some(0)).collect();
    let target: Vec<u16> = target.as_os_str().encode_wide().chain(Some(0)).collect();
    if unsafe { MoveFileExW(source.as_ptr(), target.as_ptr(), 1 | 8) } == 0 {
        Err(std::io::Error::last_os_error().to_string())
    } else {
        Ok(())
    }
}

#[derive(Debug)]
struct Recording {
    dir: PathBuf,
    index: Value,
    rows: Vec<Value>,
}
impl Recording {
    fn open(parsed: &Parsed) -> Result<Self, String> {
        let path = parsed
            .positional
            .first()
            .ok_or("command requires a recording directory")?;
        let dir = Path::new(path).canonicalize().map_err(|e| e.to_string())?;
        let index = parse_json(&read_small(&dir.join("index.json"), MAX_INDEX)?)?;
        if index["type"] != "lyflow.Recording.v1" || index["schemaVersion"] != 1 {
            return Err("unsupported recording index".into());
        }
        let first = integer(parsed, "from-sequence", 0, u64::MAX)?;
        let last = integer(parsed, "to-sequence", u64::MAX, u64::MAX)?;
        let first_time = integer(parsed, "from-timestamp-ns", 0, u64::MAX)?;
        let last_time = integer(parsed, "to-timestamp-ns", u64::MAX, u64::MAX)?;
        if first_time > last_time {
            return Err("from-timestamp-ns exceeds to-timestamp-ns".into());
        }
        if first > last {
            return Err("from-sequence exceeds to-sequence".into());
        }
        let all = index["frames"]
            .as_array()
            .ok_or("recording frames must be an array")?;
        let mut rows = Vec::new();
        let mut previous = None;
        let mut ids = HashSet::new();
        for row in all {
            let sequence = decimal(&row["sequence"], "sequence")?;
            let timestamp = decimal(&row["metadata"]["timestampNs"], "timestampNs")?;
            let id = row["metadata"]["frameId"]
                .as_str()
                .filter(|s| !s.is_empty())
                .ok_or("frameId required")?;
            if !ids.insert(id) || previous.is_some_and(|(s, t)| sequence <= s || timestamp < t) {
                return Err(
                    "recording frame IDs, sequence or timestamps are not ordered and unique".into(),
                );
            }
            previous = Some((sequence, timestamp));
            if sequence >= first
                && sequence <= last
                && timestamp >= first_time
                && timestamp <= last_time
            {
                rows.push(row.clone());
            }
        }
        if rows.is_empty() {
            return Err("recording selection is empty".into());
        }
        Ok(Self { dir, index, rows })
    }
    fn path(&self, name: &str) -> Result<PathBuf, String> {
        let path = Path::new(name);
        if path.is_absolute()
            || path
                .components()
                .any(|c| !matches!(c, std::path::Component::Normal(_)))
        {
            return Err("recording path must be a relative descendant".into());
        }
        let full = self
            .dir
            .join(path)
            .canonicalize()
            .map_err(|e| e.to_string())?;
        if !full.starts_with(&self.dir) {
            return Err("recording path escapes its directory".into());
        }
        Ok(full)
    }
    fn load(
        &self,
        core: &Arc<Core>,
        row: &Value,
        limit: u64,
        cancel: &Cancellation,
    ) -> Result<(Frame, PathBuf), String> {
        let path = self.path(row["file"].as_str().ok_or("frame file missing")?)?;
        let (bytes, hash) = hash_file(&path, limit, cancel)?;
        if decimal(&row["bytes"], "bytes")? != bytes || row["sha256"] != hash {
            return Err("recorded archive integrity mismatch".into());
        }
        let frame = core
            .load_frame_cancellable(&path, limit, &|| cancel.cancelled())
            .map_err(|e| e.to_string())?;
        let metadata = parse_json(frame.metadata().map_err(|e| e.to_string())?.as_bytes())?;
        for key in [
            "frameId",
            "timestampNs",
            "frame",
            "calibrationVersion",
            "context",
            "session",
            "sourceRunId",
        ] {
            if metadata[key] != row["metadata"][key] {
                return Err(format!("recording index and archive disagree on {key}"));
            }
        }
        Ok((frame, path))
    }
}
fn decimal(value: &Value, name: &str) -> Result<u64, String> {
    value
        .as_str()
        .filter(|s| !s.is_empty() && s.bytes().all(|c| c.is_ascii_digit()))
        .and_then(|s| s.parse().ok())
        .ok_or_else(|| format!("{name} must be a decimal u64 string"))
}

fn replay(parsed: &Parsed, out: &Sink, err: &Sink, cancel: &Cancellation) -> Result<(), String> {
    let recording = Recording::open(parsed)?;
    let core = core()?;
    let limit = max_bytes(parsed)?;
    let recompute = parsed.has("recompute") || parsed.one("graph").is_some();
    let loaded = if recompute {
        let path = if let Some(graph) = parsed.one("graph") {
            PathBuf::from(graph)
        } else {
            recording.path(
                recording.index["graph"]
                    .as_str()
                    .ok_or("recording graph is missing")?,
            )?
        };
        if parsed.one("graph").is_none() {
            let bytes = read_small(&path, 16 * 1024 * 1024)?;
            if recording.index["graphSha256"] != format!("{:x}", Sha256::digest(&bytes)) {
                return Err("recorded graph integrity mismatch".into());
            }
        }
        Some(load_graph(parsed, &path.to_string_lossy(), err)?)
    } else {
        None
    };
    if parsed.one("graph").is_some() && parsed.many("bind").is_empty() {
        return Err("explicit --graph requires --bind sourceNode.sourcePort=nodeId.port".into());
    }
    let first = decimal(&recording.rows[0]["metadata"]["timestampNs"], "timestampNs")?;
    let start = Instant::now();
    for row in &recording.rows {
        cancel.check()?;
        if parsed.has("paced") {
            let elapsed = Duration::from_nanos(
                decimal(&row["metadata"]["timestampNs"], "timestampNs")? - first,
            );
            cancel.wait(elapsed.saturating_sub(start.elapsed()))?;
        }
        let (frame, _) = recording.load(&core, row, limit, cancel)?;
        if let Some(loaded) = &loaded {
            let bindings = bindings(&frame, parsed.many("bind"), &recording.index["sourceNodes"])?;
            let base = if parsed.one("graph").is_some() {
                loaded.base_dir.as_str()
            } else {
                parsed
                    .one("base-dir")
                    .or_else(|| recording.index["baseDir"].as_str())
                    .unwrap_or(loaded.base_dir.as_str())
            };
            let result = execute_frame(
                &core,
                RunRequest {
                    graph_json: &loaded.json,
                    base_dir: base,
                    targets: parsed.many("to"),
                    parallel: parallel_of(parsed)?,
                    preview_points: 0,
                    preview: false,
                    no_cache: parsed.has("no-cache"),
                    stream: None,
                    params_json: None,
                    inputs: &[],
                    group: Some(&cancel.group),
                    on_event: None,
                },
                Some((&frame, &bindings.to_string())),
            )?;
            json_line(
                out,
                &json!({"type":"lyflow.Replayed.v1","sequence":row["sequence"],"metadata":row["metadata"],"mode":"recompute","runId":result.run_id(),"status":result.status,"outputs":parse_json(core.run_outputs(result.run_id()).map_err(|e|e.to_string())?.as_bytes())?}),
            );
            if result.status != "ok" {
                return Err(format!(
                    "replay run finished {}: {}",
                    result.status,
                    run_errors(&result)
                ));
            }
        } else {
            json_line(
                out,
                &json!({"type":"lyflow.Replayed.v1","sequence":row["sequence"],"metadata":parse_json(frame.metadata().map_err(|e|e.to_string())?.as_bytes())?,"mode":"recorded","outputs":parse_json(core.run_outputs(frame.run_id()).map_err(|e|e.to_string())?.as_bytes())?}),
            );
        }
    }
    Ok(())
}

fn bindings(frame: &Frame, specs: &[String], sources: &Value) -> Result<Value, String> {
    let metadata = parse_json(frame.metadata().map_err(|e| e.to_string())?.as_bytes())?;
    let entries = metadata["entries"]
        .as_array()
        .ok_or("frame lacks typed output addresses")?;
    let mut bindings = Vec::new();
    let mut destinations = HashSet::new();
    if specs.is_empty() {
        if &metadata["context"]["sourceNodes"] != sources {
            return Err("recording source plan and frame source declaration disagree; provide explicit --bind".into());
        }
        let sources = sources
            .as_array()
            .filter(|a| !a.is_empty())
            .ok_or("no proven source nodes; provide explicit --bind")?;
        for node in sources {
            let node = node.as_str().ok_or("invalid source node")?;
            let has_input_injection = metadata["injectedInputs"].as_array().is_some_and(|items| {
                items.iter().any(|binding| {
                    binding["nodeId"] == node
                        && !entries.iter().any(|entry| {
                            entry["nodeId"] == node
                                && entry["port"] == binding["port"]
                                && entry["virtual"] != true
                        })
                })
            });
            if has_input_injection {
                continue;
            }
            if metadata["missing"]
                .as_array()
                .is_some_and(|items| items.iter().any(|entry| entry["nodeId"] == node))
            {
                return Err(format!(
                    "source node {node} has uncaptured declared outputs; provide explicit --bind"
                ));
            }
            let ports: Vec<_> = entries
                .iter()
                .filter(|e| e["nodeId"] == node && e["virtual"] != true)
                .collect();
            if ports.is_empty() {
                return Err(format!(
                    "source node {node} has no captured outputs; provide explicit --bind"
                ));
            }
            for entry in ports {
                let port = entry["port"].as_str().ok_or("invalid captured port")?;
                bindings
                    .push(json!({"sourceNode":node,"sourcePort":port,"nodeId":node,"port":port}));
            }
        }
        if let Some(injected) = metadata["injectedInputs"].as_array() {
            for binding in injected {
                let node = binding["sourceNode"]
                    .as_str()
                    .ok_or("injected input source node missing")?;
                let port = binding["sourcePort"]
                    .as_str()
                    .ok_or("injected input source port missing")?;
                let target = binding["nodeId"]
                    .as_str()
                    .ok_or("injected input target node missing")?;
                let target_port = binding["port"]
                    .as_str()
                    .ok_or("injected input target port missing")?;
                if !entries
                    .iter()
                    .any(|e| e["nodeId"] == node && e["port"] == port)
                {
                    return Err("original injected input payload is absent from the archive".into());
                }
                bindings.retain(|existing| {
                    existing["nodeId"] != target || existing["port"] != target_port
                });
                bindings.push(binding.clone());
            }
        }
    } else {
        for spec in specs {
            let (source, target) = spec
                .split_once('=')
                .ok_or("--bind requires sourceNode.sourcePort=nodeId.port")?;
            let (node, port) = source_address(entries, source)?;
            let (target_node, target_port) = target
                .split_once(':')
                .or_else(|| target.rsplit_once('.'))
                .filter(|(n, p)| !n.is_empty() && !p.is_empty())
                .ok_or("binding target needs node.port")?;
            if !destinations.insert((target_node.to_owned(), target_port.to_owned())) {
                return Err(format!("duplicate binding destination {target}"));
            }
            bindings.push(json!({"sourceNode":node,"sourcePort":port,"nodeId":target_node,"port":target_port}));
        }
    }
    Ok(Value::Array(bindings))
}

fn source_address<'a>(entries: &'a [Value], source: &str) -> Result<(&'a str, &'a str), String> {
    let mut dotted = std::collections::BTreeSet::new();
    let mut explicit = std::collections::BTreeSet::new();
    for entry in entries {
        let node = entry["nodeId"]
            .as_str()
            .ok_or("invalid frame node address")?;
        let port = entry["port"].as_str().ok_or("invalid frame port address")?;
        if format!("{node}.{port}") == source {
            dotted.insert((node, port));
        }
        if format!("{node}:{port}") == source {
            explicit.insert((node, port));
        }
    }
    let addresses = if explicit.is_empty() {
        dotted
    } else {
        explicit
    };
    match addresses.len() {
        0 => Err(format!("binding source {source} is absent from frame")),
        1 => Ok(*addresses.iter().next().unwrap()),
        _ => Err(format!(
            "binding source {source} is ambiguous; use node:port"
        )),
    }
}

fn publish(parsed: &Parsed, out: &Sink, cancel: &Cancellation) -> Result<(), String> {
    let recording = Recording::open(parsed)?;
    let core = core()?;
    let limit = max_bytes(parsed)?;
    let endpoint = required(parsed, "endpoint")?;
    let topic = required(parsed, "topic")?;
    let timeout = integer(parsed, "timeout-ms", 60_000, 3_600_000)?;
    for row in &recording.rows {
        let (frame, path) = recording.load(&core, row, limit, cancel)?;
        let metadata = parse_json(frame.metadata().map_err(|e| e.to_string())?.as_bytes())?;
        let receipt = wire::publish(
            &path,
            &metadata,
            endpoint,
            topic,
            Duration::from_millis(timeout),
            limit,
            || cancel.cancelled(),
        )
        .map_err(|e| e.to_string())?;
        json_line(out, &receipt);
    }
    Ok(())
}

fn log(parsed: &Parsed, out: &Sink, cancel: &Cancellation) -> Result<(), String> {
    let recording = Recording::open(parsed)?;
    let core = core()?;
    let limit = max_bytes(parsed)?;
    let output = required(parsed, "output")?;
    let source = required(parsed, "node")?;
    let (node, port) = source.rsplit_once('.').ok_or("--node requires node.port")?;
    let kind = match required(parsed, "log-kind")? {
        "string" => text::Kind::String,
        "poses2d" => text::Kind::Poses2d,
        "poses3d" => text::Kind::Poses3d,
        _ => return Err("log-kind must be string|poses2d|poses3d".into()),
    };
    let angle = parsed.one("angle-unit").unwrap_or("rad");
    if !["rad", "deg"].contains(&angle) {
        return Err("angle-unit must be rad|deg".into());
    }
    let selection = parsed.one("selection").unwrap_or("all");
    if !["all", "first"].contains(&selection) {
        return Err("selection must be all|first".into());
    }
    let options = text::Options {
        kind,
        first: selection == "first",
        pose_format: parsed.one("pose-format").unwrap_or("quat").into(),
        length_unit: parsed.one("length-unit").map(str::to_owned),
        angle_degrees: angle == "deg",
    };
    let conflict = parsed.one("conflict").unwrap_or("fail");
    if !["fail", "append"].contains(&conflict) {
        return Err("conflict must be fail|append".into());
    }
    let format = parsed.one("log-format").unwrap_or("jsonl");
    if !["jsonl", "text"].contains(&format) {
        return Err("log-format must be jsonl|text".into());
    }
    let archive = parsed.one("log-archive").unwrap_or("daily");
    if !["none", "daily"].contains(&archive) {
        return Err("log-archive must be daily|none".into());
    }
    let mut files: BTreeMap<PathBuf, File> = BTreeMap::new();
    for row in &recording.rows {
        cancel.check()?;
        let (frame, _) = recording.load(&core, row, limit, cancel)?;
        let info = parse_json(
            core.output_info(frame.run_id(), node)
                .map_err(|e| e.to_string())?
                .as_bytes(),
        )?;
        let record = info
            .as_array()
            .ok_or("node output info must be an array")?
            .iter()
            .find(|v| v["port"] == port)
            .ok_or_else(|| format!("output {source} is absent"))?;
        let values = text::render(&record["value"], &options)?;
        let timestamp = decimal(&row["metadata"]["timestampNs"], "timestampNs")?;
        let path = if archive == "daily" {
            PathBuf::from(format!("{output}.day-{}", timestamp / 86_400_000_000_000))
        } else {
            PathBuf::from(output)
        };
        if !files.contains_key(&path) {
            let file = OpenOptions::new()
                .read(true)
                .write(true)
                .append(conflict == "append")
                .create(conflict == "append")
                .create_new(conflict == "fail")
                .open(&path)
                .map_err(|e| format!("{}: {e}", path.display()))?;
            file.try_lock()
                .map_err(|e| format!("log destination busy: {e}"))?;
            files.insert(path.clone(), file);
        }
        let mut data = Vec::new();
        for value in &values {
            let event = json!({"type":"lyflow.Log.v1","timestampNs":timestamp.to_string(),"frameId":row["metadata"]["frameId"],"frame":row["metadata"]["frame"],"calibrationVersion":row["metadata"]["calibrationVersion"],"sourceRunId":row["metadata"]["sourceRunId"],"nodeId":node,"port":port,"value":value});
            let line = if format == "jsonl" {
                event.to_string()
            } else {
                format!(
                    "{timestamp}\t{}\t{}\t{}",
                    row["metadata"]["frameId"].as_str().unwrap_or(""),
                    source,
                    value
                )
            };
            data.extend_from_slice(line.as_bytes());
            data.push(b'\n');
        }
        let file = files.get_mut(&path).unwrap();
        if file
            .metadata()
            .map_err(|e| e.to_string())?
            .len()
            .saturating_add(data.len() as u64)
            > limit
        {
            return Err(
                "log archive exceeds max-bytes; use another destination or smaller selection"
                    .into(),
            );
        }
        file.write_all(&data)
            .and_then(|_| file.sync_data())
            .map_err(|e| e.to_string())?;
        json_line(
            out,
            &json!({"type":"lyflow.Logged.v1","file":path,"frameId":row["metadata"]["frameId"],"rows":values.len(),"archivePolicy":archive,"conflictPolicy":conflict}),
        );
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    struct Directory(PathBuf);
    impl Directory {
        fn new() -> Self {
            let path = std::env::temp_dir().join(format!("lyflow-delivery-{}", ulid::new()));
            std::fs::create_dir(&path).unwrap();
            Self(path)
        }
    }
    impl Drop for Directory {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }
    fn parsed(dir: &Path) -> Parsed {
        Parsed {
            positional: vec![dir.to_string_lossy().into_owned()],
            values: BTreeMap::new(),
            flags: HashSet::new(),
        }
    }
    fn index() -> Value {
        json!({"type":"lyflow.Recording.v1","schemaVersion":1,"complete":false,"frames":[{"sequence":"0","file":"first.lyframe","bytes":"3","metadata":{"frameId":"a","timestampNs":"100"}},{"sequence":"1","file":"second.lyframe","bytes":"3","metadata":{"frameId":"b","timestampNs":"200"}}]})
    }
    #[test]
    fn explicit_binding_resolves_dotted_ports_and_node_ids_without_guessing_ambiguous_addresses() {
        let entries = json!([
            {"nodeId":"source","port":"pair.first","virtual":true},
            {"nodeId":"source","port":"pair.first"},
            {"nodeId":"source.pair","port":"first"}
        ]);
        let entries = entries.as_array().unwrap();
        assert!(source_address(entries, "source.pair.first")
            .unwrap_err()
            .contains("ambiguous"));
        assert_eq!(
            source_address(entries, "source:pair.first").unwrap(),
            ("source", "pair.first")
        );
        assert_eq!(
            source_address(entries, "source.pair:first").unwrap(),
            ("source.pair", "first")
        );
        assert_eq!(
            source_address(&entries[..2], "source.pair.first").unwrap(),
            ("source", "pair.first")
        );
        assert!(source_address(entries, "missing.port")
            .unwrap_err()
            .contains("absent"));
    }
    #[test]
    fn recording_index_contract_preserves_last_checkpoint_selection_integrity_and_containment() {
        let dir = Directory::new();
        let mut value = index();
        checkpoint(&dir.0, &value).unwrap();
        write_new(&dir.0.join("first.lyframe"), b"raw").unwrap();
        let mut options = parsed(&dir.0);
        let recording = Recording::open(&options).unwrap();
        assert_eq!(recording.rows.len(), 2);
        assert!(recording.path("../escape").is_err());
        assert!(recording.path("/absolute").is_err());
        assert!(write_new(&dir.0.join("first.lyframe"), b"new").is_err());
        options
            .values
            .insert("from-sequence".into(), vec!["1".into()]);
        assert_eq!(
            Recording::open(&options).unwrap().rows[0]["metadata"]["frameId"],
            "b"
        );
        value["frames"][1]["metadata"]["timestampNs"] = json!("99");
        checkpoint(&dir.0, &value).unwrap();
        assert!(Recording::open(&options).unwrap_err().contains("ordered"));
        value["frames"][1]["metadata"]["timestampNs"] = json!("200");
        value["frames"][1]["metadata"]["frameId"] = json!("a");
        checkpoint(&dir.0, &value).unwrap();
        assert!(Recording::open(&options).is_err());
        assert!(decimal(&json!(9007199254740993u64), "timestampNs").is_err());
        assert_eq!(
            decimal(&json!("18446744073709551615"), "timestampNs").unwrap(),
            u64::MAX
        );
        let cancel = Cancellation::new();
        let (size, sha) = hash_file(&dir.0.join("first.lyframe"), 3, &cancel).unwrap();
        assert_eq!(size, 3);
        assert_eq!(sha, format!("{:x}", Sha256::digest(b"raw")));
        assert!(hash_file(&dir.0.join("first.lyframe"), 2, &cancel).is_err());
        cancel.flag.store(true, Ordering::SeqCst);
        assert!(cancel.wait(Duration::from_secs(1)).is_err());
        assert_eq!(std::fs::read(dir.0.join("first.lyframe")).unwrap(), b"raw");
        let conflict = Directory::new();
        std::fs::create_dir(conflict.0.join("index.json")).unwrap();
        write_new(&conflict.0.join("index.json").join("existing"), b"keep").unwrap();
        assert!(checkpoint(&conflict.0, &index()).is_err());
        assert_eq!(std::fs::read_dir(&conflict.0).unwrap().count(), 1);
        assert_eq!(
            std::fs::read(conflict.0.join("index.json").join("existing")).unwrap(),
            b"keep"
        );
    }
}
