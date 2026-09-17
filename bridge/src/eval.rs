use std::collections::{BTreeMap, BTreeSet};
use std::path::{Path, PathBuf};
use std::sync::Arc;

use serde_json::{json, Map, Value};

use crate::cli::{
    core, defaults_by_op, diagnostics_of, execute, has_errors, json_line, line, load_graph,
    parse_axis, component_count, Loaded, Parsed, RunRequest, Sink, EXIT_CANCELLED, EXIT_FAILED,
    EXIT_INVALID, EXIT_OK, EXIT_USAGE,
};
use crate::core_ffi::Core;
use crate::graph::GraphDoc;

const MAX_LEAF_DEPTH: usize = 6;
const MAX_LISTED_PATHS: usize = 200;

#[derive(Clone, Debug, PartialEq)]
pub(crate) enum MetricKind {
    RunDuration,
    NodeDuration(String),
    NodeStat(String, String),
    NodePort {
        node: String,
        port: String,
        rest: Vec<String>,
    },
    Output {
        name: String,
        rest: Vec<String>,
    },
}

#[derive(Clone, Debug)]
pub(crate) struct MetricPath {
    pub raw: String,
    pub kind: MetricKind,
}

impl MetricPath {
    fn needs_outputs(&self) -> bool {
        matches!(self.kind, MetricKind::Output { .. })
    }
}

fn bad_metric(spec: &str) -> String {
    format!(
        "看不懂的指标路径 {spec}。写法是 outputs.<名字>[.字段...] / nodes.<节点>.<端口>[.字段...] / \
nodes.<节点>.durationMs|elementCount|byteSize / run.durationMs"
    )
}

pub(crate) fn parse_metric(spec: &str) -> Result<MetricPath, String> {
    let kind = if spec.contains(':') {
        legacy_metric(spec)?
    } else {
        path_metric(spec)?
    };
    Ok(MetricPath {
        raw: spec.to_string(),
        kind,
    })
}

fn legacy_metric(spec: &str) -> Result<MetricKind, String> {
    let (target, field) = spec.rsplit_once('.').ok_or_else(|| bad_metric(spec))?;
    let (node, port) = target.split_once(':').ok_or_else(|| bad_metric(spec))?;
    if node.is_empty() || port.is_empty() {
        return Err(bad_metric(spec));
    }
    match field {
        "durationMs" => Ok(MetricKind::NodeDuration(node.to_string())),
        "byteSize" => Ok(MetricKind::NodeStat(node.to_string(), "byteSize".to_string())),
        "elementCount" => Ok(MetricKind::NodePort {
            node: node.to_string(),
            port: port.to_string(),
            rest: vec!["elementCount".to_string()],
        }),
        other => Err(format!(
            "--metric 的旧写法 nodeId:port.field 只认 elementCount / byteSize / durationMs，收到 {other}"
        )),
    }
}

fn path_metric(spec: &str) -> Result<MetricKind, String> {
    let segs: Vec<&str> = spec.split('.').collect();
    if segs.len() < 2 || segs.iter().any(|s| s.is_empty()) {
        return Err(bad_metric(spec));
    }
    let owned = |s: &[&str]| s.iter().map(|x| (*x).to_string()).collect::<Vec<String>>();
    match segs[0] {
        "run" => {
            if segs.len() == 2 && segs[1] == "durationMs" {
                Ok(MetricKind::RunDuration)
            } else {
                Err(format!("run 下面只有 durationMs，收到 {spec}"))
            }
        }
        "nodes" => {
            if segs.len() < 3 {
                return Err(bad_metric(spec));
            }
            let node = segs[1].to_string();
            let rest = &segs[2..];
            if rest.len() == 1 {
                match rest[0] {
                    "durationMs" => return Ok(MetricKind::NodeDuration(node)),
                    "elementCount" | "byteSize" => {
                        return Ok(MetricKind::NodeStat(node, rest[0].to_string()))
                    }
                    _ => {}
                }
            }
            Ok(MetricKind::NodePort {
                node,
                port: rest[0].to_string(),
                rest: owned(&rest[1..]),
            })
        }
        "outputs" => Ok(MetricKind::Output {
            name: segs[1].to_string(),
            rest: owned(&segs[2..]),
        }),
        _ => Err(bad_metric(spec)),
    }
}

pub(crate) struct RunView<'a> {
    pub events: &'a [Value],
    pub outputs: Option<&'a Value>,
}

fn descend<'a>(start: &'a Value, rest: &[String]) -> Option<&'a Value> {
    let mut cur = start;
    for key in rest {
        let obj = cur.as_object()?;
        cur = match obj.get(key) {
            Some(v) => v,
            None => obj.get("data")?.as_object()?.get(key)?,
        };
    }
    Some(cur)
}

fn scalar_of(v: &Value) -> Option<f64> {
    match v {
        Value::Number(n) => n.as_f64(),
        Value::Bool(b) => Some(if *b { 1.0 } else { 0.0 }),
        Value::Object(o) => match o.get("value") {
            Some(Value::Number(n)) => n.as_f64(),
            Some(Value::Bool(b)) => Some(if *b { 1.0 } else { 0.0 }),
            _ => None,
        },
        _ => None,
    }
}

impl<'a> RunView<'a> {
    fn node_states(&self, node: &str) -> impl Iterator<Item = &'a Value> {
        let node = node.to_string();
        self.events
            .iter()
            .rev()
            .filter(move |e| e["kind"] == "node_state" && e["nodeId"] == node.as_str())
    }

    pub(crate) fn resolve(&self, metric: &MetricPath) -> Option<f64> {
        match &metric.kind {
            MetricKind::RunDuration => self
                .events
                .iter()
                .rev()
                .find(|e| e["kind"] == "run_finished")
                .and_then(|e| e["durationMs"].as_f64()),
            MetricKind::NodeDuration(node) => {
                self.node_states(node).find_map(|e| e["durationMs"].as_f64())
            }
            MetricKind::NodeStat(node, field) => self
                .node_states(node)
                .find_map(|e| e["stats"][field.as_str()].as_f64()),
            MetricKind::NodePort { node, port, rest } => self.node_states(node).find_map(|e| {
                let entry = e["stats"]["outputs"]
                    .as_array()?
                    .iter()
                    .find(|o| o["port"] == port.as_str())?;
                if rest.len() == 1 && (rest[0] == "elementCount" || rest[0] == "byteSize") {
                    if let Some(v) = entry[rest[0].as_str()].as_f64() {
                        return Some(v);
                    }
                }
                scalar_of(descend(entry.get("value")?, rest)?)
            }),
            MetricKind::Output { name, rest } => {
                let entry = self.outputs?.get(name.as_str())?;
                scalar_of(descend(entry.get("value")?, rest)?)
            }
        }
    }
}

fn collect_leaves(prefix: &str, v: &Value, depth: usize, out: &mut BTreeSet<String>) {
    if depth > MAX_LEAF_DEPTH {
        return;
    }
    let Some(obj) = v.as_object() else { return };
    if let Some(Value::Object(data)) = obj.get("data") {
        for (k, x) in data {
            leaf_or_recurse(prefix, k, x, depth, out);
        }
    }
    for (k, x) in obj {
        if k == "kind" || k == "data" {
            continue;
        }
        leaf_or_recurse(prefix, k, x, depth, out);
    }
}

fn leaf_or_recurse(prefix: &str, key: &str, v: &Value, depth: usize, out: &mut BTreeSet<String>) {
    let path = format!("{prefix}.{key}");
    match v {
        Value::Number(_) | Value::Bool(_) => {
            out.insert(path);
        }
        Value::Object(_) => collect_leaves(&path, v, depth + 1, out),
        _ => {}
    }
}

pub(crate) fn available_paths(view: &RunView) -> Vec<String> {
    let mut out = BTreeSet::new();
    if view
        .events
        .iter()
        .any(|e| e["kind"] == "run_finished" && e["durationMs"].is_number())
    {
        out.insert("run.durationMs".to_string());
    }
    let mut seen: Vec<String> = Vec::new();
    for e in view.events {
        if e["kind"] != "node_state" {
            continue;
        }
        let Some(id) = e["nodeId"].as_str() else { continue };
        if seen.iter().any(|s| s == id) {
            continue;
        }
        seen.push(id.to_string());
    }
    for id in &seen {
        for e in view.node_states(id) {
            if e["durationMs"].is_number() {
                out.insert(format!("nodes.{id}.durationMs"));
            }
            for field in ["elementCount", "byteSize"] {
                if e["stats"][field].is_number() {
                    out.insert(format!("nodes.{id}.{field}"));
                }
            }
            let Some(outputs) = e["stats"]["outputs"].as_array() else {
                continue;
            };
            for o in outputs {
                let Some(port) = o["port"].as_str() else { continue };
                let prefix = format!("nodes.{id}.{port}");
                if o["elementCount"].is_number() {
                    out.insert(format!("{prefix}.elementCount"));
                }
                if let Some(value) = o.get("value") {
                    if scalar_of(value).is_some() {
                        out.insert(prefix.clone());
                    }
                    collect_leaves(&prefix, value, 0, &mut out);
                }
            }
        }
    }
    if let Some(Value::Object(named)) = view.outputs {
        for (name, entry) in named {
            let prefix = format!("outputs.{name}");
            let Some(value) = entry.get("value") else {
                continue;
            };
            if scalar_of(value).is_some() {
                out.insert(prefix.clone());
            }
            collect_leaves(&prefix, value, 0, &mut out);
        }
    }
    out.into_iter().collect()
}

