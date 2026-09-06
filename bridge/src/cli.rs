//! headless CLI（ADR-0012）。与 app 共用 `core_ffi` 与 GraphDoc 校验，不依赖 Tauri。
//! stdout 是 JSON Lines（`run` 输出 ExecutionEvent 原样），stderr 给人看。

use std::collections::{BTreeMap, HashSet};
use std::ffi::CStr;
use std::io::Write;
use std::os::raw::{c_char, c_void};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, OnceLock};

use serde_json::{json, Value};

use crate::core_ffi::{self, Core, RunHandle, RunSpec};
use crate::graph::GraphDoc;
use crate::ulid;

pub const EXIT_OK: i32 = 0;
pub const EXIT_INVALID: i32 = 1;
pub const EXIT_FAILED: i32 = 2;
pub const EXIT_CANCELLED: i32 = 3;
pub const EXIT_USAGE: i32 = 4;

/// 输出汇。测试把它接到内存缓冲上，真跑时接 stdout/stderr。
pub type Sink = Arc<Mutex<Box<dyn Write + Send>>>;

pub fn sink_of<W: Write + Send + 'static>(w: W) -> Sink {
    Arc::new(Mutex::new(Box::new(w) as Box<dyn Write + Send>))
}

fn line(sink: &Sink, text: &str) {
    if let Ok(mut w) = sink.lock() {
        let _ = writeln!(w, "{text}");
        let _ = w.flush();
    }
}

fn json_line(sink: &Sink, value: &Value) {
    line(sink, &value.to_string());
}

const USAGE: &str = "\
lyflow —— LyFlow 的 headless 命令行（stdout 是 JSON Lines，stderr 给人看）

  lyflow run      <graph> [--to <nodeId>]... [--set <nodeId>.<param>=<json>]...
                          [--base-dir <dir>] [--parallel <n>] [--no-cache]
                          [--preview] [--preview-points <n>] [--outputs]
  lyflow import   <file> --kind <kind> [-o <out.lyflow.json>] [--base-dir <dir>]
  lyflow validate <graph> [--base-dir <dir>] [--set ...]
  lyflow plan     <graph> [--to <nodeId>]... [--base-dir <dir>] [--set ...]
  lyflow migrate  <graph> [--write]
  lyflow manifest [--check]
  lyflow dump     <graph> <nodeId>:<port> <out.pcd> [--format <binary|ascii|binary_compressed>]
  lyflow sweep    <graph> --param <nodeId>.<param>=<start>:<end>:<steps> [--param ...]
                          --metric <nodeId>:<port>.<elementCount|byteSize|durationMs>
                          [--csv <out.csv>] [--base-dir <dir>]
  lyflow diff     <a> <b> [--json]

退出码：0 成功，1 校验失败，2 执行失败，3 被取消（Ctrl+C），4 参数错。";

// ------------------------------------------------------------------ 参数解析

struct Parsed {
    positional: Vec<String>,
    values: BTreeMap<String, Vec<String>>,
    flags: HashSet<String>,
}

impl Parsed {
    fn one(&self, name: &str) -> Option<&str> {
        self.values.get(name).and_then(|v| v.last()).map(String::as_str)
    }
    fn many(&self, name: &str) -> &[String] {
        static EMPTY: &[String] = &[];
        self.values.get(name).map(Vec::as_slice).unwrap_or(EMPTY)
    }
    fn has(&self, name: &str) -> bool {
        self.flags.contains(name)
    }
}

fn parse_args(args: &[String], value_opts: &[&str], bool_opts: &[&str]) -> Result<Parsed, String> {
    let mut out = Parsed {
        positional: Vec::new(),
        values: BTreeMap::new(),
        flags: HashSet::new(),
    };
    let mut i = 0;
    while i < args.len() {
        // -o 是 --output 的短写。整个 CLI 只有这一个短选项，所以不做通用的短选项表。
        let expanded;
        let a = if args[i] == "-o" {
            expanded = String::from("--output");
            &expanded
        } else {
            &args[i]
        };
        if let Some(name) = a.strip_prefix("--") {
            // 同时认 `--name=value` 与 `--name value`：前者在 shell 里更省心
            let (name, inline) = match name.split_once('=') {
                Some((n, v)) => (n, Some(v.to_string())),
                None => (name, None),
            };
            if bool_opts.contains(&name) && inline.is_none() {
                out.flags.insert(name.to_string());
                i += 1;
                continue;
            }
            if !value_opts.contains(&name) {
                return Err(format!("不认识的选项 --{name}"));
            }
            let value = match inline {
                Some(v) => v,
                None => {
                    i += 1;
                    args.get(i)
                        .cloned()
                        .ok_or_else(|| format!("--{name} 后面缺一个值"))?
                }
            };
            out.values.entry(name.to_string()).or_default().push(value);
            i += 1;
            continue;
        }
        out.positional.push(a.clone());
        i += 1;
    }
    Ok(out)
}

// ------------------------------------------------------------------ 图的装载

struct Loaded {
    doc: GraphDoc,
    json: String,
    base_dir: String,
    path: PathBuf,
}

/// `--set nodeId.param=<json>`。值先按 JSON 解析，解析不了就当字符串 ——
/// `--set n.path=cloud.pcd` 是最常见的一条，不该逼用户写引号里的引号。
fn apply_set(doc: &mut GraphDoc, spec: &str) -> Result<(), String> {
    let (left, raw) = spec
        .split_once('=')
        .ok_or_else(|| format!("--set 的写法是 nodeId.param=<json>，收到 {spec}"))?;
    let (node_id, param) = left
        .rsplit_once('.')
        .ok_or_else(|| format!("--set 的写法是 nodeId.param=<json>，收到 {spec}"))?;
    let value: Value = serde_json::from_str(raw).unwrap_or_else(|_| Value::String(raw.to_string()));
    let node = doc
        .nodes
        .iter_mut()
        .find(|n| n.id == node_id)
        .ok_or_else(|| format!("图里没有节点 {node_id}"))?;
    node.params.insert(param.to_string(), value);
    Ok(())
}

fn load_graph(parsed: &Parsed, path: &str) -> Result<Loaded, String> {
    let file = PathBuf::from(path);
    let text = std::fs::read_to_string(&file).map_err(|e| format!("读取 {path} 失败: {e}"))?;
    let mut doc: GraphDoc =
        serde_json::from_str(&text).map_err(|e| format!("{path} 不是合法的 GraphDoc: {e}"))?;
    doc.validate_structure().map_err(|e| e.to_string())?;
    for spec in parsed.many("set") {
        apply_set(&mut doc, spec)?;
    }
    let base_dir = match parsed.one("base-dir") {
        Some(d) => d.to_string(),
        None => file
            .parent()
            .map(|d| d.to_string_lossy().into_owned())
            .unwrap_or_default(),
    };
    let json = serde_json::to_string(&doc).map_err(|e| e.to_string())?;
    Ok(Loaded {
        doc,
        json,
        base_dir,
        path: file,
    })
}

fn core() -> Result<Arc<Core>, String> {
    core_ffi::core()
}

// -------------------------------------------------------------------- 运行

struct RunCtx {
    sink: Option<Sink>,
    events: Mutex<Vec<Value>>,
}

unsafe extern "C" fn on_event(event_json: *const c_char, user: *mut c_void) {
    let _ = std::panic::catch_unwind(|| {
        if event_json.is_null() || user.is_null() {
            return;
        }
        let ctx = &*(user as *const RunCtx);
        let Ok(text) = CStr::from_ptr(event_json).to_str() else {
            return;
        };
        if let Some(sink) = &ctx.sink {
            line(sink, text);
        }
        if let Ok(value) = serde_json::from_str::<Value>(text) {
            if let Ok(mut v) = ctx.events.lock() {
                v.push(value);
            }
        }
    });
}

/// 正在跑的那一次。Ctrl+C 的处理器在别的线程上，只能从这里拿句柄。
fn active_run() -> &'static Mutex<Option<Arc<RunHandle>>> {
    static ACTIVE: OnceLock<Mutex<Option<Arc<RunHandle>>>> = OnceLock::new();
    ACTIVE.get_or_init(|| Mutex::new(None))
}

