//! eval.rs 的单元测试（从 eval.rs 里搬出来）。

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
                     "value": {"kind": "Measurement", "value": 1.25, "ok": true}},
                    {"port": "at", "type": "Point2D", "elementCount": 1,
                     "value": {"kind": "Point2D", "p": [0.5, -2.0]}}
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

#[derive(Clone, Default)]
struct Buf(Arc<std::sync::Mutex<Vec<u8>>>);

impl std::io::Write for Buf {
    fn write(&mut self, b: &[u8]) -> std::io::Result<usize> {
        self.0.lock().unwrap().extend_from_slice(b);
        Ok(b.len())
    }
    fn flush(&mut self) -> std::io::Result<()> {
        Ok(())
    }
}

/// 进度行：同一行原地刷新（短的那次补空格盖掉长的），清掉之后是一行空白、光标在行首；
/// 关着的时候（管道、MCP、测试）一个字节都不写。
#[test]
fn the_progress_line_rewrites_itself_and_stays_silent_when_off() {
    let buf = Buf::default();
    let sink = crate::cli::sink_of(buf.clone());
    let row = |status: &str, ms: f64| Row {
        param_set: 0,
        sample: 0,
        status: status.to_string(),
        metrics: Vec::new(),
        errors: Vec::new(),
        duration_ms: ms,
        skipped: Vec::new(),
        summary: None,
        quality: None,
    };
    let text = || String::from_utf8(buf.0.lock().unwrap().clone()).unwrap();

    let mut off = Progress::with(&sink, 2, false, Some(80));
    off.row(&progress_text(&row("ok", 1.0), Some("a"), false));
    off.clear();
    assert_eq!(text(), "");

    let mut p = Progress::with(&sink, 2, true, None);
    p.row(&progress_text(
        &row("validation_failed", 12.0),
        Some("一个很长的样本名"),
        true,
    ));
    let first = text();
    assert!(
        first.starts_with(
            "\r[1/2] 一个很长的样本名 · 参数组 0 · validation_failed · 12 ms · 还要约 "
        ),
        "{first}"
    );
    p.row(&progress_text(&row("ok", 3.0), Some("b"), false));
    let second = text()[first.len()..].to_string();
    assert!(second.starts_with("\r[2/2] b · ok · 3 ms"), "{second}");
    assert!(!second.contains("还要约"), "最后一行不报剩余时间");
    assert_eq!(
        display_width(&second),
        display_width(&first),
        "短的这次补空格盖掉上一行"
    );
    p.clear();
    let cleared = text()[first.len() + second.len()..].to_string();
    assert!(cleared.starts_with('\r') && cleared.ends_with('\r') && cleared.trim().is_empty());

    // 比终端窄一列以内：写满一整行会折行，\r 就回不到这一行了
    let mut narrow = Progress::with(&sink, 9, true, Some(20));
    let before = text().len();
    narrow.row(&progress_text(
        &row("ok", 3.0),
        Some("一个很长的样本名"),
        true,
    ));
    let shown = text()[before..].to_string();
    let w = display_width(shown.trim_start_matches('\r'));
    assert!(w <= 19 && shown.ends_with('…'), "{shown:?} {w}");

    assert_eq!(human_seconds(42.4), "42 秒");
    assert_eq!(human_seconds(125.0), "2 分 5 秒");
    assert_eq!(human_seconds(7300.0), "2 小时 1 分");
}

#[test]
fn the_failure_digest_groups_by_status_then_code() {
    let row = |status: &str, errors: &[&str]| Row {
        param_set: 0,
        sample: 0,
        status: status.to_string(),
        metrics: Vec::new(),
        errors: errors.iter().map(|e| e.to_string()).collect(),
        duration_ms: 0.0,
        skipped: Vec::new(),
        summary: None,
        quality: None,
    };
    assert_eq!(failure_digest(&[row("ok", &[])]), None);
    let rows = [
        row("ok", &[]),
        row("failed", &["io"]),
        row("failed", &["bad_param", "io"]),
        row("failed", &["io"]),
        row("validation_failed", &["unknown_op"]),
        row("cancelled", &[]),
    ];
    assert_eq!(
        failure_digest(&rows).as_deref(),
        Some("没成的 5 次：cancelled 1、failed 3（io × 3、bad_param × 1）、validation_failed 1（unknown_op × 1）")
    );
}

