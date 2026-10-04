//! `lyflow patch` —— 图的结构编辑（ADR-0023）。
//!
//! 七个动作、固定顺序（remove → add → rewire → connect → set → recipe → param）、幂等、每步之后过一遍形状校验，
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
use crate::graph::{Edge, GraphDoc, Node, PortRef};

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
    /// `--connect` 新加的边（原样的 spec）。
    pub connected: Vec<String>,
    pub set: Vec<String>,
    /// `--recipe` 改了 default 的顶层参数名（配方里的值写成了基础）。
    pub recipe: Vec<String>,
    /// `--param` 改了 default 的顶层参数名。
    pub params: Vec<String>,
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
            && self.connected.is_empty()
            && self.set.is_empty()
            && self.recipe.is_empty()
            && self.params.is_empty()
    }

    fn value(&self) -> Value {
        json!({
            "removed": self.removed,
            "added": self.added,
            "rewired": self.rewired,
            "connected": self.connected,
            "set": self.set,
            "recipe": self.recipe,
            "param": self.params,
        })
    }
}

type Step = fn(&mut GraphDoc, &[String], &mut Applied) -> Result<(), String>;

// ------------------------------------------------------------------ 七个动作

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
        let (from_node, from_port) = parse_port("--rewire", left, spec)?;
        let (to_node, to_port) = parse_port("--rewire", right, spec)?;
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

/// `--connect <节点>:<输出>=<节点>:<输入>`：加一条边（ADR-0023 当初留的口子）。
/// 同样的边已经在了是 no-op；目标输入口已经被别的边占了，由之后的形状校验报「单连接」、整体不写 ——
/// 要换源就用 `--rewire`，不在这里悄悄顶掉。排在 add 之后，所以同一条命令里新加的节点当场就能连。
pub(crate) fn apply_connects(
    doc: &mut GraphDoc,
    specs: &[String],
    applied: &mut Applied,
) -> Result<(), String> {
    for spec in specs {
        let (left, right) = spec.split_once('=').ok_or_else(|| {
            format!("--connect 的写法是 <节点>:<输出>=<节点>:<输入>，收到 {spec}")
        })?;
        let from = parse_port("--connect", left, spec)?;
        let to = parse_port("--connect", right, spec)?;
        for ((id, _), what) in [(&from, "源端"), (&to, "目标端")] {
            if !doc.nodes.iter().any(|n| &n.id == id) {
                return Err(format!("--connect {spec} 的{what}：图里没有节点 {id}"));
            }
        }
        let (from, to) = (
            PortRef { node: from.0, port: from.1 },
            PortRef { node: to.0, port: to.1 },
        );
        if doc.edges.iter().any(|e| {
            e.from.node == from.node && e.from.port == from.port && e.to.node == to.node && e.to.port == to.port
        }) {
            applied.noop(
                "connect",
                spec,
                "same_edge",
                &format!("--connect {spec}：这条边已经在了，跳过"),
            );
            continue;
        }
        let id = doc.unique_edge_id(&format!("e_{}_{}", from.node, to.node));
        doc.edges.push(Edge { id, from, to });
        applied.connected.push(spec.clone());
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
        if let Some(conflict) = crate::cli::set_conflict(doc, node_id, param, spec) {
            return Err(conflict);
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

/// `--recipe <文件>`：把配方里的值写成对应图参数的 default 并落盘 —— 等于在编辑器里对这个配方的
/// 每一行「写回基础」（param-recipe P4.1）。失配 ①–③ 整体不写（退出码 4），④ 在 stderr 提示；
/// 排在 `--param` 之前，所以同名的 `--param` 覆盖配方。全部同值是 no-op。
pub(crate) fn apply_recipes(
    doc: &mut GraphDoc,
    specs: &[String],
    applied: &mut Applied,
) -> Result<(), String> {
    if specs.len() > 1 {
        return Err(format!("bad_recipe: --recipe 只能给一个（收到 {} 个）", specs.len()));
    }
    for spec in specs {
        let recipe = crate::recipe::load_checked(doc, Path::new(spec))?;
        if let Some(m) = crate::recipe::report_of(doc, &recipe).spec() {
            applied.hints.push(crate::recipe::spec_hint(&recipe, m));
        }
        let changed = crate::recipe::write_as_defaults(doc, &recipe.values);
        if changed.is_empty() {
            applied.noop(
                "recipe",
                spec,
                "same_value",
                &format!("--recipe {spec}：顶层参数已经是配方里的值，跳过"),
            );
        }
        applied.recipe.extend(changed);
    }
    Ok(())
}

/// `--param <名字>=<json>`：改顶层图参数的 default 并落盘。同值是 no-op。
pub(crate) fn apply_params(
    doc: &mut GraphDoc,
    specs: &[String],
    applied: &mut Applied,
) -> Result<(), String> {
    for spec in specs {
        if crate::cli::apply_graph_param(doc, spec)? {
            applied.params.push(spec.split_once('=').map(|(n, _)| n).unwrap_or(spec).to_string());
        } else {
            applied.noop(
                "param",
                spec,
                "same_value",
                &format!("--param {spec}：顶层参数已经是这个值，跳过"),
            );
        }
    }
    Ok(())
}

fn parse_port(flag: &str, spec: &str, whole: &str) -> Result<(String, String), String> {
    let (node, port) = spec.split_once(':').ok_or_else(|| {
        format!("{flag} 的两端都要写成 <节点>:<端口>，收到 {spec}（整条是 {whole}）")
    })?;
    if node.is_empty() || port.is_empty() {
        return Err(format!(
            "{flag} 的两端都要写成 <节点>:<端口>，收到 {spec}（整条是 {whole}）"
        ));
    }
    if node.contains('/') || port.contains('/') {
        return Err(format!(
            "{flag} {spec} 指向子图内部端口：这一版只改顶层图的边"
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
             [--rewire <from>=<to>]... [--connect <from>=<to>]... [--set <node>.<param>=<json>]... [--recipe <文件>] [--param <名字>=<json>]... \
             [--dry-run] [-o <out>] [--json]",
        );
        return EXIT_USAGE;
    };
    let actions: [(&str, Step, &[String]); 7] = [
        ("remove-node", apply_removes, parsed.many("remove-node")),
        ("add-node", apply_adds, parsed.many("add-node")),
        ("rewire", apply_rewires, parsed.many("rewire")),
        ("connect", apply_connects, parsed.many("connect")),
        ("set", apply_sets, parsed.many("set")),
        ("recipe", apply_recipes, parsed.many("recipe")),
        ("param", apply_params, parsed.many("param")),
    ];
    if actions.iter().all(|(_, _, specs)| specs.is_empty()) {
        line(
            err,
            "至少给一个动作：--remove-node / --add-node / --rewire / --connect / --set / --recipe / --param",
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
    let before = match load_graph(&bare, &path, err) {
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
            return fail(err, &e, crate::cli::load_exit(&e));
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
        "删 {} 个节点、加 {} 个、改接 {} 处、加边 {} 条、改参数 {} 处、配方写回基础 {} 个、改顶层参数 {} 个；{} 条无操作",
        applied.removed.len(),
        applied.added.len(),
        applied.rewired.len(),
        applied.connected.len(),
        applied.set.len(),
        applied.recipe.len(),
        applied.params.len(),
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
mod tests;
