use std::collections::{BTreeMap, BTreeSet};
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{mpsc, Arc};

use serde_json::{json, Map, Value};

use crate::cli::{
    component_count, core, defaults_by_op, diagnostics_of, execute, has_errors, json_line, line,
    load_graph, parse_axis, Loaded, Parsed, RunGroup, RunRequest, Sink, EXIT_CANCELLED,
    EXIT_FAILED, EXIT_INVALID, EXIT_OK, EXIT_USAGE,
};
use crate::core_ffi::Core;
use crate::graph::GraphDoc;

const MAX_LEAF_DEPTH: usize = 6;
const MAX_LISTED_PATHS: usize = 200;
mod quality;

#[derive(Clone, Debug, PartialEq)]
pub(crate) enum MetricKind {
    Quality(String),
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
        matches!(
            self.kind,
            MetricKind::Output { .. } | MetricKind::Quality(_)
        )
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
        "quality" if segs.len() == 2 && quality::METRICS.contains(&segs[1]) => {
            Ok(MetricKind::Quality(segs[1].to_string()))
        }
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
        // 数组按数字段取下标：Point2D 的 `p.0`、Plane 的 `normal.2`（pointFrom 拿它们当锚点）
        if let Some(items) = cur.as_array() {
            cur = items.get(key.parse::<usize>().ok()?)?;
            continue;
        }
        let obj = cur.as_object()?;
        cur = match obj.get(key) {
            Some(v) => v,
            None => obj.get("data").and_then(|d| d.get(key)).or_else(|| {
                obj.get("fields")?
                    .as_array()?
                    .iter()
                    .find(|f| f["name"] == key.as_str())?
                    .get("value")
            })?,
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

fn quantile(values: &[f64], p: f64) -> Option<f64> {
    if values.is_empty() {
        return None;
    }
    let mut sorted = values.to_vec();
    sorted.sort_by(f64::total_cmp);
    let index = (sorted.len() - 1) as f64 * p;
    let lo = index.floor() as usize;
    Some(sorted[lo] + (sorted[index.ceil() as usize] - sorted[lo]) * index.fract())
}

fn numeric_at(v: &Value, rest: &[String]) -> Option<f64> {
    if let Some((last, path)) = rest.split_last() {
        if let Some(array) = descend(v, path).and_then(Value::as_array) {
            let values: Vec<f64> = array
                .iter()
                .filter_map(scalar_of)
                .filter(|v| v.is_finite())
                .collect();
            let n = values.len();
            let mean = || values.iter().sum::<f64>() / n as f64;
            return match last.as_str() {
                "count" => Some(array.len() as f64),
                "valid" => Some(n as f64),
                "missing" => Some((array.len() - n) as f64),
                "mean" if n > 0 => Some(mean()),
                "min" => values.iter().copied().reduce(f64::min),
                "max" => values.iter().copied().reduce(f64::max),
                "p50" => quantile(&values, 0.5),
                "p95" => quantile(&values, 0.95),
                "std" if n > 1 => Some(
                    (values.iter().map(|v| (v - mean()).powi(2)).sum::<f64>() / (n - 1) as f64)
                        .sqrt(),
                ),
                _ => scalar_of(descend(v, rest)?),
            };
        }
    }
    scalar_of(descend(v, rest)?)
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
            MetricKind::Quality(_) => None, // Engine scores these against each sample's annotation.
            MetricKind::RunDuration => self
                .events
                .iter()
                .rev()
                .find(|e| e["kind"] == "run_finished")
                .and_then(|e| e["durationMs"].as_f64()),
            MetricKind::NodeDuration(node) => self
                .node_states(node)
                .find_map(|e| e["durationMs"].as_f64()),
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
                numeric_at(entry.get("value")?, rest)
            }),
            MetricKind::Output { name, rest } => {
                let entry = self.outputs?.get(name.as_str())?;
                numeric_at(entry.get("value")?, rest)
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
    if let Some(fields) = obj.get("fields").and_then(Value::as_array) {
        for f in fields {
            if let (Some(name), Some(v)) = (f["name"].as_str(), f.get("value")) {
                leaf_or_recurse(prefix, name, v, depth, out);
            }
        }
    }
}

fn leaf_or_recurse(prefix: &str, key: &str, v: &Value, depth: usize, out: &mut BTreeSet<String>) {
    let path = format!("{prefix}.{key}");
    match v {
        Value::Number(_) | Value::Bool(_) => {
            out.insert(path);
        }
        Value::Object(_) => collect_leaves(&path, v, depth + 1, out),
        Value::Array(a) if a.iter().all(|v| v.is_null() || scalar_of(v).is_some()) => {
            for reducer in [
                "count", "valid", "missing", "min", "max", "mean", "std", "p50", "p95",
            ] {
                out.insert(format!("{path}.{reducer}"));
            }
        }
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
        let Some(id) = e["nodeId"].as_str() else {
            continue;
        };
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
                let Some(port) = o["port"].as_str() else {
                    continue;
                };
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
    pub graph_params: Map<String, Value>,
    pub truth: Option<Value>,
}

impl Sample {
    pub(crate) fn whole_graph() -> Self {
        Sample {
            id: "-".to_string(),
            set: Vec::new(),
            tags: BTreeMap::new(),
            graph_params: Map::new(),
            truth: None,
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
    let mut ids = BTreeSet::new();
    for (i, raw) in text.lines().enumerate() {
        let trimmed = raw.trim();
        if trimmed.is_empty() {
            continue;
        }
        let at = format!("{origin} 第 {} 行", i + 1);
        let value: Value =
            serde_json::from_str(trimmed).map_err(|e| format!("{at} 不是合法 JSON: {e}"))?;
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
        if id.is_empty() || !ids.insert(id.to_string()) {
            return Err(format!("{at} id 为空或重复: {id}"));
        }
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
        let graph_params = match obj.get("graphParams") {
            None => Map::new(),
            Some(v) => v
                .as_object()
                .cloned()
                .ok_or_else(|| format!("{at} graphParams 需要对象"))?,
        };
        let truth = obj.get("truth").cloned();
        if let Some(t) = &truth {
            quality::validate(t).map_err(|e| format!("{at} {e}"))?;
        }
        out.push(Sample {
            id: id.to_string(),
            set,
            tags,
            graph_params,
            truth,
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
            graph_params: Map::new(),
            truth: None,
        });
    }
    out
}

#[derive(Clone, Debug, Default)]
pub(crate) struct ParamSet {
    pub display: Map<String, Value>,
    pub writes: Vec<(String, String, Value)>,
    /// 写顶层图参数的那些（`--params` 里不含「.」的键，与 `--param` 同一条区分规则）：
    /// 落成图参数的 default，叠在 `--recipe` 之上、`--param` 之下（param-recipe P4.1）。
    pub graph: Vec<(String, Value)>,
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
            ps.display.insert(k.clone(), v.clone());
            if k.contains('.') {
                let (node, param) = split_target(k, origin)?;
                ps.writes.push((node, param, v.clone()));
            } else {
                ps.graph.push((k.clone(), v.clone()));
            }
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
                merged.writes.retain(|(n, p, _)| !(n == node && p == param));
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
    measurement_units: BTreeSet<String>,
}

impl GroupStats {
    fn include_measurement_units(&mut self, key: &str, quality: Option<&Value>) {
        if !quality::MEASUREMENT_ERROR_METRICS.contains(&key) {
            return;
        }
        for unit in quality
            .and_then(|q| q["evidence"]["measurementErrorUnits"].as_array())
            .into_iter()
            .flatten()
            .filter_map(Value::as_str)
        {
            self.measurement_units.insert(unit.into());
        }
    }
    fn enforce_measurement_units(&mut self) {
        if self.measurement_units.len() > 1 {
            self.values.clear();
            self.ok = 0;
            self.fail_codes.insert("mixed_units".into(), self.n);
        }
    }
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
        self.values
            .iter()
            .cloned()
            .fold(None, |a: Option<f64>, v| Some(a.map_or(v, |x| x.min(v))))
    }
    fn max(&self) -> Option<f64> {
        self.values
            .iter()
            .cloned()
            .fold(None, |a: Option<f64>, v| Some(a.map_or(v, |x| x.max(v))))
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
        let mut result = json!({
            "n": self.n,
            "ok": self.ok,
            "failCodes": Value::Object(codes),
            "mean": num(self.mean()),
            "std": num(self.std()),
            "min": num(self.min()),
            "max": num(self.max()),
            "p2p": p2p,
            "p50": num(quantile(&self.values, 0.5)),
            "p95": num(quantile(&self.values, 0.95)),
            "missing": self.n - self.values.len(),
        });
        if !self.measurement_units.is_empty() {
            result["units"] = json!(self.measurement_units);
            result["unit"] = if self.measurement_units.len() == 1 {
                json!(self.measurement_units.first().unwrap())
            } else {
                result["incomparableReason"] = json!("mixed_units");
                Value::Null
            };
        }
        result
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
        let side = self.holdout.as_ref().map(|_| {
            if self.is_holdout(s) {
                "holdout"
            } else {
                "train"
            }
        });
        let group = self.group_by.as_ref().map(|k| {
            s.tags
                .get(k)
                .cloned()
                .unwrap_or_else(|| "(none)".to_string())
        });
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
    pub quality: Option<Value>,
}

pub(crate) enum EngineError {
    Usage(String, Vec<String>),
    Failed(String),
}

pub(crate) struct Engine<'a> {
    pub started: Option<&'a Sink>,
    pub core: &'a Arc<Core>,
    /// 已经叠好「基础 → --recipe → --param」的图（load_graph）。
    pub base: &'a Loaded,
    /// 命令行上的 `--param <名字>=<json>`：参数组写完图参数之后再写一遍，`--param` 永远最后说了算。
    pub pinned: &'a [String],
    pub metrics: &'a [MetricPath],
    pub param_sets: &'a [ParamSet],
    pub samples: &'a [Sample],
    pub parallel: i32,
    pub no_cache: bool,
    /// 每行带一份 run summary（ADR-0022）。**默认关**，`--summary` 打开 ——
    /// 体积是逐行的，一维 bundle 就 6 KB（m6-plan §10 第 5 条）。
    pub summary: bool,
    /// `--jobs`：同时跑几次。1 = 一次接一次。多于 1 时行仍按顺序交出（run_jobs）。
    pub jobs: usize,
}

/// 终端里的一行进度：「[k/总数] 样本 · 状态 · 耗时 · 还要约 …」，同一行原地刷新（`\r` 加补空格，
/// 不靠 ANSI 转义 —— 老的 Windows 控制台不认），收场时清掉。开不开看 `cli::progress_line()`：
/// 关着的时候一个字都不写。
pub(crate) struct Progress {
    err: Sink,
    on: bool,
    total: usize,
    done: usize,
    started: std::time::Instant,
    /// 上一次写出去的显示宽度：下一次比它短时补空格盖掉
    width: usize,
    /// 终端有多宽：写满一整行会折行（光标到了下一行，\r 回不去），所以最多写到 cols - 1 列
    cols: Option<usize>,
}

impl Progress {
    pub(crate) fn new(err: &Sink, total: usize) -> Self {
        let on = crate::cli::progress_line();
        Self::with(
            err,
            total,
            on,
            if on { crate::cli::stderr_width() } else { None },
        )
    }

    fn with(err: &Sink, total: usize, on: bool, cols: Option<usize>) -> Self {
        Self {
            err: Arc::clone(err),
            on,
            total,
            done: 0,
            started: std::time::Instant::now(),
            width: 0,
            cols,
        }
    }

    /// 又交出了一行。
    pub(crate) fn row(&mut self, what: &str) {
        if !self.on {
            return;
        }
        self.done += 1;
        let left = self.total.saturating_sub(self.done);
        let eta = if left > 0 {
            let per = self.started.elapsed().as_secs_f64() / self.done as f64;
            format!(" · 还要约 {}", human_seconds(per * left as f64))
        } else {
            String::new()
        };
        self.show(&format!("[{}/{}] {what}{eta}", self.done, self.total));
    }

    /// 原地换成这一行（按终端宽度截断、比上一行短就补空格盖掉）。不计数 —— `lyflow run` 自己数节点。
    pub(crate) fn show(&mut self, text: &str) {
        if !self.on {
            return;
        }
        let text = match self.cols {
            Some(cols) => truncate_to_width(text, cols.saturating_sub(1)),
            None => text.to_string(),
        };
        let width = display_width(&text);
        let pad = " ".repeat(self.width.saturating_sub(width));
        self.write(&format!("\r{text}{pad}"));
        self.width = width;
    }

    /// 清掉进度行，光标回到行首 —— 之后的 stderr 照常一行行写。
    pub(crate) fn clear(&mut self) {
        if self.on && self.width > 0 {
            self.write(&format!("\r{}\r", " ".repeat(self.width)));
            self.width = 0;
        }
    }

    fn write(&self, text: &str) {
        if let Ok(mut w) = self.err.lock() {
            let _ = w.write_all(text.as_bytes());
            let _ = w.flush();
        }
    }
}

/// 进度行里的「样本 · 参数组 · 状态 · 耗时」。参数组只有一个时不写。
pub(crate) fn progress_text(row: &Row, sample: Option<&str>, many_sets: bool) -> String {
    let mut parts: Vec<String> = Vec::new();
    if let Some(id) = sample {
        parts.push(truncate_to_width(id, 32));
    }
    if many_sets {
        parts.push(format!("参数组 {}", row.param_set));
    }
    parts.push(row.status.clone());
    parts.push(format!("{:.0} ms", row.duration_ms));
    parts.join(" · ")
}

/// 截到 max 列以内，截掉了就以「…」结尾。
fn truncate_to_width(s: &str, max: usize) -> String {
    if display_width(s) <= max {
        return s.to_string();
    }
    const ELLIPSIS: char = '…';
    let room = max.saturating_sub(char_width(ELLIPSIS));
    let mut out = String::new();
    let mut used = 0;
    for c in s.chars() {
        let w = char_width(c);
        if used + w > room {
            break;
        }
        out.push(c);
        used += w;
    }
    out.push(ELLIPSIS);
    out
}

/// 终端里的显示宽度。U+1100 以上一律按两格算：中日韩字符确实占两格，「…」这类在中文控制台里
/// 也常画成两格 —— 宁可多算（多补几个空格、早一点截断），少算就会折行或留下残字。
fn display_width(s: &str) -> usize {
    s.chars().map(char_width).sum()
}

fn char_width(c: char) -> usize {
    if (c as u32) >= 0x1100 {
        2
    } else {
        1
    }
}

fn human_seconds(s: f64) -> String {
    let s = s.round().max(0.0) as u64;
    match s {
        0..=59 => format!("{s} 秒"),
        60..=3599 => format!("{} 分 {} 秒", s / 60, s % 60),
        _ => format!("{} 小时 {} 分", s / 3600, s % 3600 / 60),
    }
}

/// 没成的那几次归成一行给 stderr：「没成的 10 次：failed 8（io × 5、bad_param × 3）、validation_failed 2（unknown_op × 2）」。
/// 逐行的错误在 stdout 的行里，而 stdout 常被重定向进文件 —— 终端上原来只有「N 次 ok」，看不出其余的为什么没成。
/// 一行有几个错误代码就各数一次；全都 ok 时是 None。
pub(crate) fn failure_digest(rows: &[Row]) -> Option<String> {
    let bad: Vec<&Row> = rows.iter().filter(|r| r.status != "ok").collect();
    if bad.is_empty() {
        return None;
    }
    let mut by_status: BTreeMap<&str, (usize, BTreeMap<&str, usize>)> = BTreeMap::new();
    for r in &bad {
        let (n, codes) = by_status.entry(r.status.as_str()).or_default();
        *n += 1;
        for c in &r.errors {
            *codes.entry(c.as_str()).or_default() += 1;
        }
    }
    let parts: Vec<String> = by_status
        .iter()
        .map(|(status, (n, codes))| {
            if codes.is_empty() {
                return format!("{status} {n}");
            }
            let mut list: Vec<(&str, usize)> = codes.iter().map(|(c, k)| (*c, *k)).collect();
            list.sort_by(|a, b| b.1.cmp(&a.1).then(a.0.cmp(b.0)));
            let codes: Vec<String> = list.iter().map(|(c, k)| format!("{c} × {k}")).collect();
            format!("{status} {n}（{}）", codes.join("、"))
        })
        .collect();
    Some(format!("没成的 {} 次：{}", bad.len(), parts.join("、")))
}

/// 一行的状态对应的退出码。
fn row_exit(status: &str) -> i32 {
    match status {
        "ok" => EXIT_OK,
        "validation_failed" => EXIT_INVALID,
        "cancelled" => EXIT_CANCELLED,
        _ => EXIT_FAILED,
    }
}

struct Attempt {
    row: Row,
    available: Vec<String>,
    resolved: Vec<bool>,
}

impl<'a> Engine<'a> {
    fn variant(&self, ps: &ParamSet, sample: &Sample) -> Result<GraphDoc, String> {
        let mut doc = self.base.doc.clone();
        // 叠加顺序：基础 → 配方（已在 base 里）→ 参数组里的图参数 → --param
        for (name, value) in &ps.graph {
            let decl = doc
                .params
                .get_mut(name)
                .and_then(Value::as_object_mut)
                .ok_or_else(|| format!("unknown_param: 参数组里的 {name} 不是图声明的顶层参数"))?;
            decl.insert("default".to_string(), value.clone());
        }
        if !ps.graph.is_empty() {
            for spec in self.pinned {
                crate::cli::apply_graph_param(&mut doc, spec)?;
            }
        }
        // Sample inputs/geometry are applied last, just like sample.set. They are frozen by the evaluator.
        for (name, value) in &sample.graph_params {
            crate::cli::apply_graph_param(&mut doc, &format!("{name}={value}"))?;
        }
        let mut write = |node_id: &str, param: &str, value: &Value| -> Result<(), String> {
            if let Some(e) = crate::cli::set_conflict(&doc, node_id, param, "eval override") {
                return Err(e);
            }
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
        group: Option<&RunGroup>,
    ) -> Result<Attempt, String> {
        let ps = &self.param_sets[pi];
        let sample = &self.samples[si];
        if let Some(out) = self.started {
            json_line(
                out,
                &json!({"kind":"eval_started","paramSet":pi,"sample":sample.id}),
            );
        }
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
            let quality = quality::score(
                &RunView {
                    events: &[],
                    outputs: None,
                },
                sample.truth.as_ref(),
                "validation_failed",
            );
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
                    metrics: self
                        .metrics
                        .iter()
                        .map(|m| match &m.kind {
                            MetricKind::Quality(k) => quality["metrics"][k.as_str()].as_f64(),
                            _ => None,
                        })
                        .collect(),
                    errors,
                    duration_ms: 0.0,
                    skipped: Vec::new(),
                    summary: None,
                    quality: Some(quality),
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
                params_json: None,
                inputs: &[],
                group,
                on_event: None,
            },
        )?;
        let wants_outputs = enumerate
            || sample.truth.is_some()
            || self.metrics.iter().any(MetricPath::needs_outputs);
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
        let quality = quality::score(&view, sample.truth.as_ref(), &result.status);
        let values: Vec<Option<f64>> = self
            .metrics
            .iter()
            .map(|m| match &m.kind {
                MetricKind::Quality(k) => quality["metrics"][k.as_str()].as_f64(),
                _ => view.resolve(m),
            })
            .collect();
        let available = if enumerate {
            let mut paths = available_paths(&view);
            paths.extend(
                quality["metrics"]
                    .as_object()
                    .unwrap()
                    .keys()
                    .map(|k| format!("quality.{k}")),
            );
            paths
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
                quality: Some(quality),
            },
            available,
            resolved,
        })
    }

