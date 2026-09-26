//! GraphDoc 的 Rust 侧表示与结构校验（三层校验的中间那层，见 docs/graph-doc.md）。
//! 刻意不碰算子语义 —— 那需要 manifest，桥接层一旦理解它就不再只是转发层。

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
    /// 图级命名输出（ADR-0017）。宿主按名字取值，不认节点 id。
    #[serde(default, skip_serializing_if = "std::collections::BTreeMap::is_empty")]
    pub outputs: std::collections::BTreeMap<String, GraphOutput>,
    /// 顶层图参数（m7-plan J7）：`{ 名字: { type?, default, binds: ["节点.参数"], doc? } }`。
    /// 语义归 core 管，这里原样保留，只在 CLI 的 `--param` 上改它的 default。
    #[serde(default, skip_serializing_if = "serde_json::Map::is_empty")]
    pub params: serde_json::Map<String, serde_json::Value>,
    /// 未知字段容器。老客户端打开新版本写的图时不该丢数据。
    #[serde(default)]
    pub x: serde_json::Map<String, serde_json::Value>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct GraphOutput {
    pub node: String,
    pub port: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub label: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Node {
    pub id: String,
    pub op: String,
    #[serde(rename = "opVersion", default, skip_serializing_if = "Option::is_none")]
    pub op_version: Option<String>,
    /// 静音：透传输入到输出。是**执行语义**不是 UI 状态（headless 跑出来必须和界面
    /// 里一样），所以不在 ui 里。M2 只接住并原样转发给 C++，执行语义 M3 实现。
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub bypass: bool,
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

        // 图输出必须指向存在的节点。端口是否存在要 manifest 才知道，留给 C++（ADR-0017）。
        for (name, out) in &self.outputs {
            if name.is_empty() {
                problems.push("存在名字为空的图输出".to_string());
            }
            if !node_ids.contains(out.node.as_str()) {
                problems.push(format!("图输出 {name} 指向不存在的节点: {}", out.node));
            }
            if out.port.is_empty() {
                problems.push(format!("图输出 {name} 没有 port"));
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

    /// 把一条迁移诊断写回顶层节点（ADR-0008 / ADR-0025），与前端 applyMigrations 同一套语义：
    /// op / opVersion / params 整份替换；edits 里删边、插节点、加边。插进来的节点摆在被迁移
    /// 节点的左下方，标题写进 ui.title。nodeId 不是顶层节点（子图里的路径）时什么都不做，返回 false。
    pub fn apply_migration(&mut self, m: &serde_json::Value) -> bool {
        let Some(node_id) = m["nodeId"].as_str() else { return false };
        let Some(node) = self.nodes.iter_mut().find(|n| n.id == node_id) else {
            return false;
        };
        if let Some(op) = m["op"].as_str() {
            node.op = op.to_string();
        }
        if let Some(v) = m["opVersion"].as_str() {
            node.op_version = Some(v.to_string());
        }
        if let Some(params) = m["params"].as_object() {
            node.params = params.clone();
        }
        let near = node
            .ui
            .as_ref()
            .and_then(|ui| ui.get("position"))
            .map(|p| (p["x"].as_f64().unwrap_or(0.0), p["y"].as_f64().unwrap_or(0.0)));

        let edits = &m["edits"];
        if !edits.is_object() {
            return true;
        }
        let same = |a: &PortRef, b: &serde_json::Value| {
            b["node"].as_str() == Some(a.node.as_str()) && b["port"].as_str() == Some(a.port.as_str())
        };
        if let Some(remove) = edits["removeEdges"].as_array() {
            self.edges
                .retain(|e| !remove.iter().any(|r| same(&e.from, &r["from"]) && same(&e.to, &r["to"])));
        }
        if let Some(add) = edits["addNodes"].as_array() {
            for (k, n) in add.iter().enumerate() {
                let (Some(id), Some(op)) = (n["id"].as_str(), n["op"].as_str()) else { continue };
                if self.nodes.iter().any(|x| x.id == id) {
                    continue;
                }
                let mut ui = serde_json::Map::new();
                if let Some((x, y)) = near {
                    ui.insert(
                        "position".into(),
                        serde_json::json!({ "x": x - 220.0, "y": y + 140.0 * (k as f64 + 1.0) }),
                    );
                }
                if let Some(title) = n["title"].as_str().filter(|t| !t.is_empty()) {
                    ui.insert("title".into(), title.into());
                }
                self.nodes.push(Node {
                    id: id.to_string(),
                    op: op.to_string(),
                    op_version: n["opVersion"].as_str().filter(|v| !v.is_empty()).map(String::from),
                    bypass: false,
                    params: n["params"].as_object().cloned().unwrap_or_default(),
                    ui: (!ui.is_empty()).then(|| ui.into()),
                });
            }
        }
        if let Some(add) = edits["addEdges"].as_array() {
            for e in add {
                let port_ref = |v: &serde_json::Value| -> Option<PortRef> {
                    Some(PortRef {
                        node: v["node"].as_str()?.to_string(),
                        port: v["port"].as_str()?.to_string(),
                    })
                };
                let (Some(from), Some(to)) = (port_ref(&e["from"]), port_ref(&e["to"])) else {
                    continue;
                };
                let base = e["id"].as_str().unwrap_or("m").to_string();
                let mut id = base.clone();
                let mut k = 2;
                while self.edges.iter().any(|x| x.id == id) {
                    id = format!("{base}_{k}");
                    k += 1;
                }
                self.edges.push(Edge { id, from, to });
            }
        }
        true
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
            params: Default::default(),
            nodes: nodes
                .iter()
                .map(|(id, op)| Node {
                    id: (*id).into(),
                    op: (*op).into(),
                    op_version: None,
                    bypass: false,
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
            outputs: Default::default(),
            x: Default::default(),
        }
    }

    type Nodes<'a> = &'a [(&'a str, &'a str)];
    type Edges<'a> = &'a [(&'a str, &'a str, &'a str, &'a str, &'a str)];

    /// 结构校验一张表：None 是该过，Some 是错误信息里该有的字。
    #[test]
    fn validate_structure_accepts_and_rejects() {
        let cases: &[(&str, Nodes, Edges, Option<&str>)] = &[
            ("合法的链", &[("n1", "io.load_pcd"), ("n2", "filter.voxel_grid")],
             &[("e1", "n1", "cloud", "n2", "cloud")], None),
            ("悬空的边", &[("n1", "io.load_pcd")], &[("e1", "n1", "cloud", "ghost", "cloud")],
             Some("目标节点不存在")),
            ("重复的节点 id", &[("n1", "io.load_pcd"), ("n1", "filter.voxel_grid")], &[], Some("节点 id 重复")),
            // 输入端口是单连接。多输入合并必须由算子显式声明多个端口来表达，
            // 否则求值顺序是隐式的（docs/graph-doc.md）
            ("两条边进同一个输入", &[("a", "io.load_pcd"), ("b", "io.load_pcd"), ("c", "filter.voxel_grid")],
             &[("e1", "a", "cloud", "c", "cloud"), ("e2", "b", "cloud", "c", "cloud")], Some("单连接")),
            // 一个输出端口连多个输入是合法的
            ("一个输出扇出", &[("a", "io.load_pcd"), ("b", "filter.voxel_grid"), ("c", "filter.passthrough")],
             &[("e1", "a", "cloud", "b", "cloud"), ("e2", "a", "cloud", "c", "cloud")], None),
        ];
        for (name, nodes, edges, want) in cases {
            let got = doc(nodes, edges).validate_structure();
            match want {
                None => assert!(got.is_ok(), "{name}: {:?}", got.err()),
                Some(want) => {
                    let err = got.expect_err(name).to_string();
                    assert!(err.contains(want), "{name}: {err}");
                }
            }
        }
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
        // bypass 默认 false 时不写进文件：稀疏存储，老图的 diff 不该被它污染
        assert!(out["nodes"][0].get("bypass").is_none());
        // 没有声明图输出时 outputs 也不写进文件
        assert!(out.get("outputs").is_none());
    }

    #[test]
    fn keeps_graph_outputs_and_checks_their_node() {
        let raw = r#"{
            "schemaVersion": 1,
            "id": "g1",
            "nodes": [{"id": "n1", "op": "gen.synthetic"}],
            "edges": [],
            "outputs": {"cloud": {"node": "n1", "port": "cloud", "label": "点云"}}
        }"#;
        let doc: GraphDoc = serde_json::from_str(raw).unwrap();
        doc.validate_structure().unwrap();
        let out = serde_json::to_value(&doc).unwrap();
        assert_eq!(out["outputs"]["cloud"]["node"], "n1");
        assert_eq!(out["outputs"]["cloud"]["label"], "点云");

        let mut bad = doc.clone();
        bad.outputs.get_mut("cloud").unwrap().node = "ghost".into();
        let err = bad.validate_structure().unwrap_err().to_string();
        assert!(err.contains("指向不存在的节点"), "{err}");
    }

    /// ADR-0025：迁移诊断里的 edits 删边、插节点、加边；插进来的节点摆在被迁移节点旁边。
    #[test]
    fn apply_migration_rewires_edges_and_inserts_nodes() {
        let mut d = doc(
            &[("a", "t.src"), ("b", "t.src"), ("s", "t.sink")],
            &[("e1", "a", "out", "s", "old1"), ("e2", "b", "out", "s", "old2")],
        );
        d.nodes[2].ui = Some(serde_json::json!({ "position": { "x": 500, "y": 100 } }));
        let m = serde_json::json!({
            "kind": "migration", "nodeId": "s", "op": "t.sink", "opVersion": "2.0.0",
            "params": { "k": 1 },
            "edits": {
                "removeEdges": [
                    { "id": "e1", "from": { "node": "a", "port": "out" }, "to": { "node": "s", "port": "old1" } },
                    { "id": "e2", "from": { "node": "b", "port": "out" }, "to": { "node": "s", "port": "old2" } }
                ],
                "addNodes": [
                    { "id": "s_pack", "op": "t.pack", "opVersion": "1.0.0", "params": {}, "title": "打包", "near": "s" }
                ],
                "addEdges": [
                    { "id": "e1", "from": { "node": "a", "port": "out" }, "to": { "node": "s_pack", "port": "x" } },
                    { "id": "m_s_pack_y", "from": { "node": "b", "port": "out" }, "to": { "node": "s_pack", "port": "y" } },
                    { "id": "m_s_in", "from": { "node": "s_pack", "port": "out" }, "to": { "node": "s", "port": "in" } }
                ]
            }
        });
        assert!(d.apply_migration(&m));
        assert_eq!(d.nodes[2].op_version.as_deref(), Some("2.0.0"));
        assert_eq!(d.nodes[2].params["k"], 1);
        let pack = d.nodes.iter().find(|n| n.id == "s_pack").expect("插入的节点");
        assert_eq!(pack.op_version.as_deref(), Some("1.0.0"));
        let ui = pack.ui.as_ref().unwrap();
        assert_eq!(ui["position"]["x"], 280.0);
        assert_eq!(ui["title"], "打包");
        let wires: Vec<_> = d
            .edges
            .iter()
            .map(|e| format!("{}:{}.{}>{}.{}", e.id, e.from.node, e.from.port, e.to.node, e.to.port))
            .collect();
        assert_eq!(wires, ["e1:a.out>s_pack.x", "m_s_pack_y:b.out>s_pack.y", "m_s_in:s_pack.out>s.in"]);
        assert!(d.validate_structure().is_ok());

        // 子图里的路径 id 不写回
        let nested = serde_json::json!({ "nodeId": "sg/s", "op": "t.sink", "params": {} });
        assert!(!d.apply_migration(&nested));
    }
}
