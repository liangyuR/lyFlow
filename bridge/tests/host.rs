//! 外部 Rust/Tauri 宿主那条路的钉子。
//!
//! 这个文件本身就是「跨 crate 能不能用」的证据：集成测试是**独立的 crate**，
//! 它看 `lyflow_lib` 的角度和 dts-check 完全一样 —— `lyflow_handler!` 在这里展开得开，
//! 在宿主那边就展开得开。
//!
//! 需要 `lyflow_core.dll`：debug 构建走 build.rs 写进来的 `LYFLOW_CORE_BIN`
//! （`core_ffi::dll_path()`），是绝对路径，测试二进制放在哪都找得到。

#![cfg(feature = "host")]

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use lyflow_lib::core_ffi::RunInput;
use lyflow_lib::graph::GraphDoc;
use lyflow_lib::host::{self, Cloud, HostConfig, SceneProvider};
use tauri::test::{mock_builder, mock_context, noop_assets, MockRuntime, INVOKE_KEY};
use tauri::Listener;

// ------------------------------------------------------------------ 脚手架

fn repo_root() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .expect("bridge/ 总有上级")
        .to_path_buf()
}

/// 合成一条轮廓：x 从 -20 到 20 mm 共 400 点，z 是一段平底加一个凸起（胶条那一坨），
/// 亮度全 1.0。`bump` 是凸起的高度 —— 换一个值就是换一片云，用来验证 cacheKey 会变。
fn synthetic_profile(bump: f32) -> Cloud {
    const N: usize = 400;
    let mut xyz = Vec::with_capacity(N * 3);
    for i in 0..N {
        let x = -20.0 + 40.0 * (i as f32) / ((N - 1) as f32);
        let z = if x.abs() < 5.0 {
            bump * (1.0 - (x / 5.0).powi(2))
        } else {
            0.0
        };
        xyz.extend_from_slice(&[x, 0.0, z]);
    }
    Cloud {
        xyz,
        intensity: vec![1.0; N],
    }
}

/// 宿主那边的会话表。真实实现会是「相机读一帧 → 存进表 → 前端拿着 sceneId 点运行」，
/// 这里直接预置两条。端口名写死是故意的：宿主知道自己接的是哪个算子。
struct Scenes {
    sessions: Mutex<BTreeMap<String, Cloud>>,
}

impl SceneProvider for Scenes {
    fn inputs(
        &self,
        scene_id: &str,
        graph: &GraphDoc,
        manifest: &serde_json::Value,
    ) -> Result<Vec<RunInput>, String> {
        let sessions = self.sessions.lock().unwrap();
        let cloud = sessions
            .get(scene_id)
            .ok_or("点云会话已失效，请重新读取一次")?;
        let mut by_port = BTreeMap::new();
        by_port.insert("profile".to_string(), cloud.clone());
        host::inputs_for_source_op(graph, manifest, "dts.profile_in", &by_port)
    }
}

/// 收到的 `execution-event`，按到达顺序。
type Events = Arc<Mutex<Vec<serde_json::Value>>>;

struct Fixture {
    app: tauri::App<MockRuntime>,
    events: Events,
}

fn fixture() -> Fixture {
    let scenes = Arc::new(Scenes {
        sessions: Mutex::new(BTreeMap::new()),
    });
    scenes
        .sessions
        .lock()
        .unwrap()
        .insert("s1".into(), synthetic_profile(1.5));
    scenes
        .sessions
        .lock()
        .unwrap()
        .insert("s2".into(), synthetic_profile(2.5));

    let cfg = HostConfig {
        workspace_root: None,
        // 空清单 = 这个宿主不要库算子。测试进程不该去碰 app data。
        library_dirs: Some(Vec::new()),
        scenes: Some(scenes.clone() as Arc<dyn SceneProvider>),
        // 仓库里真有 build/core/bin/lyflow_core.dll 时 watcher 会起线程，
        // 测试期间一次热重载就会把结果仓清掉。
        hot_reload: false,
    };

    let app = host::attach(mock_builder(), cfg)
        .invoke_handler(lyflow_lib::lyflow_handler![])
        .build(mock_context(noop_assets()))
        .expect("建 mock app 失败");

    let events: Events = Arc::new(Mutex::new(Vec::new()));
    let sink = events.clone();
    app.listen(lyflow_lib::execution::EVENT_NAME, move |e| {
        if let Ok(v) = serde_json::from_str::<serde_json::Value>(e.payload()) {
            sink.lock().unwrap().push(v);
        }
    });

    Fixture { app, events }
}

