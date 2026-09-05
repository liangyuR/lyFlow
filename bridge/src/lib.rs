//! LyFlow 桥接层。
//!
//! 职责边界（docs/architecture.md）：IPC、序列化、文件读写、进程生命周期、
//! 事件推流、崩溃隔离。**不理解算子语义，不改写图结构。**

mod commands;
mod core_ffi;
mod graph;

pub use graph::{Edge, GraphDoc, Node, PortRef};

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    // 启动自检当 fatal。
    //
    // 契约破了还继续跑，前端会拿到一份自相矛盾的 manifest，然后以各种离奇的
    // 方式失败 —— 排查成本远高于在这里直接报错。算子描述写错是开发期的事，
    // 用户永远不该看到这条路径。
    match core_ffi::manifest_problems() {
        Ok(problems) if !problems.is_empty() => {
            eprintln!(
                "lyflow-core 算子描述自检失败（{} 条），拒绝启动：",
                problems.len()
            );
            for p in &problems {
                eprintln!("  - {p}");
            }
            std::process::exit(1);
        }
        Err(e) => {
            eprintln!("无法读取 lyflow-core 自检结果: {e}");
            std::process::exit(1);
        }
        Ok(_) => {}
    }

    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![
            commands::get_manifest,
            commands::get_core_info,
            commands::save_graph,
            commands::load_graph,
        ])
        .run(tauri::generate_context!())
        .expect("启动 Tauri 失败");
}
