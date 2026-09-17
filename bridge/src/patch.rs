//! `lyflow patch` —— 图的结构编辑（ADR-0023）。
//!
//! 四个动作、固定顺序（remove → add → rewire → set）、幂等、每步之后过一遍形状校验，
//! 最后过 core 的 `validate`；任一步不过就整体不写。图手术的那几件事（找空位、
//! 改边的源端口）与 `perturb::insert_after` 是同一套写法，只是这里由命令行指定。

use std::collections::{BTreeMap, HashSet};
use std::path::{Path, PathBuf};

use serde_json::{json, Value};

use crate::cli::{
    core, defaults_by_op, diagnostics_of, diff_docs, fail, has_errors, json_line, line, load_graph,
    render_diff, Loaded, Parsed, Sink, EXIT_FAILED, EXIT_INVALID, EXIT_OK, EXIT_USAGE,
};
use crate::eval::wildcard_match_cs;
use crate::graph::{GraphDoc, Node, PortRef};

/// 新节点没给坐标时放在图的右下角空白处。UI 字段，不影响执行。
const CORNER_DX: f64 = 260.0;
const CORNER_DY: f64 = 140.0;

/// `--add-node` 的 JSON 里认哪些键。多一个键就报错 ——
/// 「参数名拼错静默无效」是 M6 要消掉的那一类 bug，不该在这里新造一个。
const NODE_KEYS: &[&str] = &["id", "op", "opVersion", "bypass", "params", "ui"];

#[derive(Default)]
pub(crate) struct Applied {
    pub removed: Vec<String>,
    pub added: Vec<String>,
    pub rewired: Vec<String>,
    pub set: Vec<String>,
    /// 什么都没做的那些动作。幂等的形态是「第二遍全进这里」。
    pub noops: Vec<Value>,
    /// 给人看的旁注，走 stderr。
    pub hints: Vec<String>,
}

impl Applied {
    fn noop(&mut self, action: &str, spec: &str, reason: &str, message: &str) {
        self.noops.push(json!({
            "action": action,
            "spec": spec,
            "reason": reason,
            "message": message,
        }));
    }

    fn nothing_applied(&self) -> bool {
        self.removed.is_empty()
            && self.added.is_empty()
            && self.rewired.is_empty()
            && self.set.is_empty()
    }

    fn value(&self) -> Value {
        json!({
            "removed": self.removed,
            "added": self.added,
            "rewired": self.rewired,
            "set": self.set,
        })
    }
}

type Step = fn(&mut GraphDoc, &[String], &mut Applied) -> Result<(), String>;

// ------------------------------------------------------------------ 四个动作

/// `--remove-node <id|glob>`：删节点与它的所有边。glob 只对 id，**大小写敏感**
/// （m6-plan §10 第 6 条）：`N_FB_*` 不该配上 `n_fb_*`，多删一批比少删一个更难发现。
/// 图级 `outputs` 还指着被删节点时报错 —— 静默删掉一个图输出是宿主端最难查的一类改动。
pub(crate) fn apply_removes(
    doc: &mut GraphDoc,
    specs: &[String],
    applied: &mut Applied,
) -> Result<(), String> {
    for spec in specs {
        let hits: HashSet<String> = doc
            .nodes
            .iter()
            .filter(|n| wildcard_match_cs(spec, &n.id))
            .map(|n| n.id.clone())
            .collect();
        if hits.is_empty() {
            applied.noop(
                "remove-node",
                spec,
                "no_match",
                &format!("--remove-node {spec}：图里没有匹配的节点，跳过"),
            );
            continue;
        }
        let referenced: Vec<String> = doc
            .outputs
            .iter()
            .filter(|(_, o)| hits.contains(&o.node))
            .map(|(name, o)| format!("{name} -> {}:{}", o.node, o.port))
            .collect();
        if !referenced.is_empty() {
            return Err(format!(
                "--remove-node {spec} 会删掉图级输出还指着的节点：{}。\n\
                 图输出是宿主按名字取值的契约，不静默删 —— 先改 outputs，或者换一个选择",
                referenced.join("；")
            ));
        }
        doc.nodes.retain(|n| !hits.contains(&n.id));
        doc.edges
            .retain(|e| !hits.contains(&e.from.node) && !hits.contains(&e.to.node));
        let mut ids: Vec<String> = hits.into_iter().collect();
        ids.sort();
        applied.removed.extend(ids);
    }
    Ok(())
}

