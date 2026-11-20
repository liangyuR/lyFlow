use lyflow_lib::core_ffi::{core, generation, reload_from, RunInput};
use std::sync::Arc;

#[test]
fn hot_reload_swaps_in_a_fresh_generation() {
    let before = generation();
    let old = core().expect("加载 core 失败");
    let old_ops = serde_json::from_str::<serde_json::Value>(&old.manifest_json().unwrap()).unwrap()
        ["operators"]
        .as_array()
        .unwrap()
        .len();

    // 源就用当前这份 DLL：本测试要验的是换代机制，不是「新代码生效了没有」
    let dll = std::env::current_exe()
        .unwrap()
        .with_file_name("lyflow_core.dll");
    let generation_id = reload_from(&dll).expect("热重载失败");
    assert_eq!(generation_id, before.wrapping_add(1));
    assert_eq!(generation(), generation_id);

    let fresh = core().expect("换代后拿不到 core");
    assert!(!Arc::ptr_eq(&old, &fresh), "还是旧的那一份");
    let fresh_ops = serde_json::from_str::<serde_json::Value>(&fresh.manifest_json().unwrap())
        .unwrap()["operators"]
        .as_array()
        .unwrap()
        .len();
    assert_eq!(fresh_ops, old_ops);

    // 新一代真的能干活：编译一张图并拿到 cacheKey
    let doc = serde_json::json!({
        "schemaVersion": 1, "id": "t",
        "nodes": [{"id": "g", "op": "util.reroute"}], "edges": []
    });
    let input = RunInput {
        node_id: "g".into(),
        port: "in".into(),
        xyz: vec![0.0, 0.0, 0.0],
        intensity: vec![],
        normals: vec![],
        rgb: vec![],
    };
    let plan: serde_json::Value = serde_json::from_str(
        &fresh
            .plan_with_inputs(&doc.to_string(), "", &[], None, &[input], &[])
            .unwrap(),
    )
    .unwrap();
    assert_eq!(plan[0]["nodeId"], "g");
    assert_eq!(plan[0]["cacheKey"].as_str().unwrap().len(), 32);

    // 旧代的 gen DLL 删不掉是常态（自己还锁着），所以清理只在启动时做一次
    drop(old);
}
