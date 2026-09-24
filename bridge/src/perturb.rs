use serde_json::{json, Map, Value};

use crate::cli::{
    core, json_line, line, load_graph, parse_axis, Loaded, Parsed, Sink, EXIT_FAILED, EXIT_INVALID,
    EXIT_USAGE,
};
use crate::eval::{
    collect_samples, csv_text, parse_metric, Engine, EngineError, Grouping, MetricPath, ParamSet,
    Row, Sample,
};
use crate::graph::{Edge, GraphDoc, Node, PortRef};

pub(crate) const DEFAULT_TOLERANCE: f64 = 0.1;
const MAX_LISTED_PATHS: usize = 200;

#[derive(Clone, Debug, PartialEq)]
pub(crate) enum Region {
    Halfspace { point: [f64; 3], normal: [f64; 3] },
    Box { min: [f64; 3], max: [f64; 3] },
}

fn vec3_of(v: Option<&Value>, what: &str) -> Result<[f64; 3], String> {
    let Some(Value::Array(a)) = v else {
        return Err(format!("--region 的 {what} 要是一个三元数组"));
    };
    if a.len() != 3 {
        return Err(format!("--region 的 {what} 要是一个三元数组，收到 {} 个", a.len()));
    }
    let mut out = [0.0f64; 3];
    for (i, item) in a.iter().enumerate() {
        out[i] = item
            .as_f64()
            .ok_or_else(|| format!("--region 的 {what} 第 {i} 个分量不是数字"))?;
    }
    Ok(out)
}

pub(crate) fn parse_region(text: &str) -> Result<Region, String> {
    let value: Value =
        serde_json::from_str(text).map_err(|e| format!("--region 不是合法 JSON: {e}"))?;
    let Some(obj) = value.as_object() else {
        return Err("--region 要是一个 JSON 对象".to_string());
    };
    match obj.get("kind").and_then(Value::as_str) {
        Some("halfspace") => {
            let point = vec3_of(obj.get("point"), "point")?;
            let normal = vec3_of(obj.get("normal"), "normal")?;
            if normal.iter().all(|v| *v == 0.0) {
                return Err("--region 的 normal 是零向量，分不出哪一侧".to_string());
            }
            Ok(Region::Halfspace { point, normal })
        }
        Some("box") => {
            let min = vec3_of(obj.get("min"), "min")?;
            let max = vec3_of(obj.get("max"), "max")?;
            for i in 0..3 {
                if min[i] > max[i] {
                    return Err("--region 的 min 每个分量都要不大于 max".to_string());
                }
            }
            Ok(Region::Box { min, max })
        }
        Some(other) => Err(format!(
            "--region 的 kind 只认 halfspace 与 box，收到 {other}"
        )),
        None => Err("--region 缺 kind（halfspace 或 box）".to_string()),
    }
}

impl Region {
    pub(crate) fn params(&self) -> Map<String, Value> {
        let mut m = Map::new();
        match self {
            Region::Halfspace { point, normal } => {
                m.insert("regionKind".to_string(), json!("halfspace"));
                m.insert("point".to_string(), json!(point));
                m.insert("normal".to_string(), json!(normal));
            }
            Region::Box { min, max } => {
                m.insert("regionKind".to_string(), json!("box"));
                m.insert("boxMin".to_string(), json!(min));
                m.insert("boxMax".to_string(), json!(max));
            }
        }
        m.insert("translation".to_string(), json!([0.0, 0.0, 0.0]));
        m
    }
}

pub(crate) fn parse_after(spec: &str) -> Result<(String, String), String> {
    let (node, port) = spec
        .split_once(':')
        .ok_or_else(|| format!("--after 的写法是 <节点>:<端口>，收到 {spec}"))?;
    if node.is_empty() || port.is_empty() {
        return Err(format!("--after 的写法是 <节点>:<端口>，收到 {spec}"));
    }
    if node.contains('/') || port.contains('/') {
        return Err(format!(
            "--after {spec} 指向子图内部端口：本版本只能在顶层图的端口后插扰动节点"
        ));
    }
    Ok((node.to_string(), port.to_string()))
}

pub(crate) fn unique_id(doc: &GraphDoc, base: &str) -> String {
    let taken = |id: &str| doc.nodes.iter().any(|n| n.id == id);
    if !taken(base) {
        return base.to_string();
    }
    let mut n = 2usize;
    loop {
        let candidate = format!("{base}_{n}");
        if !taken(&candidate) {
            return candidate;
        }
        n += 1;
    }
}