#[derive(Clone, Debug)]
pub(crate) struct Sample {
    pub id: String,
    pub set: Vec<(String, Value)>,
    pub tags: BTreeMap<String, String>,
}

impl Sample {
    pub(crate) fn whole_graph() -> Self {
        Sample {
            id: "-".to_string(),
            set: Vec::new(),
            tags: BTreeMap::new(),
        }
    }

    fn tags_json(&self) -> Value {
        let mut m = Map::new();
        for (k, v) in &self.tags {
            m.insert(k.clone(), Value::String(v.clone()));
        }
        Value::Object(m)
    }
}

fn tag_to_string(v: &Value) -> Option<String> {
    match v {
        Value::String(s) => Some(s.clone()),
        Value::Number(n) => Some(n.to_string()),
        Value::Bool(b) => Some(b.to_string()),
        _ => None,
    }
}

pub(crate) fn parse_samples(text: &str, origin: &str) -> Result<Vec<Sample>, String> {
    let mut out = Vec::new();
    for (i, raw) in text.lines().enumerate() {
        let trimmed = raw.trim();
        if trimmed.is_empty() {
            continue;
        }
        let at = format!("{origin} 第 {} 行", i + 1);
        let value: Value = serde_json::from_str(trimmed)
            .map_err(|e| format!("{at} 不是合法 JSON: {e}"))?;
        let Some(obj) = value.as_object() else {
            return Err(format!("{at} 不是 JSON 对象"));
        };
        if obj.contains_key("scene") {
            return Err(format!(
                "{at} 带 scene 字段：本版本不支持 scene 注入，样本只能用 set 覆盖参数（m5-plan §8.1）"
            ));
        }
        let Some(id) = obj.get("id").and_then(Value::as_str) else {
            return Err(format!("{at} 缺 id（字符串）"));
        };
        let mut set = Vec::new();
        match obj.get("set") {
            None | Some(Value::Null) => {}
            Some(Value::Object(m)) => {
                for (k, v) in m {
                    if !k.contains('.') {
                        return Err(format!("{at} 的 set 键 {k} 不是 <节点>.<参数>"));
                    }
                    set.push((k.clone(), v.clone()));
                }
            }
            Some(_) => return Err(format!("{at} 的 set 不是对象")),
        }
        let mut tags = BTreeMap::new();
        match obj.get("tags") {
            None | Some(Value::Null) => {}
            Some(Value::Object(m)) => {
                for (k, v) in m {
                    let Some(s) = tag_to_string(v) else {
                        return Err(format!("{at} 的 tag {k} 不是字符串/数字/布尔"));
                    };
                    tags.insert(k.clone(), s);
                }
            }
            Some(_) => return Err(format!("{at} 的 tags 不是对象")),
        }
        out.push(Sample {
            id: id.to_string(),
            set,
            tags,
        });
    }
    if out.is_empty() {
        return Err(format!("{origin} 里一个样本都没有"));
    }
    Ok(out)
}

pub(crate) fn load_samples(path: &str) -> Result<Vec<Sample>, String> {
    let text = std::fs::read_to_string(path).map_err(|e| format!("读取 {path} 失败: {e}"))?;
    parse_samples(&text, path)
}

/// 文件名通配。**大小写不敏感**：`*Master*.pcd` 要能配上 `..._master_0.pcd`，
/// 采集端在不同版本里两种写法都出现过，而那不是用户能控制的。
pub(crate) fn wildcard_match(pattern: &str, name: &str) -> bool {
    glob_match(pattern, name, /*case_sensitive=*/ false)
}

/// 节点 id 通配。**大小写敏感**（m6-plan §10 第 6 条）：节点 id 是图里写死的标识符，
/// `N_FB_*` 与 `n_fb_*` 是两个不同的选择，让它们互相匹配只会让
/// `--remove-node` 悄悄多删一批。文件名那份刻意保持不敏感，两者分开。
pub(crate) fn wildcard_match_cs(pattern: &str, name: &str) -> bool {
    glob_match(pattern, name, /*case_sensitive=*/ true)
}

fn glob_match(pattern: &str, name: &str, case_sensitive: bool) -> bool {
    let fold = |s: &str| -> Vec<char> {
        if case_sensitive {
            s.chars().collect()
        } else {
            s.chars().flat_map(char::to_lowercase).collect()
        }
    };
    let p: Vec<char> = fold(pattern);
    let n: Vec<char> = fold(name);
    let (mut pi, mut ni) = (0usize, 0usize);
    let (mut star, mut mark) = (usize::MAX, 0usize);
    while ni < n.len() {
        if pi < p.len() && (p[pi] == '?' || p[pi] == n[ni]) {
            pi += 1;
            ni += 1;
        } else if pi < p.len() && p[pi] == '*' {
            star = pi;
            mark = ni;
            pi += 1;
        } else if star != usize::MAX {
            pi = star + 1;
            mark += 1;
            ni = mark;
        } else {
            return false;
        }
    }
    while pi < p.len() && p[pi] == '*' {
        pi += 1;
    }
    pi == p.len()
}

fn absolute(path: &Path) -> PathBuf {
    if path.is_absolute() {
        return path.to_path_buf();
    }
    match std::env::current_dir() {
        Ok(cwd) => cwd.join(path),
        Err(_) => path.to_path_buf(),
    }
}

fn walk_glob(base: &Path, segments: &[String], out: &mut Vec<PathBuf>) {
    if segments.is_empty() {
        if base.is_file() {
            out.push(base.to_path_buf());
        }
        return;
    }
    let seg = &segments[0];
    let rest = &segments[1..];
    if seg == "**" {
        walk_glob(base, rest, out);
        let Ok(entries) = std::fs::read_dir(base) else {
            return;
        };
        let mut dirs: Vec<PathBuf> = entries
            .flatten()
            .map(|e| e.path())
            .filter(|p| p.is_dir())
            .collect();
        dirs.sort();
        for d in dirs {
            walk_glob(&d, segments, out);
        }
        return;
    }
    if !seg.contains('*') && !seg.contains('?') {
        walk_glob(&base.join(seg), rest, out);
        return;
    }
    let Ok(entries) = std::fs::read_dir(base) else {
        return;
    };
    let mut hits: Vec<PathBuf> = entries
        .flatten()
        .map(|e| e.path())
        .filter(|p| {
            p.file_name()
                .and_then(|n| n.to_str())
                .map(|n| wildcard_match(seg, n))
                .unwrap_or(false)
        })
        .collect();
    hits.sort();
    for h in hits {
        walk_glob(&h, rest, out);
    }
}

pub(crate) fn glob_files(pattern: &str) -> Result<Vec<PathBuf>, String> {
    let normalized = pattern.replace('\\', "/");
    if normalized.is_empty() {
        return Err("--samples-glob 是空的".to_string());
    }
    let parts: Vec<String> = normalized.split('/').map(str::to_string).collect();
    let mut base = PathBuf::new();
    let mut i = 0usize;
    let absolute_pattern = normalized.starts_with('/')
        || parts
            .first()
            .map(|s| s.len() == 2 && s.ends_with(':'))
            .unwrap_or(false);
    while i < parts.len() {
        let seg = &parts[i];
        if seg.contains('*') || seg.contains('?') {
            break;
        }
        if seg.is_empty() {
            base.push("/");
        } else if i == 0 && seg.len() == 2 && seg.ends_with(':') {
            base.push(format!("{seg}/"));
        } else {
            base.push(seg);
        }
        i += 1;
    }
    if i == parts.len() {
        let file = absolute(&base);
        if !file.is_file() {
            return Err(format!("--samples-glob 没匹配到文件: {pattern}"));
        }
        return Ok(vec![file]);
    }
    if !absolute_pattern {
        base = absolute(&base);
    }
    if base.as_os_str().is_empty() {
        base = absolute(Path::new("."));
    }
    let mut out = Vec::new();
    walk_glob(&base, &parts[i..], &mut out);
    out.sort();
    if out.is_empty() {
        return Err(format!("--samples-glob 没匹配到文件: {pattern}"));
    }
    Ok(out)
}

pub(crate) fn samples_from_files(files: &[PathBuf], bind: &str) -> Vec<Sample> {
    let stem = |p: &Path| {
        p.file_stem()
            .and_then(|s| s.to_str())
            .map(str::to_string)
            .unwrap_or_else(|| p.to_string_lossy().into_owned())
    };
    let mut counts: BTreeMap<String, usize> = BTreeMap::new();
    for f in files {
        *counts.entry(stem(f)).or_default() += 1;
    }
    let mut used: BTreeSet<String> = BTreeSet::new();
    let mut out = Vec::new();
    for f in files {
        let short = stem(f);
        let mut id = if counts.get(&short).copied().unwrap_or(0) > 1 {
            let parent = f
                .parent()
                .and_then(|p| p.file_name())
                .and_then(|n| n.to_str())
                .unwrap_or("");
            if parent.is_empty() {
                short.clone()
            } else {
                format!("{parent}/{short}")
            }
        } else {
            short.clone()
        };
        let mut n = 2;
        while used.contains(&id) {
            id = format!("{short}#{n}");
            n += 1;
        }
        used.insert(id.clone());
        out.push(Sample {
            id,
            set: vec![(
                bind.to_string(),
                Value::String(absolute(f).to_string_lossy().replace('\\', "/")),
            )],
            tags: BTreeMap::new(),
        });
    }
    out
}

#[derive(Clone, Debug, Default)]
pub(crate) struct ParamSet {
    pub display: Map<String, Value>,
    pub writes: Vec<(String, String, Value)>,
}

