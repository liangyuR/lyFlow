//! Annotation scoring shared by CLI and MCP. Does not alter detection or product specifications.
use super::{descend, parse_metric, RunView};
use serde_json::{json, Map, Value};
use std::collections::BTreeSet;

pub(super) const MEASUREMENT_ERROR_METRICS: &[&str] = &["measurementMae", "measurementMaxError"];

pub(super) const METRICS: &[&str] = &[
    "executionOk",
    "poseFail",
    "poseMissing",
    "measurementMissing",
    "measurementCount",
    "measurementMae",
    "measurementMaxError",
    "measurementWithinTolerance",
    "defectTp",
    "defectFn",
    "defectFp",
    "defectRecall",
    "defectMissRate",
    "defectPrecision",
    "breakEndpointMae",
    "breakEndpointMaxError",
    "breakWithinTolerance",
    "breakOutputMissing",
    "productFalseAccept",
    "productFalseReject",
    "productCorrect",
    "productMissing",
];

fn array<'a>(v: &'a Value, name: &str) -> Result<&'a Vec<Value>, String> {
    v.get(name)
        .and_then(Value::as_array)
        .ok_or_else(|| format!("truth.{name} 必须是数组"))
}

pub(super) fn validate(truth: &Value) -> Result<(), String> {
    let obj = truth.as_object().ok_or("truth 必须是对象")?;
    for key in obj.keys() {
        if !["pose", "verdict", "measurements", "breaks"].contains(&key.as_str()) {
            return Err(format!("truth 不认识字段 {key}"));
        }
    }
    for key in ["pose", "verdict"] {
        if let Some(v) = truth.get(key) {
            if v["output"].as_str().map_or(true, str::is_empty) || !v["ok"].is_boolean() {
                return Err(format!("truth.{key} 需要 output 名和布尔 ok"));
            }
        }
    }
    if truth.get("measurements").is_some() {
        for m in array(truth, "measurements")? {
            parse_metric(m["path"].as_str().ok_or("truth.measurements 需要 path")?)?;
            if m["value"].as_f64().is_none()
                || m["unit"].as_str().map_or(true, str::is_empty)
                || m["unitPath"].as_str().map_or(true, str::is_empty)
            {
                return Err(
                    "truth.measurements 需要数值 value、unit、unitPath；不跨单位比较".into(),
                );
            }
            let unit_path = m["unitPath"].as_str().unwrap();
            parse_metric(unit_path)?;
            if let Some(t) = m.get("tolerance") {
                if t.as_f64().map_or(true, |n| n < 0.0) {
                    return Err("measurement tolerance 必须非负".into());
                }
            }
        }
    }
    if let Some(b) = truth.get("breaks") {
        if b["output"].as_str().map_or(true, str::is_empty) {
            return Err("truth.breaks 需要 output".into());
        }
        let image = b["coordinate"] == "image_px";
        if b.get("coordinate")
            .is_some_and(|v| v != "path_s_px" && v != "image_px")
        {
            return Err("断口坐标用 path_s_px 或 image_px；length 的 mm 不能当 s 坐标".into());
        }
        if image && b["lineOutput"].as_str().map_or(true, str::is_empty) {
            return Err("image_px 真值需要 lineOutput".into());
        }
        let intervals = array(b, "intervals")?;
        for i in intervals {
            if image {
                if point(&i["start"]).is_none() || point(&i["end"]).is_none() {
                    return Err("image_px 断口需要 start/end 两个像素点".into());
                }
            } else if i["sStart"].as_f64().is_none()
                || i["sEnd"].as_f64().is_none()
                || i["sEnd"].as_f64() <= i["sStart"].as_f64()
            {
                return Err("truth.breaks 每段需要 sStart < sEnd".into());
            }
        }
        if b.get("maxProjectionDistance")
            .is_some_and(|v| v.as_f64().map_or(true, |n| n < 0.0))
        {
            return Err("maxProjectionDistance 需要非负 px".into());
        }
        if b.get("minIou")
            .is_some_and(|v| v.as_f64().map_or(true, |n| !(0.0..=1.0).contains(&n)))
        {
            return Err("minIou 必须在 [0,1]".into());
        }
        if b.get("endpointTolerance")
            .is_some_and(|v| v.as_f64().map_or(true, |n| n < 0.0))
        {
            return Err("endpointTolerance 必须非负 px".into());
        }
    }
    Ok(())
}