/// 第 k 个任务睡 (n - k) × 2 ms：越靠后的越先做完，交出的顺序仍是 0..n。
#[test]
fn ordered_parallel_hands_results_over_in_order() {
    let n = 12;
    let group = RunGroup::default();
    let work = |k: usize| -> Result<usize, String> {
        std::thread::sleep(std::time::Duration::from_millis(((n - k) * 2) as u64));
        Ok(k)
    };
    let mut got = Vec::new();
    let r = ordered_parallel(2..n, 4, &group, &work, &mut |k: usize| {
        got.push(k);
        None::<()>
    });
    assert_eq!(r, Ok(None));
    assert_eq!(got, (2..n).collect::<Vec<_>>());
    assert!(!group.cancelled());
    // 任务比线程少也照常
    let mut few = Vec::new();
    assert_eq!(
        ordered_parallel(0..2, 8, &group, &work, &mut |k| {
            few.push(k);
            None::<()>
        }),
        Ok(None)
    );
    assert_eq!(few, vec![0, 1]);
}

/// 第 5 个出错：0..5 照常交出（4 比 5 晚做完也一样），然后报这个错；之后不再起新的。
#[test]
fn ordered_parallel_stops_at_the_first_error_in_order() {
    let group = RunGroup::default();
    let started = AtomicUsize::new(0);
    let work = |k: usize| -> Result<usize, String> {
        started.fetch_add(1, Ordering::SeqCst);
        if k == 4 {
            std::thread::sleep(std::time::Duration::from_millis(30));
        }
        if k == 5 {
            return Err("第 5 个坏了".to_string());
        }
        Ok(k)
    };
    let mut got = Vec::new();
    let r = ordered_parallel(0..40, 3, &group, &work, &mut |k: usize| {
        got.push(k);
        None::<()>
    });
    assert_eq!(r, Err("第 5 个坏了".to_string()));
    assert_eq!(got, vec![0, 1, 2, 3, 4]);
    assert!(
        started.load(Ordering::SeqCst) < 40,
        "出错之后不该把剩下的都起完"
    );
    assert!(group.cancelled(), "排在后面、还在跑的要取消");
}

/// take 说到此为止（这一行被取消了）：交到这一行为止，整批取消，不再起新的。
#[test]
fn ordered_parallel_stops_when_a_row_says_so() {
    let group = RunGroup::default();
    let started = AtomicUsize::new(0);
    let work = |k: usize| -> Result<usize, String> {
        started.fetch_add(1, Ordering::SeqCst);
        std::thread::sleep(std::time::Duration::from_millis(2));
        Ok(k)
    };
    let mut got = Vec::new();
    let r = ordered_parallel(0..1000, 4, &group, &work, &mut |k: usize| {
        got.push(k);
        (k == 6).then_some("cancelled")
    });
    assert_eq!(r, Ok(Some("cancelled")));
    assert_eq!(got, (0..=6).collect::<Vec<_>>());
    assert!(group.cancelled());
    assert!(started.load(Ordering::SeqCst) < 100, "停了之后不该再起新的");
}

