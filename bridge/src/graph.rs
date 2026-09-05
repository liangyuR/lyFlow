//! GraphDoc 的 Rust 侧表示与**结构校验**。
//!
//! 校验分三层（见 docs/graph-doc.md）：
//!   前端 —— 手感，挡掉明显错误，可被绕过
//!   Rust —— 结构完整性，就是本文件
//!   C++  —— 权威，类型系统 / 参数范围 / 资源可行性
//!
//! 这里刻意**不碰算子语义**：不检查端口类型是否匹配、参数是否在范围内。
//! 那需要 manifest，而一旦桥接层开始理解 manifest，它就不再只是转发层了。

use serde::{Deserialize, Serialize};
use std::collections::HashSet;

pub const SCHEMA_VERSION: u32 = 1;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct GraphDoc {
    #[serde(rename = "schemaVersion")]
    pub schema_version: u32,
    pub id: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub name: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub meta: Option<serde_json::Value>,
    pub nodes: Vec<Node>,
    pub edges: Vec<Edge>,
    #[serde(default)]
    pub groups: Vec<serde_json::Value>,
    #[serde(default)]
    pub subgraphs: serde_json::Map<String, serde_json::Value>,
    /// 未知字段容器。老客户端打开新版本写的图时不该丢数据。
    #[serde(default)]
    pub x: serde_json::Map<String, serde_json::Value>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Node {
    pub id: String,
    pub op: String,
    #[serde(rename = "opVersion", default, skip_serializing_if = "Option::is_none")]
    pub op_version: Option<String>,
    #[serde(default)]
    pub params: serde_json::Map<String, serde_json::Value>,
    /// 纯 UI 状态。桥接层原样透传，从不解释 —— 后端不关心坐标（ADR-0002）。
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub ui: Option<serde_json::Value>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Edge {
    pub id: String,
    pub from: PortRef,
    pub to: PortRef,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PortRef {
    pub node: String,
    pub port: String,
}

#[derive(Debug, thiserror::Error)]
pub enum GraphError {
    #[error("schemaVersion {found} 无法识别（本版本支持 {supported}）")]
    UnsupportedVersion { found: u32, supported: u32 },
    #[error("结构校验失败:\n{}", .0.join("\n"))]
    Invalid(Vec<String>),
}

impl GraphDoc {
    /// 结构完整性校验。不需要 manifest。
    pub fn validate_structure(&self) -> Result<(), GraphError> {
        if self.schema_version != SCHEMA_VERSION {
            return Err(GraphError::UnsupportedVersion {
                found: self.schema_version,
                supported: SCHEMA_VERSION,
            });
        }

        let mut problems = Vec::new();

        let mut node_ids = HashSet::new();
        for node in &self.nodes {
            if node.id.is_empty() {
                problems.push("存在 id 为空的节点".to_string());
            } else if !node_ids.insert(node.id.as_str()) {
                problems.push(format!("节点 id 重复: {}", node.id));
            }
            if node.op.is_empty() {
                problems.push(format!("节点 {} 没有 op", node.id));
            }
        }

        let mut edge_ids = HashSet::new();
        // 一个输入端口最多一条边（见 docs/graph-doc.md）。多输入合并要由算子
        // 显式声明多个端口来表达，靠往一个端口连多条边的话求值顺序是隐式的。
        let mut occupied_inputs = HashSet::new();

        for edge in &self.edges {
            if !edge_ids.insert(edge.id.as_str()) {
                problems.push(format!("边 id 重复: {}", edge.id));
            }
            if !node_ids.contains(edge.from.node.as_str()) {
                problems.push(format!("边 {} 的源节点不存在: {}", edge.id, edge.from.node));
            }
            if !node_ids.contains(edge.to.node.as_str()) {
                problems.push(format!("边 {} 的目标节点不存在: {}", edge.id, edge.to.node));
            }
            if edge.from.node == edge.to.node {
                problems.push(format!("边 {} 自环", edge.id));
            }
            let key = (edge.to.node.as_str(), edge.to.port.as_str());
            if !occupied_inputs.insert(key) {
                problems.push(format!(
                    "输入端口 {}.{} 上有多条边（输入端口是单连接）",
                    edge.to.node, edge.to.port
                ));
            }
        }

        if problems.is_empty() {
            Ok(())
        } else {
            Err(GraphError::Invalid(problems))
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn doc(nodes: &[(&str, &str)], edges: &[(&str, &str, &str, &str, &str)]) -> GraphDoc {
        GraphDoc {
            schema_version: SCHEMA_VERSION,
            id: "test".into(),
            name: None,
            meta: None,
            nodes: nodes
                .iter()
                .map(|(id, op)| Node {
                    id: (*id).into(),
                    op: (*op).into(),
                    op_version: None,
                    params: Default::default(),
                    ui: None,
                })
                .collect(),
            edges: edges
                .iter()
                .map(|(id, fnode, fport, tnode, tport)| Edge {
                    id: (*id).into(),
                    from: PortRef { node: (*fnode).into(), port: (*fport).into() },
                    to: PortRef { node: (*tnode).into(), port: (*tport).into() },
                })
                .collect(),
            groups: Vec::new(),
            subgraphs: Default::default(),
            x: Default::default(),
        }
    }

    #[test]
    fn accepts_a_well_formed_graph() {
        let g = doc(
            &[("n1", "io.load_pcd"), ("n2", "filter.voxel_grid")],
            &[("e1", "n1", "cloud", "n2", "cloud")],
        );
        assert!(g.validate_structure().is_ok());
    }

    #[test]
    fn rejects_dangling_edge() {
        let g = doc(&[("n1", "io.load_pcd")], &[("e1", "n1", "cloud", "ghost", "cloud")]);
        let err = g.validate_structure().unwrap_err().to_string();
        assert!(err.contains("目标节点不存在"), "{err}");
    }

    #[test]
    fn rejects_duplicate_node_ids() {
        let g = doc(&[("n1", "io.load_pcd"), ("n1", "filter.voxel_grid")], &[]);
        let err = g.validate_structure().unwrap_err().to_string();
        assert!(err.contains("节点 id 重复"), "{err}");
    }

    /// 输入端口是单连接。多输入合并必须由算子显式声明多个端口来表达，
    /// 否则求值顺序是隐式的（docs/graph-doc.md）。
    #[test]
    fn rejects_two_edges_into_one_input_port() {
        let g = doc(
            &[("a", "io.load_pcd"), ("b", "io.load_pcd"), ("c", "filter.voxel_grid")],
            &[("e1", "a", "cloud", "c", "cloud"), ("e2", "b", "cloud", "c", "cloud")],
        );
        let err = g.validate_structure().unwrap_err().to_string();
        assert!(err.contains("单连接"), "{err}");
    }

    /// 一个输出端口连多个输入是合法的。
    #[test]
    fn allows_fan_out_from_one_output_port() {
        let g = doc(
            &[("a", "io.load_pcd"), ("b", "filter.voxel_grid"), ("c", "filter.passthrough")],
            &[("e1", "a", "cloud", "b", "cloud"), ("e2", "a", "cloud", "c", "cloud")],
        );
        assert!(g.validate_structure().is_ok());
    }

    #[test]
    fn rejects_unknown_schema_version() {
        let mut g = doc(&[], &[]);
        g.schema_version = 999;
        assert!(matches!(
            g.validate_structure(),
            Err(GraphError::UnsupportedVersion { found: 999, .. })
        ));
    }

    /// ui 是纯 UI 状态，桥接层原样透传，round-trip 不能丢（ADR-0002）。
    #[test]
    fn preserves_ui_and_unknown_fields_through_roundtrip() {
        let raw = r#"{
            "schemaVersion": 1,
            "id": "g1",
            "nodes": [{
                "id": "n1", "op": "io.load_pcd",
                "params": {"path": "a.pcd"},
                "ui": {"position": {"x": 12, "y": 34}, "title": "自定义标题"}
            }],
            "edges": [],
            "x": {"futureField": [1, 2, 3]}
        }"#;
        let doc: GraphDoc = serde_json::from_str(raw).unwrap();
        doc.validate_structure().unwrap();

        let out = serde_json::to_value(&doc).unwrap();
        assert_eq!(out["nodes"][0]["ui"]["position"]["x"], 12);
        assert_eq!(out["nodes"][0]["ui"]["title"], "自定义标题");
        assert_eq!(out["x"]["futureField"][2], 3);
    }
}