fn split_target(key: &str, what: &str) -> Result<(String, String), String> {
    key.rsplit_once('.')
        .map(|(n, p)| (n.to_string(), p.to_string()))
        .ok_or_else(|| format!("{what} 的键 {key} 不是 <节点>.<参数>"))
}

pub(crate) fn parse_param_file(text: &str, origin: &str) -> Result<Vec<ParamSet>, String> {
    let value: Value =
        serde_json::from_str(text).map_err(|e| format!("{origin} 不是合法 JSON: {e}"))?;
    let Some(items) = value.as_array() else {
        return Err(format!("{origin} 要是一个 JSON 数组"));
    };
    let mut out = Vec::new();
    for (i, item) in items.iter().enumerate() {
        let Some(obj) = item.as_object() else {
            return Err(format!("{origin} 第 {i} 项不是对象"));
        };
        let mut ps = ParamSet::default();
        for (k, v) in obj {
            let (node, param) = split_target(k, origin)?;
            ps.display.insert(k.clone(), v.clone());
            ps.writes.push((node, param, v.clone()));
        }
        out.push(ps);
    }
    if out.is_empty() {
        return Err(format!("{origin} 是空数组"));
    }
    Ok(out)
}

pub(crate) fn axis_param_sets(
    doc: &GraphDoc,
    defaults: &BTreeMap<String, BTreeMap<String, Value>>,
    specs: &[String],
) -> Result<Vec<ParamSet>, String> {
    if specs.is_empty() {
        return Ok(Vec::new());
    }
    let mut axes = Vec::new();
    for spec in specs {
        let mut axis = parse_axis(spec)?;
        if !doc.nodes.iter().any(|n| n.id == axis.node) {
            return Err(format!("图里没有节点 {}", axis.node));
        }
        axis.components = component_count(doc, defaults, &axis.node, &axis.param);
        axes.push(axis);
    }
    let total: usize = axes.iter().map(|a| a.values.len()).product();
    let mut out = Vec::with_capacity(total);
    for index in 0..total {
        let mut ps = ParamSet::default();
        let mut rest = index;
        for axis in &axes {
            let pick = rest % axis.values.len();
            rest /= axis.values.len();
            let value = axis.values[pick];
            let written = if axis.components > 0 {
                json!(vec![value; axis.components])
            } else {
                json!(value)
            };
            ps.display
                .insert(format!("{}.{}", axis.node, axis.param), json!(value));
            ps.writes
                .push((axis.node.clone(), axis.param.clone(), written));
        }
        out.push(ps);
    }
    Ok(out)
}

pub(crate) fn combine_param_sets(explicit: Vec<ParamSet>, axes: Vec<ParamSet>) -> Vec<ParamSet> {
    if explicit.is_empty() && axes.is_empty() {
        return vec![ParamSet::default()];
    }
    if axes.is_empty() {
        return explicit;
    }
    if explicit.is_empty() {
        return axes;
    }
    let mut out = Vec::with_capacity(explicit.len() * axes.len());
    for e in &explicit {
        for a in &axes {
            let mut merged = e.clone();
            for (k, v) in &a.display {
                merged.display.insert(k.clone(), v.clone());
            }
            for (node, param, v) in &a.writes {
                merged
                    .writes
                    .retain(|(n, p, _)| !(n == node && p == param));
                merged.writes.push((node.clone(), param.clone(), v.clone()));
            }
            out.push(merged);
        }
    }
    out
}

#[derive(Clone, Debug, Default)]
pub(crate) struct GroupStats {
    pub n: usize,
    pub ok: usize,
    pub fail_codes: BTreeMap<String, usize>,
    pub values: Vec<f64>,
}

impl GroupStats {
    fn mean(&self) -> Option<f64> {
        if self.values.is_empty() {
            return None;
        }
        Some(self.values.iter().sum::<f64>() / self.values.len() as f64)
    }
    fn std(&self) -> Option<f64> {
        if self.values.len() < 2 {
            return None;
        }
        let m = self.mean()?;
        let sum: f64 = self.values.iter().map(|v| (v - m) * (v - m)).sum();
        Some((sum / (self.values.len() - 1) as f64).sqrt())
    }
    fn min(&self) -> Option<f64> {
        self.values.iter().cloned().fold(None, |a: Option<f64>, v| {
            Some(a.map_or(v, |x| x.min(v)))
        })
    }
    fn max(&self) -> Option<f64> {
        self.values.iter().cloned().fold(None, |a: Option<f64>, v| {
            Some(a.map_or(v, |x| x.max(v)))
        })
    }
    fn to_json(&self) -> Value {
        let num = |v: Option<f64>| v.map(|x| json!(x)).unwrap_or(Value::Null);
        let p2p = match (self.min(), self.max()) {
            (Some(a), Some(b)) => json!(b - a),
            _ => Value::Null,
        };
        let mut codes = Map::new();
        for (k, v) in &self.fail_codes {
            codes.insert(k.clone(), json!(v));
        }
        json!({
            "n": self.n,
            "ok": self.ok,
            "failCodes": Value::Object(codes),
            "mean": num(self.mean()),
            "std": num(self.std()),
            "min": num(self.min()),
            "max": num(self.max()),
            "p2p": p2p,
        })
    }
}

#[derive(Clone, Debug)]
pub(crate) struct Grouping {
    pub holdout: Option<(String, String)>,
    pub group_by: Option<String>,
}

impl Grouping {
    pub(crate) fn is_holdout(&self, s: &Sample) -> bool {
        match &self.holdout {
            Some((k, v)) => s.tags.get(k).map(|x| x == v).unwrap_or(false),
            None => false,
        }
    }
    pub(crate) fn name_of(&self, s: &Sample) -> String {
        let side = self
            .holdout
            .as_ref()
            .map(|_| if self.is_holdout(s) { "holdout" } else { "train" });
        let group = self
            .group_by
            .as_ref()
            .map(|k| s.tags.get(k).cloned().unwrap_or_else(|| "(none)".to_string()));
        match (side, group) {
            (Some(a), Some(b)) => format!("{a}/{b}"),
            (Some(a), None) => a.to_string(),
            (None, Some(b)) => b,
            (None, None) => "all".to_string(),
        }
    }
}

pub(crate) fn parse_holdout(spec: &str) -> Result<(String, String), String> {
    spec.split_once('=')
        .map(|(k, v)| (k.to_string(), v.to_string()))
        .filter(|(k, _)| !k.is_empty())
        .ok_or_else(|| format!("--holdout 的写法是 <标签>=<值>，收到 {spec}"))
}

#[derive(Clone, Debug)]
pub(crate) struct Row {
    pub param_set: usize,
    pub sample: usize,
    pub status: String,
    pub metrics: Vec<Option<f64>>,
    pub errors: Vec<String>,
    pub duration_ms: f64,
    pub skipped: Vec<String>,
    /// 这一次运行的 run summary（ADR-0022）。没给 `--summary`、或校验就没过时是 None。
    pub summary: Option<Value>,
}

pub(crate) enum EngineError {
    Usage(String, Vec<String>),
    Failed(String),
}

pub(crate) struct Engine<'a> {
    pub core: &'a Arc<Core>,
    pub base: &'a Loaded,
    pub metrics: &'a [MetricPath],
    pub param_sets: &'a [ParamSet],
    pub samples: &'a [Sample],
    pub parallel: i32,
    pub no_cache: bool,
    /// 每行带一份 run summary（ADR-0022）。**默认关**，`--summary` 打开 ——
    /// 体积是逐行的，一维 bundle 就 6 KB（m6-plan §10 第 5 条）。
    pub summary: bool,
}

struct Attempt {
    row: Row,
    available: Vec<String>,
    resolved: Vec<bool>,
}

impl<'a> Engine<'a> {
    fn variant(&self, ps: &ParamSet, sample: &Sample) -> Result<GraphDoc, String> {
        let mut doc = self.base.doc.clone();
        let mut write = |node_id: &str, param: &str, value: &Value| -> Result<(), String> {
            let node = doc
                .nodes
                .iter_mut()
                .find(|n| n.id == node_id)
                .ok_or_else(|| format!("图里没有节点 {node_id}"))?;
            node.params.insert(param.to_string(), value.clone());
            Ok(())
        };
        for (node, param, value) in &ps.writes {
            write(node, param, value)?;
        }
        for (key, value) in &sample.set {
            let (node, param) = split_target(key, "样本的 set")?;
            write(&node, &param, value)?;
        }
        Ok(doc)
    }