#[test]
fn metric_paths_cover_outputs_nodes_and_run() {
    let ev = events();
    let out = named();
    let view = RunView {
        events: &ev,
        outputs: Some(&out),
    };
    assert_eq!(view.resolve(&metric("run.durationMs")), Some(9.75));
    assert_eq!(view.resolve(&metric("nodes.n_fit.durationMs")), Some(4.5));
    assert_eq!(
        view.resolve(&metric("nodes.n_fit.elementCount")),
        Some(12.0)
    );
    assert_eq!(view.resolve(&metric("nodes.n_fit.byteSize")), Some(480.0));
    assert_eq!(
        view.resolve(&metric("nodes.n_fit.quality.rmsResidualMm")),
        Some(0.03)
    );
    assert_eq!(
        view.resolve(&metric("nodes.n_fit.line.elementCount")),
        Some(1.0)
    );
    assert_eq!(
        view.resolve(&metric("nodes.n_fit.line.hasSegment")),
        Some(1.0)
    );
    assert_eq!(view.resolve(&metric("nodes.n_off.dx")), Some(1.25));
    assert_eq!(view.resolve(&metric("outputs.gap")), Some(1.25));
    assert_eq!(view.resolve(&metric("outputs.gap.ok")), Some(1.0));
    assert_eq!(
        view.resolve(&metric("outputs.bundle.point_counts.left")),
        Some(640.0)
    );
    assert_eq!(view.resolve(&metric("outputs.cloud")), None);
    assert_eq!(view.resolve(&metric("outputs.nope")), None);
    assert_eq!(view.resolve(&metric("nodes.n_fit.nope.x")), None);
    // 数组按数字段取下标（pointFrom 拿 Point2D 的分量当锚点）
    assert_eq!(view.resolve(&metric("nodes.n_off.at.p.1")), Some(-2.0));
    assert_eq!(view.resolve(&metric("nodes.n_off.at.p.2")), None);
    assert_eq!(view.resolve(&metric("nodes.n_off.at.p.x")), None);
}

#[test]
fn a_malformed_metric_path_is_rejected_up_front() {
    for bad in [
        "gap",
        "stuff.gap",
        "run.elementCount",
        "nodes.n_fit",
        "outputs.",
    ] {
        assert!(parse_metric(bad).is_err(), "{bad} 应当被拒");
    }
}

