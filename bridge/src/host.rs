//! 把整个 IPC 层交给外部 Rust/Tauri 宿主（docs/embedding.md「Rust/Tauri 宿主」）。
//!
//! 宿主自己建 `tauri::Builder`、自己开窗、自己注册自己的命令；LyFlow 这边出
//! `attach()`（managed state + 启动步骤）与 `lyflow_handler!`（35 条命令的清单）。
//! LyFlow 自己的 `run()` 走的是同一对东西 —— 分叉的实现会长出只在其中一边出现的 bug。

use std::collections::BTreeMap;
use std::path::{Component, Path, PathBuf};
use std::sync::Arc;

use crate::core_ffi::{self, RunInput};
use crate::graph::GraphDoc;

/// 一片要注入的点云。`intensity` 可以是空的，其余通道（法线、RGB）目前用不上，
/// 真需要时直接给 `core_ffi::RunInput` 就是了。
#[derive(Clone, Debug, Default)]
pub struct Cloud {
    /// 交错的 x,y,z。
    pub xyz: Vec<f32>,
    /// 空或与点数等长。
    pub intensity: Vec<f32>,
}

/// 宿主持有的点云会话表。`run_graph` 拿到 `sceneId` 时问它要注入项。
///
/// 实现通常就是「查会话表 → 调 [`inputs_for_source_op`]」：按**算子 id**而不是
/// 节点 id 找源节点，用户在编辑器里怎么重命名节点都不影响产线（ADR-0017 的动机）。
pub trait SceneProvider: Send + Sync {
    /// 把宿主持有的某个点云会话，按 graph 里的源节点组装成注入项。
    ///
    /// 找不到会话 → `Err`（给人看的中文提示，`run_graph` 原样交给前端）；
    /// graph 里没有可注入的源节点 → `Err`。
    fn inputs(
        &self,
        scene_id: &str,
        graph: &GraphDoc,
        manifest: &serde_json::Value,
    ) -> Result<Vec<RunInput>, String>;
}

/// 宿主对 IPC 层的全部旋钮。全是 `Option`，`None` 一律表示「和 LyFlow 桌面壳一样」。
pub struct HostConfig {
    /// `Some` 时 `path` / `graphPath` 里的相对路径按它解析，并拒绝逃出去；
    /// `None` = 现状：路径原样用（LyFlow 自己的壳配了文件对话框，路径是绝对的）。
    pub workspace_root: Option<PathBuf>,
    /// 库算子目录（ADR-0010）。`None` = 现状：app data 下的 `library/` 加
    /// `LYFLOW_LIBRARY_DIRS`。给 `Some(vec![])` 就是「这个宿主不要库算子」。
    pub library_dirs: Option<Vec<PathBuf>>,
    /// 点云注入的来源。`None` 时带 `sceneId` 的 `run_graph` 直接报错，
    /// 而不是静默地跑一张没注入的图。
    pub scenes: Option<Arc<dyn SceneProvider>>,
    /// 开发期热重载（ADR-0009）。`false` 时不起 watcher 线程。
    /// 按 git 依赖引 LyFlow 的宿主本来就没有可盯的 CMake 产物（`watch_source()` 是 None），
    /// 这个开关是给「有产物但不想要」的场合用的。
    pub hot_reload: bool,
}

impl Default for HostConfig {
    fn default() -> Self {
        Self {
            workspace_root: None,
            library_dirs: None,
            scenes: None,
            hot_reload: true,
        }
    }
}

/// 把 LyFlow 的 managed state 与启动步骤挂到宿主的 Builder 上。
///
/// ```ignore
/// let app = lyflow_lib::host::attach(tauri::Builder::default(), cfg)
///     .invoke_handler(lyflow_lib::lyflow_handler![my_own_command])
///     .run(tauri::generate_context!())?;
/// ```
///
/// **它占用了 `Builder::setup`**（tauri 的 `setup` 是整份替换，不是追加）。宿主要有
/// 自己的 setup 就别再调 `.setup()`，改成在自己的 setup 里调 [`setup`]。
pub fn attach<R: tauri::Runtime>(
    builder: tauri::Builder<R>,
    cfg: HostConfig,
) -> tauri::Builder<R> {
    builder
        .manage(crate::execution::RunManager::new())
        .manage(Arc::new(cfg))
        .setup(|app| {
            setup(&app.handle().clone());
            Ok(())
        })
}

/// `attach()` 的 setup 里做的那些事，单独拿出来给「自己要 setup」的宿主调。
/// 一件都不 fatal：库目录扫不出来只是少几个算子，不该拦住宿主启动。
pub fn setup<R: tauri::Runtime>(app: &tauri::AppHandle<R>) {
    let hot_reload = config(app).hot_reload;
    if hot_reload {
        crate::watcher::spawn(app.clone());
    }
    // 库算子目录（ADR-0010）。
    use tauri::Manager;
    let runs = app.state::<crate::execution::RunManager>();
    match crate::commands::rescan_library(app, &runs) {
        Ok(status) if !status.problems.is_empty() => {
            for p in &status.problems {
                eprintln!("库算子: {p}");
            }
        }
        Err(e) => eprintln!("库算子目录不可用: {e}"),
        Ok(_) => {}
    }
    if hot_reload {
        crate::watcher::spawn_library(app.clone());
    }
}