    fn attempt(
        &self,
        pi: usize,
        si: usize,
        enumerate: bool,
    ) -> Result<Attempt, String> {
        let ps = &self.param_sets[pi];
        let sample = &self.samples[si];
        let doc = self.variant(ps, sample)?;
        let graph_json = serde_json::to_string(&doc).map_err(|e| e.to_string())?;
        let loaded = Loaded {
            doc,
            json: graph_json,
            base_dir: self.base.base_dir.clone(),
            path: self.base.path.clone(),
        };
        let diags = diagnostics_of(self.core, &loaded)?;
        if has_errors(&diags) {
            let mut errors: Vec<String> = Vec::new();
            for d in &diags {
                if d["severity"] != "error" {
                    continue;
                }
                if let Some(code) = d["code"].as_str() {
                    if !errors.iter().any(|c| c == code) {
                        errors.push(code.to_string());
                    }
                }
            }
            return Ok(Attempt {
                row: Row {
                    param_set: pi,
                    sample: si,
                    status: "validation_failed".to_string(),
                    metrics: vec![None; self.metrics.len()],
                    errors,
                    duration_ms: 0.0,
                    skipped: Vec::new(),
                    summary: None,
                },
                available: Vec::new(),
                resolved: vec![false; self.metrics.len()],
            });
        }
        let result = execute(
            self.core,
            RunRequest {
                graph_json: &loaded.json,
                base_dir: &loaded.base_dir,
                targets: &[],
                parallel: self.parallel,
                preview_points: 0,
                preview: false,
                no_cache: self.no_cache,
                stream: None,
            },
        )?;
        let wants_outputs = enumerate || self.metrics.iter().any(MetricPath::needs_outputs);
        let outputs: Option<Value> = if wants_outputs {
            self.core
                .run_outputs(result.run_id())
                .ok()
                .and_then(|raw| serde_json::from_str::<Value>(&raw).ok())
        } else {
            None
        };
        let view = RunView {
            events: &result.events,
            outputs: outputs.as_ref(),
        };
        let values: Vec<Option<f64>> = self.metrics.iter().map(|m| view.resolve(m)).collect();
        let available = if enumerate {
            available_paths(&view)
        } else {
            Vec::new()
        };
        let mut errors: Vec<String> = Vec::new();
        for e in &result.events {
            if e["kind"] != "node_state" {
                continue;
            }
            let mut push = |code: &str| {
                if !errors.iter().any(|c| c == code) {
                    errors.push(code.to_string());
                }
            };
            if let Some(code) = e["error"]["code"].as_str() {
                push(code);
            }
            if let Some(list) = e["errors"].as_array() {
                for item in list {
                    if let Some(code) = item["code"].as_str() {
                        push(code);
                    }
                }
            }
        }
        let status = match result.status.as_str() {
            "ok" => "ok",
            "cancelled" => "cancelled",
            _ => "failed",
        };
        let resolved = values.iter().map(Option::is_some).collect();
        Ok(Attempt {
            row: Row {
                param_set: pi,
                sample: si,
                status: status.to_string(),
                metrics: values,
                errors,
                duration_ms: result.duration_ms(),
                skipped: result.skipped_nodes(),
                summary: if self.summary {
                    result.summary().cloned()
                } else {
                    None
                },
            },
            available,
            resolved,
        })
    }

    pub(crate) fn run(&self, on_row: &mut dyn FnMut(&Row)) -> Result<i32, EngineError> {
        let mut worst = EXIT_OK;
        let mut first: Option<Attempt> = None;
        if !self.param_sets.is_empty() && !self.samples.is_empty() && !self.metrics.is_empty() {
            let a = self.attempt(0, 0, true).map_err(EngineError::Failed)?;
            if a.row.status == "ok" {
                let missing: Vec<String> = self
                    .metrics
                    .iter()
                    .zip(&a.resolved)
                    .filter(|(_, ok)| !**ok)
                    .map(|(m, _)| m.raw.clone())
                    .collect();
                if !missing.is_empty() {
                    return Err(EngineError::Usage(
                        format!("这些指标路径在图上取不到值：{}", missing.join(", ")),
                        a.available,
                    ));
                }
            }
            first = Some(a);
        }
        for pi in 0..self.param_sets.len() {
            for si in 0..self.samples.len() {
                let attempt = match first.take() {
                    Some(a) => a,
                    None => self.attempt(pi, si, false).map_err(EngineError::Failed)?,
                };
                let code = match attempt.row.status.as_str() {
                    "ok" => EXIT_OK,
                    "validation_failed" => EXIT_INVALID,
                    "cancelled" => EXIT_CANCELLED,
                    _ => EXIT_FAILED,
                };
                if code == EXIT_CANCELLED {
                    on_row(&attempt.row);
                    return Ok(EXIT_CANCELLED);
                }
                worst = worst.max(code);
                on_row(&attempt.row);
            }
        }
        Ok(worst)
    }
}

fn csv_cell(v: &Value) -> String {
    match v {
        Value::Null => String::new(),
        Value::String(s) => s.clone(),
        other => other.to_string(),
    }
}

fn csv_escape(s: &str) -> String {
    if s.contains(',') || s.contains('"') || s.contains('\n') {
        format!("\"{}\"", s.replace('"', "\"\""))
    } else {
        s.to_string()
    }
}

pub(crate) fn csv_text(
    metrics: &[MetricPath],
    param_sets: &[ParamSet],
    samples: &[Sample],
    grouping: &Grouping,
    rows: &[Row],
) -> String {
    let mut keys: Vec<String> = Vec::new();
    for ps in param_sets {
        for k in ps.display.keys() {
            if !keys.iter().any(|x| x == k) {
                keys.push(k.clone());
            }
        }
    }
    keys.sort();
    let mut header: Vec<String> = vec!["paramSet".to_string()];
    header.extend(keys.iter().cloned());
    header.push("sample".to_string());
    header.push("holdout".to_string());
    header.push("status".to_string());
    header.extend(metrics.iter().map(|m| m.raw.clone()));
    let mut text = header
        .iter()
        .map(|h| csv_escape(h))
        .collect::<Vec<_>>()
        .join(",");
    text.push('\n');
    for row in rows {
        let ps = &param_sets[row.param_set];
        let sample = &samples[row.sample];
        let mut cells: Vec<String> = vec![row.param_set.to_string()];
        for k in &keys {
            cells.push(csv_cell(ps.display.get(k).unwrap_or(&Value::Null)));
        }
        cells.push(sample.id.clone());
        cells.push(grouping.is_holdout(sample).to_string());
        cells.push(row.status.clone());
        for v in &row.metrics {
            cells.push(v.map(|x| x.to_string()).unwrap_or_default());
        }
        text.push_str(
            &cells
                .iter()
                .map(|c| csv_escape(c))
                .collect::<Vec<_>>()
                .join(","),
        );
        text.push('\n');
    }
    text
}

pub(crate) fn summarize(
    samples: &[Sample],
    grouping: &Grouping,
    metric_index: usize,
    rows: &[Row],
) -> BTreeMap<String, GroupStats> {
    let mut groups: BTreeMap<String, GroupStats> = BTreeMap::new();
    for row in rows {
        let sample = &samples[row.sample];
        let entry = groups.entry(grouping.name_of(sample)).or_default();
        entry.n += 1;
        let value = row.metrics.get(metric_index).copied().flatten();
        match (row.status.as_str(), value) {
            ("ok", Some(v)) => {
                entry.ok += 1;
                entry.values.push(v);
            }
            ("ok", None) => {
                *entry
                    .fail_codes
                    .entry("metric_missing".to_string())
                    .or_default() += 1;
            }
            (status, _) => {
                if row.errors.is_empty() {
                    *entry.fail_codes.entry(status.to_string()).or_default() += 1;
                } else {
                    for code in &row.errors {
                        *entry.fail_codes.entry(code.clone()).or_default() += 1;
                    }
                }
            }
        }
    }
    groups
}