fn output<'a>(view: &RunView<'a>, name: &str) -> Option<&'a Value> {
    view.outputs?.get(name)?.get("value")
}

fn data(v: &Value) -> &Value {
    v.get("data").unwrap_or(v)
}

fn point(v: &Value) -> Option<(f64, f64)> {
    let a = v.as_array()?;
    if a.len() != 2 {
        return None;
    }
    Some((a[0].as_f64()?, a[1].as_f64()?))
}
fn project(line: &Value, p: (f64, f64), max_distance: Option<f64>) -> Option<f64> {
    let pts = line["points"].as_array()?;
    let ss = line["s"].as_array()?;
    if pts.len() < 2 || pts.len() != ss.len() {
        return None;
    }
    let mut best: Option<(f64, f64)> = None;
    for i in 0..pts.len() - 1 {
        let a = point(&pts[i])?;
        let b = point(&pts[i + 1])?;
        let (s0, s1) = (ss[i].as_f64()?, ss[i + 1].as_f64()?);
        if s1 < s0 {
            return None;
        }
        let (dx, dy) = (b.0 - a.0, b.1 - a.1);
        let l2 = dx * dx + dy * dy;
        let t = (if l2 > 0.0 {
            ((p.0 - a.0) * dx + (p.1 - a.1) * dy) / l2
        } else {
            0.0
        })
        .clamp(0.0, 1.0);
        let d = (p.0 - a.0 - t * dx).powi(2) + (p.1 - a.1 - t * dy).powi(2);
        if best.map_or(true, |v| d < v.0) {
            best = Some((d, s0 + t * (s1 - s0)));
        }
    }
    let (distance, s) = best?;
    if max_distance.is_some_and(|max| distance > max * max) {
        None
    } else {
        Some(s)
    }
}

fn unit_at<'a>(view: &RunView<'a>, path: &str) -> Option<&'a str> {
    let m = parse_metric(path).ok()?;
    match m.kind {
        super::MetricKind::Output { name, rest } => descend(output(view, &name)?, &rest)?.as_str(),
        super::MetricKind::NodePort { node, port, rest } => view.node_states(&node).find_map(|e| {
            let p = e["stats"]["outputs"]
                .as_array()?
                .iter()
                .find(|o| o["port"] == port)?;
            descend(p.get("value")?, &rest)?.as_str()
        }),
        _ => None,
    }
}

/// Maximum cardinality interval matching; a prediction can match at most one annotation.
/// Overlap must be positive even when minIou=0. Ties prefer greater IoU then original index.
fn match_intervals(
    expected: &[(f64, f64)],
    got: &[(f64, f64)],
    min_iou: f64,
) -> Vec<(usize, usize)> {
    let edges: Vec<Vec<usize>> = expected
        .iter()
        .map(|&(a, b)| {
            let mut options: Vec<(usize, f64)> = got
                .iter()
                .enumerate()
                .filter_map(|(j, &(c, d))| {
                    let overlap = b.min(d) - a.max(c);
                    let iou = overlap / (b.max(d) - a.min(c));
                    (overlap > 0.0 && iou >= min_iou).then_some((j, iou))
                })
                .collect();
            options.sort_by(|a, b| b.1.total_cmp(&a.1).then(a.0.cmp(&b.0)));
            options.into_iter().map(|x| x.0).collect()
        })
        .collect();
    fn augment(
        i: usize,
        edges: &[Vec<usize>],
        owner: &mut [Option<usize>],
        seen: &mut [bool],
    ) -> bool {
        for &j in &edges[i] {
            if seen[j] {
                continue;
            }
            seen[j] = true;
            if owner[j].map_or(true, |old| augment(old, edges, owner, seen)) {
                owner[j] = Some(i);
                return true;
            }
        }
        false
    }
    let mut owner = vec![None; got.len()];
    for i in 0..expected.len() {
        augment(i, &edges, &mut owner, &mut vec![false; got.len()]);
    }
    owner
        .into_iter()
        .enumerate()
        .filter_map(|(j, i)| i.map(|i| (i, j)))
        .collect()
}

