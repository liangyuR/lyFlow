//! patch.rs 的单元测试（从 patch.rs 里搬出来：原文件四成是测试）。

use super::*;
use crate::cli::test_support::cli;

fn workspace(name: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("lyflow-patch-{name}"));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    dir
}

/// 一张有「主路径 + 一个 b_ 开头的旁支」的图。b_alt 挂在 g 上（边 e3）、谁也不喂，
/// 删掉它连同 e3 之后图仍然合法 —— 这样测的是 patch 自己，不是算子的输入要求。
fn graph(dir: &Path, outputs: bool) -> String {
    let mut doc = json!({
        "schemaVersion": 1,
        "id": "01J8XQZ4K7N3M2R5V8W1YB6TCD",
        "name": "patch",
        "nodes": [
            {"id": "g", "op": "gen.synthetic",
             "params": {"pointCount": 500, "seed": 11},
             "ui": {"position": {"x": 0, "y": 0}}},
            {"id": "b_alt", "op": "filter.passthrough",
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
                         "to": {"node": "sink", "port": "cloud"}},
            {"id": "e3", "from": {"node": "g", "port": "cloud"},
                         "to": {"node": "b_alt", "port": "cloud"}}
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
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
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
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
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
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
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
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
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
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
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
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
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

    // sink 的输入已经接着 a：--connect 不替人顶掉，由「单连接」拦住（要换源用 --rewire）
    let taken = cli(&["patch", &path, "--connect", "g:cloud=sink:cloud", "--json"]);
    assert_eq!(taken.code, EXIT_INVALID, "{}", taken.out);
    assert!(taken.err.contains("connect 之后"), "{}", taken.err);
    assert_eq!(std::fs::read_to_string(&path).unwrap(), before, "不该写盘");
}

/// `--add-node` 单独用只对源算子成立（ADR-0023）；配上 `--connect`，一条命令就能挂上一个要输入的节点。
/// 同一条 `--connect` 再跑一遍是 no-op（same_edge），文件一个字节不动。
#[test]
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
fn connect_wires_a_node_added_in_the_same_patch_and_is_idempotent() {
    let dir = workspace("connect");
    let path = graph(&dir, false);
    let first = cli(&[
        "patch",
        &path,
        "--add-node",
        r#"{"id":"v2","op":"filter.voxel_grid","params":{"leafSize":[0.05,0.05,0.05]}}"#,
        "--connect",
        "g:cloud=v2:cloud",
        "--json",
    ]);
    assert_eq!(first.code, EXIT_OK, "{}", first.err);
    assert_eq!(ids(&first.result()["applied"]["connected"]), vec!["g:cloud=v2:cloud"]);
    let doc = read(&path);
    let wired = doc["edges"].as_array().unwrap().iter().any(|e| {
        e["from"] == json!({"node": "g", "port": "cloud"}) && e["to"] == json!({"node": "v2", "port": "cloud"})
    });
    assert!(wired, "{}", doc["edges"]);
    let after_first = std::fs::read_to_string(&path).unwrap();

    let again = cli(&["patch", &path, "--connect", "g:cloud=v2:cloud", "--json"]);
    assert_eq!(again.code, EXIT_OK, "{}", again.err);
    let res = again.result();
    assert_eq!(res["noops"][0]["reason"], "same_edge", "{}", again.out);
    assert_eq!(res["wrote"], Value::Null, "全 no-op 的原地覆写不该落盘");
    assert_eq!(std::fs::read_to_string(&path).unwrap(), after_first);
}

/// 节点 id 的通配是大小写敏感的（m6-plan §10 第 6 条）：`N_FB_*` 配不上 `n_fb_*`，
/// 于是这条命令是 no-op 而不是「悄悄把那个节点删了」。
#[test]
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
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

    // 同一条命令换成小写就真删掉了 —— 证明差别只在大小写上。
    // 删的是节点连同它的全部边：b_alt 的入边 e3 也得跟着走，主路径的 e1 / e2 不动
    let lower = cli(&["patch", &path, "--remove-node", "b_*", "--json"]);
    assert_eq!(lower.code, EXIT_OK, "{}", lower.err);
    let res = lower.result();
    assert_eq!(ids(&res["applied"]["removed"]), vec!["b_alt"]);
    assert_eq!(res["wrote"].as_str().unwrap(), path);
    assert_eq!(res["diff"]["nodesRemoved"][0]["id"], "b_alt");
    assert_eq!(res["diff"]["edgesRemoved"], json!(["g.cloud -> b_alt.cloud"]), "{}", lower.out);
    let doc = read(&path);
    let node_ids: Vec<&str> = doc["nodes"].as_array().unwrap().iter().map(|n| n["id"].as_str().unwrap()).collect();
    assert_eq!(node_ids, ["g", "a", "sink"]);
    let edge_ids: Vec<&str> = doc["edges"].as_array().unwrap().iter().map(|e| e["id"].as_str().unwrap()).collect();
    assert_eq!(edge_ids, ["e1", "e2"]);
}

#[test]
fn the_corner_is_empty_even_when_no_node_has_a_position() {
    let doc: GraphDoc = serde_json::from_str(
        r#"{"schemaVersion":1,"id":"g","nodes":[{"id":"n","op":"gen.synthetic"}],"edges":[]}"#,
    )
    .unwrap();
    assert_eq!(free_corner(&doc), json!({"x": 0.0, "y": 0.0}));
}