pub(crate) fn cmd_eval(parsed: &Parsed, out: &Sink, err: &Sink) -> i32 {
    let Some(path) = parsed.positional.first().cloned() else {
        line(err, "用法：lyflow eval <graph> --samples <samples.jsonl> --metric <path>");
        return EXIT_USAGE;
    };

    let metric_specs = parsed.many("metric");
    if metric_specs.is_empty() {
        line(err, "至少给一个 --metric <path>，例如 --metric outputs.gap");
        return EXIT_USAGE;
    }
    let metrics: Vec<MetricPath> = match metric_specs
        .iter()
        .map(|s| parse_metric(s))
        .collect::<Result<Vec<_>, _>>()
    {
        Ok(m) => m,
        Err(e) => {
            line(err, &e);
            return EXIT_USAGE;
        }
    };

    let holdout = match parsed.one("holdout").map(parse_holdout) {
        Some(Ok(h)) => Some(h),
        Some(Err(e)) => {
            line(err, &e);
            return EXIT_USAGE;
        }
        None => None,
    };
    let grouping = Grouping {
        holdout,
        group_by: parsed.one("group-by").map(str::to_string),
    };

    let samples = match collect_samples(parsed, err) {
        Ok(s) => s,
        Err(e) => {
            line(err, &e);
            return EXIT_USAGE;
        }
    };

    let core = match core() {
        Ok(c) => c,
        Err(e) => {
            line(err, &e);
            return EXIT_FAILED;
        }
    };
    let loaded = match load_graph(parsed, &path) {
        Ok(l) => l,
        Err(e) => {
            line(err, &e);
            return EXIT_INVALID;
        }
    };
    let defaults = match defaults_by_op(&core) {
        Ok(d) => d,
        Err(e) => {
            line(err, &e);
            return EXIT_FAILED;
        }
    };

    let explicit = match parsed.one("params") {
        Some(file) => match std::fs::read_to_string(file)
            .map_err(|e| format!("读取 {file} 失败: {e}"))
            .and_then(|t| parse_param_file(&t, file))
        {
            Ok(v) => v,
            Err(e) => {
                line(err, &e);
                return EXIT_USAGE;
            }
        },
        None => Vec::new(),
    };
    let axes = match axis_param_sets(&loaded.doc, &defaults, parsed.many("param")) {
        Ok(v) => v,
        Err(e) => {
            line(err, &e);
            return EXIT_USAGE;
        }
    };
    let param_sets = combine_param_sets(explicit, axes);

    let parallel = parsed
        .one("parallel")
        .and_then(|v| v.parse::<i32>().ok())
        .unwrap_or(0);

    let engine = Engine {
        core: &core,
        base: &loaded,
        metrics: &metrics,
        param_sets: &param_sets,
        samples: &samples,
        parallel,
        no_cache: parsed.has("no-cache"),
        // 默认关（m6-plan §10 第 5 条）：gap 图一维 bundle 就 6 KB，51 帧 × 8 组
        // 参数 2.5 MB —— 一个「每行都带上」的默认值会把 eval 的输出撑成不可读。
        // `--no-summary` 留着当 no-op：老脚本照样跑得过。
        summary: parsed.has("summary"),
    };

    let mut rows: Vec<Row> = Vec::new();
    let code = {
        let mut on_row = |row: &Row| {
            let sample = &samples[row.sample];
            let mut m = Map::new();
            for (metric, value) in metrics.iter().zip(&row.metrics) {
                m.insert(
                    metric.raw.clone(),
                    value.map(|v| json!(v)).unwrap_or(Value::Null),
                );
            }
            let mut line_json = json!({
                "kind": "eval_row",
                "paramSet": row.param_set,
                "params": Value::Object(param_sets[row.param_set].display.clone()),
                "sample": sample.id,
                "tags": sample.tags_json(),
                "holdout": grouping.is_holdout(sample),
                "status": row.status,
                "metrics": Value::Object(m),
                "errors": row.errors,
                "durationMs": row.duration_ms,
            });
            // summary 是这一行的结论（ADR-0022）：status 三态与每个图输出的三态。
            // 默认不带，`--summary` 才有；没跑到执行期时也没有。
            if let (Some(s), Some(obj)) = (row.summary.as_ref(), line_json.as_object_mut()) {
                obj.insert("summary".to_string(), s.clone());
            }
            json_line(out, &line_json);
            rows.push(row.clone());
        };
        match engine.run(&mut on_row) {
            Ok(c) => c,
            Err(EngineError::Failed(e)) => {
                line(err, &e);
                return EXIT_FAILED;
            }
            Err(EngineError::Usage(message, available)) => {
                line(err, &message);
                if available.is_empty() {
                    line(err, "这张图跑完一次后没有任何标量路径可取");
                } else {
                    line(err, "这张图上可用的标量路径：");
                    for p in available.iter().take(MAX_LISTED_PATHS) {
                        line(err, &format!("  {p}"));
                    }
                    if available.len() > MAX_LISTED_PATHS {
                        line(
                            err,
                            &format!("  …还有 {} 条", available.len() - MAX_LISTED_PATHS),
                        );
                    }
                }
                return EXIT_USAGE;
            }
        }
    };

    for (pi, ps) in param_sets.iter().enumerate() {
        let of_set: Vec<Row> = rows.iter().filter(|r| r.param_set == pi).cloned().collect();
        if of_set.is_empty() {
            continue;
        }
        for (mi, metric) in metrics.iter().enumerate() {
            let groups = summarize(&samples, &grouping, mi, &of_set);
            let mut g = Map::new();
            for (name, stats) in &groups {
                g.insert(name.clone(), stats.to_json());
            }
            json_line(
                out,
                &json!({
                    "kind": "eval_summary",
                    "paramSet": pi,
                    "params": Value::Object(ps.display.clone()),
                    "metric": metric.raw,
                    "groups": Value::Object(g),
                }),
            );
        }
    }

    if let Some(csv_path) = parsed.one("csv") {
        let text = csv_text(&metrics, &param_sets, &samples, &grouping, &rows);
        if let Err(e) = std::fs::write(csv_path, text) {
            line(err, &format!("写入 {csv_path} 失败: {e}"));
            return EXIT_FAILED;
        }
        line(err, &format!("表格写到 {csv_path}"));
    }

    let ok = rows.iter().filter(|r| r.status == "ok").count();
    line(
        err,
        &format!(
            "{} 组参数 × {} 个样本 = {} 次运行，{} 次 ok",
            param_sets.len(),
            samples.len(),
            rows.len(),
            ok
        ),
    );
    code
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) enum SortBy {
    Name,
    Mtime,
}

#[derive(Clone, Debug)]
pub(crate) struct DirSpec {
    pub root: PathBuf,
    pub subdir: Option<String>,
    pub binds: Vec<String>,
    pub patterns: Vec<String>,
    pub sort_by: SortBy,
    pub split_half: Option<String>,
}

pub(crate) fn parse_dir_timestamp(name: &str) -> Option<[u32; 6]> {
    const WIDTHS: [usize; 6] = [2, 2, 4, 2, 2, 2];
    const TOTAL: usize = 2 + 1 + 2 + 1 + 4 + 1 + 2 + 1 + 2 + 1 + 2;
    let b = name.as_bytes();
    if b.len() < TOTAL {
        return None;
    }
    for start in 0..=(b.len() - TOTAL) {
        if start > 0 && b[start - 1].is_ascii_digit() {
            continue;
        }
        let end = start + TOTAL;
        if end < b.len() && b[end].is_ascii_digit() {
            continue;
        }
        let mut pos = start;
        let mut vals = [0u32; 6];
        let mut ok = true;
        for (i, w) in WIDTHS.iter().enumerate() {
            if i > 0 {
                if b[pos] != b'-' {
                    ok = false;
                    break;
                }
                pos += 1;
            }
            let mut v = 0u32;
            for k in 0..*w {
                let c = b[pos + k];
                if !c.is_ascii_digit() {
                    ok = false;
                    break;
                }
                v = v * 10 + u32::from(c - b'0');
            }
            if !ok {
                break;
            }
            vals[i] = v;
            pos += w;
        }
        if ok {
            return Some([vals[2], vals[1], vals[0], vals[3], vals[4], vals[5]]);
        }
    }
    None
}

fn dir_mtime(path: &Path) -> u128 {
    std::fs::metadata(path)
        .and_then(|m| m.modified())
        .ok()
        .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
        .map(|d| d.as_nanos())
        .unwrap_or(0)
}

fn pick_one(dir: &Path, pattern: &str, frame: &str) -> Result<PathBuf, String> {
    let entries = std::fs::read_dir(dir)
        .map_err(|e| format!("帧 {frame}：读不了目录 {} ({e})", dir.display()))?;
    let mut hits: Vec<PathBuf> = entries
        .flatten()
        .map(|e| e.path())
        .filter(|p| p.is_file())
        .filter(|p| {
            p.file_name()
                .and_then(|n| n.to_str())
                .map(|n| wildcard_match(pattern, n))
                .unwrap_or(false)
        })
        .collect();
    hits.sort();
    match hits.len() {
        1 => Ok(hits.remove(0)),
        0 => Err(format!(
            "帧 {frame}：--pattern {pattern} 在 {} 下没匹配到文件",
            dir.display()
        )),
        n => {
            let names: Vec<String> = hits
                .iter()
                .filter_map(|p| p.file_name().and_then(|s| s.to_str()))
                .map(str::to_string)
                .collect();
            Err(format!(
                "帧 {frame}：--pattern {pattern} 在 {} 下匹配到 {n} 个文件（{}），要正好一个",
                dir.display(),
                names.join(", ")
            ))
        }
    }
}

pub(crate) fn samples_from_dir(spec: &DirSpec) -> Result<(Vec<Sample>, Option<String>), String> {
    let root = absolute(&spec.root);
    if !root.is_dir() {
        return Err(format!("--samples-dir 不是一个目录: {}", root.display()));
    }
    let entries = std::fs::read_dir(&root)
        .map_err(|e| format!("读不了 --samples-dir {} ({e})", root.display()))?;
    let mut frames: Vec<(String, PathBuf)> = entries
        .flatten()
        .map(|e| e.path())
        .filter(|p| p.is_dir())
        .filter_map(|p| {
            p.file_name()
                .and_then(|n| n.to_str())
                .map(|n| (n.to_string(), p.clone()))
        })
        .collect();
    if frames.is_empty() {
        return Err(format!(
            "--samples-dir {} 下一个子目录都没有（每个直接子目录是一帧）",
            root.display()
        ));
    }

    let mut note = None;
    match spec.sort_by {
        SortBy::Mtime => {
            let mut keyed: Vec<(u128, String, PathBuf)> = frames
                .into_iter()
                .map(|(n, p)| (dir_mtime(&p), n, p))
                .collect();
            keyed.sort();
            frames = keyed.into_iter().map(|(_, n, p)| (n, p)).collect();
        }
        SortBy::Name => {
            let stamps: Vec<Option<[u32; 6]>> =
                frames.iter().map(|(n, _)| parse_dir_timestamp(n)).collect();
            if stamps.iter().all(Option::is_some) {
                let mut keyed: Vec<([u32; 6], String, PathBuf)> = frames
                    .into_iter()
                    .zip(&stamps)
                    .map(|((n, p), ts)| (ts.unwrap_or_default(), n, p))
                    .collect();
                keyed.sort();
                frames = keyed.into_iter().map(|(_, n, p)| (n, p)).collect();
            } else {
                let bad = frames
                    .iter()
                    .zip(&stamps)
                    .find(|(_, ts)| ts.is_none())
                    .map(|((n, _), _)| n.clone())
                    .unwrap_or_default();
                note = Some(format!(
                    "--sort-by name：帧目录名里读不出 dd-MM-yyyy-HH-mm-ss 时间戳（比如 {bad}），退回字典序"
                ));
                frames.sort();
            }
        }
    }

    let mut out = Vec::new();
    for (name, dir) in &frames {
        let look = match &spec.subdir {
            Some(s) => dir.join(s),
            None => dir.clone(),
        };
        if !look.is_dir() {
            return Err(format!(
                "帧 {name}：找不到目录 {}",
                look.display()
            ));
        }
        let mut set = Vec::new();
        for (bind, pattern) in spec.binds.iter().zip(&spec.patterns) {
            let file = pick_one(&look, pattern, name)?;
            set.push((
                bind.clone(),
                Value::String(absolute(&file).to_string_lossy().replace('\\', "/")),
            ));
        }
        out.push(Sample {
            id: name.clone(),
            set,
            tags: BTreeMap::new(),
        });
    }

    if let Some(key) = &spec.split_half {
        let first = out.len().div_ceil(2);
        for (i, s) in out.iter_mut().enumerate() {
            s.tags
                .insert(key.clone(), if i < first { "a" } else { "b" }.to_string());
        }
    }
    Ok((out, note))
}