/// `--add-node <json>`：一个节点对象。id 撞了报错（**不**是 no-op：
/// 「这个 id 已经有人了」与「我要的那个节点已经在了」是两件事，猜错代价太大）。
pub(crate) fn apply_adds(
    doc: &mut GraphDoc,
    specs: &[String],
    applied: &mut Applied,
) -> Result<(), String> {
    for spec in specs {
        let value: Value = serde_json::from_str(spec)
            .map_err(|e| format!("--add-node 不是合法 JSON: {e}\n收到：{spec}"))?;
        let Some(obj) = value.as_object() else {
            return Err(format!("--add-node 要是一个 JSON 对象，收到 {spec}"));
        };
        for key in obj.keys() {
            if !NODE_KEYS.contains(&key.as_str()) {
                return Err(format!(
                    "--add-node 不认识的字段 {key}（认的是 {}）",
                    NODE_KEYS.join(" / ")
                ));
            }
        }
        let mut node: Node = serde_json::from_value(value.clone())
            .map_err(|e| format!("--add-node 不是一个节点：{e}\n收到：{spec}"))?;
        if node.id.is_empty() {
            return Err("--add-node 的 id 是空的".to_string());
        }
        if node.op.is_empty() {
            return Err(format!("--add-node {} 没有 op", node.id));
        }
        if doc.nodes.iter().any(|n| n.id == node.id) {
            return Err(format!(
                "--add-node {}：图里已经有这个 id 了。改个 id，或者先 --remove-node 它",
                node.id
            ));
        }
        if node.ui.is_none() {
            node.ui = Some(json!({ "position": free_corner(doc) }));
        }
        applied.added.push(node.id.clone());
        doc.nodes.push(node);
    }
    Ok(())
}

/// `--rewire <节点>:<端口>=<节点>:<端口>`：所有从左端口出发的边改为从右端口出发。
/// 左端口没有出边就是 no-op —— 跑第二遍时正是这个形态。
pub(crate) fn apply_rewires(
    doc: &mut GraphDoc,
    specs: &[String],
    applied: &mut Applied,
) -> Result<(), String> {
    for spec in specs {
        let (left, right) = spec.split_once('=').ok_or_else(|| {
            format!("--rewire 的写法是 <节点>:<端口>=<节点>:<端口>，收到 {spec}")
        })?;
        let (from_node, from_port) = parse_port(left, spec)?;
        let (to_node, to_port) = parse_port(right, spec)?;
        for (id, what) in [(&from_node, "左端口"), (&to_node, "右端口")] {
            if !doc.nodes.iter().any(|n| &n.id == id) {
                return Err(format!("--rewire {spec} 的{what}：图里没有节点 {id}"));
            }
        }
        let hits = doc
            .edges
            .iter()
            .filter(|e| e.from.node == from_node && e.from.port == from_port)
            .count();
        if hits == 0 {
            applied.noop(
                "rewire",
                spec,
                "no_outgoing_edge",
                &format!("--rewire {spec}：没有从 {from_node}:{from_port} 出发的边，跳过"),
            );
            continue;
        }
        for edge in doc.edges.iter_mut() {
            if edge.from.node == from_node && edge.from.port == from_port {
                edge.from = PortRef {
                    node: to_node.clone(),
                    port: to_port.clone(),
                };
            }
        }
        if doc
            .outputs
            .values()
            .any(|o| o.node == from_node && o.port == from_port)
        {
            applied.hints.push(format!(
                "--rewire {spec} 只改了边；图级 outputs 里还有指着 {from_node}:{from_port} 的，没动它",
            ));
        }
        applied.rewired.push(spec.clone());
    }
    Ok(())
}

/// `--set <节点>.<参数>=<json>`，语义与 `run --set` 相同（解析不出 JSON 就当字符串）。
/// 图里已经是同一个值时是 no-op。
pub(crate) fn apply_sets(
    doc: &mut GraphDoc,
    specs: &[String],
    applied: &mut Applied,
) -> Result<(), String> {
    for spec in specs {
        let (left, raw) = spec
            .split_once('=')
            .ok_or_else(|| format!("--set 的写法是 <节点>.<参数>=<json>，收到 {spec}"))?;
        let (node_id, param) = left
            .rsplit_once('.')
            .ok_or_else(|| format!("--set 的写法是 <节点>.<参数>=<json>，收到 {spec}"))?;
        if param.is_empty() {
            return Err(format!("--set 的参数名是空的，收到 {spec}"));
        }
        let value: Value =
            serde_json::from_str(raw).unwrap_or_else(|_| Value::String(raw.to_string()));
        let node = doc
            .nodes
            .iter_mut()
            .find(|n| n.id == node_id)
            .ok_or_else(|| format!("--set {spec}：图里没有节点 {node_id}"))?;
        if node.params.get(param) == Some(&value) {
            applied.noop(
                "set",
                spec,
                "same_value",
                &format!("--set {spec}：图里已经是这个值，跳过"),
            );
            continue;
        }
        node.params.insert(param.to_string(), value);
        applied.set.push(left.to_string());
    }
    Ok(())
}