/// 取 managed 的 `HostConfig`。宿主忘了 `attach()` 时退回一份默认配置，
/// 而不是 panic —— 少一条 `manage` 的表现不该是「点一下运行就崩」。
pub fn config<R: tauri::Runtime>(app: &tauri::AppHandle<R>) -> Arc<HostConfig> {
    use tauri::Manager;
    app.try_state::<Arc<HostConfig>>()
        .map(|s| s.inner().clone())
        .unwrap_or_else(|| Arc::new(HostConfig::default()))
}

/// 启动自检：算子描述的契约检查 + 清掉上一代热重载留下的 DLL。
///
/// LyFlow 自己的壳把它当 fatal（见 `lib.rs::run`）：契约破了继续跑，前端会拿到一份
/// 自相矛盾的 manifest，然后以各种离奇的方式失败，排查成本远高于在这里直接报错。
/// 宿主可以选择只记一条日志 —— 它的界面上未必只有 LyFlow 一块。
pub fn startup_self_check() -> Result<(), String> {
    // 最常见的失败原因是 lyflow_core.dll 不在 exe 旁边。CoreError 的文本里已经
    // 写了它该在哪 —— 白屏加一句「加载失败」是最难排查的形态。
    let problems =
        core_ffi::manifest_problems().map_err(|e| format!("无法读取 lyflow-core 自检结果: {e}"))?;
    if !problems.is_empty() {
        let mut msg = format!("lyflow-core 算子描述自检失败（{} 条）：", problems.len());
        for p in &problems {
            msg.push_str("\n  - ");
            msg.push_str(p);
        }
        return Err(msg);
    }

    // 上一代的 gen DLL 那时还被自己锁着，删不掉；清理只能放在下次启动（ADR-0009）。
    let stale = core_ffi::cleanup_old_generations();
    if stale > 0 {
        println!("清理了 {stale} 个上次留下的热重载 DLL");
    }
    Ok(())
}

// ------------------------------------------------------------------ 路径

/// 纯文本地消掉 `.` 与 `..`，不碰文件系统。
///
/// 不用 `canonicalize()`：它要求路径存在（save_graph 写的是还不存在的文件），
/// 而且在 Windows 上会带出 `\\?\` 扩展长度前缀，`starts_with` 立刻就对不上了。
fn lexically_normal(p: &Path) -> PathBuf {
    let mut out = PathBuf::new();
    for comp in p.components() {
        match comp {
            Component::CurDir => {}
            // 弹不动（已经到根，或者开头就是一串 `..`）时保留它，
            // 这样下面的 starts_with 一定判它逃逸，而不是悄悄吃掉。
            Component::ParentDir => {
                if !out.pop() {
                    out.push("..");
                }
            }
            c => out.push(c.as_os_str()),
        }
    }
    out
}

/// 把前端给的路径参数解析成真正要读写的路径。
///
/// - `workspace_root` 是 `None`：原样。LyFlow 自己的壳走系统文件对话框，
///   拿到的本来就是用户亲自选的绝对路径。
/// - `workspace_root` 是 `Some`：相对路径 join 到根上并消掉 `..`，结果不在根下就拒绝。
///   绝对路径原样放行 —— 宿主自己的 webview 是可信的，限制它只会让「另存到 D 盘」失败。
pub fn resolve_path(cfg: &HostConfig, s: &str) -> Result<PathBuf, String> {
    let raw = PathBuf::from(s);
    let Some(root) = cfg.workspace_root.as_deref() else {
        return Ok(raw);
    };
    if raw.is_absolute() {
        return Ok(raw);
    }
    let root = lexically_normal(root);
    // 注意 `root.join("D:x")`：带盘符前缀的「相对」路径会把 root 整个换掉，
    // 于是它自然落到下面这条 starts_with 上被拒 —— 不用单独判一次盘符。
    let joined = lexically_normal(&root.join(&raw));
    if !joined.starts_with(&root) {
        return Err(format!("路径逃出了工作区: {s}"));
    }
    Ok(joined)
}

/// `resolve_path` 的 String 版，给那些把路径原样交给 core 的地方（base_dir）用。
pub fn resolve_path_str(cfg: &HostConfig, s: &str) -> Result<String, String> {
    Ok(resolve_path(cfg, s)?.to_string_lossy().into_owned())
}

// ------------------------------------------------------------------ 注入

