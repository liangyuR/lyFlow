//! Tauri commands —— 前端能调到的全部东西。错误一律是 String：
//! 前端拿到错误只显示给人看，不做程序化分支。

use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};
use std::sync::RwLock;
use tauri::Manager;

use crate::core_ffi;
use crate::execution::{encode_cloud, PreviewOptions, RunManager};
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

/// 权威校验（C++ 侧）。返回全部诊断，不是第一条（D5）。
#[tauri::command]
pub fn validate_graph(
    doc: GraphDoc,
    #[allow(non_snake_case)] graphPath: Option<String>,
) -> Result<serde_json::Value, String> {
    let core = core_ffi::core()?;
    let json = serde_json::to_string(&doc).map_err(|e| e.to_string())?;
    let raw = core
        .validate(&json, &base_dir_of(graphPath))
        .map_err(|e| e.to_string())?;
    serde_json::from_str(&raw).map_err(|e| format!("core 返回的诊断不是合法 JSON: {e}"))
}

/// 启动一次运行。立刻返回 run id，状态通过 `execution-event` 事件流推。
/// `mode = "preview"` 时源算子的输出先抽稀，结果进独立的缓存命名空间（ADR-0011）。
#[tauri::command]
#[allow(clippy::too_many_arguments)]
pub fn run_graph(
    app: tauri::AppHandle,
    runs: tauri::State<'_, RunManager>,
    doc: GraphDoc,
    #[allow(non_snake_case)] graphPath: Option<String>,
    targets: Option<Vec<String>>,
    mode: Option<String>,
    #[allow(non_snake_case)] previewMaxPoints: Option<u32>,
    #[allow(non_snake_case)] previewBudgetMs: Option<u32>,
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
    runs.start(
        &app,
        core,
        &json,
        &base_dir_of(graphPath),
        &targets.unwrap_or_default(),
        preview,
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
) -> Result<serde_json::Value, String> {
    let core = core_ffi::core()?;
    let json = serde_json::to_string(&doc).map_err(|e| e.to_string())?;
    let raw = core
        .plan(&json, &base_dir_of(graphPath), &targets.unwrap_or_default())
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

/// 重扫一遍库目录。调用方必须保证没有活跃 run —— 它会重建注册表。
pub fn rescan_library(app: &tauri::AppHandle) -> Result<LibraryStatus, String> {
    let dirs = library_dirs(app)?;
    let core = core_ffi::core()?;
    let problems = core.set_library_dirs(&dirs).map_err(|e| e.to_string())?;
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
    runs.drop_all();
    let status = rescan_library(&app)?;
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

    runs.drop_all();
    rescan_library(&app)
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

#[cfg(test)]
mod tests {
    use super::*;

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

        let plan = plan_graph(doc.clone(), None, None).expect("plan_graph 失败");
        let nodes = plan.as_array().expect("计划不是数组");
        assert_eq!(nodes.len(), 2);
        assert_eq!(nodes[0]["nodeId"], "g");
        assert_eq!(nodes[0]["level"], 0);
        assert_eq!(nodes[1]["level"], 1);
        assert_eq!(nodes[0]["cached"], false);
        assert_eq!(nodes[0]["cacheKey"].as_str().unwrap().len(), 32);
        // 同一张图两次编译必须给出同一批 cacheKey，否则 stale 标记会自己闪
        let again = plan_graph(doc, None, None).unwrap();
        assert_eq!(again, plan);
    }

    #[test]
    fn plan_graph_returns_diagnostics_when_validation_fails() {
        let doc = graph_with(
            serde_json::json!([{"id": "a", "op": "no.such.op"}]),
            serde_json::json!([]),
        );
        let out = plan_graph(doc, None, None).unwrap();
        let items = out.as_array().unwrap();
        assert_eq!(items[0]["kind"], "diagnostic");
        assert_eq!(items[0]["code"], "unknown_op");
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
}