fn parse_port(spec: &str, whole: &str) -> Result<(String, String), String> {
    let (node, port) = spec.split_once(':').ok_or_else(|| {
        format!("--rewire 的两端都要写成 <节点>:<端口>，收到 {spec}（整条是 {whole}）")
    })?;
    if node.is_empty() || port.is_empty() {
        return Err(format!(
            "--rewire 的两端都要写成 <节点>:<端口>，收到 {spec}（整条是 {whole}）"
        ));
    }
    if node.contains('/') || port.contains('/') {
        return Err(format!(
            "--rewire {spec} 指向子图内部端口：这一版只改顶层图的边"
        ));
    }
    Ok((node.to_string(), port.to_string()))
}

/// 图里现有节点坐标的右下角外面一格。一次加多个节点时会沿对角线排开。
pub(crate) fn free_corner(doc: &GraphDoc) -> Value {
    let mut max = None::<(f64, f64)>;
    for node in &doc.nodes {
        let Some(ui) = node.ui.as_ref() else { continue };
        let pos = &ui["position"];
        let (Some(x), Some(y)) = (pos["x"].as_f64(), pos["y"].as_f64()) else {
            continue;
        };
        max = Some(match max {
            None => (x, y),
            Some((mx, my)) => (mx.max(x), my.max(y)),
        });
    }
    match max {
        None => json!({ "x": 0.0, "y": 0.0 }),
        Some((x, y)) => json!({ "x": x + CORNER_DX, "y": y + CORNER_DY }),
    }
}

// ------------------------------------------------------------------ 落盘

/// 先写同目录下的临时文件再改名。写一半的图比没写更糟 —— 原地覆写时尤其。
pub(crate) fn write_atomic(dest: &Path, text: &str) -> Result<(), String> {
    if let Some(dir) = dest.parent() {
        if !dir.as_os_str().is_empty() {
            std::fs::create_dir_all(dir)
                .map_err(|e| format!("建目录 {} 失败: {e}", dir.display()))?;
        }
    }
    let name = dest
        .file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_else(|| "graph".to_string());
    let tmp = dest.with_file_name(format!(".{name}.patch-{}.tmp", std::process::id()));
    std::fs::write(&tmp, text).map_err(|e| format!("写 {} 失败: {e}", tmp.display()))?;
    match std::fs::rename(&tmp, dest) {
        Ok(()) => Ok(()),
        Err(e) => {
            let _ = std::fs::remove_file(&tmp);
            Err(format!("改名到 {} 失败: {e}", dest.display()))
        }
    }
}

// ------------------------------------------------------------------ 子命令

