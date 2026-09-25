//! headless CLI（ADR-0012）。与 app 共用 `core_ffi` 与 GraphDoc 校验，不依赖 Tauri。
//! stdout 是 JSON Lines（`run` 输出 ExecutionEvent 原样），stderr 给人看。

use std::collections::{BTreeMap, HashSet};
use std::ffi::CStr;
use std::io::Write;
use std::os::raw::{c_char, c_void};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, OnceLock};

use serde_json::{json, Value};

use crate::core_ffi::{self, Core, RunHandle, RunInput, RunSpec};
use crate::eval;
use crate::patch;
use crate::perturb;
use crate::recipe;
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

pub(crate) fn line(sink: &Sink, text: &str) {
    if let Ok(mut w) = sink.lock() {
        let _ = writeln!(w, "{text}");
        let _ = w.flush();
    }
}

pub(crate) fn json_line(sink: &Sink, value: &Value) {
    line(sink, &value.to_string());
}

const USAGE: &str = "\
lyflow —— LyFlow 的 headless 命令行（stdout 是 JSON Lines，stderr 给人看）

  lyflow run      <graph> [--to <nodeId>]... [--set <nodeId>.<param>=<json>]...
                          [--recipe <配方文件>] [--param <名字>=<json>]...
                          [--base-dir <dir>] [--parallel <n>] [--no-cache]
                          [--preview] [--preview-points <n>] [--outputs] [--summary]
                          [--input <nodeId>.<port>=<file.pcd>]...
        --input：把一片点云注入这个端口（与宿主经 C ABI 的 run inputs 同一条路）。端口是输出时
                 整节点注入、compute 不跑；只是输入时是输入注入，例如 --input n_scan.primary=a.pcd
                 --input n_scan.secondary=b.pcd 直接喂 gap.read_scan（m8-plan L18）。
        --summary：JSON Lines 末尾多一行 {\"kind\":\"run_summary\", ...}，
                   status 三态 ok|degraded|failed，每个图级输出三态 value|inactive|failed，
                   外加 decisions（全部 FallbackChoice）。ADR-0022。
  lyflow import   <file> --kind <kind> [--fine] [-o <out.lyflow.json>] [--base-dir <dir>]
        --fine：产出细粒度图（每一步一个节点）而不是默认的积木图（m8-plan L12），
        等价于 --kind <kind>:fine；导入器没注册那种 kind 时报错。
  lyflow validate <graph> [--base-dir <dir>] [--set ...] [--recipe <配方文件>] [--param ...]
  lyflow plan     <graph> [--to <nodeId>]... [--base-dir <dir>] [--set ...] [--recipe ...] [--param ...]
        每个节点一行：cacheKey、cached、level、upstreamMissing、bypass，
        外加 lazy（只被惰性端口依赖，主路径成功时不跑）与 demandedBy（谁的哪个惰性端口管着它）。
  lyflow params   <graph> [--node <id>]... [--only explicit|default|bound|graph]
                          [--set <nodeId>.<param>=<json>]... [--recipe <配方文件>] [--param <名字>=<json>]...
                          [--base-dir <dir>] [--json]
        每节点每参数一行 { node, op, param, value, source, graphParam?, unit?, min?, max? }。
        source 四种：default（图里没写）/ explicit（图里写了）/ bound（子图提升参数灌进来的）/
                     graph（顶层图参数灌进来的，graphParam 说是哪一个）。
        --set 先应用再解析，所以「这组 --set 之后生效值是什么」一条命令。
        未知节点 / 未知参数按 unknown_node / unknown_param 报，退出码 4。
  lyflow migrate  <graph> [--write]
  lyflow manifest [--check]
  lyflow dump     <graph> <nodeId>:<port> <out.pcd> [--format <binary|ascii|binary_compressed>]
  lyflow sweep    <graph> --param <nodeId>.<param>=<start>:<end>:<steps> [--param ...]
                          --metric <nodeId>:<port>.<elementCount|byteSize|durationMs>
                          [--csv <out.csv>] [--base-dir <dir>]
  lyflow eval     <graph> [<样本集>] [--params <paramsets.json>]
                          [--param <n>.<p>=<start>:<end>:<steps>]...
                          --metric <path> [--metric <path>]...
                          [--holdout <tag>=<value>] [--group-by <tag>]
                          [--csv <out.csv>] [--base-dir <dir>] [--parallel <n>] [--no-cache]
                          [--set <nodeId>.<param>=<json>]... [--recipe <配方文件>] [--param <名字>=<json>]...
                          [--summary]
        --recipe 作用于所有样本；--params 的参数组里不含「.」的键写顶层图参数。
        叠加顺序：基础（default）→ --recipe → 参数组 → --param。
        每行 eval_row **默认不带** summary（ADR-0022）；--summary 打开。
        一维 bundle 就 6 KB，51 帧 × 8 组参数 2.5 MB，那不该是默认值。
        （--no-summary 还认，但已经是 no-op。）
        指标路径：outputs.<名字>[.字段...] / nodes.<节点>.<端口>[.字段...]
                  nodes.<节点>.durationMs|elementCount|byteSize / run.durationMs
        样本集三选一：
          --samples <samples.jsonl>
          --samples-glob <pat> --bind <n>.<p>
          --samples-dir <root> --bind-pair <n>.<pA>,<n>.<pB> --pattern <globA>,<globB>
                        （单文件时 --bind <n>.<p> --pattern <glob>）
                        [--sample-subdir <name>] [--sort-by name|mtime] [--split-half <tagKey>]
        另有 [--samples-jsonl-out <path>]：把生成的样本集写出来，可核对可复用。
        --samples-dir 下每个直接子目录是一帧，样本 id 取帧目录名；--sample-subdir 再往下一层。
        --sort-by name（默认）先从帧目录名里读 dd-MM-yyyy-HH-mm-ss 时间戳排序，读不出退字典序；
        --split-half 排序后前一半打 a、后一半打 b（奇数时前半多一个），配 --holdout <tagKey>=b 用。
  lyflow perturb  <graph> --after <nodeId>:<port> --region <json> --axis <x|y|z>=<s>:<e>:<n>
                          [<样本集>，与 eval 同一组选项]
                          --metric <path> [--metric <path>]...
                          [--expect <slope>] [--tolerance <v>] [--csv <out.csv>]
                          [--base-dir <dir>] [--parallel <n>] [--no-cache] [--set ...]
        选区 JSON：{\"kind\":\"halfspace\",\"point\":[x,y,z],\"normal\":[x,y,z]}
                   {\"kind\":\"box\",\"min\":[x,y,z],\"max\":[x,y,z]}
        单位：--region 的 point / min / max 与 --axis 的位移都是「米」，与点云同帧同单位
              （传感器帧与测量帧都是米）；outputs.* 这类 Measurement 是「毫米」。
              所以「张开 1 mm 读数加 1 mm」是 --expect 1000，不是 1。
  lyflow diff     <a> <b> [--json]
  lyflow recipes  <graph> [--recipe <配方文件>]... [--json]
        图旁配方目录（<图名>.recipes/）里的配方：名字、值个数、四类失配与建议、默认配方。只读，退出码 0。
        给了 --recipe 就只看这几个文件，每个另带 params（合成好的图参数取值，给宿主 / MCP 用）。
  lyflow patch    <graph> [--remove-node <id|glob>]... [--add-node <json>]...
                          [--rewire <节点>:<端口>=<节点>:<端口>]...
                          [--set <nodeId>.<param>=<json>]... [--recipe <配方文件>] [--param <名字>=<json>]...
                          [--dry-run] [-o <out>] [--json] [--base-dir <dir>]
        动作顺序定死 remove → add → rewire → set → recipe（把配方的值写回基础）→ param（改顶层参数的 default）；
        每步之后过形状校验，最后过 validate，
        任一步不过就整体不写（退出码 1）。幂等：删不存在的 id、没有出边的 rewire、
        同值的 set 都是 no-op 并在 stderr 说一句，所以同一条命令跑两遍第二遍 diff 为空
        （这一遍不落盘，免得白白动 mtime；给了 -o 就照写）。
        --dry-run 不写文件，stdout 是与 `lyflow diff` 逐字相同的差异。
        -o 省略时原地覆写（先写临时文件再改名）。

顶层图参数（GraphDoc 顶层 params）：--param <名字>=<json> 给它传值，值的写法同 --set。
  eval / sweep 的 --param 另有扫描轴写法 <节点>.<参数>=<start>:<end>:<steps>：
  「=」左边含「.」的是扫描轴，不含的是顶层参数。图没声明的名字报 unknown_param；
  --set 命中被顶层参数绑定的参数报 param_conflict（一处定义，改用 --param）。两者退出码 4。

配方（docs/recipe.md）：--recipe <文件> 读一个 .lyflow-recipe.json，把它的值当作图参数的取值；
  与 --param 叠加时 --param 优先。失配 ①多出 ②类型不符 ③越界 时不跑、退出码 4，stderr 列出每一条与建议；
  ④规格变了 只在 stderr 提示，不改退出码。

退出码：0 成功，1 校验失败，2 执行失败，3 被取消（Ctrl+C），4 参数错。";

// ------------------------------------------------------------------ 参数解析

pub(crate) struct Parsed {
    pub positional: Vec<String>,
    pub values: BTreeMap<String, Vec<String>>,
    pub flags: HashSet<String>,
}

impl Parsed {
    pub(crate) fn one(&self, name: &str) -> Option<&str> {
        self.values.get(name).and_then(|v| v.last()).map(String::as_str)
    }
    pub(crate) fn many(&self, name: &str) -> &[String] {
        static EMPTY: &[String] = &[];
        self.values.get(name).map(Vec::as_slice).unwrap_or(EMPTY)
    }
    pub(crate) fn has(&self, name: &str) -> bool {
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

pub(crate) struct Loaded {
    pub doc: GraphDoc,
    pub json: String,
    pub base_dir: String,
    pub path: PathBuf,
}

/// 哪个顶层图参数绑着 `节点.参数`。没有返回 None。
pub(crate) fn binding_graph_param<'a>(doc: &'a GraphDoc, node: &str, param: &str) -> Option<&'a str> {
    let target = format!("{node}.{param}");
    doc.params.iter().find_map(|(name, decl)| {
        decl["binds"]
            .as_array()
            .filter(|binds| binds.iter().any(|b| b.as_str() == Some(target.as_str())))
            .map(|_| name.as_str())
    })
}

/// `--set` 命中被顶层参数绑定的参数：一处定义（J7），报错并指向 `--param`。
pub(crate) fn set_conflict(doc: &GraphDoc, node: &str, param: &str, spec: &str) -> Option<String> {
    binding_graph_param(doc, node, param).map(|name| {
        format!(
            "param_conflict: --set {spec} 命中的参数由顶层参数 '{name}' 绑定；\
             改用 --param {name}=<json>"
        )
    })
}

/// `--param` 里属于顶层图参数的那些：`=` 左边不含 `.`。含 `.` 的是 eval / sweep 的扫描轴。
pub(crate) fn graph_param_specs(parsed: &Parsed) -> Vec<&String> {
    parsed
        .many("param")
        .iter()
        .filter(|s| s.split_once('=').is_some_and(|(l, _)| !l.contains('.')))
        .collect()
}

/// `--param` 里的扫描轴（`<节点>.<参数>=<start>:<end>:<steps>`）。
pub(crate) fn axis_param_specs(parsed: &Parsed) -> Vec<String> {
    parsed
        .many("param")
        .iter()
        .filter(|s| !s.split_once('=').is_some_and(|(l, _)| !l.contains('.')))
        .cloned()
        .collect()
}

/// `--param <名字>=<json>`：把值写成该顶层参数的 default。core 的语义就是「给了值用值，
/// 没给用 default」，所以校验、计划、生效参数与运行看到的是同一个值。
pub(crate) fn apply_graph_param(doc: &mut GraphDoc, spec: &str) -> Result<bool, String> {
    let (name, raw) = spec
        .split_once('=')
        .ok_or_else(|| format!("--param 的写法是 <名字>=<json>，收到 {spec}"))?;
    let value: Value = serde_json::from_str(raw).unwrap_or_else(|_| Value::String(raw.to_string()));
    let Some(decl) = doc.params.get_mut(name).and_then(Value::as_object_mut) else {
        let known: Vec<&String> = doc.params.keys().collect();
        return Err(format!(
            "unknown_param: 图没有声明顶层参数 '{name}'（有的是 {known:?}）"
        ));
    };
    if decl.get("default") == Some(&value) {
        return Ok(false);
    }
    decl.insert("default".to_string(), value);
    Ok(true)
}

/// load_graph 失败时的退出码：`--param` / `--set` / `--recipe` 本身有问题是参数错（4），其余是图不合法（1）。
/// 配方的失配 ①–③ 也归 4（`recipe_mismatch:`）：图没错，是这份取值不能用。
pub(crate) fn load_exit(message: &str) -> i32 {
    const USAGE_CODES: &[&str] = &["unknown_param:", "param_conflict:", "recipe_mismatch:", "bad_recipe:"];
    if USAGE_CODES.iter().any(|c| message.starts_with(c)) {
        EXIT_USAGE
    } else {
        EXIT_INVALID
    }
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
    if let Some(conflict) = set_conflict(doc, node_id, param, spec) {
        return Err(conflict);
    }
    let value: Value = serde_json::from_str(raw).unwrap_or_else(|_| Value::String(raw.to_string()));
    let node = doc
        .nodes
        .iter_mut()
        .find(|n| n.id == node_id)
        .ok_or_else(|| format!("图里没有节点 {node_id}"))?;
    node.params.insert(param.to_string(), value);
    Ok(())
}

/// 读图并应用取值选项，顺序定死：基础（图里的 default）→ `--recipe` → `--param` → `--set`。
/// `--recipe` 与 `--param` 都落成图参数的 default，后写的赢，所以 `--param` 覆盖配方里的同名值
/// （param-recipe P4.1）。配方的失配 ①–③ 在这里就拦下（退出码 4），④ 只在 err 上提示。
pub(crate) fn load_graph(parsed: &Parsed, path: &str, err: &Sink) -> Result<Loaded, String> {
    let file = PathBuf::from(path);
    let text = std::fs::read_to_string(&file).map_err(|e| format!("读取 {path} 失败: {e}"))?;
    let mut doc: GraphDoc =
        serde_json::from_str(&text).map_err(|e| format!("{path} 不是合法的 GraphDoc: {e}"))?;
    doc.validate_structure().map_err(|e| e.to_string())?;
    recipe::apply_recipe_option(&mut doc, parsed, err)?;
    for spec in graph_param_specs(parsed) {
        apply_graph_param(&mut doc, spec)?;
    }
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

pub(crate) fn core() -> Result<Arc<Core>, String> {
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

pub(crate) struct RunResult {
    pub events: Vec<Value>,
    pub status: String,
    /// 句柄活着结果仓的索引才在（Drop 会 freeRun）。dump 要在这之后取输出。
    _handle: Arc<RunHandle>,
}

impl RunResult {
    pub(crate) fn run_id(&self) -> &str {
        self._handle.run_id()
    }

    /// 本次运行的 run summary（ADR-0022）。事件里那份与 `lyflow_run_summary`
    /// 拿到的是同一个对象，所以直接从 run_finished 上取 —— 不必再过一次 FFI。
    /// 老 core 没有这个字段时返回 None。
    pub(crate) fn summary(&self) -> Option<&Value> {
        self.events
            .iter()
            .rev()
            .find(|e| e["kind"] == "run_finished")
            .map(|e| &e["summary"])
            .filter(|s| s.is_object())
    }

    pub(crate) fn exit_code(&self) -> i32 {
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

    pub(crate) fn skipped_nodes(&self) -> Vec<String> {
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

    pub(crate) fn duration_ms(&self) -> f64 {
        self.events
            .iter()
            .rev()
            .find(|e| e["kind"] == "run_finished")
            .and_then(|e| e["durationMs"].as_f64())
            .unwrap_or(0.0)
    }
}

pub(crate) struct RunRequest<'a> {
    pub graph_json: &'a str,
    pub base_dir: &'a str,
    pub targets: &'a [String],
    pub parallel: i32,
    pub preview_points: u32,
    pub preview: bool,
    /// `--no-cache`：本次运行不吃缓存。不清进程级结果仓 —— 那会连累别的 run。
    pub no_cache: bool,
    pub stream: Option<Sink>,
    /// 顶层图参数的取值，经 C ABI 的 `params_json` 交给 core（宿主的那条路）。
    /// CLI 自己的 `--param` 走 load_graph 改 default，两条路给出同一个结果。
    pub params_json: Option<&'a str>,
    /// 运行时注入的点云（`--input`），经 C ABI 的 run inputs 交给 core（ADR-0017 / m8-plan L18）。
    pub inputs: &'a [RunInput],
}

/// 用 core 自己的 `io.load_pcd` 读一个点云文件：跑一个单节点的图，按 `max_points`（0 = 全量）
/// 取回。编辑器的 2D 拖框底图（模板云，Tauri 的 load_cloud_file）走它，PCD / PLY 都认；
/// 取回的视图没有 rgb 通道，所以 CLI 的 `--input` 读 PCD 时不用它（见 load_inputs）。
pub(crate) fn read_cloud_file(
    core: &Arc<Core>,
    path: &Path,
    max_points: u32,
) -> Result<core_ffi::CloudView, String> {
    if !path.is_file() {
        return Err(format!("文件不存在 {}", path.display()));
    }
    let graph = json!({
        "schemaVersion": 1,
        "id": ulid::new(),
        "name": "read_cloud_file",
        "nodes": [{ "id": "load", "op": "io.load_pcd",
                    "params": { "path": path.to_string_lossy() } }],
        "edges": [],
    })
    .to_string();
    let result = execute(
        core,
        RunRequest {
            graph_json: &graph,
            base_dir: "",
            targets: &[],
            parallel: 1,
            preview_points: 0,
            preview: false,
            no_cache: true,
            stream: None,
            params_json: None,
            inputs: &[],
        },
    )?;
    if result.status != "ok" {
        let why = result
            .events
            .iter()
            .find_map(|e| e["error"]["message"].as_str().map(str::to_owned))
            .unwrap_or_else(|| result.status.clone());
        return Err(format!("读不了 {}：{why}", path.display()));
    }
    // 取回的那一份是拷贝，RunResult 在这里释放掉结果仓的索引也不影响它
    core.output_cloud(result.run_id(), "load", "cloud", max_points)
        .map_err(|e| e.to_string())
}

/// `--input <节点>.<端口>=<file.pcd>`：CLI 这边的注入，与宿主经 C ABI 的 `lyflow_run_options.inputs`
/// 是同一条路。端口是输出时整节点注入，只是输入（例如 `n_scan.primary`）时是输入注入。
///
/// PCD 由 `crate::pcd` 原样读出（点序、NaN 槽、intensity、rgb 都在）：core 的取数视图没有 rgb，
/// 而 gap 的模型定位靠 rgb 的 R 当强度特征。别的格式借 core 的 `io.load_pcd`（见 read_cloud_file）。
fn load_inputs(core: &Arc<Core>, parsed: &Parsed) -> Result<Vec<RunInput>, String> {
    let mut out = Vec::new();
    for spec in parsed.many("input") {
        let bad = || format!("--input 的写法是 <节点>.<端口>=<file.pcd>，收到 {spec}");
        let (left, file) = spec.split_once('=').ok_or_else(bad)?;
        let (node, port) = left.rsplit_once('.').ok_or_else(bad)?;
        if node.is_empty() || port.is_empty() || file.is_empty() {
            return Err(bad());
        }
        let path = std::path::absolute(file).map_err(|e| format!("--input {spec}: {e}"))?;
        if !path.is_file() {
            return Err(format!("--input {spec}: 文件不存在 {}", path.display()));
        }
        let is_pcd = path
            .extension()
            .is_some_and(|e| e.to_string_lossy().eq_ignore_ascii_case("pcd"));
        let input = if is_pcd {
            // PCD 自己读：要带上 rgb（gap 的强度在 R 上），core 的取数视图没有这个通道
            let c = crate::pcd::read_pcd(&path).map_err(|e| format!("--input {spec}: {e}"))?;
            RunInput {
                node_id: node.to_string(),
                port: port.to_string(),
                xyz: c.xyz,
                intensity: c.intensity,
                normals: c.normals,
                rgb: c.rgb,
            }
        } else {
            // 其余格式（PLY）借 core 的 io.load_pcd：没有 rgb 通道
            let view = read_cloud_file(core, &path, 0).map_err(|e| format!("--input {spec}: {e}"))?;
            RunInput {
                node_id: node.to_string(),
                port: port.to_string(),
                xyz: view.xyz().to_vec(),
                intensity: if view.has_intensity() { view.intensity().to_vec() } else { Vec::new() },
                normals: if view.has_normals() { view.normals().to_vec() } else { Vec::new() },
                rgb: Vec::new(),
            }
        };
        out.push(input);
    }
    Ok(out)
}

pub(crate) fn execute(core: &Arc<Core>, req: RunRequest<'_>) -> Result<RunResult, String> {
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
    spec.params_json = req.params_json;
    spec.inputs = req.inputs;
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

pub(crate) fn diagnostics_of(core: &Arc<Core>, loaded: &Loaded) -> Result<Vec<Value>, String> {
    let raw = core
        .validate(&loaded.json, &loaded.base_dir)
        .map_err(|e| e.to_string())?;
    serde_json::from_str(&raw).map_err(|e| format!("core 返回的诊断不是合法 JSON: {e}"))
}

pub(crate) fn has_errors(diags: &[Value]) -> bool {
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
    let loaded = match load_graph(parsed, &path, err) {
        Ok(l) => l,
        Err(e) => return fail(err, &e, load_exit(&e)),
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
    let loaded = match load_graph(parsed, &path, err) {
        Ok(l) => l,
        Err(e) => return fail(err, &e, load_exit(&e)),
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

// --------------------------------------------------------------------- params

const PARAM_SOURCES: &[&str] = &["explicit", "default", "bound", "graph"];

/// 一行的宽度按**字符数**算。中文在等宽终端里占两格，但按字符数对齐已经够看，
/// 而按显示宽度对齐要背一张 East Asian Width 表 —— 那不是这个命令该扛的复杂度。
fn pad(s: &str, width: usize) -> String {
    let n = s.chars().count();
    if n >= width {
        return s.to_string();
    }
    format!("{s}{}", " ".repeat(width - n))
}

/// `lyflow params <graph>` —— 每节点每参数的生效值与来源（m6-plan §2 / H6）。
///
/// 稀疏存储是对的（GraphDoc 只存改过的键），但「这组 `--set` 之后到底跑的是什么值」
/// 要有一条命令能问，否则每次都得拿 manifest 的默认值去和图 join 一遍。
/// **生效值一律由 core 给**（`lyflow_effective_params`）：在这里重算一遍默认值合并、
/// 类型规整与参数迁移，迟早会与执行器真正用的那份漂开，而漂开的表现是
/// 「参数明明改了，结果没变」—— 最难查的一类。
fn cmd_params(parsed: &Parsed, out: &Sink, err: &Sink) -> i32 {
    let Some(path) = parsed.positional.first().cloned() else {
        line(
            err,
            "用法：lyflow params <graph> [--node <id>]... [--only explicit|default|bound|graph] \
             [--set <节点>.<参数>=<json>]... [--param <名字>=<json>]... [--base-dir <dir>] [--json]",
        );
        return EXIT_USAGE;
    };
    let only = parsed.one("only").map(str::to_string);
    if let Some(o) = &only {
        if !PARAM_SOURCES.contains(&o.as_str()) {
            return fail(
                err,
                &format!("--only 只认 {}，收到 {o}", PARAM_SOURCES.join(" / ")),
                EXIT_USAGE,
            );
        }
    }
    let core = match core() {
        Ok(c) => c,
        Err(e) => return fail(err, &e, EXIT_FAILED),
    };

    // 先不带 --set 读一遍：`--set` 指到不存在的节点上时要报 `unknown_node` + 退出码 4，
    // 而 load_graph 那条路会把它算成校验失败（退出码 1）。两者不是一回事。
    let mut bare = Parsed {
        positional: Vec::new(),
        values: BTreeMap::new(),
        flags: HashSet::new(),
    };
    if let Some(dir) = parsed.one("base-dir") {
        bare.values.insert("base-dir".to_string(), vec![dir.to_string()]);
    }
    let probe = match load_graph(&bare, &path, err) {
        Ok(l) => l,
        Err(e) => return fail(err, &e, EXIT_INVALID),
    };
    for spec in parsed.many("set") {
        let Some(node_id) = spec.split_once('=').and_then(|(l, _)| l.rsplit_once('.')).map(|(n, _)| n)
        else {
            return fail(
                err,
                &format!("--set 的写法是 <节点>.<参数>=<json>，收到 {spec}"),
                EXIT_USAGE,
            );
        };
        if !probe.doc.nodes.iter().any(|n| n.id == node_id) {
            return fail(
                err,
                &format!("unknown_node: --set {spec} 指向图里没有的节点 {node_id}"),
                EXIT_USAGE,
            );
        }
    }

    let loaded = match load_graph(parsed, &path, err) {
        Ok(l) => l,
        Err(e) => return fail(err, &e, load_exit(&e)),
    };
    let raw = match core.effective_params(&loaded.json, &loaded.base_dir) {
        Ok(r) => r,
        Err(e) => return fail(err, &e.to_string(), EXIT_FAILED),
    };
    // 校验没过时 core 返回的是诊断**数组**，与 plan 同一套区分办法（'[' vs '{'）。
    if raw.trim_start().starts_with('[') {
        let diags: Vec<Value> = serde_json::from_str(&raw).unwrap_or_default();
        json_line(out, &Value::Array(diags.clone()));
        let usage = diags
            .iter()
            .any(|d| d["code"] == "unknown_node" || d["code"] == "unknown_param");
        for d in &diags {
            if d["severity"] != "error" {
                continue;
            }
            line(
                err,
                &format!(
                    "{}: {} {}",
                    d["code"].as_str().unwrap_or("error"),
                    d["nodeId"].as_str().unwrap_or(""),
                    d["message"].as_str().unwrap_or("")
                ),
            );
        }
        return if usage { EXIT_USAGE } else { EXIT_INVALID };
    }
    let view: Value = match serde_json::from_str(&raw) {
        Ok(v) => v,
        Err(e) => return fail(err, &format!("core 返回的参数视图不是合法 JSON: {e}"), EXIT_FAILED),
    };

    let wanted: Vec<String> = parsed.many("node").to_vec();
    let mut hit: HashSet<String> = HashSet::new();
    let mut rows: Vec<Value> = Vec::new();
    for node in view["nodes"].as_array().cloned().unwrap_or_default() {
        let node_id = node["node"].as_str().unwrap_or_default().to_string();
        // --node 精确匹配；子图展开后的内部节点也认「父/子」的整段前缀（与 --to 一致）。
        if !wanted.is_empty() {
            let Some(matched) = wanted
                .iter()
                .find(|t| node_id == **t || node_id.starts_with(&format!("{t}/")))
            else {
                continue;
            };
            hit.insert(matched.clone());
        }
        let op = node["op"].as_str().unwrap_or_default().to_string();
        for p in node["params"].as_array().cloned().unwrap_or_default() {
            let source = p["source"].as_str().unwrap_or_default();
            if let Some(o) = &only {
                if source != o {
                    continue;
                }
            }
            let mut row = serde_json::Map::new();
            row.insert("node".into(), json!(node_id));
            row.insert("op".into(), json!(op));
            row.insert("param".into(), p["param"].clone());
            row.insert("value".into(), p["value"].clone());
            row.insert("source".into(), json!(source));
            for key in ["graphParam", "unit", "min", "max"] {
                if !p[key].is_null() {
                    row.insert(key.into(), p[key].clone());
                }
            }
            rows.push(Value::Object(row));
        }
    }
    let missing: Vec<&String> = wanted.iter().filter(|t| !hit.contains(*t)).collect();
    if !missing.is_empty() {
        for t in &missing {
            line(err, &format!("unknown_node: 图里没有节点 {t}"));
        }
        return EXIT_USAGE;
    }

    if parsed.has("json") {
        for row in &rows {
            json_line(out, row);
        }
    } else {
        let width = |key: &str, head: &str| {
            rows.iter()
                .map(|r| r[key].as_str().unwrap_or_default().chars().count())
                .chain(std::iter::once(head.chars().count()))
                .max()
                .unwrap_or(0)
        };
        let (wn, wo, wp) = (width("node", "节点"), width("op", "算子"), width("param", "参数"));
        line(
            out,
            &format!(
                "{}  {}  {}  {}  {}",
                pad("节点", wn),
                pad("算子", wo),
                pad("参数", wp),
                pad("来源", 8),
                "值"
            ),
        );
        for r in &rows {
            let unit = r["unit"].as_str().unwrap_or_default();
            line(
                out,
                &format!(
                    "{}  {}  {}  {}  {}{}",
                    pad(r["node"].as_str().unwrap_or_default(), wn),
                    pad(r["op"].as_str().unwrap_or_default(), wo),
                    pad(r["param"].as_str().unwrap_or_default(), wp),
                    pad(r["source"].as_str().unwrap_or_default(), 8),
                    r["value"],
                    if unit.is_empty() { String::new() } else { format!(" {unit}") },
                ),
            );
        }
    }
    let explicit = rows.iter().filter(|r| r["source"] == "explicit").count();
    let bound = rows.iter().filter(|r| r["source"] == "bound").count();
    let graph = rows.iter().filter(|r| r["source"] == "graph").count();
    line(
        err,
        &format!(
            "{} 个参数：{explicit} 个显式、{bound} 个来自子图提升、{graph} 个来自顶层参数、{} 个默认值",
            rows.len(),
            rows.len() - explicit - bound - graph
        ),
    );
    EXIT_OK
}

fn cmd_migrate(parsed: &Parsed, out: &Sink, err: &Sink) -> i32 {
    let Some(path) = parsed.positional.first().cloned() else {
        line(err, "用法：lyflow migrate <graph> [--write]");
        return EXIT_USAGE;
    };
    // migrate --write 写回的是读进来的整份 doc：带着配方读，就会把配方的值烙成基础
    if !parsed.many("recipe").is_empty() {
        return fail(err, "migrate 不认 --recipe：迁移改的是图本身，与配方无关", EXIT_USAGE);
    }
    let core = match core() {
        Ok(c) => c,
        Err(e) => return fail(err, &e, EXIT_FAILED),
    };
    let mut loaded = match load_graph(parsed, &path, err) {
        Ok(l) => l,
        Err(e) => return fail(err, &e, load_exit(&e)),
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
        // 与前端 applyMigrations 同一套语义：params 是完整对象而不是补丁（ADR-0008），
        // edits 里的连线改动一并写回（ADR-0025）
        for m in &migrations {
            loaded.doc.apply_migration(m);
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
    for m in &migrations {
        for note in m["notes"].as_array().into_iter().flatten() {
            if let Some(note) = note.as_str() {
                line(err, &format!("  {}：{note}", m["nodeId"].as_str().unwrap_or("?")));
            }
        }
    }
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
    let loaded = match load_graph(parsed, &path, err) {
        Ok(l) => l,
        Err(e) => return fail(err, &e, load_exit(&e)),
    };
    let inputs = match load_inputs(&core, parsed) {
        Ok(i) => i,
        Err(e) => return fail(err, &e, EXIT_USAGE),
    };
    // 先单独校验一遍：校验失败与执行失败是两个不同的退出码，混在一次 run 里分不开。
    // lyflow_validate 看不见注入：被 --input 喂了的必填输入不算 missing_input（执行期
    // 的校验知道注入，会把它当成已接）。
    match diagnostics_of(&core, &loaded) {
        Ok(diags) => {
            let fed = |d: &Value| {
                d["code"] == "missing_input"
                    && inputs.iter().any(|i| d["nodeId"] == i.node_id.as_str() && d["portName"] == i.port.as_str())
            };
            let remaining: Vec<Value> = diags.into_iter().filter(|d| !fed(d)).collect();
            if has_errors(&remaining) {
                for d in &remaining {
                    json_line(out, d);
                }
                line(err, "校验失败，没有执行");
                return EXIT_INVALID;
            }
        }
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
            params_json: None,
            inputs: &inputs,
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
    // --summary：JSON Lines 末尾多一行（ADR-0022）。放在 --outputs 之后，
    // 「最后一行就是这一轮的结论」对读脚本最省事。
    if parsed.has("summary") {
        match result.summary() {
            Some(s) => {
                let mut wrapped = s.clone();
                if let Some(obj) = wrapped.as_object_mut() {
                    obj.insert("kind".to_string(), json!("run_summary"));
                }
                json_line(out, &wrapped);
            }
            None => {
                return fail(err, "这份 core 不产出 run summary（ABI < v9）", EXIT_FAILED);
            }
        }
    }
    line(
        err,
        &format!(
            "run {} in {:.0} ms{}",
            result.status,
            result.duration_ms(),
            result
                .summary()
                .and_then(|s| s["status"].as_str())
                .map(|s| format!("（summary {s}）"))
                .unwrap_or_default()
        ),
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
    // --fine 是「同一种格式的细粒度那一版」：导入器把它注册成 `<kind>:fine`（m8-plan L12）。
    let fine_kind;
    let kind = if parsed.has("fine") && !kind.ends_with(":fine") {
        fine_kind = format!("{kind}:fine");
        fine_kind.as_str()
    } else {
        kind
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
    let loaded = match load_graph(parsed, &path, err) {
        Ok(l) => l,
        Err(e) => return fail(err, &e, load_exit(&e)),
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
            params_json: None,
            inputs: &[],
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
pub(crate) struct SweepAxis {
    pub node: String,
    pub param: String,
    pub values: Vec<f64>,
    /// vecNf 参数的分量数。0 = 标量。扫 leafSize 时一个数广播到三个分量。
    pub components: usize,
}

pub(crate) fn parse_axis(spec: &str) -> Result<SweepAxis, String> {
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
pub(crate) fn component_count(
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
    if axis_param_specs(parsed).is_empty() {
        line(err, "至少给一个 --param nodeId.param=start:end:steps");
        return EXIT_USAGE;
    }
    let Some(metric_spec) = parsed.one("metric").map(str::to_string) else {
        line(err, "缺 --metric nodeId:port.field");
        return EXIT_USAGE;
    };
    let metric = match eval::parse_metric(&metric_spec) {
        Ok(m) => m,
        Err(e) => return fail(err, &e, EXIT_USAGE),
    };

    let core = match core() {
        Ok(c) => c,
        Err(e) => return fail(err, &e, EXIT_FAILED),
    };
    let loaded = match load_graph(parsed, &path, err) {
        Ok(l) => l,
        Err(e) => return fail(err, &e, load_exit(&e)),
    };
    let defaults = match defaults_by_op(&core) {
        Ok(d) => d,
        Err(e) => return fail(err, &e, EXIT_FAILED),
    };
    let param_sets = match eval::axis_param_sets(&loaded.doc, &defaults, &axis_param_specs(parsed)) {
        Ok(v) => v,
        Err(e) => return fail(err, &e, EXIT_USAGE),
    };

    let metrics = [metric];
    let samples = [eval::Sample::whole_graph()];
    let engine = eval::Engine {
        core: &core,
        base: &loaded,
        pinned: &[],
        metrics: &metrics,
        param_sets: &param_sets,
        samples: &samples,
        parallel: 0,
        no_cache: false,
        // sweep 的每一行只报一个标量，summary 在这里是纯体积
        summary: false,
    };

    let mut csv_rows: Vec<String> = Vec::new();
    let keys: Vec<String> = param_sets
        .first()
        .map(|ps| ps.display.keys().cloned().collect())
        .unwrap_or_default();
    let worst = {
        let mut on_row = |row: &eval::Row| {
            let combo = param_sets[row.param_set].display.clone();
            let value = row.metrics.first().copied().flatten();
            json_line(
                out,
                &json!({
                    "kind": "sweep_row",
                    "index": row.param_set,
                    "params": Value::Object(combo.clone()),
                    "metric": metric_spec,
                    "value": value,
                    "status": row.status,
                    "durationMs": row.duration_ms,
                    "skipped": row.skipped,
                }),
            );
            let mut cells: Vec<String> = keys
                .iter()
                .map(|k| combo.get(k).map(|v| v.to_string()).unwrap_or_default())
                .collect();
            cells.push(value.map(|v| v.to_string()).unwrap_or_default());
            csv_rows.push(cells.join(","));
        };
        match engine.run(&mut on_row) {
            Ok(c) => c,
            Err(eval::EngineError::Failed(e)) => return fail(err, &e, EXIT_FAILED),
            Err(eval::EngineError::Usage(message, available)) => {
                line(err, &message);
                for p in available.iter().take(200) {
                    line(err, &format!("  {p}"));
                }
                return EXIT_USAGE;
            }
        }
    };

    if let Some(csv_path) = parsed.one("csv") {
        let mut text = String::new();
        for k in &keys {
            text.push_str(k);
            text.push(',');
        }
        text.push_str(&metric_spec);
        text.push('\n');
        for row in &csv_rows {
            text.push_str(row);
            text.push('\n');
        }
        if let Err(e) = std::fs::write(csv_path, text) {
            return fail(err, &format!("写入 {csv_path} 失败: {e}"), EXIT_FAILED);
        }
        line(err, &format!("表格写到 {csv_path}"));
    }
    line(err, &format!("扫了 {} 组", param_sets.len()));
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

pub(crate) fn defaults_by_op(core: &Arc<Core>) -> Result<BTreeMap<String, BTreeMap<String, Value>>, String> {
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
    let a = match load_graph(&empty, &parsed.positional[0], err) {
        Ok(l) => l,
        Err(e) => return fail(err, &e, EXIT_INVALID),
    };
    let b = match load_graph(&empty, &parsed.positional[1], err) {
        Ok(l) => l,
        Err(e) => return fail(err, &e, EXIT_INVALID),
    };

    let diff = diff_docs(&defaults, &a.doc, &b.doc);
    if parsed.has("json") {
        json_line(out, &diff);
        return EXIT_OK;
    }
    if diff["empty"] == true {
        line(err, "两份图在语义上完全一样（ui 不算）");
        return EXIT_OK;
    }
    render_diff(out, &diff);
    EXIT_OK
}

/// 两份图的结构差异。`patch --dry-run` 用的是同一份实现 —— 「差异长什么样」只该有一处定义。
pub(crate) fn diff_docs(
    defaults: &BTreeMap<String, BTreeMap<String, Value>>,
    a: &GraphDoc,
    b: &GraphDoc,
) -> Value {
    let mut added = Vec::new();
    let mut removed = Vec::new();
    let mut changed = Vec::new();
    let no_defaults = BTreeMap::new();

    for node in &b.nodes {
        match a.nodes.iter().find(|n| n.id == node.id) {
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
    for node in &a.nodes {
        if !b.nodes.iter().any(|n| n.id == node.id) {
            removed.push(json!({ "id": node.id, "op": node.op }));
        }
    }

    let keys_a: HashSet<String> = a.edges.iter().map(edge_key).collect();
    let keys_b: HashSet<String> = b.edges.iter().map(edge_key).collect();
    let mut edges_added: Vec<String> = keys_b.difference(&keys_a).cloned().collect();
    let mut edges_removed: Vec<String> = keys_a.difference(&keys_b).cloned().collect();
    edges_added.sort();
    edges_removed.sort();

    let mut subgraphs = Vec::new();
    for (id, def) in &b.subgraphs {
        match a.subgraphs.get(id) {
            None => subgraphs.push(json!({ "id": id, "change": "added" })),
            Some(old) if old != def => subgraphs.push(json!({ "id": id, "change": "modified" })),
            _ => {}
        }
    }
    for id in a.subgraphs.keys() {
        if !b.subgraphs.contains_key(id) {
            subgraphs.push(json!({ "id": id, "change": "removed" }));
        }
    }

    // 顶层图参数：名字对名字比整份声明（default 与 binds 都算语义）。
    let mut graph_params = Vec::new();
    for (name, decl) in &b.params {
        match a.params.get(name) {
            None => graph_params.push(json!({ "name": name, "change": "added" })),
            Some(old) if old != decl => graph_params.push(json!({
                "name": name,
                "change": "modified",
                "from": old.get("default"),
                "to": decl.get("default"),
            })),
            _ => {}
        }
    }
    for name in a.params.keys() {
        if !b.params.contains_key(name) {
            graph_params.push(json!({ "name": name, "change": "removed" }));
        }
    }

    let empty_diff = added.is_empty()
        && removed.is_empty()
        && changed.is_empty()
        && edges_added.is_empty()
        && edges_removed.is_empty()
        && subgraphs.is_empty()
        && graph_params.is_empty();

    json!({
        "nodesAdded": added,
        "nodesRemoved": removed,
        "nodesChanged": changed,
        "edgesAdded": edges_added,
        "edgesRemoved": edges_removed,
        "subgraphs": subgraphs,
        "graphParams": graph_params,
        "empty": empty_diff,
    })
}

/// 人读的那一份。`diff` 与 `patch --dry-run` 的 stdout 逐字相同。
pub(crate) fn render_diff(out: &Sink, diff: &Value) {
    let list = |key: &str| diff[key].as_array().cloned().unwrap_or_default();
    let added = list("nodesAdded");
    let removed = list("nodesRemoved");
    let changed = list("nodesChanged");
    let edges_added = list("edgesAdded");
    let edges_removed = list("edgesRemoved");
    let subgraphs = list("subgraphs");
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
        line(out, &format!("+ 边 {}", e.as_str().unwrap_or_default()));
    }
    for e in &edges_removed {
        line(out, &format!("- 边 {}", e.as_str().unwrap_or_default()));
    }
    for s in &subgraphs {
        line(out, &format!("~ 子图 {} {}", s["id"], s["change"]));
    }
    for p in &list("graphParams") {
        if p["change"] == "modified" {
            line(out, &format!("~ 顶层参数 {}: {} -> {}", p["name"], p["from"], p["to"]));
        } else {
            line(out, &format!("~ 顶层参数 {} {}", p["name"], p["change"]));
        }
    }
}

// ---------------------------------------------------------------------- 入口

pub(crate) fn fail(err: &Sink, message: &str, code: i32) -> i32 {
    line(err, message);
    code
}

const VALUE_OPTS: &[&str] = &[
    "to", "set", "base-dir", "parallel", "preview-points", "param", "metric", "csv", "format",
    "kind", "output", "samples", "samples-glob", "bind", "params", "holdout", "group-by",
    "after", "region", "axis", "expect", "tolerance", "samples-dir", "bind-pair", "pattern",
    "sample-subdir", "sort-by", "split-half", "samples-jsonl-out",
    "remove-node", "add-node", "rewire", "node", "only", "input", "recipe",
];
const BOOL_OPTS: &[&str] = &[
    "no-cache", "preview", "write", "check", "json", "help", "outputs", "dry-run",
    "summary", "no-summary", "fine",
];

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
        "params" => cmd_params(&parsed, out, err),
        "migrate" => cmd_migrate(&parsed, out, err),
        "manifest" => cmd_manifest(&parsed, out, err),
        "import" => cmd_import(&parsed, out, err),
        "dump" => cmd_dump(&parsed, out, err),
        "sweep" => cmd_sweep(&parsed, out, err),
        "eval" => eval::cmd_eval(&parsed, out, err),
        "perturb" => perturb::cmd_perturb(&parsed, out, err),
        "diff" => cmd_diff(&parsed, out, err),
        "patch" => patch::cmd_patch(&parsed, out, err),
        "recipes" => recipe::cmd_recipes(&parsed, out, err),
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
        let owned: Vec<String> = args.iter().map(|s| (*s).to_string()).collect();
        cli_owned(&owned)
    }

    fn cli_owned(owned: &[String]) -> Ran {
        let obuf = Arc::new(Mutex::new(Vec::new()));
        let ebuf = Arc::new(Mutex::new(Vec::new()));
        let out = sink_of(SharedBuf(Arc::clone(&obuf)));
        let err = sink_of(SharedBuf(Arc::clone(&ebuf)));
        let code = run_cli(owned, &out, &err);
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

    /// ASCII PCD，y 恒为 row。只有 x y z 三个字段。
    fn write_pcd(file: &Path, n: usize, row: f32) {
        let mut text = format!(
            "# .PCD v0.7\nVERSION 0.7\nFIELDS x y z\nSIZE 4 4 4\nTYPE F F F\nCOUNT 1 1 1\n\
             WIDTH {n}\nHEIGHT 1\nVIEWPOINT 0 0 0 1 0 0 0\nPOINTS {n}\nDATA ascii\n"
        );
        for i in 0..n {
            text.push_str(&format!("{} {row} 0\n", i as f32 * 0.01));
        }
        std::fs::write(file, text).unwrap();
    }

    /// `--input` 自己读 PCD（为了带上 rgb）：三种 DATA 格式都要与 core 的 io.load_pcd 读出同一批点。
    #[test]
    fn pcd_reader_agrees_with_io_load_pcd_on_all_three_formats() {
        let dir = workspace("pcdformats");
        let core = core().unwrap();
        for format in ["ascii", "binary", "binary_compressed"] {
            let file = dir.join(format!("c_{format}.pcd"));
            let doc = json!({
                "schemaVersion": 1, "id": "01J8XQZ4K7N3M2R5V8W1YB6TCF", "name": "save",
                "nodes": [
                    {"id": "g", "op": "gen.synthetic", "params": {"pointCount": 777, "seed": 11}},
                    {"id": "s", "op": "io.save_pcd",
                     "params": {"path": file.to_string_lossy(), "format": format}}
                ],
                "edges": [{"id": "e", "from": {"node": "g", "port": "cloud"}, "to": {"node": "s", "port": "cloud"}}]
            });
            let graph = dir.join(format!("save_{format}.lyflow.json"));
            std::fs::write(&graph, doc.to_string()).unwrap();
            let r = cli(&["run", &graph.to_string_lossy(), "--no-cache"]);
            assert_eq!(r.code, EXIT_OK, "{format}: {}", r.err);

            let ours = crate::pcd::read_pcd(&file).unwrap();
            let view = read_cloud_file(&core, &file, 0).unwrap();
            assert_eq!(ours.xyz.len(), 777 * 3, "{format}");
            assert_eq!(ours.xyz.as_slice(), view.xyz(), "{format}: xyz");
            if view.has_intensity() {
                assert_eq!(ours.intensity.as_slice(), view.intensity(), "{format}: intensity");
            }
        }
    }

    #[test]
    fn input_injects_a_cloud_into_an_input_port() {
        let dir = workspace("input");
        let a = dir.join("a.pcd");
        let b = dir.join("b.pcd");
        write_pcd(&a, 3, 0.0);
        write_pcd(&b, 2, 1.0);
        let doc = json!({
            "schemaVersion": 1,
            "id": "01J8XQZ4K7N3M2R5V8W1YB6TCE",
            "name": "input",
            "nodes": [{"id": "m", "op": "util.merge"}],
            "edges": [],
            "outputs": {"merged": {"node": "m", "port": "cloud"}}
        });
        let file = dir.join("m.lyflow.json");
        std::fs::write(&file, doc.to_string()).unwrap();
        let graph = file.to_string_lossy().into_owned();

        // 没有 --input：两个必填输入都没接，校验期就拦下
        assert_eq!(cli(&["run", &graph, "--no-cache"]).code, EXIT_INVALID);

        let fa = format!("m.a={}", a.to_string_lossy());
        let fb = format!("m.b={}", b.to_string_lossy());
        let r = cli(&["run", &graph, "--input", &fa, "--input", &fb, "--outputs", "--no-cache"]);
        assert_eq!(r.code, EXIT_OK, "{}\n{}", r.err, r.out);
        let lines = r.lines();
        let done = lines
            .iter()
            .find(|e| e["kind"] == "node_state" && e["nodeId"] == "m" && e["state"] == "done")
            .expect("m 没有 done");
        // 输入注入：compute 真的跑了（不是 provided）
        assert!(done["stats"].get("provided").is_none(), "{done}");
        let outputs = lines.iter().find(|e| e.get("merged").is_some()).expect("没有 --outputs 那一行");
        assert_eq!(outputs["merged"]["elementCount"], 5);

        // 端口名写错：执行期校验报 unknown_port，退出码 2；写法不对是参数错
        let wrong = format!("m.nope={}", a.to_string_lossy());
        let r = cli(&["run", &graph, "--input", &fa, "--input", &fb, "--input", &wrong, "--no-cache"]);
        assert_eq!(r.code, EXIT_FAILED, "{}", r.out);
        assert!(r.out.contains("unknown_port"), "{}", r.out);
        assert_eq!(cli(&["run", &graph, "--input", "m.a"]).code, EXIT_USAGE);
        let missing = format!("m.a={}", dir.join("nope.pcd").to_string_lossy());
        assert_eq!(cli(&["run", &graph, "--input", &missing]).code, EXIT_USAGE);
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

    /// `plan` 的惰性标记（m6-plan §5 / H9）。这张链上一个惰性端口都没有，
    /// 所以 lazy 全是 false、demandedBy 全空 —— 「默认什么都不标」是它该有的样子。
    #[test]
    fn plan_marks_lazy_nodes_and_who_demands_them() {
        let dir = workspace("plan-lazy");
        let graph = chain(&dir, 312);
        let r = cli(&["plan", &graph]);
        assert_eq!(r.code, EXIT_OK, "{}", r.err);
        for n in r.first().as_array().unwrap() {
            assert_eq!(n["lazy"], false, "{n}");
            assert_eq!(n["demandedBy"], json!([]), "{n}");
        }
    }

    /// `lyflow params`（m6-plan §2 / H6）：生效值来自 core，来源分得开
    /// 「图里写了」与「合进来的默认值」。
    #[test]
    fn params_joins_defaults_and_marks_the_source() {
        let dir = workspace("params");
        let graph = chain(&dir, 313);
        let r = cli(&["params", &graph, "--json"]);
        assert_eq!(r.code, EXIT_OK, "{}", r.err);
        let rows = r.lines();
        assert!(!rows.is_empty(), "{}", r.out);

        let find = |node: &str, param: &str| {
            rows.iter()
                .find(|x| x["node"] == node && x["param"] == param)
                .unwrap_or_else(|| panic!("没有 {node}.{param}：{}", r.out))
                .clone()
        };
        // 图里写了的那三个
        let seed = find("g", "seed");
        assert_eq!(seed["source"], "explicit");
        assert_eq!(seed["value"], json!(313));
        assert_eq!(find("v", "leafSize")["source"], "explicit");
        assert_eq!(find("v", "leafSize")["value"], json!([0.02, 0.02, 0.02]));
        // 图里没写的：值必须与 manifest 的默认值逐字相同
        let defaults = defaults_by_op(&core().unwrap()).unwrap();
        for (node, op) in [("g", "gen.synthetic"), ("v", "filter.voxel_grid")] {
            for (name, def) in &defaults[op] {
                let row = find(node, name);
                if row["source"] == "default" {
                    assert_eq!(&row["value"], def, "{node}.{name} 的默认值对不上");
                }
            }
        }
        assert!(rows.iter().any(|x| x["source"] == "default"), "{}", r.out);
        assert!(rows.iter().all(|x| x["source"] != "bound"), "这张图没有子图");

        // --set 先应用再解析：问的是「这组 --set 之后生效值是什么」
        let after = cli(&["params", &graph, "--json", "--set", "g.seed=999"]);
        assert_eq!(after.code, EXIT_OK, "{}", after.err);
        let seed = after
            .lines()
            .into_iter()
            .find(|x| x["node"] == "g" && x["param"] == "seed")
            .unwrap();
        assert_eq!(seed["value"], json!(999));
        assert_eq!(seed["source"], "explicit");
    }

    #[test]
    fn params_filters_by_node_and_by_source() {
        let dir = workspace("params-filter");
        let graph = chain(&dir, 314);
        let one = cli(&["params", &graph, "--json", "--node", "v"]);
        assert_eq!(one.code, EXIT_OK, "{}", one.err);
        assert!(one.lines().iter().all(|x| x["node"] == "v"), "{}", one.out);

        let explicit = cli(&["params", &graph, "--json", "--only", "explicit"]);
        assert_eq!(explicit.code, EXIT_OK, "{}", explicit.err);
        let names: Vec<String> = explicit
            .lines()
            .iter()
            .map(|x| format!("{}.{}", x["node"].as_str().unwrap(), x["param"].as_str().unwrap()))
            .collect();
        assert_eq!(names, vec!["g.pointCount", "g.seed", "v.leafSize"]);

        // 人读的那一份：表头 + 每个参数一行
        let human = cli(&["params", &graph, "--node", "v", "--only", "explicit"]);
        assert_eq!(human.code, EXIT_OK, "{}", human.err);
        assert!(human.out.contains("leafSize"), "{}", human.out);
        assert!(human.out.contains("explicit"), "{}", human.out);
    }

    /// 未知节点 / 未知参数按 unknown_node / unknown_param 报，退出码 4（用法错），
    /// 而不是与「图本身不合法」混成同一个 1。
    #[test]
    fn params_rejects_unknown_nodes_and_params_with_the_usage_code() {
        let dir = workspace("params-unknown");
        let graph = chain(&dir, 315);

        let node = cli(&["params", &graph, "--node", "nope"]);
        assert_eq!(node.code, EXIT_USAGE, "{}", node.err);
        assert!(node.err.contains("unknown_node"), "{}", node.err);

        let set_node = cli(&["params", &graph, "--set", "nope.seed=1"]);
        assert_eq!(set_node.code, EXIT_USAGE, "{}", set_node.err);
        assert!(set_node.err.contains("unknown_node"), "{}", set_node.err);

        let param = cli(&["params", &graph, "--set", "g.nope=1"]);
        assert_eq!(param.code, EXIT_USAGE, "{}", param.err);
        assert_eq!(param.first()[0]["code"], "unknown_param");

        assert_eq!(cli(&["params", &graph, "--only", "nope"]).code, EXIT_USAGE);
        assert_eq!(cli(&["params"]).code, EXIT_USAGE);
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

    /// ADR-0022：带 fallback 的图里，主路径炸了但结果量出来了 —— run_finished 是
    /// ok，summary 必须说 degraded，并把那次回退记进 decisions。
    fn fallback_graph(dir: &Path, seed: i64, outputs: Value) -> String {
        let doc = json!({
            "schemaVersion": 1, "id": "01J8XQZ4K7N3M2R5V8W1YB6TCF",
            "nodes": [
                {"id": "n_bad", "op": "io.load_pcd", "params": {"path": "没有这个文件.pcd"}},
                {"id": "n_b", "op": "gen.synthetic",
                 "params": {"pointCount": 100, "seed": seed}},
                {"id": "n_fb", "op": "flow.fallback"}
            ],
            "edges": [
                {"id": "e1", "from": {"node": "n_bad", "port": "cloud"},
                             "to": {"node": "n_fb", "port": "a"}},
                {"id": "e2", "from": {"node": "n_b", "port": "cloud"},
                             "to": {"node": "n_fb", "port": "b"}}
            ],
            "outputs": outputs
        });
        let file = dir.join(format!("fb{seed}.lyflow.json"));
        std::fs::write(&file, doc.to_string()).unwrap();
        file.to_string_lossy().into_owned()
    }

    #[test]
    fn run_summary_says_degraded_when_a_fallback_saved_the_run() {
        let dir = workspace("summary-degraded");
        let graph = fallback_graph(
            &dir,
            3401,
            json!({ "result": {"node": "n_fb", "port": "out"} }),
        );
        let r = cli(&["run", &graph, "--summary", "--no-cache"]);
        assert_eq!(r.code, EXIT_OK, "{} / {}", r.out, r.err);

        let lines = r.lines();
        let last = lines.last().unwrap();
        assert_eq!(last["kind"], "run_summary");
        // run_finished 是 ok（失败被 acceptsError 接住了），summary 却是 degraded
        let finished = lines.iter().find(|e| e["kind"] == "run_finished").unwrap();
        assert_eq!(finished["status"], "ok");
        assert_eq!(last["status"], "degraded", "{last}");
        // 事件里那份与末尾这行是同一个对象（H1）
        let mut from_event = finished["summary"].clone();
        from_event["kind"] = json!("run_summary");
        assert_eq!(&from_event, last);

        assert_eq!(last["nodes"]["n_bad"]["state"], "error");
        assert_eq!(last["outputs"]["result"]["state"], "value");
        assert_eq!(last["outputs"]["result"]["node"], "n_fb");
        assert_eq!(last["outputs"]["result"]["elementCount"], 100);
        assert_eq!(last["decisions"]["n_fb"]["choice"], "b");
        assert_eq!(last["decisions"]["n_fb"]["type"], "FallbackChoice");
        assert!(last["contractViolations"].as_array().unwrap().is_empty());
    }

    #[test]
    fn run_summary_separates_a_missing_dimension_from_a_broken_one() {
        let dir = workspace("summary-three-state");
        // flush 挂在没被 demand 的备用分支上 → inactive；crash 挂在炸了的节点上 → failed
        let graph = fallback_graph(
            &dir,
            3402,
            json!({
                "result": {"node": "n_fb", "port": "out"},
                "crash": {"node": "n_bad", "port": "cloud"}
            }),
        );
        let r = cli(&["run", &graph, "--summary", "--no-cache"]);
        assert_eq!(r.code, EXIT_OK, "{}", r.err);
        let lines = r.lines();
        let s = lines.last().unwrap();
        assert_eq!(s["status"], "failed", "声明输出里有一维崩了（H2）: {s}");
        assert_eq!(s["outputs"]["crash"]["state"], "failed");
        assert_eq!(s["outputs"]["crash"]["from"], "n_bad");
        assert_eq!(s["outputs"]["crash"]["code"], "io");
        assert_eq!(s["outputs"]["result"]["state"], "value");

        // 主路径成功的那张图上，挂在惰性备用分支的那一维是 inactive 而不是 failed
        let doc = json!({
            "schemaVersion": 1, "id": "01J8XQZ4K7N3M2R5V8W1YB6TCG",
            "nodes": [
                {"id": "n_a", "op": "gen.synthetic", "params": {"pointCount": 40, "seed": 3403}},
                {"id": "n_b", "op": "gen.synthetic", "params": {"pointCount": 70, "seed": 3404}},
                {"id": "n_fb", "op": "flow.fallback"}
            ],
            "edges": [
                {"id": "e1", "from": {"node": "n_a", "port": "cloud"},
                             "to": {"node": "n_fb", "port": "a"}},
                {"id": "e2", "from": {"node": "n_b", "port": "cloud"},
                             "to": {"node": "n_fb", "port": "b"}}
            ],
            "outputs": {
                "gap": {"node": "n_fb", "port": "out"},
                "flush": {"node": "n_b", "port": "cloud"}
            }
        });
        let file = dir.join("happy.lyflow.json");
        std::fs::write(&file, doc.to_string()).unwrap();
        let r = cli(&["run", &file.to_string_lossy(), "--summary", "--no-cache"]);
        assert_eq!(r.code, EXIT_OK, "{}", r.err);
        let lines = r.lines();
        let s = lines.last().unwrap();
        assert_eq!(s["status"], "ok");
        assert_eq!(s["outputs"]["gap"]["state"], "value");
        assert_eq!(s["outputs"]["gap"]["elementCount"], 40);
        assert_eq!(s["outputs"]["flush"]["state"], "inactive");
        assert_eq!(s["outputs"]["flush"]["reason"], "not_demanded");
    }

    #[test]
    fn run_without_summary_flag_keeps_the_old_json_lines() {
        let dir = workspace("summary-off");
        let graph = chain(&dir, 3405);
        let r = cli(&["run", &graph, "--no-cache"]);
        assert_eq!(r.code, EXIT_OK, "{}", r.err);
        let lines = r.lines();
        assert_eq!(lines.last().unwrap()["kind"], "run_finished");
        assert!(lines.iter().all(|l| l["kind"] != "run_summary"));
        // 事件里那份照旧有 —— --summary 只管末尾那一行
        assert!(lines.last().unwrap()["summary"].is_object());
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

    fn samples_file(dir: &Path, name: &str, lines: &[&str]) -> String {
        let file = dir.join(name);
        std::fs::write(&file, format!("{}\n", lines.join("\n"))).unwrap();
        file.to_string_lossy().into_owned()
    }

    #[test]
    fn eval_reports_rows_and_per_group_statistics() {
        let dir = workspace("eval");
        let graph = chain(&dir, 320);
        let samples = samples_file(
            &dir,
            "s.jsonl",
            &[
                r#"{"id":"a","set":{"g.seed":3201},"tags":{"half":"a"}}"#,
                r#"{"id":"b","set":{"g.seed":3202},"tags":{"half":"a"}}"#,
                r#"{"id":"c","set":{"g.seed":3203},"tags":{"half":"b"}}"#,
            ],
        );
        let csv = dir.join("eval.csv");
        let r = cli(&[
            "eval",
            &graph,
            "--samples",
            &samples,
            "--metric",
            "nodes.v.elementCount",
            "--metric",
            "run.durationMs",
            "--holdout",
            "half=b",
            "--csv",
            &csv.to_string_lossy(),
        ]);
        assert_eq!(r.code, EXIT_OK, "{}", r.err);
        let lines = r.lines();
        let rows: Vec<&Value> = lines.iter().filter(|l| l["kind"] == "eval_row").collect();
        let summaries: Vec<&Value> = lines.iter().filter(|l| l["kind"] == "eval_summary").collect();
        assert_eq!(rows.len(), 3);
        assert_eq!(summaries.len(), 2, "每个 metric 一行 summary");

        assert_eq!(rows[0]["sample"], "a");
        assert_eq!(rows[0]["status"], "ok");
        assert_eq!(rows[0]["holdout"], false);
        assert_eq!(rows[0]["tags"]["half"], "a");
        assert!(rows[0]["metrics"]["nodes.v.elementCount"].as_f64().unwrap() > 0.0);
        assert_eq!(rows[2]["holdout"], true);

        let first = summaries[0];
        assert_eq!(first["metric"], "nodes.v.elementCount");
        assert_eq!(first["groups"]["train"]["n"], 2);
        assert_eq!(first["groups"]["train"]["ok"], 2);
        assert_eq!(first["groups"]["holdout"]["n"], 1);
        assert!(first["groups"]["holdout"]["std"].is_null(), "n<2 时 std 是 null");
        assert!(first["groups"]["train"]["std"].as_f64().unwrap() >= 0.0);

        let text = std::fs::read_to_string(&csv).unwrap();
        assert!(
            text.starts_with("paramSet,sample,holdout,status,nodes.v.elementCount,run.durationMs"),
            "{text}"
        );
        assert_eq!(text.lines().count(), 4);
    }

    /// ADR-0022：`--summary` 时每个 eval_row 带这一次运行的结论。
    /// **默认关**（m6-plan §10 第 5 条）：体积是逐行的，一维 bundle 就 6 KB。
    #[test]
    fn eval_rows_carry_the_run_summary_only_when_asked() {
        let dir = workspace("eval-summary");
        let graph = chain(&dir, 3410);
        let samples = samples_file(
            &dir,
            "s.jsonl",
            &[
                r#"{"id":"a","set":{"g.seed":34101}}"#,
                r#"{"id":"b","set":{"g.seed":34102}}"#,
            ],
        );
        let args = |extra: &[&str]| {
            let mut v = vec![
                "eval".to_string(),
                graph.clone(),
                "--samples".to_string(),
                samples.clone(),
                "--metric".to_string(),
                "nodes.v.elementCount".to_string(),
            ];
            v.extend(extra.iter().map(|s| (*s).to_string()));
            v
        };

        let r = cli_owned(&args(&["--summary"]));
        assert_eq!(r.code, EXIT_OK, "{}", r.err);
        let rows: Vec<Value> = r
            .lines()
            .into_iter()
            .filter(|l| l["kind"] == "eval_row")
            .collect();
        assert_eq!(rows.len(), 2);
        for row in &rows {
            assert_eq!(row["summary"]["status"], "ok", "{row}");
            assert_eq!(row["summary"]["nodes"]["v"]["state"], "done");
            assert!(row["summary"]["contractViolations"].is_array());
        }

        for extra in [&[][..], &["--no-summary"][..]] {
            let off = cli_owned(&args(extra));
            assert_eq!(off.code, EXIT_OK, "{}", off.err);
            let rows: Vec<Value> = off
                .lines()
                .into_iter()
                .filter(|l| l["kind"] == "eval_row")
                .collect();
            assert_eq!(rows.len(), 2);
            for row in &rows {
                assert!(row["summary"].is_null(), "默认不该带 summary（{extra:?}）: {row}");
            }
        }
    }

    #[test]
    fn eval_groups_by_a_tag_key() {
        let dir = workspace("evalgroup");
        let graph = chain(&dir, 321);
        let samples = samples_file(
            &dir,
            "s.jsonl",
            &[
                r#"{"id":"a","set":{"g.seed":3211},"tags":{"lot":"x"}}"#,
                r#"{"id":"b","set":{"g.seed":3212},"tags":{"lot":"y"}}"#,
                r#"{"id":"c","set":{"g.seed":3213}}"#,
            ],
        );
        let r = cli(&[
            "eval",
            &graph,
            "--samples",
            &samples,
            "--metric",
            "nodes.v.elementCount",
            "--group-by",
            "lot",
        ]);
        assert_eq!(r.code, EXIT_OK, "{}", r.err);
        let summary = r
            .lines()
            .into_iter()
            .find(|l| l["kind"] == "eval_summary")
            .unwrap();
        let groups = summary["groups"].as_object().unwrap();
        assert_eq!(groups.len(), 3, "{summary}");
        assert_eq!(groups["x"]["n"], 1);
        assert_eq!(groups["y"]["n"], 1);
        assert_eq!(groups["(none)"]["n"], 1);
    }

    #[test]
    fn eval_crosses_parameter_sets_with_samples() {
        let dir = workspace("evalparam");
        let graph = chain(&dir, 322);
        let samples = samples_file(
            &dir,
            "s.jsonl",
            &[
                r#"{"id":"a","set":{"g.seed":3221}}"#,
                r#"{"id":"b","set":{"g.seed":3222}}"#,
            ],
        );
        let r = cli(&[
            "eval",
            &graph,
            "--samples",
            &samples,
            "--param",
            "v.minPointsPerVoxel=1:3:3",
            "--metric",
            "nodes.v.elementCount",
        ]);
        assert_eq!(r.code, EXIT_OK, "{}", r.err);
        let lines = r.lines();
        let rows: Vec<&Value> = lines.iter().filter(|l| l["kind"] == "eval_row").collect();
        assert_eq!(rows.len(), 6);
        assert_eq!(rows[0]["paramSet"], 0);
        assert_eq!(rows[0]["params"]["v.minPointsPerVoxel"], 1.0);
        assert_eq!(rows[5]["paramSet"], 2);
        assert_eq!(rows[5]["params"]["v.minPointsPerVoxel"], 3.0);
        let summaries: Vec<&Value> = lines.iter().filter(|l| l["kind"] == "eval_summary").collect();
        assert_eq!(summaries.len(), 3);
        let mean = |s: &Value| s["groups"]["all"]["mean"].as_f64().unwrap();
        assert!(mean(summaries[2]) < mean(summaries[0]));
    }

    #[test]
    fn eval_lists_the_available_paths_when_the_metric_is_wrong() {
        let dir = workspace("evalpath");
        let graph = chain(&dir, 323);
        let samples = samples_file(&dir, "s.jsonl", &[r#"{"id":"a","set":{"g.seed":3231}}"#]);
        let r = cli(&[
            "eval",
            &graph,
            "--samples",
            &samples,
            "--metric",
            "outputs.gap",
        ]);
        assert_eq!(r.code, EXIT_USAGE, "{}", r.out);
        assert!(r.err.contains("outputs.gap"), "{}", r.err);
        assert!(r.err.contains("nodes.v.elementCount"), "{}", r.err);
        assert!(r.err.contains("run.durationMs"), "{}", r.err);
        let bad = cli(&["eval", &graph, "--samples", &samples, "--metric", "gap"]);
        assert_eq!(bad.code, EXIT_USAGE);
    }

    #[test]
    fn eval_refuses_the_scene_field() {
        let dir = workspace("evalscene");
        let graph = chain(&dir, 324);
        let samples = samples_file(&dir, "s.jsonl", &[r#"{"id":"a","scene":"sc_1"}"#]);
        let r = cli(&[
            "eval",
            &graph,
            "--samples",
            &samples,
            "--metric",
            "nodes.v.elementCount",
        ]);
        assert_eq!(r.code, EXIT_USAGE);
        assert!(r.err.contains("scene"), "{}", r.err);
    }

    #[test]
    fn eval_builds_samples_from_a_glob() {
        let dir = workspace("evalglob");
        let clouds = dir.join("clouds");
        std::fs::create_dir_all(&clouds).unwrap();
        let source = chain(&dir, 325);
        for name in ["one", "two"] {
            let target = clouds.join(format!("{name}.pcd"));
            let d = cli(&["dump", &source, "v:cloud", &target.to_string_lossy()]);
            assert_eq!(d.code, EXIT_OK, "{}", d.err);
        }
        let doc = json!({
            "schemaVersion": 1, "id": "01J8XQZ4K7N3M2R5V8W1YB6TD1",
            "nodes": [{"id": "r", "op": "io.load_pcd", "params": {"path": "placeholder.pcd"}}],
            "edges": []
        });
        let file = dir.join("load.lyflow.json");
        std::fs::write(&file, doc.to_string()).unwrap();
        let pattern = format!("{}/*.pcd", clouds.to_string_lossy().replace('\\', "/"));

        let r = cli(&[
            "eval",
            &file.to_string_lossy(),
            "--samples-glob",
            &pattern,
            "--bind",
            "r.path",
            "--metric",
            "nodes.r.elementCount",
        ]);
        assert_eq!(r.code, EXIT_OK, "{} / {}", r.out, r.err);
        let lines = r.lines();
        let rows: Vec<&Value> = lines.iter().filter(|l| l["kind"] == "eval_row").collect();
        assert_eq!(rows.len(), 2);
        assert_eq!(rows[0]["sample"], "one");
        assert_eq!(rows[1]["sample"], "two");
        assert!(rows[0]["metrics"]["nodes.r.elementCount"].as_f64().unwrap() > 0.0);
        let no_bind = cli(&[
            "eval",
            &file.to_string_lossy(),
            "--samples-glob",
            &pattern,
            "--metric",
            "nodes.r.elementCount",
        ]);
        assert_eq!(no_bind.code, EXIT_USAGE);
    }

    #[test]
    fn eval_separates_validation_failures_from_run_failures() {
        let dir = workspace("evalfail");
        let doc = json!({
            "schemaVersion": 1, "id": "01J8XQZ4K7N3M2R5V8W1YB6TD2",
            "nodes": [{"id": "r", "op": "io.load_pcd", "params": {"path": "有.pcd"}}],
            "edges": []
        });
        let file = dir.join("load.lyflow.json");
        std::fs::write(&file, doc.to_string()).unwrap();
        let samples = samples_file(
            &dir,
            "s.jsonl",
            &[r#"{"id":"missing","set":{"r.path":"没有这个文件.pcd"}}"#],
        );
        let r = cli(&[
            "eval",
            &file.to_string_lossy(),
            "--samples",
            &samples,
            "--metric",
            "nodes.r.elementCount",
        ]);
        assert_eq!(r.code, EXIT_FAILED, "{} / {}", r.out, r.err);
        let row = r
            .lines()
            .into_iter()
            .find(|l| l["kind"] == "eval_row")
            .unwrap();
        assert_eq!(row["status"], "failed");
        assert!(row["metrics"]["nodes.r.elementCount"].is_null());
        let summary = r
            .lines()
            .into_iter()
            .find(|l| l["kind"] == "eval_summary")
            .unwrap();
        assert_eq!(summary["groups"]["all"]["ok"], 0);
        assert!(
            summary["groups"]["all"]["failCodes"]
                .as_object()
                .unwrap()
                .values()
                .any(|v| v == 1),
            "{summary}"
        );
    }

    #[test]
    fn eval_without_samples_runs_the_graph_once() {
        let dir = workspace("evalnosample");
        let graph = chain(&dir, 326);
        let r = cli(&["eval", &graph, "--metric", "nodes.v.elementCount"]);
        assert_eq!(r.code, EXIT_OK, "{}", r.err);
        let rows: Vec<Value> = r
            .lines()
            .into_iter()
            .filter(|l| l["kind"] == "eval_row")
            .collect();
        assert_eq!(rows.len(), 1);
        assert_eq!(rows[0]["sample"], "-");
    }

    #[test]
    fn eval_accepts_the_old_sweep_metric_spelling() {
        let dir = workspace("evallegacy");
        let graph = chain(&dir, 327);
        let r = cli(&["eval", &graph, "--metric", "v:cloud.elementCount"]);
        assert_eq!(r.code, EXIT_OK, "{}", r.err);
        let row = r
            .lines()
            .into_iter()
            .find(|l| l["kind"] == "eval_row")
            .unwrap();
        assert!(row["metrics"]["v:cloud.elementCount"].as_f64().unwrap() > 0.0);
    }


    fn crop_chain(dir: &Path, seed: i64, count: i64) -> String {
        let doc = json!({
            "schemaVersion": 1,
            "id": "01J8XQZ4K7N3M2R5V8W1YB6TP1",
            "name": "perturb",
            "nodes": [
                {"id": "g", "op": "gen.synthetic",
                 "params": {"pointCount": count, "seed": seed, "outlierRatio": 0.0}},
                {"id": "c", "op": "filter.crop_box",
                 "params": {"min": [0.05, -10.0, -10.0], "max": [10.0, 10.0, 10.0]}}
            ],
            "edges": [
                {"id": "e1", "from": {"node": "g", "port": "cloud"},
                             "to": {"node": "c", "port": "cloud"}}
            ]
        });
        let file = dir.join("p.lyflow.json");
        std::fs::write(&file, serde_json::to_string_pretty(&doc).unwrap()).unwrap();
        file.to_string_lossy().into_owned()
    }

    const HALFSPACE: &str = r#"{"kind":"halfspace","point":[0,0,0],"normal":[1,0,0]}"#;

    #[test]
    fn perturb_inserts_the_node_and_reports_a_slope() {
        let dir = workspace("perturb");
        let graph = crop_chain(&dir, 3401, 17003);
        let r = cli(&[
            "perturb",
            &graph,
            "--after",
            "g:cloud",
            "--region",
            HALFSPACE,
            "--axis",
            "x=-0.04:0.04:5",
            "--metric",
            "nodes.c.elementCount",
        ]);
        assert_eq!(r.code, EXIT_OK, "{} / {}", r.out, r.err);
        let lines = r.lines();
        let rows: Vec<&Value> = lines.iter().filter(|l| l["kind"] == "perturb_row").collect();
        assert_eq!(rows.len(), 5);
        assert!((rows[0]["displacement"].as_f64().unwrap() + 0.04).abs() < 1e-12);
        assert!(rows[2]["displacement"].as_f64().unwrap().abs() < 1e-12);
        assert!((rows[4]["displacement"].as_f64().unwrap() - 0.04).abs() < 1e-12);
        assert_eq!(rows[0]["sample"], "-");

        let counts: Vec<f64> = rows
            .iter()
            .map(|r| r["metrics"]["nodes.c.elementCount"].as_f64().unwrap())
            .collect();
        assert!(counts[0] < counts[4], "{counts:?}");

        let per: Vec<&Value> = lines
            .iter()
            .filter(|l| l["kind"] == "perturb_sample")
            .collect();
        assert_eq!(per.len(), 1);
        assert_eq!(per[0]["n"], 5);
        assert!(per[0]["slope"].as_f64().unwrap() > 0.0, "{}", per[0]);
        assert!(per[0]["slopeNeg"].as_f64().unwrap() > 0.0, "{}", per[0]);
        assert!(per[0]["slopePos"].as_f64().unwrap() > 0.0, "{}", per[0]);
        assert!(per[0]["pass"].is_null(), "没给 --expect 时不判定");

        let sum = lines
            .iter()
            .find(|l| l["kind"] == "perturb_summary")
            .unwrap();
        assert_eq!(sum["samples"], 1);
        assert_eq!(sum["signFold"], 0);
        assert_eq!(sum["nonResponsive"], 0);
        assert_eq!(sum["metric"], "nodes.c.elementCount");
        assert!(r.err.contains("__perturb"), "{}", r.err);
    }

    #[test]
    fn perturb_flags_a_reading_that_does_not_move_and_exits_failed() {
        let dir = workspace("perturbflat");
        let graph = crop_chain(&dir, 3402, 17005);
        let r = cli(&[
            "perturb",
            &graph,
            "--after",
            "g:cloud",
            "--region",
            HALFSPACE,
            "--axis",
            "x=-0.04:0.04:5",
            "--metric",
            "nodes.g.elementCount",
            "--expect",
            "1",
        ]);
        assert_eq!(r.code, EXIT_FAILED, "{} / {}", r.out, r.err);
        let lines = r.lines();
        let per = lines
            .iter()
            .find(|l| l["kind"] == "perturb_sample")
            .unwrap();
        assert_eq!(per["slope"], 0.0);
        assert_eq!(per["pass"], false);
        let sum = lines
            .iter()
            .find(|l| l["kind"] == "perturb_summary")
            .unwrap();
        assert_eq!(sum["pass"], 0);
        assert_eq!(sum["nonResponsive"], 1);
    }

    #[test]
    fn perturb_refuses_a_subgraph_internal_port() {
        let dir = workspace("perturbsub");
        let graph = crop_chain(&dir, 3403, 17007);
        let r = cli(&[
            "perturb",
            &graph,
            "--after",
            "sub/g:cloud",
            "--region",
            HALFSPACE,
            "--axis",
            "x=0:1:2",
            "--metric",
            "nodes.c.elementCount",
        ]);
        assert_eq!(r.code, EXIT_USAGE);
        assert!(r.err.contains("子图"), "{}", r.err);

        let bad_region = cli(&[
            "perturb",
            &graph,
            "--after",
            "g:cloud",
            "--region",
            r#"{"kind":"sphere"}"#,
            "--axis",
            "x=0:1:2",
            "--metric",
            "nodes.c.elementCount",
        ]);
        assert_eq!(bad_region.code, EXIT_USAGE);
        assert!(bad_region.err.contains("kind"), "{}", bad_region.err);

        let ghost = cli(&[
            "perturb",
            &graph,
            "--after",
            "nope:cloud",
            "--region",
            HALFSPACE,
            "--axis",
            "x=0:1:2",
            "--metric",
            "nodes.c.elementCount",
        ]);
        assert_eq!(ghost.code, EXIT_USAGE);
        assert!(ghost.err.contains("没有节点"), "{}", ghost.err);
    }

    // ------------------------------------------------------------ 顶层图参数（M7 J7/J8）

    /// g → v 的直链外加一支不相干的 h。顶层参数 count 绑 g.pointCount（g 上不再显式写它）。
    fn param_chain(dir: &Path, seed: i64) -> String {
        let doc = json!({
            "schemaVersion": 1,
            "id": "01J8XQZ4K7N3M2R5V8W1YB6TCD",
            "name": "cli-params",
            "params": {
                "count": {"type": "int", "default": 20000, "binds": ["g.pointCount"],
                          "doc": "g 的点数"}
            },
            "nodes": [
                {"id": "g", "op": "gen.synthetic", "params": {"seed": seed}},
                {"id": "v", "op": "filter.voxel_grid",
                 "params": {"leafSize": [0.02, 0.02, 0.02]}},
                {"id": "h", "op": "gen.synthetic", "params": {"pointCount": 500, "seed": seed + 1}}
            ],
            "edges": [
                {"id": "e1", "from": {"node": "g", "port": "cloud"},
                             "to": {"node": "v", "port": "cloud"}}
            ]
        });
        let file = dir.join("p.lyflow.json");
        std::fs::write(&file, serde_json::to_string_pretty(&doc).unwrap()).unwrap();
        file.to_string_lossy().into_owned()
    }

    fn plan_keys(r: &Ran) -> BTreeMap<String, String> {
        r.first()
            .as_array()
            .unwrap()
            .iter()
            .map(|n| {
                (
                    n["nodeId"].as_str().unwrap().to_string(),
                    n["cacheKey"].as_str().unwrap().to_string(),
                )
            })
            .collect()
    }

    fn run_started_keys(events: &[Value]) -> BTreeMap<String, String> {
        let started = events.iter().find(|e| e["kind"] == "run_started").expect("没有 run_started");
        started["nodes"]
            .as_array()
            .unwrap()
            .iter()
            .map(|n| {
                (
                    n["id"].as_str().unwrap().to_string(),
                    n["cacheKey"].as_str().unwrap().to_string(),
                )
            })
            .collect()
    }

    fn done_count(events: &[Value], node: &str) -> Option<i64> {
        events
            .iter()
            .filter(|e| e["kind"] == "node_state" && e["nodeId"] == node)
            .filter(|e| e["state"] == "done" || e["state"] == "skipped")
            .last()
            .and_then(|e| e["stats"]["elementCount"].as_i64())
    }

    #[test]
    fn params_reports_the_graph_source_and_which_graph_param() {
        let dir = workspace("gparam-params");
        let graph = param_chain(&dir, 7101);
        let r = cli(&["params", &graph, "--json", "--param", "count=1234"]);
        assert_eq!(r.code, EXIT_OK, "{}", r.err);
        let row = r
            .lines()
            .into_iter()
            .find(|x| x["node"] == "g" && x["param"] == "pointCount")
            .expect("没有 g.pointCount");
        assert_eq!(row["value"], 1234);
        assert_eq!(row["source"], "graph");
        assert_eq!(row["graphParam"], "count");
        // 不传值就是 default，来源仍是 graph
        let d = cli(&["params", &graph, "--json", "--only", "graph"]);
        assert_eq!(d.code, EXIT_OK, "{}", d.err);
        let rows = d.lines();
        assert_eq!(rows.len(), 1, "{}", d.out);
        assert_eq!(rows[0]["value"], 20000);
    }

    #[test]
    fn param_only_moves_the_bound_node_and_its_downstream() {
        let dir = workspace("gparam-plan");
        let graph = param_chain(&dir, 7102);
        let a = cli(&["plan", &graph]);
        let b = cli(&["plan", &graph, "--param", "count=1234"]);
        assert_eq!(a.code, EXIT_OK, "{}", a.err);
        assert_eq!(b.code, EXIT_OK, "{}", b.err);
        let (ka, kb) = (plan_keys(&a), plan_keys(&b));
        assert_ne!(ka["g"], kb["g"]);
        assert_ne!(ka["v"], kb["v"]);
        assert_eq!(ka["h"], kb["h"]);
        assert_eq!(cli(&["validate", &graph, "--param", "count=1234"]).code, EXIT_OK);
    }

    /// J8：宿主走 C ABI 的 params_json，CLI 走 --param —— 两条路算出同一个东西。
    #[test]
    fn abi_params_json_and_cli_param_agree() {
        let dir = workspace("gparam-abi");
        let graph = param_chain(&dir, 7103);
        let r = cli(&["run", &graph, "--param", "count=1234", "--no-cache"]);
        assert_eq!(r.code, EXIT_OK, "{}", r.err);
        let cli_events = r.lines();
        assert_eq!(done_count(&cli_events, "g"), Some(1234));

        let core = core().unwrap();
        let text = std::fs::read_to_string(&graph).unwrap();
        let abi = execute(
            &core,
            RunRequest {
                graph_json: &text,
                base_dir: &dir.to_string_lossy(),
                targets: &[],
                parallel: 0,
                preview_points: 0,
                preview: false,
                no_cache: true,
                stream: None,
                params_json: Some(r#"{"count": 1234}"#),
                inputs: &[],
            },
        )
        .unwrap();
        assert_eq!(abi.status, "ok");
        assert_eq!(done_count(&abi.events, "g"), Some(1234));
        assert_eq!(done_count(&abi.events, "v"), done_count(&cli_events, "v"));
        assert_eq!(run_started_keys(&abi.events), run_started_keys(&cli_events));

        // ABI 上传了图没声明的名字：校验阶段就失败
        let bad = execute(
            &core,
            RunRequest {
                graph_json: &text,
                base_dir: &dir.to_string_lossy(),
                targets: &[],
                parallel: 0,
                preview_points: 0,
                preview: false,
                no_cache: true,
                stream: None,
                params_json: Some(r#"{"nope": 1}"#),
                inputs: &[],
            },
        )
        .unwrap();
        assert_eq!(bad.status, "error");
        let finished = bad.events.iter().find(|e| e["kind"] == "run_finished").unwrap();
        assert_eq!(finished["error"]["code"], "unknown_param", "{finished}");
    }

    #[test]
    fn set_on_a_bound_param_and_unknown_param_are_usage_errors() {
        let dir = workspace("gparam-conflict");
        let graph = param_chain(&dir, 7104);
        let set = cli(&["run", &graph, "--set", "g.pointCount=5"]);
        assert_eq!(set.code, EXIT_USAGE, "{}", set.err);
        assert!(set.err.contains("param_conflict"), "{}", set.err);
        assert!(set.err.contains("--param count="), "{}", set.err);
        assert_eq!(cli(&["params", &graph, "--set", "g.pointCount=5"]).code, EXIT_USAGE);

        let unknown = cli(&["validate", &graph, "--param", "nope=1"]);
        assert_eq!(unknown.code, EXIT_USAGE, "{}", unknown.err);
        assert!(unknown.err.contains("unknown_param"), "{}", unknown.err);

        // 图里自己写了被绑定的参数：core 的 validate 报 param_conflict
        let mut doc: Value = serde_json::from_str(&std::fs::read_to_string(&graph).unwrap()).unwrap();
        doc["nodes"][0]["params"]["pointCount"] = json!(5);
        let file = dir.join("conflict.lyflow.json");
        std::fs::write(&file, doc.to_string()).unwrap();
        let v = cli(&["validate", &file.to_string_lossy()]);
        assert_eq!(v.code, EXIT_INVALID, "{}", v.err);
        assert!(v.first().as_array().unwrap().iter().any(|d| d["code"] == "param_conflict"));
    }

    #[test]
    fn patch_param_rewrites_the_default_and_is_idempotent() {
        let dir = workspace("gparam-patch");
        let graph = param_chain(&dir, 7105);
        let out = dir.join("patched.lyflow.json");
        let r = cli(&["patch", &graph, "--param", "count=777", "-o", &out.to_string_lossy(), "--json"]);
        assert_eq!(r.code, EXIT_OK, "{}", r.err);
        let result = r.first();
        assert_eq!(result["applied"]["param"], json!(["count"]));
        assert_eq!(result["diff"]["graphParams"][0]["to"], 777);
        let written: Value = serde_json::from_str(&std::fs::read_to_string(&out).unwrap()).unwrap();
        assert_eq!(written["params"]["count"]["default"], 777);
        // 同值再来一遍：no-op，diff 为空
        let again = cli(&["patch", &out.to_string_lossy(), "--param", "count=777", "--dry-run", "--json"]);
        assert_eq!(again.code, EXIT_OK, "{}", again.err);
        assert_eq!(again.first()["diff"]["empty"], true);
    }

    #[test]
    fn eval_takes_graph_params_next_to_sweep_axes() {
        let dir = workspace("gparam-eval");
        let graph = param_chain(&dir, 7106);
        // count=3000 是顶层参数；v.leafSize=... 是扫描轴（左边带点）
        let r = cli(&[
            "eval",
            &graph,
            "--param",
            "count=3000",
            "--param",
            "v.leafSize=0.02:0.04:2",
            "--metric",
            "nodes.g.elementCount",
        ]);
        assert_eq!(r.code, EXIT_OK, "{}", r.err);
        let rows: Vec<Value> = r.lines().into_iter().filter(|l| l["kind"] == "eval_row").collect();
        assert_eq!(rows.len(), 2, "{}", r.out);
        for row in &rows {
            assert_eq!(row["metrics"]["nodes.g.elementCount"].as_f64(), Some(3000.0), "{row}");
        }
    }

    // ------------------------------------------------------------ 配方（param-recipe P4.1）

    /// g → v 的直链外加 h；count 绑 g.pointCount，leaf 绑 v.leafSize，都有完整规格。
    /// 返回图路径；配方写进旁边的 `r.recipes/`（目录约定，`lyflow recipes` 列它）。
    fn recipe_chain(dir: &Path, seed: i64) -> String {
        let doc = json!({
            "schemaVersion": 1,
            "id": "01J8XQZ4K7N3M2R5V8W1YB6RCP",
            "name": "cli-recipe",
            "params": {
                "count": {"type": "int", "default": 20000, "binds": ["g.pointCount"], "min": 1, "max": 100000},
                "leaf": {"type": "vec3f", "default": [0.02, 0.02, 0.02], "binds": ["v.leafSize"],
                         "min": 0.001, "max": 1}
            },
            "nodes": [
                {"id": "g", "op": "gen.synthetic", "params": {"seed": seed}},
                {"id": "v", "op": "filter.voxel_grid"},
                {"id": "h", "op": "gen.synthetic", "params": {"pointCount": 500, "seed": seed + 1}}
            ],
            "edges": [
                {"id": "e1", "from": {"node": "g", "port": "cloud"}, "to": {"node": "v", "port": "cloud"}}
            ]
        });
        let file = dir.join("r.lyflow.json");
        std::fs::write(&file, serde_json::to_string_pretty(&doc).unwrap()).unwrap();
        file.to_string_lossy().into_owned()
    }

    /// 在图旁的配方目录里写一个配方文件。graph_ref = None 时记成当前图（id 与摘要都对）。
    fn write_recipe(graph: &str, name: &str, values: Value, graph_ref: Option<Value>) -> String {
        let doc: GraphDoc = serde_json::from_str(&std::fs::read_to_string(graph).unwrap()).unwrap();
        let dir = recipe::recipe_dir_of(Path::new(graph));
        std::fs::create_dir_all(&dir).unwrap();
        let file = dir.join(format!("{name}.lyflow-recipe.json"));
        let g = graph_ref.unwrap_or_else(|| json!({"id": doc.id, "specDigest": recipe::spec_digest(&doc.params)}));
        let body = json!({"schemaVersion": 1, "name": name, "graph": g, "values": values,
                          "updatedAt": "2026-09-25T00:00:00.000Z"});
        std::fs::write(&file, serde_json::to_string_pretty(&body).unwrap()).unwrap();
        file.to_string_lossy().into_owned()
    }

    /// P4 验收 26 的 CLI 这一半：`--recipe` 与把同一组值写成 `--param` 是同一个结果（cacheKey 与点数），
    /// 同名的 `--param` 覆盖配方。编辑器那一半在 scripts/e2e/params_p4.mjs（真实 gap 图逐位比）。
    #[test]
    fn recipe_runs_like_the_same_values_given_as_param_and_param_wins() {
        let dir = workspace("recipe-run");
        let graph = recipe_chain(&dir, 7201);
        let a = write_recipe(&graph, "车型A", json!({"count": 1234, "leaf": [0.05, 0.05, 0.05]}), None);

        let r = cli(&["run", &graph, "--recipe", &a, "--no-cache"]);
        assert_eq!(r.code, EXIT_OK, "{}", r.err);
        assert!(r.err.contains("配方「车型A」：2 个值，2 个与基础不同"), "{}", r.err);
        let via_recipe = r.lines();
        assert_eq!(done_count(&via_recipe, "g"), Some(1234));

        let p = cli(&["run", &graph, "--param", "count=1234", "--param", "leaf=[0.05,0.05,0.05]", "--no-cache"]);
        assert_eq!(p.code, EXIT_OK, "{}", p.err);
        let via_param = p.lines();
        assert_eq!(run_started_keys(&via_recipe), run_started_keys(&via_param));
        assert_eq!(done_count(&via_recipe, "v"), done_count(&via_param, "v"));

        // --param 优先：配方里的 count 被盖掉，leaf 仍是配方的
        let both = cli(&["run", &graph, "--recipe", &a, "--param", "count=777", "--no-cache"]);
        assert_eq!(both.code, EXIT_OK, "{}", both.err);
        assert_eq!(done_count(&both.lines(), "g"), Some(777));
        let plan_both = cli(&["plan", &graph, "--recipe", &a, "--param", "count=777"]);
        let plan_param = cli(&["plan", &graph, "--param", "count=777", "--param", "leaf=[0.05,0.05,0.05]"]);
        assert_eq!(plan_keys(&plan_both), plan_keys(&plan_param));
        // 没绑到配方参数的 h 不受影响
        assert_eq!(plan_keys(&plan_both)["h"], plan_keys(&cli(&["plan", &graph]))["h"]);

        let v = cli(&["validate", &graph, "--recipe", &a]);
        assert_eq!(v.code, EXIT_OK, "{}", v.err);
        let params = cli(&["params", &graph, "--json", "--only", "graph", "--recipe", &a]);
        assert_eq!(params.code, EXIT_OK, "{}", params.err);
        let row = params.lines().into_iter().find(|x| x["node"] == "v").expect("没有 v.leafSize");
        assert_eq!(row["value"], json!([0.05, 0.05, 0.05]));
        assert_eq!(row["graphParam"], "leaf");

        assert_eq!(cli(&["run", &graph, "--recipe", &a, "--recipe", &a]).code, EXIT_USAGE);
        let nowhere = dir.join("没有.lyflow-recipe.json").to_string_lossy().into_owned();
        let missing = cli(&["run", &graph, "--recipe", &nowhere]);
        assert_eq!(missing.code, EXIT_USAGE, "{}", missing.err);
        assert!(missing.err.contains("bad_recipe"), "{}", missing.err);
    }

    /// P4 验收 27：失配 ①–③ → 退出码 4、stderr 逐条列出（与编辑器同一套用语），一个节点都不跑；
    /// run / validate / plan / params / eval 都一样。
    #[test]
    fn a_mismatched_recipe_stops_every_command_with_exit_4_and_the_report() {
        let dir = workspace("recipe-mismatch");
        let graph = recipe_chain(&dir, 7202);
        let bad = write_recipe(&graph, "坏", json!({"count": 0, "leaf": "abc", "nope": 1}), None);
        for args in [
            vec!["run", graph.as_str(), "--recipe", bad.as_str()],
            vec!["validate", graph.as_str(), "--recipe", bad.as_str()],
            vec!["plan", graph.as_str(), "--recipe", bad.as_str()],
            vec!["params", graph.as_str(), "--recipe", bad.as_str()],
            vec!["eval", graph.as_str(), "--recipe", bad.as_str(), "--metric", "nodes.g.elementCount"],
        ] {
            let r = cli(&args);
            assert_eq!(r.code, EXIT_USAGE, "{args:?}: {}", r.err);
            assert!(r.out.trim().is_empty(), "{args:?} 不该有 stdout：{}", r.out);
            for want in [
                "配方「坏」有 3 处失配，不能运行",
                "[越界] count：不能小于 1 → 夹到限位：1",
                "[类型不符] leaf：应当是 3 个数的数组，实际是 \"abc\" → 删除这个值（用基础）",
                "[多出] nope：图里没有图参数 nope（改名或删掉了？） → 删除这个值",
            ] {
                assert!(r.err.contains(want), "{args:?} 缺「{want}」：\n{}", r.err);
            }
        }
        // 共享夹具：每一条都照 expected.json 的文案出现在 stderr 上
        let fixtures = Path::new(env!("CARGO_MANIFEST_DIR")).join("../schema/fixtures/recipes");
        let expected: Value =
            serde_json::from_str(&std::fs::read_to_string(fixtures.join("expected.json")).unwrap()).unwrap();
        let fixture_graph = fixtures.join("graph.lyflow.json").to_string_lossy().into_owned();
        for (file, want) in expected["recipes"].as_object().unwrap() {
            let path = fixtures.join("graph.recipes").join(file).to_string_lossy().into_owned();
            let r = cli(&["validate", &fixture_graph, "--recipe", &path]);
            if want["blocking"].as_u64().unwrap() == 0 {
                // 没失配时交给 core 校验（夹具图的 test.param_showcase 只在 LYFLOW_TEST_OPS=1 时注册），退出码不是 4
                assert_ne!(r.code, EXIT_USAGE, "{file}: {}", r.err);
                continue;
            }
            assert_eq!(r.code, EXIT_USAGE, "{file}: {}", r.err);
            for item in want["items"].as_array().unwrap() {
                let label = match item["kind"].as_str().unwrap() {
                    "extra" => "多出",
                    "type" => "类型不符",
                    "range" => "越界",
                    _ => "规格变了",
                };
                let (message, fix) = (item["message"].as_str().unwrap(), item["fixLabel"].as_str().unwrap());
                let line = match item["param"].as_str() {
                    Some(p) => format!("[{label}] {p}：{message} → {fix}"),
                    None => format!("[{label}] {message} → {fix}"),
                };
                assert!(r.err.contains(&line), "{file} 缺「{line}」：\n{}", r.err);
            }
        }
    }

    /// ④ 规格变了只提示：退出码照旧，stderr 一行「提示：…」，值照常用上。
    #[test]
    fn a_recipe_written_for_another_graph_only_warns() {
        let dir = workspace("recipe-spec");
        let graph = recipe_chain(&dir, 7203);
        let other = write_recipe(
            &graph,
            "别的图",
            json!({"count": 4321}),
            Some(json!({"id": "01JSOMEOTHERGRAPH000000000", "specDigest": format!("sha256:{}", "0".repeat(64))})),
        );
        let r = cli(&["run", &graph, "--recipe", &other, "--no-cache"]);
        assert_eq!(r.code, EXIT_OK, "{}", r.err);
        assert!(
            r.err.contains("提示：配方「别的图」[规格变了] 图 id 不同（配方记的是 01JSOMEOTHERGRAPH000000000）"),
            "{}",
            r.err
        );
        assert_eq!(done_count(&r.lines(), "g"), Some(4321));
    }

    /// eval：配方作用于所有样本；参数组里不含「.」的键写图参数，叠在配方上面；--param 最后说了算。
    #[test]
    fn eval_layers_base_recipe_paramsets_then_param() {
        let dir = workspace("recipe-eval");
        let graph = recipe_chain(&dir, 7204);
        let a = write_recipe(&graph, "A", json!({"count": 1234}), None);
        let samples = dir.join("samples.jsonl");
        std::fs::write(&samples, "{\"id\":\"s1\",\"set\":{\"h.seed\":1}}\n{\"id\":\"s2\",\"set\":{\"h.seed\":2}}\n").unwrap();
        let sets = dir.join("sets.json");
        std::fs::write(&sets, r#"[{"count": 3000}, {"h.pointCount": 600}]"#).unwrap();
        let (samples, sets) = (samples.to_string_lossy().into_owned(), sets.to_string_lossy().into_owned());
        let rows = |r: &Ran| -> Vec<(u64, String, f64)> {
            r.lines()
                .into_iter()
                .filter(|l| l["kind"] == "eval_row")
                .map(|l| {
                    (
                        l["paramSet"].as_u64().unwrap(),
                        l["sample"].as_str().unwrap().to_string(),
                        l["metrics"]["nodes.g.elementCount"].as_f64().unwrap_or(-1.0),
                    )
                })
                .collect()
        };
        let base = ["eval", graph.as_str(), "--samples", samples.as_str(), "--metric", "nodes.g.elementCount"];

        let only = cli(&[&base[..], &["--recipe", a.as_str()]].concat());
        assert_eq!(only.code, EXIT_OK, "{}", only.err);
        assert_eq!(rows(&only), [(0, "s1".into(), 1234.0), (0, "s2".into(), 1234.0)]);

        let layered = cli(&[&base[..], &["--recipe", a.as_str(), "--params", sets.as_str()]].concat());
        assert_eq!(layered.code, EXIT_OK, "{}", layered.err);
        assert_eq!(
            rows(&layered),
            [(0, "s1".into(), 3000.0), (0, "s2".into(), 3000.0), (1, "s1".into(), 1234.0), (1, "s2".into(), 1234.0)]
        );

        let pinned =
            cli(&[&base[..], &["--recipe", a.as_str(), "--params", sets.as_str(), "--param", "count=500"]].concat());
        assert_eq!(pinned.code, EXIT_OK, "{}", pinned.err);
        assert!(rows(&pinned).iter().all(|(_, _, n)| *n == 500.0), "{:?}", rows(&pinned));

        let typo = dir.join("typo.json");
        std::fs::write(&typo, r#"[{"cuont": 1}]"#).unwrap();
        let typo = typo.to_string_lossy().into_owned();
        let t = cli(&[&base[..], &["--params", typo.as_str()]].concat());
        assert_eq!(t.code, EXIT_USAGE, "{}", t.err);
        assert!(t.err.contains("unknown_param"), "{}", t.err);
    }

    /// patch --recipe：配方的值写回基础（等于对每一行「写回基础」），--param 排在后面；失配整体不写。
    #[test]
    fn patch_recipe_writes_the_values_back_as_defaults() {
        let dir = workspace("recipe-patch");
        let graph = recipe_chain(&dir, 7205);
        let a = write_recipe(&graph, "A", json!({"count": 1234, "leaf": [0.05, 0.05, 0.05]}), None);
        let out = dir.join("baked.lyflow.json").to_string_lossy().into_owned();
        let r = cli(&["patch", &graph, "--recipe", &a, "--param", "count=999", "-o", &out, "--json"]);
        assert_eq!(r.code, EXIT_OK, "{}", r.err);
        assert_eq!(r.first()["applied"]["recipe"], json!(["count", "leaf"]));
        assert_eq!(r.first()["applied"]["param"], json!(["count"]));
        let written: Value = serde_json::from_str(&std::fs::read_to_string(&out).unwrap()).unwrap();
        assert_eq!(written["params"]["count"]["default"], 999);
        assert_eq!(written["params"]["leaf"]["default"], json!([0.05, 0.05, 0.05]));

        let again = cli(&["patch", &out, "--recipe", &a, "--param", "count=999", "--dry-run", "--json"]);
        assert_eq!(again.code, EXIT_OK, "{}", again.err);
        assert_eq!(again.first()["diff"]["empty"], true);

        let bad = write_recipe(&graph, "坏", json!({"count": 0}), None);
        let before = std::fs::read_to_string(&graph).unwrap();
        let b = cli(&["patch", &graph, "--recipe", &bad]);
        assert_eq!(b.code, EXIT_USAGE, "{}", b.err);
        assert!(b.err.contains("[越界] count"), "{}", b.err);
        assert_eq!(std::fs::read_to_string(&graph).unwrap(), before, "失配时不该写图");
    }

    /// `lyflow recipes`：列配方目录（index.json 的顺序与默认），每个配方的失配与夹具一致；
    /// 给 --recipe 时带上合成好的 params。不需要 core。
    #[test]
    fn recipes_lists_the_dir_with_reports_and_params() {
        let fixtures = Path::new(env!("CARGO_MANIFEST_DIR")).join("../schema/fixtures/recipes");
        let graph = fixtures.join("graph.lyflow.json").to_string_lossy().into_owned();
        let expected: Value =
            serde_json::from_str(&std::fs::read_to_string(fixtures.join("expected.json")).unwrap()).unwrap();
        let r = cli(&["recipes", &graph, "--json"]);
        assert_eq!(r.code, EXIT_OK, "{}", r.err);
        let lines = r.lines();
        let (rows, tail) = lines.split_at(lines.len() - 1);
        let names: Vec<&str> = rows.iter().map(|x| x["name"].as_str().unwrap()).collect();
        assert_eq!(names, ["ok", "extra", "type", "range", "spec", "nograph"]);
        for row in rows {
            let file = format!("{}.lyflow-recipe.json", row["name"].as_str().unwrap());
            let want = &expected["recipes"][&file];
            assert_eq!(row["blocking"], want["blocking"], "{file}");
            assert_eq!(row["items"].as_array().unwrap().len(), want["items"].as_array().unwrap().len(), "{file}");
            assert_eq!(row["default"], row["name"] == "ok");
            assert!(row.get("params").is_none());
        }
        assert_eq!(tail[0]["kind"], "recipe_dir");
        assert_eq!(tail[0]["default"], "ok");
        assert_eq!(tail[0]["count"], 6);
        assert_eq!(tail[0]["specDigest"], expected["specDigest"]);

        let ok = fixtures.join("graph.recipes/ok.lyflow-recipe.json").to_string_lossy().into_owned();
        let one = cli(&["recipes", &graph, "--recipe", &ok, "--json"]);
        assert_eq!(one.code, EXIT_OK, "{}", one.err);
        let row = one.first();
        assert_eq!(row["params"]["cutMax"], json!(2.5));
        assert_eq!(row["params"]["pointCount"], json!(20000));
        assert_eq!(row["params"]["legacyMin"], json!("随便什么都不查"));

        // 没有配方目录的图：0 个配方，exists=false
        let dir = workspace("recipe-none");
        let lonely = recipe_chain(&dir, 7206);
        let none = cli(&["recipes", &lonely, "--json"]);
        assert_eq!(none.code, EXIT_OK, "{}", none.err);
        assert_eq!(none.first()["exists"], false);
        assert_eq!(none.first()["count"], 0);
    }

    /// J5/J6：dirMode=band 却没接 refLine。检查在 validate 里，所以 `run` 在执行任何节点
    /// 之前就停下（没有一条 node_state）。gap 包没编进来时 gap.fit_line 是 unknown_op，
    /// 同样校验失败 —— 所以前半句在任何构建里都成立，诊断码只在带 gap 包时才断言。
    #[test]
    fn fit_line_band_without_ref_line_fails_at_validate() {
        let dir = workspace("m7-fit-line");
        let doc = json!({
            "schemaVersion": 1,
            "id": "01J8XQZ4K7N3M2R5V8W1YB6TCD",
            "nodes": [
                {"id": "g", "op": "gen.synthetic", "params": {"pointCount": 2000, "seed": 7107}},
                {"id": "roi", "op": "gap.overall_roi"},
                {"id": "fit", "op": "gap.fit_line", "params": {"dirMode": "band"}}
            ],
            "edges": [
                {"id": "e1", "from": {"node": "g", "port": "cloud"}, "to": {"node": "roi", "port": "primary"}},
                {"id": "e2", "from": {"node": "g", "port": "cloud"}, "to": {"node": "roi", "port": "secondary"}},
                {"id": "e3", "from": {"node": "g", "port": "cloud"}, "to": {"node": "fit", "port": "cloud"}},
                {"id": "e4", "from": {"node": "roi", "port": "box"}, "to": {"node": "fit", "port": "box"}},
                {"id": "e5", "from": {"node": "roi", "port": "box"}, "to": {"node": "fit", "port": "toward"}}
            ]
        });
        let file = dir.join("band.lyflow.json");
        std::fs::write(&file, doc.to_string()).unwrap();
        let path = file.to_string_lossy().into_owned();

        let v = cli(&["validate", &path]);
        assert_eq!(v.code, EXIT_INVALID, "{}", v.err);
        let run = cli(&["run", &path]);
        assert_eq!(run.code, EXIT_INVALID, "{}", run.err);
        assert!(
            !run.lines().iter().any(|e| e["kind"] == "node_state" || e["kind"] == "run_started"),
            "校验没过就不该起跑：{}",
            run.out
        );

        let gap_built = std::env::var("LYFLOW_PACKS").unwrap_or_default().contains("gap");
        let manifest = cli(&["manifest"]).first();
        let has_fit_line = manifest["operators"]
            .as_array()
            .unwrap()
            .iter()
            .any(|o| o["id"] == "gap.fit_line");
        assert!(!gap_built || has_fit_line, "LYFLOW_PACKS 带了 gap，manifest 里却没有 gap.fit_line");
        if has_fit_line {
            let diags = v.first();
            let d = diags
                .as_array()
                .unwrap()
                .iter()
                .find(|d| d["severity"] == "error")
                .unwrap()
                .clone();
            assert_eq!(d["code"], "bad_param", "{diags}");
            assert_eq!(d["phase"], "validate", "{diags}");
            assert_eq!(d["nodeId"], "fit", "{diags}");
        }
    }

    /// m8-plan L12 与 M8a 验收 4：`import` 默认产出积木图，`--fine` 产出细粒度图；
    /// 积木图里把 datum 框拖到 target 那一侧，`lyflow validate` 在执行前就报错。
    /// 导入器属于 gap 包，没编进来时整条跳过（manifest 里没有这个 kind）。
    #[test]
    fn import_defaults_to_blocks_and_fine_flag_gives_the_fine_graph() {
        let manifest = cli(&["manifest"]).first();
        let has_importer = manifest["importers"]
            .as_array()
            .map(|a| a.iter().any(|i| i["kind"] == "StandardGap.yml:template:fine"))
            .unwrap_or(false);
        let gap_built = std::env::var("LYFLOW_PACKS").unwrap_or_default().contains("gap");
        assert!(!gap_built || has_importer, "LYFLOW_PACKS 带了 gap，却没有细粒度导入器");
        if !has_importer {
            return;
        }
        let dir = workspace("m8a-import");
        let config = dir.join("StandardGap.yml");
        std::fs::write(
            &config,
            "common_settings: {seg_mode: ROI, overall_roi: [-18, 150, 18, 180]}\n\
             flush: {base_type: fit line, ref_type: line end, base_roi: [-15, 163, -5, 167],\
               ref_roi: [5, 162, 15, 166]}\n\
             gap: {left_type: circle, right_type: circle, left_roi: [-4.5, 164, -1.5, 167],\
               right_roi: [1.5, 163, 4.5, 166], radius: {left_circle_radius_min: 0.5,\
               left_circle_radius_max: 2, right_circle_radius_min: 0.5, right_circle_radius_max: 2}}\n\
             align: {align_cloud: true}\n",
        )
        .unwrap();
        let config = config.to_string_lossy().into_owned();
        let blocks_path = dir.join("blocks.lyflow.json").to_string_lossy().into_owned();
        let fine_path = dir.join("fine.lyflow.json").to_string_lossy().into_owned();

        let r = cli(&["import", &config, "--kind", "StandardGap.yml:template", "-o", &blocks_path]);
        assert_eq!(r.code, EXIT_OK, "{}", r.err);
        let r = cli(&["import", &config, "--kind", "StandardGap.yml:template", "--fine", "-o", &fine_path]);
        assert_eq!(r.code, EXIT_OK, "{}", r.err);

        let read = |p: &str| -> Value { serde_json::from_str(&std::fs::read_to_string(p).unwrap()).unwrap() };
        let ops = |doc: &Value| -> Vec<String> {
            doc["nodes"].as_array().unwrap().iter().map(|n| n["op"].as_str().unwrap().to_string()).collect()
        };
        let blocks = read(&blocks_path);
        let fine = read(&fine_path);
        assert!(ops(&blocks).contains(&"gap.locate_template".to_string()), "{blocks}");
        assert!(!ops(&blocks).contains(&"gap.fit_line".to_string()), "{blocks}");
        assert!(ops(&blocks).len() <= 12, "{blocks}");
        assert!(ops(&fine).contains(&"gap.fit_line".to_string()), "{fine}");
        assert!(ops(&fine).contains(&"gap.business_rois".to_string()), "{fine}");
        for doc in [&blocks, &fine] {
            assert!(!ops(doc).contains(&"gap.measure_reference".to_string()), "{doc}");
        }
        for p in [&blocks_path, &fine_path] {
            let v = cli(&["validate", p]);
            assert_eq!(v.code, EXIT_OK, "{p}: {}", v.out);
        }

        // 把槽 1 的 datum 框拖到缝右边、target 旁边
        let mut dragged = blocks.clone();
        for n in dragged["nodes"].as_array_mut().unwrap() {
            if n["op"] == "gap.locate_template" {
                n["params"]["template1DatumRoi"] = json!([16, 162, 17.5, 166]);
            }
        }
        let dragged_path = dir.join("dragged.lyflow.json");
        std::fs::write(&dragged_path, dragged.to_string()).unwrap();
        let dragged_path = dragged_path.to_string_lossy().into_owned();
        let v = cli(&["validate", &dragged_path]);
        assert_eq!(v.code, EXIT_INVALID, "{}", v.out);
        let diags = v.first();
        let d = diags
            .as_array()
            .unwrap()
            .iter()
            .find(|d| d["severity"] == "error")
            .unwrap()
            .clone();
        assert_eq!(d["code"], "bad_param", "{diags}");
        assert_eq!(d["phase"], "validate", "{diags}");
        assert_eq!(d["nodeId"], "n_locate", "{diags}");
        assert_eq!(d["paramPath"], "template1DatumRoi", "{diags}");
        let run = cli(&["run", &dragged_path]);
        assert_eq!(run.code, EXIT_INVALID, "{}", run.err);
        assert!(
            !run.lines().iter().any(|e| e["kind"] == "node_state" || e["kind"] == "run_started"),
            "校验没过就不该起跑：{}",
            run.out
        );
    }
}