fn webview(app: &tauri::App<MockRuntime>) -> tauri::WebviewWindow<MockRuntime> {
    tauri::WebviewWindowBuilder::new(app, "main", Default::default())
        .build()
        .expect("建 mock webview 失败")
}

fn invoke(
    view: &tauri::WebviewWindow<MockRuntime>,
    cmd: &str,
    args: serde_json::Value,
) -> Result<serde_json::Value, String> {
    let res = tauri::test::get_ipc_response(
        view,
        tauri::webview::InvokeRequest {
            cmd: cmd.into(),
            callback: tauri::ipc::CallbackFn(0),
            error: tauri::ipc::CallbackFn(1),
            url: "http://tauri.localhost".parse().unwrap(),
            body: tauri::ipc::InvokeBody::Json(args),
            headers: Default::default(),
            invoke_key: INVOKE_KEY.to_string(),
        },
    );
    match res {
        Ok(body) => Ok(body.deserialize::<serde_json::Value>().expect("回包不是 JSON")),
        Err(e) => Err(e.to_string()),
    }
}

fn dts_default_graph() -> serde_json::Value {
    let path = repo_root().join("packs/dts/graphs/default.lyflow.json");
    let text = std::fs::read_to_string(&path)
        .unwrap_or_else(|e| panic!("读不到 {}: {e}", path.display()));
    serde_json::from_str(&text).expect("default.lyflow.json 不是合法 JSON")
}

/// 等到本次 run 的 `run_finished` 出现。超时就把收到的事件打出来 ——
/// 「测试卡住了」是最没有信息量的失败。
fn wait_for_finish(events: &Events, run_id: &str) -> serde_json::Value {
    let deadline = Instant::now() + Duration::from_secs(30);
    loop {
        {
            let seen = events.lock().unwrap();
            if let Some(e) = seen
                .iter()
                .find(|e| e["kind"] == "run_finished" && e["runId"] == run_id)
            {
                return e.clone();
            }
            if Instant::now() > deadline {
                panic!("等 run {run_id} 结束超时，收到的事件：{seen:#?}");
            }
        }
        std::thread::sleep(Duration::from_millis(20));
    }
}

fn events_of<'a>(
    all: &'a [serde_json::Value],
    run_id: &str,
) -> Vec<&'a serde_json::Value> {
    all.iter().filter(|e| e["runId"] == run_id).collect()
}

// ------------------------------------------------------------------ 冒烟

/// `lyflow_handler!` 跨 crate 挂得上，两条只读命令走得通。
#[test]
fn manifest_and_core_info_over_ipc() {
    let f = fixture();
    let view = webview(&f.app);

    let manifest = invoke(&view, "get_manifest", serde_json::json!({})).expect("get_manifest 失败");
    assert_eq!(manifest["schemaVersion"], 1);
    let ops = manifest["operators"].as_array().expect("没有 operators");

    let info = invoke(&view, "get_core_info", serde_json::json!({})).expect("get_core_info 失败");
    assert_eq!(info["operatorCount"].as_u64().unwrap() as usize, ops.len());
    assert!(!info["version"].as_str().unwrap().is_empty());
}

// ------------------------------------------------------------------ 注入

/// 端到端：宿主注入一条轮廓 → 图跑完 → 图级命名输出里有 flush，
/// 且 `dts.profile_in` 是被注入的（compute 整个没跑）。
#[test]
#[cfg_attr(not(dts_pack), ignore = "要 dts 包：LYFLOW_PACKS=dts")]
fn run_graph_injects_the_host_profile() {
    let f = fixture();
    let view = webview(&f.app);
    let doc = dts_default_graph();

    let run_id = invoke(
        &view,
        "run_graph",
        serde_json::json!({
            "doc": doc,
            "graphPath": null,
            "targets": null,
            "mode": "full",
            "previewMaxPoints": null,
            "previewBudgetMs": null,
            "sceneId": "s1",
        }),
    )
    .expect("run_graph 失败");
    let run_id = run_id.as_str().expect("run_graph 没返回 runId").to_string();

    wait_for_finish(&f.events, &run_id);

    // 注入命中的直接证据：节点事件上的 stats.provided（ADR-0017）。
    let all = f.events.lock().unwrap().clone();
    let provided = events_of(&all, &run_id).into_iter().any(|e| {
        e["kind"] == "node_state" && e["nodeId"] == "n_in" && e["stats"]["provided"] == true
    });
    assert!(
        provided,
        "n_in 上没有 stats.provided=true，注入没生效：{:#?}",
        events_of(&all, &run_id)
    );

    // 图级命名输出的键集合只由图决定，不由运行结果决定（ADR-0017）——
    // 所以 flush 一定在，missing 与否不在本测试的关心范围内。
    let outputs = invoke(&view, "get_run_outputs", serde_json::json!({ "runId": run_id }))
        .expect("get_run_outputs 失败");
    assert!(
        outputs.get("flush").is_some(),
        "图输出里没有 flush: {outputs:#?}"
    );
}