pub(crate) fn insert_after(
    doc: &mut GraphDoc,
    node: &str,
    port: &str,
    region: &Region,
) -> Result<String, String> {
    if !doc.nodes.iter().any(|n| n.id == node) {
        return Err(format!("图里没有节点 {node}"));
    }
    let id = unique_id(doc, "__perturb");
    for edge in doc.edges.iter_mut() {
        if edge.from.node == node && edge.from.port == port {
            edge.from = PortRef {
                node: id.clone(),
                port: "cloud".to_string(),
            };
        }
    }
    for (_, out) in doc.outputs.iter_mut() {
        if out.node == node && out.port == port {
            out.node = id.clone();
            out.port = "cloud".to_string();
        }
    }
    let mut edge_id = format!("__perturb_in_{id}");
    let mut n = 2usize;
    while doc.edges.iter().any(|e| e.id == edge_id) {
        edge_id = format!("__perturb_in_{id}_{n}");
        n += 1;
    }
    doc.edges.push(Edge {
        id: edge_id,
        from: PortRef {
            node: node.to_string(),
            port: port.to_string(),
        },
        to: PortRef {
            node: id.clone(),
            port: "cloud".to_string(),
        },
    });
    doc.nodes.push(Node {
        id: id.clone(),
        op: "edit.translate_region".to_string(),
        op_version: None,
        bypass: false,
        params: region.params(),
        ui: None,
    });
    Ok(id)
}

pub(crate) fn parse_axis_spec(spec: &str) -> Result<(usize, Vec<f64>), String> {
    let (name, range) = spec
        .split_once('=')
        .ok_or_else(|| format!("--axis 的写法是 <x|y|z>=<start>:<end>:<steps>，收到 {spec}"))?;
    let index = match name.trim().to_ascii_lowercase().as_str() {
        "x" => 0usize,
        "y" => 1,
        "z" => 2,
        other => {
            return Err(format!("--axis 的轴只认 x / y / z，收到 {other}"));
        }
    };
    let axis = parse_axis(&format!("_perturb.translation={range}"))
        .map_err(|e| e.replace("--param", "--axis"))?;
    Ok((index, axis.values))
}

pub(crate) fn displacement_param_sets(node: &str, index: usize, values: &[f64]) -> Vec<ParamSet> {
    values
        .iter()
        .map(|v| {
            let mut t = [0.0f64; 3];
            t[index] = *v;
            let key = format!("{node}.translation");
            let mut display = Map::new();
            display.insert(key, json!(v));
            ParamSet {
                display,
                writes: vec![(
                    node.to_string(),
                    "translation".to_string(),
                    json!([t[0], t[1], t[2]]),
                )],
                graph: Vec::new(),
            }
        })
        .collect()
}

#[derive(Clone, Debug, Default, PartialEq)]
pub(crate) struct Fit {
    pub n: usize,
    pub slope: Option<f64>,
    pub intercept: Option<f64>,
    pub rmse: Option<f64>,
}

pub(crate) fn least_squares(points: &[(f64, f64)]) -> Fit {
    let n = points.len();
    if n < 2 {
        return Fit {
            n,
            ..Fit::default()
        };
    }
    let mean_x = points.iter().map(|p| p.0).sum::<f64>() / n as f64;
    let mean_y = points.iter().map(|p| p.1).sum::<f64>() / n as f64;
    let sxx: f64 = points.iter().map(|p| (p.0 - mean_x) * (p.0 - mean_x)).sum();
    if sxx <= 0.0 {
        return Fit {
            n,
            ..Fit::default()
        };
    }
    let sxy: f64 = points
        .iter()
        .map(|p| (p.0 - mean_x) * (p.1 - mean_y))
        .sum();
    let slope = sxy / sxx;
    let intercept = mean_y - slope * mean_x;
    let sse: f64 = points
        .iter()
        .map(|p| {
            let r = p.1 - (intercept + slope * p.0);
            r * r
        })
        .sum();
    Fit {
        n,
        slope: Some(slope),
        intercept: Some(intercept),
        rmse: Some((sse / n as f64).sqrt()),
    }
}

#[derive(Clone, Debug)]
pub(crate) struct SampleFit {
    pub sample: String,
    pub metric: String,
    pub fit: Fit,
    pub slope_neg: Option<f64>,
    pub slope_pos: Option<f64>,
    pub pass: Option<bool>,
}

