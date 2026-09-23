//! LyFlow 桥接层：IPC、序列化、文件读写、进程生命周期、事件推流、崩溃隔离。
//! 不理解算子语义，不改写图结构（docs/architecture.md）。

pub mod cli;
pub mod core_ffi;
mod eval;
pub mod graph;
mod patch;
pub mod pcd;
mod perturb;
pub mod ulid;

#[cfg(feature = "desktop")]
mod commands;
#[cfg(feature = "desktop")]
mod execution;
#[cfg(feature = "desktop")]
mod watcher;

pub use graph::{Edge, GraphDoc, Node, PortRef};

#[cfg(feature = "desktop")]
#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    // 启动自检当 fatal：契约破了继续跑，前端会拿到一份自相矛盾的 manifest，
    // 然后以各种离奇的方式失败，排查成本远高于在这里直接报错。
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
            // 最常见的原因是 lyflow_core.dll 不在 exe 旁边。错误信息里已经
            // 写了它该在哪 —— 白屏加一句「加载失败」是最难排查的形态。
            eprintln!("无法读取 lyflow-core 自检结果: {e}");
            std::process::exit(1);
        }
        Ok(_) => {}
    }

    // 上一代的 gen DLL 那时还被自己锁着，删不掉；清理只能放在下次启动（ADR-0009）。
    let stale = core_ffi::cleanup_old_generations();
    if stale > 0 {
        println!("清理了 {stale} 个上次留下的热重载 DLL");
    }

    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .manage(execution::RunManager::new())
        .setup(|app| {
            watcher::spawn(app.handle().clone());
            // 库算子目录（ADR-0010）。扫不出来只是少几个算子，不该拦住启动。
            match commands::rescan_library(app.handle()) {
                Ok(status) if !status.problems.is_empty() => {
                    for p in &status.problems {
                        eprintln!("库算子: {p}");
                    }
                }
                Err(e) => eprintln!("库算子目录不可用: {e}"),
                Ok(_) => {}
            }
            watcher::spawn_library(app.handle().clone());
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            commands::get_manifest,
            commands::get_core_info,
            commands::save_graph,
            commands::load_graph,
            commands::validate_graph,
            commands::plan_graph,
            commands::clear_cache,
            commands::cache_stats,
            commands::run_graph,
            commands::cancel_run,
            commands::get_output_info,
            commands::get_run_outputs,
            commands::import_graph,
            commands::get_output_cloud,
            commands::get_output_tensor,
            commands::get_output_indices,
            commands::load_cloud_file,
            commands::list_snippets,
            commands::get_recent_files,
            commands::push_recent_file,
            commands::write_backup,
            commands::backup_status,
            commands::read_backup,
            commands::discard_backup,
            commands::write_file_bytes,
            commands::get_library_status,
            commands::refresh_library,
            commands::save_as_library,
        ])
        .run(tauri::generate_context!())
        .expect("启动 Tauri 失败");
}