pub(super) fn score(view: &RunView, truth: Option<&Value>, status: &str) -> Value {
    let mut metrics = Map::new();
    metrics.insert("executionOk".into(), json!(u8::from(status == "ok")));
    let mut missing = Vec::<Value>::new();
    let mut evidence = Map::new();
    let mut report_missing = |field: &str, why: &str| {
        missing.push(json!({"field":field,"reason":why}));
    };
    if let Some(t) = truth {
        for (key, label) in [("pose", "pose"), ("verdict", "product")] {
            let Some(target) = t.get(key) else { continue };
            let name = target["output"].as_str().unwrap();
            let got = output(view, name).and_then(|v| data(v)["ok"].as_bool());
            let expected = target["ok"].as_bool().unwrap();
            if got.is_none() {
                report_missing(key, "output_missing_or_invalid_ok");
            }
            metrics.insert(format!("{label}Missing"), json!(u8::from(got.is_none())));
            if key == "pose" {
                metrics.insert(
                    "poseFail".into(),
                    json!(u8::from(expected && got != Some(true))),
                );
            } else {
                metrics.insert(
                    "productCorrect".into(),
                    got.map(|b| json!(u8::from(b == expected)))
                        .unwrap_or(Value::Null),
                );
                metrics.insert(
                    "productFalseAccept".into(),
                    got.map(|b| json!(u8::from(b && !expected)))
                        .unwrap_or(Value::Null),
                );
                metrics.insert(
                    "productFalseReject".into(),
                    got.map(|b| json!(u8::from(!b && expected)))
                        .unwrap_or(Value::Null),
                );
            }
            evidence.insert(
                key.into(),
                json!({"output":name,"expected":expected,"actual":got}),
            );
        }
        if let Some(ms) = t["measurements"].as_array() {
            let mut errors = Vec::new();
            let mut error_units = BTreeSet::new();
            let mut within = Vec::new();
            let mut rows = Vec::new();
            for m in ms {
                let path = m["path"].as_str().unwrap();
                let actual = view.resolve(&parse_metric(path).unwrap());
                let unit = unit_at(view, m["unitPath"].as_str().unwrap());
                let expected = m["value"].as_f64().unwrap();
                let valid = actual.is_some() && unit == m["unit"].as_str();
                let error = actual.filter(|_| valid).map(|n| (n - expected).abs());
                if !valid {
                    report_missing(
                        path,
                        if actual.is_none() {
                            "value_missing"
                        } else {
                            "unit_mismatch_or_missing"
                        },
                    );
                }
                if let Some(e) = error {
                    errors.push(e);
                    error_units.insert(unit.unwrap());
                    if let Some(tol) = m["tolerance"].as_f64() {
                        within.push(u8::from(e <= tol));
                    }
                }
                rows.push(json!({"path":path,"actual":actual,"expected":expected,"unit":unit,"absError":error}));
            }
            let count = errors.len();
            let comparable = error_units.len() == 1;
            if error_units.len() > 1 {
                report_missing("measurements.absError", "mixed_error_units");
            }
            metrics.insert("measurementCount".into(), json!(ms.len()));
            metrics.insert("measurementMissing".into(), json!(ms.len() - count));
            metrics.insert(
                "measurementMae".into(),
                comparable
                    .then(|| json!(errors.iter().sum::<f64>() / count as f64))
                    .unwrap_or(Value::Null),
            );
            metrics.insert(
                "measurementMaxError".into(),
                comparable
                    .then(|| errors.iter().copied().reduce(f64::max))
                    .flatten()
                    .map(|n| json!(n))
                    .unwrap_or(Value::Null),
            );
            metrics.insert(
                "measurementWithinTolerance".into(),
                (!within.is_empty())
                    .then(|| {
                        json!(within.iter().map(|n| *n as f64).sum::<f64>() / within.len() as f64)
                    })
                    .unwrap_or(Value::Null),
            );
            evidence.insert("measurementErrorUnits".into(), json!(error_units));
            evidence.insert(
                "measurementErrorUnit".into(),
                comparable
                    .then(|| json!(error_units.first().unwrap()))
                    .unwrap_or(Value::Null),
            );
            evidence.insert("measurements".into(), json!(rows));
        }
        if let Some(b) = t.get("breaks") {
            let name = b["output"].as_str().unwrap();
            let value = output(view, name).map(data);
            let raw = value.and_then(|v| v["breaks"].as_array());
            let annotations = b["intervals"].as_array().unwrap();
            let projected: Option<Vec<(f64, f64)>> = annotations
                .iter()
                .map(|v| {
                    let (a, z) = if b["coordinate"] == "image_px" {
                        let line = output(view, b["lineOutput"].as_str()?)?;
                        (
                            project(
                                data(line),
                                point(&v["start"])?,
                                b["maxProjectionDistance"].as_f64(),
                            )?,
                            project(
                                data(line),
                                point(&v["end"])?,
                                b["maxProjectionDistance"].as_f64(),
                            )?,
                        )
                    } else {
                        (v["sStart"].as_f64()?, v["sEnd"].as_f64()?)
                    };
                    ((z - a).abs() > 0.0).then_some((a.min(z), a.max(z)))
                })
                .collect();
            let projection_failed = projected.is_none();
            let expected = projected.unwrap_or_default();
            let got: Vec<(f64, f64)> = raw
                .into_iter()
                .flatten()
                .filter_map(|v| {
                    let (a, b) = (v["sStart"].as_f64()?, v["sEnd"].as_f64()?);
                    (b > a).then_some((a, b))
                })
                .collect();
            let unavailable = projection_failed
                || raw.is_none()
                || raw.is_some_and(|r| r.len() != got.len())
                || value.is_some_and(|v| v["pathOk"] == false);
            metrics.insert("breakOutputMissing".into(), json!(u8::from(unavailable)));
            if unavailable {
                report_missing("breaks", "break_output_missing_invalid_or_path_failed");
            }
            if projection_failed {
                report_missing("breaks.intervals", "truth_projection_failed");
            }
            let matches = if unavailable {
                Vec::new()
            } else {
                match_intervals(&expected, &got, b["minIou"].as_f64().unwrap_or(0.0))
            };
            let tp = matches.len();
            let fn_ = annotations.len() - tp;
            metrics.insert("defectTp".into(), json!(tp));
            metrics.insert("defectFn".into(), json!(fn_));
            metrics.insert(
                "defectFp".into(),
                (!unavailable)
                    .then(|| json!(got.len() - tp))
                    .unwrap_or(Value::Null),
            );
            metrics.insert(
                "defectRecall".into(),
                (!annotations.is_empty())
                    .then(|| json!(tp as f64 / annotations.len() as f64))
                    .unwrap_or(Value::Null),
            );
            metrics.insert(
                "defectMissRate".into(),
                (!annotations.is_empty())
                    .then(|| json!(fn_ as f64 / annotations.len() as f64))
                    .unwrap_or(Value::Null),
            );
            metrics.insert(
                "defectPrecision".into(),
                (!unavailable && !got.is_empty())
                    .then(|| json!(tp as f64 / got.len() as f64))
                    .unwrap_or(Value::Null),
            );
            let errors: Vec<f64> = matches
                .iter()
                .flat_map(|&(i, j)| {
                    [
                        (got[j].0 - expected[i].0).abs(),
                        (got[j].1 - expected[i].1).abs(),
                    ]
                })
                .collect();
            metrics.insert(
                "breakEndpointMae".into(),
                (!errors.is_empty())
                    .then(|| json!(errors.iter().sum::<f64>() / errors.len() as f64))
                    .unwrap_or(Value::Null),
            );
            metrics.insert(
                "breakEndpointMaxError".into(),
                errors
                    .iter()
                    .copied()
                    .reduce(f64::max)
                    .map(|n| json!(n))
                    .unwrap_or(Value::Null),
            );
            metrics.insert(
                "breakWithinTolerance".into(),
                b["endpointTolerance"]
                    .as_f64()
                    .filter(|_| !errors.is_empty())
                    .map(|tol| {
                        json!(
                            errors.iter().filter(|&&e| e <= tol).count() as f64
                                / errors.len() as f64
                        )
                    })
                    .unwrap_or(Value::Null),
            );
            evidence.insert("breaks".into(), json!({"output":name,"coordinate":"path_s_px","expected":expected,"actual":got,"matches":matches}));
        }
    }
    json!({"metrics":metrics,"missingReasons":missing,"evidence":evidence})
}