fn sample_to_json(s: &Sample) -> Value {
    let mut set = Map::new();
    for (k, v) in &s.set {
        set.insert(k.clone(), v.clone());
    }
    let mut o = Map::new();
    o.insert("id".to_string(), Value::String(s.id.clone()));
    o.insert("set".to_string(), Value::Object(set));
    if !s.tags.is_empty() {
        o.insert("tags".to_string(), s.tags_json());
    }
    Value::Object(o)
}

pub(crate) fn samples_jsonl(samples: &[Sample]) -> String {
    let mut text = String::new();
    for s in samples {
        text.push_str(&sample_to_json(s).to_string());
        text.push('\n');
    }
    text
}

fn dir_spec(parsed: &Parsed, root: &str) -> Result<DirSpec, String> {
    let explicit = parsed.many("bind");
    let binds: Vec<String> = match parsed.one("bind-pair") {
        Some(spec) => {
            if !explicit.is_empty() {
                return Err("--bind-pair 与 --bind 只能给一个".to_string());
            }
            let parts: Vec<String> = spec
                .split(',')
                .map(str::trim)
                .filter(|s| !s.is_empty())
                .map(str::to_string)
                .collect();
            if parts.len() != 2 {
                return Err(format!(
                    "--bind-pair 要正好两个，写成 <节点>.<参数A>,<节点>.<参数B>，收到 {spec}"
                ));
            }
            parts
        }
        None => {
            if explicit.len() != 1 {
                return Err(
                    "--samples-dir 要配 --bind-pair <a>,<b>（双相机）或一个 --bind <节点>.<参数>（单文件）"
                        .to_string(),
                );
            }
            vec![explicit[0].clone()]
        }
    };
    for b in &binds {
        split_target(b, "--bind-pair")?;
    }
    let Some(raw) = parsed.one("pattern") else {
        return Err(
            "--samples-dir 要配 --pattern <glob>；两个相机时用逗号隔开两个 glob"
                .to_string(),
        );
    };
    let patterns: Vec<String> = raw
        .split(',')
        .map(str::trim)
        .filter(|s| !s.is_empty())
        .map(str::to_string)
        .collect();
    if patterns.len() != binds.len() {
        return Err(format!(
            "--pattern 给了 {} 个，--bind-pair/--bind 给了 {} 个，要一一对应",
            patterns.len(),
            binds.len()
        ));
    }
    let sort_by = match parsed.one("sort-by") {
        None | Some("name") => SortBy::Name,
        Some("mtime") => SortBy::Mtime,
        Some(other) => {
            return Err(format!(
                "--sort-by 只认 name 或 mtime，收到 {other}"
            ))
        }
    };
    Ok(DirSpec {
        root: PathBuf::from(root),
        subdir: parsed.one("sample-subdir").map(str::to_string),
        binds,
        patterns,
        sort_by,
        split_half: parsed.one("split-half").map(str::to_string),
    })
}

