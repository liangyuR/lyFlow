//! Tauri commands —— 前端能调到的全部东西。
//!
//! 返回 String 类型的错误：Tauri 要求错误可序列化，而前端拿到错误只会
//! 显示给人看，不做程序化分支。真需要结构化错误码时再引 code。

use serde::Serialize;
use std::path::PathBuf;
use std::sync::OnceLock;

use crate::core_ffi;
use crate::graph::GraphDoc;

/// manifest 在进程生命周期内不变（热重载是 M3 的事），解析一次就够。
static MANIFEST: OnceLock<serde_json::Value> = OnceLock::new();

#[derive(Serialize)]
pub struct CoreInfo {
    pub version: String,
    #[serde(rename = "operatorCount")]
    pub operator_count: usize,
    #[serde(rename = "typeCount")]
    pub type_count: usize,
}

fn manifest_value() -> Result<&'static serde_json::Value, String> {
    if let Some(v) = MANIFEST.get() {
        return Ok(v);
    }
    let raw = core_ffi::manifest_json().map_err(|e| e.to_string())?;
    // 在边界上解析一次，等于顺手验证了 C++ 那个手写 JSON writer 的输出。
    // 它坏掉的话应该在这里炸，而不是让前端拿到半截 JSON 去猜。
    let value: serde_json::Value = serde_json::from_str(&raw)
        .map_err(|e| format!("core 返回的 manifest 不是合法 JSON: {e}"))?;
    Ok(MANIFEST.get_or_init(|| value))
}

#[tauri::command]
pub fn get_manifest() -> Result<serde_json::Value, String> {
    manifest_value().cloned()
}

#[tauri::command]
pub fn get_core_info() -> Result<CoreInfo, String> {
    let m = manifest_value()?;
    Ok(CoreInfo {
        version: core_ffi::version(),
        operator_count: m["operators"].as_array().map_or(0, Vec::len),
        type_count: m["types"].as_array().map_or(0, Vec::len),
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
pub fn load_graph(path: String) -> Result<GraphDoc, String> {
    let text = std::fs::read_to_string(&path).map_err(|e| format!("读取 {path} 失败: {e}"))?;
    let doc: GraphDoc =
        serde_json::from_str(&text).map_err(|e| format!("{path} 不是合法的 GraphDoc: {e}"))?;
    doc.validate_structure().map_err(|e| e.to_string())?;
    Ok(doc)
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
        let back = load_graph(path.to_string_lossy().into_owned()).expect("load_graph 失败");

        assert_eq!(back.nodes.len(), 2);
        assert_eq!(back.edges.len(), 1);
        assert_eq!(back.nodes[1].params["leafSize"][0], 0.005);
        // ui 是纯 UI 状态，桥接层原样透传不解释（ADR-0002）
        assert_eq!(back.nodes[1].ui.as_ref().unwrap()["title"], "粗降采样");

        // 存盘格式必须是可 diff 的：缩进 + 结尾换行
        let text = std::fs::read_to_string(&path).unwrap();
        assert!(text.contains("\n  \"id\""), "存盘不是 pretty JSON");
        assert!(text.ends_with('\n'), "存盘缺结尾换行");

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
