//! 库算子目录的用户设置（ADR-0010）。设置界面写、app 与 CLI 同读同一个文件 ——
//! 两边各算一份目录列表迟早漂开，就会出现「界面里能跑、CLI 里报缺算子」。
//!
//! 文件：`<app data>/library-dirs.json`，`{ "extraDirs": ["D:/libs", …] }`。
//! 完整列表的顺序：app data 下的 `library/`（「保存到库」写这里）→ 设置里加的 → 环境变量 `LYFLOW_LIBRARY_DIRS`
//! （分号分隔），去重（Windows 上不分大小写、不分斜杠方向）。

use std::path::{Path, PathBuf};

use serde_json::{json, Value};

pub(crate) const FILE_NAME: &str = "library-dirs.json";
pub(crate) const ENV: &str = "LYFLOW_LIBRARY_DIRS";

pub(crate) fn settings_file(app_data: &Path) -> PathBuf {
    app_data.join(FILE_NAME)
}

/// 设置里加的目录。文件不在、读不懂都当空：设置坏了不该让库算子全挂。
pub(crate) fn read_extra(app_data: &Path) -> Vec<String> {
    let Ok(text) = std::fs::read_to_string(settings_file(app_data)) else {
        return Vec::new();
    };
    let Ok(value) = serde_json::from_str::<Value>(&text) else {
        return Vec::new();
    };
    value["extraDirs"]
        .as_array()
        .map(|a| a.iter().filter_map(Value::as_str).map(str::to_string).collect())
        .map(|v: Vec<String>| normalize(&v))
        .unwrap_or_default()
}

/// 写设置。先规整（去空白、去空串、去重），写临时文件再改名。返回写进去的那一份。
pub(crate) fn write_extra(app_data: &Path, dirs: &[String]) -> Result<Vec<String>, String> {
    let dirs = normalize(dirs);
    std::fs::create_dir_all(app_data).map_err(|e| format!("创建 {} 失败: {e}", app_data.display()))?;
    let file = settings_file(app_data);
    let tmp = file.with_extension("json.tmp");
    let mut text = serde_json::to_string_pretty(&json!({ "extraDirs": dirs })).map_err(|e| e.to_string())?;
    text.push('\n');
    std::fs::write(&tmp, text).map_err(|e| format!("写入 {} 失败: {e}", tmp.display()))?;
    std::fs::rename(&tmp, &file).map_err(|e| format!("写入 {} 失败: {e}", file.display()))?;
    Ok(dirs)
}

/// 环境变量里的目录（分号分隔）。
pub(crate) fn env_dirs() -> Vec<String> {
    std::env::var(ENV)
        .map(|v| v.split(';').map(str::trim).filter(|d| !d.is_empty()).map(str::to_string).collect())
        .unwrap_or_default()
}

/// 比较用的键：Windows 路径不分大小写、不分斜杠方向，末尾斜杠不算。
fn key(dir: &str) -> String {
    let d = dir.trim().replace('\\', "/");
    let d = d.trim_end_matches('/');
    if cfg!(windows) {
        d.to_lowercase()
    } else {
        d.to_string()
    }
}

fn normalize(dirs: &[String]) -> Vec<String> {
    let mut out: Vec<String> = Vec::new();
    for d in dirs {
        let d = d.trim();
        if d.is_empty() || out.iter().any(|o| key(o) == key(d)) {
            continue;
        }
        out.push(d.to_string());
    }
    out
}

/// 完整列表：默认目录在前，然后是设置、环境变量；后面与前面重复的去掉。
pub(crate) fn compose(default_dir: &str, extra: &[String], env: &[String]) -> Vec<String> {
    let mut all = vec![default_dir.to_string()];
    all.extend(extra.iter().cloned());
    all.extend(env.iter().cloned());
    normalize(&all)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn settings_round_trip_and_merge_in_a_fixed_order_without_duplicates() {
        let dir = std::env::temp_dir().join(format!("lyflow-libset-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        assert!(read_extra(&dir).is_empty(), "文件不在当空");

        let written = write_extra(&dir, &[" D:/libs ".into(), "".into(), "d:\\LIBS\\".into(), "E:/more".into()])
            .unwrap();
        let expect_dedupe = if cfg!(windows) { vec!["D:/libs", "E:/more"] } else { vec!["D:/libs", "d:\\LIBS\\", "E:/more"] };
        assert_eq!(written, expect_dedupe, "去空白、去空串、（Windows）不分大小写与斜杠去重");
        assert_eq!(read_extra(&dir), written);

        let all = compose("C:/app/library", &written, &["E:/more".into(), "F:/env".into()]);
        let mut want = vec!["C:/app/library".to_string()];
        want.extend(written.iter().cloned());
        want.push("F:/env".to_string());
        assert_eq!(all, want, "默认 → 设置 → 环境变量，环境变量里重复的去掉");

        std::fs::write(settings_file(&dir), "not json").unwrap();
        assert!(read_extra(&dir).is_empty(), "读不懂当空");
        let _ = std::fs::remove_dir_all(&dir);
    }
}
