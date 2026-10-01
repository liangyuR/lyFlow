//! cli.rs 的单元与集成测试（从 cli.rs 里搬出来：那一个文件原来四千多行，一半是测试）。

use super::test_support::*;
use super::*;

/// 每个测试一个目录：cargo 默认并行跑，共用目录会互相覆盖。
fn workspace(name: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("lyflow-cli-{name}"));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    dir
}

/// seed 参数化不是装饰：缓存是进程级的，两个测试用同一张图的话后跑的那个
/// 会拿到 skipped 而不是 done。
fn chain(dir: &Path, seed: i64) -> String {
    let doc = json!({
        "schemaVersion": 1,
        "id": "01J8XQZ4K7N3M2R5V8W1YB6TCD",
        "name": "cli",
        "nodes": [
            {"id": "g", "op": "gen.synthetic",
             "params": {"pointCount": 20000, "seed": seed}},
            {"id": "v", "op": "filter.voxel_grid",
             "params": {"leafSize": [0.02, 0.02, 0.02]}}
        ],
        "edges": [
            {"id": "e1", "from": {"node": "g", "port": "cloud"},
                         "to": {"node": "v", "port": "cloud"}}
        ]
    });
    let file = dir.join("g.lyflow.json");
    std::fs::write(&file, serde_json::to_string_pretty(&doc).unwrap()).unwrap();
    file.to_string_lossy().into_owned()
}

/// ASCII PCD，y 恒为 row。只有 x y z 三个字段。
fn write_pcd(file: &Path, n: usize, row: f32) {
    let mut text = format!(
        "# .PCD v0.7\nVERSION 0.7\nFIELDS x y z\nSIZE 4 4 4\nTYPE F F F\nCOUNT 1 1 1\n\
         WIDTH {n}\nHEIGHT 1\nVIEWPOINT 0 0 0 1 0 0 0\nPOINTS {n}\nDATA ascii\n"
    );
    for i in 0..n {
        text.push_str(&format!("{} {row} 0\n", i as f32 * 0.01));
    }
    std::fs::write(file, text).unwrap();
}

/// `--input` 自己读 PCD（为了带上 rgb）：三种 DATA 格式都要与 core 的 io.load_pcd 读出同一批点。
#[test]
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
fn pcd_reader_agrees_with_io_load_pcd_on_all_three_formats() {
    let dir = workspace("pcdformats");
    let core = core().unwrap();
    for format in ["ascii", "binary", "binary_compressed"] {
        let file = dir.join(format!("c_{format}.pcd"));
        let doc = json!({
            "schemaVersion": 1, "id": "01J8XQZ4K7N3M2R5V8W1YB6TCF", "name": "save",
            "nodes": [
                {"id": "g", "op": "gen.synthetic", "params": {"pointCount": 777, "seed": 11}},
                {"id": "s", "op": "io.save_pcd",
                 "params": {"path": file.to_string_lossy(), "format": format}}
            ],
            "edges": [{"id": "e", "from": {"node": "g", "port": "cloud"}, "to": {"node": "s", "port": "cloud"}}]
        });
        let graph = dir.join(format!("save_{format}.lyflow.json"));
        std::fs::write(&graph, doc.to_string()).unwrap();
        let r = cli(&["run", &graph.to_string_lossy(), "--no-cache"]);
        assert_eq!(r.code, EXIT_OK, "{format}: {}", r.err);

        let ours = crate::pcd::read_pcd(&file).unwrap();
        let view = read_cloud_file(&core, &file, 0).unwrap();
        assert_eq!(ours.xyz.len(), 777 * 3, "{format}");
        assert_eq!(ours.xyz.as_slice(), view.xyz(), "{format}: xyz");
        if view.has_intensity() {
            assert_eq!(ours.intensity.as_slice(), view.intensity(), "{format}: intensity");
        }
    }
}

#[test]
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
fn input_injects_a_cloud_into_an_input_port() {
    let dir = workspace("input");
    let a = dir.join("a.pcd");
    let b = dir.join("b.pcd");
    write_pcd(&a, 3, 0.0);
    write_pcd(&b, 2, 1.0);
    let doc = json!({
        "schemaVersion": 1,
        "id": "01J8XQZ4K7N3M2R5V8W1YB6TCE",
        "name": "input",
        "nodes": [{"id": "m", "op": "util.merge"}],
        "edges": [],
        "outputs": {"merged": {"node": "m", "port": "cloud"}}
    });
    let file = dir.join("m.lyflow.json");
    std::fs::write(&file, doc.to_string()).unwrap();
    let graph = file.to_string_lossy().into_owned();

    // 没有 --input：两个必填输入都没接，校验期就拦下
    assert_eq!(cli(&["run", &graph, "--no-cache"]).code, EXIT_INVALID);

    let fa = format!("m.a={}", a.to_string_lossy());
    let fb = format!("m.b={}", b.to_string_lossy());
    let r = cli(&["run", &graph, "--input", &fa, "--input", &fb, "--outputs", "--no-cache"]);
    assert_eq!(r.code, EXIT_OK, "{}\n{}", r.err, r.out);
    let lines = r.lines();
    let done = lines
        .iter()
        .find(|e| e["kind"] == "node_state" && e["nodeId"] == "m" && e["state"] == "done")
        .expect("m 没有 done");
    // 输入注入：compute 真的跑了（不是 provided）
    assert!(done["stats"].get("provided").is_none(), "{done}");
    let outputs = lines.iter().find(|e| e.get("merged").is_some()).expect("没有 --outputs 那一行");
    assert_eq!(outputs["merged"]["elementCount"], 5);

    // 端口名写错：执行期校验报 unknown_port，退出码 2；写法不对是参数错
    let wrong = format!("m.nope={}", a.to_string_lossy());
    let r = cli(&["run", &graph, "--input", &fa, "--input", &fb, "--input", &wrong, "--no-cache"]);
    assert_eq!(r.code, EXIT_FAILED, "{}", r.out);
    assert!(r.out.contains("unknown_port"), "{}", r.out);
    assert_eq!(cli(&["run", &graph, "--input", "m.a"]).code, EXIT_USAGE);
    let missing = format!("m.a={}", dir.join("nope.pcd").to_string_lossy());
    assert_eq!(cli(&["run", &graph, "--input", &missing]).code, EXIT_USAGE);
}

#[test]
fn manifest_check_is_clean() {
    let r = cli(&["manifest", "--check"]);
    assert_eq!(r.code, EXIT_OK, "{}", r.err);
    assert_eq!(r.first()["problems"].as_array().unwrap().len(), 0);
}

#[test]
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
fn validate_accepts_a_good_graph_and_rejects_a_bad_one() {
    let dir = workspace("validate");
    let good = chain(&dir, 301);
    let r = cli(&["validate", &good]);
    assert_eq!(r.code, EXIT_OK, "{}", r.err);
    assert_eq!(r.first(), json!([]));

    let bad = cli(&["validate", &good, "--set", "v.leafSize=[0,0.01,0.01]"]);
    assert_eq!(bad.code, EXIT_INVALID);
    let diags = bad.first();
    assert_eq!(diags[0]["code"], "bad_param");
    assert_eq!(diags[0]["paramPath"], "leafSize");
}

/// `plan` 每个节点一行：cacheKey 与 level；外加惰性标记（m6-plan §5 / H9）。这张链上一个
/// 惰性端口都没有，所以 lazy 全是 false、demandedBy 全空 —— 「默认什么都不标」是它该有的样子。
#[test]
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
fn plan_marks_lazy_nodes_and_who_demands_them() {
    let dir = workspace("plan-lazy");
    let graph = chain(&dir, 312);
    let r = cli(&["plan", &graph]);
    assert_eq!(r.code, EXIT_OK, "{}", r.err);
    let nodes = r.first();
    assert_eq!(nodes[0]["nodeId"], "g");
    assert_eq!(nodes[0]["cacheKey"].as_str().unwrap().len(), 32);
    assert_eq!(nodes[1]["level"], 1);
    for n in nodes.as_array().unwrap() {
        assert_eq!(n["lazy"], false, "{n}");
        assert_eq!(n["demandedBy"], json!([]), "{n}");
    }
}

/// `lyflow params`（m6-plan §2 / H6）：生效值来自 core，来源分得开
/// 「图里写了」与「合进来的默认值」。
#[test]
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
fn params_joins_defaults_and_marks_the_source() {
    let dir = workspace("params");
    let graph = chain(&dir, 313);
    let r = cli(&["params", &graph, "--json"]);
    assert_eq!(r.code, EXIT_OK, "{}", r.err);
    let rows = r.lines();
    assert!(!rows.is_empty(), "{}", r.out);

    let find = |node: &str, param: &str| {
        rows.iter()
            .find(|x| x["node"] == node && x["param"] == param)
            .unwrap_or_else(|| panic!("没有 {node}.{param}：{}", r.out))
            .clone()
    };
    // 图里写了的那三个
    let seed = find("g", "seed");
    assert_eq!(seed["source"], "explicit");
    assert_eq!(seed["value"], json!(313));
    assert_eq!(find("v", "leafSize")["source"], "explicit");
    assert_eq!(find("v", "leafSize")["value"], json!([0.02, 0.02, 0.02]));
    // 图里没写的：值必须与 manifest 的默认值逐字相同
    let defaults = defaults_by_op(&core().unwrap()).unwrap();
    for (node, op) in [("g", "gen.synthetic"), ("v", "filter.voxel_grid")] {
        for (name, def) in &defaults[op] {
            let row = find(node, name);
            if row["source"] == "default" {
                assert_eq!(&row["value"], def, "{node}.{name} 的默认值对不上");
            }
        }
    }
    assert!(rows.iter().any(|x| x["source"] == "default"), "{}", r.out);
    assert!(rows.iter().all(|x| x["source"] != "bound"), "这张图没有子图");

    // --set 先应用再解析：问的是「这组 --set 之后生效值是什么」
    let after = cli(&["params", &graph, "--json", "--set", "g.seed=999"]);
    assert_eq!(after.code, EXIT_OK, "{}", after.err);
    let seed = after
        .lines()
        .into_iter()
        .find(|x| x["node"] == "g" && x["param"] == "seed")
        .unwrap();
    assert_eq!(seed["value"], json!(999));
    assert_eq!(seed["source"], "explicit");
}

#[test]
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
fn params_filters_by_node_and_by_source() {
    let dir = workspace("params-filter");
    let graph = chain(&dir, 314);
    let one = cli(&["params", &graph, "--json", "--node", "v"]);
    assert_eq!(one.code, EXIT_OK, "{}", one.err);
    assert!(one.lines().iter().all(|x| x["node"] == "v"), "{}", one.out);

    let explicit = cli(&["params", &graph, "--json", "--only", "explicit"]);
    assert_eq!(explicit.code, EXIT_OK, "{}", explicit.err);
    let names: Vec<String> = explicit
        .lines()
        .iter()
        .map(|x| format!("{}.{}", x["node"].as_str().unwrap(), x["param"].as_str().unwrap()))
        .collect();
    assert_eq!(names, vec!["g.pointCount", "g.seed", "v.leafSize"]);

    // 人读的那一份：表头 + 每个参数一行
    let human = cli(&["params", &graph, "--node", "v", "--only", "explicit"]);
    assert_eq!(human.code, EXIT_OK, "{}", human.err);
    assert!(human.out.contains("leafSize"), "{}", human.out);
    assert!(human.out.contains("explicit"), "{}", human.out);
}

