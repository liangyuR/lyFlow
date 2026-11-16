//! GraphDoc 的 Rust 侧表示与结构校验（三层校验的中间那层，见 docs/graph-doc.md）。
//! 刻意不碰算子语义 —— 那需要 manifest，桥接层一旦理解它就不再只是转发层。

use serde::{Deserialize, Serialize};
use std::collections::HashSet;

pub const SCHEMA_VERSION: u32 = 1;

fn deserialize_present_json<'de, D>(deserializer: D) -> Result<Option<serde_json::Value>, D::Error>
where
    D: serde::Deserializer<'de>,
{
    serde_json::Value::deserialize(deserializer).map(Some)
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct GraphDoc {
    #[serde(rename = "schemaVersion")]
    pub schema_version: u32,
    pub id: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub name: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub meta: Option<serde_json::Value>,
    #[serde(
        rename = "contractUpgrade",
        default,
        deserialize_with = "deserialize_present_json",
        skip_serializing_if = "Option::is_none"
    )]
    pub contract_upgrade: Option<serde_json::Value>,
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
    /// 图里还没被占用的边 id：`base`，占了就 `base_2`、`base_3`……
    /// 图手术（迁移加边、perturb 插节点、patch --connect）都走它，免得三处各写一份。
    pub(crate) fn unique_edge_id(&self, base: &str) -> String {
        let mut id = base.to_string();
        let mut k = 2usize;
        while self.edges.iter().any(|e| e.id == id) {
            id = format!("{base}_{k}");
            k += 1;
        }
        id
    }

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
        // 子图里标的输出是路径 id（「子图节点/里面的节点」，docs/graph-doc.md）：这里只查第一段在顶层，
        // 后面几段要展开子图才知道，同样留给 C++
        for (name, out) in &self.outputs {
            if name.is_empty() {
                problems.push("存在名字为空的图输出".to_string());
            }
            let head = out.node.split('/').next().unwrap_or_default();
            if !node_ids.contains(head) {
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
        if let Some(replacement) = m["graphUpdate"].get("contractUpgradeReplacement") {
            if serde_json::to_value(&*self).ok().as_ref() != Some(&replacement["before"]) {
                return false;
            }
            let Ok(candidate) = serde_json::from_value::<GraphDoc>(replacement["after"].clone())
            else {
                return false;
            };
            if candidate.validate_structure().is_err() {
                return false;
            }
            *self = candidate;
            return true;
        }
        let Some(node_id) = m["nodeId"].as_str() else {
            return false;
        };
        if node_id.is_empty() {
            return self.retire_contract_upgrade(&m["graphUpdate"]);
        }
        let pending_source = self
            .contract_upgrade
            .as_ref()
            .and_then(|request| request["rules"].as_array())
            .is_some_and(|rules| {
                rules.iter().any(|rule| {
                    rule.get("scope").is_none() && rule["source"]["node"].as_str() == Some(node_id)
                })
            });
        if pending_source
            && self.nodes.iter().any(|node| {
                node.id == node_id
                    && (m["op"].as_str().is_some_and(|op| op != node.op)
                        || m["opVersion"]
                            .as_str()
                            .is_some_and(|version| Some(version) != node.op_version.as_deref()))
            })
        {
            return false;
        }
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
        let near = node.ui.as_ref().and_then(|ui| ui.get("position")).map(|p| {
            (
                p["x"].as_f64().unwrap_or(0.0),
                p["y"].as_f64().unwrap_or(0.0),
            )
        });

        let edits = &m["edits"];
        if !edits.is_object() {
            self.retire_contract_upgrade(&m["graphUpdate"]);
            return true;
        }
        let same = |a: &PortRef, b: &serde_json::Value| {
            b["node"].as_str() == Some(a.node.as_str())
                && b["port"].as_str() == Some(a.port.as_str())
        };
        if let Some(remove) = edits["removeEdges"].as_array() {
            self.edges.retain(|e| {
                !remove
                    .iter()
                    .any(|r| same(&e.from, &r["from"]) && same(&e.to, &r["to"]))
            });
        }
        if let Some(add) = edits["addNodes"].as_array() {
            for (k, n) in add.iter().enumerate() {
                let (Some(id), Some(op)) = (n["id"].as_str(), n["op"].as_str()) else {
                    continue;
                };
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
                    op_version: n["opVersion"]
                        .as_str()
                        .filter(|v| !v.is_empty())
                        .map(String::from),
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
                let id = self.unique_edge_id(e["id"].as_str().unwrap_or("m"));
                self.edges.push(Edge { id, from, to });
            }
        }
        self.retire_contract_upgrade(&m["graphUpdate"]);
        true
    }

    fn retire_contract_upgrade(&mut self, update: &serde_json::Value) -> bool {
        let retirement = &update["contractUpgradeRetirement"];
        let Some(request) = self.contract_upgrade.as_ref() else {
            return false;
        };
        let Some(requirements) = retirement["requirements"].as_array() else {
            return false;
        };
        if request != &retirement["request"] || !retirement["historyEntry"].is_object() {
            return false;
        }
        let same = |a: &PortRef, b: &serde_json::Value| {
            b["node"].as_str() == Some(a.node.as_str())
                && b["port"].as_str() == Some(a.port.as_str())
        };
        for required in requirements {
            let source = &required["source"];
            let adapter = &required["adapter"];
            let Some(source_node) = self
                .nodes
                .iter()
                .find(|n| Some(n.id.as_str()) == source["node"].as_str())
            else {
                return false;
            };
            let Some(adapter_node) = self
                .nodes
                .iter()
                .find(|n| Some(n.id.as_str()) == adapter["id"].as_str())
            else {
                return false;
            };
            let from = serde_json::json!({"node":source["node"],"port":source["port"]});
            let middle = serde_json::json!({"node":adapter["id"],"port":required["port"]});
            let target = &required["target"];
            if Some(source_node.op.as_str()) != source["op"].as_str()
                || source_node.op_version.as_deref() != source["opVersion"].as_str()
                || Some(adapter_node.op.as_str()) != adapter["op"].as_str()
                || adapter_node.op_version.as_deref() != adapter["opVersion"].as_str()
                || adapter_node.bypass
                || serde_json::Value::Object(adapter_node.params.clone()) != adapter["params"]
                || !self
                    .nodes
                    .iter()
                    .any(|n| Some(n.id.as_str()) == target["node"].as_str())
                || self
                    .edges
                    .iter()
                    .any(|e| same(&e.from, &from) && same(&e.to, target))
                || self
                    .edges
                    .iter()
                    .filter(|e| same(&e.from, &from) && same(&e.to, &middle))
                    .count()
                    != 1
                || self
                    .edges
                    .iter()
                    .filter(|e| same(&e.from, &middle) && same(&e.to, target))
                    .count()
                    != 1
            {
                return false;
            }
        }
        if self.meta.as_ref().is_some_and(|meta| !meta.is_object()) {
            return false;
        }
        let meta = self.meta.get_or_insert_with(|| serde_json::json!({}));
        if meta
            .get("contractUpgradeHistory")
            .is_some_and(|history| !history.is_array())
        {
            return false;
        }
        let history = meta
            .as_object_mut()
            .unwrap()
            .entry("contractUpgradeHistory")
            .or_insert_with(|| serde_json::json!([]))
            .as_array_mut()
            .unwrap();
        if !history.contains(&retirement["historyEntry"]) {
            history.push(retirement["historyEntry"].clone());
        }
        self.contract_upgrade = None;
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
            contract_upgrade: None,
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
                    from: PortRef {
                        node: (*fnode).into(),
                        port: (*fport).into(),
                    },
                    to: PortRef {
                        node: (*tnode).into(),
                        port: (*tport).into(),
                    },
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
            (
                "合法的链",
                &[("n1", "io.load_pcd"), ("n2", "filter.voxel_grid")],
                &[("e1", "n1", "cloud", "n2", "cloud")],
                None,
            ),
            (
                "悬空的边",
                &[("n1", "io.load_pcd")],
                &[("e1", "n1", "cloud", "ghost", "cloud")],
                Some("目标节点不存在"),
            ),
            (
                "重复的节点 id",
                &[("n1", "io.load_pcd"), ("n1", "filter.voxel_grid")],
                &[],
                Some("节点 id 重复"),
            ),
            // 输入端口是单连接。多输入合并必须由算子显式声明多个端口来表达，
            // 否则求值顺序是隐式的（docs/graph-doc.md）
            (
                "两条边进同一个输入",
                &[
                    ("a", "io.load_pcd"),
                    ("b", "io.load_pcd"),
                    ("c", "filter.voxel_grid"),
                ],
                &[
                    ("e1", "a", "cloud", "c", "cloud"),
                    ("e2", "b", "cloud", "c", "cloud"),
                ],
                Some("单连接"),
            ),
            // 一个输出端口连多个输入是合法的
            (
                "一个输出扇出",
                &[
                    ("a", "io.load_pcd"),
                    ("b", "filter.voxel_grid"),
                    ("c", "filter.passthrough"),
                ],
                &[
                    ("e1", "a", "cloud", "b", "cloud"),
                    ("e2", "a", "cloud", "c", "cloud"),
                ],
                None,
            ),
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
            "x": {"futureField": [1, 2, 3]},
            "contractUpgrade": {"schemaVersion":1,"rules":[]}
        }"#;
        let doc: GraphDoc = serde_json::from_str(raw).unwrap();
        doc.validate_structure().unwrap();

        let out = serde_json::to_value(&doc).unwrap();
        assert_eq!(out["nodes"][0]["ui"]["position"]["x"], 12);
        assert_eq!(out["nodes"][0]["ui"]["title"], "自定义标题");
        assert_eq!(out["x"]["futureField"][2], 3);
        assert_eq!(
            out["contractUpgrade"],
            serde_json::json!({"schemaVersion":1,"rules":[]})
        );
        let roundtrip: GraphDoc = serde_json::from_value(out.clone()).unwrap();
        assert_eq!(roundtrip.contract_upgrade, doc.contract_upgrade);
        for payload in [
            serde_json::Value::Null,
            serde_json::json!([]),
            serde_json::json!({"schemaVersion":999,"rules":[]}),
        ] {
            let mut present = out.clone();
            present["contractUpgrade"] = payload.clone();
            let parsed: GraphDoc = serde_json::from_value(present).unwrap();
            assert_eq!(parsed.contract_upgrade, Some(payload.clone()));
            assert_eq!(
                serde_json::to_value(parsed).unwrap()["contractUpgrade"],
                payload
            );
        }
        let mut absent = roundtrip;
        absent.contract_upgrade = None;
        assert!(serde_json::to_value(absent)
            .unwrap()
            .get("contractUpgrade")
            .is_none());
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

        // 子图里标的输出（路径 id）：第一段在顶层就收下，里面几段留给 C++ 展开时查
        let mut nested = doc.clone();
        nested.outputs.get_mut("cloud").unwrap().node = "n1/inner".into();
        nested.validate_structure().unwrap();
        nested.outputs.get_mut("cloud").unwrap().node = "ghost/inner".into();
        assert!(nested.validate_structure().is_err());
    }

    /// ADR-0025：迁移诊断里的 edits 删边、插节点、加边；插进来的节点摆在被迁移节点旁边。
    #[test]
    fn apply_migration_rewires_edges_and_inserts_nodes() {
        let mut d = doc(
            &[("a", "t.src"), ("b", "t.src"), ("s", "t.sink")],
            &[
                ("e1", "a", "out", "s", "old1"),
                ("e2", "b", "out", "s", "old2"),
            ],
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
        let pack = d
            .nodes
            .iter()
            .find(|n| n.id == "s_pack")
            .expect("插入的节点");
        assert_eq!(pack.op_version.as_deref(), Some("1.0.0"));
        let ui = pack.ui.as_ref().unwrap();
        assert_eq!(ui["position"]["x"], 280.0);
        assert_eq!(ui["title"], "打包");
        let wires: Vec<_> = d
            .edges
            .iter()
            .map(|e| {
                format!(
                    "{}:{}.{}>{}.{}",
                    e.id, e.from.node, e.from.port, e.to.node, e.to.port
                )
            })
            .collect();
        assert_eq!(
            wires,
            [
                "e1:a.out>s_pack.x",
                "m_s_pack_y:b.out>s_pack.y",
                "m_s_in:s_pack.out>s.in"
            ]
        );
        assert!(d.validate_structure().is_ok());

        // 子图里的路径 id 不写回
        let nested = serde_json::json!({ "nodeId": "sg/s", "op": "t.sink", "params": {} });
        assert!(!d.apply_migration(&nested));
    }

    #[test]
    fn contract_upgrade_partial_write_retires_to_passive_history_after_completion() {
        let mut raw: serde_json::Value = serde_json::from_str(include_str!(
            "../../core/tests/fixtures/contract-upgrade/plane-calib.lyflow.json"
        ))
        .unwrap();
        raw["meta"] = serde_json::json!({"owner":"keep", "contractUpgradeHistory":[{"schemaVersion":2,"future":true}]});
        raw["nodes"].as_array_mut().unwrap().push(
            serde_json::json!({"id":"other","op":"util.reroute","opVersion":"1.0.0","params":{}}),
        );
        raw["edges"].as_array_mut().unwrap().push(serde_json::json!({"id":"other-edge","from":{"node":"load","port":"calib"},"to":{"node":"other","port":"in"}}));
        let mut second_rule = raw["contractUpgrade"]["rules"][0].clone();
        second_rule["adapterId"] = "other_calib".into();
        second_rule["target"]["node"] = "other".into();
        raw["contractUpgrade"]["rules"]
            .as_array_mut()
            .unwrap()
            .push(second_rule);
        let request = raw["contractUpgrade"].clone();
        let params =
            serde_json::json!({"missingUnit":"mm","imageFrame":"camera","planeFrame":"table"});
        let mut requirements = Vec::new();
        let mut actions = Vec::new();
        for (index, rule) in request["rules"].as_array().unwrap().iter().enumerate() {
            let adapter = serde_json::json!({"id":rule["adapterId"],"op":"image.upgrade_plane_calib","opVersion":"1.0.0","params":params});
            let from = raw["edges"][index]["from"].clone();
            let middle = serde_json::json!({"node":rule["adapterId"],"port":"calib"});
            requirements.push(serde_json::json!({"source":rule["source"],"target":rule["target"],"adapter":adapter,"port":"calib"}));
            actions.push(serde_json::json!({"nodeId":rule["target"]["node"],"op":"util.reroute","opVersion":"1.0.0","params":{},
                "edits":{"removeEdges":[raw["edges"][index]],"addNodes":[adapter],"addEdges":[
                    {"id":format!("contract_{}_in",rule["adapterId"].as_str().unwrap()),"from":from,"to":middle},
                    {"id":raw["edges"][index]["id"],"from":middle,"to":rule["target"]}]}}));
        }
        let entry = serde_json::json!({"schemaVersion":1,"request":request,"appliedSources":request["rules"].as_array().unwrap().iter().map(|r|r["source"].clone()).collect::<Vec<_>>()});
        let update = serde_json::json!({"contractUpgradeRetirement":{"request":request,"historyEntry":entry,"requirements":requirements}});
        actions[1]["graphUpdate"] = update.clone();
        let mut d: GraphDoc = serde_json::from_value(raw).unwrap();
        let source_migration = serde_json::json!({"nodeId":"load","op":"image.renamed_calib","opVersion":"2.0.0","params":{}});
        assert!(!d.apply_migration(&source_migration));
        assert_eq!(d.nodes[0].op_version.as_deref(), Some("1.0.0"));
        assert!(d.apply_migration(&actions[1]));
        assert_eq!(d.contract_upgrade.as_ref(), Some(&request));
        assert_eq!(
            d.meta.as_ref().unwrap()["contractUpgradeHistory"]
                .as_array()
                .unwrap()
                .len(),
            1
        );
        let mut resumed: GraphDoc =
            serde_json::from_value(serde_json::to_value(d).unwrap()).unwrap();
        actions[0]["graphUpdate"] = update.clone();
        assert!(resumed.apply_migration(&actions[0]));
        assert!(resumed.contract_upgrade.is_none());
        assert!(resumed.apply_migration(&source_migration));
        assert_eq!(resumed.nodes[0].op, "image.renamed_calib");
        assert_eq!(resumed.nodes[0].op_version.as_deref(), Some("2.0.0"));
        assert_eq!(resumed.meta.as_ref().unwrap()["owner"], "keep");
        assert_eq!(
            resumed.meta.as_ref().unwrap()["contractUpgradeHistory"]
                .as_array()
                .unwrap()
                .len(),
            2
        );
        resumed.nodes[0].op = "image.load_calib".into();
        resumed.nodes[0].op_version = Some("1.0.0".into());
        resumed.contract_upgrade = Some(request);
        let graph_only = serde_json::json!({"nodeId":"","graphUpdate":update});
        let mut disabled = resumed.clone();
        disabled.meta = Some(serde_json::json!({"owner":"keep","contractUpgradeHistory":[]}));
        disabled
            .nodes
            .iter_mut()
            .find(|node| node.id == "explicit_calib")
            .unwrap()
            .bypass = true;
        let untouched = serde_json::to_value(&disabled).unwrap();
        assert!(!disabled.apply_migration(&graph_only));
        assert_eq!(serde_json::to_value(&disabled).unwrap(), untouched);
        for bypass in [
            serde_json::Value::Null,
            serde_json::json!("false"),
            serde_json::json!(0),
        ] {
            let mut malformed = untouched.clone();
            malformed["nodes"]
                .as_array_mut()
                .unwrap()
                .iter_mut()
                .find(|node| node["id"] == "explicit_calib")
                .unwrap()["bypass"] = bypass;
            assert!(serde_json::from_value::<GraphDoc>(malformed).is_err());
        }
        disabled
            .nodes
            .iter_mut()
            .find(|node| node.id == "explicit_calib")
            .unwrap()
            .bypass = false;
        assert!(disabled.apply_migration(&graph_only));
        assert!(disabled.contract_upgrade.is_none());
        assert_eq!(
            disabled.meta.as_ref().unwrap()["contractUpgradeHistory"]
                .as_array()
                .unwrap()
                .len(),
            1
        );
        assert!(resumed.apply_migration(&graph_only));
        assert!(resumed.contract_upgrade.is_none());
        assert_eq!(
            resumed.meta.as_ref().unwrap()["contractUpgradeHistory"]
                .as_array()
                .unwrap()
                .len(),
            2
        );
        resumed.nodes.retain(|n| n.id != "consumer");
        resumed
            .edges
            .retain(|e| e.from.node != "consumer" && e.to.node != "consumer");
        resumed.validate_structure().unwrap();
        let nested = serde_json::to_value(&resumed).unwrap();
        resumed.subgraphs.insert("inside".into(), serde_json::json!({"nodes":nested["nodes"],"edges":nested["edges"],"inputs":[],"outputs":[],"params":[]}));
        resumed.nodes = vec![Node {
            id: "group".into(),
            op: "sub:inside".into(),
            op_version: None,
            bypass: false,
            params: Default::default(),
            ui: None,
        }];
        resumed.edges.clear();
        resumed.validate_structure().unwrap();
        assert!(resumed.contract_upgrade.is_none());
    }
    #[test]
    fn contract_upgrade_replacement_is_atomic_and_rejects_stale_documents() {
        let mut d: GraphDoc = serde_json::from_str(include_str!(
            "../../core/tests/fixtures/contract-upgrade/dts-profile-v1.lyflow.json"
        ))
        .unwrap();
        let before = serde_json::to_value(&d).unwrap();
        let mut after = before.clone();
        after["nodes"][1]["op"] = serde_json::json!("profile.clean");
        after["meta"]["contractUpgradeHistory"] = serde_json::json!([{"schemaVersion":1,"request":{"schemaVersion":1,"rules":[]},"appliedSources":[]}]);
        let action = serde_json::json!({"nodeId":"","graphUpdate":{"contractUpgradeReplacement":{"before":before,"after":after}}});
        let mut partial = d.clone();
        partial.nodes[1].op = "profile.clean".into();
        let snapshot = serde_json::to_value(&partial).unwrap();
        assert!(!partial.apply_migration(&action));
        assert_eq!(serde_json::to_value(&partial).unwrap(), snapshot);
        assert!(d.apply_migration(&action));
        assert_eq!(serde_json::to_value(&d).unwrap(), after);
        assert!(!d.apply_migration(&action));
        let saved = serde_json::to_string(&d).unwrap();
        let loaded: GraphDoc = serde_json::from_str(&saved).unwrap();
        assert_eq!(serde_json::to_value(loaded).unwrap(), after);
    }
}