/// 按**算子 id**找图里唯一的源节点，把 `clouds` 装配成注入项。宿主的
/// [`SceneProvider`] 通常就是「查会话表拿到 `clouds`，再调它」。
///
/// ADR-0017：注入是整节点级的 —— 被注入的节点 compute 整个不跑，所以它声明的
/// **每个**输出端口都要有一片云。少给一个在 core 那边的表现是
/// 「算子没有写输出端口 X」，离现场太远，所以在这里就挡住。
pub fn inputs_for_source_op(
    graph: &GraphDoc,
    manifest: &serde_json::Value,
    source_op: &str,
    clouds: &BTreeMap<String, Cloud>,
) -> Result<Vec<RunInput>, String> {
    let matched: Vec<&str> = graph
        .nodes
        .iter()
        .filter(|n| n.op == source_op)
        .map(|n| n.id.as_str())
        .collect();
    let node_id = match matched.as_slice() {
        [one] => *one,
        [] => return Err(format!("图里没有 {source_op} 节点，注入无处可去")),
        many => {
            return Err(format!(
                "图里有 {} 个 {source_op} 节点（{}），不知道该注入哪一个",
                many.len(),
                many.join("、")
            ))
        }
    };

    let ports = output_ports(manifest, source_op)?;
    let mut inputs = Vec::with_capacity(ports.len());
    for port in ports {
        let cloud = clouds.get(&port).ok_or_else(|| {
            format!(
                "{source_op} 的输出端口 {port} 没有给点云 —— \
                 注入是整节点级的，它声明的每个输出端口都要给（ADR-0017）"
            )
        })?;
        inputs.push(RunInput {
            node_id: node_id.to_string(),
            port,
            xyz: cloud.xyz.clone(),
            intensity: cloud.intensity.clone(),
            normals: Vec::new(),
            rgb: Vec::new(),
        });
    }
    Ok(inputs)
}

/// manifest 里某个算子声明的全部输出端口名。
fn output_ports(manifest: &serde_json::Value, op: &str) -> Result<Vec<String>, String> {
    let operators = manifest["operators"]
        .as_array()
        .ok_or("manifest 里没有 operators 数组")?;
    let desc = operators
        .iter()
        .find(|o| o["id"] == op)
        .ok_or_else(|| format!("manifest 里没有算子 {op}"))?;
    let ports: Vec<String> = desc["outputs"]
        .as_array()
        .map(|a| {
            a.iter()
                .filter_map(|p| p["name"].as_str().map(str::to_owned))
                .collect()
        })
        .unwrap_or_default();
    if ports.is_empty() {
        return Err(format!("算子 {op} 一个输出端口都没有，注入没有意义"));
    }
    Ok(ports)
}

// ------------------------------------------------------------------ 命令清单

/// LyFlow 的 35 条命令，加上宿主自己的那些，合成一个 `invoke_handler`。
///
/// ```ignore
/// .invoke_handler(lyflow_lib::lyflow_handler![my_open_scene, my_close_scene])
/// ```
///
/// 能跨 crate 是因为 `#[tauri::command]` 除了函数本身还发一对 `#[macro_export]` 的
/// `macro_rules!`，并在同一个模块里 `pub use` 了它们 —— 于是
/// `lyflow_lib::commands::get_manifest` 这条路径对函数和对宏都解析得开，
/// 而 `tauri::generate_handler!` 要的正是这个。
///
/// 命令名与事件名都**不带前缀**（没有做成 tauri 插件），前端那份 `TauriTransport`
/// 一行都不用改。
#[macro_export]
macro_rules! lyflow_handler {
    ($($host_cmd:path),* $(,)?) => {
        $crate::tauri::generate_handler![
            $crate::commands::get_manifest,
            $crate::commands::get_core_info,
            $crate::commands::save_graph,
            $crate::commands::load_graph,
            $crate::commands::validate_graph,
            $crate::commands::plan_graph,
            $crate::commands::clear_cache,
            $crate::commands::evict_cache,
            $crate::commands::cache_stats,
            $crate::commands::run_graph,
            $crate::commands::cancel_run,
            $crate::commands::get_output_info,
            $crate::commands::get_run_outputs,
            $crate::commands::import_graph,
            $crate::commands::get_output_cloud,
            $crate::commands::get_output_tensor,
            $crate::commands::get_output_indices,
            $crate::commands::load_cloud_file,
            $crate::commands::list_snippets,
            $crate::commands::get_recent_files,
            $crate::commands::push_recent_file,
            $crate::commands::write_backup,
            $crate::commands::backup_status,
            $crate::commands::read_backup,
            $crate::commands::discard_backup,
            $crate::commands::write_file_bytes,
            $crate::commands::list_recipe_dir,
            $crate::commands::read_recipe_file,
            $crate::commands::write_recipe_file,
            $crate::commands::delete_recipe_file,
            $crate::commands::rename_recipe_file,
            $crate::commands::get_library_status,
            $crate::commands::refresh_library,
            $crate::commands::save_as_library,
            $crate::commands::get_library_definition,
            $($host_cmd,)*
        ]
    };
}