    /// `--list-metrics`：只跑第一个参数组 × 第一个样本，列出这张图在这批样本上能取的全部标量路径。
    /// 路径只有跑过才知道（Record 的字段、图输出的值都是运行期的），所以不做静态推导 ——
    /// 与 eval 真跑时第一次运行是同一条路（`attempt(0, 0, true)`），列出来的就是 eval 认的。
    pub(crate) fn metric_paths(&self) -> Result<(Row, Vec<String>), String> {
        if self.param_sets.is_empty() || self.samples.is_empty() {
            return Err("没有参数组或样本，跑不了第一次运行".to_string());
        }
        let a = self.attempt(0, 0, true, None)?;
        Ok((a.row, a.available))
    }

    /// 第 k 次运行是哪组参数 × 哪个样本。参数组在外层：同时在跑的几次多半是同一组参数的不同样本，
    /// 与一次接一次时的顺序也一致。
    fn split(&self, k: usize) -> (usize, usize) {
        (k / self.samples.len(), k % self.samples.len())
    }

    pub(crate) fn run(&self, on_row: &mut dyn FnMut(&Row)) -> Result<i32, EngineError> {
        self.run_remaining(on_row, &BTreeSet::new(), usize::MAX)
    }

    fn run_remaining(
        &self,
        on_row: &mut dyn FnMut(&Row),
        completed: &BTreeSet<(usize, usize)>,
        max_runs: usize,
    ) -> Result<i32, EngineError> {
        let indices: Vec<usize> = (0..self.param_sets.len() * self.samples.len())
            .filter(|&k| !completed.contains(&self.split(k)))
            .take(max_runs)
            .collect();
        let mut first: Option<Attempt> = None;
        if let Some(&k) = indices.first() {
            let (pi, si) = self.split(k);
            let a = self
                .attempt(pi, si, true, None)
                .map_err(EngineError::Failed)?;
            if a.row.status == "ok" {
                let missing: Vec<String> = self
                    .metrics
                    .iter()
                    .zip(&a.resolved)
                    .filter(|(m, ok)| {
                        !**ok
                            && !matches!(m.kind, MetricKind::Quality(_))
                            && !a.available.contains(&m.raw)
                    })
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
        let total = indices.len();
        let mut worst = EXIT_OK;
        // 交出一行。Some(退出码) = 到此为止：这一行被取消了（Ctrl+C），后面的不再跑
        let mut emit = |row: &Row| -> Option<i32> {
            on_row(row);
            match row_exit(&row.status) {
                EXIT_CANCELLED => Some(EXIT_CANCELLED),
                code => {
                    worst = worst.max(code);
                    None
                }
            }
        };
        let mut start = 0;
        if let Some(a) = first.take() {
            if let Some(code) = emit(&a.row) {
                return Ok(code);
            }
            start = 1;
        }
        let stopped = if self.jobs > 1 && total.saturating_sub(start) > 1 {
            self.run_jobs(&indices[start..], &mut emit)?
        } else {
            let mut stopped = None;
            for k in start..total {
                let (pi, si) = self.split(indices[k]);
                let a = self
                    .attempt(pi, si, false, None)
                    .map_err(EngineError::Failed)?;
                stopped = emit(&a.row);
                if stopped.is_some() {
                    break;
                }
            }
            stopped
        };
        Ok(stopped.unwrap_or(worst))
    }

    /// `--jobs`：第 start..total 次分给 jobs 个线程跑，行仍按顺序交给 emit —— 输出与一次接一次时
    /// 逐行相同（durationMs 除外）。core 允许几次运行同时在算，线程预算按整个进程在算的节点数分。
    /// 有一次回来是 cancelled（Ctrl+C 把在跑的都取消了）就整批取消：之后才起的那次一登记就取消。
    fn run_jobs(
        &self,
        indices: &[usize],
        emit: &mut dyn FnMut(&Row) -> Option<i32>,
    ) -> Result<Option<i32>, EngineError> {
        let group = RunGroup::default();
        let work = |k: usize| -> Result<Attempt, String> {
            let (pi, si) = self.split(indices[k]);
            let a = self.attempt(pi, si, false, Some(&group))?;
            if a.row.status == "cancelled" {
                group.cancel();
            }
            Ok(a)
        };
        ordered_parallel(
            0..indices.len(),
            self.jobs,
            &group,
            &work,
            &mut |a: Attempt| emit(&a.row),
        )
        .map_err(EngineError::Failed)
    }
}

/// 把 range 里的任务分给 jobs 个线程做，结果按序号顺序交给 take —— 交出的顺序与一个接一个做时相同。
/// 收场也与一个接一个时停在同一处：
/// - take 返回 Some（这一行被取消了）：不再起新的，group 里还在跑的取消，排在后面的结果丢掉。
/// - 某个任务返回 Err：不再起新的；排在它前面、还在做的照常做完交出，然后返回这个 Err，
///   排在它后面的取消、丢掉。
fn ordered_parallel<T: Send, R>(
    range: std::ops::Range<usize>,
    jobs: usize,
    group: &RunGroup,
    work: &(dyn Fn(usize) -> Result<T, String> + Sync),
    take: &mut dyn FnMut(T) -> Option<R>,
) -> Result<Option<R>, String> {
    let end = range.end;
    let next = AtomicUsize::new(range.start);
    let stop = AtomicBool::new(false);
    let (tx, rx) = mpsc::channel::<(usize, Result<T, String>)>();
    std::thread::scope(|scope| {
        for _ in 0..jobs.min(range.len()) {
            let tx = tx.clone();
            let (next, stop) = (&next, &stop);
            scope.spawn(move || {
                while !stop.load(Ordering::SeqCst) && !group.cancelled() {
                    let k = next.fetch_add(1, Ordering::SeqCst);
                    if k >= end {
                        break;
                    }
                    let result = work(k);
                    if result.is_err() {
                        stop.store(true, Ordering::SeqCst);
                    }
                    if tx.send((k, result)).is_err() {
                        break;
                    }
                }
            });
        }
        drop(tx);
        let mut ready: BTreeMap<usize, Result<T, String>> = BTreeMap::new();
        let mut want = range.start;
        for (k, result) in rx.iter() {
            ready.insert(k, result);
            while let Some(result) = ready.remove(&want) {
                want += 1;
                let done = match result {
                    Err(e) => Err(e),
                    Ok(t) => match take(t) {
                        Some(r) => Ok(Some(r)),
                        None => continue,
                    },
                };
                stop.store(true, Ordering::SeqCst);
                group.cancel();
                return done;
            }
        }
        Ok(None)
    })
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
    metric: &MetricPath,
    rows: &[Row],
) -> BTreeMap<String, GroupStats> {
    let mut groups: BTreeMap<String, GroupStats> = BTreeMap::new();
    for row in rows {
        let sample = &samples[row.sample];
        let entry = groups.entry(grouping.name_of(sample)).or_default();
        entry.n += 1;
        if let MetricKind::Quality(key) = &metric.kind {
            entry.include_measurement_units(key, row.quality.as_ref());
        }
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
    for stats in groups.values_mut() {
        stats.enforce_measurement_units();
    }
    groups
}

/// `eval --list-metrics` 的输出：一行 `metric_paths`。第一次运行没成功时列不出路径，
/// 说清楚是哪种失败（校验不过是退出码 1，执行失败是 2），而不是给一张空表假装「这张图没有指标」。
fn list_metric_paths(engine: &Engine, samples: &[Sample], out: &Sink, err: &Sink) -> i32 {
    let (row, paths) = match engine.metric_paths() {
        Ok(v) => v,
        Err(e) => {
            line(err, &e);
            return EXIT_FAILED;
        }
    };
    if row.status != "ok" {
        line(
            err,
            &format!(
                "第一次运行（参数组 0 × 样本 {}）没成功：{}{}，列不出路径",
                samples[row.sample].id,
                row.status,
                if row.errors.is_empty() {
                    String::new()
                } else {
                    format!("（{}）", row.errors.join(", "))
                }
            ),
        );
        return if row.status == "validation_failed" {
            EXIT_INVALID
        } else {
            EXIT_FAILED
        };
    }
    json_line(
        out,
        &json!({
            "kind": "metric_paths",
            "paramSet": row.param_set,
            "sample": samples[row.sample].id,
            "paths": paths,
        }),
    );
    line(
        err,
        &format!(
            "{} 条标量路径（参数组 0 × 样本 {} 跑出来的）",
            paths.len(),
            samples[row.sample].id
        ),
    );
    EXIT_OK
}

pub(crate) fn cmd_eval(parsed: &Parsed, out: &Sink, err: &Sink) -> i32 {
    let Some(path) = parsed.positional.first().cloned() else {
        line(
            err,
            "用法：lyflow eval <graph> --samples <samples.jsonl> --metric <path>",
        );
        return EXIT_USAGE;
    };

    let metric_specs = parsed.many("metric");
    // --list-metrics 不要指标：它就是用来问「有哪些指标可写」的
    let list_only = parsed.has("list-metrics");
    if metric_specs.is_empty() && !list_only {
        line(err, "至少给一个 --metric <path>，例如 --metric outputs.gap（不知道写什么就先 --list-metrics）");
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
    let (parallel, jobs) =
        match crate::cli::parallel_of(parsed).and_then(|p| Ok((p, crate::cli::jobs_of(parsed)?))) {
            Ok(v) => v,
            Err(e) => {
                line(err, &e);
                return EXIT_USAGE;
            }
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
    if let Err(e) = crate::disk_cache::enable(parsed, &core, err) {
        line(err, &e);
        return EXIT_USAGE;
    }
    let loaded = match load_graph(parsed, &path, err) {
        Ok(l) => l,
        Err(e) => {
            line(err, &e);
            return crate::cli::load_exit(&e);
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
    // --param 里左边带「.」的才是扫描轴；不带的是顶层图参数，load_graph 已经应用过。
    let axes = match axis_param_sets(
        &loaded.doc,
        &defaults,
        &crate::cli::axis_param_specs(parsed),
    ) {
        Ok(v) => v,
        Err(e) => {
            line(err, &e);
            return EXIT_USAGE;
        }
    };
    // 参数组里写图参数的名字要是图声明过的：写错了是参数错（退出码 4），不是跑出来一排 validation_failed
    for (i, ps) in explicit.iter().enumerate() {
        if let Some((name, _)) = ps
            .graph
            .iter()
            .find(|(n, _)| !loaded.doc.params.contains_key(n))
        {
            let known: Vec<&String> = loaded.doc.params.keys().collect();
            line(
                err,
                &format!("unknown_param: --params 第 {i} 组的 {name} 不是图声明的顶层参数（有的是 {known:?}）"),
            );
            return EXIT_USAGE;
        }
    }
    let param_sets = combine_param_sets(explicit, axes);
    for sample in &samples {
        if let Some(name) = sample
            .graph_params
            .keys()
            .find(|n| !loaded.doc.params.contains_key(*n))
        {
            line(
                err,
                &format!("unknown_param: 样本 {} 的 {name} 不是图参数", sample.id),
            );
            return EXIT_USAGE;
        }
        for (key, _) in &sample.set {
            if let Ok((node, param)) = split_target(key, "sample") {
                if let Some(e) = crate::cli::set_conflict(&loaded.doc, &node, &param, key) {
                    line(err, &e);
                    return EXIT_USAGE;
                }
            }
        }
    }
    for ps in &param_sets {
        for (node, param, _) in &ps.writes {
            if let Some(e) = crate::cli::set_conflict(&loaded.doc, node, param, "params") {
                line(err, &e);
                return EXIT_USAGE;
            }
        }
    }
    let pinned: Vec<String> = crate::cli::graph_param_specs(parsed)
        .into_iter()
        .cloned()
        .collect();

    let engine = Engine {
        started: if !list_only && parsed.has("progress-json") {
            Some(out)
        } else {
            None
        },
        core: &core,
        base: &loaded,
        pinned: &pinned,
        metrics: &metrics,
        param_sets: &param_sets,
        samples: &samples,
        parallel,
        no_cache: parsed.has("no-cache"),
        // 默认关（m6-plan §10 第 5 条）：gap 图一维 bundle 就 6 KB，51 帧 × 8 组
        // 参数 2.5 MB —— 一个「每行都带上」的默认值会把 eval 的输出撑成不可读。
        // `--no-summary` 留着当 no-op：老脚本照样跑得过。
        summary: parsed.has("summary"),
        jobs,
    };

    if list_only {
        return list_metric_paths(&engine, &samples, out, err);
    }

    let mut rows: Vec<Row> = match parsed.one("resume-rows") {
        Some(file) => match resume_rows(file, &param_sets, &samples, &metrics) {
            Ok(rows) => rows,
            Err(e) => {
                line(err, &e);
                return EXIT_USAGE;
            }
        },
        None => Vec::new(),
    };
    let completed: BTreeSet<(usize, usize)> =
        rows.iter().map(|r| (r.param_set, r.sample)).collect();
    let max_runs = match crate::cli::uint_opt(parsed, "max-runs", u32::MAX) {
        Ok(n) => n as usize,
        Err(e) => {
            line(err, &e);
            return EXIT_USAGE;
        }
    };
    let mut progress = Progress::new(err, param_sets.len() * samples.len());
    let result = {
        let mut on_row = |row: &Row| {
            let sample = &samples[row.sample];
            progress.row(&progress_text(row, Some(&sample.id), param_sets.len() > 1));
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
                "quality": row.quality,
                "split": sample.tags.get("split"),
                "graphParams": sample.graph_params,
            });
            // summary 是这一行的结论（ADR-0022）：status 三态与每个图输出的三态。
            // 默认不带，`--summary` 才有；没跑到执行期时也没有。
            if let (Some(s), Some(obj)) = (row.summary.as_ref(), line_json.as_object_mut()) {
                obj.insert("summary".to_string(), s.clone());
            }
            json_line(out, &line_json);
            rows.push(row.clone());
        };
        engine.run_remaining(&mut on_row, &completed, max_runs)
    };
    progress.clear();
    let code = match result {
        Ok(c) => rows
            .iter()
            .fold(c, |worst, r| worst.max(row_exit(&r.status))),
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
    };

    if rows.len() < param_sets.len() * samples.len()
        && max_runs < param_sets.len() * samples.len() - completed.len()
    {
        json_line(
            out,
            &json!({"kind":"eval_budget","maxRuns":max_runs,"completed":rows.len(),"total":param_sets.len()*samples.len()}),
        );
    }
    for (pi, ps) in param_sets.iter().enumerate() {
        let of_set: Vec<Row> = rows.iter().filter(|r| r.param_set == pi).cloned().collect();
        if of_set.is_empty() {
            continue;
        }
        for (mi, metric) in metrics.iter().enumerate() {
            // Annotation metrics include execution failures in their denominators.
            let scored: Vec<Row> = of_set
                .iter()
                .cloned()
                .map(|mut r| {
                    if matches!(metric.kind, MetricKind::Quality(_)) {
                        r.status = "ok".into();
                    }
                    r
                })
                .collect();
            let groups = summarize(&samples, &grouping, mi, metric, &scored);
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
        json_line(
            out,
            &json!({"kind":"quality_summary","paramSet":pi,"groups":quality_groups(&samples,&grouping,&of_set)}),
        );
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
    if let Some(digest) = failure_digest(&rows) {
        line(err, &digest);
    }
    code
}

fn resume_rows(
    file: &str,
    params: &[ParamSet],
    samples: &[Sample],
    metrics: &[MetricPath],
) -> Result<Vec<Row>, String> {
    let text = std::fs::read_to_string(file).map_err(|e| e.to_string())?;
    let mut rows = BTreeMap::<(usize, usize), Row>::new();
    for raw in text.lines().filter(|s| !s.trim().is_empty()) {
        let v: Value = serde_json::from_str(raw).map_err(|e| format!("resume JSONL: {e}"))?;
        if v["kind"] != "eval_row" || v["status"] == "cancelled" {
            continue;
        }
        let pi = v["paramSet"].as_u64().ok_or("resume 缺 paramSet")? as usize;
        let si = samples
            .iter()
            .position(|s| v["sample"].as_str() == Some(s.id.as_str()))
            .ok_or("resume 样本不在当前冻结集")?;
        let ps = params.get(pi).ok_or("resume 参数组不存在")?;
        if v["params"] != Value::Object(ps.display.clone()) {
            return Err("resume 参数组已改变".into());
        }
        if metrics.iter().any(|m| v["metrics"].get(&m.raw).is_none()) {
            return Err("resume 指标集合已改变".into());
        }
        let row = Row {
            param_set: pi,
            sample: si,
            status: v["status"].as_str().ok_or("resume 缺 status")?.into(),
            metrics: metrics
                .iter()
                .map(|m| v["metrics"][&m.raw].as_f64())
                .collect(),
            errors: v["errors"]
                .as_array()
                .into_iter()
                .flatten()
                .filter_map(|e| e.as_str().map(str::to_string))
                .collect(),
            duration_ms: v["durationMs"].as_f64().unwrap_or(0.0),
            skipped: Vec::new(),
            summary: v.get("summary").cloned(),
            quality: v.get("quality").cloned(),
        };
        if rows.insert((pi, si), row).is_some() {
            return Err("resume 中样本与参数组组合重复".into());
        }
    }
    Ok(rows.into_values().collect())
}

fn quality_groups(samples: &[Sample], grouping: &Grouping, rows: &[Row]) -> Value {
    let mut groups: BTreeMap<String, Vec<&Row>> = BTreeMap::new();
    for row in rows {
        groups
            .entry(grouping.name_of(&samples[row.sample]))
            .or_default()
            .push(row);
    }
    let mut out = Map::new();
    for (name, rows) in groups {
        let mut stats = Map::new();
        for key in quality::METRICS {
            let mut g = GroupStats {
                n: rows.len(),
                ..Default::default()
            };
            for r in &rows {
                g.include_measurement_units(key, r.quality.as_ref());
                if let Some(v) = r.quality.as_ref().and_then(|q| q["metrics"][*key].as_f64()) {
                    g.ok += 1;
                    g.values.push(v);
                }
            }
            g.enforce_measurement_units();
            if g.ok > 0 || !g.measurement_units.is_empty() {
                stats.insert((*key).into(), g.to_json());
            }
        }
        let sum = |key: &str| {
            rows.iter()
                .filter_map(|r| r.quality.as_ref()?.get("metrics")?.get(key)?.as_f64())
                .sum::<f64>()
        };
        let expected = sum("defectTp") + sum("defectFn");
        let ratio = |a: f64, b: f64| if b > 0.0 { json!(a / b) } else { Value::Null };
        let mut missing = BTreeMap::<String, usize>::new();
        for r in &rows {
            for m in r
                .quality
                .as_ref()
                .and_then(|q| q["missingReasons"].as_array())
                .into_iter()
                .flatten()
            {
                *missing
                    .entry(m["reason"].as_str().unwrap_or("unknown").into())
                    .or_default() += 1;
            }
        }
        let negatives = rows
            .iter()
            .filter(|r| {
                samples[r.sample]
                    .truth
                    .as_ref()
                    .is_some_and(|t| t["verdict"]["ok"] == false)
            })
            .count();
        let positives = rows
            .iter()
            .filter(|r| {
                samples[r.sample]
                    .truth
                    .as_ref()
                    .is_some_and(|t| t["verdict"]["ok"] == true)
            })
            .count();
        out.insert(name,json!({"n":rows.len(),"metrics":stats,"missingReasons":missing,
            "defectRecallMicro":ratio(sum("defectTp"),expected),"defectMissRateMicro":ratio(sum("defectFn"),expected),
            "falsePositives":sum("defectFp"),"falseAcceptRate":ratio(sum("productFalseAccept"),negatives as f64),
            "falseRejectRate":ratio(sum("productFalseReject"),positives as f64),
            "productMissing":sum("productMissing"),"measurementMissing":sum("measurementMissing")}));
    }
    Value::Object(out)
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
            return Err(format!("帧 {name}：找不到目录 {}", look.display()));
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
            graph_params: Map::new(),
            truth: None,
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
    if !s.graph_params.is_empty() {
        o.insert("graphParams".into(), Value::Object(s.graph_params.clone()));
    }
    if let Some(t) = &s.truth {
        o.insert("truth".into(), t.clone());
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
            "--samples-dir 要配 --pattern <glob>；两个相机时用逗号隔开两个 glob".to_string(),
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
        Some(other) => return Err(format!("--sort-by 只认 name 或 mtime，收到 {other}")),
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
        return Err("--samples / --samples-glob / --samples-dir 只能给一个".to_string());
    }
    if dir.is_none() {
        for name in [
            "bind-pair",
            "pattern",
            "sample-subdir",
            "sort-by",
            "split-half",
        ] {
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
            return Err("--bind 要和 --samples-glob 或 --samples-dir 一起给".to_string());
        }
        vec![Sample::whole_graph()]
    };

    if let Some(out) = parsed.one("samples-jsonl-out") {
        std::fs::write(out, samples_jsonl(&samples))
            .map_err(|e| format!("写入 {out} 失败: {e}"))?;
        line(
            err,
            &format!("样本集写到 {out}（{} 个样本）", samples.len()),
        );
    }
    Ok(samples)
}

#[cfg(test)]
mod tests;
