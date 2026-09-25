//! Tauri commands —— 前端能调到的全部东西。错误一律是 String：
//! 前端拿到错误只显示给人看，不做程序化分支。

use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};
use std::sync::{Mutex, RwLock};
use tauri::Manager;

use crate::core_ffi;
use crate::execution::{
    encode_cloud, encode_indices, encode_tensor, PreviewOptions, RunManager, StartOptions,
};
use crate::graph::GraphDoc;

/// 解析过的 manifest，连同它属于第几代 core。热重载换代后这份要作废（ADR-0009）。
static MANIFEST: RwLock<Option<(u32, serde_json::Value)>> = RwLock::new(None);

#[derive(Serialize)]
pub struct CoreInfo {
    pub version: String,
    #[serde(rename = "operatorCount")]
    pub operator_count: usize,
    #[serde(rename = "typeCount")]
    pub type_count: usize,
    /// 热重载换了几代。前端拿它判断 manifest 要不要重取。
    pub generation: u32,
    /// 开发期才是 true：安装包里没有可盯的 CMake 产物。
    #[serde(rename = "hotReload")]
    pub hot_reload: bool,
}

/// `load_graph` 的回包。迁移动作由前端以语义化动作写回 doc（ADR-0008）——
/// 桥接层不改图，C++ 不拥有文档，诊断是两者之间唯一干净的通道。
#[derive(Serialize)]
pub struct LoadedGraph {
    pub doc: GraphDoc,
    pub migrations: Vec<serde_json::Value>,
}

fn manifest_value() -> Result<serde_json::Value, String> {
    let generation = core_ffi::generation();
    if let Some((cached, value)) = MANIFEST.read().unwrap_or_else(|e| e.into_inner()).as_ref() {
        if *cached == generation {
            return Ok(value.clone());
        }
    }
    let raw = core_ffi::manifest_json()?;
    // 在边界上解析一次，等于顺手验证了 C++ 那个手写 JSON writer 的输出。
    // 它坏掉的话应该在这里炸，而不是让前端拿到半截 JSON 去猜。
    let value: serde_json::Value = serde_json::from_str(&raw)
        .map_err(|e| format!("core 返回的 manifest 不是合法 JSON: {e}"))?;
    *MANIFEST.write().unwrap_or_else(|e| e.into_inner()) = Some((generation, value.clone()));
    Ok(value)
}

#[tauri::command]
pub fn get_manifest() -> Result<serde_json::Value, String> {
    manifest_value()
}

#[tauri::command]
pub fn get_core_info() -> Result<CoreInfo, String> {
    let m = manifest_value()?;
    Ok(CoreInfo {
        version: core_ffi::version(),
        operator_count: m["operators"].as_array().map_or(0, Vec::len),
        type_count: m["types"].as_array().map_or(0, Vec::len),
        generation: core_ffi::generation(),
        #[allow(clippy::redundant_closure_for_method_calls)]
        hot_reload: core_ffi::watch_source().is_some(),
    })
}

#[tauri::command]
pub fn save_graph(path: String, doc: GraphDoc) -> Result<(), String> {
    doc.validate_structure().map_err(|e| e.to_string())?;

    let path = PathBuf::from(path);
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent).map_err(|e| format!("创建目录失败: {e}"))?;
    }
    // pretty 两空格 + 结尾换行：图文件是要进 git 的，diff 必须可读。
    let mut text = serde_json::to_string_pretty(&doc).map_err(|e| e.to_string())?;
    text.push('\n');
    std::fs::write(&path, text).map_err(|e| format!("写入 {} 失败: {e}", path.display()))
}

#[tauri::command]
pub fn load_graph(path: String) -> Result<LoadedGraph, String> {
    let text = std::fs::read_to_string(&path).map_err(|e| format!("读取 {path} 失败: {e}"))?;
    let doc: GraphDoc =
        serde_json::from_str(&text).map_err(|e| format!("{path} 不是合法的 GraphDoc: {e}"))?;
    doc.validate_structure().map_err(|e| e.to_string())?;
    let migrations = migrations_of(&doc, Some(path))?;
    Ok(LoadedGraph { doc, migrations })
}

/// 走一遍 C++ 的 validate，把 kind=migration 的诊断挑出来。
/// 别名重定向与主版本迁移都在里面，前端只管把它们写回 doc。
fn migrations_of(doc: &GraphDoc, path: Option<String>) -> Result<Vec<serde_json::Value>, String> {
    let core = core_ffi::core()?;
    let json = serde_json::to_string(doc).map_err(|e| e.to_string())?;
    let raw = core
        .validate(&json, &base_dir_of(path))
        .map_err(|e| e.to_string())?;
    let diags: Vec<serde_json::Value> =
        serde_json::from_str(&raw).map_err(|e| format!("core 返回的诊断不是合法 JSON: {e}"))?;
    Ok(diags
        .into_iter()
        .filter(|d| d["kind"] == "migration")
        .collect())
}

// --------------------------------------------------------------------- 执行

/// 图文件所在目录，相对路径参数靠它解析。图没保存时为空 —— 不替用户猜一个目录，
/// 猜错的表现是「读到了另一个文件夹里的同名 pcd」，比直接报错难查。
fn base_dir_of(graph_path: Option<String>) -> String {
    graph_path
        .and_then(|p| {
            PathBuf::from(p)
                .parent()
                .map(|d| d.to_string_lossy().into_owned())
        })
        .unwrap_or_default()
}

/// 顶层图参数的取值（编辑器合成的「default + 当前配方覆盖」，param-recipe K3）→ C ABI 的
/// `params_json`。没给或是空对象都当作「全用 default」，与老调用方逐字节同一条路。
pub(crate) fn params_json_of(
    params: Option<&serde_json::Map<String, serde_json::Value>>,
) -> Result<Option<String>, String> {
    match params {
        Some(p) if !p.is_empty() => serde_json::to_string(p).map(Some).map_err(|e| e.to_string()),
        _ => Ok(None),
    }
}

/// 权威校验（C++ 侧）。返回全部诊断，不是第一条（D5）。
/// `params` 是这次要校验的图参数取值（K5：诊断反映的是当前配方的值）。
#[tauri::command]
pub fn validate_graph(
    doc: GraphDoc,
    #[allow(non_snake_case)] graphPath: Option<String>,
    params: Option<serde_json::Map<String, serde_json::Value>>,
) -> Result<serde_json::Value, String> {
    let core = core_ffi::core()?;
    let json = serde_json::to_string(&doc).map_err(|e| e.to_string())?;
    let params_json = params_json_of(params.as_ref())?;
    let raw = core
        .validate_with_params(&json, &base_dir_of(graphPath), params_json.as_deref())
        .map_err(|e| e.to_string())?;
    serde_json::from_str(&raw).map_err(|e| format!("core 返回的诊断不是合法 JSON: {e}"))
}