#[test]
fn available_paths_lists_every_scalar_on_the_graph() {
    let ev = events();
    let out = named();
    let paths = available_paths(&RunView {
        events: &ev,
        outputs: Some(&out),
    });
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
    assert_eq!(
        parse_metric("v:cloud.elementCount").unwrap().raw,
        "v:cloud.elementCount"
    );
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
    let annotated = parse_samples(r#"{"id":"x","graphParams":{"template":"t.png"},"truth":{"verdict":{"output":"v","ok":false}},"tags":{"workpiece":"a"}}"#, "t").unwrap();
    assert_eq!(annotated[0].graph_params["template"], json!("t.png"));
    assert_eq!(
        parse_samples(&samples_jsonl(&annotated), "roundtrip").unwrap()[0].truth,
        annotated[0].truth
    );
    for bad in [
        r#"{"id":"x","truth":{"unknown":1}}"#,
        r#"{"id":"x","truth":{"measurements":[{"path":"outputs.width","value":1}]}}"#,
        "{\"id\":\"x\"}\n{\"id\":\"x\"}",
    ] {
        assert!(parse_samples(bad, "bad").is_err());
    }
}

#[test]
fn annotations_separate_failures_units_and_one_to_one_defects() {
    let outputs = json!({"pose":{"value":{"kind":"Record","data":{"ok":false}}},
        "verdict":{"value":{"data":{"ok":true}}},
        "stations":{"value":{"data":{"width":[2.0,null,4.0],"unit":"mm"}}},
        "breaks":{"value":{"data":{"pathOk":true,"breaks":[{"sStart":9,"sEnd":31},{"sStart":10,"sEnd":30},{"sStart":80,"sEnd":90}]}}}});
    let view = RunView {
        events: &[],
        outputs: Some(&outputs),
    };
    let truth = json!({"pose":{"output":"pose","ok":true},"verdict":{"output":"verdict","ok":false},
        "measurements":[{"path":"outputs.stations.width.0","value":2.5,"unit":"mm","unitPath":"outputs.stations.unit","tolerance":0.6},
            {"path":"outputs.stations.width.1","value":3,"unit":"mm","unitPath":"outputs.stations.unit"},
            {"path":"outputs.stations.width.2","value":4,"unit":"px","unitPath":"outputs.stations.unit"}],
        "breaks":{"output":"breaks","intervals":[{"sStart":10,"sEnd":30},{"sStart":50,"sEnd":70}],"minIou":0.5,"endpointTolerance":2}});
    quality::validate(&truth).unwrap();
    let q = quality::score(&view, Some(&truth), "ok");
    for (key, value) in [
        ("executionOk", 1.0),
        ("poseFail", 1.0),
        ("measurementMissing", 2.0),
        ("measurementMae", 0.5),
        ("productFalseAccept", 1.0),
        ("defectTp", 1.0),
        ("defectFn", 1.0),
        ("defectFp", 2.0),
        ("defectRecall", 0.5),
    ] {
        assert_eq!(q["metrics"][key].as_f64(), Some(value), "{key}");
    }
    assert_eq!(q["missingReasons"].as_array().unwrap().len(), 2);
    assert_eq!(q["evidence"]["measurementErrorUnits"], json!(["mm"]));
    let mixed_outputs = json!({"physical":{"value":{"data":{"width":2.0,"unit":"mm"}}},
        "pixel":{"value":{"data":{"width":20.0,"unit":"px"}}}});
    let mixed_truth = json!({"measurements":[
        {"path":"outputs.physical.width","value":2.5,"unit":"mm","unitPath":"outputs.physical.unit","tolerance":0.6},
        {"path":"outputs.pixel.width","value":22.0,"unit":"px","unitPath":"outputs.pixel.unit","tolerance":1.0}]});
    let mixed_view = RunView {
        events: &[],
        outputs: Some(&mixed_outputs),
    };
    let mixed = quality::score(&mixed_view, Some(&mixed_truth), "ok");
    assert!(mixed["metrics"]["measurementMae"].is_null());
    assert!(mixed["metrics"]["measurementMaxError"].is_null());
    assert_eq!(mixed["metrics"]["measurementMissing"], json!(0));
    assert_eq!(mixed["metrics"]["measurementWithinTolerance"], json!(0.5));
    assert_eq!(
        mixed["evidence"]["measurementErrorUnits"],
        json!(["mm", "px"])
    );
    assert_eq!(mixed["evidence"]["measurements"][1]["absError"], json!(2.0));
    assert_eq!(
        mixed["missingReasons"][0]["reason"],
        json!("mixed_error_units")
    );
    let split_quality: Vec<Value> = mixed_truth["measurements"]
        .as_array()
        .unwrap()
        .iter()
        .map(|m| quality::score(&mixed_view, Some(&json!({"measurements":[m]})), "ok"))
        .collect();
    let unit_samples = vec![
        sample("mm", &[("unit", "mm")]),
        sample("px", &[("unit", "px")]),
    ];
    let unit_rows: Vec<Row> = split_quality
        .into_iter()
        .enumerate()
        .map(|(i, q)| {
            let mut r = row(i, q["metrics"]["measurementMae"].as_f64(), "ok", &[]);
            r.quality = Some(q);
            r
        })
        .collect();
    let ungrouped = Grouping {
        holdout: None,
        group_by: None,
    };
    let mae = metric("quality.measurementMae");
    let aggregate = summarize(&unit_samples, &ungrouped, 0, &mae, &unit_rows)["all"].to_json();
    assert!(aggregate["mean"].is_null());
    assert!(aggregate["p95"].is_null());
    assert_eq!(aggregate["missing"], json!(2));
    assert_eq!(aggregate["incomparableReason"], json!("mixed_units"));
    let quality_aggregate = quality_groups(&unit_samples, &ungrouped, &unit_rows);
    for key in quality::MEASUREMENT_ERROR_METRICS {
        let stats = &quality_aggregate["all"]["metrics"][*key];
        assert!(stats["mean"].is_null());
        assert_eq!(stats["incomparableReason"], json!("mixed_units"));
    }
    assert_eq!(
        quality_aggregate["all"]["metrics"]["measurementWithinTolerance"]["mean"],
        json!(0.5)
    );
    let by_unit = Grouping {
        holdout: None,
        group_by: Some("unit".into()),
    };
    let grouped = summarize(&unit_samples, &by_unit, 0, &mae, &unit_rows);
    assert_eq!(grouped["mm"].to_json()["mean"], json!(0.5));
    assert_eq!(grouped["mm"].to_json()["unit"], json!("mm"));
    assert_eq!(grouped["px"].to_json()["mean"], json!(2.0));
    let grouped_quality = quality_groups(&unit_samples, &by_unit, &unit_rows);
    assert_eq!(
        grouped_quality["px"]["metrics"]["measurementMae"]["mean"],
        json!(2.0)
    );
    let absent = quality::score(
        &RunView {
            events: &[],
            outputs: None,
        },
        Some(&truth),
        "failed",
    );
    assert_eq!(absent["metrics"]["defectFn"], json!(2));
    assert!(absent["metrics"]["defectFp"].is_null());
    assert_eq!(
        view.resolve(&metric("outputs.stations.width.mean")),
        Some(3.0)
    );
    assert_eq!(
        view.resolve(&metric("outputs.stations.width.missing")),
        Some(1.0)
    );
    assert!((view.resolve(&metric("outputs.stations.width.p95")).unwrap() - 3.9).abs() < 1e-9);
    let spatial = json!({"breaks":{"output":"breaks","coordinate":"image_px","lineOutput":"line",
        "maxProjectionDistance":2,"intervals":[{"start":[120,200],"end":[140,200]}]}});
    quality::validate(&spatial).unwrap();
    let image_outputs = json!({"line":{"value":{"data":{"points":[[100,200],[150,200]],"s":[0,50]}}},
        "breaks":{"value":{"data":{"pathOk":true,"breaks":[{"sStart":20,"sEnd":40}]}}}});
    let image_view = RunView {
        events: &[],
        outputs: Some(&image_outputs),
    };
    let image_score = quality::score(&image_view, Some(&spatial), "ok");
    assert_eq!(image_score["metrics"]["defectTp"], json!(1));
    assert_eq!(image_score["metrics"]["breakEndpointMae"], json!(0.0));
    let mut far = spatial.clone();
    far["breaks"]["intervals"][0]["start"] = json!([120, 210]);
    let unavailable = quality::score(&image_view, Some(&far), "ok");
    assert_eq!(unavailable["metrics"]["breakOutputMissing"], json!(1));
    assert_eq!(unavailable["metrics"]["defectFn"], json!(1));
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
    assert!(samples[0].set[0]
        .1
        .as_str()
        .unwrap()
        .ends_with("f1/p4/f1_master.pcd"));

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
        quality: None,
    }
}