/// 未知节点 / 未知参数按 unknown_node / unknown_param 报，退出码 4（用法错），
/// 而不是与「图本身不合法」混成同一个 1。
#[test]
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
fn params_rejects_unknown_nodes_and_params_with_the_usage_code() {
    let dir = workspace("params-unknown");
    let graph = chain(&dir, 315);

    let node = cli(&["params", &graph, "--node", "nope"]);
    assert_eq!(node.code, EXIT_USAGE, "{}", node.err);
    assert!(node.err.contains("unknown_node"), "{}", node.err);

    let set_node = cli(&["params", &graph, "--set", "nope.seed=1"]);
    assert_eq!(set_node.code, EXIT_USAGE, "{}", set_node.err);
    assert!(set_node.err.contains("unknown_node"), "{}", set_node.err);

    let param = cli(&["params", &graph, "--set", "g.nope=1"]);
    assert_eq!(param.code, EXIT_USAGE, "{}", param.err);
    assert_eq!(param.first()[0]["code"], "unknown_param");

    assert_eq!(cli(&["params", &graph, "--only", "nope"]).code, EXIT_USAGE);
    assert_eq!(cli(&["params"]).code, EXIT_USAGE);
}

#[test]
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
fn run_streams_execution_events_as_json_lines() {
    let dir = workspace("run");
    let graph = chain(&dir, 303);
    let r = cli(&["run", &graph, "--no-cache"]);
    assert_eq!(r.code, EXIT_OK, "{}", r.err);

    let lines = r.lines();
    assert_eq!(lines.first().unwrap()["kind"], "run_started");
    // 没给 --summary：末尾仍是 run_finished，不多出 run_summary 那一行；
    // 事件里那份 summary 照旧有 —— --summary 只管末尾那一行
    assert_eq!(lines.last().unwrap()["kind"], "run_finished");
    assert_eq!(lines.last().unwrap()["status"], "ok");
    assert!(lines.last().unwrap()["summary"].is_object());
    assert!(lines.iter().all(|l| l["kind"] != "run_summary"));
    // seq 连续：CI 消费的和前端消费的是同一条流（F7）
    for (i, e) in lines.iter().enumerate() {
        assert_eq!(e["seq"], i as i64, "seq 不连续：{e}");
        assert_eq!(e["schemaVersion"], 1);
    }
    let done = lines
        .iter()
        .find(|e| e["kind"] == "node_state" && e["nodeId"] == "v" && e["state"] == "done")
        .expect("v 没跑完");
    assert!(done["stats"]["elementCount"].as_i64().unwrap() > 0);
}

/// 改源头点数的两条路：`--set` 覆盖参数、`--preview-points` 在预览里降采样源头。
#[test]
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
fn set_and_preview_change_the_source_point_count() {
    let dir = workspace("set-preview");
    let graph = chain(&dir, 304);
    for (extra, want) in [
        (&["--set", "g.pointCount=1234"][..], 1234),
        (&["--preview", "--preview-points", "2000"][..], 2000),
    ] {
        let r = cli(&[&["run", graph.as_str(), "--no-cache"][..], extra].concat());
        assert_eq!(r.code, EXIT_OK, "{extra:?}: {}", r.err);
        let done = r
            .lines()
            .into_iter()
            .find(|e| e["kind"] == "node_state" && e["nodeId"] == "g" && e["state"] == "done")
            .unwrap_or_else(|| panic!("{extra:?}: g 没有 done"));
        assert_eq!(done["stats"]["elementCount"], want, "{extra:?}");
    }
}

#[test]
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
fn run_to_node_prunes_the_downstream() {
    let dir = workspace("runto");
    let graph = chain(&dir, 305);
    let r = cli(&["run", &graph, "--to", "g"]);
    assert_eq!(r.code, EXIT_OK, "{}", r.err);
    let started = r.first();
    let plan: Vec<&str> = started["plan"]
        .as_array()
        .unwrap()
        .iter()
        .map(|v| v.as_str().unwrap())
        .collect();
    assert_eq!(plan, vec!["g"]);
}

#[test]
fn run_exits_one_on_validation_failure() {
    let dir = workspace("badrun");
    let graph = chain(&dir, 306);
    let r = cli(&["run", &graph, "--set", "v.leafSize=[0,0,0]"]);
    assert_eq!(r.code, EXIT_INVALID, "{}", r.err);
    // 校验失败时输出的是诊断而不是事件
    assert_eq!(r.first()["kind"], "diagnostic");
}

#[test]
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
fn run_exits_two_when_a_node_fails() {
    let dir = workspace("failrun");
    let doc = json!({
        "schemaVersion": 1, "id": "01J8XQZ4K7N3M2R5V8W1YB6TCE",
        "nodes": [{"id": "r", "op": "io.load_pcd", "params": {"path": "没有这个文件.pcd"}}],
        "edges": []
    });
    let file = dir.join("bad.lyflow.json");
    std::fs::write(&file, doc.to_string()).unwrap();
    let r = cli(&["run", &file.to_string_lossy()]);
    assert_eq!(r.code, EXIT_FAILED, "{} / {}", r.out, r.err);
    assert_eq!(r.lines().last().unwrap()["status"], "error");
}

/// ADR-0022：带 fallback 的图里，主路径炸了但结果量出来了 —— run_finished 是
/// ok，summary 必须说 degraded，并把那次回退记进 decisions。
fn fallback_graph(dir: &Path, seed: i64, outputs: Value) -> String {
    let doc = json!({
        "schemaVersion": 1, "id": "01J8XQZ4K7N3M2R5V8W1YB6TCF",
        "nodes": [
            {"id": "n_bad", "op": "io.load_pcd", "params": {"path": "没有这个文件.pcd"}},
            {"id": "n_b", "op": "gen.synthetic",
             "params": {"pointCount": 100, "seed": seed}},
            {"id": "n_fb", "op": "flow.fallback"}
        ],
        "edges": [
            {"id": "e1", "from": {"node": "n_bad", "port": "cloud"},
                         "to": {"node": "n_fb", "port": "a"}},
            {"id": "e2", "from": {"node": "n_b", "port": "cloud"},
                         "to": {"node": "n_fb", "port": "b"}}
        ],
        "outputs": outputs
    });
    let file = dir.join(format!("fb{seed}.lyflow.json"));
    std::fs::write(&file, doc.to_string()).unwrap();
    file.to_string_lossy().into_owned()
}

#[test]
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
fn run_summary_says_degraded_when_a_fallback_saved_the_run() {
    let dir = workspace("summary-degraded");
    let graph = fallback_graph(
        &dir,
        3401,
        json!({ "result": {"node": "n_fb", "port": "out"} }),
    );
    let r = cli(&["run", &graph, "--summary", "--no-cache"]);
    assert_eq!(r.code, EXIT_OK, "{} / {}", r.out, r.err);

    let lines = r.lines();
    let last = lines.last().unwrap();
    assert_eq!(last["kind"], "run_summary");
    // run_finished 是 ok（失败被 acceptsError 接住了），summary 却是 degraded
    let finished = lines.iter().find(|e| e["kind"] == "run_finished").unwrap();
    assert_eq!(finished["status"], "ok");
    assert_eq!(last["status"], "degraded", "{last}");
    // 事件里那份与末尾这行是同一个对象（H1）
    let mut from_event = finished["summary"].clone();
    from_event["kind"] = json!("run_summary");
    assert_eq!(&from_event, last);

    assert_eq!(last["nodes"]["n_bad"]["state"], "error");
    assert_eq!(last["outputs"]["result"]["state"], "value");
    assert_eq!(last["outputs"]["result"]["node"], "n_fb");
    assert_eq!(last["outputs"]["result"]["elementCount"], 100);
    assert_eq!(last["decisions"]["n_fb"]["choice"], "b");
    assert_eq!(last["decisions"]["n_fb"]["type"], "FallbackChoice");
    assert!(last["contractViolations"].as_array().unwrap().is_empty());
}

/// `lyflow run` 的进度行：数 plan 里的节点，算完一个（不管成没成）加一，正在算的按开跑先后列出来。
/// 开关只在 main 里设，这里只看计数与那一行的字（eval::Progress 的原地刷新在 eval.rs 里测）。
#[test]
fn the_run_progress_line_counts_nodes_and_names_the_running_ones() {
    let err = sink_of(std::io::sink());
    let mut p = RunProgress::new(&err);
    p.event(&json!({"kind": "run_started", "plan": ["a", "b", "c"]}));
    p.event(&json!({"kind": "node_state", "nodeId": "a", "state": "running"}));
    p.event(&json!({"kind": "node_state", "nodeId": "b", "state": "running"}));
    p.event(&json!({"kind": "node_state", "nodeId": "a", "state": "done"}));
    p.event(&json!({"kind": "node_state", "nodeId": "c", "state": "pending"}));
    assert!(p.text().starts_with("[1/3] 节点 · 正在算 b · 已过 "), "{}", p.text());
    p.event(&json!({"kind": "node_state", "nodeId": "b", "state": "error"}));
    p.event(&json!({"kind": "node_state", "nodeId": "c", "state": "skipped"}));
    assert!(p.text().starts_with("[3/3] 节点 · 已过 "), "{}", p.text());
}