/// 启动一次运行。立刻返回 run id，状态通过 `execution-event` 事件流推。
/// `mode = "preview"` 时源算子的输出先抽稀，结果进独立的缓存命名空间（ADR-0011）。
/// `isolate` 非空是单节点运行（docs/node-run-plan.md R1–R2）：上游只取缓存。
/// `force` 里的节点跳过缓存强制重算（修订一 V1），可与 targets / isolate / preview 组合。
#[tauri::command]
#[allow(clippy::too_many_arguments)]
pub fn run_graph(
    app: tauri::AppHandle,
    runs: tauri::State<'_, RunManager>,
    doc: GraphDoc,
    #[allow(non_snake_case)] graphPath: Option<String>,
    targets: Option<Vec<String>>,
    isolate: Option<Vec<String>>,
    force: Option<Vec<String>>,
    mode: Option<String>,
    #[allow(non_snake_case)] previewMaxPoints: Option<u32>,
    #[allow(non_snake_case)] previewBudgetMs: Option<u32>,
    params: Option<serde_json::Map<String, serde_json::Value>>,
) -> Result<String, String> {
    // 结构校验挡在前面：C++ 也会查一遍，但那要等到事件流里才看得见，
    // 而一个悬空的边根本不该走到执行器。
    doc.validate_structure().map_err(|e| e.to_string())?;
    let core = core_ffi::core()?;
    let json = serde_json::to_string(&doc).map_err(|e| e.to_string())?;
    let preview = if mode.as_deref() == Some("preview") {
        Some(PreviewOptions {
            max_points: previewMaxPoints.unwrap_or(0),
            budget_ms: previewBudgetMs.unwrap_or(0),
        })
    } else {
        None
    };
    let targets = targets.unwrap_or_default();
    let isolate = isolate.unwrap_or_default();
    let force = force.unwrap_or_default();
    let params_json = params_json_of(params.as_ref())?;
    runs.start(
        &app,
        core,
        &json,
        &base_dir_of(graphPath),
        StartOptions {
            targets: &targets,
            isolate: &isolate,
            force: &force,
            preview,
            params_json: params_json.as_deref(),
        },
    )
}

#[tauri::command]
pub fn cancel_run(runs: tauri::State<'_, RunManager>, #[allow(non_snake_case)] runId: String) {
    runs.cancel(&runId);
}

/// 编译一次但不执行，报告每节点的 cacheKey 与是否已缓存（ADR-0007）。
/// 前端的 stale 标记只读它的结论 —— 自己推一定会在 IO 算子上错。
#[tauri::command]
pub fn plan_graph(
    doc: GraphDoc,
    #[allow(non_snake_case)] graphPath: Option<String>,
    targets: Option<Vec<String>>,
    params: Option<serde_json::Map<String, serde_json::Value>>,
) -> Result<serde_json::Value, String> {
    let core = core_ffi::core()?;
    let json = serde_json::to_string(&doc).map_err(|e| e.to_string())?;
    let params_json = params_json_of(params.as_ref())?;
    let raw = core
        .plan_with_params(
            &json,
            &base_dir_of(graphPath),
            &targets.unwrap_or_default(),
            params_json.as_deref(),
        )
        .map_err(|e| e.to_string())?;
    serde_json::from_str(&raw).map_err(|e| format!("core 返回的计划不是合法 JSON: {e}"))
}

#[tauri::command]
pub fn clear_cache() -> Result<(), String> {
    core_ffi::core()?.cache_clear();
    Ok(())
}

#[tauri::command]
pub fn cache_stats() -> Result<serde_json::Value, String> {
    let raw = core_ffi::core()?.cache_stats().map_err(|e| e.to_string())?;
    serde_json::from_str(&raw).map_err(|e| format!("core 返回的缓存统计不是合法 JSON: {e}"))
}

/// 某节点全部输出的 { port, type, elementCount, byteSize }。
#[tauri::command]
pub fn get_output_info(
    #[allow(non_snake_case)] runId: String,
    #[allow(non_snake_case)] nodeId: String,
) -> Result<serde_json::Value, String> {
    let core = core_ffi::core()?;
    let raw = core
        .output_info(&runId, &nodeId)
        .map_err(|e| e.to_string())?;
    serde_json::from_str(&raw).map_err(|e| format!("core 返回的输出信息不是合法 JSON: {e}"))
}

/// 图级命名输出（ADR-0017）。`{ 名字: { node, port, type, elementCount, byteSize, value? } }`。
#[tauri::command]
pub fn get_run_outputs(
    #[allow(non_snake_case)] runId: String,
) -> Result<serde_json::Value, String> {
    let core = core_ffi::core()?;
    let raw = core.run_outputs(&runId).map_err(|e| e.to_string())?;
    serde_json::from_str(&raw).map_err(|e| format!("core 返回的图输出不是合法 JSON: {e}"))
}

/// 走注册好的导入器把一段文本变成图（ADR-0017）。可用的 kind 见 manifest 的 importers。
#[tauri::command]
pub fn import_graph(
    kind: String,
    text: String,
    #[allow(non_snake_case)] baseDir: Option<String>,
) -> Result<GraphDoc, String> {
    let core = core_ffi::core()?;
    let raw = core
        .import(&kind, &text, baseDir.as_deref().unwrap_or(""))
        .map_err(|diags| format!("导入失败: {diags}"))?;
    serde_json::from_str(&raw).map_err(|e| format!("导入器产出的不是合法 GraphDoc: {e}"))
}

/// 点云走二进制，绝不 JSON（ADR-0006）。布局见 `execution::encode_cloud`。
#[tauri::command]
pub fn get_output_cloud(
    #[allow(non_snake_case)] runId: String,
    #[allow(non_snake_case)] nodeId: String,
    port: String,
    #[allow(non_snake_case)] maxPoints: Option<u32>,
) -> Result<tauri::ipc::Response, String> {
    let core = core_ffi::core()?;
    let view = core
        .output_cloud(&runId, &nodeId, &port, maxPoints.unwrap_or(2_000_000))
        .map_err(|e| e.to_string())?;
    Ok(tauri::ipc::Response::new(encode_cloud(&view)))
}

/// 读一个磁盘上的点云文件（不属于任何一次运行）。编辑器的 2D 拖框要把模板云画在框底下
/// （m8-plan L15），而那时 locate_template 多半还没跑过 —— 框没填好它根本过不了校验。
/// 相对路径按图文件所在目录解析，与图里的 path 参数同一口径。布局同 `get_output_cloud`。
#[tauri::command]
pub fn load_cloud_file(
    path: String,
    #[allow(non_snake_case)] graphPath: Option<String>,
    #[allow(non_snake_case)] maxPoints: Option<u32>,
) -> Result<tauri::ipc::Response, String> {
    let raw = PathBuf::from(&path);
    let file = if raw.is_absolute() {
        raw
    } else {
        let base = base_dir_of(graphPath);
        if base.is_empty() {
            return Err(format!("{path} 是相对路径，图还没保存，不知道相对哪个目录"));
        }
        Path::new(&base).join(raw)
    };
    let core = core_ffi::core()?;
    let view = crate::cli::read_cloud_file(&core, &file, maxPoints.unwrap_or(500_000))?;
    Ok(tauri::ipc::Response::new(encode_cloud(&view)))
}