fn sample(id: &str, tags: &[(&str, &str)]) -> Sample {
    Sample {
        id: id.to_string(),
        set: Vec::new(),
        graph_params: Map::new(),
        truth: None,
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
    let g = summarize(&samples, &grouping, 0, &metric("run.durationMs"), &rows);
    let train = &g["train"];
    assert_eq!((train.n, train.ok), (5, 5));
    assert_eq!(train.mean(), Some(3.8));
    assert!(
        (train.std().unwrap() - 1.2f64.sqrt()).abs() < 1e-12,
        "{:?}",
        train.std()
    );
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
    let g2 = summarize(&samples, &both, 0, &metric("run.durationMs"), &rows);
    assert_eq!(g2["train/a"].n, 5);
    assert_eq!(g2["holdout/b"].n, 2);

    let plain = Grouping {
        holdout: None,
        group_by: None,
    };
    assert_eq!(
        summarize(&samples, &plain, 0, &metric("run.durationMs"), &rows)["all"].n,
        7
    );
}

#[test]
fn a_missing_metric_on_a_successful_run_counts_as_a_failure() {
    let samples = vec![sample("s1", &[]), sample("s2", &[])];
    let rows = vec![row(0, Some(1.0), "ok", &[]), row(1, None, "ok", &[])];
    let grouping = Grouping {
        holdout: None,
        group_by: None,
    };
    let g = summarize(&samples, &grouping, 0, &metric("run.durationMs"), &rows);
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
        graph: Vec::new(),
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
    assert_eq!(
        parse_holdout("half=b").unwrap(),
        ("half".to_string(), "b".to_string())
    );
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
    let root = std::env::temp_dir().join(format!("lyflow-eval-{tag}-{}-{n}", std::process::id()));
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
    assert!(samples[0].set[0]
        .1
        .as_str()
        .unwrap()
        .ends_with(&format!("vin_14-09-2026-03-44-38/4/{MASTER}")));
    assert_eq!(samples[0].set[1].0, "n_load.secondaryFile");
    assert!(samples[0].set[1].1.as_str().unwrap().ends_with(SLAVE));
    assert!(samples[0].tags.is_empty());
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
    let t = tree(
        "fallback",
        &[("frame-002", files), ("frame-001", files)],
        None,
    );
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