#[cfg(windows)]
mod console {
    use std::sync::atomic::{AtomicBool, Ordering};

    pub static INSTALLED: AtomicBool = AtomicBool::new(false);

    type Handler = unsafe extern "system" fn(u32) -> i32;

    extern "system" {
        fn SetConsoleCtrlHandler(handler: Option<Handler>, add: i32) -> i32;
    }

    unsafe extern "system" fn on_break(_kind: u32) -> i32 {
        if let Ok(guard) = super::active_run().lock() {
            if let Some(run) = guard.as_ref() {
                run.cancel();
                // 返回 1 = 我们处理了，别让默认处理器直接杀进程；
                // 取消是协作式的，主线程还要把 run_finished 发完。
                return 1;
            }
        }
        0
    }

    pub fn install() {
        if INSTALLED.swap(true, Ordering::SeqCst) {
            return;
        }
        unsafe {
            SetConsoleCtrlHandler(Some(on_break), 1);
        }
    }
}

#[cfg(not(windows))]
mod console {
    pub fn install() {}
}

struct RunResult {
    events: Vec<Value>,
    status: String,
    /// 句柄活着结果仓的索引才在（Drop 会 freeRun）。dump 要在这之后取输出。
    _handle: Arc<RunHandle>,
}

impl RunResult {
    fn run_id(&self) -> &str {
        self._handle.run_id()
    }

    fn exit_code(&self) -> i32 {
        match self.status.as_str() {
            "ok" => EXIT_OK,
            "cancelled" => EXIT_CANCELLED,
            _ => EXIT_FAILED,
        }
    }

    /// 某节点最后一次 node_state 的 state。
    fn final_state(&self, node_id: &str) -> Option<String> {
        self.events
            .iter()
            .filter(|e| e["kind"] == "node_state" && e["nodeId"] == node_id)
            .last()
            .and_then(|e| e["state"].as_str().map(str::to_owned))
    }

    fn skipped_nodes(&self) -> Vec<String> {
        let mut out = Vec::new();
        let mut seen = HashSet::new();
        for e in &self.events {
            if e["kind"] != "node_state" {
                continue;
            }
            let Some(id) = e["nodeId"].as_str() else { continue };
            if !seen.insert(id.to_string()) {
                continue;
            }
            if self.final_state(id).as_deref() == Some("skipped") {
                out.push(id.to_string());
            }
        }
        out.sort();
        out
    }

    fn metric(&self, node_id: &str, port: &str, field: &str) -> Option<f64> {
        for e in self.events.iter().rev() {
            if e["kind"] != "node_state" || e["nodeId"] != node_id {
                continue;
            }
            if field == "durationMs" {
                if let Some(v) = e["durationMs"].as_f64() {
                    return Some(v);
                }
                continue;
            }
            let outputs = e["stats"]["outputs"].as_array()?;
            for o in outputs {
                if o["port"] == port {
                    return o[field].as_f64();
                }
            }
        }
        None
    }

    fn duration_ms(&self) -> f64 {
        self.events
            .iter()
            .rev()
            .find(|e| e["kind"] == "run_finished")
            .and_then(|e| e["durationMs"].as_f64())
            .unwrap_or(0.0)
    }
}

struct RunRequest<'a> {
    graph_json: &'a str,
    base_dir: &'a str,
    targets: &'a [String],
    parallel: i32,
    preview_points: u32,
    preview: bool,
    /// `--no-cache`：本次运行不吃缓存。不清进程级结果仓 —— 那会连累别的 run。
    no_cache: bool,
    stream: Option<Sink>,
}

fn execute(core: &Arc<Core>, req: RunRequest<'_>) -> Result<RunResult, String> {
    console::install();
    let run_id = ulid::new();
    let ctx = Box::new(RunCtx {
        sink: req.stream,
        events: Mutex::new(Vec::new()),
    });
    let ptr = &*ctx as *const RunCtx;
    let mut spec = RunSpec::new(req.graph_json, &run_id, req.base_dir, req.targets);
    spec.max_parallel = req.parallel;
    spec.no_reuse = req.no_cache;
    if req.preview {
        spec.mode = 1;
        spec.preview_max_points = req.preview_points;
    }
    let handle = Arc::new(
        unsafe { RunHandle::start(Arc::clone(core), spec, on_event, ctx) }
            .map_err(|e| e.to_string())?,
    );
    if let Ok(mut guard) = active_run().lock() {
        *guard = Some(Arc::clone(&handle));
    }
    handle.join();
    if let Ok(mut guard) = active_run().lock() {
        *guard = None;
    }
    // join 返回后 core 保证不再回调，这时读收集到的事件才是安全的
    let events = unsafe { (*ptr).events.lock().map_err(|e| e.to_string())?.clone() };
    let status = events
        .iter()
        .rev()
        .find(|e| e["kind"] == "run_finished")
        .and_then(|e| e["status"].as_str().map(str::to_owned))
        .unwrap_or_else(|| "error".to_string());
    Ok(RunResult {
        events,
        status,
        _handle: handle,
    })
}

// ---------------------------------------------------------------- 各子命令

fn diagnostics_of(core: &Arc<Core>, loaded: &Loaded) -> Result<Vec<Value>, String> {
    let raw = core
        .validate(&loaded.json, &loaded.base_dir)
        .map_err(|e| e.to_string())?;
    serde_json::from_str(&raw).map_err(|e| format!("core 返回的诊断不是合法 JSON: {e}"))
}

fn has_errors(diags: &[Value]) -> bool {
    diags.iter().any(|d| d["severity"] == "error")
}

fn cmd_validate(parsed: &Parsed, out: &Sink, err: &Sink) -> i32 {
    let Some(path) = parsed.positional.first().cloned() else {
        line(err, "用法：lyflow validate <graph>");
        return EXIT_USAGE;
    };
    let core = match core() {
        Ok(c) => c,
        Err(e) => return fail(err, &e, EXIT_FAILED),
    };
    let loaded = match load_graph(parsed, &path) {
        Ok(l) => l,
        Err(e) => return fail(err, &e, EXIT_INVALID),
    };
    let diags = match diagnostics_of(&core, &loaded) {
        Ok(d) => d,
        Err(e) => return fail(err, &e, EXIT_FAILED),
    };
    json_line(out, &Value::Array(diags.clone()));
    if has_errors(&diags) {
        line(err, &format!("{} 条错误", diags.len()));
        return EXIT_INVALID;
    }
    line(err, "校验通过");
    EXIT_OK
}

fn cmd_plan(parsed: &Parsed, out: &Sink, err: &Sink) -> i32 {
    let Some(path) = parsed.positional.first().cloned() else {
        line(err, "用法：lyflow plan <graph>");
        return EXIT_USAGE;
    };
    let core = match core() {
        Ok(c) => c,
        Err(e) => return fail(err, &e, EXIT_FAILED),
    };
    let loaded = match load_graph(parsed, &path) {
        Ok(l) => l,
        Err(e) => return fail(err, &e, EXIT_INVALID),
    };
    let targets: Vec<String> = parsed.many("to").to_vec();
    let raw = match core.plan(&loaded.json, &loaded.base_dir, &targets) {
        Ok(r) => r,
        Err(e) => return fail(err, &e.to_string(), EXIT_FAILED),
    };
    let items: Vec<Value> = match serde_json::from_str(&raw) {
        Ok(v) => v,
        Err(e) => return fail(err, &format!("core 返回的计划不是合法 JSON: {e}"), EXIT_FAILED),
    };
    json_line(out, &Value::Array(items.clone()));
    // 校验没过时 core 返回的是诊断数组，靠有没有 cacheKey 区分（ADR-0007）
    if items.iter().any(|n| !n["cacheKey"].is_string()) {
        line(err, "图当前不合法，编译不出计划");
        return EXIT_INVALID;
    }
    let recompute = items.iter().filter(|n| n["cached"] != true).count();
    line(err, &format!("{} 个节点，其中 {recompute} 个要重算", items.len()));
    EXIT_OK
}