/// 片段库里用户自己的那一部分（m8-plan L14）：app data 下的 `snippets/` 与 `LYFLOW_SNIPPET_DIRS`
/// （分号分隔）里的每个 `*.lyflow-snippet.json`。算子包随附的片段在 manifest 的 snippets 段，不在这里。
#[derive(Serialize)]
pub struct SnippetScan {
    pub dirs: Vec<String>,
    pub snippets: Vec<serde_json::Value>,
    pub problems: Vec<String>,
}

pub fn snippet_dirs(app: &tauri::AppHandle) -> Result<Vec<String>, String> {
    let dir = app
        .path()
        .app_data_dir()
        .map_err(|e| format!("拿不到 app data 目录: {e}"))?
        .join("snippets");
    let mut dirs = vec![dir.to_string_lossy().into_owned()];
    if let Ok(extra) = std::env::var("LYFLOW_SNIPPET_DIRS") {
        for d in extra.split(';').filter(|d| !d.is_empty()) {
            dirs.push(d.to_string());
        }
    }
    Ok(dirs)
}

/// 扫一组目录。只查文件形状的最低要求（是 JSON 对象、有 id / label / nodes）；
/// 算子缺失这类问题由编辑器在插入时按当前 manifest 说清楚。
pub fn scan_snippets(dirs: &[String]) -> SnippetScan {
    let mut snippets = Vec::new();
    let mut problems = Vec::new();
    for dir in dirs {
        let Ok(entries) = std::fs::read_dir(dir) else { continue };
        let mut files: Vec<PathBuf> = entries
            .flatten()
            .map(|e| e.path())
            .filter(|p| p.to_string_lossy().ends_with(".lyflow-snippet.json"))
            .collect();
        files.sort();
        for file in files {
            let parsed = std::fs::read_to_string(&file)
                .map_err(|e| e.to_string())
                .and_then(|t| serde_json::from_str::<serde_json::Value>(&t).map_err(|e| e.to_string()));
            match parsed {
                Ok(mut v)
                    if v["id"].is_string() && v["label"].is_string() && v["nodes"].is_array() =>
                {
                    v["source"] = serde_json::json!(file.to_string_lossy());
                    snippets.push(v);
                }
                Ok(_) => problems.push(format!("{}：缺 id / label / nodes", file.display())),
                Err(e) => problems.push(format!("{}：{e}", file.display())),
            }
        }
    }
    SnippetScan {
        dirs: dirs.to_vec(),
        snippets,
        problems,
    }
}

#[tauri::command]
pub fn list_snippets(app: tauri::AppHandle) -> Result<SnippetScan, String> {
    Ok(scan_snippets(&snippet_dirs(&app)?))
}

const SLICE_LIMIT: u32 = 4_194_304;

fn clamp_slice(count: Option<u32>) -> u32 {
    match count {
        Some(0) | None => SLICE_LIMIT,
        Some(n) => n.min(SLICE_LIMIT),
    }
}

#[tauri::command]
pub fn get_output_tensor(
    #[allow(non_snake_case)] runId: String,
    #[allow(non_snake_case)] nodeId: String,
    port: String,
    offset: Option<u64>,
    count: Option<u32>,
) -> Result<tauri::ipc::Response, String> {
    let core = core_ffi::core()?;
    let view = core
        .output_tensor(&runId, &nodeId, &port, offset.unwrap_or(0), clamp_slice(count))
        .map_err(|e| e.to_string())?;
    Ok(tauri::ipc::Response::new(encode_tensor(&view)))
}

#[tauri::command]
pub fn get_output_indices(
    #[allow(non_snake_case)] runId: String,
    #[allow(non_snake_case)] nodeId: String,
    port: String,
    offset: Option<u64>,
    count: Option<u32>,
) -> Result<tauri::ipc::Response, String> {
    let core = core_ffi::core()?;
    let view = core
        .output_indices(&runId, &nodeId, &port, offset.unwrap_or(0), clamp_slice(count))
        .map_err(|e| e.to_string())?;
    Ok(tauri::ipc::Response::new(encode_indices(&view)))
}

/// 写一段二进制到磁盘。3D 视图导出 PNG 与「把结果另存」都走它 ——
/// 装 fs 插件要开一整片 ACL，而前端真正需要的只有「写用户刚选的那个文件」。
#[tauri::command]
pub fn write_file_bytes(path: String, contents: Vec<u8>) -> Result<(), String> {
    let target = PathBuf::from(&path);
    if let Some(parent) = target.parent() {
        std::fs::create_dir_all(parent).map_err(|e| format!("创建目录失败: {e}"))?;
    }
    std::fs::write(&target, contents).map_err(|e| format!("写入 {path} 失败: {e}"))
}

// ---- 库算子目录（ADR-0010）

#[derive(Serialize, Clone)]
pub struct LibraryStatus {
    /// 扫描过的目录，第一个是 app data 下的默认库。
    pub dirs: Vec<String>,
    pub count: usize,
    pub problems: Vec<String>,
}

/// 默认库目录：app data 下的 `library/`。设置里的额外目录排在它后面。
pub fn library_dirs(app: &tauri::AppHandle) -> Result<Vec<String>, String> {
    let dir = app
        .path()
        .app_data_dir()
        .map_err(|e| format!("拿不到 app data 目录: {e}"))?
        .join("library");
    std::fs::create_dir_all(&dir).map_err(|e| format!("创建 {} 失败: {e}", dir.display()))?;
    let mut dirs = vec![dir.to_string_lossy().into_owned()];
    if let Ok(extra) = std::env::var("LYFLOW_LIBRARY_DIRS") {
        for d in extra.split(';').filter(|d| !d.is_empty()) {
            dirs.push(d.to_string());
        }
    }
    Ok(dirs)
}

/// 库目录里每个 `*.lyflow-op.json` 的（路径, 大小, 修改时间），排好序。
type LibraryFingerprint = Vec<(PathBuf, u64, Option<std::time::SystemTime>)>;

/// 上一次重扫时库目录的样子。watcher 拿它认出「这次变动已经扫过了」。
static LAST_SCAN: Mutex<Option<LibraryFingerprint>> = Mutex::new(None);

fn library_fingerprint(dirs: &[String]) -> LibraryFingerprint {
    let mut out = Vec::new();
    for dir in dirs {
        let Ok(entries) = std::fs::read_dir(dir) else {
            continue;
        };
        for entry in entries.flatten() {
            let path = entry.path();
            if !path.to_string_lossy().ends_with(".lyflow-op.json") {
                continue;
            }
            let Ok(meta) = entry.metadata() else {
                continue;
            };
            if meta.is_file() {
                out.push((path, meta.len(), meta.modified().ok()));
            }
        }
    }
    out.sort();
    out
}

