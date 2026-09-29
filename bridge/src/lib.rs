//! LyFlow 桥接层：IPC、序列化、文件读写、进程生命周期、事件推流、崩溃隔离。
//! 不理解算子语义，不改写图结构（docs/architecture.md）。

pub mod cli;
pub mod core_ffi;
mod disk_cache;
pub mod library_settings;
mod eval;
pub mod graph;
mod patch;
pub mod pcd;
mod perturb;
pub mod recipe;
pub mod ulid;

// IPC 层。关在 `host` 而不是 `desktop` 后面：外部 Rust/Tauri 宿主要把这几个模块挂到
// 自己的 Builder 上（docs/embedding.md「Rust/Tauri 宿主」），所以它们必须 pub。
// 只有「自己开窗」那一段（`run()`）仍然是 LyFlow 桌面壳专属。
#[cfg(feature = "host")]
pub mod commands;
#[cfg(feature = "host")]
pub mod execution;
#[cfg(feature = "host")]
pub mod host;
#[cfg(feature = "host")]
pub mod watcher;

pub use graph::{Edge, GraphDoc, Node, PortRef};

/// `lyflow_handler!` 展开时要用 tauri 的 proc-macro。从这里再导出一份，宿主就不必
/// 保证自己那个 `tauri` 依赖没被 rename 过。
#[cfg(feature = "host")]
pub use tauri;

/// LyFlow 自己的桌面壳。除了窗口、对话框插件和 `generate_context!`，
/// 它和外部宿主走的是同一份 `host::attach` —— 两条路分叉了，分叉处就会长出
/// 只在其中一边出现的 bug。
#[cfg(feature = "desktop")]
#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    // 启动自检当 fatal：契约破了继续跑，前端会拿到一份自相矛盾的 manifest，
    // 然后以各种离奇的方式失败，排查成本远高于在这里直接报错。
    // 外部宿主拿到的是同一个 Result，但它可以选择不 fatal。
    if let Err(e) = host::startup_self_check() {
        eprintln!("{e}");
        std::process::exit(1);
    }

    let builder = tauri::Builder::default().plugin(tauri_plugin_dialog::init());
    host::attach(builder, host::HostConfig::default())
        .invoke_handler(crate::lyflow_handler![])
        .run(tauri::generate_context!())
        .expect("启动 Tauri 失败");
}