/// 换一片云，`n_in` 的 cacheKey 必须变 —— 不变的表现是「产线上两台车量出一模一样的值」。
#[test]
#[cfg_attr(not(dts_pack), ignore = "要 dts 包：LYFLOW_PACKS=dts")]
fn injected_data_changes_the_cache_key() {
    let f = fixture();
    let view = webview(&f.app);
    let doc = dts_default_graph();

    let mut keys = Vec::new();
    for scene in ["s1", "s2"] {
        let run_id = invoke(
            &view,
            "run_graph",
            serde_json::json!({
                "doc": doc, "graphPath": null, "targets": null, "mode": "full",
                "previewMaxPoints": null, "previewBudgetMs": null, "sceneId": scene,
            }),
        )
        .expect("run_graph 失败");
        let run_id = run_id.as_str().unwrap().to_string();
        wait_for_finish(&f.events, &run_id);

        let all = f.events.lock().unwrap().clone();
        let started = events_of(&all, &run_id)
            .into_iter()
            .find(|e| e["kind"] == "run_started")
            .expect("没收到 run_started")
            .clone();
        let key = started["nodes"]
            .as_array()
            .expect("run_started 没有 nodes")
            .iter()
            .find(|n| n["id"] == "n_in")
            .expect("计划里没有 n_in")["cacheKey"]
            .as_str()
            .expect("cacheKey 不是字符串")
            .to_string();
        keys.push(key);
    }
    assert_ne!(keys[0], keys[1], "换了一片云，n_in 的 cacheKey 却没变");
}

/// 会话不存在时的报错要能原样交给人看。
#[test]
fn unknown_scene_id_reports_the_hosts_message() {
    let f = fixture();
    let view = webview(&f.app);
    let err = invoke(
        &view,
        "run_graph",
        serde_json::json!({
            "doc": dts_default_graph(), "graphPath": null, "targets": null, "mode": "full",
            "previewMaxPoints": null, "previewBudgetMs": null, "sceneId": "不存在的会话",
        }),
    )
    .unwrap_err();
    assert!(err.contains("点云会话已失效"), "{err}");
}

/// 没配 SceneProvider 的宿主带着 sceneId 来，要直接报错而不是跑一张没注入的图。
#[test]
fn scene_id_without_a_provider_is_an_error() {
    let app = host::attach(
        mock_builder(),
        HostConfig {
            library_dirs: Some(Vec::new()),
            hot_reload: false,
            ..HostConfig::default()
        },
    )
    .invoke_handler(lyflow_lib::lyflow_handler![])
    .build(mock_context(noop_assets()))
    .expect("建 mock app 失败");
    let view = webview(&app);

    let err = invoke(
        &view,
        "run_graph",
        serde_json::json!({
            "doc": dts_default_graph(), "graphPath": null, "targets": null, "mode": "full",
            "previewMaxPoints": null, "previewBudgetMs": null, "sceneId": "s1",
        }),
    )
    .unwrap_err();
    assert!(err.contains("不支持点云注入"), "{err}");
}

// ------------------------------------------------------------------ 路径

#[test]
fn resolve_path_without_a_workspace_root_is_a_passthrough() {
    let cfg = HostConfig::default();
    assert_eq!(
        host::resolve_path(&cfg, "任意/相对/路径.json").unwrap(),
        PathBuf::from("任意/相对/路径.json")
    );
}

#[test]
fn resolve_path_joins_relative_paths_onto_the_root() {
    let cfg = HostConfig {
        workspace_root: Some(PathBuf::from(root_str())),
        ..HostConfig::default()
    };
    let got = host::resolve_path(&cfg, "graphs/a.lyflow.json").unwrap();
    assert_eq!(got, Path::new(root_str()).join("graphs").join("a.lyflow.json"));
    // `.` 与中间的 `..` 只要没逃出去就正常解析
    let got = host::resolve_path(&cfg, "./graphs/../a.json").unwrap();
    assert_eq!(got, Path::new(root_str()).join("a.json"));
}

