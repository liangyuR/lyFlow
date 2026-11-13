use super::*;
use sha2::{Digest, Sha256};
use std::io::Read;

pub(crate) struct Capture {
    dir: PathBuf,
    policy: String,
    selected: Vec<(String, String)>,
    max_bytes: u64,
    max_total_bytes: u64,
    effective: Value,
    inputs: Vec<Value>,
}

fn identity(path: &Path) -> Result<Value, String> {
    let mut file = std::fs::File::open(path).map_err(|e| e.to_string())?;
    let mut hash = Sha256::new();
    let mut buf = [0u8; 65536];
    let mut bytes = 0u64;
    loop {
        let n = file.read(&mut buf).map_err(|e| e.to_string())?;
        if n == 0 {
            break;
        }
        hash.update(&buf[..n]);
        bytes += n as u64;
    }
    Ok(json!({"path":path, "bytes":bytes, "sha256":format!("{:x}", hash.finalize())}))
}

fn collect_directory(dir: &Path, paths: &mut Vec<PathBuf>, depth: usize) -> Result<(), String> {
    if depth > 32 {
        return Err("input directory identity exceeds 32 levels".into());
    }
    for entry in std::fs::read_dir(dir).map_err(|e| e.to_string())? {
        let entry = entry.map_err(|e| e.to_string())?;
        let p = entry.path();
        if entry.file_type().map_err(|e| e.to_string())?.is_symlink() {
            continue;
        }
        if p.is_dir() {
            collect_directory(&p, paths, depth + 1)?;
        } else if p.is_file() {
            let p = p.canonicalize().map_err(|e| e.to_string())?;
            if !paths.contains(&p) {
                paths.push(p);
            }
            if paths.len() > 10000 {
                return Err(
                    "input identity exceeds 10000 files; narrow the input directory".into(),
                );
            }
        }
    }
    Ok(())
}

fn input_paths(value: &Value, base: &Path, paths: &mut Vec<PathBuf>) -> Result<(), String> {
    match value {
        Value::String(s) if !s.is_empty() => {
            let p = base.join(s);
            if p.is_dir() {
                collect_directory(&p, paths, 0)?;
            } else if p.is_file() {
                let p = p.canonicalize().map_err(|e| e.to_string())?;
                if !paths.contains(&p) {
                    paths.push(p);
                }
            }
        }
        Value::Array(a) => {
            for v in a {
                input_paths(v, base, paths)?;
            }
        }
        Value::Object(o) => {
            for v in o.values() {
                input_paths(v, base, paths)?;
            }
        }
        _ => {}
    }
    Ok(())
}

impl Capture {
    pub(crate) fn prepare_eval(
        parsed: &Parsed,
        core: &Arc<Core>,
        loaded: &Loaded,
    ) -> Result<Option<Self>, String> {
        let Some(root) = parsed.one("snapshot-root") else {
            return Ok(None);
        };
        let mut options = parsed.clone();
        options.values.insert(
            "snapshot-dir".into(),
            vec![Path::new(root)
                .join(format!("pending-{}", crate::ulid::new()))
                .to_string_lossy()
                .into_owned()],
        );
        Self::prepare(&options, core, loaded)
    }

    pub(crate) fn save_eval(
        self,
        core: &Arc<Core>,
        loaded: &Loaded,
        result: &RunResult,
    ) -> Result<String, String> {
        let dir = self.dir.clone();
        let dest = dir.parent().unwrap().join(result.run_id());
        if dest.exists() {
            return Err("snapshot already exists".into());
        }
        self.save(core, loaded, result)?;
        std::fs::rename(dir, dest).map_err(|e| e.to_string())?;
        Ok(result.run_id().to_string())
    }

