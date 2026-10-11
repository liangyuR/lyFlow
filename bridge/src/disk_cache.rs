//! CLI 磁盘缓存按产品版本和源码修订隔离；未提交构建不启用跨进程复用。

use std::path::{Path, PathBuf};

use serde_json::json;

use crate::cli::{json_line, line, Parsed, Sink, EXIT_FAILED, EXIT_OK, EXIT_USAGE};
use crate::core_ffi::{self, Core};

/// MCP 起的每个 CLI 进程都继承它 —— 在 MCP server 的环境里配一次，工具参数不用变。
pub(crate) const ENV: &str = "LYFLOW_CACHE_DIR";

const FORMAT: &str = "lfc1";

/// `--cache-dir` 优先，其次环境变量；空串等于没给。
pub(crate) fn dir_of(parsed: &Parsed) -> Option<String> {
    parsed
        .one("cache-dir")
        .map(str::to_string)
        .or_else(|| std::env::var(ENV).ok())
        .filter(|d| !d.trim().is_empty())
}

fn identity_for(version: &str, revision: &str) -> Result<String, String> {
    if version != lyflow_client::ABI_VERSION {
        return Err("缓存产品版本与客户端 ABI 不一致".into());
    }
    if revision.len() != 40 || !revision.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b)) {
        return Err("跨进程缓存需要干净构建的明确源码修订；请先提交源码并重新构建".into());
    }
    let directory_version = version.replace('.', "_");
    Ok(format!("{FORMAT}-v{directory_version}-source-{revision}"))
}

pub(crate) fn build_identity(core: &Core) -> Result<String, String> {
    identity_for(&core.version(), &core.source_revision())
}

/// 给了目录就打开落盘缓存；没给什么都不做（默认关）。stderr 记一行实际用的目录。
pub(crate) fn enable(parsed: &Parsed, core: &Core, err: &Sink) -> Result<(), String> {
    let Some(dir) = dir_of(parsed) else {
        return Ok(());
    };
    let identity = build_identity(core)?;
    core.cache_set_dir(&dir, &identity)
        .map_err(|e| format!("--cache-dir: {e}"))?;
    line(err, &format!("落盘缓存：{}", Path::new(&dir).join(&identity).display()));
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

fn identity_dirs(root: &Path) -> Vec<(String, PathBuf)> {
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
    let current = match core_ffi::Core::load_from(&core_ffi::dll_file()).map_err(|e| e.to_string()).and_then(|core| build_identity(&core)) {
        Ok(f) => f,
        Err(e) => {
            line(err, &e);
            return EXIT_FAILED;
        }
    };
    let root = PathBuf::from(&dir);
    match action {
        Some("info") => {
            let entries: Vec<_> = identity_dirs(&root)
                .into_iter()
                .map(|(name, path)| {
                    let (files, bytes) = tally(&path);
                    json!({ "buildIdentity": name, "files": files, "bytes": bytes, "current": name == current })
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
            for (name, path) in identity_dirs(&root) {
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn build_identity_uses_explicit_version_and_revision() {
        let revision = "0123456789abcdef0123456789abcdef01234567";
        assert_eq!(identity_for(lyflow_client::ABI_VERSION, revision).unwrap(),
            format!("lfc1-v{}-source-{revision}", lyflow_client::ABI_VERSION.replace('.', "_")));
        assert_ne!(identity_for(lyflow_client::ABI_VERSION, revision).unwrap(),
            identity_for(lyflow_client::ABI_VERSION, "1123456789abcdef0123456789abcdef01234567").unwrap());
    }

    #[test]
    fn undeclared_builds_and_version_mismatches_cannot_reuse_disk_cache() {
        for revision in ["", "dirty", "../outside", "a15-0123456789abcdef"] {
            assert!(identity_for(lyflow_client::ABI_VERSION, revision).is_err());
        }
        let wrong_version = format!("{}.mismatch", lyflow_client::ABI_VERSION);
        assert!(identity_for(&wrong_version, "0123456789abcdef0123456789abcdef01234567").is_err());
    }
}
