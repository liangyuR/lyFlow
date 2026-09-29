//! 结果缓存落盘的 CLI 一侧（docs/disk-cache-plan.md）：算构建指纹、按 `--cache-dir` / `LYFLOW_CACHE_DIR`
//! 打开、`lyflow cache info|clear`。缓存判定在 core（`lyflow_cache_set_dir`，ADR-0007），这里只给目录与指纹。

use std::path::{Path, PathBuf};
use std::time::UNIX_EPOCH;

use serde_json::json;
use sha2::{Digest, Sha256};

use crate::cli::{json_line, line, Parsed, Sink, EXIT_FAILED, EXIT_OK, EXIT_USAGE};
use crate::core_ffi::{self, Core};

/// MCP 起的每个 CLI 进程都继承它 —— 在 MCP server 的环境里配一次，工具参数不用变。
pub(crate) const ENV: &str = "LYFLOW_CACHE_DIR";
/// 与 core 的 .lfc 格式版本一起进指纹：格式改了，旧目录自然作废。
const FORMAT: &str = "lfc1";

/// `--cache-dir` 优先，其次环境变量；空串等于没给。
pub(crate) fn dir_of(parsed: &Parsed) -> Option<String> {
    parsed
        .one("cache-dir")
        .map(str::to_string)
        .or_else(|| std::env::var(ENV).ok())
        .filter(|d| !d.trim().is_empty())
}

/// 构建指纹：core DLL 的内容哈希 + 同目录其它 DLL（onnxruntime、PCL……）的「名字 / 大小 / 修改时间」
/// + ABI 号 + 格式版本。改了算子实现重编 core 就是新指纹、整个旧目录不再复用 —— cacheKey 只看算子版本，
/// 靠它避开实现变了版本没变的旧结果。依赖 DLL 不读全文：每次起进程多花上百毫秒不值。
pub(crate) fn fingerprint() -> Result<String, String> {
    let dll = core_ffi::dll_file();
    let bytes = std::fs::read(&dll).map_err(|e| format!("读不到 core DLL {}: {e}", dll.display()))?;
    let mut h = Sha256::new();
    h.update(FORMAT.as_bytes());
    h.update(lyflow_client::ABI_VERSION.to_le_bytes());
    h.update(&bytes);
    let mut deps: Vec<(String, u64, u128)> = Vec::new();
    if let Some(dir) = dll.parent() {
        for entry in std::fs::read_dir(dir).into_iter().flatten().flatten() {
            let path = entry.path();
            let name = entry.file_name().to_string_lossy().to_lowercase();
            // core 自己已经按内容算过；热重载的 gen 副本（lyflow_core.genN.dll）不算依赖
            if !name.ends_with(".dll") || name.starts_with("lyflow_core") || path == dll {
                continue;
            }
            if let Ok(meta) = entry.metadata() {
                let modified = meta
                    .modified()
                    .ok()
                    .and_then(|t| t.duration_since(UNIX_EPOCH).ok())
                    .map(|d| d.as_nanos())
                    .unwrap_or(0);
                deps.push((name, meta.len(), modified));
            }
        }
    }
    deps.sort();
    for (name, len, modified) in deps {
        h.update(name.as_bytes());
        h.update(len.to_le_bytes());
        h.update(modified.to_le_bytes());
    }
    let hex: String = h.finalize().iter().take(8).map(|b| format!("{b:02x}")).collect();
    Ok(format!("a{}-{hex}", lyflow_client::ABI_VERSION))
}

/// 给了目录就打开落盘缓存；没给什么都不做（默认关）。stderr 记一行实际用的目录。
pub(crate) fn enable(parsed: &Parsed, core: &Core, err: &Sink) -> Result<(), String> {
    let Some(dir) = dir_of(parsed) else {
        return Ok(());
    };
    let fp = fingerprint()?;
    core.cache_set_dir(&dir, &fp)
        .map_err(|e| format!("--cache-dir: {e}"))?;
    line(err, &format!("落盘缓存：{}", Path::new(&dir).join(&fp).display()));
    Ok(())
}

fn tally(dir: &Path) -> (u64, u64) {
    let mut files = 0u64;
    let mut bytes = 0u64;
    let mut stack = vec![dir.to_path_buf()];
    while let Some(d) = stack.pop() {
        for entry in std::fs::read_dir(&d).into_iter().flatten().flatten() {
            let path = entry.path();
            if path.is_dir() {
                stack.push(path);
            } else if path.extension().is_some_and(|e| e == "lfc") {
                files += 1;
                bytes += entry.metadata().map(|m| m.len()).unwrap_or(0);
            }
        }
    }
    (files, bytes)
}

fn fingerprint_dirs(root: &Path) -> Vec<(String, PathBuf)> {
    let mut out: Vec<(String, PathBuf)> = std::fs::read_dir(root)
        .into_iter()
        .flatten()
        .flatten()
        .filter(|e| e.path().is_dir())
        .map(|e| (e.file_name().to_string_lossy().into_owned(), e.path()))
        .collect();
    out.sort();
    out
}

/// `lyflow cache info|clear --cache-dir <dir> [--stale]`。v1 不做自动淘汰：占用靠 info 看、靠 clear 清。
pub(crate) fn cmd_cache(parsed: &Parsed, out: &Sink, err: &Sink) -> i32 {
    let action = parsed.positional.first().map(String::as_str);
    let Some(dir) = dir_of(parsed) else {
        line(err, "lyflow cache 要 --cache-dir <dir>（或环境变量 LYFLOW_CACHE_DIR）");
        return EXIT_USAGE;
    };
    let current = match fingerprint() {
        Ok(f) => f,
        Err(e) => {
            line(err, &e);
            return EXIT_FAILED;
        }
    };
    let root = PathBuf::from(&dir);
    match action {
        Some("info") => {
            let entries: Vec<_> = fingerprint_dirs(&root)
                .into_iter()
                .map(|(name, path)| {
                    let (files, bytes) = tally(&path);
                    json!({ "fingerprint": name, "files": files, "bytes": bytes, "current": name == current })
                })
                .collect();
            json_line(
                out,
                &json!({ "kind": "cache_info", "dir": dir, "current": current, "entries": entries }),
            );
            EXIT_OK
        }
        Some("clear") => {
            let stale_only = parsed.has("stale");
            let mut removed = Vec::new();
            for (name, path) in fingerprint_dirs(&root) {
                if stale_only && name == current {
                    continue;
                }
                match std::fs::remove_dir_all(&path) {
                    Ok(()) => removed.push(name),
                    Err(e) => line(err, &format!("删不掉 {}: {e}", path.display())),
                }
            }
            json_line(out, &json!({ "kind": "cache_cleared", "dir": dir, "removed": removed }));
            EXIT_OK
        }
        _ => {
            line(err, "用法：lyflow cache info|clear --cache-dir <dir> [--stale]");
            EXIT_USAGE
        }
    }
}
