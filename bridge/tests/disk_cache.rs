//! 结果缓存落盘（docs/disk-cache-plan.md）的跨进程验收：真的起两次 `lyflow` 进程。
//!
//! 放在集成测试而不是 cli.rs 的单元测试里：落盘开关是进程级的，单元测试和别的用例在同一个进程里
//! 并行跑，打开它会让别的用例意外命中缓存。这里每次都是一个新进程 —— 这正是要验证的场景。

use std::path::{Path, PathBuf};
use std::process::Command;

use serde_json::{json, Value};

struct Ran {
    code: i32,
    out: String,
    err: String,
}

impl Ran {
    fn lines(&self) -> Vec<Value> {
        self.out.lines().filter_map(|l| serde_json::from_str(l).ok()).collect()
    }

    /// 某个节点最后一条 node_state。
    fn node(&self, id: &str) -> Value {
        self.lines()
            .into_iter()
            .filter(|l| l["kind"] == "node_state" && l["nodeId"] == id)
            .last()
            .unwrap_or_else(|| panic!("没有 {id} 的 node_state：{}", self.out))
    }
}

fn lyflow(args: &[&str], env: &[(&str, &str)]) -> Ran {
    let mut cmd = Command::new(env!("CARGO_BIN_EXE_lyflow"));
    cmd.args(args).env_remove("LYFLOW_CACHE_DIR");
    for (k, v) in env {
        cmd.env(k, v);
    }
    let o = cmd.output().expect("起不来 lyflow");
    Ran {
        code: o.status.code().unwrap_or(-1),
        out: String::from_utf8_lossy(&o.stdout).into_owned(),
        err: String::from_utf8_lossy(&o.stderr).into_owned(),
    }
}

fn workspace(tag: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("lyflow-disk-cache-{tag}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    dir
}

/// 生成一片够大的云（≥ 20 ms，过得了落盘阈值）再体素降采样：两个节点都只出点云，都该落盘。
fn graph(dir: &Path) -> String {
    let doc = json!({
        "schemaVersion": 1,
        "id": "01J8XQZ4K7N3M2R5V8W1YB6DC1",
        "name": "disk-cache",
        "nodes": [
            {"id": "g", "op": "gen.synthetic",
             "params": {"pointCount": 1500000, "seed": 4242, "outlierRatio": 0.0}},
            {"id": "v", "op": "filter.voxel_grid", "params": {"leafSize": [0.01, 0.01, 0.01]}}
        ],
        "edges": [
            {"id": "e1", "from": {"node": "g", "port": "cloud"}, "to": {"node": "v", "port": "cloud"}}
        ]
    });
    let file = dir.join("g.lyflow.json");
    std::fs::write(&file, serde_json::to_string_pretty(&doc).unwrap()).unwrap();
    file.to_string_lossy().into_owned()
}

fn lfc_files(dir: &Path) -> usize {
    let mut n = 0;
    let mut stack = vec![dir.to_path_buf()];
    while let Some(d) = stack.pop() {
        for e in std::fs::read_dir(&d).into_iter().flatten().flatten() {
            let p = e.path();
            if p.is_dir() {
                stack.push(p);
            } else if p.extension().is_some_and(|x| x == "lfc") {
                n += 1;
            }
        }
    }
    n
}

#[test]
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
fn a_second_process_reuses_the_expensive_upstream_from_disk() {
    let ws = workspace("reuse");
    let g = graph(&ws);
    let cache = ws.join("cache");
    let cache_s = cache.to_string_lossy().into_owned();

    // 默认关：不给目录就什么都不写
    let plain = lyflow(&["run", &g], &[]);
    assert_eq!(plain.code, 0, "{} / {}", plain.out, plain.err);
    assert!(!cache.exists());

    // 第一个进程：真算、落盘
    let first = lyflow(&["run", &g, "--cache-dir", &cache_s], &[]);
    assert_eq!(first.code, 0, "{} / {}", first.out, first.err);
    assert_eq!(first.node("g")["state"], "done");
    assert!(first.err.contains("落盘缓存"), "{}", first.err);
    assert_eq!(lfc_files(&cache), 2, "g 与 v 都只出点云、都够贵");

    // 第二个进程（内存缓存是空的）：两个节点都从盘上命中，点数相同
    let second = lyflow(&["run", &g, "--cache-dir", &cache_s], &[]);
    assert_eq!(second.code, 0, "{} / {}", second.out, second.err);
    for id in ["g", "v"] {
        let n = second.node(id);
        assert_eq!([&n["state"], &n["stats"]["cached"]], [&json!("skipped"), &json!(true)], "{id}: {n}");
        assert_eq!(n["stats"]["elementCount"], first.node(id)["stats"]["elementCount"], "{id}");
    }

    // 环境变量与 --cache-dir 等价（MCP 起的 CLI 进程靠它）
    let via_env = lyflow(&["run", &g], &[("LYFLOW_CACHE_DIR", &cache_s)]);
    assert_eq!(via_env.code, 0, "{}", via_env.err);
    assert_eq!(via_env.node("v")["stats"]["cached"], true);

    // cache info：一个指纹目录、就是当前的、两个文件
    let info = lyflow(&["cache", "info", "--cache-dir", &cache_s], &[]);
    assert_eq!(info.code, 0, "{}", info.err);
    let report = info.lines().into_iter().find(|l| l["kind"] == "cache_info").unwrap();
    let entries = report["entries"].as_array().unwrap();
    assert_eq!(entries.len(), 1, "{report}");
    assert_eq!([&entries[0]["current"], &entries[0]["files"]], [&json!(true), &json!(2)]);
    assert!(entries[0]["bytes"].as_u64().unwrap() > 1_000_000);

    // 旧指纹的目录（重编 core 之前留下的）：--stale 只删它，当前的留着
    std::fs::create_dir_all(cache.join("a14-0000000000000000").join("ab")).unwrap();
    std::fs::write(cache.join("a14-0000000000000000").join("ab").join("x.lfc"), b"old").unwrap();
    let stale = lyflow(&["cache", "clear", "--stale", "--cache-dir", &cache_s], &[]);
    assert_eq!(stale.code, 0, "{}", stale.err);
    assert_eq!(lfc_files(&cache), 2, "当前指纹的两个文件还在");
    let all = lyflow(&["cache", "clear", "--cache-dir", &cache_s], &[]);
    assert_eq!(all.code, 0, "{}", all.err);
    assert_eq!(lfc_files(&cache), 0);

    let _ = std::fs::remove_dir_all(&ws);
}

#[test]
fn cache_subcommand_needs_a_directory() {
    let r = lyflow(&["cache", "info"], &[]);
    assert_eq!(r.code, 4, "{}", r.err);
    assert!(r.err.contains("--cache-dir"), "{}", r.err);
}