    pub(crate) fn prepare(
        parsed: &Parsed,
        core: &Arc<Core>,
        loaded: &Loaded,
    ) -> Result<Option<Self>, String> {
        let Some(dir) = parsed.one("snapshot-dir") else {
            return Ok(None);
        };
        let policy = parsed.one("capture-policy").unwrap_or("all").to_string();
        if !["all", "selected", "failures", "metrics"].contains(&policy.as_str()) {
            return Err("capture-policy must be all, selected, failures or metrics".into());
        }
        let max_bytes = parsed
            .one("capture-max-bytes")
            .unwrap_or("268435456")
            .parse::<u64>()
            .map_err(|_| "capture-max-bytes must be a nonnegative integer")?;
        let max_total_bytes = parsed
            .one("snapshot-max-total-bytes")
            .unwrap_or("10737418240")
            .parse::<u64>()
            .map_err(|_| "snapshot-max-total-bytes must be a nonnegative integer")?;
        let selected = parsed
            .many("capture-output")
            .iter()
            .map(|s| {
                s.split_once(':')
                    .filter(|(n, p)| !n.is_empty() && !p.is_empty())
                    .map(|(n, p)| (n.to_string(), p.to_string()))
                    .ok_or_else(|| format!("invalid capture-output: {s}"))
            })
            .collect::<Result<Vec<_>, _>>()?;
        if policy == "selected" && selected.is_empty() {
            return Err("selected capture requires capture-output".into());
        }
        let effective: Value = serde_json::from_str(
            &core
                .effective_params(&loaded.json, &loaded.base_dir)
                .map_err(|e| e.to_string())?,
        )
        .map_err(|e| e.to_string())?;
        let mut paths = Vec::new();
        let manifest: Value =
            serde_json::from_str(&core.manifest_json().map_err(|e| e.to_string())?)
                .map_err(|e| e.to_string())?;
        for node in effective["nodes"].as_array().into_iter().flatten() {
            let op = manifest["operators"]
                .as_array()
                .into_iter()
                .flatten()
                .find(|o| o["id"] == node["op"]);
            for param in node["params"].as_array().into_iter().flatten() {
                let desc = op
                    .and_then(|o| o["params"].as_array())
                    .into_iter()
                    .flatten()
                    .find(|p| p["name"] == param["param"]);
                if desc.is_some_and(|p| p["type"] == "path" && p["mode"] != "save") {
                    input_paths(&param["value"], Path::new(&loaded.base_dir), &mut paths)?;
                }
            }
        }
        for spec in parsed.many("input") {
            if let Some((_, file)) = spec.split_once('=') {
                input_paths(&json!(file), Path::new(&loaded.base_dir), &mut paths)?;
            }
        }
        let inputs = paths
            .iter()
            .map(|p| identity(p))
            .collect::<Result<Vec<_>, _>>()?;
        let dir = std::env::current_dir()
            .map_err(|e| e.to_string())?
            .join(dir);
        if let Some(parent) = dir.parent() {
            std::fs::create_dir_all(parent).map_err(|e| e.to_string())?;
        }
        std::fs::create_dir(&dir)
            .map_err(|e| format!("snapshot directory must be new: {}: {e}", dir.display()))?;
        Ok(Some(Self {
            dir,
            policy,
            selected,
            max_bytes,
            max_total_bytes,
            effective,
            inputs,
        }))
    }