fn cmd_migrate(parsed: &Parsed, out: &Sink, err: &Sink) -> i32 {
    let Some(path) = parsed.positional.first().cloned() else {
        line(err, "用法：lyflow migrate <graph> [--write]");
        return EXIT_USAGE;
    };
    let core = match core() {
        Ok(c) => c,
        Err(e) => return fail(err, &e, EXIT_FAILED),
    };
    let mut loaded = match load_graph(parsed, &path) {
        Ok(l) => l,
        Err(e) => return fail(err, &e, EXIT_INVALID),
    };
    let diags = match diagnostics_of(&core, &loaded) {
        Ok(d) => d,
        Err(e) => return fail(err, &e, EXIT_FAILED),
    };
    let migrations: Vec<Value> = diags
        .into_iter()
        .filter(|d| d["kind"] == "migration")
        .collect();

    let mut written = Value::Null;
    if parsed.has("write") && !migrations.is_empty() {
        // 与前端 applyMigrations 同一套语义：params 是完整对象而不是补丁（ADR-0008）
        for m in &migrations {
            let Some(node_id) = m["nodeId"].as_str() else { continue };
            let Some(node) = loaded.doc.nodes.iter_mut().find(|n| n.id == node_id) else {
                continue;
            };
            if let Some(op) = m["op"].as_str() {
                node.op = op.to_string();
            }
            if let Some(v) = m["opVersion"].as_str() {
                node.op_version = Some(v.to_string());
            }
            if let Some(params) = m["params"].as_object() {
                node.params = params.clone();
            }
        }
        let mut text = match serde_json::to_string_pretty(&loaded.doc) {
            Ok(t) => t,
            Err(e) => return fail(err, &e.to_string(), EXIT_FAILED),
        };
        text.push('\n');
        if let Err(e) = std::fs::write(&loaded.path, text) {
            return fail(err, &format!("写入 {} 失败: {e}", loaded.path.display()), EXIT_FAILED);
        }
        written = json!(loaded.path.to_string_lossy());
    }

    json_line(
        out,
        &json!({ "migrations": migrations, "written": written }),
    );
    line(err, &format!("{} 个节点需要迁移", migrations.len()));
    EXIT_OK
}

fn cmd_manifest(parsed: &Parsed, out: &Sink, err: &Sink) -> i32 {
    let core = match core() {
        Ok(c) => c,
        Err(e) => return fail(err, &e, EXIT_FAILED),
    };
    if parsed.has("check") {
        let problems = match core.manifest_problems() {
            Ok(p) => p,
            Err(e) => return fail(err, &e.to_string(), EXIT_FAILED),
        };
        json_line(out, &json!({ "problems": problems }));
        if problems.is_empty() {
            line(err, "算子描述自检干净");
            return EXIT_OK;
        }
        for p in &problems {
            line(err, &format!("  - {p}"));
        }
        return EXIT_INVALID;
    }
    match core.manifest_json() {
        // core 那份是缩进过的，压成一行才对得上「stdout 是 JSON Lines」
        Ok(raw) => match serde_json::from_str::<Value>(&raw) {
            Ok(v) => {
                json_line(out, &v);
                EXIT_OK
            }
            Err(e) => fail(err, &format!("core 返回的 manifest 不是合法 JSON: {e}"), EXIT_FAILED),
        },
        Err(e) => fail(err, &e.to_string(), EXIT_FAILED),
    }
}

fn cmd_run(parsed: &Parsed, out: &Sink, err: &Sink) -> i32 {
    let Some(path) = parsed.positional.first().cloned() else {
        line(err, "用法：lyflow run <graph>");
        return EXIT_USAGE;
    };
    let core = match core() {
        Ok(c) => c,
        Err(e) => return fail(err, &e, EXIT_FAILED),
    };
    let loaded = match load_graph(parsed, &path) {
        Ok(l) => l,
        Err(e) => return fail(err, &e, EXIT_INVALID),
    };
    // 先单独校验一遍：校验失败与执行失败是两个不同的退出码，混在一次 run 里分不开
    match diagnostics_of(&core, &loaded) {
        Ok(diags) if has_errors(&diags) => {
            for d in &diags {
                json_line(out, d);
            }
            line(err, "校验失败，没有执行");
            return EXIT_INVALID;
        }
        Ok(_) => {}
        Err(e) => return fail(err, &e, EXIT_FAILED),
    }
    let parallel = parsed
        .one("parallel")
        .and_then(|v| v.parse::<i32>().ok())
        .unwrap_or(0);
    let preview_points = parsed
        .one("preview-points")
        .and_then(|v| v.parse::<u32>().ok())
        .unwrap_or(0);
    let targets: Vec<String> = parsed.many("to").to_vec();

    let result = match execute(
        &core,
        RunRequest {
            graph_json: &loaded.json,
            base_dir: &loaded.base_dir,
            targets: &targets,
            parallel,
            preview_points,
            preview: parsed.has("preview"),
            no_cache: parsed.has("no-cache"),
            stream: Some(Arc::clone(out)),
        },
    ) {
        Ok(r) => r,
        Err(e) => return fail(err, &e, EXIT_FAILED),
    };
    // --outputs：图级命名输出（ADR-0017）。必须在 RunResult 还活着时取 ——
    // 它一 drop 就 lyflow_run_free，结果仓的索引跟着没了。
    if parsed.has("outputs") {
        match core.run_outputs(result.run_id()) {
            Ok(raw) => match serde_json::from_str::<Value>(&raw) {
                Ok(v) => json_line(out, &v),
                Err(e) => return fail(err, &format!("图输出不是合法 JSON: {e}"), EXIT_FAILED),
            },
            Err(e) => return fail(err, &e.to_string(), EXIT_FAILED),
        }
    }
    line(
        err,
        &format!("run {} in {:.0} ms", result.status, result.duration_ms()),
    );
    result.exit_code()
}

/// `lyflow import --kind <kind> <file> [-o out.json]`。导入器由算子包注册（ADR-0017）。
fn cmd_import(parsed: &Parsed, out: &Sink, err: &Sink) -> i32 {
    let Some(path) = parsed.positional.first().cloned() else {
        line(err, "用法：lyflow import --kind <kind> <file> [-o <out.lyflow.json>]");
        return EXIT_USAGE;
    };
    let Some(kind) = parsed.one("kind") else {
        line(err, "缺 --kind。可用的 kind 见 `lyflow manifest` 的 importers 段");
        return EXIT_USAGE;
    };
    let core = match core() {
        Ok(c) => c,
        Err(e) => return fail(err, &e, EXIT_FAILED),
    };
    let file = PathBuf::from(&path);
    let text = match std::fs::read_to_string(&file) {
        Ok(t) => t,
        Err(e) => return fail(err, &format!("读取 {path} 失败: {e}"), EXIT_INVALID),
    };
    // baseDir 默认是被导入文件所在目录：导入器写出来的相对路径据它解析
    let base_dir = match parsed.one("base-dir") {
        Some(d) => d.to_string(),
        None => file
            .parent()
            .map(|d| d.to_string_lossy().into_owned())
            .unwrap_or_default(),
    };

    let graph_json = match core.import(kind, &text, &base_dir) {
        Ok(g) => g,
        Err(diags) => {
            if let Ok(items) = serde_json::from_str::<Vec<Value>>(&diags) {
                for d in &items {
                    json_line(out, d);
                }
            } else {
                line(err, &diags);
            }
            line(err, "导入失败");
            return EXIT_INVALID;
        }
    };

    // 导入器产出的图必须自己就是合法的 GraphDoc：坏图不该落盘
    let doc: GraphDoc = match serde_json::from_str(&graph_json) {
        Ok(d) => d,
        Err(e) => return fail(err, &format!("导入器产出的不是合法 GraphDoc: {e}"), EXIT_FAILED),
    };
    if let Err(e) = doc.validate_structure() {
        return fail(err, &e.to_string(), EXIT_FAILED);
    }

    let pretty = match serde_json::to_string_pretty(&doc) {
        Ok(t) => t,
        Err(e) => return fail(err, &e.to_string(), EXIT_FAILED),
    };
    match parsed.one("output") {
        Some(target) => {
            let dest = PathBuf::from(target);
            if let Some(dir) = dest.parent() {
                if !dir.as_os_str().is_empty() {
                    let _ = std::fs::create_dir_all(dir);
                }
            }
            if let Err(e) = std::fs::write(&dest, pretty + "\n") {
                return fail(err, &format!("写 {target} 失败: {e}"), EXIT_FAILED);
            }
            line(
                err,
                &format!("{target}  ({} 节点, {} 连线)", doc.nodes.len(), doc.edges.len()),
            );
        }
        None => line(out, &pretty),
    }
    EXIT_OK
}