/// 库目录自上一次重扫以来有没有变过。存库、手动重扫都是先写盘再扫，
/// 400 ms 之后 watcher 还会为同一次写盘再来一遍 —— 那一遍什么都换不来，
/// 却会把这 400 ms 里刚开跑的 run 停掉。
pub fn library_changed_since_scan(dirs: &[String]) -> bool {
    let now = library_fingerprint(dirs);
    LAST_SCAN.lock().unwrap_or_else(|e| e.into_inner()).as_ref() != Some(&now)
}

/// 重扫一遍库目录。
pub fn rescan_library(app: &tauri::AppHandle, runs: &RunManager) -> Result<LibraryStatus, String> {
    rescan_library_dirs(runs, library_dirs(app)?)
}

/// 重建注册表之前只停活跃的 run，上一次跑完的留着（`RunManager::stop_active`）：
/// 不换 DLL，结果仓里的数据照样有效；drop_all 是热重载的事（ADR-0009）。
pub fn rescan_library_dirs(runs: &RunManager, dirs: Vec<String>) -> Result<LibraryStatus, String> {
    runs.stop_active();
    // 扫之前取：扫的途中文件又变了的话，记下的是旧样子，watcher 会再扫一遍
    let seen = library_fingerprint(&dirs);
    let core = core_ffi::core()?;
    let problems = core.set_library_dirs(&dirs).map_err(|e| e.to_string())?;
    *LAST_SCAN.lock().unwrap_or_else(|e| e.into_inner()) = Some(seen);
    // 算子集合变了，缓存的 manifest 立刻作废
    *MANIFEST.write().unwrap_or_else(|e| e.into_inner()) = None;
    Ok(LibraryStatus {
        dirs,
        count: core.library_count(),
        problems,
    })
}

#[tauri::command]
pub fn get_library_status(app: tauri::AppHandle) -> Result<LibraryStatus, String> {
    let dirs = library_dirs(&app)?;
    let core = core_ffi::core()?;
    Ok(LibraryStatus {
        dirs,
        count: core.library_count(),
        problems: Vec::new(),
    })
}

#[derive(Serialize)]
pub struct LibraryRefresh {
    pub status: LibraryStatus,
    pub manifest: serde_json::Value,
}

#[tauri::command]
pub fn refresh_library(
    app: tauri::AppHandle,
    runs: tauri::State<'_, RunManager>,
) -> Result<LibraryRefresh, String> {
    let status = rescan_library(&app, &runs)?;
    Ok(LibraryRefresh {
        status,
        manifest: manifest_value()?,
    })
}

#[derive(Deserialize)]
pub struct LibraryMeta {
    pub id: String,
    #[serde(default)]
    pub category: String,
    #[serde(default)]
    pub keywords: Vec<String>,
    #[serde(default)]
    pub version: Option<String>,
}

/// 把 doc 里的一个子图存成库文件。文件名就是 `<id>.lyflow-op.json`。
#[tauri::command]
pub fn save_as_library(
    app: tauri::AppHandle,
    runs: tauri::State<'_, RunManager>,
    doc: GraphDoc,
    #[allow(non_snake_case)] subgraphId: String,
    meta: LibraryMeta,
) -> Result<LibraryStatus, String> {
    if meta.id.trim().is_empty() {
        return Err("库算子的 id 不能为空".into());
    }
    if !meta
        .id
        .chars()
        .all(|c| c.is_ascii_alphanumeric() || c == '_' || c == '-' || c == '.')
    {
        return Err("库算子的 id 只能用字母、数字、下划线、短横线和点".into());
    }
    let def = doc
        .subgraphs
        .get(&subgraphId)
        .ok_or_else(|| format!("doc 里没有子图 {subgraphId}"))?
        .clone();
    // 库文件是自包含的：里面再引用别的子图就没人解得开了
    if let Some(nodes) = def.get("nodes").and_then(|n| n.as_array()) {
        for n in nodes {
            if let Some(op) = n.get("op").and_then(|o| o.as_str()) {
                if op.starts_with("sub:") {
                    return Err(format!(
                        "子图里还嵌着 {op}，库文件必须自包含 —— 先把内层也保存到库，或者解散它"
                    ));
                }
            }
        }
    }

    let mut body = def;
    if let Some(obj) = body.as_object_mut() {
        obj.insert("id".into(), serde_json::json!(meta.id));
        obj.insert(
            "version".into(),
            serde_json::json!(meta.version.unwrap_or_else(|| "1.0.0".into())),
        );
        if !meta.category.is_empty() {
            obj.insert("category".into(), serde_json::json!(meta.category));
        }
        if !meta.keywords.is_empty() {
            obj.insert("keywords".into(), serde_json::json!(meta.keywords));
        }
    }

    let dirs = library_dirs(&app)?;
    let root = PathBuf::from(dirs.first().ok_or("没有可写的库目录")?);
    let file = root.join(format!("{}.lyflow-op.json", meta.id));
    let mut text = serde_json::to_string_pretty(&body).map_err(|e| e.to_string())?;
    text.push('\n');
    std::fs::write(&file, text).map_err(|e| format!("写入 {} 失败: {e}", file.display()))?;

    rescan_library(&app, &runs)
}

// ---- 最近文件与备份

/// 备份文件名：`<file>~`。编辑器的老约定，一眼看得出是什么，也不会被
/// `*.lyflow.json` 的通配符扫到。
fn backup_path(path: &str) -> PathBuf {
    PathBuf::from(format!("{path}~"))
}

fn modified_ms(path: &Path) -> Option<u64> {
    let meta = std::fs::metadata(path).ok()?;
    let t = meta.modified().ok()?;
    Some(t.duration_since(std::time::UNIX_EPOCH).ok()?.as_millis() as u64)
}

#[derive(Serialize, Deserialize, Clone)]
pub struct RecentEntry {
    pub path: String,
    /// 毫秒时间戳，前端按它排序与显示。
    #[serde(rename = "openedAt")]
    pub opened_at: u64,
}

fn recent_file(app: &tauri::AppHandle) -> Result<PathBuf, String> {
    let dir = app
        .path()
        .app_data_dir()
        .map_err(|e| format!("拿不到 app data 目录: {e}"))?;
    std::fs::create_dir_all(&dir).map_err(|e| format!("创建 {} 失败: {e}", dir.display()))?;
    Ok(dir.join("recent.json"))
}

/// 最多 10 条。读不出来就当空列表 —— 最近文件坏了不该拦住用户开 app。
#[tauri::command]
pub fn get_recent_files(app: tauri::AppHandle) -> Vec<RecentEntry> {
    let Ok(path) = recent_file(&app) else {
        return Vec::new();
    };
    std::fs::read_to_string(path)
        .ok()
        .and_then(|t| serde_json::from_str::<Vec<RecentEntry>>(&t).ok())
        .unwrap_or_default()
}