pub(crate) fn collect_samples(parsed: &Parsed, err: &Sink) -> Result<Vec<Sample>, String> {
    let file = parsed.one("samples");
    let glob = parsed.one("samples-glob");
    let dir = parsed.one("samples-dir");
    let given = [file.is_some(), glob.is_some(), dir.is_some()]
        .iter()
        .filter(|b| **b)
        .count();
    if given > 1 {
        return Err(
            "--samples / --samples-glob / --samples-dir 只能给一个".to_string(),
        );
    }
    if dir.is_none() {
        for name in ["bind-pair", "pattern", "sample-subdir", "sort-by", "split-half"] {
            if parsed.one(name).is_some() {
                return Err(format!("--{name} 要和 --samples-dir 一起给"));
            }
        }
    }

    let samples = if let Some(file) = file {
        if !parsed.many("bind").is_empty() {
            return Err(
                "--bind 只跟 --samples-glob / --samples-dir 配套；用 --samples 时样本自己写 set"
                    .to_string(),
            );
        }
        load_samples(file)?
    } else if let Some(glob) = glob {
        let binds = parsed.many("bind");
        if binds.len() != 1 {
            return Err(
                "--samples-glob 要正好配一个 --bind <节点>.<参数>；两个相机请用 --samples-dir --bind-pair"
                    .to_string(),
            );
        }
        split_target(&binds[0], "--bind")?;
        let files = glob_files(glob)?;
        samples_from_files(&files, &binds[0])
    } else if let Some(root) = dir {
        let spec = dir_spec(parsed, root)?;
        let (samples, note) = samples_from_dir(&spec)?;
        if let Some(note) = note {
            line(err, &note);
        }
        samples
    } else {
        if !parsed.many("bind").is_empty() {
            return Err(
                "--bind 要和 --samples-glob 或 --samples-dir 一起给".to_string(),
            );
        }
        vec![Sample::whole_graph()]
    };

    if let Some(out) = parsed.one("samples-jsonl-out") {
        std::fs::write(out, samples_jsonl(&samples))
            .map_err(|e| format!("写入 {out} 失败: {e}"))?;
        line(
            err,
            &format!(
                "样本集写到 {out}（{} 个样本）",
                samples.len()
            ),
        );
    }
    Ok(samples)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn metric(spec: &str) -> MetricPath {
        parse_metric(spec).unwrap_or_else(|e| panic!("{spec}: {e}"))
    }

    fn events() -> Vec<Value> {
        vec![
            json!({"kind": "run_started", "seq": 0}),
            json!({
                "kind": "node_state", "nodeId": "n_fit", "state": "done", "durationMs": 4.5,
                "stats": {
                    "elementCount": 12, "byteSize": 480,
                    "outputs": [
                        {"port": "line", "type": "Line2D", "elementCount": 1,
                         "value": {"kind": "Line2D", "hasSegment": true}},
                        {"port": "quality", "type": "Record", "elementCount": 1,
                         "value": {"kind": "Record", "type": "fit",
                                   "data": {"rmsResidualMm": 0.03, "inlierCount": 9}}}
                    ]
                }
            }),
            json!({
                "kind": "node_state", "nodeId": "n_off", "state": "done", "durationMs": 0.2,
                "stats": {
                    "elementCount": 1, "byteSize": 8,
                    "outputs": [
                        {"port": "dx", "type": "Measurement", "elementCount": 1,
                         "value": {"kind": "Measurement", "value": 1.25, "ok": true}}
                    ]
                }
            }),
            json!({"kind": "run_finished", "status": "ok", "durationMs": 9.75}),
        ]
    }

    fn named() -> Value {
        json!({
            "gap": {"node": "n_off", "port": "dx", "type": "Measurement", "elementCount": 1,
                    "value": {"kind": "Measurement", "value": 1.25, "ok": true}},
            "bundle": {"node": "n_b", "port": "bundle", "type": "Record", "elementCount": 1,
                       "value": {"kind": "Record", "type": "gap",
                                 "data": {"point_counts": {"left": 640, "right": 512}}}},
            "cloud": {"node": "n_c", "port": "cloud", "type": "PointCloud", "elementCount": 99}
        })
    }

    #[test]
    fn metric_paths_cover_outputs_nodes_and_run() {
        let ev = events();
        let out = named();
        let view = RunView { events: &ev, outputs: Some(&out) };
        assert_eq!(view.resolve(&metric("run.durationMs")), Some(9.75));
        assert_eq!(view.resolve(&metric("nodes.n_fit.durationMs")), Some(4.5));
        assert_eq!(view.resolve(&metric("nodes.n_fit.elementCount")), Some(12.0));
        assert_eq!(view.resolve(&metric("nodes.n_fit.byteSize")), Some(480.0));
        assert_eq!(view.resolve(&metric("nodes.n_fit.quality.rmsResidualMm")), Some(0.03));
        assert_eq!(view.resolve(&metric("nodes.n_fit.line.elementCount")), Some(1.0));
        assert_eq!(view.resolve(&metric("nodes.n_fit.line.hasSegment")), Some(1.0));
        assert_eq!(view.resolve(&metric("nodes.n_off.dx")), Some(1.25));
        assert_eq!(view.resolve(&metric("outputs.gap")), Some(1.25));
        assert_eq!(view.resolve(&metric("outputs.gap.ok")), Some(1.0));
        assert_eq!(view.resolve(&metric("outputs.bundle.point_counts.left")), Some(640.0));
        assert_eq!(view.resolve(&metric("outputs.cloud")), None);
        assert_eq!(view.resolve(&metric("outputs.nope")), None);
        assert_eq!(view.resolve(&metric("nodes.n_fit.nope.x")), None);
    }

    #[test]
    fn a_malformed_metric_path_is_rejected_up_front() {
        for bad in ["gap", "stuff.gap", "run.elementCount", "nodes.n_fit", "outputs."] {
            assert!(parse_metric(bad).is_err(), "{bad} 应当被拒");
        }
    }

    #[test]
    fn available_paths_lists_every_scalar_on_the_graph() {
        let ev = events();
        let out = named();
        let paths = available_paths(&RunView { events: &ev, outputs: Some(&out) });
        for want in [
            "run.durationMs",
            "nodes.n_fit.durationMs",
            "nodes.n_fit.elementCount",
            "nodes.n_fit.byteSize",
            "nodes.n_fit.line.elementCount",
            "nodes.n_fit.line.hasSegment",
            "nodes.n_fit.quality.rmsResidualMm",
            "nodes.n_fit.quality.inlierCount",
            "nodes.n_off.dx",
            "nodes.n_off.dx.value",
            "outputs.gap",
            "outputs.gap.ok",
            "outputs.bundle.point_counts.left",
            "outputs.bundle.point_counts.right",
        ] {
            assert!(paths.iter().any(|p| p == want), "少了 {want}: {paths:?}");
        }
        assert!(!paths.iter().any(|p| p == "outputs.cloud"));
        assert!(!paths.iter().any(|p| p.contains(".data.")));
    }

    #[test]
    fn the_old_sweep_metric_spelling_translates_into_a_path() {
        assert_eq!(
            parse_metric("v:cloud.elementCount").unwrap().kind,
            MetricKind::NodePort {
                node: "v".to_string(),
                port: "cloud".to_string(),
                rest: vec!["elementCount".to_string()],
            }
        );
        assert_eq!(
            parse_metric("v:cloud.durationMs").unwrap().kind,
            MetricKind::NodeDuration("v".to_string())
        );
        assert_eq!(
            parse_metric("v:cloud.byteSize").unwrap().kind,
            MetricKind::NodeStat("v".to_string(), "byteSize".to_string())
        );
        assert!(parse_metric("v:cloud.nope").is_err());
        assert_eq!(parse_metric("v:cloud.elementCount").unwrap().raw, "v:cloud.elementCount");
    }

    #[test]
    fn samples_reject_scene_and_carry_tags() {
        let text = concat!(
            r#"{"id":"f1","set":{"n.path":"a.pcd"},"tags":{"half":"a"}}"#,
            "\n",
            r#"{"id":"f2","set":{"n.path":"b.pcd"}}"#,
            "\n"
        );
        let s = parse_samples(text, "t.jsonl").unwrap();
        assert_eq!(s.len(), 2);
        assert_eq!(s[0].id, "f1");
        assert_eq!(s[0].set, vec![("n.path".to_string(), json!("a.pcd"))]);
        assert_eq!(s[0].tags.get("half").map(String::as_str), Some("a"));
        assert!(s[1].tags.is_empty());

        let e = parse_samples(concat!(r#"{"id":"f1","scene":"s1"}"#, "\n"), "t.jsonl").unwrap_err();
        assert!(e.contains("scene"), "{e}");
        assert!(parse_samples(concat!(r#"{"set":{}}"#, "\n"), "t.jsonl").is_err());
        assert!(parse_samples(concat!(r#"{"id":"f","set":{"nodot":1}}"#, "\n"), "t.jsonl").is_err());
    }

    #[test]
    fn glob_turns_matching_files_into_samples() {
        let dir = std::env::temp_dir().join("lyflow-eval-glob");
        let _ = std::fs::remove_dir_all(&dir);
        for frame in ["f1", "f2"] {
            let sub = dir.join(frame).join("p4");
            std::fs::create_dir_all(&sub).unwrap();
            std::fs::write(sub.join(format!("{frame}_master.pcd")), "x").unwrap();
            std::fs::write(sub.join(format!("{frame}_slave.pcd")), "x").unwrap();
        }
        let root = dir.to_string_lossy().replace('\\', "/");
        let files = glob_files(&format!("{root}/*/p4/*master.pcd")).unwrap();
        assert_eq!(files.len(), 2, "{files:?}");
        let samples = samples_from_files(&files, "n_load.primaryFile");
        assert_eq!(samples[0].id, "f1_master");
        assert_eq!(samples[1].id, "f2_master");
        assert_eq!(samples[0].set.len(), 1);
        assert_eq!(samples[0].set[0].0, "n_load.primaryFile");
        assert!(samples[0].set[0].1.as_str().unwrap().ends_with("f1/p4/f1_master.pcd"));

        assert!(glob_files(&format!("{root}/*/p4/*.tif")).is_err());
        assert!(wildcard_match("*master*.pcd", "f1_master_0.pcd"));
        assert!(!wildcard_match("*master*.pcd", "f1_slave_0.pcd"));
        assert!(wildcard_match("a?c", "abc"));
        // 文件名那份刻意不敏感：采集端写过 Master 也写过 master
        assert!(wildcard_match("*Master*.pcd", "f1_master_0.pcd"));
    }

    /// 两份通配是两件事（m6-plan §10 第 6 条）：文件名不敏感、节点 id 敏感。
    #[test]
    fn node_id_globs_are_case_sensitive_but_file_globs_are_not() {
        assert!(wildcard_match_cs("n_fb_*", "n_fb_line"));
        assert!(!wildcard_match_cs("N_FB_*", "n_fb_line"));
        assert!(!wildcard_match_cs("n_fb_*", "N_FB_LINE"));
        assert!(wildcard_match_cs("b_*", "b_alt"));
        assert!(!wildcard_match_cs("B_*", "b_alt"));
        // 同一对输入在文件名那份上是匹配的 —— 差别只在这一条规则上
        assert!(wildcard_match("N_FB_*", "n_fb_line"));
    }

    #[test]
    fn glob_gives_colliding_stems_distinct_ids() {
        let dir = std::env::temp_dir().join("lyflow-eval-glob-dup");
        let _ = std::fs::remove_dir_all(&dir);
        for frame in ["f1", "f2"] {
            let sub = dir.join(frame);
            std::fs::create_dir_all(&sub).unwrap();
            std::fs::write(sub.join("scan.pcd"), "x").unwrap();
        }
        let root = dir.to_string_lossy().replace('\\', "/");
        let files = glob_files(&format!("{root}/*/scan.pcd")).unwrap();
        let samples = samples_from_files(&files, "n.path");
        assert_eq!(samples[0].id, "f1/scan");
        assert_eq!(samples[1].id, "f2/scan");
    }

    fn row(sample: usize, value: Option<f64>, status: &str, errors: &[&str]) -> Row {
        Row {
            param_set: 0,
            sample,
            status: status.to_string(),
            metrics: vec![value],
            errors: errors.iter().map(|s| (*s).to_string()).collect(),
            duration_ms: 1.0,
            skipped: Vec::new(),
            summary: None,
        }
    }

    fn sample(id: &str, tags: &[(&str, &str)]) -> Sample {
        Sample {
            id: id.to_string(),
            set: Vec::new(),
            tags: tags
                .iter()
                .map(|(k, v)| ((*k).to_string(), (*v).to_string()))
                .collect(),
        }
    }

    #[test]
    fn holdout_and_group_statistics_are_the_hand_computed_numbers() {
        let samples: Vec<Sample> = vec![
            sample("s1", &[("half", "a")]),
            sample("s2", &[("half", "a")]),
            sample("s3", &[("half", "a")]),
            sample("s4", &[("half", "a")]),
            sample("s5", &[("half", "a")]),
            sample("s6", &[("half", "b")]),
            sample("s7", &[("half", "b")]),
        ];
        let rows = vec![
            row(0, Some(2.0), "ok", &[]),
            row(1, Some(4.0), "ok", &[]),
            row(2, Some(4.0), "ok", &[]),
            row(3, Some(4.0), "ok", &[]),
            row(4, Some(5.0), "ok", &[]),
            row(5, Some(7.0), "ok", &[]),
            row(6, None, "failed", &["io"]),
        ];
        let grouping = Grouping {
            holdout: Some(("half".to_string(), "b".to_string())),
            group_by: None,
        };
        let g = summarize(&samples, &grouping, 0, &rows);
        let train = &g["train"];
        assert_eq!((train.n, train.ok), (5, 5));
        assert_eq!(train.mean(), Some(3.8));
        assert!((train.std().unwrap() - 1.2f64.sqrt()).abs() < 1e-12, "{:?}", train.std());
        assert_eq!(train.min(), Some(2.0));
        assert_eq!(train.max(), Some(5.0));
        assert_eq!(train.to_json()["p2p"], json!(3.0));
        assert!(train.fail_codes.is_empty());

        let hold = &g["holdout"];
        assert_eq!((hold.n, hold.ok), (2, 1));
        assert_eq!(hold.mean(), Some(7.0));
        assert_eq!(hold.std(), None);
        assert_eq!(hold.fail_codes.get("io"), Some(&1));

        let both = Grouping {
            holdout: Some(("half".to_string(), "b".to_string())),
            group_by: Some("half".to_string()),
        };
        let g2 = summarize(&samples, &both, 0, &rows);
        assert_eq!(g2["train/a"].n, 5);
        assert_eq!(g2["holdout/b"].n, 2);

        let plain = Grouping { holdout: None, group_by: None };
        assert_eq!(summarize(&samples, &plain, 0, &rows)["all"].n, 7);
    }

    #[test]
    fn a_missing_metric_on_a_successful_run_counts_as_a_failure() {
        let samples = vec![sample("s1", &[]), sample("s2", &[])];
        let rows = vec![row(0, Some(1.0), "ok", &[]), row(1, None, "ok", &[])];
        let grouping = Grouping { holdout: None, group_by: None };
        let g = summarize(&samples, &grouping, 0, &rows);
        assert_eq!((g["all"].n, g["all"].ok), (2, 1));
        assert_eq!(g["all"].fail_codes.get("metric_missing"), Some(&1));
    }

    #[test]
    fn explicit_param_sets_and_axes_multiply() {
        let explicit = parse_param_file(r#"[{"a.x": 1}, {"a.x": 2}]"#, "p.json").unwrap();
        assert_eq!(explicit.len(), 2);
        let axis = |v: f64| ParamSet {
            display: [("b.y".to_string(), json!(v))].into_iter().collect(),
            writes: vec![("b".to_string(), "y".to_string(), json!(v))],
        };
        let merged = combine_param_sets(explicit, vec![axis(10.0), axis(20.0)]);
        assert_eq!(merged.len(), 4);
        assert_eq!(merged[0].display["a.x"], json!(1));
        assert_eq!(merged[0].display["b.y"], json!(10.0));
        assert_eq!(merged[3].display["a.x"], json!(2));
        assert_eq!(merged[3].display["b.y"], json!(20.0));
        assert_eq!(merged[3].writes.len(), 2);
        assert_eq!(combine_param_sets(Vec::new(), Vec::new()).len(), 1);
    }

    #[test]
    fn holdout_needs_a_key_value_pair() {
        assert_eq!(parse_holdout("half=b").unwrap(), ("half".to_string(), "b".to_string()));
        assert!(parse_holdout("half").is_err());
        assert!(parse_holdout("=b").is_err());
    }

    struct TempTree(PathBuf);

    impl Drop for TempTree {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }

    fn tree(tag: &str, frames: &[(&str, &[&str])], subdir: Option<&str>) -> TempTree {
        static NEXT: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);
        let n = NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        let root =
            std::env::temp_dir().join(format!("lyflow-eval-{tag}-{}-{n}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        for (frame, files) in frames {
            let dir = match subdir {
                Some(sub) => root.join(frame).join(sub),
                None => root.join(frame),
            };
            std::fs::create_dir_all(&dir).expect("建目录");
            for f in *files {
                std::fs::write(dir.join(f), b"").expect("建文件");
            }
        }
        TempTree(root)
    }

    fn pair_spec(root: &Path, subdir: Option<&str>, split: Option<&str>, sort: SortBy) -> DirSpec {
        DirSpec {
            root: root.to_path_buf(),
            subdir: subdir.map(str::to_string),
            binds: vec![
                "n_load.primaryFile".to_string(),
                "n_load.secondaryFile".to_string(),
            ],
            patterns: vec!["*Master*.pcd".to_string(), "*Slave*.pcd".to_string()],
            sort_by: sort,
            split_half: split.map(str::to_string),
        }
    }

    const MASTER: &str = "LaserProfile_L0_Master_4_x_0.pcd";
    const SLAVE: &str = "LaserProfile_R1_Slave_4_x_0.pcd";

    #[test]
    fn a_samples_dir_pairs_two_globs_per_frame() {
        let files: &[&str] = &[MASTER, SLAVE, "notes.txt"];
        let t = tree(
            "pair",
            &[
                ("vin_15-09-2026-08-00-00", files),
                ("vin_15-09-2026-08-01-00", files),
                ("vin_14-09-2026-03-44-38", files),
            ],
            Some("4"),
        );
        let (samples, note) =
            samples_from_dir(&pair_spec(&t.0, Some("4"), None, SortBy::Name)).expect("配对");
        assert!(note.is_none());
        assert_eq!(samples.len(), 3);
        assert_eq!(samples[0].id, "vin_14-09-2026-03-44-38");
        assert_eq!(samples[2].id, "vin_15-09-2026-08-01-00");
        assert_eq!(samples[0].set.len(), 2);
        assert_eq!(samples[0].set[0].0, "n_load.primaryFile");
        assert!(samples[0]
            .set[0]
            .1
            .as_str()
            .unwrap()
            .ends_with(&format!("vin_14-09-2026-03-44-38/4/{MASTER}")));
        assert_eq!(samples[0].set[1].0, "n_load.secondaryFile");
        assert!(samples[0].set[1].1.as_str().unwrap().ends_with(SLAVE));
        assert!(samples[0].tags.is_empty());
    }

    #[test]
    fn without_a_subdir_the_files_sit_in_the_frame_dir() {
        let files: &[&str] = &[MASTER, SLAVE];
        let t = tree("flat", &[("15-09-2026-08-00-00", files)], None);
        let (samples, _) =
            samples_from_dir(&pair_spec(&t.0, None, None, SortBy::Name)).expect("配对");
        assert_eq!(samples.len(), 1);
        assert!(samples[0].set[0].1.as_str().unwrap().ends_with(MASTER));
    }

    #[test]
    fn the_timestamp_in_the_dir_name_beats_lexicographic_order() {
        assert_eq!(
            parse_dir_timestamp("12345678998765432_14-09-2026-03-44-38"),
            Some([2026, 9, 14, 3, 44, 38])
        );
        assert_eq!(parse_dir_timestamp("frame-001"), None);
        assert_eq!(parse_dir_timestamp("1-09-2026-03-44-38"), None);

        let files: &[&str] = &[MASTER, SLAVE];
        let t = tree(
            "order",
            &[
                ("a_09-10-2026-08-00-00", files),
                ("b_10-09-2026-08-00-00", files),
            ],
            None,
        );
        let (by_time, note) =
            samples_from_dir(&pair_spec(&t.0, None, None, SortBy::Name)).expect("配对");
        assert!(note.is_none());
        assert_eq!(by_time[0].id, "b_10-09-2026-08-00-00");
        assert_eq!(by_time[1].id, "a_09-10-2026-08-00-00");
    }

    #[test]
    fn an_unparsable_dir_name_falls_back_to_lexicographic_and_says_so() {
        let files: &[&str] = &[MASTER, SLAVE];
        let t = tree("fallback", &[("frame-002", files), ("frame-001", files)], None);
        let (samples, note) =
            samples_from_dir(&pair_spec(&t.0, None, None, SortBy::Name)).expect("配对");
        assert_eq!(samples[0].id, "frame-001");
        assert_eq!(samples[1].id, "frame-002");
        let note = note.expect("要在 stderr 说一句");
        assert!(note.contains("dd-MM-yyyy-HH-mm-ss"), "{note}");
    }

    #[test]
    fn split_half_tags_the_front_half_a_and_gives_it_the_odd_one() {
        let files: &[&str] = &[MASTER, SLAVE];
        let frames: Vec<String> = (0..5).map(|i| format!("f_15-09-2026-08-0{i}-00")).collect();
        let spec: Vec<(&str, &[&str])> = frames.iter().map(|f| (f.as_str(), files)).collect();
        let t = tree("half", &spec, None);
        let (samples, _) =
            samples_from_dir(&pair_spec(&t.0, None, Some("half"), SortBy::Name)).expect("配对");
        let tags: Vec<&str> = samples
            .iter()
            .map(|s| s.tags.get("half").map(String::as_str).unwrap_or(""))
            .collect();
        assert_eq!(tags, vec!["a", "a", "a", "b", "b"]);
    }

    #[test]
    fn zero_or_two_matches_name_the_frame_and_fail() {
        let t = tree("zero", &[("f_15-09-2026-08-00-00", &[SLAVE])], None);
        let e = samples_from_dir(&pair_spec(&t.0, None, None, SortBy::Name)).unwrap_err();
        assert!(e.contains("f_15-09-2026-08-00-00"), "{e}");
        assert!(e.contains("*Master*.pcd"), "{e}");

        let t2 = tree(
            "two",
            &[(
                "f_15-09-2026-08-00-00",
                &["a_Master_1.pcd", "b_Master_2.pcd", SLAVE],
            )],
            None,
        );
        let e2 = samples_from_dir(&pair_spec(&t2.0, None, None, SortBy::Name)).unwrap_err();
        assert!(e2.contains("f_15-09-2026-08-00-00"), "{e2}");
        assert!(e2.contains("a_Master_1.pcd"), "{e2}");
        assert!(e2.contains("b_Master_2.pcd"), "{e2}");
    }

    #[test]
    fn a_single_bind_binds_one_glob_per_frame() {
        let t = tree(
            "single",
            &[
                ("f_15-09-2026-08-00-00", &[MASTER]),
                ("f_15-09-2026-08-01-00", &[MASTER]),
            ],
            None,
        );
        let spec = DirSpec {
            root: t.0.clone(),
            subdir: None,
            binds: vec!["r.path".to_string()],
            patterns: vec!["*.pcd".to_string()],
            sort_by: SortBy::Name,
            split_half: None,
        };
        let (samples, _) = samples_from_dir(&spec).expect("配对");
        assert_eq!(samples.len(), 2);
        assert_eq!(samples[0].set.len(), 1);
        assert_eq!(samples[0].set[0].0, "r.path");
    }

    #[test]
    fn the_generated_sample_set_round_trips_through_jsonl() {
        let files: &[&str] = &[MASTER, SLAVE];
        let t = tree(
            "jsonl",
            &[
                ("f_15-09-2026-08-00-00", files),
                ("f_15-09-2026-08-01-00", files),
            ],
            None,
        );
        let (samples, _) =
            samples_from_dir(&pair_spec(&t.0, None, Some("half"), SortBy::Name)).expect("配对");
        let text = samples_jsonl(&samples);
        let back = parse_samples(&text, "<mem>").expect("回读");
        assert_eq!(back.len(), 2);
        assert_eq!(back[0].id, samples[0].id);
        assert_eq!(back[0].set, samples[0].set);
        assert_eq!(back[0].tags.get("half").map(String::as_str), Some("a"));
        assert_eq!(back[1].tags.get("half").map(String::as_str), Some("b"));
    }
}