    pub(crate) fn save(
        self,
        core: &Arc<Core>,
        loaded: &Loaded,
        result: &RunResult,
    ) -> Result<Value, String> {
        let mut outputs = Vec::new();
        let mut used = 0u64;
        let mut node_ids: Vec<String> = loaded.doc.nodes.iter().map(|n| n.id.clone()).collect();
        for event in &result.events {
            if let Some(id) = event["nodeId"].as_str() {
                if !node_ids.iter().any(|n| n == id) {
                    node_ids.push(id.to_string());
                }
            }
        }
        for node_id in &node_ids {
            let raw = core
                .output_info(result.run_id(), node_id)
                .map_err(|e| e.to_string())?;
            let info: Vec<Value> = serde_json::from_str(&raw).map_err(|e| e.to_string())?;
            for mut item in info {
                let port = item["port"].as_str().unwrap_or_default().to_string();
                let ty = item["type"].as_str().unwrap_or_default().to_string();
                item["nodeId"] = json!(node_id);
                let binary = ty == "PointCloud" || ty == "Image";
                let include = match self.policy.as_str() {
                    "selected" => self.selected.contains(&(node_id.clone(), port.clone())),
                    "failures" => {
                        result.status != "ok"
                            || result.summary().is_some_and(|s| {
                                s["status"]
                                    .as_str()
                                    .is_some_and(|v| v != "ok" && v != "pass")
                            })
                    }
                    "metrics" => !binary,
                    _ => true,
                };
                let missing = if !include {
                    Some("not_selected".to_string())
                } else if binary {
                    let name = format!(
                        "output-{}.{}",
                        outputs.len(),
                        if ty == "Image" { "lyim" } else { "pcd" }
                    );
                    let file = self.dir.join(&name);
                    if item["byteSize"].as_u64().unwrap_or(0) > self.max_bytes.saturating_sub(used)
                    {
                        Some("quota_exceeded".to_string())
                    } else {
                        match core.output_save(
                            result.run_id(),
                            node_id,
                            &port,
                            &file.to_string_lossy(),
                            "ascii",
                        ) {
                            Err(e) => {
                                let _ = std::fs::remove_file(&file);
                                Some(format!("capture_failed: {e}"))
                            }
                            Ok(()) => {
                                let size =
                                    std::fs::metadata(&file).map_err(|e| e.to_string())?.len();
                                if size > self.max_bytes.saturating_sub(used) {
                                    std::fs::remove_file(&file).map_err(|e| e.to_string())?;
                                    Some("quota_exceeded".to_string())
                                } else {
                                    used += size;
                                    item["file"] = json!(name);
                                    item["capturedBytes"] = json!(size);
                                    None
                                }
                            }
                        }
                    }
                } else if item.get("value").is_some_and(|v| !v.is_null()) {
                    let size = serde_json::to_vec(&item["value"])
                        .map_err(|e| e.to_string())?
                        .len() as u64;
                    if size > self.max_bytes.saturating_sub(used) {
                        Some("quota_exceeded".to_string())
                    } else {
                        used += size;
                        None
                    }
                } else {
                    Some("unsupported_type".to_string())
                };
                if let Some(reason) = missing {
                    item.as_object_mut().unwrap().remove("value");
                    item["missing"] = json!(reason);
                }
                outputs.push(item);
            }
        }
        for (node, port) in &self.selected {
            if !outputs
                .iter()
                .any(|o| o["nodeId"] == *node && o["port"] == *port)
            {
                outputs.push(json!({"nodeId":node,"port":port,"missing":"output_unavailable"}));
            }
        }
        let inputs: Vec<Value> = self
            .inputs
            .into_iter()
            .map(|mut before| {
                let after = identity(Path::new(before["path"].as_str().unwrap()));
                before["unchangedDuringRun"] = json!(after
                    .as_ref()
                    .is_ok_and(|a| a["sha256"] == before["sha256"]));
                before
            })
            .collect();
        let manifest = json!({"schemaVersion":1,"snapshotId":result.run_id(),"runId":result.run_id(),
            "status":result.status,"graph":serde_json::from_str::<Value>(&loaded.json).map_err(|e|e.to_string())?,
            "baseDir":loaded.base_dir,"effectiveParams":self.effective,"engine":{"version":core.version(),"abiVersion":core.abi_version(),"buildIdentity":crate::disk_cache::build_identity(core).ok(),"corePath":core_ffi::dll_file()},
            "inputIdentityScope":"manifest path parameters except save mode; directories recursively enumerated, at most 10000 files / 32 levels; directory symlink entries excluded", "inputs":inputs,"outputs":outputs,"summary":result.summary(),
            "capture":{"policy":self.policy,"maxBytes":self.max_bytes,"capturedBytes":used,"budgetScope":"output payloads","maxTotalBytes":self.max_total_bytes}});
        let parent = self
            .dir
            .parent()
            .ok_or("snapshot directory has no parent")?;
        let lock = std::fs::OpenOptions::new()
            .create(true)
            .read(true)
            .write(true)
            .truncate(false)
            .open(parent.join(".snapshot-quota.lock"))
            .map_err(|e| e.to_string())?;
        lock.lock().map_err(|e| e.to_string())?;
        let mut total = manifest["outputs"]
            .as_array()
            .unwrap()
            .iter()
            .map(|o| o["capturedBytes"].as_u64().unwrap_or(0))
            .sum::<u64>();
        for entry in std::fs::read_dir(parent).map_err(|e| e.to_string())? {
            let dir = entry.map_err(|e| e.to_string())?.path();
            let pending = dir
                .file_name()
                .and_then(|n| n.to_str())
                .is_some_and(|n| n.starts_with("pending-"));
            if dir == self.dir || (!dir.join("snapshot.json").is_file() && !pending) {
                continue;
            }
            for file in std::fs::read_dir(dir).map_err(|e| e.to_string())? {
                let file = file.map_err(|e| e.to_string())?;
                if file.file_type().map_err(|e| e.to_string())?.is_file() {
                    total = total.saturating_add(file.metadata().map_err(|e| e.to_string())?.len());
                }
            }
        }
        let bytes = serde_json::to_vec_pretty(&manifest).map_err(|e| e.to_string())?;
        if total.saturating_add(bytes.len() as u64) > self.max_total_bytes {
            std::fs::remove_dir_all(&self.dir).map_err(|e| e.to_string())?;
            return Err(format!("snapshot store quota exceeded ({} bytes); published snapshots and pending captures are counted; stop captures before manually inspecting/removing abandoned pending-* directories, or increase --snapshot-max-total-bytes",self.max_total_bytes));
        }
        let tmp = self.dir.join("snapshot.json.tmp");
        std::fs::write(&tmp, bytes).map_err(|e| e.to_string())?;
        std::fs::rename(tmp, self.dir.join("snapshot.json")).map_err(|e| e.to_string())?;
        Ok(manifest)
    }
}