fn cmd_dump(parsed: &Parsed, out: &Sink, err: &Sink) -> i32 {
    if parsed.positional.len() < 3 {
        line(err, "用法：lyflow dump <graph> <nodeId>:<port> <out.pcd>");
        return EXIT_USAGE;
    }
    let path = parsed.positional[0].clone();
    let Some((node_id, port)) = parsed.positional[1].split_once(':') else {
        line(err, "目标的写法是 nodeId:port");
        return EXIT_USAGE;
    };
    let target_file = parsed.positional[2].clone();
    let core = match core() {
        Ok(c) => c,
        Err(e) => return fail(err, &e, EXIT_FAILED),
    };
    let loaded = match load_graph(parsed, &path) {
        Ok(l) => l,
        Err(e) => return fail(err, &e, EXIT_INVALID),
    };
    let targets = vec![node_id.to_string()];
    let result = match execute(
        &core,
        RunRequest {
            graph_json: &loaded.json,
            base_dir: &loaded.base_dir,
            targets: &targets,
            parallel: 0,
            preview_points: 0,
            preview: false,
            no_cache: parsed.has("no-cache"),
            stream: Some(Arc::clone(out)),
        },
    ) {
        Ok(r) => r,
        Err(e) => return fail(err, &e, EXIT_FAILED),
    };
    if result.status != "ok" {
        line(err, &format!("运行 {}，没有可写的结果", result.status));
        return result.exit_code();
    }
    // dump 的目标路径按当前工作目录解析，不跟着图文件走 —— 命令行里的路径就该是命令行的
    let format = parsed.one("format").unwrap_or("binary").to_string();
    let run_id = result.events
        .first()
        .and_then(|e| e["runId"].as_str())
        .unwrap_or_default()
        .to_string();
    if let Err(e) = core.output_save(&run_id, node_id, port, &target_file, &format) {
        return fail(err, &e, EXIT_FAILED);
    }
    let count = result.metric(node_id, port, "elementCount").unwrap_or(0.0);
    json_line(
        out,
        &json!({
            "kind": "dump_written",
            "nodeId": node_id,
            "port": port,
            "path": target_file,
            "elementCount": count,
        }),
    );
    line(err, &format!("写出 {count} 个元素 -> {target_file}"));
    EXIT_OK
}

/// `nodeId.param=start:end:steps`。steps=1 时只取 start。
struct SweepAxis {
    node: String,
    param: String,
    values: Vec<f64>,
    /// vecNf 参数的分量数。0 = 标量。扫 leafSize 时一个数广播到三个分量。
    components: usize,
}

fn parse_axis(spec: &str) -> Result<SweepAxis, String> {
    let (left, range) = spec
        .split_once('=')
        .ok_or_else(|| format!("--param 的写法是 nodeId.param=start:end:steps，收到 {spec}"))?;
    let (node, param) = left
        .rsplit_once('.')
        .ok_or_else(|| format!("--param 的写法是 nodeId.param=start:end:steps，收到 {spec}"))?;
    let parts: Vec<&str> = range.split(':').collect();
    if parts.len() != 3 {
        return Err(format!("范围的写法是 start:end:steps，收到 {range}"));
    }
    let start: f64 = parts[0].parse().map_err(|_| format!("start 不是数字: {}", parts[0]))?;
    let end: f64 = parts[1].parse().map_err(|_| format!("end 不是数字: {}", parts[1]))?;
    let steps: usize = parts[2].parse().map_err(|_| format!("steps 不是整数: {}", parts[2]))?;
    if steps == 0 {
        return Err("steps 至少是 1".to_string());
    }
    let mut values = Vec::with_capacity(steps);
    for i in 0..steps {
        values.push(if steps == 1 {
            start
        } else {
            start + (end - start) * (i as f64) / ((steps - 1) as f64)
        });
    }
    Ok(SweepAxis {
        node: node.to_string(),
        param: param.to_string(),
        values,
        components: 0,
    })
}

/// 这个参数当前是几个分量。先看图里写了什么，再看 manifest 的默认值，
/// 最后看子图定义 —— 三处都问不到就当标量。
fn component_count(
    doc: &GraphDoc,
    defaults: &BTreeMap<String, BTreeMap<String, Value>>,
    node_id: &str,
    param: &str,
) -> usize {
    let Some(node) = doc.nodes.iter().find(|n| n.id == node_id) else {
        return 0;
    };
    if let Some(Value::Array(a)) = node.params.get(param) {
        return a.len();
    }
    if let Some(Value::Array(a)) = defaults.get(&node.op).and_then(|d| d.get(param)) {
        return a.len();
    }
    if let Some(id) = node.op.strip_prefix("sub:") {
        let declared = doc.subgraphs.get(id).and_then(|def| {
            def.get("params")?
                .as_array()?
                .iter()
                .find(|p| p["name"] == param)
                .map(|p| p["default"].clone())
        });
        if let Some(Value::Array(a)) = declared {
            return a.len();
        }
    }
    0
}