#[tauri::command]
pub fn push_recent_file(app: tauri::AppHandle, path: String) -> Result<Vec<RecentEntry>, String> {
    let mut list = get_recent_files(app.clone());
    list.retain(|e| e.path != path);
    let now = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0);
    list.insert(
        0,
        RecentEntry {
            path,
            opened_at: now,
        },
    );
    list.truncate(10);
    let file = recent_file(&app)?;
    std::fs::write(&file, serde_json::to_string_pretty(&list).map_err(|e| e.to_string())?)
        .map_err(|e| format!("写入 {} 失败: {e}", file.display()))?;
    Ok(list)
}

/// 定时备份。只在有文件路径时才有意义 —— 没存过盘的图没有 `<file>~` 可写。
#[tauri::command]
pub fn write_backup(path: String, doc: GraphDoc) -> Result<(), String> {
    let text = serde_json::to_string(&doc).map_err(|e| e.to_string())?;
    let target = backup_path(&path);
    std::fs::write(&target, text).map_err(|e| format!("写入 {} 失败: {e}", target.display()))
}

#[derive(Serialize)]
pub struct BackupStatus {
    pub exists: bool,
    /// 备份比正文新 —— 上次是崩溃或强杀退出的，值得问一句要不要恢复。
    pub newer: bool,
    #[serde(rename = "backupModified")]
    pub backup_modified: Option<u64>,
    #[serde(rename = "fileModified")]
    pub file_modified: Option<u64>,
}

#[tauri::command]
pub fn backup_status(path: String) -> BackupStatus {
    let backup = backup_path(&path);
    let backup_modified = modified_ms(&backup);
    let file_modified = modified_ms(Path::new(&path));
    BackupStatus {
        exists: backup_modified.is_some(),
        newer: match (backup_modified, file_modified) {
            (Some(b), Some(f)) => b > f + 1000,
            (Some(_), None) => true,
            _ => false,
        },
        backup_modified,
        file_modified,
    }
}

#[tauri::command]
pub fn read_backup(path: String) -> Result<LoadedGraph, String> {
    let backup = backup_path(&path);
    load_graph(backup.to_string_lossy().into_owned())
}

#[tauri::command]
pub fn discard_backup(path: String) -> Result<(), String> {
    let backup = backup_path(&path);
    match std::fs::remove_file(&backup) {
        Ok(()) => Ok(()),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(()),
        Err(e) => Err(format!("删除 {} 失败: {e}", backup.display())),
    }
}

// ---- 配方文件（param-recipe P3.2）
//
// 文本进、文本出：配方的格式、失配判定都在编辑器（P4 的 CLI 再在 Rust 里实现一遍），桥接层只管读写
// 与路径约束 —— 前端能调到的只有这几种文件，任意路径读写口不开：
// - 配方目录（名字以 `.recipes` 结尾的目录，图文件旁边的 `<图名>.recipes/`）里的
//   `*.lyflow-recipe.json`、`index.json`、`autosave~.json`：读、写、删；`*.lyflow-recipe.json` 之间改名；
// - 任意位置的 `*.lyflow-recipe.json`：读（导入）、写（导出，路径是用户在对话框里选的）。
// 路径一律要绝对路径、不许有 `..`。

const RECIPE_EXT: &str = ".lyflow-recipe.json";
const RECIPE_INDEX: &str = "index.json";
const RECIPE_AUTOSAVE: &str = "autosave~.json";
const RECIPE_DIR_SUFFIX: &str = ".recipes";

#[derive(Serialize, Debug)]
pub struct RecipeDirEntry {
    pub name: String,
    pub modified: Option<u64>,
}

#[derive(Serialize, Debug)]
pub struct RecipeDirListing {
    pub exists: bool,
    pub files: Vec<RecipeDirEntry>,
}

fn ends_with_ci(s: &str, suffix: &str) -> bool {
    s.len() > suffix.len() && s.to_lowercase().ends_with(suffix)
}

fn is_recipe_file_name(name: &str) -> bool {
    ends_with_ci(name, RECIPE_EXT)
}

/// 目录里除了配方文件之外认的两个：索引与自动备份。
fn is_recipe_aux_name(name: &str) -> bool {
    name.eq_ignore_ascii_case(RECIPE_INDEX) || name.eq_ignore_ascii_case(RECIPE_AUTOSAVE)
}

fn is_recipe_dir(dir: &Path) -> bool {
    dir.file_name()
        .and_then(|n| n.to_str())
        .is_some_and(|n| ends_with_ci(n, RECIPE_DIR_SUFFIX))
}

fn plain_absolute(raw: &str) -> Result<PathBuf, String> {
    let p = PathBuf::from(raw);
    if !p.is_absolute() {
        return Err(format!("配方文件要给绝对路径：{raw}"));
    }
    if p.components().any(|c| matches!(c, std::path::Component::ParentDir)) {
        return Err(format!("路径里不能有 ..：{raw}"));
    }
    Ok(p)
}

#[derive(Clone, Copy, PartialEq)]
enum RecipeAccess {
    /// 读、写：配方目录里的三种文件，或任意位置的配方文件（导入 / 导出）。
    ReadWrite,
    /// 删、改名：只在配方目录里。
    InDir,
}

fn check_recipe_path(raw: &str, access: RecipeAccess) -> Result<PathBuf, String> {
    let p = plain_absolute(raw)?;
    let name = p
        .file_name()
        .and_then(|n| n.to_str())
        .ok_or_else(|| format!("不是文件路径：{raw}"))?;
    let in_dir = p.parent().is_some_and(is_recipe_dir);
    let ok = match access {
        RecipeAccess::ReadWrite => is_recipe_file_name(name) || (in_dir && is_recipe_aux_name(name)),
        RecipeAccess::InDir => in_dir && (is_recipe_file_name(name) || is_recipe_aux_name(name)),
    };
    if ok {
        Ok(p)
    } else {
        Err(format!(
            "这里只能读写配方文件（*{RECIPE_EXT}，或 *{RECIPE_DIR_SUFFIX}/ 里的 {RECIPE_INDEX}、{RECIPE_AUTOSAVE}）：{raw}"
        ))
    }
}

/// 列配方目录。目录还不存在不是错误（图旁边从没建过配方）。
#[tauri::command]
pub fn list_recipe_dir(dir: String) -> Result<RecipeDirListing, String> {
    let p = plain_absolute(&dir)?;
    if !is_recipe_dir(&p) {
        return Err(format!("不是配方目录（名字要以 {RECIPE_DIR_SUFFIX} 结尾）：{dir}"));
    }
    if !p.is_dir() {
        return Ok(RecipeDirListing { exists: false, files: Vec::new() });
    }
    let mut files = Vec::new();
    for entry in std::fs::read_dir(&p).map_err(|e| format!("读目录 {dir} 失败: {e}"))? {
        let entry = entry.map_err(|e| e.to_string())?;
        let Some(name) = entry.file_name().to_str().map(str::to_owned) else { continue };
        if !(is_recipe_file_name(&name) || is_recipe_aux_name(&name)) {
            continue;
        }
        if !entry.file_type().is_ok_and(|t| t.is_file()) {
            continue;
        }
        files.push(RecipeDirEntry { modified: modified_ms(&entry.path()), name });
    }
    files.sort_by(|a, b| a.name.cmp(&b.name));
    Ok(RecipeDirListing { exists: true, files })
}