#[test]
fn resolve_path_rejects_escapes() {
    let cfg = HostConfig {
        workspace_root: Some(PathBuf::from(root_str())),
        ..HostConfig::default()
    };
    let err = host::resolve_path(&cfg, "../外面/a.json").unwrap_err();
    assert!(err.contains("逃出了工作区"), "{err}");
    let err = host::resolve_path(&cfg, "graphs/../../a.json").unwrap_err();
    assert!(err.contains("逃出了工作区"), "{err}");
    // 带盘符的「相对」路径（`D:x`）在 join 时会把根整个换掉，同样要被拦住
    #[cfg(windows)]
    {
        let err = host::resolve_path(&cfg, "D:别的盘.json").unwrap_err();
        assert!(err.contains("逃出了工作区"), "{err}");
    }
}

#[test]
fn resolve_path_lets_absolute_paths_through() {
    let cfg = HostConfig {
        workspace_root: Some(PathBuf::from(root_str())),
        ..HostConfig::default()
    };
    let abs = std::env::temp_dir().join("lyflow-host-abs.json");
    let got = host::resolve_path(&cfg, &abs.to_string_lossy()).unwrap();
    assert_eq!(got, abs);
}

fn root_str() -> &'static str {
    if cfg!(windows) {
        "C:/lyflow-ws"
    } else {
        "/lyflow-ws"
    }
}

// ------------------------------------------------------------------ 注入帮手

/// 只有 `inputs_for_source_op` 用得着的一份最小 manifest。
fn tiny_manifest() -> serde_json::Value {
    serde_json::json!({
        "operators": [
            {"id": "src.one", "outputs": [{"name": "profile", "type": "PointCloud"}]},
            {"id": "src.two", "outputs": [
                {"name": "primary", "type": "PointCloud"},
                {"name": "secondary", "type": "PointCloud"}
            ]}
        ]
    })
}

fn graph_of(nodes: &[(&str, &str)]) -> GraphDoc {
    let nodes: Vec<serde_json::Value> = nodes
        .iter()
        .map(|(id, op)| serde_json::json!({"id": id, "op": op}))
        .collect();
    serde_json::from_value(serde_json::json!({
        "schemaVersion": 1, "id": "01J8XQZ4K7N3M2R5V8W1YB6TCD",
        "nodes": nodes, "edges": []
    }))
    .unwrap()
}

fn one_cloud(port: &str) -> BTreeMap<String, Cloud> {
    let mut m = BTreeMap::new();
    m.insert(port.to_string(), synthetic_profile(1.0));
    m
}

#[test]
fn inputs_for_source_op_finds_the_single_source_node() {
    let g = graph_of(&[("a", "src.one"), ("b", "dts.profile_clean")]);
    let got =
        host::inputs_for_source_op(&g, &tiny_manifest(), "src.one", &one_cloud("profile")).unwrap();
    assert_eq!(got.len(), 1);
    assert_eq!(got[0].node_id, "a");
    assert_eq!(got[0].port, "profile");
    assert_eq!(got[0].xyz.len(), 400 * 3);
    assert_eq!(got[0].intensity.len(), 400);
}

#[test]
fn inputs_for_source_op_rejects_a_graph_without_the_source() {
    let g = graph_of(&[("b", "dts.profile_clean")]);
    let err = host::inputs_for_source_op(&g, &tiny_manifest(), "src.one", &one_cloud("profile"))
        .unwrap_err();
    assert!(err.contains("图里没有 src.one 节点"), "{err}");
}

#[test]
fn inputs_for_source_op_rejects_an_ambiguous_graph() {
    let g = graph_of(&[("a", "src.one"), ("a2", "src.one")]);
    let err = host::inputs_for_source_op(&g, &tiny_manifest(), "src.one", &one_cloud("profile"))
        .unwrap_err();
    assert!(err.contains("不知道该注入哪一个"), "{err}");
}

/// ADR-0017：注入是整节点级的，声明的每个输出端口都要给。
#[test]
fn inputs_for_source_op_rejects_a_missing_port() {
    let g = graph_of(&[("a", "src.two")]);
    let err = host::inputs_for_source_op(&g, &tiny_manifest(), "src.two", &one_cloud("primary"))
        .unwrap_err();
    assert!(err.contains("secondary"), "{err}");
}