fn cmd_sweep(parsed: &Parsed, out: &Sink, err: &Sink) -> i32 {
    let Some(path) = parsed.positional.first().cloned() else {
        line(err, "用法：lyflow sweep <graph> --param ... --metric ...");
        return EXIT_USAGE;
    };
    let mut axes: Vec<SweepAxis> = match parsed
        .many("param")
        .iter()
        .map(|s| parse_axis(s))
        .collect::<Result<Vec<_>, _>>()
    {
        Ok(a) => a,
        Err(e) => return fail(err, &e, EXIT_USAGE),
    };
    if axes.is_empty() {
        line(err, "至少给一个 --param nodeId.param=start:end:steps");
        return EXIT_USAGE;
    }
    let Some(metric) = parsed.one("metric").map(str::to_string) else {
        line(err, "缺 --metric nodeId:port.field");
        return EXIT_USAGE;
    };
    let Some((metric_target, field)) = metric.rsplit_once('.') else {
        line(err, "--metric 的写法是 nodeId:port.field");
        return EXIT_USAGE;
    };
    let Some((metric_node, metric_port)) = metric_target.split_once(':') else {
        line(err, "--metric 的写法是 nodeId:port.field");
        return EXIT_USAGE;
    };

    let core = match core() {
        Ok(c) => c,
        Err(e) => return fail(err, &e, EXIT_FAILED),
    };
    let loaded = match load_graph(parsed, &path) {
        Ok(l) => l,
        Err(e) => return fail(err, &e, EXIT_INVALID),
    };
    let defaults = match defaults_by_op(&core) {
        Ok(d) => d,
        Err(e) => return fail(err, &e, EXIT_FAILED),
    };
    for axis in &mut axes {
        axis.components = component_count(&loaded.doc, &defaults, &axis.node, &axis.param);
    }

    // 笛卡尔积。上游靠缓存只算一次 —— 这正是 sweep 值得做在同一个进程里的理由。
    let total: usize = axes.iter().map(|a| a.values.len()).product();
    let mut rows: Vec<Vec<u8>> = Vec::new();
    let mut worst = EXIT_OK;
    for index in 0..total {
        let mut doc = loaded.doc.clone();
        let mut combo = serde_json::Map::new();
        let mut rest = index;
        for axis in &axes {
            let pick = rest % axis.values.len();
            rest /= axis.values.len();
            let value = axis.values[pick];
            let Some(node) = doc.nodes.iter_mut().find(|n| n.id == axis.node) else {
                return fail(err, &format!("图里没有节点 {}", axis.node), EXIT_USAGE);
            };
            // vecNf 的参数把一个数广播到全部分量：扫 leafSize 才写得出来
            let written = if axis.components > 0 {
                json!(vec![value; axis.components])
            } else {
                json!(value)
            };
            node.params.insert(axis.param.clone(), written);
            combo.insert(format!("{}.{}", axis.node, axis.param), json!(value));
        }
        let graph_json = match serde_json::to_string(&doc) {
            Ok(j) => j,
            Err(e) => return fail(err, &e.to_string(), EXIT_FAILED),
        };
        let result = match execute(
            &core,
            RunRequest {
                graph_json: &graph_json,
                base_dir: &loaded.base_dir,
                targets: &[],
                parallel: 0,
                preview_points: 0,
                preview: false,
                no_cache: false,
                stream: None,
            },
        ) {
            Ok(r) => r,
            Err(e) => return fail(err, &e, EXIT_FAILED),
        };
        if result.exit_code() != EXIT_OK {
            worst = result.exit_code();
        }
        let value = result.metric(metric_node, metric_port, field);
        let row = json!({
            "kind": "sweep_row",
            "index": index,
            "params": Value::Object(combo.clone()),
            "metric": metric,
            "value": value,
            "status": result.status,
            "durationMs": result.duration_ms(),
            "skipped": result.skipped_nodes(),
        });
        json_line(out, &row);
        let mut csv = Vec::new();
        for axis in &axes {
            let key = format!("{}.{}", axis.node, axis.param);
            let _ = write!(csv, "{},", combo[&key]);
        }
        let _ = write!(
            csv,
            "{}",
            value.map(|v| v.to_string()).unwrap_or_default()
        );
        rows.push(csv);
    }

    if let Some(csv_path) = parsed.one("csv") {
        let mut text = String::new();
        for axis in &axes {
            text.push_str(&format!("{}.{},", axis.node, axis.param));
        }
        text.push_str(&metric);
        text.push('\n');
        for row in &rows {
            text.push_str(&String::from_utf8_lossy(row));
            text.push('\n');
        }
        if let Err(e) = std::fs::write(csv_path, text) {
            return fail(err, &format!("写入 {csv_path} 失败: {e}"), EXIT_FAILED);
        }
        line(err, &format!("表格写到 {csv_path}"));
    }
    line(err, &format!("扫了 {total} 组"));
    worst
}

// ---------------------------------------------------------------------- diff

/// 合并默认值之后的参数。稀疏存储让「没写」和「写了个等于默认的值」在文件里长得不一样。
fn effective_params(defaults: &BTreeMap<String, Value>, node: &crate::graph::Node) -> BTreeMap<String, Value> {
    let mut out = defaults.clone();
    for (k, v) in &node.params {
        if out.contains_key(k) {
            out.insert(k.clone(), v.clone());
        }
    }
    out
}

fn defaults_by_op(core: &Arc<Core>) -> Result<BTreeMap<String, BTreeMap<String, Value>>, String> {
    let raw = core.manifest_json().map_err(|e| e.to_string())?;
    let manifest: Value = serde_json::from_str(&raw).map_err(|e| e.to_string())?;
    let mut out = BTreeMap::new();
    for op in manifest["operators"].as_array().cloned().unwrap_or_default() {
        let Some(id) = op["id"].as_str() else { continue };
        let mut defaults = BTreeMap::new();
        for p in op["params"].as_array().cloned().unwrap_or_default() {
            if let Some(name) = p["name"].as_str() {
                defaults.insert(name.to_string(), p["default"].clone());
            }
        }
        out.insert(id.to_string(), defaults);
    }
    Ok(out)
}

fn edge_key(e: &crate::graph::Edge) -> String {
    format!("{}.{} -> {}.{}", e.from.node, e.from.port, e.to.node, e.to.port)
}

fn cmd_diff(parsed: &Parsed, out: &Sink, err: &Sink) -> i32 {
    if parsed.positional.len() < 2 {
        line(err, "用法：lyflow diff <a> <b> [--json]");
        return EXIT_USAGE;
    }
    let core = match core() {
        Ok(c) => c,
        Err(e) => return fail(err, &e, EXIT_FAILED),
    };
    let defaults = match defaults_by_op(&core) {
        Ok(d) => d,
        Err(e) => return fail(err, &e, EXIT_FAILED),
    };
    let empty = Parsed {
        positional: Vec::new(),
        values: BTreeMap::new(),
        flags: HashSet::new(),
    };
    let a = match load_graph(&empty, &parsed.positional[0]) {
        Ok(l) => l,
        Err(e) => return fail(err, &e, EXIT_INVALID),
    };
    let b = match load_graph(&empty, &parsed.positional[1]) {
        Ok(l) => l,
        Err(e) => return fail(err, &e, EXIT_INVALID),
    };

    let mut added = Vec::new();
    let mut removed = Vec::new();
    let mut changed = Vec::new();
    let no_defaults = BTreeMap::new();

    for node in &b.doc.nodes {
        match a.doc.nodes.iter().find(|n| n.id == node.id) {
            None => added.push(json!({ "id": node.id, "op": node.op })),
            Some(old) => {
                let mut fields = serde_json::Map::new();
                if old.op != node.op {
                    fields.insert("op".into(), json!({ "from": old.op, "to": node.op }));
                }
                if old.bypass != node.bypass {
                    fields.insert("bypass".into(), json!({ "from": old.bypass, "to": node.bypass }));
                }
                let d = defaults.get(&node.op).unwrap_or(&no_defaults);
                let pa = effective_params(d, old);
                let pb = effective_params(d, node);
                let mut params = serde_json::Map::new();
                for (k, v) in &pb {
                    let before = pa.get(k);
                    if before != Some(v) {
                        params.insert(k.clone(), json!({ "from": before, "to": v }));
                    }
                }
                if !params.is_empty() {
                    fields.insert("params".into(), Value::Object(params));
                }
                if !fields.is_empty() {
                    fields.insert("id".into(), json!(node.id));
                    changed.push(Value::Object(fields));
                }
            }
        }
    }
    for node in &a.doc.nodes {
        if !b.doc.nodes.iter().any(|n| n.id == node.id) {
            removed.push(json!({ "id": node.id, "op": node.op }));
        }
    }

    let keys_a: HashSet<String> = a.doc.edges.iter().map(edge_key).collect();
    let keys_b: HashSet<String> = b.doc.edges.iter().map(edge_key).collect();
    let mut edges_added: Vec<String> = keys_b.difference(&keys_a).cloned().collect();
    let mut edges_removed: Vec<String> = keys_a.difference(&keys_b).cloned().collect();
    edges_added.sort();
    edges_removed.sort();

    let mut subgraphs = Vec::new();
    for (id, def) in &b.doc.subgraphs {
        match a.doc.subgraphs.get(id) {
            None => subgraphs.push(json!({ "id": id, "change": "added" })),
            Some(old) if old != def => subgraphs.push(json!({ "id": id, "change": "modified" })),
            _ => {}
        }
    }
    for id in a.doc.subgraphs.keys() {
        if !b.doc.subgraphs.contains_key(id) {
            subgraphs.push(json!({ "id": id, "change": "removed" }));
        }
    }

    let empty_diff = added.is_empty()
        && removed.is_empty()
        && changed.is_empty()
        && edges_added.is_empty()
        && edges_removed.is_empty()
        && subgraphs.is_empty();

    if parsed.has("json") {
        json_line(
            out,
            &json!({
                "nodesAdded": added,
                "nodesRemoved": removed,
                "nodesChanged": changed,
                "edgesAdded": edges_added,
                "edgesRemoved": edges_removed,
                "subgraphs": subgraphs,
                "empty": empty_diff,
            }),
        );
        return EXIT_OK;
    }

    if empty_diff {
        line(err, "两份图在语义上完全一样（ui 不算）");
        return EXIT_OK;
    }
    for n in &added {
        line(out, &format!("+ 节点 {} ({})", n["id"], n["op"]));
    }
    for n in &removed {
        line(out, &format!("- 节点 {} ({})", n["id"], n["op"]));
    }
    for n in &changed {
        line(out, &format!("~ 节点 {}", n["id"]));
        if let Some(params) = n["params"].as_object() {
            for (k, v) in params {
                line(out, &format!("    {k}: {} -> {}", v["from"], v["to"]));
            }
        }
        if n["op"].is_object() {
            line(out, &format!("    op: {} -> {}", n["op"]["from"], n["op"]["to"]));
        }
        if n["bypass"].is_object() {
            line(out, &format!("    bypass: {} -> {}", n["bypass"]["from"], n["bypass"]["to"]));
        }
    }
    for e in &edges_added {
        line(out, &format!("+ 边 {e}"));
    }
    for e in &edges_removed {
        line(out, &format!("- 边 {e}"));
    }
    for s in &subgraphs {
        line(out, &format!("~ 子图 {} {}", s["id"], s["change"]));
    }
    EXIT_OK
}

