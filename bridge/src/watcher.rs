//! 开发期热重载（ADR-0009）。盯着 CMake 写出来的 lyflow_core.dll，
//! 变了就丢掉旧代、复制一份新的加载、自检、把新 manifest 推给前端。

use std::path::{Path, PathBuf};
use std::sync::mpsc;
use std::time::{Duration, Instant};

use notify::{RecommendedWatcher, RecursiveMode, Watcher};
use tauri::{AppHandle, Emitter, Manager};

use crate::core_ffi;
use crate::execution::RunManager;

/// manifest 换代了，前端应当替换 manifest store 并对当前 doc 重新校验。
pub const EVENT_UPDATED: &str = "manifest-updated";
/// 新的一代自检没过，旧代仍在服役。
pub const EVENT_FAILED: &str = "core-reload-failed";

/// 连续写盘会触发一串事件（链接器边写边改），必须等它安静下来再动手。
const QUIET: Duration = Duration::from_millis(400);

#[derive(serde::Serialize, Clone)]
struct ReloadOk {
    generation: u32,
    manifest: serde_json::Value,
    #[serde(rename = "operatorCount")]
    operator_count: usize,
}

#[derive(serde::Serialize, Clone)]
struct ReloadFailed {
    problems: Vec<String>,
    /// 仍在服役的那一代。前端据此知道界面上的算子还是旧的。
    generation: u32,
}

/// 起一个后台线程盯着 DLL。源文件不存在（安装包里就是这样）时什么都不做。
pub fn spawn(app: AppHandle) -> Option<PathBuf> {
    let source = core_ffi::watch_source()?;
    let dir = source.parent()?.to_path_buf();
    let watched = source.clone();

    std::thread::spawn(move || {
        let (tx, rx) = mpsc::channel();
        let mut watcher: RecommendedWatcher = match notify::recommended_watcher(move |res| {
            let _ = tx.send(res);
        }) {
            Ok(w) => w,
            Err(e) => {
                eprintln!("热重载：建不了文件监视器，功能关闭 ({e})");
                return;
            }
        };
        // 盯目录而不是盯文件：链接器是「删掉再建」，盯文件的话句柄跟着一起没了。
        if let Err(e) = watcher.watch(&dir, RecursiveMode::NonRecursive) {
            eprintln!("热重载：监视 {} 失败 ({e})", dir.display());
            return;
        }
        println!("热重载已开启，正在盯 {}", watched.display());

        let mut pending: Option<Instant> = None;
        loop {
            let timeout = pending.map_or(Duration::from_secs(3600), |_| QUIET);
            match rx.recv_timeout(timeout) {
                Ok(Ok(event)) => {
                    if event.paths.iter().any(|p| same_file_name(p, &watched)) {
                        pending = Some(Instant::now());
                    }
                }
                Ok(Err(_)) => {}
                Err(mpsc::RecvTimeoutError::Timeout) => {}
                Err(mpsc::RecvTimeoutError::Disconnected) => return,
            }
            if pending.is_some_and(|t| t.elapsed() >= QUIET) {
                pending = None;
                reload(&app, &watched);
            }
        }
    });

    Some(source)
}

fn same_file_name(a: &Path, b: &Path) -> bool {
    a.file_name() == b.file_name()
}

/// 一轮换代。顺序不能变：先停活跃 run，再放掉全部 RunHandle，最后才换 Core ——
/// 旧 DLL 只要还有一个 Arc 就不会真的卸载，而缓存里的 Data 是旧 DLL 里的对象。
fn reload(app: &AppHandle, source: &Path) {
    if let Some(runs) = app.try_state::<RunManager>() {
        runs.drop_all();
    }
    if let Ok(core) = core_ffi::core() {
        core.cache_clear();
    }

    match core_ffi::reload_from(source) {
        Ok(generation) => {
            let payload = core_ffi::core()
                .and_then(|c| c.manifest_json().map_err(|e| e.to_string()))
                .and_then(|raw| {
                    serde_json::from_str::<serde_json::Value>(&raw).map_err(|e| e.to_string())
                });
            match payload {
                Ok(manifest) => {
                    let count = manifest["operators"].as_array().map_or(0, Vec::len);
                    println!("热重载：第 {generation} 代已就绪，{count} 个算子");
                    let _ = app.emit(
                        EVENT_UPDATED,
                        ReloadOk {
                            generation,
                            manifest,
                            operator_count: count,
                        },
                    );
                }
                Err(e) => emit_failed(app, vec![e]),
            }
        }
        Err(e) => {
            eprintln!("热重载失败，继续用旧的一代：{e}");
            emit_failed(app, e.lines().map(str::to_owned).collect());
        }
    }
}

/// 盯着库目录。库文件变了走的是和热重载同一条 `manifest-updated` 通路（ADR-0010）。
pub fn spawn_library(app: AppHandle) {
    let Ok(dirs) = crate::commands::library_dirs(&app) else {
        return;
    };
    std::thread::spawn(move || {
        let (tx, rx) = mpsc::channel();
        let Ok(mut watcher): Result<RecommendedWatcher, _> =
            notify::recommended_watcher(move |res| {
                let _ = tx.send(res);
            })
        else {
            return;
        };
        let mut watching = 0;
        for dir in &dirs {
            if watcher.watch(Path::new(dir), RecursiveMode::NonRecursive).is_ok() {
                watching += 1;
            }
        }
        if watching == 0 {
            return;
        }
        let mut pending: Option<Instant> = None;
        loop {
            let timeout = pending.map_or(Duration::from_secs(3600), |_| QUIET);
            match rx.recv_timeout(timeout) {
                Ok(Ok(event)) => {
                    if event.paths.iter().any(|p| is_library_file(p)) {
                        pending = Some(Instant::now());
                    }
                }
                Ok(Err(_)) => {}
                Err(mpsc::RecvTimeoutError::Timeout) => {}
                Err(mpsc::RecvTimeoutError::Disconnected) => return,
            }
            if pending.is_some_and(|t| t.elapsed() >= QUIET) {
                pending = None;
                reload_library(&app);
            }
        }
    });
}

fn is_library_file(p: &Path) -> bool {
    p.file_name()
        .map(|n| n.to_string_lossy().ends_with(".lyflow-op.json"))
        .unwrap_or(false)
}

fn reload_library(app: &AppHandle) {
    if let Some(runs) = app.try_state::<RunManager>() {
        runs.drop_all();
    }
    match crate::commands::rescan_library(app) {
        Ok(status) => {
            let payload = core_ffi::core()
                .and_then(|c| c.manifest_json().map_err(|e| e.to_string()))
                .and_then(|raw| {
                    serde_json::from_str::<serde_json::Value>(&raw).map_err(|e| e.to_string())
                });
            match payload {
                Ok(manifest) => {
                    let count = manifest["operators"].as_array().map_or(0, Vec::len);
                    println!("库算子已重扫：{} 个库算子", status.count);
                    let _ = app.emit(
                        EVENT_UPDATED,
                        ReloadOk {
                            generation: core_ffi::generation(),
                            manifest,
                            operator_count: count,
                        },
                    );
                }
                Err(e) => emit_failed(app, vec![e]),
            }
            if !status.problems.is_empty() {
                emit_failed(app, status.problems);
            }
        }
        Err(e) => emit_failed(app, vec![e]),
    }
}

fn emit_failed(app: &AppHandle, problems: Vec<String>) {
    let _ = app.emit(
        EVENT_FAILED,
        ReloadFailed {
            problems,
            generation: core_ffi::generation(),
        },
    );
}