pub(crate) fn cmd_patch(parsed: &Parsed, out: &Sink, err: &Sink) -> i32 {
    let Some(path) = parsed.positional.first().cloned() else {
        line(
            err,
            "用法：lyflow patch <graph> [--remove-node <id|glob>]... [--add-node <json>]... \
             [--rewire <from>=<to>]... [--set <node>.<param>=<json>]... [--dry-run] [-o <out>] [--json]",
        );
        return EXIT_USAGE;
    };
    let actions: [(&str, Step, &[String]); 4] = [
        ("remove-node", apply_removes, parsed.many("remove-node")),
        ("add-node", apply_adds, parsed.many("add-node")),
        ("rewire", apply_rewires, parsed.many("rewire")),
        ("set", apply_sets, parsed.many("set")),
    ];
    if actions.iter().all(|(_, _, specs)| specs.is_empty()) {
        line(
            err,
            "至少给一个动作：--remove-node / --add-node / --rewire / --set",
        );
        return EXIT_USAGE;
    }

    let core = match core() {
        Ok(c) => c,
        Err(e) => return fail(err, &e, EXIT_FAILED),
    };
    // 刻意不让 load_graph 应用 --set：这里的 --set 是四个动作里的一个，
    // 要判「同值 no-op」，而且它排在 rewire 之后。
    let mut bare = Parsed {
        positional: Vec::new(),
        values: BTreeMap::new(),
        flags: HashSet::new(),
    };
    if let Some(dir) = parsed.one("base-dir") {
        bare.values.insert("base-dir".to_string(), vec![dir.to_string()]);
    }
    let before = match load_graph(&bare, &path) {
        Ok(l) => l,
        Err(e) => return fail(err, &e, EXIT_INVALID),
    };

    let mut doc = before.doc.clone();
    let mut applied = Applied::default();
    for (label, step, specs) in actions {
        if specs.is_empty() {
            continue;
        }
        if let Err(e) = step(&mut doc, specs, &mut applied) {
            return fail(err, &e, EXIT_INVALID);
        }
        // 每一步之后过一遍形状校验：坏在哪一个动作上，比「最后整张图不合法」有用得多
        if let Err(e) = doc.validate_structure() {
            return fail(err, &format!("{label} 之后图不合法：\n{e}"), EXIT_INVALID);
        }
    }
    for noop in &applied.noops {
        line(err, noop["message"].as_str().unwrap_or_default());
    }
    for hint in &applied.hints {
        line(err, hint);
    }

    let json = match serde_json::to_string(&doc) {
        Ok(j) => j,
        Err(e) => return fail(err, &e.to_string(), EXIT_FAILED),
    };
    let after = Loaded {
        doc,
        json,
        base_dir: before.base_dir.clone(),
        path: before.path.clone(),
    };
    let diags = match diagnostics_of(&core, &after) {
        Ok(d) => d,
        Err(e) => return fail(err, &e, EXIT_FAILED),
    };
    if has_errors(&diags) {
        json_line(out, &Value::Array(diags.clone()));
        line(
            err,
            &format!("改完之后图不合法（{} 条诊断），没有写任何文件", diags.len()),
        );
        return EXIT_INVALID;
    }

    let defaults = match defaults_by_op(&core) {
        Ok(d) => d,
        Err(e) => return fail(err, &e, EXIT_FAILED),
    };
    let diff = diff_docs(&defaults, &before.doc, &after.doc);

    let dry_run = parsed.has("dry-run");
    // 全是 no-op 的原地覆写不落盘：内容一个字节都不会变，但会动 mtime，
    // 而编辑器与 watcher 盯的正是 mtime。给了 -o 就照写，那是「我要这个文件」。
    let idle = applied.nothing_applied() && diff["empty"] == true;
    let in_place = parsed.one("output").is_none();
    let mut wrote = Value::Null;
    if !dry_run && !(idle && in_place) {
        let dest = parsed
            .one("output")
            .map(PathBuf::from)
            .unwrap_or_else(|| before.path.clone());
        let mut text = match serde_json::to_string_pretty(&after.doc) {
            Ok(t) => t,
            Err(e) => return fail(err, &e.to_string(), EXIT_FAILED),
        };
        text.push('\n');
        if let Err(e) = write_atomic(&dest, &text) {
            return fail(err, &e, EXIT_FAILED);
        }
        wrote = json!(dest.to_string_lossy());
    }

    if parsed.has("json") {
        json_line(
            out,
            &json!({
                "kind": "patch_result",
                "applied": applied.value(),
                "noops": applied.noops,
                "wrote": wrote,
                "diff": diff,
            }),
        );
    } else {
        render_diff(out, &diff);
    }

    let done = format!(
        "删 {} 个节点、加 {} 个、改接 {} 处、改参数 {} 处；{} 条无操作",
        applied.removed.len(),
        applied.added.len(),
        applied.rewired.len(),
        applied.set.len(),
        applied.noops.len()
    );
    let tail = match (dry_run, wrote.as_str()) {
        (true, _) => "--dry-run，没有写文件".to_string(),
        (false, Some(p)) => format!("写到 {p}"),
        (false, None) => "全是 no-op，原地覆写跳过，文件一个字节没动".to_string(),
    };
    line(err, &format!("{done}；{tail}"));
    if diff["empty"] == true {
        line(err, "图没有任何语义变化（ui 不算）");
    }
    EXIT_OK
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::cli::{run_cli, sink_of};
    use std::io::Write;
    use std::sync::{Arc, Mutex};

    #[derive(Clone)]
    struct SharedBuf(Arc<Mutex<Vec<u8>>>);

    impl Write for SharedBuf {
        fn write(&mut self, buf: &[u8]) -> std::io::Result<usize> {
            self.0
                .lock()
                .unwrap_or_else(|e| e.into_inner())
                .extend_from_slice(buf);
            Ok(buf.len())
        }
        fn flush(&mut self) -> std::io::Result<()> {
            Ok(())
        }
    }

    struct Ran {
        code: i32,
        out: String,
        err: String,
    }

    impl Ran {
        fn lines(&self) -> Vec<Value> {
            self.out
                .lines()
                .filter(|l| !l.trim().is_empty())
                .filter_map(|l| serde_json::from_str(l).ok())
                .collect()
        }
        /// `--json` 那一行。
        fn result(&self) -> Value {
            self.lines()
                .into_iter()
                .find(|v| v["kind"] == "patch_result")
                .unwrap_or_else(|| panic!("stdout 里没有 patch_result：{}\n{}", self.out, self.err))
        }
    }

    fn cli(args: &[&str]) -> Ran {
        let obuf = Arc::new(Mutex::new(Vec::new()));
        let ebuf = Arc::new(Mutex::new(Vec::new()));
        let out = sink_of(SharedBuf(Arc::clone(&obuf)));
        let err = sink_of(SharedBuf(Arc::clone(&ebuf)));
        let owned: Vec<String> = args.iter().map(|s| (*s).to_string()).collect();
        let code = run_cli(&owned, &out, &err);
        let out_text = String::from_utf8_lossy(&obuf.lock().unwrap()).into_owned();
        let err_text = String::from_utf8_lossy(&ebuf.lock().unwrap()).into_owned();
        Ran {
            code,
            out: out_text,
            err: err_text,
        }
    }

    fn workspace(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("lyflow-patch-{name}"));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    /// 一张有「主路径 + 一个 b_ 开头的旁支」的图。b_alt 谁也不喂，
    /// 删掉它之后图仍然合法 —— 这样测的是 patch 自己，不是算子的输入要求。
    fn graph(dir: &Path, outputs: bool) -> String {
        let mut doc = json!({
            "schemaVersion": 1,
            "id": "01J8XQZ4K7N3M2R5V8W1YB6TCD",
            "name": "patch",
            "nodes": [
                {"id": "g", "op": "gen.synthetic",
                 "params": {"pointCount": 500, "seed": 11},
                 "ui": {"position": {"x": 0, "y": 0}}},
                {"id": "b_alt", "op": "gen.synthetic",
                 "params": {"pointCount": 500, "seed": 12},
                 "ui": {"position": {"x": 0, "y": 200}}},
                {"id": "a", "op": "filter.voxel_grid",
                 "params": {"leafSize": [0.02, 0.02, 0.02]},
                 "ui": {"position": {"x": 260, "y": 0}}},
                {"id": "sink", "op": "filter.passthrough",
                 "ui": {"position": {"x": 520, "y": 0}}}
            ],
            "edges": [
                {"id": "e1", "from": {"node": "g", "port": "cloud"},
                             "to": {"node": "a", "port": "cloud"}},
                {"id": "e2", "from": {"node": "a", "port": "cloud"},
                             "to": {"node": "sink", "port": "cloud"}}
            ]
        });
        if outputs {
            doc["outputs"] = json!({ "cloud": { "node": "sink", "port": "cloud" } });
        }
        let file = dir.join("g.lyflow.json");
        std::fs::write(&file, serde_json::to_string_pretty(&doc).unwrap()).unwrap();
        file.to_string_lossy().into_owned()
    }

    fn read(path: &str) -> Value {
        serde_json::from_str(&std::fs::read_to_string(path).unwrap()).unwrap()
    }

    fn ids(v: &Value) -> Vec<String> {
        v.as_array()
            .unwrap()
            .iter()
            .map(|x| x.as_str().unwrap().to_string())
            .collect()
    }

    #[test]
    fn remove_takes_the_node_and_all_of_its_edges() {
        let dir = workspace("remove");
        let path = graph(&dir, false);
        let r = cli(&["patch", &path, "--remove-node", "b_*", "--json"]);
        assert_eq!(r.code, EXIT_OK, "{}", r.err);
        let res = r.result();
        assert_eq!(ids(&res["applied"]["removed"]), vec!["b_alt"]);
        assert_eq!(res["wrote"].as_str().unwrap(), path);
        let doc = read(&path);
        assert_eq!(doc["nodes"].as_array().unwrap().len(), 3);
        assert!(doc["nodes"]
            .as_array()
            .unwrap()
            .iter()
            .all(|n| n["id"] != "b_alt"));
        assert_eq!(res["diff"]["nodesRemoved"][0]["id"], "b_alt");
    }

    #[test]
    fn add_gives_a_new_node_a_corner_of_its_own() {
        let dir = workspace("add");
        let path = graph(&dir, false);
        let r = cli(&[
            "patch",
            &path,
            "--add-node",
            r#"{"id":"g2","op":"gen.synthetic","params":{"pointCount":7,"seed":3}}"#,
            "--json",
        ]);
        assert_eq!(r.code, EXIT_OK, "{}", r.err);
        assert_eq!(ids(&r.result()["applied"]["added"]), vec!["g2"]);
        let doc = read(&path);
        let added = doc["nodes"]
            .as_array()
            .unwrap()
            .iter()
            .find(|n| n["id"] == "g2")
            .unwrap()
            .clone();
        assert_eq!(added["params"]["pointCount"], 7);
        // 右下角：最大的 x 是 520（sink），最大的 y 是 200（b_alt）
        assert_eq!(added["ui"]["position"]["x"], json!(780.0));
        assert_eq!(added["ui"]["position"]["y"], json!(340.0));

        let dup = cli(&[
            "patch",
            &path,
            "--add-node",
            r#"{"id":"g2","op":"gen.synthetic"}"#,
            "--json",
        ]);
        assert_eq!(dup.code, EXIT_INVALID);
        assert!(dup.err.contains("已经有这个 id"), "{}", dup.err);

        let typo = cli(&[
            "patch",
            &path,
            "--add-node",
            r#"{"id":"g3","op":"gen.synthetic","parms":{"seed":1}}"#,
        ]);
        assert_eq!(typo.code, EXIT_INVALID);
        assert!(typo.err.contains("不认识的字段 parms"), "{}", typo.err);
    }

    /// 真实用途是「短接一段」：让 a 的下游改从 a 的上游取值，a 自己留在图里。
    #[test]
    fn rewire_moves_every_edge_off_the_left_port() {
        let dir = workspace("rewire");
        let path = graph(&dir, false);
        let r = cli(&["patch", &path, "--rewire", "a:cloud=g:cloud", "--json"]);
        assert_eq!(r.code, EXIT_OK, "{}", r.err);
        assert_eq!(
            ids(&r.result()["applied"]["rewired"]),
            vec!["a:cloud=g:cloud"]
        );
        let doc = read(&path);
        let e2 = doc["edges"]
            .as_array()
            .unwrap()
            .iter()
            .find(|e| e["id"] == "e2")
            .unwrap()
            .clone();
        assert_eq!(e2["from"]["node"], "g");
        assert_eq!(e2["from"]["port"], "cloud");
        assert_eq!(r.result()["diff"]["edgesAdded"][0], "g.cloud -> sink.cloud");

        let ghost = cli(&["patch", &path, "--rewire", "ghost:cloud=a:cloud"]);
        assert_eq!(ghost.code, EXIT_INVALID);
        assert!(ghost.err.contains("图里没有节点 ghost"), "{}", ghost.err);
        let shape = cli(&["patch", &path, "--rewire", "g:cloud"]);
        assert_eq!(shape.code, EXIT_INVALID);
    }

    #[test]
    fn set_writes_one_param_and_reports_it_in_the_diff() {
        let dir = workspace("set");
        let path = graph(&dir, false);
        let r = cli(&[
            "patch",
            &path,
            "--set",
            "a.leafSize=[0.05,0.05,0.05]",
            "--json",
        ]);
        assert_eq!(r.code, EXIT_OK, "{}", r.err);
        let res = r.result();
        assert_eq!(ids(&res["applied"]["set"]), vec!["a.leafSize"]);
        assert_eq!(
            read(&path)["nodes"][2]["params"]["leafSize"],
            json!([0.05, 0.05, 0.05])
        );
        assert_eq!(
            res["diff"]["nodesChanged"][0]["params"]["leafSize"]["to"],
            json!([0.05, 0.05, 0.05])
        );
    }

    /// 顺序是定死的 remove → add → rewire → set，所以 rewire 引用一个刚被删掉的
    /// 节点就是错，不是「后面再补上」。
    #[test]
    fn rewire_after_remove_cannot_point_at_the_removed_node() {
        let dir = workspace("order");
        let path = graph(&dir, false);
        let before = std::fs::read_to_string(&path).unwrap();
        let r = cli(&[
            "patch",
            &path,
            "--remove-node",
            "b_alt",
            "--rewire",
            "g:cloud=b_alt:cloud",
        ]);
        assert_eq!(r.code, EXIT_INVALID);
        assert!(r.err.contains("图里没有节点 b_alt"), "{}", r.err);
        assert_eq!(std::fs::read_to_string(&path).unwrap(), before, "不该写盘");
    }

    /// 幂等：同一条命令跑两遍，第二遍三个动作全是 no-op，diff 为空。
    #[test]
    fn running_the_same_patch_twice_changes_nothing_the_second_time() {
        let dir = workspace("idempotent");
        let path = graph(&dir, false);
        let args = [
            "patch",
            &path,
            "--remove-node",
            "b_*",
            "--rewire",
            "a:cloud=g:cloud",
            "--set",
            "a.leafSize=[0.05,0.05,0.05]",
            "--json",
        ];
        let first = cli(&args);
        assert_eq!(first.code, EXIT_OK, "{}", first.err);
        assert_eq!(first.result()["diff"]["empty"], false);
        let after_first = std::fs::read_to_string(&path).unwrap();

        let second = cli(&args);
        assert_eq!(second.code, EXIT_OK, "{}", second.err);
        let res = second.result();
        assert_eq!(res["diff"]["empty"], true, "{}", second.out);
        for key in ["removed", "added", "rewired", "set"] {
            assert!(
                res["applied"][key].as_array().unwrap().is_empty(),
                "{key} 不该有东西：{}",
                second.out
            );
        }
        assert_eq!(res["wrote"], Value::Null, "全 no-op 的原地覆写不该落盘");
        let reasons: Vec<String> = res["noops"]
            .as_array()
            .unwrap()
            .iter()
            .map(|n| n["reason"].as_str().unwrap().to_string())
            .collect();
        assert_eq!(reasons, vec!["no_match", "no_outgoing_edge", "same_value"]);
        assert_eq!(std::fs::read_to_string(&path).unwrap(), after_first);

        // 同一件事用 diff 再问一遍：两次之后的文件与第一次之后的文件没有差别
        let copy = dir.join("after-first.lyflow.json");
        std::fs::write(&copy, &after_first).unwrap();
        let d = cli(&["diff", copy.to_str().unwrap(), &path, "--json"]);
        assert_eq!(d.code, EXIT_OK, "{}", d.err);
        assert_eq!(d.lines()[0]["empty"], true, "{}", d.out);
    }

    #[test]
    fn a_node_the_graph_outputs_still_point_at_is_not_removed() {
        let dir = workspace("outputs");
        let path = graph(&dir, true);
        let before = std::fs::read_to_string(&path).unwrap();
        let r = cli(&["patch", &path, "--remove-node", "sink", "--json"]);
        assert_eq!(r.code, EXIT_INVALID);
        assert!(r.err.contains("图级输出"), "{}", r.err);
        assert!(r.err.contains("cloud -> sink:cloud"), "{}", r.err);
        assert_eq!(std::fs::read_to_string(&path).unwrap(), before, "不该写盘");
    }

    #[test]
    fn dry_run_prints_the_same_diff_and_touches_nothing() {
        let dir = workspace("dryrun");
        let path = graph(&dir, false);
        let before = std::fs::read_to_string(&path).unwrap();
        let r = cli(&[
            "patch",
            &path,
            "--remove-node",
            "b_*",
            "--set",
            "a.leafSize=[0.05,0.05,0.05]",
            "--dry-run",
            "--json",
        ]);
        assert_eq!(r.code, EXIT_OK, "{}", r.err);
        assert_eq!(r.result()["wrote"], Value::Null);
        assert_eq!(std::fs::read_to_string(&path).unwrap(), before, "不该写盘");

        // 不带 --json 时 stdout 与 lyflow diff 是同一套人读的行
        let human = cli(&[
            "patch",
            &path,
            "--remove-node",
            "b_*",
            "--set",
            "a.leafSize=[0.05,0.05,0.05]",
            "--dry-run",
        ]);
        assert_eq!(human.code, EXIT_OK, "{}", human.err);
        assert!(human.out.contains("- 节点 \"b_alt\""), "{}", human.out);
        assert!(human.out.contains("leafSize"), "{}", human.out);

        // 真写一次，再对原图与写出来的图跑 diff，输出应当一模一样
        let out_path = dir.join("patched.lyflow.json");
        let wrote = cli(&[
            "patch",
            &path,
            "--remove-node",
            "b_*",
            "--set",
            "a.leafSize=[0.05,0.05,0.05]",
            "-o",
            out_path.to_str().unwrap(),
        ]);
        assert_eq!(wrote.code, EXIT_OK, "{}", wrote.err);
        let d = cli(&["diff", &path, out_path.to_str().unwrap()]);
        assert_eq!(d.out, human.out);
        assert_eq!(std::fs::read_to_string(&path).unwrap(), before, "-o 时不动原图");
    }

    #[test]
    fn writing_goes_through_a_temp_file_and_leaves_none_behind() {
        let dir = workspace("atomic");
        let dest = dir.join("out.lyflow.json");
        std::fs::write(&dest, "老内容").unwrap();
        write_atomic(&dest, "新内容\n").unwrap();
        assert_eq!(std::fs::read_to_string(&dest).unwrap(), "新内容\n");
        let left: Vec<String> = std::fs::read_dir(&dir)
            .unwrap()
            .map(|e| e.unwrap().file_name().to_string_lossy().into_owned())
            .collect();
        assert_eq!(left, vec!["out.lyflow.json".to_string()], "{left:?}");

        // 目标目录还不存在时也要能写（-o 指到新目录下）
        let nested = dir.join("sub").join("deep.lyflow.json");
        write_atomic(&nested, "x").unwrap();
        assert_eq!(std::fs::read_to_string(&nested).unwrap(), "x");
    }

    /// 两层校验各拦一次：形状（自环）在 Rust 这一层，参数范围在 core 那一层。
    #[test]
    fn a_patch_that_breaks_the_shape_is_not_written() {
        let dir = workspace("invalid");
        let path = graph(&dir, false);
        let before = std::fs::read_to_string(&path).unwrap();
        // e1 的源改成 a 自己 —— 自环，结构校验那一层就该拦住
        let loop_ = cli(&["patch", &path, "--rewire", "g:cloud=a:cloud", "--json"]);
        assert_eq!(loop_.code, EXIT_INVALID, "{}", loop_.out);
        assert!(loop_.err.contains("自环"), "{}", loop_.err);
        assert!(loop_.err.contains("rewire 之后"), "{}", loop_.err);
        assert_eq!(std::fs::read_to_string(&path).unwrap(), before, "不该写盘");

        let bad = cli(&["patch", &path, "--set", "a.leafSize=[0,0.01,0.01]", "--json"]);
        assert_eq!(bad.code, EXIT_INVALID, "{}", bad.out);
        assert_eq!(bad.lines()[0][0]["code"], "bad_param");
        assert!(bad.err.contains("没有写任何文件"), "{}", bad.err);
        assert_eq!(std::fs::read_to_string(&path).unwrap(), before, "不该写盘");
    }

    /// 节点 id 的通配是大小写敏感的（m6-plan §10 第 6 条）：`N_FB_*` 配不上 `n_fb_*`，
    /// 于是这条命令是 no-op 而不是「悄悄把那个节点删了」。
    #[test]
    fn node_globs_are_case_sensitive() {
        let dir = workspace("case");
        let path = graph(&dir, false);
        let before = std::fs::read_to_string(&path).unwrap();

        let upper = cli(&["patch", &path, "--remove-node", "B_*", "--json"]);
        assert_eq!(upper.code, EXIT_OK, "{}", upper.err);
        let res = upper.result();
        assert!(
            res["applied"]["removed"].as_array().unwrap().is_empty(),
            "B_* 不该配上 b_alt：{}",
            upper.out
        );
        assert_eq!(res["noops"][0]["reason"], "no_match");
        assert_eq!(std::fs::read_to_string(&path).unwrap(), before, "不该写盘");

        // 同一条命令换成小写就真删掉了 —— 证明差别只在大小写上
        let lower = cli(&["patch", &path, "--remove-node", "b_*", "--json"]);
        assert_eq!(lower.code, EXIT_OK, "{}", lower.err);
        assert_eq!(ids(&lower.result()["applied"]["removed"]), vec!["b_alt"]);
    }

    #[test]
    fn a_patch_with_no_action_is_a_usage_error() {
        let dir = workspace("usage");
        let path = graph(&dir, false);
        assert_eq!(cli(&["patch", &path]).code, EXIT_USAGE);
        assert_eq!(cli(&["patch"]).code, EXIT_USAGE);
    }

    #[test]
    fn the_corner_is_empty_even_when_no_node_has_a_position() {
        let doc: GraphDoc = serde_json::from_str(
            r#"{"schemaVersion":1,"id":"g","nodes":[{"id":"n","op":"gen.synthetic"}],"edges":[]}"#,
        )
        .unwrap();
        assert_eq!(free_corner(&doc), json!({"x": 0.0, "y": 0.0}));
    }
}