pub(crate) fn sign_fold(neg: Option<f64>, pos: Option<f64>) -> bool {
    match (neg, pos) {
        (Some(a), Some(b)) => a * b < 0.0,
        _ => false,
    }
}

pub(crate) fn fit_sample(
    sample: &str,
    metric: &str,
    points: &[(f64, f64)],
    expect: Option<f64>,
    tolerance: f64,
) -> SampleFit {
    let fit = least_squares(points);
    let side = |keep: &dyn Fn(f64) -> bool| {
        let subset: Vec<(f64, f64)> = points.iter().copied().filter(|p| keep(p.0)).collect();
        least_squares(&subset).slope
    };
    let slope_neg = side(&|d: f64| d < 0.0);
    let slope_pos = side(&|d: f64| d > 0.0);
    let same_sign = match (slope_neg, slope_pos) {
        (Some(a), Some(b)) => a * b > 0.0,
        _ => true,
    };
    let pass = expect.map(|want| match fit.slope {
        Some(s) => (s - want).abs() <= tolerance && same_sign,
        None => false,
    });
    SampleFit {
        sample: sample.to_string(),
        metric: metric.to_string(),
        fit,
        slope_neg,
        slope_pos,
        pass,
    }
}

#[derive(Clone, Debug, Default, PartialEq)]
pub(crate) struct Summary {
    pub samples: usize,
    pub pass: usize,
    pub mean: Option<f64>,
    pub std: Option<f64>,
    pub min: Option<f64>,
    pub max: Option<f64>,
    pub non_responsive: usize,
    pub sign_fold: usize,
}

pub(crate) fn summarize_fits(fits: &[SampleFit], tolerance: f64) -> Summary {
    let slopes: Vec<f64> = fits.iter().filter_map(|f| f.fit.slope).collect();
    let mean = if slopes.is_empty() {
        None
    } else {
        Some(slopes.iter().sum::<f64>() / slopes.len() as f64)
    };
    let std = match (slopes.len(), mean) {
        (n, Some(m)) if n >= 2 => {
            let sum: f64 = slopes.iter().map(|v| (v - m) * (v - m)).sum();
            Some((sum / (n - 1) as f64).sqrt())
        }
        _ => None,
    };
    Summary {
        samples: fits.len(),
        pass: fits.iter().filter(|f| f.pass == Some(true)).count(),
        mean,
        std,
        min: slopes.iter().cloned().fold(None, |a: Option<f64>, v| {
            Some(a.map_or(v, |x| x.min(v)))
        }),
        max: slopes.iter().cloned().fold(None, |a: Option<f64>, v| {
            Some(a.map_or(v, |x| x.max(v)))
        }),
        non_responsive: slopes.iter().filter(|s| s.abs() < tolerance / 2.0).count(),
        sign_fold: fits
            .iter()
            .filter(|f| sign_fold(f.slope_neg, f.slope_pos))
            .count(),
    }
}

fn num(v: Option<f64>) -> Value {
    v.map(|x| json!(x)).unwrap_or(Value::Null)
}