#[tauri::command]
pub fn read_recipe_file(path: String) -> Result<String, String> {
    let p = check_recipe_path(&path, RecipeAccess::ReadWrite)?;
    std::fs::read_to_string(&p).map_err(|e| format!("读取 {path} 失败: {e}"))
}

/// 写配方文件。先确认是一个 JSON 对象（格式由编辑器定，这里只挡住明显写坏的），
/// 再写到同目录的临时文件、改名覆盖 —— 写到一半断电不会留下半截配方。
#[tauri::command]
pub fn write_recipe_file(path: String, text: String) -> Result<(), String> {
    let p = check_recipe_path(&path, RecipeAccess::ReadWrite)?;
    let value: serde_json::Value =
        serde_json::from_str(&text).map_err(|e| format!("{path} 的内容不是合法 JSON: {e}"))?;
    if !value.is_object() {
        return Err(format!("{path} 的内容应当是一个 JSON 对象"));
    }
    if let Some(parent) = p.parent() {
        std::fs::create_dir_all(parent).map_err(|e| format!("创建目录失败: {e}"))?;
    }
    let tmp = PathBuf::from(format!("{}.tmp~", p.to_string_lossy()));
    std::fs::write(&tmp, text).map_err(|e| format!("写入 {} 失败: {e}", tmp.display()))?;
    std::fs::rename(&tmp, &p).map_err(|e| {
        let _ = std::fs::remove_file(&tmp);
        format!("写入 {path} 失败: {e}")
    })
}

#[tauri::command]
pub fn delete_recipe_file(path: String) -> Result<(), String> {
    let p = check_recipe_path(&path, RecipeAccess::InDir)?;
    match std::fs::remove_file(&p) {
        Ok(()) => Ok(()),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(()),
        Err(e) => Err(format!("删除 {path} 失败: {e}")),
    }
}

/// 同一个配方目录里改名。目标已经存在时拒绝（只差大小写的改名除外：Windows 上那是同一个文件）。
#[tauri::command]
pub fn rename_recipe_file(from: String, to: String) -> Result<(), String> {
    let a = check_recipe_path(&from, RecipeAccess::InDir)?;
    let b = check_recipe_path(&to, RecipeAccess::InDir)?;
    let names_ok = a.file_name().and_then(|n| n.to_str()).is_some_and(is_recipe_file_name)
        && b.file_name().and_then(|n| n.to_str()).is_some_and(is_recipe_file_name);
    if !names_ok {
        return Err("只有配方文件（*.lyflow-recipe.json）能改名".into());
    }
    if a.parent() != b.parent() {
        return Err(format!("改名只能在同一个配方目录里：{from} → {to}"));
    }
    let case_only = from.to_lowercase() == to.to_lowercase();
    if b.exists() && !case_only {
        return Err(format!("{to} 已经存在"));
    }
    std::fs::rename(&a, &b).map_err(|e| format!("改名 {from} → {to} 失败: {e}"))
}

#[cfg(test)]
mod tests {
    use super::*;