/// 写法错一张表：参数、退出码、stderr 里该有的字。都在任何节点开跑之前拦下，
/// 不依赖标准包 —— 纯平台构建里照样跑。
#[test]
fn wrong_usage_is_rejected_with_its_exit_code_and_message() {
    let dir = workspace("usage");
    let graph = chain(&dir, 309);
    // 纯平台构建里也成立的两张图：一个不存在的算子、一张空图
    let unknown = dir.join("unknown-op.lyflow.json");
    std::fs::write(&unknown, r#"{"schemaVersion":1,"id":"u","nodes":[{"id":"a","op":"nope.op"}],"edges":[]}"#).unwrap();
    let unknown = unknown.to_string_lossy().into_owned();
    let empty = dir.join("empty.lyflow.json");
    std::fs::write(&empty, r#"{"schemaVersion":1,"id":"e","nodes":[],"edges":[]}"#).unwrap();
    let empty = empty.to_string_lossy().into_owned();
    let crop = crop_chain(&dir, 3403, 17007);
    let scene = samples_file(&dir, "scene.jsonl", &[r#"{"id":"a","scene":"sc_1"}"#]);
    let perturb = |after: &'static str, region: &'static str| {
        vec!["perturb", crop.as_str(), "--after", after, "--region", region, "--axis", "x=0:1:2",
             "--metric", "nodes.c.elementCount"]
    };
    let cases: Vec<(Vec<&str>, i32, &str)> = vec![
        (vec![], EXIT_USAGE, "lyflow run"),
        (vec!["nope"], EXIT_USAGE, "不认识的子命令 nope"),
        (vec!["run", "x.json", "--nope"], EXIT_USAGE, "不认识的选项 --nope"),
        (vec!["sweep", &graph, "--param", "v.x=1:2", "--metric", "v:cloud.elementCount"], EXIT_USAGE,
         "start:end:steps"),
        // 节点不存在是这张图套不上这条 --set，按「图不合法」报 1，不是写法错的 4
        (vec!["run", &graph, "--set", "nope.x=1"], EXIT_INVALID, "没有节点"),
        (vec!["eval", &graph, "--samples", &scene, "--metric", "nodes.v.elementCount"], EXIT_USAGE, "scene"),
        (perturb("sub/g:cloud", HALFSPACE), EXIT_USAGE, "子图"),
        (perturb("g:cloud", r#"{"kind":"sphere"}"#), EXIT_USAGE, "kind"),
        (perturb("nope:cloud", HALFSPACE), EXIT_USAGE, "没有节点"),
        (vec!["patch", &graph], EXIT_USAGE, "至少给一个动作"),
        (vec!["patch"], EXIT_USAGE, "用法：lyflow patch"),
        // 数字选项写错：以前悄悄当成默认值
        (vec!["run", &graph, "--parallel", "4x"], EXIT_USAGE, "--parallel 要一个非负整数，收到 4x"),
        (vec!["run", &graph, "--preview-points", "-5"], EXIT_USAGE, "--preview-points"),
        (vec!["eval", &graph, "--metric", "run.durationMs", "--parallel", "abc"], EXIT_USAGE, "--parallel"),
        (vec!["eval", &graph, "--metric", "run.durationMs", "--jobs", "0"], EXIT_USAGE, "--jobs"),
        (vec!["sweep", &graph, "--param", "v.minPointsPerVoxel=1:2:2", "--metric", "v:cloud.elementCount",
              "--jobs", "two"], EXIT_USAGE, "--jobs"),
        (perturb("g:cloud", HALFSPACE).into_iter().chain(["--jobs", "-1"]).collect(), EXIT_USAGE, "--jobs"),
        // 失败的原因也写在 stderr 上：诊断在 stdout 的 JSON 行里，stdout 常被重定向进文件
        (vec!["run", &unknown], EXIT_INVALID, "a：当前 core 没有注册算子 'nope.op'（unknown_op）"),
        (vec!["validate", &unknown], EXIT_INVALID, "1 条错误：\n  a：当前 core 没有注册算子"),
        (vec!["run", &empty, "--to", "zzz"], EXIT_FAILED, "目标不存在: zzz（unknown_node）"),
        (vec!["dump", &graph, "v:cloud", "out.pcd", "--format", "asci"], EXIT_USAGE, "--format 只认 binary / ascii / binary_compressed，收到 asci"),
    ];
    for (args, code, want) in &cases {
        let r = cli(args);
        assert_eq!(r.code, *code, "{args:?}: {}", r.err);
        assert!(r.err.contains(want), "{args:?} 的 stderr 缺「{want}」：{}", r.err);
    }
}

#[test]
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
fn dump_writes_the_output_to_disk() {
    let dir = workspace("dump");
    let graph = chain(&dir, 307);
    let target = dir.join("out.pcd");
    let r = cli(&["dump", &graph, "v:cloud", &target.to_string_lossy()]);
    assert_eq!(r.code, EXIT_OK, "{}", r.err);
    assert!(target.exists(), "没写出 {}", target.display());
    assert!(std::fs::metadata(&target).unwrap().len() > 1000);
    let last = r.lines().last().cloned().unwrap();
    assert_eq!(last["kind"], "dump_written");
    assert!(last["elementCount"].as_f64().unwrap() > 0.0);
}

/// m4-plan §3 的验收原话是「sweep 5 组 leafSize」——而 leafSize 是 vec3f。
/// 一个数要能广播到三个分量，否则那条验收根本写不出来。
#[test]
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
fn sweep_broadcasts_a_scalar_onto_a_vector_param() {
    let dir = workspace("sweepvec");
    let graph = chain(&dir, 315);
    let csv = dir.join("out.csv");
    let r = cli(&[
        "sweep",
        &graph,
        "--param",
        "v.leafSize=0.01:0.05:5",
        "--metric",
        "v:cloud.elementCount",
        "--csv",
        &csv.to_string_lossy(),
    ]);
    assert_eq!(r.code, EXIT_OK, "{}", r.err);
    let rows = r.lines();
    assert_eq!(rows.len(), 5);
    for row in &rows {
        assert_eq!(row["status"], "ok", "{row}");
        assert!(row["value"].as_f64().unwrap() > 0.0, "{row}");
    }
    // leafSize 越大点越少
    assert!(rows[4]["value"].as_f64().unwrap() < rows[0]["value"].as_f64().unwrap());
    // 源头只算一次
    let reused = rows
        .iter()
        .filter(|r| r["skipped"].as_array().unwrap().iter().any(|v| v == "g"))
        .count();
    assert_eq!(reused, 4, "{}", r.out);

    let text = std::fs::read_to_string(&csv).unwrap();
    assert!(text.starts_with("v.leafSize,v:cloud.elementCount"), "{text}");
    assert_eq!(text.lines().count(), 6);
}

/// §3 验收：只移动了节点的两份图，diff 输出为空。
#[test]
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
fn diff_ignores_ui_and_catches_params() {
    let dir = workspace("diff");
    let graph = chain(&dir, 310);
    let mut doc: Value = serde_json::from_str(&std::fs::read_to_string(&graph).unwrap()).unwrap();
    for node in doc["nodes"].as_array_mut().unwrap() {
        node["ui"] = json!({"position": {"x": 999, "y": 42}, "title": "挪过的"});
    }
    let moved = dir.join("moved.lyflow.json");
    std::fs::write(&moved, doc.to_string()).unwrap();

    let r = cli(&["diff", &graph, &moved.to_string_lossy(), "--json"]);
    assert_eq!(r.code, EXIT_OK, "{}", r.err);
    assert_eq!(r.first()["empty"], true, "{}", r.out);

    doc["nodes"][1]["params"]["leafSize"] = json!([0.05, 0.05, 0.05]);
    doc["nodes"].as_array_mut().unwrap().push(json!({
        "id": "p", "op": "filter.passthrough"
    }));
    let changed = dir.join("changed.lyflow.json");
    std::fs::write(&changed, doc.to_string()).unwrap();

    let r = cli(&["diff", &graph, &changed.to_string_lossy(), "--json"]);
    let d = r.first();
    assert_eq!(d["empty"], false);
    assert_eq!(d["nodesAdded"][0]["id"], "p");
    assert_eq!(d["nodesChanged"][0]["id"], "v");
    assert_eq!(d["nodesChanged"][0]["params"]["leafSize"]["to"], json!([0.05, 0.05, 0.05]));
}

/// 稀疏存储：写一个等于默认值的参数不该被 diff 当成变化。
#[test]
fn diff_merges_defaults_before_comparing() {
    let dir = workspace("diffdefault");
    let graph = chain(&dir, 311);
    let mut doc: Value = serde_json::from_str(&std::fs::read_to_string(&graph).unwrap()).unwrap();
    // 默认值从 manifest 现取：写死一个数字会在算子调默认值那天变成假绿
    let manifest = cli(&["manifest"]).first();
    let default_noise = manifest["operators"]
        .as_array()
        .unwrap()
        .iter()
        .find(|op| op["id"] == "gen.synthetic")
        .and_then(|op| op["params"].as_array())
        .and_then(|ps| ps.iter().find(|p| p["name"] == "noise"))
        .map(|p| p["default"].clone())
        .expect("manifest 里没有 gen.synthetic.noise");
    doc["nodes"][0]["params"]["noise"] = default_noise;
    let same = dir.join("same.lyflow.json");
    std::fs::write(&same, doc.to_string()).unwrap();
    let r = cli(&["diff", &graph, &same.to_string_lossy(), "--json"]);
    assert_eq!(r.first()["empty"], true, "{}", r.out);
}

/// ADR-0008：迁移只是诊断；--write 才落盘，落完再跑一次就没有迁移了。
#[test]
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
fn migrate_reports_and_optionally_writes() {
    let dir = workspace("migrate");
    let doc = json!({
        "schemaVersion": 1, "id": "01J8XQZ4K7N3M2R5V8W1YB6TCF",
        "nodes": [
            {"id": "g", "op": "gen.synthetic", "opVersion": "1.0.0",
             "params": {"pointCount": 1000}},
            {"id": "s", "op": "filter.random_sample", "opVersion": "1.0.0",
             "params": {"count": 250, "seed": 3}}
        ],
        "edges": [
            {"id": "e", "from": {"node": "g", "port": "cloud"},
                        "to": {"node": "s", "port": "cloud"}}
        ]
    });
    let file = dir.join("old.lyflow.json");
    std::fs::write(&file, serde_json::to_string_pretty(&doc).unwrap()).unwrap();
    let path = file.to_string_lossy().into_owned();

    let r = cli(&["migrate", &path]);
    assert_eq!(r.code, EXIT_OK, "{}", r.err);
    let report = r.first();
    assert_eq!(report["migrations"].as_array().unwrap().len(), 1);
    assert_eq!(report["written"], Value::Null);
    // 没有 --write 就一个字节都不许改
    assert!(std::fs::read_to_string(&file).unwrap().contains("\"count\""));

    let w = cli(&["migrate", &path, "--write"]);
    assert_eq!(w.code, EXIT_OK, "{}", w.err);
    assert!(w.first()["written"].is_string());
    let text = std::fs::read_to_string(&file).unwrap();
    assert!(text.contains("keepCount"), "{text}");
    assert!(text.ends_with('\n'));

    let again = cli(&["migrate", &path]);
    assert_eq!(again.first()["migrations"].as_array().unwrap().len(), 0);
}

/// F1：子图在 compile 前展开，CLI 看见的事件里只有路径式 id。
#[test]
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
fn subgraph_expands_into_path_ids() {
    let dir = workspace("subgraph");
    let doc = json!({
        "schemaVersion": 1, "id": "01J8XQZ4K7N3M2R5V8W1YB6TCG",
        "nodes": [
            {"id": "g", "op": "gen.synthetic", "params": {"pointCount": 20000, "seed": 312}},
            {"id": "s", "op": "sub:clean", "params": {"leaf": [0.03, 0.03, 0.03]}}
        ],
        "edges": [
            {"id": "e1", "from": {"node": "g", "port": "cloud"},
                         "to": {"node": "s", "port": "cloud"}}
        ],
        "subgraphs": {
            "clean": {
                "name": "去噪",
                "nodes": [{"id": "v", "op": "filter.voxel_grid"}],
                "edges": [],
                "inputs": [{"name": "cloud", "type": "PointCloud",
                            "to": [{"node": "v", "port": "cloud"}]}],
                "outputs": [{"name": "cloud", "type": "PointCloud",
                             "from": {"node": "v", "port": "cloud"}}],
                "params": [{"name": "leaf", "type": "vec3f",
                            "default": [0.02, 0.02, 0.02],
                            "binds": [{"node": "v", "param": "leafSize"}]}]
            }
        }
    });
    let file = dir.join("sub.lyflow.json");
    std::fs::write(&file, doc.to_string()).unwrap();
    let r = cli(&["run", &file.to_string_lossy(), "--no-cache"]);
    assert_eq!(r.code, EXIT_OK, "{} / {}", r.out, r.err);
    let plan: Vec<String> = r.first()["plan"]
        .as_array()
        .unwrap()
        .iter()
        .map(|v| v.as_str().unwrap().to_string())
        .collect();
    assert_eq!(plan, vec!["g".to_string(), "s/v".to_string()]);
}

fn samples_file(dir: &Path, name: &str, lines: &[&str]) -> String {
    let file = dir.join(name);
    std::fs::write(&file, format!("{}\n", lines.join("\n"))).unwrap();
    file.to_string_lossy().into_owned()
}

#[test]
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
fn eval_reports_rows_and_per_group_statistics() {
    let dir = workspace("eval");
    let graph = chain(&dir, 320);
    let samples = samples_file(
        &dir,
        "s.jsonl",
        &[
            r#"{"id":"a","set":{"g.seed":3201},"tags":{"half":"a"}}"#,
            r#"{"id":"b","set":{"g.seed":3202},"tags":{"half":"a"}}"#,
            r#"{"id":"c","set":{"g.seed":3203},"tags":{"half":"b"}}"#,
        ],
    );
    let csv = dir.join("eval.csv");
    let r = cli(&[
        "eval",
        &graph,
        "--samples",
        &samples,
        "--metric",
        "nodes.v.elementCount",
        "--metric",
        "run.durationMs",
        "--holdout",
        "half=b",
        "--csv",
        &csv.to_string_lossy(),
    ]);
    assert_eq!(r.code, EXIT_OK, "{}", r.err);
    let lines = r.lines();
    let rows: Vec<&Value> = lines.iter().filter(|l| l["kind"] == "eval_row").collect();
    let summaries: Vec<&Value> = lines.iter().filter(|l| l["kind"] == "eval_summary").collect();
    assert_eq!(rows.len(), 3);
    assert_eq!(summaries.len(), 2, "每个 metric 一行 summary");

    assert_eq!(rows[0]["sample"], "a");
    assert_eq!(rows[0]["status"], "ok");
    assert_eq!(rows[0]["holdout"], false);
    assert_eq!(rows[0]["tags"]["half"], "a");
    assert!(rows[0]["metrics"]["nodes.v.elementCount"].as_f64().unwrap() > 0.0);
    assert_eq!(rows[2]["holdout"], true);

    let first = summaries[0];
    assert_eq!(first["metric"], "nodes.v.elementCount");
    assert_eq!(first["groups"]["train"]["n"], 2);
    assert_eq!(first["groups"]["train"]["ok"], 2);
    assert_eq!(first["groups"]["holdout"]["n"], 1);
    assert!(first["groups"]["holdout"]["std"].is_null(), "n<2 时 std 是 null");
    assert!(first["groups"]["train"]["std"].as_f64().unwrap() >= 0.0);

    let text = std::fs::read_to_string(&csv).unwrap();
    assert!(
        text.starts_with("paramSet,sample,holdout,status,nodes.v.elementCount,run.durationMs"),
        "{text}"
    );
    assert_eq!(text.lines().count(), 4);
}

/// ADR-0022：`--summary` 时每个 eval_row 带这一次运行的结论。
/// **默认关**（m6-plan §10 第 5 条）：体积是逐行的，一维 bundle 就 6 KB。
#[test]
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
fn eval_rows_carry_the_run_summary_only_when_asked() {
    let dir = workspace("eval-summary");
    let graph = chain(&dir, 3410);
    let samples = samples_file(
        &dir,
        "s.jsonl",
        &[
            r#"{"id":"a","set":{"g.seed":34101}}"#,
            r#"{"id":"b","set":{"g.seed":34102}}"#,
        ],
    );
    let args = |extra: &[&str]| {
        let mut v = vec![
            "eval".to_string(),
            graph.clone(),
            "--samples".to_string(),
            samples.clone(),
            "--metric".to_string(),
            "nodes.v.elementCount".to_string(),
        ];
        v.extend(extra.iter().map(|s| (*s).to_string()));
        v
    };

    let r = cli_owned(&args(&["--summary"]));
    assert_eq!(r.code, EXIT_OK, "{}", r.err);
    let rows: Vec<Value> = r
        .lines()
        .into_iter()
        .filter(|l| l["kind"] == "eval_row")
        .collect();
    assert_eq!(rows.len(), 2);
    for row in &rows {
        assert_eq!(row["summary"]["status"], "ok", "{row}");
        assert_eq!(row["summary"]["nodes"]["v"]["state"], "done");
        assert!(row["summary"]["contractViolations"].is_array());
    }

    for extra in [&[][..], &["--no-summary"][..]] {
        let off = cli_owned(&args(extra));
        assert_eq!(off.code, EXIT_OK, "{}", off.err);
        let rows: Vec<Value> = off
            .lines()
            .into_iter()
            .filter(|l| l["kind"] == "eval_row")
            .collect();
        assert_eq!(rows.len(), 2);
        for row in &rows {
            assert!(row["summary"].is_null(), "默认不该带 summary（{extra:?}）: {row}");
        }
    }
}

#[test]
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
fn eval_groups_by_a_tag_key() {
    let dir = workspace("evalgroup");
    let graph = chain(&dir, 321);
    let samples = samples_file(
        &dir,
        "s.jsonl",
        &[
            r#"{"id":"a","set":{"g.seed":3211},"tags":{"lot":"x"}}"#,
            r#"{"id":"b","set":{"g.seed":3212},"tags":{"lot":"y"}}"#,
            r#"{"id":"c","set":{"g.seed":3213}}"#,
        ],
    );
    let r = cli(&[
        "eval",
        &graph,
        "--samples",
        &samples,
        "--metric",
        "nodes.v.elementCount",
        "--group-by",
        "lot",
    ]);
    assert_eq!(r.code, EXIT_OK, "{}", r.err);
    let summary = r
        .lines()
        .into_iter()
        .find(|l| l["kind"] == "eval_summary")
        .unwrap();
    let groups = summary["groups"].as_object().unwrap();
    assert_eq!(groups.len(), 3, "{summary}");
    assert_eq!(groups["x"]["n"], 1);
    assert_eq!(groups["y"]["n"], 1);
    assert_eq!(groups["(none)"]["n"], 1);
}

#[test]
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
fn eval_crosses_parameter_sets_with_samples() {
    let dir = workspace("evalparam");
    let graph = chain(&dir, 322);
    let samples = samples_file(
        &dir,
        "s.jsonl",
        &[
            r#"{"id":"a","set":{"g.seed":3221}}"#,
            r#"{"id":"b","set":{"g.seed":3222}}"#,
        ],
    );
    let args = [
        "eval",
        &graph,
        "--samples",
        &samples,
        "--param",
        "v.minPointsPerVoxel=1:3:3",
        "--metric",
        "nodes.v.elementCount",
    ];
    let r = cli(&args);
    assert_eq!(r.code, EXIT_OK, "{}", r.err);
    let lines = r.lines();
    let rows: Vec<&Value> = lines.iter().filter(|l| l["kind"] == "eval_row").collect();
    assert_eq!(rows.len(), 6);
    assert_eq!(rows[0]["paramSet"], 0);
    assert_eq!(rows[0]["params"]["v.minPointsPerVoxel"], 1.0);
    assert_eq!(rows[5]["paramSet"], 2);
    assert_eq!(rows[5]["params"]["v.minPointsPerVoxel"], 3.0);
    let summaries: Vec<&Value> = lines.iter().filter(|l| l["kind"] == "eval_summary").collect();
    assert_eq!(summaries.len(), 3);
    let mean = |s: &Value| s["groups"]["all"]["mean"].as_f64().unwrap();
    assert!(mean(summaries[2]) < mean(summaries[0]));

    // --jobs：同时跑几次，输出与一次接一次时逐行相同（durationMs 除外）
    let jobs = cli(&[&args[..], &["--jobs", "4"]].concat());
    assert_eq!(jobs.code, EXIT_OK, "{}", jobs.err);
    assert_eq!(without_durations(jobs.lines()), without_durations(lines));
}

fn without_durations(lines: Vec<Value>) -> Vec<Value> {
    lines
        .into_iter()
        .map(|mut l| {
            if let Some(obj) = l.as_object_mut() {
                obj.remove("durationMs");
            }
            l
        })
        .collect()
}

/// 某个样本自己就跑不起来（set 写到了不存在的节点）：--jobs 与一次接一次停在同一行 ——
/// 排在它前面的照常交出，然后报错退出 2；排在后面、已经起了的那几次结果丢掉。
#[test]
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
fn eval_with_jobs_stops_at_the_same_row_as_without() {
    let dir = workspace("evaljobsfail");
    let graph = chain(&dir, 327);
    let samples = samples_file(
        &dir,
        "s.jsonl",
        &[
            r#"{"id":"a","set":{"g.seed":3271}}"#,
            r#"{"id":"b","set":{"g.seed":3272}}"#,
            r#"{"id":"c","set":{"nope.seed":1}}"#,
            r#"{"id":"d","set":{"g.seed":3274}}"#,
            r#"{"id":"e","set":{"g.seed":3275}}"#,
        ],
    );
    let args = ["eval", &graph, "--samples", &samples, "--metric", "nodes.v.elementCount"];
    let serial = cli(&args);
    let jobs = cli(&[&args[..], &["--jobs", "3"]].concat());
    for r in [&serial, &jobs] {
        assert_eq!(r.code, EXIT_FAILED, "{} / {}", r.out, r.err);
        assert!(r.err.contains("nope"), "{}", r.err);
        let ids: Vec<Value> = r
            .lines()
            .into_iter()
            .filter(|l| l["kind"] == "eval_row")
            .map(|l| l["sample"].clone())
            .collect();
        assert_eq!(ids, vec![json!("a"), json!("b")]);
    }
}

#[test]
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
fn eval_lists_the_available_paths_when_the_metric_is_wrong() {
    let dir = workspace("evalpath");
    let graph = chain(&dir, 323);
    let samples = samples_file(&dir, "s.jsonl", &[r#"{"id":"a","set":{"g.seed":3231}}"#]);
    let r = cli(&[
        "eval",
        &graph,
        "--samples",
        &samples,
        "--metric",
        "outputs.gap",
    ]);
    assert_eq!(r.code, EXIT_USAGE, "{}", r.out);
    assert!(r.err.contains("outputs.gap"), "{}", r.err);
    assert!(r.err.contains("nodes.v.elementCount"), "{}", r.err);
    assert!(r.err.contains("run.durationMs"), "{}", r.err);
    let bad = cli(&["eval", &graph, "--samples", &samples, "--metric", "gap"]);
    assert_eq!(bad.code, EXIT_USAGE);

    // --list-metrics：同一批路径正向给出来（一行 metric_paths、退出码 0），不用故意写错一个指标
    let listed = cli(&["eval", &graph, "--samples", &samples, "--list-metrics"]);
    assert_eq!(listed.code, EXIT_OK, "{}", listed.err);
    let first = &listed.lines()[0];
    let paths: Vec<&str> = first["paths"].as_array().unwrap().iter().filter_map(Value::as_str).collect();
    assert!(
        first["kind"] == "metric_paths"
            && first["sample"] == "a"
            && ["nodes.v.elementCount", "run.durationMs"].iter().all(|p| paths.contains(p)),
        "{}",
        listed.out
    );
}

#[test]
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
fn eval_builds_samples_from_a_glob() {
    let dir = workspace("evalglob");
    let clouds = dir.join("clouds");
    std::fs::create_dir_all(&clouds).unwrap();
    let source = chain(&dir, 325);
    for name in ["one", "two"] {
        let target = clouds.join(format!("{name}.pcd"));
        let d = cli(&["dump", &source, "v:cloud", &target.to_string_lossy()]);
        assert_eq!(d.code, EXIT_OK, "{}", d.err);
    }
    let doc = json!({
        "schemaVersion": 1, "id": "01J8XQZ4K7N3M2R5V8W1YB6TD1",
        "nodes": [{"id": "r", "op": "io.load_pcd", "params": {"path": "placeholder.pcd"}}],
        "edges": []
    });
    let file = dir.join("load.lyflow.json");
    std::fs::write(&file, doc.to_string()).unwrap();
    let pattern = format!("{}/*.pcd", clouds.to_string_lossy().replace('\\', "/"));

    let r = cli(&[
        "eval",
        &file.to_string_lossy(),
        "--samples-glob",
        &pattern,
        "--bind",
        "r.path",
        "--metric",
        "nodes.r.elementCount",
    ]);
    assert_eq!(r.code, EXIT_OK, "{} / {}", r.out, r.err);
    let lines = r.lines();
    let rows: Vec<&Value> = lines.iter().filter(|l| l["kind"] == "eval_row").collect();
    assert_eq!(rows.len(), 2);
    assert_eq!(rows[0]["sample"], "one");
    assert_eq!(rows[1]["sample"], "two");
    assert!(rows[0]["metrics"]["nodes.r.elementCount"].as_f64().unwrap() > 0.0);
    let no_bind = cli(&[
        "eval",
        &file.to_string_lossy(),
        "--samples-glob",
        &pattern,
        "--metric",
        "nodes.r.elementCount",
    ]);
    assert_eq!(no_bind.code, EXIT_USAGE);
}

#[test]
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
fn eval_separates_validation_failures_from_run_failures() {
    let dir = workspace("evalfail");
    let doc = json!({
        "schemaVersion": 1, "id": "01J8XQZ4K7N3M2R5V8W1YB6TD2",
        "nodes": [{"id": "r", "op": "io.load_pcd", "params": {"path": "有.pcd"}}],
        "edges": []
    });
    let file = dir.join("load.lyflow.json");
    std::fs::write(&file, doc.to_string()).unwrap();
    let samples = samples_file(
        &dir,
        "s.jsonl",
        &[r#"{"id":"missing","set":{"r.path":"没有这个文件.pcd"}}"#],
    );
    let r = cli(&[
        "eval",
        &file.to_string_lossy(),
        "--samples",
        &samples,
        "--metric",
        "nodes.r.elementCount",
    ]);
    assert_eq!(r.code, EXIT_FAILED, "{} / {}", r.out, r.err);
    let row = r
        .lines()
        .into_iter()
        .find(|l| l["kind"] == "eval_row")
        .unwrap();
    assert_eq!(row["status"], "failed");
    assert!(row["metrics"]["nodes.r.elementCount"].is_null());
    let summary = r
        .lines()
        .into_iter()
        .find(|l| l["kind"] == "eval_summary")
        .unwrap();
    assert_eq!(summary["groups"]["all"]["ok"], 0);
    assert!(
        summary["groups"]["all"]["failCodes"]
            .as_object()
            .unwrap()
            .values()
            .any(|v| v == 1),
        "{summary}"
    );
}

/// 不给样本就跑一次图；指标用的是 sweep 那套老写法 `v:cloud.elementCount`，eval 也得认。
#[test]
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
fn eval_without_samples_runs_the_graph_once() {
    let dir = workspace("evalnosample");
    let graph = chain(&dir, 326);
    let r = cli(&["eval", &graph, "--metric", "v:cloud.elementCount"]);
    assert_eq!(r.code, EXIT_OK, "{}", r.err);
    let rows: Vec<Value> = r
        .lines()
        .into_iter()
        .filter(|l| l["kind"] == "eval_row")
        .collect();
    assert_eq!(rows.len(), 1);
    assert_eq!(rows[0]["sample"], "-");
    assert!(rows[0]["metrics"]["v:cloud.elementCount"].as_f64().unwrap() > 0.0, "{}", rows[0]);
}


fn crop_chain(dir: &Path, seed: i64, count: i64) -> String {
    let doc = json!({
        "schemaVersion": 1,
        "id": "01J8XQZ4K7N3M2R5V8W1YB6TP1",
        "name": "perturb",
        "nodes": [
            {"id": "g", "op": "gen.synthetic",
             "params": {"pointCount": count, "seed": seed, "outlierRatio": 0.0}},
            {"id": "c", "op": "filter.crop_box",
             "params": {"min": [0.05, -10.0, -10.0], "max": [10.0, 10.0, 10.0]}}
        ],
        "edges": [
            {"id": "e1", "from": {"node": "g", "port": "cloud"},
                         "to": {"node": "c", "port": "cloud"}}
        ]
    });
    let file = dir.join("p.lyflow.json");
    std::fs::write(&file, serde_json::to_string_pretty(&doc).unwrap()).unwrap();
    file.to_string_lossy().into_owned()
}

const HALFSPACE: &str = r#"{"kind":"halfspace","point":[0,0,0],"normal":[1,0,0]}"#;

#[test]
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
fn perturb_inserts_the_node_and_reports_a_slope() {
    let dir = workspace("perturb");
    let graph = crop_chain(&dir, 3401, 17003);
    let r = cli(&[
        "perturb",
        &graph,
        "--after",
        "g:cloud",
        "--region",
        HALFSPACE,
        "--axis",
        "x=-0.04:0.04:5",
        "--metric",
        "nodes.c.elementCount",
    ]);
    assert_eq!(r.code, EXIT_OK, "{} / {}", r.out, r.err);
    let lines = r.lines();
    let rows: Vec<&Value> = lines.iter().filter(|l| l["kind"] == "perturb_row").collect();
    assert_eq!(rows.len(), 5);
    assert!((rows[0]["displacement"].as_f64().unwrap() + 0.04).abs() < 1e-12);
    assert!(rows[2]["displacement"].as_f64().unwrap().abs() < 1e-12);
    assert!((rows[4]["displacement"].as_f64().unwrap() - 0.04).abs() < 1e-12);
    assert_eq!(rows[0]["sample"], "-");

    let counts: Vec<f64> = rows
        .iter()
        .map(|r| r["metrics"]["nodes.c.elementCount"].as_f64().unwrap())
        .collect();
    assert!(counts[0] < counts[4], "{counts:?}");

    let per: Vec<&Value> = lines
        .iter()
        .filter(|l| l["kind"] == "perturb_sample")
        .collect();
    assert_eq!(per.len(), 1);
    assert_eq!(per[0]["n"], 5);
    assert!(per[0]["slope"].as_f64().unwrap() > 0.0, "{}", per[0]);
    assert!(per[0]["slopeNeg"].as_f64().unwrap() > 0.0, "{}", per[0]);
    assert!(per[0]["slopePos"].as_f64().unwrap() > 0.0, "{}", per[0]);
    assert!(per[0]["pass"].is_null(), "没给 --expect 时不判定");

    let sum = lines
        .iter()
        .find(|l| l["kind"] == "perturb_summary")
        .unwrap();
    assert_eq!(sum["samples"], 1);
    assert_eq!(sum["signFold"], 0);
    assert_eq!(sum["nonResponsive"], 0);
    assert_eq!(sum["metric"], "nodes.c.elementCount");
    assert!(r.err.contains("__perturb"), "{}", r.err);
}

#[test]
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
fn perturb_flags_a_reading_that_does_not_move_and_exits_failed() {
    let dir = workspace("perturbflat");
    let graph = crop_chain(&dir, 3402, 17005);
    let r = cli(&[
        "perturb",
        &graph,
        "--after",
        "g:cloud",
        "--region",
        HALFSPACE,
        "--axis",
        "x=-0.04:0.04:5",
        "--metric",
        "nodes.g.elementCount",
        "--expect",
        "1",
    ]);
    assert_eq!(r.code, EXIT_FAILED, "{} / {}", r.out, r.err);
    let lines = r.lines();
    let per = lines
        .iter()
        .find(|l| l["kind"] == "perturb_sample")
        .unwrap();
    assert_eq!(per["slope"], 0.0);
    assert_eq!(per["pass"], false);
    let sum = lines
        .iter()
        .find(|l| l["kind"] == "perturb_summary")
        .unwrap();
    assert_eq!(sum["pass"], 0);
    assert_eq!(sum["nonResponsive"], 1);
}

#[test]
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
fn perturb_point_from_moves_the_cut_to_each_frames_anchor() {
    let dir = workspace("perturbanchor");
    let graph = crop_chain(&dir, 3403, 17003);
    // f1 的锚点 = 17003 × 1e-5 ≈ 0.170 m：刀口挪过去之后 ±0.04 的位移再也够不到 0.05 处的裁剪边界，
    // 读数不再响应（同一张图刀口在 0 时斜率 > 0，见 perturb_inserts_the_node_and_reports_a_slope）。
    // bad 把 g.pointCount 写坏，第一遍就跑不通、取不到锚点
    let samples = dir.join("s.jsonl");
    std::fs::write(
        &samples,
        "{\"id\":\"f1\",\"set\":{}}
{\"id\":\"bad\",\"set\":{\"g.pointCount\":-5}}
",
    )
    .unwrap();
    let region = r#"{"kind":"halfspace","point":[0,0,0],"normal":[1,0,0],
                         "pointFrom":{"x":{"path":"nodes.g.elementCount","scale":0.00001}}}"#;
    let r = cli(&[
        "perturb",
        &graph,
        "--after",
        "g:cloud",
        "--region",
        region,
        "--axis",
        "x=-0.04:0.04:5",
        "--metric",
        "nodes.c.elementCount",
        "--samples",
        &samples.to_string_lossy(),
    ]);
    assert_eq!(r.code, EXIT_FAILED, "取不到锚点的样本判失败：{} / {}", r.out, r.err);
    let lines = r.lines();
    let anchor = |id: &str| {
        lines
            .iter()
            .find(|l| l["kind"] == "perturb_anchor" && l["sample"] == id)
            .cloned()
            .unwrap_or_else(|| panic!("没有 {id} 的 perturb_anchor：{}", r.out))
    };
    let f1 = anchor("f1");
    assert_eq!(f1["status"], "ok");
    let point: Vec<f64> = f1["point"].as_array().unwrap().iter().map(|v| v.as_f64().unwrap()).collect();
    assert!((point[0] - 0.17003).abs() < 1e-9 && point[1] == 0.0 && point[2] == 0.0, "{point:?}");
    assert_eq!(f1["paths"], json!(["nodes.g.elementCount"]));
    let bad = anchor("bad");
    assert_eq!([&bad["status"], &bad["point"]], [&json!("anchor_missing"), &Value::Null]);

    let rows: Vec<&Value> = lines.iter().filter(|l| l["kind"] == "perturb_row").collect();
    assert_eq!(rows.len(), 5, "只有取到锚点的 f1 跑第二遍");
    assert!(rows.iter().all(|r| r["sample"] == "f1"));
    let per = |id: &str| lines.iter().find(|l| l["kind"] == "perturb_sample" && l["sample"] == id).unwrap();
    assert_eq!(per("f1")["slope"], 0.0, "刀口跟着锚点走到 0.17，读数不响应");
    assert_eq!(per("bad")["n"], 0);
}

// ------------------------------------------------------------ 顶层图参数（M7 J7/J8）

/// g → v 的直链外加一支不相干的 h。顶层参数 count 绑 g.pointCount（g 上不再显式写它）。
fn param_chain(dir: &Path, seed: i64) -> String {
    let doc = json!({
        "schemaVersion": 1,
        "id": "01J8XQZ4K7N3M2R5V8W1YB6TCD",
        "name": "cli-params",
        "params": {
            "count": {"type": "int", "default": 20000, "binds": ["g.pointCount"],
                      "doc": "g 的点数"}
        },
        "nodes": [
            {"id": "g", "op": "gen.synthetic", "params": {"seed": seed}},
            {"id": "v", "op": "filter.voxel_grid",
             "params": {"leafSize": [0.02, 0.02, 0.02]}},
            {"id": "h", "op": "gen.synthetic", "params": {"pointCount": 500, "seed": seed + 1}}
        ],
        "edges": [
            {"id": "e1", "from": {"node": "g", "port": "cloud"},
                         "to": {"node": "v", "port": "cloud"}}
        ]
    });
    let file = dir.join("p.lyflow.json");
    std::fs::write(&file, serde_json::to_string_pretty(&doc).unwrap()).unwrap();
    file.to_string_lossy().into_owned()
}

fn plan_keys(r: &Ran) -> BTreeMap<String, String> {
    r.first()
        .as_array()
        .unwrap()
        .iter()
        .map(|n| {
            (
                n["nodeId"].as_str().unwrap().to_string(),
                n["cacheKey"].as_str().unwrap().to_string(),
            )
        })
        .collect()
}

fn run_started_keys(events: &[Value]) -> BTreeMap<String, String> {
    let started = events.iter().find(|e| e["kind"] == "run_started").expect("没有 run_started");
    started["nodes"]
        .as_array()
        .unwrap()
        .iter()
        .map(|n| {
            (
                n["id"].as_str().unwrap().to_string(),
                n["cacheKey"].as_str().unwrap().to_string(),
            )
        })
        .collect()
}

fn done_count(events: &[Value], node: &str) -> Option<i64> {
    events
        .iter()
        .filter(|e| e["kind"] == "node_state" && e["nodeId"] == node)
        .rfind(|e| e["state"] == "done" || e["state"] == "skipped")
        .and_then(|e| e["stats"]["elementCount"].as_i64())
}

#[test]
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
fn params_reports_the_graph_source_and_which_graph_param() {
    let dir = workspace("gparam-params");
    let graph = param_chain(&dir, 7101);
    let r = cli(&["params", &graph, "--json", "--param", "count=1234"]);
    assert_eq!(r.code, EXIT_OK, "{}", r.err);
    let row = r
        .lines()
        .into_iter()
        .find(|x| x["node"] == "g" && x["param"] == "pointCount")
        .expect("没有 g.pointCount");
    assert_eq!(row["value"], 1234);
    assert_eq!(row["source"], "graph");
    assert_eq!(row["graphParam"], "count");
    // 不传值就是 default，来源仍是 graph
    let d = cli(&["params", &graph, "--json", "--only", "graph"]);
    assert_eq!(d.code, EXIT_OK, "{}", d.err);
    let rows = d.lines();
    assert_eq!(rows.len(), 1, "{}", d.out);
    assert_eq!(rows[0]["value"], 20000);
}

/// J8：宿主走 C ABI 的 params_json，CLI 走 --param —— 两条路算出同一个东西。
#[test]
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
fn abi_params_json_and_cli_param_agree() {
    let dir = workspace("gparam-abi");
    let graph = param_chain(&dir, 7103);
    let r = cli(&["run", &graph, "--param", "count=1234", "--no-cache"]);
    assert_eq!(r.code, EXIT_OK, "{}", r.err);
    let cli_events = r.lines();
    assert_eq!(done_count(&cli_events, "g"), Some(1234));

    let core = core().unwrap();
    let text = std::fs::read_to_string(&graph).unwrap();
    let abi = execute(
        &core,
        RunRequest {
            graph_json: &text,
            base_dir: &dir.to_string_lossy(),
            targets: &[],
            parallel: 0,
            preview_points: 0,
            preview: false,
            no_cache: true,
            stream: None,
            params_json: Some(r#"{"count": 1234}"#),
            inputs: &[],
            group: None,
            on_event: None,
        },
    )
    .unwrap();
    assert_eq!(abi.status, "ok");
    assert_eq!(done_count(&abi.events, "g"), Some(1234));
    assert_eq!(done_count(&abi.events, "v"), done_count(&cli_events, "v"));
    assert_eq!(run_started_keys(&abi.events), run_started_keys(&cli_events));

    // ABI 上传了图没声明的名字：校验阶段就失败
    let bad = execute(
        &core,
        RunRequest {
            graph_json: &text,
            base_dir: &dir.to_string_lossy(),
            targets: &[],
            parallel: 0,
            preview_points: 0,
            preview: false,
            no_cache: true,
            stream: None,
            params_json: Some(r#"{"nope": 1}"#),
            inputs: &[],
            group: None,
            on_event: None,
        },
    )
    .unwrap();
    assert_eq!(bad.status, "error");
    let finished = bad.events.iter().find(|e| e["kind"] == "run_finished").unwrap();
    assert_eq!(finished["error"]["code"], "unknown_param", "{finished}");
}

#[test]
fn set_on_a_bound_param_and_unknown_param_are_usage_errors() {
    let dir = workspace("gparam-conflict");
    let graph = param_chain(&dir, 7104);
    let set = cli(&["run", &graph, "--set", "g.pointCount=5"]);
    assert_eq!(set.code, EXIT_USAGE, "{}", set.err);
    assert!(set.err.contains("param_conflict"), "{}", set.err);
    assert!(set.err.contains("--param count="), "{}", set.err);
    assert_eq!(cli(&["params", &graph, "--set", "g.pointCount=5"]).code, EXIT_USAGE);

    let unknown = cli(&["validate", &graph, "--param", "nope=1"]);
    assert_eq!(unknown.code, EXIT_USAGE, "{}", unknown.err);
    assert!(unknown.err.contains("unknown_param"), "{}", unknown.err);

    // 图里自己写了被绑定的参数：core 的 validate 报 param_conflict
    let mut doc: Value = serde_json::from_str(&std::fs::read_to_string(&graph).unwrap()).unwrap();
    doc["nodes"][0]["params"]["pointCount"] = json!(5);
    let file = dir.join("conflict.lyflow.json");
    std::fs::write(&file, doc.to_string()).unwrap();
    let v = cli(&["validate", &file.to_string_lossy()]);
    assert_eq!(v.code, EXIT_INVALID, "{}", v.err);
    assert!(v.first().as_array().unwrap().iter().any(|d| d["code"] == "param_conflict"));
}

#[test]
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
fn eval_takes_graph_params_next_to_sweep_axes() {
    let dir = workspace("gparam-eval");
    let graph = param_chain(&dir, 7106);
    // count=3000 是顶层参数；v.leafSize=... 是扫描轴（左边带点）
    let r = cli(&[
        "eval",
        &graph,
        "--param",
        "count=3000",
        "--param",
        "v.leafSize=0.02:0.04:2",
        "--metric",
        "nodes.g.elementCount",
    ]);
    assert_eq!(r.code, EXIT_OK, "{}", r.err);
    let rows: Vec<Value> = r.lines().into_iter().filter(|l| l["kind"] == "eval_row").collect();
    assert_eq!(rows.len(), 2, "{}", r.out);
    for row in &rows {
        assert_eq!(row["metrics"]["nodes.g.elementCount"].as_f64(), Some(3000.0), "{row}");
    }
}

// ------------------------------------------------------------ 配方（param-recipe P4.1）

/// g → v 的直链外加 h；count 绑 g.pointCount，leaf 绑 v.leafSize，都有完整规格。
/// 返回图路径；配方写进旁边的 `r.recipes/`（目录约定，`lyflow recipes` 列它）。
fn recipe_chain(dir: &Path, seed: i64) -> String {
    let doc = json!({
        "schemaVersion": 1,
        "id": "01J8XQZ4K7N3M2R5V8W1YB6RCP",
        "name": "cli-recipe",
        "params": {
            "count": {"type": "int", "default": 20000, "binds": ["g.pointCount"], "min": 1, "max": 100000},
            "leaf": {"type": "vec3f", "default": [0.02, 0.02, 0.02], "binds": ["v.leafSize"],
                     "min": 0.001, "max": 1}
        },
        "nodes": [
            {"id": "g", "op": "gen.synthetic", "params": {"seed": seed}},
            {"id": "v", "op": "filter.voxel_grid"},
            {"id": "h", "op": "gen.synthetic", "params": {"pointCount": 500, "seed": seed + 1}}
        ],
        "edges": [
            {"id": "e1", "from": {"node": "g", "port": "cloud"}, "to": {"node": "v", "port": "cloud"}}
        ]
    });
    let file = dir.join("r.lyflow.json");
    std::fs::write(&file, serde_json::to_string_pretty(&doc).unwrap()).unwrap();
    file.to_string_lossy().into_owned()
}

/// 在图旁的配方目录里写一个配方文件。graph_ref = None 时记成当前图（id 与摘要都对）。
fn write_recipe(graph: &str, name: &str, values: Value, graph_ref: Option<Value>) -> String {
    let doc: GraphDoc = serde_json::from_str(&std::fs::read_to_string(graph).unwrap()).unwrap();
    let dir = recipe::recipe_dir_of(Path::new(graph));
    std::fs::create_dir_all(&dir).unwrap();
    let file = dir.join(format!("{name}.lyflow-recipe.json"));
    let g = graph_ref.unwrap_or_else(|| json!({"id": doc.id, "specDigest": recipe::spec_digest(&doc.params)}));
    let body = json!({"schemaVersion": 1, "name": name, "graph": g, "values": values,
                      "updatedAt": "2026-09-25T00:00:00.000Z"});
    std::fs::write(&file, serde_json::to_string_pretty(&body).unwrap()).unwrap();
    file.to_string_lossy().into_owned()
}

/// P4 验收 26 的 CLI 这一半：`--recipe` 与把同一组值写成 `--param` 是同一个结果（cacheKey 与点数），
/// 同名的 `--param` 覆盖配方。编辑器那一半在 scripts/e2e/params_p4.mjs（真实 gap 图逐位比）。
#[test]
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
fn recipe_runs_like_the_same_values_given_as_param_and_param_wins() {
    let dir = workspace("recipe-run");
    let graph = recipe_chain(&dir, 7201);
    let a = write_recipe(&graph, "车型A", json!({"count": 1234, "leaf": [0.05, 0.05, 0.05]}), None);

    let r = cli(&["run", &graph, "--recipe", &a, "--no-cache"]);
    assert_eq!(r.code, EXIT_OK, "{}", r.err);
    assert!(r.err.contains("配方「车型A」：2 个值，2 个与基础不同"), "{}", r.err);
    let via_recipe = r.lines();
    assert_eq!(done_count(&via_recipe, "g"), Some(1234));

    let p = cli(&["run", &graph, "--param", "count=1234", "--param", "leaf=[0.05,0.05,0.05]", "--no-cache"]);
    assert_eq!(p.code, EXIT_OK, "{}", p.err);
    let via_param = p.lines();
    assert_eq!(run_started_keys(&via_recipe), run_started_keys(&via_param));
    assert_eq!(done_count(&via_recipe, "v"), done_count(&via_param, "v"));

    // --param 优先：配方里的 count 被盖掉，leaf 仍是配方的
    let both = cli(&["run", &graph, "--recipe", &a, "--param", "count=777", "--no-cache"]);
    assert_eq!(both.code, EXIT_OK, "{}", both.err);
    assert_eq!(done_count(&both.lines(), "g"), Some(777));
    let plan_both = cli(&["plan", &graph, "--recipe", &a, "--param", "count=777"]);
    let plan_param = cli(&["plan", &graph, "--param", "count=777", "--param", "leaf=[0.05,0.05,0.05]"]);
    assert_eq!(plan_keys(&plan_both), plan_keys(&plan_param));
    // 没绑到配方参数的 h 不受影响
    assert_eq!(plan_keys(&plan_both)["h"], plan_keys(&cli(&["plan", &graph]))["h"]);

    let v = cli(&["validate", &graph, "--recipe", &a]);
    assert_eq!(v.code, EXIT_OK, "{}", v.err);
    let params = cli(&["params", &graph, "--json", "--only", "graph", "--recipe", &a]);
    assert_eq!(params.code, EXIT_OK, "{}", params.err);
    let row = params.lines().into_iter().find(|x| x["node"] == "v").expect("没有 v.leafSize");
    assert_eq!(row["value"], json!([0.05, 0.05, 0.05]));
    assert_eq!(row["graphParam"], "leaf");

    assert_eq!(cli(&["run", &graph, "--recipe", &a, "--recipe", &a]).code, EXIT_USAGE);
    let nowhere = dir.join("没有.lyflow-recipe.json").to_string_lossy().into_owned();
    let missing = cli(&["run", &graph, "--recipe", &nowhere]);
    assert_eq!(missing.code, EXIT_USAGE, "{}", missing.err);
    assert!(missing.err.contains("bad_recipe"), "{}", missing.err);
}

/// P4 验收 27：失配 ①–③ → 退出码 4、stderr 逐条列出（与编辑器同一套用语），一个节点都不跑；
/// run / validate / plan / params / eval 都一样。
#[test]
fn a_mismatched_recipe_stops_every_command_with_exit_4_and_the_report() {
    let dir = workspace("recipe-mismatch");
    let graph = recipe_chain(&dir, 7202);
    let bad = write_recipe(&graph, "坏", json!({"count": 0, "leaf": "abc", "nope": 1}), None);
    for args in [
        vec!["run", graph.as_str(), "--recipe", bad.as_str()],
        vec!["validate", graph.as_str(), "--recipe", bad.as_str()],
        vec!["plan", graph.as_str(), "--recipe", bad.as_str()],
        vec!["params", graph.as_str(), "--recipe", bad.as_str()],
        vec!["eval", graph.as_str(), "--recipe", bad.as_str(), "--metric", "nodes.g.elementCount"],
    ] {
        let r = cli(&args);
        assert_eq!(r.code, EXIT_USAGE, "{args:?}: {}", r.err);
        assert!(r.out.trim().is_empty(), "{args:?} 不该有 stdout：{}", r.out);
        for want in [
            "配方「坏」有 3 处失配，不能运行",
            "[越界] count：不能小于 1 → 夹到限位：1",
            "[类型不符] leaf：应当是 3 个数的数组，实际是 \"abc\" → 删除这个值（用基础）",
            "[多出] nope：图里没有图参数 nope（改名或删掉了？） → 删除这个值",
        ] {
            assert!(r.err.contains(want), "{args:?} 缺「{want}」：\n{}", r.err);
        }
    }
    // 共享夹具：每一条都照 expected.json 的文案出现在 stderr 上
    let fixtures = Path::new(env!("CARGO_MANIFEST_DIR")).join("../schema/fixtures/recipes");
    let expected: Value =
        serde_json::from_str(&std::fs::read_to_string(fixtures.join("expected.json")).unwrap()).unwrap();
    let fixture_graph = fixtures.join("graph.lyflow.json").to_string_lossy().into_owned();
    for (file, want) in expected["recipes"].as_object().unwrap() {
        let path = fixtures.join("graph.recipes").join(file).to_string_lossy().into_owned();
        let r = cli(&["validate", &fixture_graph, "--recipe", &path]);
        if want["blocking"].as_u64().unwrap() == 0 {
            // 没失配时交给 core 校验（夹具图的 test.param_showcase 只在 LYFLOW_TEST_OPS=1 时注册），退出码不是 4
            assert_ne!(r.code, EXIT_USAGE, "{file}: {}", r.err);
            continue;
        }
        assert_eq!(r.code, EXIT_USAGE, "{file}: {}", r.err);
        for item in want["items"].as_array().unwrap() {
            let label = match item["kind"].as_str().unwrap() {
                "extra" => "多出",
                "type" => "类型不符",
                "range" => "越界",
                _ => "规格变了",
            };
            let (message, fix) = (item["message"].as_str().unwrap(), item["fixLabel"].as_str().unwrap());
            let line = match item["param"].as_str() {
                Some(p) => format!("[{label}] {p}：{message} → {fix}"),
                None => format!("[{label}] {message} → {fix}"),
            };
            assert!(r.err.contains(&line), "{file} 缺「{line}」：\n{}", r.err);
        }
    }
}

/// ④ 规格变了只提示：退出码照旧，stderr 一行「提示：…」，值照常用上。
#[test]
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
fn a_recipe_written_for_another_graph_only_warns() {
    let dir = workspace("recipe-spec");
    let graph = recipe_chain(&dir, 7203);
    let other = write_recipe(
        &graph,
        "别的图",
        json!({"count": 4321}),
        Some(json!({"id": "01JSOMEOTHERGRAPH000000000", "specDigest": format!("sha256:{}", "0".repeat(64))})),
    );
    let r = cli(&["run", &graph, "--recipe", &other, "--no-cache"]);
    assert_eq!(r.code, EXIT_OK, "{}", r.err);
    assert!(
        r.err.contains("提示：配方「别的图」[规格变了] 图 id 不同（配方记的是 01JSOMEOTHERGRAPH000000000）"),
        "{}",
        r.err
    );
    assert_eq!(done_count(&r.lines(), "g"), Some(4321));
}

/// eval：配方作用于所有样本；参数组里不含「.」的键写图参数，叠在配方上面；--param 最后说了算。
#[test]
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
fn eval_layers_base_recipe_paramsets_then_param() {
    let dir = workspace("recipe-eval");
    let graph = recipe_chain(&dir, 7204);
    let a = write_recipe(&graph, "A", json!({"count": 1234}), None);
    let samples = dir.join("samples.jsonl");
    std::fs::write(&samples, "{\"id\":\"s1\",\"set\":{\"h.seed\":1}}\n{\"id\":\"s2\",\"set\":{\"h.seed\":2}}\n").unwrap();
    let sets = dir.join("sets.json");
    std::fs::write(&sets, r#"[{"count": 3000}, {"h.pointCount": 600}]"#).unwrap();
    let (samples, sets) = (samples.to_string_lossy().into_owned(), sets.to_string_lossy().into_owned());
    let rows = |r: &Ran| -> Vec<(u64, String, f64)> {
        r.lines()
            .into_iter()
            .filter(|l| l["kind"] == "eval_row")
            .map(|l| {
                (
                    l["paramSet"].as_u64().unwrap(),
                    l["sample"].as_str().unwrap().to_string(),
                    l["metrics"]["nodes.g.elementCount"].as_f64().unwrap_or(-1.0),
                )
            })
            .collect()
    };
    let base = ["eval", graph.as_str(), "--samples", samples.as_str(), "--metric", "nodes.g.elementCount"];

    let only = cli(&[&base[..], &["--recipe", a.as_str()]].concat());
    assert_eq!(only.code, EXIT_OK, "{}", only.err);
    assert_eq!(rows(&only), [(0, "s1".into(), 1234.0), (0, "s2".into(), 1234.0)]);

    let layered = cli(&[&base[..], &["--recipe", a.as_str(), "--params", sets.as_str()]].concat());
    assert_eq!(layered.code, EXIT_OK, "{}", layered.err);
    assert_eq!(
        rows(&layered),
        [(0, "s1".into(), 3000.0), (0, "s2".into(), 3000.0), (1, "s1".into(), 1234.0), (1, "s2".into(), 1234.0)]
    );

    let pinned =
        cli(&[&base[..], &["--recipe", a.as_str(), "--params", sets.as_str(), "--param", "count=500"]].concat());
    assert_eq!(pinned.code, EXIT_OK, "{}", pinned.err);
    assert!(rows(&pinned).iter().all(|(_, _, n)| *n == 500.0), "{:?}", rows(&pinned));

    let typo = dir.join("typo.json");
    std::fs::write(&typo, r#"[{"cuont": 1}]"#).unwrap();
    let typo = typo.to_string_lossy().into_owned();
    let t = cli(&[&base[..], &["--params", typo.as_str()]].concat());
    assert_eq!(t.code, EXIT_USAGE, "{}", t.err);
    assert!(t.err.contains("unknown_param"), "{}", t.err);
}

/// patch --recipe：配方的值写回基础（等于对每一行「写回基础」），--param 排在后面；失配整体不写。
#[test]
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
fn patch_recipe_writes_the_values_back_as_defaults() {
    let dir = workspace("recipe-patch");
    let graph = recipe_chain(&dir, 7205);
    let a = write_recipe(&graph, "A", json!({"count": 1234, "leaf": [0.05, 0.05, 0.05]}), None);
    let out = dir.join("baked.lyflow.json").to_string_lossy().into_owned();
    let r = cli(&["patch", &graph, "--recipe", &a, "--param", "count=999", "-o", &out, "--json"]);
    assert_eq!(r.code, EXIT_OK, "{}", r.err);
    assert_eq!(r.first()["applied"]["recipe"], json!(["count", "leaf"]));
    assert_eq!(r.first()["applied"]["param"], json!(["count"]));
    let changed = r.first()["diff"]["graphParams"].clone();
    let count = changed.as_array().unwrap().iter().find(|p| p["name"] == "count").expect("diff 里没有 count");
    assert_eq!(count["from"], 20000, "{changed}");
    assert_eq!(count["to"], 999, "{changed}");
    let written: Value = serde_json::from_str(&std::fs::read_to_string(&out).unwrap()).unwrap();
    assert_eq!(written["params"]["count"]["default"], 999);
    assert_eq!(written["params"]["leaf"]["default"], json!([0.05, 0.05, 0.05]));

    let again = cli(&["patch", &out, "--recipe", &a, "--param", "count=999", "--dry-run", "--json"]);
    assert_eq!(again.code, EXIT_OK, "{}", again.err);
    assert_eq!(again.first()["diff"]["empty"], true);

    let bad = write_recipe(&graph, "坏", json!({"count": 0}), None);
    let before = std::fs::read_to_string(&graph).unwrap();
    let b = cli(&["patch", &graph, "--recipe", &bad]);
    assert_eq!(b.code, EXIT_USAGE, "{}", b.err);
    assert!(b.err.contains("[越界] count"), "{}", b.err);
    assert_eq!(std::fs::read_to_string(&graph).unwrap(), before, "失配时不该写图");
}

/// `lyflow recipes`：列配方目录（index.json 的顺序与默认），每个配方的失配与夹具一致；
/// 给 --recipe 时带上合成好的 params。不需要 core。
#[test]
fn recipes_lists_the_dir_with_reports_and_params() {
    let fixtures = Path::new(env!("CARGO_MANIFEST_DIR")).join("../schema/fixtures/recipes");
    let graph = fixtures.join("graph.lyflow.json").to_string_lossy().into_owned();
    let expected: Value =
        serde_json::from_str(&std::fs::read_to_string(fixtures.join("expected.json")).unwrap()).unwrap();
    let r = cli(&["recipes", &graph, "--json"]);
    assert_eq!(r.code, EXIT_OK, "{}", r.err);
    let lines = r.lines();
    let (rows, tail) = lines.split_at(lines.len() - 1);
    let names: Vec<&str> = rows.iter().map(|x| x["name"].as_str().unwrap()).collect();
    assert_eq!(names, ["ok", "extra", "type", "range", "spec", "nograph"]);
    for row in rows {
        let file = format!("{}.lyflow-recipe.json", row["name"].as_str().unwrap());
        let want = &expected["recipes"][&file];
        assert_eq!(row["blocking"], want["blocking"], "{file}");
        assert_eq!(row["items"].as_array().unwrap().len(), want["items"].as_array().unwrap().len(), "{file}");
        assert_eq!(row["default"], row["name"] == "ok");
        assert!(row.get("params").is_none());
    }
    assert_eq!(tail[0]["kind"], "recipe_dir");
    assert_eq!(tail[0]["default"], "ok");
    assert_eq!(tail[0]["count"], 6);
    assert_eq!(tail[0]["specDigest"], expected["specDigest"]);

    let ok = fixtures.join("graph.recipes/ok.lyflow-recipe.json").to_string_lossy().into_owned();
    let one = cli(&["recipes", &graph, "--recipe", &ok, "--json"]);
    assert_eq!(one.code, EXIT_OK, "{}", one.err);
    let row = one.first();
    assert_eq!(row["params"]["cutMax"], json!(2.5));
    assert_eq!(row["params"]["pointCount"], json!(20000));
    assert_eq!(row["params"]["legacyMin"], json!("随便什么都不查"));

    // 没有配方目录的图：0 个配方，exists=false
    let dir = workspace("recipe-none");
    let lonely = recipe_chain(&dir, 7206);
    let none = cli(&["recipes", &lonely, "--json"]);
    assert_eq!(none.code, EXIT_OK, "{}", none.err);
    assert_eq!(none.first()["exists"], false);
    assert_eq!(none.first()["count"], 0);
}

/// J5/J6：dirMode=band 却没接 refLine。检查在 validate 里，所以 `run` 在执行任何节点
/// 之前就停下（没有一条 node_state）。gap 包没编进来时 gap.fit_line 是 unknown_op，
/// 同样校验失败 —— 所以前半句在任何构建里都成立，诊断码只在带 gap 包时才断言。
#[test]
fn fit_line_band_without_ref_line_fails_at_validate() {
    let dir = workspace("m7-fit-line");
    let doc = json!({
        "schemaVersion": 1,
        "id": "01J8XQZ4K7N3M2R5V8W1YB6TCD",
        "nodes": [
            {"id": "g", "op": "gen.synthetic", "params": {"pointCount": 2000, "seed": 7107}},
            {"id": "roi", "op": "gap.overall_roi"},
            {"id": "fit", "op": "gap.fit_line", "params": {"dirMode": "band"}}
        ],
        "edges": [
            {"id": "e1", "from": {"node": "g", "port": "cloud"}, "to": {"node": "roi", "port": "primary"}},
            {"id": "e2", "from": {"node": "g", "port": "cloud"}, "to": {"node": "roi", "port": "secondary"}},
            {"id": "e3", "from": {"node": "g", "port": "cloud"}, "to": {"node": "fit", "port": "cloud"}},
            {"id": "e4", "from": {"node": "roi", "port": "box"}, "to": {"node": "fit", "port": "box"}},
            {"id": "e5", "from": {"node": "roi", "port": "box"}, "to": {"node": "fit", "port": "toward"}}
        ]
    });
    let file = dir.join("band.lyflow.json");
    std::fs::write(&file, doc.to_string()).unwrap();
    let path = file.to_string_lossy().into_owned();

    let v = cli(&["validate", &path]);
    assert_eq!(v.code, EXIT_INVALID, "{}", v.err);
    let run = cli(&["run", &path]);
    assert_eq!(run.code, EXIT_INVALID, "{}", run.err);
    assert!(
        !run.lines().iter().any(|e| e["kind"] == "node_state" || e["kind"] == "run_started"),
        "校验没过就不该起跑：{}",
        run.out
    );

    let gap_built = std::env::var("LYFLOW_PACKS").unwrap_or_default().contains("gap");
    let manifest = cli(&["manifest"]).first();
    let has_fit_line = manifest["operators"]
        .as_array()
        .unwrap()
        .iter()
        .any(|o| o["id"] == "gap.fit_line");
    assert!(!gap_built || has_fit_line, "LYFLOW_PACKS 带了 gap，manifest 里却没有 gap.fit_line");
    if has_fit_line {
        let diags = v.first();
        let d = diags
            .as_array()
            .unwrap()
            .iter()
            .find(|d| d["severity"] == "error")
            .unwrap()
            .clone();
        assert_eq!(d["code"], "bad_param", "{diags}");
        assert_eq!(d["phase"], "validate", "{diags}");
        assert_eq!(d["nodeId"], "fit", "{diags}");
    }
}

/// m8-plan L12 与 M8a 验收 4：`import` 默认产出积木图，`--fine` 产出细粒度图；
/// 积木图里把 datum 框拖到 target 那一侧，`lyflow validate` 在执行前就报错。
/// 导入器属于 gap 包，没编进来时整条跳过（manifest 里没有这个 kind）。
#[test]
fn import_defaults_to_blocks_and_fine_flag_gives_the_fine_graph() {
    let manifest = cli(&["manifest"]).first();
    let has_importer = manifest["importers"]
        .as_array()
        .map(|a| a.iter().any(|i| i["kind"] == "StandardGap.yml:template:fine"))
        .unwrap_or(false);
    let gap_built = std::env::var("LYFLOW_PACKS").unwrap_or_default().contains("gap");
    assert!(!gap_built || has_importer, "LYFLOW_PACKS 带了 gap，却没有细粒度导入器");
    if !has_importer {
        return;
    }
    let dir = workspace("m8a-import");
    let config = dir.join("StandardGap.yml");
    std::fs::write(
        &config,
        "common_settings: {seg_mode: ROI, overall_roi: [-18, 150, 18, 180]}\n\
         flush: {base_type: fit line, ref_type: line end, base_roi: [-15, 163, -5, 167],\
           ref_roi: [5, 162, 15, 166]}\n\
         gap: {left_type: circle, right_type: circle, left_roi: [-4.5, 164, -1.5, 167],\
           right_roi: [1.5, 163, 4.5, 166], radius: {left_circle_radius_min: 0.5,\
           left_circle_radius_max: 2, right_circle_radius_min: 0.5, right_circle_radius_max: 2}}\n\
         align: {align_cloud: true}\n",
    )
    .unwrap();
    let config = config.to_string_lossy().into_owned();
    let blocks_path = dir.join("blocks.lyflow.json").to_string_lossy().into_owned();
    let fine_path = dir.join("fine.lyflow.json").to_string_lossy().into_owned();

    let r = cli(&["import", &config, "--kind", "StandardGap.yml:template", "-o", &blocks_path]);
    assert_eq!(r.code, EXIT_OK, "{}", r.err);
    let r = cli(&["import", &config, "--kind", "StandardGap.yml:template", "--fine", "-o", &fine_path]);
    assert_eq!(r.code, EXIT_OK, "{}", r.err);

    let read = |p: &str| -> Value { serde_json::from_str(&std::fs::read_to_string(p).unwrap()).unwrap() };
    let ops = |doc: &Value| -> Vec<String> {
        doc["nodes"].as_array().unwrap().iter().map(|n| n["op"].as_str().unwrap().to_string()).collect()
    };
    let blocks = read(&blocks_path);
    let fine = read(&fine_path);
    assert!(ops(&blocks).contains(&"gap.locate_template".to_string()), "{blocks}");
    assert!(!ops(&blocks).contains(&"gap.fit_line".to_string()), "{blocks}");
    assert!(ops(&blocks).len() <= 12, "{blocks}");
    assert!(ops(&fine).contains(&"gap.fit_line".to_string()), "{fine}");
    assert!(ops(&fine).contains(&"gap.business_rois".to_string()), "{fine}");
    for doc in [&blocks, &fine] {
        assert!(!ops(doc).contains(&"gap.measure_reference".to_string()), "{doc}");
    }
    for p in [&blocks_path, &fine_path] {
        let v = cli(&["validate", p]);
        assert_eq!(v.code, EXIT_OK, "{p}: {}", v.out);
    }

    // 把槽 1 的 datum 框拖到缝右边、target 旁边
    let mut dragged = blocks.clone();
    for n in dragged["nodes"].as_array_mut().unwrap() {
        if n["op"] == "gap.locate_template" {
            n["params"]["template1DatumRoi"] = json!([16, 162, 17.5, 166]);
        }
    }
    let dragged_path = dir.join("dragged.lyflow.json");
    std::fs::write(&dragged_path, dragged.to_string()).unwrap();
    let dragged_path = dragged_path.to_string_lossy().into_owned();
    let v = cli(&["validate", &dragged_path]);
    assert_eq!(v.code, EXIT_INVALID, "{}", v.out);
    let diags = v.first();
    let d = diags
        .as_array()
        .unwrap()
        .iter()
        .find(|d| d["severity"] == "error")
        .unwrap()
        .clone();
    assert_eq!(d["code"], "bad_param", "{diags}");
    assert_eq!(d["phase"], "validate", "{diags}");
    assert_eq!(d["nodeId"], "n_locate", "{diags}");
    assert_eq!(d["paramPath"], "template1DatumRoi", "{diags}");
    let run = cli(&["run", &dragged_path]);
    assert_eq!(run.code, EXIT_INVALID, "{}", run.err);
    assert!(
        !run.lines().iter().any(|e| e["kind"] == "node_state" || e["kind"] == "run_started"),
        "校验没过就不该起跑：{}",
        run.out
    );
}