pub(crate) fn cmd_perturb(parsed: &Parsed, out: &Sink, err: &Sink) -> i32 {
    let Some(path) = parsed.positional.first().cloned() else {
        line(
            err,
            "用法：lyflow perturb <graph> --after <节点>:<端口> --region <json> --axis <x|y|z>=<start>:<end>:<steps> --metric <path>",
        );
        return EXIT_USAGE;
    };

    let Some(after_spec) = parsed.one("after") else {
        line(err, "缺 --after <节点>:<端口>");
        return EXIT_USAGE;
    };
    let (src_node, src_port) = match parse_after(after_spec) {
        Ok(v) => v,
        Err(e) => {
            line(err, &e);
            return EXIT_USAGE;
        }
    };
    let Some(region_spec) = parsed.one("region") else {
        line(
            err,
            r#"缺 --region，例如 --region {"kind":"halfspace","point":[0,0,0],"normal":[1,0,0]}"#,
        );
        return EXIT_USAGE;
    };
    let region = match parse_region(region_spec) {
        Ok(r) => r,
        Err(e) => {
            line(err, &e);
            return EXIT_USAGE;
        }
    };
    let Some(axis_spec) = parsed.one("axis") else {
        line(err, "缺 --axis <x|y|z>=<start>:<end>:<steps>");
        return EXIT_USAGE;
    };
    let (axis_index, displacements) = match parse_axis_spec(axis_spec) {
        Ok(v) => v,
        Err(e) => {
            line(err, &e);
            return EXIT_USAGE;
        }
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

    let expect = match parsed.one("expect").map(str::parse::<f64>) {
        Some(Ok(v)) => Some(v),
        Some(Err(_)) => {
            line(err, "--expect 要是一个数字");
            return EXIT_USAGE;
        }
        None => None,
    };
    let tolerance = match parsed.one("tolerance").map(str::parse::<f64>) {
        Some(Ok(v)) if v > 0.0 => v,
        Some(_) => {
            line(err, "--tolerance 要是一个正数");
            return EXIT_USAGE;
        }
        None => DEFAULT_TOLERANCE,
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
    let mut loaded = match load_graph(parsed, &path, err) {
        Ok(l) => l,
        Err(e) => {
            line(err, &e);
            return crate::cli::load_exit(&e);
        }
    };
    let perturb_id = match insert_after(&mut loaded.doc, &src_node, &src_port, &region) {
        Ok(id) => id,
        Err(e) => {
            line(err, &e);
            return EXIT_USAGE;
        }
    };
    if let Err(e) = loaded.doc.validate_structure() {
        line(err, &format!("插入扰动节点后图不合法: {e}"));
        return EXIT_INVALID;
    }
    loaded.json = match serde_json::to_string(&loaded.doc) {
        Ok(j) => j,
        Err(e) => {
            line(err, &e.to_string());
            return EXIT_FAILED;
        }
    };
    let loaded: Loaded = loaded;

    let param_sets = displacement_param_sets(&perturb_id, axis_index, &displacements);
    let parallel = parsed
        .one("parallel")
        .and_then(|v| v.parse::<i32>().ok())
        .unwrap_or(0);

    let engine = Engine {
        core: &core,
        base: &loaded,
        pinned: &[],
        metrics: &metrics,
        param_sets: &param_sets,
        samples: &samples,
        parallel,
        no_cache: parsed.has("no-cache"),
        // perturb 报的是斜率，逐样本行不带 summary（要看收尾状态用 eval）
        summary: false,
    };

    let mut rows: Vec<Row> = Vec::new();
    let code = {
        let mut on_row = |row: &Row| {
            let sample = &samples[row.sample];
            let mut m = Map::new();
            for (metric, value) in metrics.iter().zip(&row.metrics) {
                m.insert(metric.raw.clone(), num(*value));
            }
            json_line(
                out,
                &json!({
                    "kind": "perturb_row",
                    "paramSet": row.param_set,
                    "displacement": displacements[row.param_set],
                    "params": Value::Object(param_sets[row.param_set].display.clone()),
                    "sample": sample.id,
                    "tags": sample_tags(sample),
                    "holdout": false,
                    "status": row.status,
                    "metrics": Value::Object(m),
                    "errors": row.errors,
                    "durationMs": row.duration_ms,
                }),
            );
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
                }
                return EXIT_USAGE;
            }
        }
    };

    let mut worst = code;
    for (mi, metric) in metrics.iter().enumerate() {
        let mut fits: Vec<SampleFit> = Vec::new();
        for (si, sample) in samples.iter().enumerate() {
            let points: Vec<(f64, f64)> = rows
                .iter()
                .filter(|r| r.sample == si && r.status == "ok")
                .filter_map(|r| {
                    r.metrics
                        .get(mi)
                        .copied()
                        .flatten()
                        .map(|v| (displacements[r.param_set], v))
                })
                .collect();
            let f = fit_sample(&sample.id, &metric.raw, &points, expect, tolerance);
            json_line(
                out,
                &json!({
                    "kind": "perturb_sample",
                    "sample": f.sample,
                    "metric": f.metric,
                    "n": f.fit.n,
                    "slope": num(f.fit.slope),
                    "intercept": num(f.fit.intercept),
                    "rmse": num(f.fit.rmse),
                    "slopeNeg": num(f.slope_neg),
                    "slopePos": num(f.slope_pos),
                    "pass": f.pass.map(Value::Bool).unwrap_or(Value::Null),
                }),
            );
            fits.push(f);
        }
        let s = summarize_fits(&fits, tolerance);
        json_line(
            out,
            &json!({
                "kind": "perturb_summary",
                "metric": metric.raw,
                "axis": axis_spec,
                "expect": num(expect),
                "tolerance": tolerance,
                "samples": s.samples,
                "pass": s.pass,
                "slopeMean": num(s.mean),
                "slopeStd": num(s.std),
                "slopeMin": num(s.min),
                "slopeMax": num(s.max),
                "nonResponsive": s.non_responsive,
                "signFold": s.sign_fold,
            }),
        );
        if expect.is_some() && s.pass < s.samples {
            worst = worst.max(EXIT_FAILED);
        }
    }

    if let Some(csv_path) = parsed.one("csv") {
        let grouping = Grouping {
            holdout: None,
            group_by: None,
        };
        let text = csv_text(&metrics, &param_sets, &samples, &grouping, &rows);
        if let Err(e) = std::fs::write(csv_path, text) {
            line(err, &format!("写入 {csv_path} 失败: {e}"));
            return EXIT_FAILED;
        }
        line(err, &format!("表格写到 {csv_path}"));
    }

    line(
        err,
        &format!(
            "在 {src_node}:{src_port} 之后插入 {perturb_id}；{} 个位移 × {} 个样本 = {} 次运行",
            displacements.len(),
            samples.len(),
            rows.len()
        ),
    );
    worst
}