// ---------------------------------------------------------------------- 入口

fn fail(err: &Sink, message: &str, code: i32) -> i32 {
    line(err, message);
    code
}

const VALUE_OPTS: &[&str] = &[
    "to", "set", "base-dir", "parallel", "preview-points", "param", "metric", "csv", "format",
    "kind", "output",
];
const BOOL_OPTS: &[&str] = &["no-cache", "preview", "write", "check", "json", "help", "outputs"];

pub fn run_cli(args: &[String], out: &Sink, err: &Sink) -> i32 {
    let Some(command) = args.first().cloned() else {
        line(err, USAGE);
        return EXIT_USAGE;
    };
    if command == "--help" || command == "-h" || command == "help" {
        line(err, USAGE);
        return EXIT_OK;
    }
    if command == "--version" {
        line(out, &json!({ "core": core_ffi::version() }).to_string());
        return EXIT_OK;
    }
    let parsed = match parse_args(&args[1..], VALUE_OPTS, BOOL_OPTS) {
        Ok(p) => p,
        Err(e) => {
            line(err, &e);
            line(err, USAGE);
            return EXIT_USAGE;
        }
    };
    match command.as_str() {
        "run" => cmd_run(&parsed, out, err),
        "validate" => cmd_validate(&parsed, out, err),
        "plan" => cmd_plan(&parsed, out, err),
        "migrate" => cmd_migrate(&parsed, out, err),
        "manifest" => cmd_manifest(&parsed, out, err),
        "import" => cmd_import(&parsed, out, err),
        "dump" => cmd_dump(&parsed, out, err),
        "sweep" => cmd_sweep(&parsed, out, err),
        "diff" => cmd_diff(&parsed, out, err),
        other => {
            line(err, &format!("不认识的子命令 {other}"));
            line(err, USAGE);
            EXIT_USAGE
        }
    }
}

/// 库算子目录。CLI 与 app 用同一份列表，否则会出现「界面里能跑、CI 里报缺算子」。
/// 顺序与 app 一致：app data 下的 library/，再加 LYFLOW_LIBRARY_DIRS（分号分隔）。
pub fn library_dirs() -> Vec<String> {
    let mut dirs = Vec::new();
    if let Some(base) = std::env::var_os("APPDATA").map(PathBuf::from) {
        dirs.push(
            base.join("com.lyflow.app")
                .join("library")
                .to_string_lossy()
                .into_owned(),
        );
    }
    if let Ok(extra) = std::env::var("LYFLOW_LIBRARY_DIRS") {
        for d in extra.split(';').filter(|d| !d.is_empty()) {
            dirs.push(d.to_string());
        }
    }
    dirs
}

/// 进程启动时装一次库算子。目录不存在时什么都不做。
pub fn load_library(dirs: &[String]) -> Vec<String> {
    let Ok(core) = core_ffi::core() else {
        return Vec::new();
    };
    let existing: Vec<String> = dirs
        .iter()
        .filter(|d| Path::new(d).is_dir())
        .cloned()
        .collect();
    if existing.is_empty() {
        return Vec::new();
    }
    core.set_library_dirs(&existing).unwrap_or_default()
}