    /// watcher 跳过「已经扫过的那次写盘」全靠它：只认库文件，增、删、改都看得出来。
    #[test]
    fn library_fingerprint_sees_add_change_and_delete() {
        let dir = std::env::temp_dir().join(format!("lyflow-lib-fp-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let dirs = vec![dir.to_string_lossy().into_owned()];
        let empty = library_fingerprint(&dirs);
        assert!(empty.is_empty());

        std::fs::write(dir.join("notes.txt"), "x").unwrap();
        assert_eq!(library_fingerprint(&dirs), empty, "不是库文件的不算");

        let file = dir.join("a.lyflow-op.json");
        std::fs::write(&file, "{}").unwrap();
        let one = library_fingerprint(&dirs);
        assert_eq!(one.len(), 1);
        assert_eq!(library_fingerprint(&dirs), one, "没动就一样");

        std::fs::write(&file, "{ }").unwrap();
        assert_ne!(library_fingerprint(&dirs), one, "内容变了");

        std::fs::remove_file(&file).unwrap();
        assert_eq!(library_fingerprint(&dirs), empty, "删掉了");
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn manifest_command_returns_parsed_json() {
        let v = get_manifest().expect("get_manifest 失败");
        assert_eq!(v["schemaVersion"], 1);
        assert!(!v["operators"].as_array().unwrap().is_empty());
    }

    #[test]
    fn core_info_counts_match_the_manifest() {
        let info = get_core_info().expect("get_core_info 失败");
        let m = get_manifest().unwrap();
        assert_eq!(info.operator_count, m["operators"].as_array().unwrap().len());
        assert_eq!(info.type_count, m["types"].as_array().unwrap().len());
        assert!(!info.version.is_empty());
    }

    #[test]
    fn graph_survives_a_save_load_roundtrip() {
        let dir = std::env::temp_dir().join("lyflow-test-roundtrip");
        let path = dir.join("g.lyflow.json");
        let _ = std::fs::remove_file(&path);

        let raw = r#"{
            "schemaVersion": 1,
            "id": "01J8XQZ4K7N3M2R5V8W1YB6TCD",
            "name": "roundtrip",
            "nodes": [
                {"id": "n1", "op": "io.load_pcd", "opVersion": "1.0.0",
                 "params": {"path": "a.pcd"},
                 "ui": {"position": {"x": 0, "y": 0}}},
                {"id": "n2", "op": "filter.voxel_grid", "opVersion": "1.0.0",
                 "params": {"leafSize": [0.005, 0.005, 0.005]},
                 "ui": {"position": {"x": 280, "y": 0}, "title": "粗降采样"}}
            ],
            "edges": [
                {"id": "e1", "from": {"node": "n1", "port": "cloud"},
                             "to": {"node": "n2", "port": "cloud"}}
            ]
        }"#;
        let doc: GraphDoc = serde_json::from_str(raw).unwrap();

        save_graph(path.to_string_lossy().into_owned(), doc).expect("save_graph 失败");
        let loaded = load_graph(path.to_string_lossy().into_owned()).expect("load_graph 失败");
        let back = loaded.doc;

        assert_eq!(back.nodes.len(), 2);
        assert_eq!(back.edges.len(), 1);
        assert_eq!(back.nodes[1].params["leafSize"][0], 0.005);
        // ui 是纯 UI 状态，桥接层原样透传不解释（ADR-0002）
        assert_eq!(back.nodes[1].ui.as_ref().unwrap()["title"], "粗降采样");
        // 两个节点都是当前版本，没有迁移可做
        assert!(loaded.migrations.is_empty(), "{:?}", loaded.migrations);

        // 存盘格式必须是可 diff 的：缩进 + 结尾换行
        let text = std::fs::read_to_string(&path).unwrap();
        assert!(text.contains("\n  \"id\""), "存盘不是 pretty JSON");
        assert!(text.ends_with('\n'), "存盘缺结尾换行");

        let _ = std::fs::remove_dir_all(&dir);
    }

    fn graph_with(nodes: serde_json::Value, edges: serde_json::Value) -> GraphDoc {
        serde_json::from_value(serde_json::json!({
            "schemaVersion": 1, "id": "01J8XQZ4K7N3M2R5V8W1YB6TCD",
            "nodes": nodes, "edges": edges
        }))
        .unwrap()
    }

    /// ADR-0007：stale 标记的权威在 C++。前端只对 cacheKey，不自己推。
    #[test]
    #[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
    fn plan_graph_reports_cache_keys_and_levels() {
        // seed 77 是本测试专属：靠它拿到 cached=false，不能清进程级缓存 ——
        // 那会把并行跑着的别的测试刚放进结果仓的结果一并清掉。
        let doc = graph_with(
            serde_json::json!([
                {"id": "g", "op": "gen.synthetic", "params": {"pointCount": 3000, "seed": 77}},
                {"id": "v", "op": "filter.voxel_grid"}
            ]),
            serde_json::json!([
                {"id": "e", "from": {"node": "g", "port": "cloud"},
                            "to": {"node": "v", "port": "cloud"}}
            ]),
        );

        let plan = plan_graph(doc.clone(), None, None, None).expect("plan_graph 失败");
        let nodes = plan.as_array().expect("计划不是数组");
        assert_eq!(nodes.len(), 2);
        assert_eq!(nodes[0]["nodeId"], "g");
        assert_eq!(nodes[0]["level"], 0);
        assert_eq!(nodes[1]["level"], 1);
        assert_eq!(nodes[0]["cached"], false);
        assert_eq!(nodes[0]["cacheKey"].as_str().unwrap().len(), 32);
        // 同一张图两次编译必须给出同一批 cacheKey，否则 stale 标记会自己闪
        let again = plan_graph(doc, None, None, None).unwrap();
        assert_eq!(again, plan);
    }

    #[test]
    fn plan_graph_returns_diagnostics_when_validation_fails() {
        let doc = graph_with(
            serde_json::json!([{"id": "a", "op": "no.such.op"}]),
            serde_json::json!([]),
        );
        let out = plan_graph(doc, None, None, None).unwrap();
        let items = out.as_array().unwrap();
        assert_eq!(items[0]["kind"], "diagnostic");
        assert_eq!(items[0]["code"], "unknown_op");
    }

    /// param-recipe P1 验收 1：带完整规格的图参数、老格式的图参数，存盘再读回一个字段都不丢，
    /// 键顺序也不变（存盘是给人 diff 的）。夹具就是 pnpm check 对着 schema 校验的那一份。
    #[test]
    #[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
    fn graph_params_full_spec_survives_a_save_load_roundtrip() {
        let fixture = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../schema/examples/graph-params.example.lyflow.json");
        let raw = std::fs::read_to_string(&fixture).expect("读不到图参数样例");
        let original: serde_json::Value = serde_json::from_str(&raw).unwrap();
        let doc: GraphDoc = serde_json::from_str(&raw).unwrap();

        let dir = std::env::temp_dir().join(format!("lyflow-test-graph-params-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("g.lyflow.json");
        save_graph(path.to_string_lossy().into_owned(), doc).expect("save_graph 失败");
        let loaded = load_graph(path.to_string_lossy().into_owned()).expect("load_graph 失败");
        let back = serde_json::to_value(&loaded.doc).unwrap();

        assert_eq!(back["params"], original["params"], "图参数有字段丢了或变了");
        let keys = |v: &serde_json::Value| -> Vec<String> {
            v.as_object().unwrap().keys().cloned().collect()
        };
        assert_eq!(keys(&back["params"]), keys(&original["params"]));
        assert_eq!(keys(&back["params"]["leafSize"]), keys(&original["params"]["leafSize"]));
        // 规格字段逐个在：label、限位、单位、分组、分量名、options、placeholder
        let leaf = &back["params"]["leafSize"];
        for key in ["label", "min", "max", "softMin", "softMax", "step", "unit", "group", "componentLabels"] {
            assert!(leaf.get(key).is_some(), "leafSize 丢了 {key}");
        }
        assert_eq!(back["params"]["cutField"]["options"][2]["doc"], "竖直方向");
        assert_eq!(back["params"]["pointCount"]["placeholder"], "点数");
        // 老格式那一条仍是老样子
        assert!(back["params"]["cutMax"].get("type").is_none());

        // core 认这份图：完整规格与老格式都能过校验
        let diags = validate_graph(loaded.doc, None, None).unwrap();
        assert_eq!(diags, serde_json::json!([]), "{diags}");
        let _ = std::fs::remove_dir_all(&dir);
    }

    /// param-recipe P1.5 / K5：validate 与 plan 带上编辑器合成的图参数取值，经 C ABI 的
    /// params_json 交给 core —— 诊断与 cacheKey 反映的是这组值，不是 doc 里的 default。
    #[test]
    #[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
    fn validate_and_plan_carry_graph_param_values() {
        let mut doc = graph_with(
            serde_json::json!([
                {"id": "g", "op": "gen.synthetic", "params": {"seed": 7711}},
                {"id": "v", "op": "filter.voxel_grid"}
            ]),
            serde_json::json!([
                {"id": "e", "from": {"node": "g", "port": "cloud"},
                            "to": {"node": "v", "port": "cloud"}}
            ]),
        );
        doc.params = serde_json::from_value(serde_json::json!({
            "count": {"type": "int", "label": "点数", "min": 10, "max": 5000,
                      "default": 3000, "binds": ["g.pointCount"]}
        }))
        .unwrap();
        let values = |v: serde_json::Value| -> serde_json::Map<String, serde_json::Value> {
            serde_json::from_value(v).unwrap()
        };

        assert_eq!(validate_graph(doc.clone(), None, None).unwrap(), serde_json::json!([]));
        let bad = validate_graph(doc.clone(), None, Some(values(serde_json::json!({"count": 9000}))))
            .unwrap();
        let d = &bad.as_array().unwrap()[0];
        assert_eq!(d["code"], "bad_param", "{bad}");
        assert_eq!(d["paramPath"], "count");
        assert!(d.get("nodeId").map_or(true, |n| n == "" || n.is_null()), "{d}");
        // 空对象 = 全用 default，与不给一样
        assert_eq!(
            validate_graph(doc.clone(), None, Some(values(serde_json::json!({})))).unwrap(),
            serde_json::json!([])
        );

        let key_of = |plan: &serde_json::Value, id: &str| -> String {
            plan.as_array().unwrap().iter().find(|n| n["nodeId"] == id).unwrap()["cacheKey"]
                .as_str()
                .unwrap()
                .to_string()
        };
        let base = plan_graph(doc.clone(), None, None, None).unwrap();
        let same = plan_graph(doc.clone(), None, None, Some(values(serde_json::json!({"count": 3000}))))
            .unwrap();
        let other = plan_graph(doc.clone(), None, None, Some(values(serde_json::json!({"count": 2500}))))
            .unwrap();
        assert_eq!(key_of(&base, "g"), key_of(&same, "g"), "传回 default 键不变");
        assert_ne!(key_of(&base, "g"), key_of(&other, "g"), "被绑定节点的键跟着值变");
        assert_ne!(key_of(&base, "v"), key_of(&other, "v"), "下游跟着变");
        // 越界值连计划都编不出来：返回诊断数组
        let blocked = plan_graph(doc, None, None, Some(values(serde_json::json!({"count": 1}))))
            .unwrap();
        assert_eq!(blocked[0]["code"], "bad_param");
    }

    #[test]
    fn cache_stats_has_the_documented_shape() {
        let stats = cache_stats().expect("cache_stats 失败");
        for key in ["entries", "bytes", "budgetBytes", "hits", "misses", "evictions"] {
            assert!(stats[key].is_number(), "{key} 不是数字: {stats}");
        }
        assert!(stats["budgetBytes"].as_u64().unwrap() > 0);
    }

    /// ADR-0008：C++ 出诊断，前端写回。桥接层只负责把迁移动作从诊断里挑出来。
    #[test]
    #[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
    fn load_graph_returns_migrations_for_an_old_document() {
        let dir = std::env::temp_dir().join("lyflow-test-migration");
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("old.lyflow.json");
        std::fs::write(
            &path,
            r#"{
                "schemaVersion": 1,
                "id": "01J8XQZ4K7N3M2R5V8W1YB6TCD",
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
            }"#,
        )
        .unwrap();

        let loaded = load_graph(path.to_string_lossy().into_owned()).expect("load_graph 失败");
        assert_eq!(loaded.doc.nodes.len(), 2);
        assert_eq!(loaded.migrations.len(), 1, "{:?}", loaded.migrations);
        let m = &loaded.migrations[0];
        assert_eq!(m["kind"], "migration");
        assert_eq!(m["nodeId"], "s");
        assert_eq!(m["op"], "filter.random_sample");
        assert_eq!(m["opVersion"], "2.0.0");
        assert_eq!(m["params"]["keepCount"], 250);
        assert!(m["params"].get("count").is_none());
        // 文档本身没被改写：写回 doc 是前端的事，桥接层不改图
        assert_eq!(loaded.doc.nodes[1].params["count"], 250);

        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn save_rejects_a_structurally_broken_graph() {
        let raw = r#"{
            "schemaVersion": 1, "id": "bad",
            "nodes": [{"id": "n1", "op": "io.load_pcd"}],
            "edges": [{"id": "e1", "from": {"node": "n1", "port": "cloud"},
                                   "to": {"node": "ghost", "port": "cloud"}}]
        }"#;
        let doc: GraphDoc = serde_json::from_str(raw).unwrap();
        let path = std::env::temp_dir().join("lyflow-should-not-exist.json");
        let _ = std::fs::remove_file(&path);

        let err = save_graph(path.to_string_lossy().into_owned(), doc).unwrap_err();
        assert!(err.contains("目标节点不存在"), "{err}");
        assert!(!path.exists(), "校验失败却还是写盘了");
    }

    /// 配方文件的五个命令（param-recipe P3.2）：在中文路径的配方目录里走一遍列、写、读、改名、删。
    #[test]
    fn recipe_files_list_write_read_rename_delete() {
        let root = std::env::temp_dir().join(format!("lyflow 配方 {}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        let dir = root.join("车门.recipes");
        let s = |p: &Path| p.to_string_lossy().into_owned();

        // 目录还不存在：不是错误
        let listing = list_recipe_dir(s(&dir)).unwrap();
        assert!(!listing.exists && listing.files.is_empty());

        let a = dir.join("车型A·左前门.lyflow-recipe.json");
        write_recipe_file(s(&a), "{\"schemaVersion\":1,\"name\":\"车型A·左前门\",\"values\":{}}\n".into())
            .expect("写配方失败");
        write_recipe_file(s(&dir.join("index.json")), "{\"order\":[\"车型A·左前门\"]}\n".into()).unwrap();
        std::fs::write(dir.join("别的文件.txt"), "x").unwrap();
        let names: Vec<String> = list_recipe_dir(s(&dir)).unwrap().files.into_iter().map(|f| f.name).collect();
        assert_eq!(names, vec!["index.json".to_string(), "车型A·左前门.lyflow-recipe.json".to_string()]);
        assert!(read_recipe_file(s(&a)).unwrap().contains("车型A·左前门"));
        // 没留下临时文件
        assert!(!dir.join("车型A·左前门.lyflow-recipe.json.tmp~").exists());

        let b = dir.join("车型B.lyflow-recipe.json");
        rename_recipe_file(s(&a), s(&b)).expect("改名失败");
        assert!(!a.exists() && b.exists());
        // 目标已存在：拒绝
        write_recipe_file(s(&a), "{}".into()).unwrap();
        assert!(rename_recipe_file(s(&a), s(&b)).unwrap_err().contains("已经存在"));
        // 只差大小写的改名放行
        let upper = dir.join("车型b.lyflow-recipe.json");
        rename_recipe_file(s(&b), s(&upper)).expect("大小写改名失败");

        delete_recipe_file(s(&a)).unwrap();
        delete_recipe_file(s(&a)).expect("删一个不存在的文件不算错");
        assert!(!a.exists());
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn recipe_paths_outside_the_rules_are_refused() {
        let root = std::env::temp_dir().join(format!("lyflow-recipe-guard-{}", std::process::id()));
        let dir = root.join("g.recipes");
        let s = |p: &Path| p.to_string_lossy().into_owned();
        // 不是配方文件
        assert!(write_recipe_file(s(&dir.join("evil.json")), "{}".into()).is_err());
        assert!(read_recipe_file(s(&root.join("secret.txt"))).is_err());
        // index.json 只认配方目录里的
        assert!(write_recipe_file(s(&root.join("index.json")), "{}".into()).is_err());
        // 相对路径与 ..
        assert!(read_recipe_file("a.lyflow-recipe.json".into()).is_err());
        assert!(write_recipe_file(s(&dir.join("..").join("x.lyflow-recipe.json")), "{}".into()).is_err());
        // 删与改名只在配方目录里
        assert!(delete_recipe_file(s(&root.join("x.lyflow-recipe.json"))).is_err());
        assert!(rename_recipe_file(s(&dir.join("a.lyflow-recipe.json")), s(&root.join("b.lyflow-recipe.json"))).is_err());
        assert!(list_recipe_dir(s(&root)).is_err());
        // 内容不是 JSON 对象
        assert!(write_recipe_file(s(&dir.join("a.lyflow-recipe.json")), "[1,2]".into()).is_err());
        assert!(write_recipe_file(s(&dir.join("a.lyflow-recipe.json")), "not json".into()).is_err());
        // 导出：任意位置的配方文件能写
        let out = root.join("导出").join("x.lyflow-recipe.json");
        write_recipe_file(s(&out), "{}".into()).expect("导出失败");
        assert!(out.exists());
        let _ = std::fs::remove_dir_all(&root);
    }
}