fn sample_tags(s: &Sample) -> Value {
    let mut m = Map::new();
    for (k, v) in &s.tags {
        m.insert(k.clone(), Value::String(v.clone()));
    }
    Value::Object(m)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::graph::SCHEMA_VERSION;

    fn doc(nodes: &[(&str, &str)], edges: &[(&str, &str, &str, &str, &str)]) -> GraphDoc {
        GraphDoc {
            schema_version: SCHEMA_VERSION,
            id: "t".into(),
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

    fn halfspace() -> Region {
        Region::Halfspace {
            point: [0.0, 0.0, 0.0],
            normal: [1.0, 0.0, 0.0],
        }
    }

    #[test]
    fn surgery_rewires_every_consumer_of_the_port() {
        let mut g = doc(
            &[
                ("g", "gen.synthetic"),
                ("a", "filter.voxel_grid"),
                ("b", "filter.passthrough"),
            ],
            &[
                ("e1", "g", "cloud", "a", "cloud"),
                ("e2", "g", "cloud", "b", "cloud"),
            ],
        );
        let id = insert_after(&mut g, "g", "cloud", &halfspace()).unwrap();
        assert_eq!(id, "__perturb");
        g.validate_structure().unwrap();
        assert_eq!(g.nodes.len(), 4);
        let inserted = g.nodes.iter().find(|n| n.id == id).unwrap();
        assert_eq!(inserted.op, "edit.translate_region");
        assert_eq!(inserted.params["regionKind"], json!("halfspace"));
        assert_eq!(inserted.params["normal"], json!([1.0, 0.0, 0.0]));

        let into: Vec<&Edge> = g.edges.iter().filter(|e| e.to.node == id).collect();
        assert_eq!(into.len(), 1);
        assert_eq!(into[0].from.node, "g");
        assert_eq!(into[0].from.port, "cloud");
        for name in ["a", "b"] {
            let e = g.edges.iter().find(|e| e.to.node == name).unwrap();
            assert_eq!(e.from.node, id);
            assert_eq!(e.from.port, "cloud");
        }
    }

    #[test]
    fn surgery_leaves_other_ports_alone_and_avoids_id_collisions() {
        let mut g = doc(
            &[
                ("__perturb", "gen.synthetic"),
                ("e", "segment.extract_indices"),
                ("v", "filter.voxel_grid"),
            ],
            &[
                ("e1", "e", "selected", "v", "cloud"),
                ("e2", "e", "rest", "__perturb", "cloud"),
            ],
        );
        let id = insert_after(&mut g, "e", "selected", &halfspace()).unwrap();
        assert_eq!(id, "__perturb_2");
        g.validate_structure().unwrap();
        let rest = g.edges.iter().find(|e| e.id == "e2").unwrap();
        assert_eq!(rest.from.node, "e");
        assert_eq!(rest.from.port, "rest");
        let moved = g.edges.iter().find(|e| e.id == "e1").unwrap();
        assert_eq!(moved.from.node, id);
    }

    #[test]
    fn surgery_refuses_subgraph_internal_ports_and_unknown_nodes() {
        assert!(parse_after("sub1/inner:cloud").is_err());
        assert!(parse_after("n:out/inner").is_err());
        assert!(parse_after("n").is_err());
        assert_eq!(
            parse_after("n_load:primary").unwrap(),
            ("n_load".to_string(), "primary".to_string())
        );
        let mut g = doc(&[("g", "gen.synthetic")], &[]);
        assert!(insert_after(&mut g, "ghost", "cloud", &halfspace()).is_err());
    }

    #[test]
    fn surgery_follows_graph_level_outputs_that_name_the_port() {
        let mut g = doc(&[("g", "gen.synthetic")], &[]);
        g.outputs.insert(
            "cloud".to_string(),
            crate::graph::GraphOutput {
                node: "g".to_string(),
                port: "cloud".to_string(),
                label: None,
            },
        );
        let id = insert_after(&mut g, "g", "cloud", &halfspace()).unwrap();
        assert_eq!(g.outputs["cloud"].node, id);
        assert_eq!(g.outputs["cloud"].port, "cloud");
    }

    #[test]
    fn region_json_maps_onto_the_flat_operator_parameters() {
        let h = parse_region(r#"{"kind":"halfspace","point":[1,2,3],"normal":[0,-1,0]}"#).unwrap();
        assert_eq!(
            h,
            Region::Halfspace {
                point: [1.0, 2.0, 3.0],
                normal: [0.0, -1.0, 0.0]
            }
        );
        let p = h.params();
        assert_eq!(p["regionKind"], json!("halfspace"));
        assert_eq!(p["point"], json!([1.0, 2.0, 3.0]));
        assert_eq!(p["translation"], json!([0.0, 0.0, 0.0]));
        assert!(!p.contains_key("boxMin"));

        let b = parse_region(r#"{"kind":"box","min":[-1,-1,-1],"max":[1,1,1]}"#).unwrap();
        let bp = b.params();
        assert_eq!(bp["regionKind"], json!("box"));
        assert_eq!(bp["boxMin"], json!([-1.0, -1.0, -1.0]));
        assert_eq!(bp["boxMax"], json!([1.0, 1.0, 1.0]));
        assert!(!bp.contains_key("normal"));

        assert!(parse_region(r#"{"kind":"sphere","c":[0,0,0]}"#).is_err());
        assert!(parse_region(r#"{"kind":"halfspace","point":[0,0,0],"normal":[0,0,0]}"#).is_err());
        assert!(parse_region(r#"{"kind":"halfspace","point":[0,0],"normal":[1,0,0]}"#).is_err());
        assert!(parse_region(r#"{"kind":"box","min":[1,1,1],"max":[0,0,0]}"#).is_err());
        assert!(parse_region("not json").is_err());
    }

    #[test]
    fn axis_spec_picks_one_component_and_leaves_the_others_at_zero() {
        let (i, values) = parse_axis_spec("y=-0.2:0.2:5").unwrap();
        assert_eq!(i, 1);
        let want = [-0.2, -0.1, 0.0, 0.1, 0.2];
        assert_eq!(values.len(), want.len());
        for (a, b) in values.iter().zip(want.iter()) {
            assert!((a - b).abs() < 1e-12, "{values:?}");
        }
        let sets = displacement_param_sets("__perturb", i, &values);
        assert_eq!(sets.len(), 5);
        assert_eq!(sets[0].writes[0].0, "__perturb");
        assert_eq!(sets[0].writes[0].1, "translation");
        assert_eq!(sets[0].writes[0].2, json!([0.0, -0.2, 0.0]));
        assert!((sets[4].writes[0].2[1].as_f64().unwrap() - 0.2).abs() < 1e-12);
        assert_eq!(sets[4].writes[0].2[0], json!(0.0));
        assert!((sets[4].display["__perturb.translation"].as_f64().unwrap() - 0.2).abs() < 1e-12);

        assert!(parse_axis_spec("w=0:1:2").is_err());
        assert!(parse_axis_spec("x=0:1").is_err());
        assert!(parse_axis_spec("x").is_err());
    }

    #[test]
    fn a_perfect_follower_has_slope_one_and_no_fold() {
        let points: Vec<(f64, f64)> = [-2.0, -1.0, 0.0, 1.0, 2.0]
            .iter()
            .map(|d| (*d, 10.0 + *d))
            .collect();
        let f = fit_sample("s", "outputs.gap", &points, Some(1.0), 0.1);
        assert_eq!(f.fit.n, 5);
        assert!((f.fit.slope.unwrap() - 1.0).abs() < 1e-12);
        assert!((f.fit.intercept.unwrap() - 10.0).abs() < 1e-12);
        assert!(f.fit.rmse.unwrap() < 1e-12);
        assert!((f.slope_neg.unwrap() - 1.0).abs() < 1e-12);
        assert!((f.slope_pos.unwrap() - 1.0).abs() < 1e-12);
        assert_eq!(f.pass, Some(true));
        assert!(!sign_fold(f.slope_neg, f.slope_pos));
    }

    #[test]
    fn absolute_value_folding_shows_up_as_opposite_side_slopes() {
        let points: Vec<(f64, f64)> = [-2.0, -1.0, 0.0, 1.0, 2.0]
            .iter()
            .map(|d: &f64| (*d, d.abs()))
            .collect();
        let f = fit_sample("s", "outputs.flush", &points, Some(1.0), 0.1);
        assert!(f.fit.slope.unwrap().abs() < 1e-12);
        assert!((f.slope_neg.unwrap() + 1.0).abs() < 1e-12);
        assert!((f.slope_pos.unwrap() - 1.0).abs() < 1e-12);
        assert!(sign_fold(f.slope_neg, f.slope_pos));
        assert_eq!(f.pass, Some(false));

        let s = summarize_fits(&[f], 0.1);
        assert_eq!(s.samples, 1);
        assert_eq!(s.pass, 0);
        assert_eq!(s.sign_fold, 1);
        assert_eq!(s.non_responsive, 1);
    }

    #[test]
    fn a_non_responsive_reading_is_a_zero_slope() {
        let points: Vec<(f64, f64)> = [-2.0, -1.0, 0.0, 1.0, 2.0]
            .iter()
            .map(|d| (*d, 3.7))
            .collect();
        let f = fit_sample("audio", "outputs.gap", &points, Some(1.0), 0.1);
        assert_eq!(f.fit.slope, Some(0.0));
        assert_eq!(f.pass, Some(false));
        let s = summarize_fits(&[f], 0.1);
        assert_eq!(s.non_responsive, 1);
        assert_eq!(s.sign_fold, 0);
        assert_eq!(s.mean, Some(0.0));
        assert_eq!(s.std, None);
    }

    #[test]
    fn a_fit_needs_two_distinct_displacements() {
        assert_eq!(least_squares(&[]).slope, None);
        assert_eq!(least_squares(&[(1.0, 2.0)]).n, 1);
        assert_eq!(least_squares(&[(1.0, 2.0)]).slope, None);
        assert_eq!(least_squares(&[(1.0, 2.0), (1.0, 3.0)]).slope, None);
        let one_sided = fit_sample("s", "m", &[(0.0, 1.0), (1.0, 2.0)], None, 0.1);
        assert_eq!(one_sided.slope_neg, None);
        assert_eq!(one_sided.slope_pos, None);
        assert_eq!(one_sided.pass, None);
    }

    #[test]
    fn the_summary_counts_pass_non_responsive_and_fold_separately() {
        let good: Vec<(f64, f64)> = [-2.0, -1.0, 1.0, 2.0].iter().map(|d| (*d, *d)).collect();
        let flat: Vec<(f64, f64)> = [-2.0, -1.0, 1.0, 2.0].iter().map(|d| (*d, 1.0)).collect();
        let fold: Vec<(f64, f64)> = [-2.0, -1.0, 1.0, 2.0]
            .iter()
            .map(|d: &f64| (*d, d.abs()))
            .collect();
        let fits = vec![
            fit_sample("a", "m", &good, Some(1.0), 0.1),
            fit_sample("b", "m", &flat, Some(1.0), 0.1),
            fit_sample("c", "m", &fold, Some(1.0), 0.1),
        ];
        let s = summarize_fits(&fits, 0.1);
        assert_eq!(s.samples, 3);
        assert_eq!(s.pass, 1);
        assert_eq!(s.non_responsive, 2);
        assert_eq!(s.sign_fold, 1);
        assert_eq!(s.min, Some(0.0));
        assert_eq!(s.max, Some(1.0));
        assert!((s.mean.unwrap() - 1.0 / 3.0).abs() < 1e-12);
    }
}