pub fn main() -> i32 {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let out = sink_of(std::io::stdout());
    let err = sink_of(std::io::stderr());
    for problem in load_library(&library_dirs()) {
        line(&err, &format!("库算子: {problem}"));
    }
    run_cli(&args, &out, &err)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[derive(Clone)]
    struct SharedBuf(Arc<Mutex<Vec<u8>>>);

    impl Write for SharedBuf {
        fn write(&mut self, buf: &[u8]) -> std::io::Result<usize> {
            self.0.lock().unwrap_or_else(|e| e.into_inner()).extend_from_slice(buf);
            Ok(buf.len())
        }
        fn flush(&mut self) -> std::io::Result<()> {
            Ok(())
        }
    }

    struct Ran {
        code: i32,
        out: String,
        err: String,
    }

    impl Ran {
        /// stdout 是 JSON Lines：一行一个对象。
        fn lines(&self) -> Vec<Value> {
            self.out
                .lines()
                .filter(|l| !l.trim().is_empty())
                .map(|l| serde_json::from_str(l).unwrap_or_else(|e| panic!("不是 JSON: {l} ({e})")))
                .collect()
        }
        fn first(&self) -> Value {
            self.lines().into_iter().next().expect("stdout 是空的")
        }
    }

    fn cli(args: &[&str]) -> Ran {
        let obuf = Arc::new(Mutex::new(Vec::new()));
        let ebuf = Arc::new(Mutex::new(Vec::new()));
        let out = sink_of(SharedBuf(Arc::clone(&obuf)));
        let err = sink_of(SharedBuf(Arc::clone(&ebuf)));
        let owned: Vec<String> = args.iter().map(|s| (*s).to_string()).collect();
        let code = run_cli(&owned, &out, &err);
        let out_text = String::from_utf8_lossy(&obuf.lock().unwrap()).into_owned();
        let err_text = String::from_utf8_lossy(&ebuf.lock().unwrap()).into_owned();
        Ran {
            code,
            out: out_text,
            err: err_text,
        }
    }

    /// 每个测试一个目录：cargo 默认并行跑，共用目录会互相覆盖。
    fn workspace(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("lyflow-cli-{name}"));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    /// seed 参数化不是装饰：缓存是进程级的，两个测试用同一张图的话后跑的那个
    /// 会拿到 skipped 而不是 done。
    fn chain(dir: &Path, seed: i64) -> String {
        let doc = json!({
            "schemaVersion": 1,
            "id": "01J8XQZ4K7N3M2R5V8W1YB6TCD",
            "name": "cli",
            "nodes": [
                {"id": "g", "op": "gen.synthetic",
                 "params": {"pointCount": 20000, "seed": seed}},
                {"id": "v", "op": "filter.voxel_grid",
                 "params": {"leafSize": [0.02, 0.02, 0.02]}}
            ],
            "edges": [
                {"id": "e1", "from": {"node": "g", "port": "cloud"},
                             "to": {"node": "v", "port": "cloud"}}
            ]
        });
        let file = dir.join("g.lyflow.json");
        std::fs::write(&file, serde_json::to_string_pretty(&doc).unwrap()).unwrap();
        file.to_string_lossy().into_owned()
    }

    #[test]
    fn manifest_check_is_clean() {
        let r = cli(&["manifest", "--check"]);
        assert_eq!(r.code, EXIT_OK, "{}", r.err);
        assert_eq!(r.first()["problems"].as_array().unwrap().len(), 0);
    }

    #[test]
    fn manifest_dumps_the_whole_bundle() {
        let r = cli(&["manifest"]);
        assert_eq!(r.code, EXIT_OK);
        let m = r.first();
        assert_eq!(m["schemaVersion"], 1);
        assert!(m["operators"].as_array().unwrap().len() >= 16);
    }

    #[test]
    fn validate_accepts_a_good_graph_and_rejects_a_bad_one() {
        let dir = workspace("validate");
        let good = chain(&dir, 301);
        let r = cli(&["validate", &good]);
        assert_eq!(r.code, EXIT_OK, "{}", r.err);
        assert_eq!(r.first(), json!([]));

        let bad = cli(&["validate", &good, "--set", "v.leafSize=[0,0.01,0.01]"]);
        assert_eq!(bad.code, EXIT_INVALID);
        let diags = bad.first();
        assert_eq!(diags[0]["code"], "bad_param");
        assert_eq!(diags[0]["paramPath"], "leafSize");
    }

    #[test]
    fn plan_reports_cache_keys() {
        let dir = workspace("plan");
        let graph = chain(&dir, 302);
        let r = cli(&["plan", &graph]);
        assert_eq!(r.code, EXIT_OK, "{}", r.err);
        let nodes = r.first();
        assert_eq!(nodes[0]["nodeId"], "g");
        assert_eq!(nodes[0]["cacheKey"].as_str().unwrap().len(), 32);
        assert_eq!(nodes[1]["level"], 1);
    }

    #[test]
    fn run_streams_execution_events_as_json_lines() {
        let dir = workspace("run");
        let graph = chain(&dir, 303);
        let r = cli(&["run", &graph, "--no-cache"]);
        assert_eq!(r.code, EXIT_OK, "{}", r.err);

        let lines = r.lines();
        assert_eq!(lines.first().unwrap()["kind"], "run_started");
        assert_eq!(lines.last().unwrap()["kind"], "run_finished");
        assert_eq!(lines.last().unwrap()["status"], "ok");
        // seq 连续：CI 消费的和前端消费的是同一条流（F7）
        for (i, e) in lines.iter().enumerate() {
            assert_eq!(e["seq"], i as i64, "seq 不连续：{e}");
            assert_eq!(e["schemaVersion"], 1);
        }
        let done = lines
            .iter()
            .find(|e| e["kind"] == "node_state" && e["nodeId"] == "v" && e["state"] == "done")
            .expect("v 没跑完");
        assert!(done["stats"]["elementCount"].as_i64().unwrap() > 0);
    }

    #[test]
    fn run_set_overrides_a_param() {
        let dir = workspace("set");
        let graph = chain(&dir, 304);
        let r = cli(&["run", &graph, "--set", "g.pointCount=1234", "--no-cache"]);
        assert_eq!(r.code, EXIT_OK, "{}", r.err);
        let done = r
            .lines()
            .into_iter()
            .find(|e| e["kind"] == "node_state" && e["nodeId"] == "g" && e["state"] == "done")
            .unwrap();
        assert_eq!(done["stats"]["elementCount"], 1234);
    }

    #[test]
    fn run_to_node_prunes_the_downstream() {
        let dir = workspace("runto");
        let graph = chain(&dir, 305);
        let r = cli(&["run", &graph, "--to", "g"]);
        assert_eq!(r.code, EXIT_OK, "{}", r.err);
        let started = r.first();
        let plan: Vec<&str> = started["plan"]
            .as_array()
            .unwrap()
            .iter()
            .map(|v| v.as_str().unwrap())
            .collect();
        assert_eq!(plan, vec!["g"]);
    }

    #[test]
    fn run_exits_one_on_validation_failure() {
        let dir = workspace("badrun");
        let graph = chain(&dir, 306);
        let r = cli(&["run", &graph, "--set", "v.leafSize=[0,0,0]"]);
        assert_eq!(r.code, EXIT_INVALID, "{}", r.err);
        // 校验失败时输出的是诊断而不是事件
        assert_eq!(r.first()["kind"], "diagnostic");
    }

    #[test]
    fn run_exits_two_when_a_node_fails() {
        let dir = workspace("failrun");
        let doc = json!({
            "schemaVersion": 1, "id": "01J8XQZ4K7N3M2R5V8W1YB6TCE",
            "nodes": [{"id": "r", "op": "io.load_pcd", "params": {"path": "没有这个文件.pcd"}}],
            "edges": []
        });
        let file = dir.join("bad.lyflow.json");
        std::fs::write(&file, doc.to_string()).unwrap();
        let r = cli(&["run", &file.to_string_lossy()]);
        assert_eq!(r.code, EXIT_FAILED, "{} / {}", r.out, r.err);
        assert_eq!(r.lines().last().unwrap()["status"], "error");
    }

    #[test]
    fn unknown_subcommand_and_option_are_usage_errors() {
        assert_eq!(cli(&["nope"]).code, EXIT_USAGE);
        assert_eq!(cli(&["run", "x.json", "--nope"]).code, EXIT_USAGE);
        assert_eq!(cli(&[]).code, EXIT_USAGE);
    }

    #[test]
    fn dump_writes_the_output_to_disk() {
        let dir = workspace("dump");
        let graph = chain(&dir, 307);
        let target = dir.join("out.pcd");
        let r = cli(&["dump", &graph, "v:cloud", &target.to_string_lossy()]);
        assert_eq!(r.code, EXIT_OK, "{}", r.err);
        assert!(target.exists(), "没写出 {}", target.display());
        assert!(std::fs::metadata(&target).unwrap().len() > 1000);
        let last = r.lines().last().cloned().unwrap();
        assert_eq!(last["kind"], "dump_written");
        assert!(last["elementCount"].as_f64().unwrap() > 0.0);
    }

    /// §3 验收：sweep 5 组 leafSize，源头只加载一次（其余 4 次 skipped）。
    #[test]
    fn sweep_reuses_the_upstream_across_the_grid() {
        let dir = workspace("sweep");
        let graph = chain(&dir, 308);
        let csv = dir.join("out.csv");
        let r = cli(&[
            "sweep",
            &graph,
            "--param",
            "v.minPointsPerVoxel=1:5:5",
            "--metric",
            "v:cloud.elementCount",
            "--csv",
            &csv.to_string_lossy(),
        ]);
        assert_eq!(r.code, EXIT_OK, "{}", r.err);
        let rows = r.lines();
        assert_eq!(rows.len(), 5);
        let with_skip = rows
            .iter()
            .filter(|row| {
                row["skipped"]
                    .as_array()
                    .map(|s| s.iter().any(|v| v == "g"))
                    .unwrap_or(false)
            })
            .count();
        assert_eq!(with_skip, 4, "源头应当只算一次: {}", r.out);
        // 指标随参数单调下降 —— 说明扫的是真参数而不是同一份结果
        let first = rows[0]["value"].as_f64().unwrap();
        let last = rows[4]["value"].as_f64().unwrap();
        assert!(last < first, "{first} -> {last}");

        let text = std::fs::read_to_string(&csv).unwrap();
        assert!(text.starts_with("v.minPointsPerVoxel,v:cloud.elementCount"));
        assert_eq!(text.lines().count(), 6);
    }

    /// m4-plan §3 的验收原话是「sweep 5 组 leafSize」——而 leafSize 是 vec3f。
    /// 一个数要能广播到三个分量，否则那条验收根本写不出来。
    #[test]
    fn sweep_broadcasts_a_scalar_onto_a_vector_param() {
        let dir = workspace("sweepvec");
        let graph = chain(&dir, 315);
        let r = cli(&[
            "sweep",
            &graph,
            "--param",
            "v.leafSize=0.01:0.05:5",
            "--metric",
            "v:cloud.elementCount",
        ]);
        assert_eq!(r.code, EXIT_OK, "{}", r.err);
        let rows = r.lines();
        assert_eq!(rows.len(), 5);
        for row in &rows {
            assert_eq!(row["status"], "ok", "{row}");
            assert!(row["value"].as_f64().unwrap() > 0.0, "{row}");
        }
        // leafSize 越大点越少
        assert!(rows[4]["value"].as_f64().unwrap() < rows[0]["value"].as_f64().unwrap());
        // 源头只算一次
        let reused = rows
            .iter()
            .filter(|r| r["skipped"].as_array().unwrap().iter().any(|v| v == "g"))
            .count();
        assert_eq!(reused, 4, "{}", r.out);
    }

    #[test]
    fn sweep_rejects_a_malformed_axis() {
        let dir = workspace("sweepbad");
        let graph = chain(&dir, 309);
        let r = cli(&["sweep", &graph, "--param", "v.x=1:2", "--metric", "v:cloud.elementCount"]);
        assert_eq!(r.code, EXIT_USAGE);
    }

    /// §3 验收：只移动了节点的两份图，diff 输出为空。
    #[test]
    fn diff_ignores_ui_and_catches_params() {
        let dir = workspace("diff");
        let graph = chain(&dir, 310);
        let mut doc: Value = serde_json::from_str(&std::fs::read_to_string(&graph).unwrap()).unwrap();
        for node in doc["nodes"].as_array_mut().unwrap() {
            node["ui"] = json!({"position": {"x": 999, "y": 42}, "title": "挪过的"});
        }
        let moved = dir.join("moved.lyflow.json");
        std::fs::write(&moved, doc.to_string()).unwrap();

        let r = cli(&["diff", &graph, &moved.to_string_lossy(), "--json"]);
        assert_eq!(r.code, EXIT_OK, "{}", r.err);
        assert_eq!(r.first()["empty"], true, "{}", r.out);

        doc["nodes"][1]["params"]["leafSize"] = json!([0.05, 0.05, 0.05]);
        doc["nodes"].as_array_mut().unwrap().push(json!({
            "id": "p", "op": "filter.passthrough"
        }));
        let changed = dir.join("changed.lyflow.json");
        std::fs::write(&changed, doc.to_string()).unwrap();

        let r = cli(&["diff", &graph, &changed.to_string_lossy(), "--json"]);
        let d = r.first();
        assert_eq!(d["empty"], false);
        assert_eq!(d["nodesAdded"][0]["id"], "p");
        assert_eq!(d["nodesChanged"][0]["id"], "v");
        assert_eq!(d["nodesChanged"][0]["params"]["leafSize"]["to"], json!([0.05, 0.05, 0.05]));
    }

    /// 稀疏存储：写一个等于默认值的参数不该被 diff 当成变化。
    #[test]
    fn diff_merges_defaults_before_comparing() {
        let dir = workspace("diffdefault");
        let graph = chain(&dir, 311);
        let mut doc: Value = serde_json::from_str(&std::fs::read_to_string(&graph).unwrap()).unwrap();
        // 默认值从 manifest 现取：写死一个数字会在算子调默认值那天变成假绿
        let manifest = cli(&["manifest"]).first();
        let default_noise = manifest["operators"]
            .as_array()
            .unwrap()
            .iter()
            .find(|op| op["id"] == "gen.synthetic")
            .and_then(|op| op["params"].as_array())
            .and_then(|ps| ps.iter().find(|p| p["name"] == "noise"))
            .map(|p| p["default"].clone())
            .expect("manifest 里没有 gen.synthetic.noise");
        doc["nodes"][0]["params"]["noise"] = default_noise;
        let same = dir.join("same.lyflow.json");
        std::fs::write(&same, doc.to_string()).unwrap();
        let r = cli(&["diff", &graph, &same.to_string_lossy(), "--json"]);
        assert_eq!(r.first()["empty"], true, "{}", r.out);
    }

    /// ADR-0008：迁移只是诊断；--write 才落盘，落完再跑一次就没有迁移了。
    #[test]
    fn migrate_reports_and_optionally_writes() {
        let dir = workspace("migrate");
        let doc = json!({
            "schemaVersion": 1, "id": "01J8XQZ4K7N3M2R5V8W1YB6TCF",
            "nodes": [
                {"id": "g", "op": "gen.synthetic", "opVersion": "1.0.0",
                 "params": {"pointCount": 1000}},
                {"id": "s", "op": "filter.random_sample", "opVersion": "1.0.0",
                 "params": {"count": 250, "seed": 3}}
            ],
            "edges": [
                {"id": "e", "from": {"node": "g", "port": "cloud"},
                            "to": {"node": "s", "port": "cloud"}}
            ]
        });
        let file = dir.join("old.lyflow.json");
        std::fs::write(&file, serde_json::to_string_pretty(&doc).unwrap()).unwrap();
        let path = file.to_string_lossy().into_owned();

        let r = cli(&["migrate", &path]);
        assert_eq!(r.code, EXIT_OK, "{}", r.err);
        let report = r.first();
        assert_eq!(report["migrations"].as_array().unwrap().len(), 1);
        assert_eq!(report["written"], Value::Null);
        // 没有 --write 就一个字节都不许改
        assert!(std::fs::read_to_string(&file).unwrap().contains("\"count\""));

        let w = cli(&["migrate", &path, "--write"]);
        assert_eq!(w.code, EXIT_OK, "{}", w.err);
        assert!(w.first()["written"].is_string());
        let text = std::fs::read_to_string(&file).unwrap();
        assert!(text.contains("keepCount"), "{text}");
        assert!(text.ends_with('\n'));

        let again = cli(&["migrate", &path]);
        assert_eq!(again.first()["migrations"].as_array().unwrap().len(), 0);
    }

    /// F1：子图在 compile 前展开，CLI 看见的事件里只有路径式 id。
    #[test]
    fn subgraph_expands_into_path_ids() {
        let dir = workspace("subgraph");
        let doc = json!({
            "schemaVersion": 1, "id": "01J8XQZ4K7N3M2R5V8W1YB6TCG",
            "nodes": [
                {"id": "g", "op": "gen.synthetic", "params": {"pointCount": 20000, "seed": 312}},
                {"id": "s", "op": "sub:clean", "params": {"leaf": [0.03, 0.03, 0.03]}}
            ],
            "edges": [
                {"id": "e1", "from": {"node": "g", "port": "cloud"},
                             "to": {"node": "s", "port": "cloud"}}
            ],
            "subgraphs": {
                "clean": {
                    "name": "去噪",
                    "nodes": [{"id": "v", "op": "filter.voxel_grid"}],
                    "edges": [],
                    "inputs": [{"name": "cloud", "type": "PointCloud",
                                "to": [{"node": "v", "port": "cloud"}]}],
                    "outputs": [{"name": "cloud", "type": "PointCloud",
                                 "from": {"node": "v", "port": "cloud"}}],
                    "params": [{"name": "leaf", "type": "vec3f",
                                "default": [0.02, 0.02, 0.02],
                                "binds": [{"node": "v", "param": "leafSize"}]}]
                }
            }
        });
        let file = dir.join("sub.lyflow.json");
        std::fs::write(&file, doc.to_string()).unwrap();
        let r = cli(&["run", &file.to_string_lossy(), "--no-cache"]);
        assert_eq!(r.code, EXIT_OK, "{} / {}", r.out, r.err);
        let plan: Vec<String> = r.first()["plan"]
            .as_array()
            .unwrap()
            .iter()
            .map(|v| v.as_str().unwrap().to_string())
            .collect();
        assert_eq!(plan, vec!["g".to_string(), "s/v".to_string()]);
    }

    #[test]
    fn preview_decimates_the_source() {
        let dir = workspace("preview");
        let graph = chain(&dir, 313);
        let r = cli(&[
            "run",
            &graph,
            "--preview",
            "--preview-points",
            "2000",
            "--no-cache",
        ]);
        assert_eq!(r.code, EXIT_OK, "{}", r.err);
        let done = r
            .lines()
            .into_iter()
            .find(|e| e["kind"] == "node_state" && e["nodeId"] == "g" && e["state"] == "done")
            .unwrap();
        assert_eq!(done["stats"]["elementCount"], 2000);
    }

    #[test]
    fn set_rejects_an_unknown_node() {
        let dir = workspace("setbad");
        let graph = chain(&dir, 314);
        let r = cli(&["run", &graph, "--set", "nope.x=1"]);
        assert_eq!(r.code, EXIT_INVALID);
        assert!(r.err.contains("没有节点"), "{}", r.err);
    }
}
