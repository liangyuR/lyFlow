use lyflow_lib::core_ffi::{self, RunHandle, RunProfileInput, RunSpec};
use lyflow_lib::profile_wire::encode_profile;
use serde_json::{json, Value};
use std::ffi::{c_char, c_void};

unsafe extern "C" fn event(_: *const c_char, _: *mut c_void) {}

#[test]
fn retained_profiles_cross_rust_abi_cache_frames_and_binary_transport_without_losing_slots() {
    let core = core_ffi::core().unwrap();
    assert!(core.supports_profiles());
    let mut input = RunProfileInput {
        node_id: "p".into(), port: "out".into(),
        xyz: vec![0.0, 0.0, 1.0, f32::NAN, f32::NAN, f32::NAN, 3.0, 0.0, 4.0],
        intensity: vec![1.0, 0.0, 2.0], normals: vec![],
        rgb: vec![255, 0, 0, 0, 255, 0, 0, 0, 255],
        descriptor_json: json!({"schemaVersion":1,"frame":"sensor","unit":"mm","plane":"XZ","handedness":"right","sampleOrder":"acquisition","sourceId":"scan-1","sourceCount":4}).to_string(),
        validity: vec![1, 0, 1], source_indices: vec![0, 2, 3], retained: None,
    };
    let retained = core.retain_profile_input(&input).unwrap();
    input.xyz.clear();
    input.validity.clear();
    input.source_indices.clear();
    input.retained = Some(retained);
    let graph = json!({"schemaVersion":1,"id":"rust-profiles","name":"Rust profiles","nodes":[{"id":"p","op":"util.reroute","params":{}},{"id":"q","op":"util.reroute","params":{}}],"edges":[{"id":"e","from":{"node":"p","port":"out"},"to":{"node":"q","port":"in"}}]}).to_string();
    let inputs = [input];
    let mut spec = RunSpec::new(&graph, "rust-profiles", "", &[]);
    spec.profile_inputs = &inputs;
    let run = unsafe { RunHandle::start(core.clone(), spec, event, Box::new(())) }.unwrap();
    run.join();
    let view = core.output_profile(run.run_id(), "q", "out", 0, 0).unwrap();
    assert_eq!(view.point_count(), 3);
    assert_eq!(view.xyz()[0], 0.0);
    assert!(view.xyz()[3].is_nan());
    assert_eq!(view.validity(), &[1, 0, 1]);
    assert_eq!(view.source_indices(), &[0, 2, 3]);
    let bytes = encode_profile(&view).unwrap();
    let word =
        |index: usize| u32::from_le_bytes(bytes[index * 4..index * 4 + 4].try_into().unwrap());
    assert_eq!(&bytes[..4], b"LYPR");
    assert_eq!(
        [word(1), word(2), word(3), word(4), word(5)],
        [1, 3, 3, 0, 5]
    );
    let envelope: Value = serde_json::from_slice(&bytes[32..32 + word(6) as usize]).unwrap();
    assert_eq!(envelope["descriptor"]["sourceId"], "scan-1");
    assert_eq!(envelope["cloudId"], view.cloud_id().to_string());
    assert_eq!(
        &bytes[bytes.len() - 12..],
        &[0, 0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0]
    );
    let plan: Value = serde_json::from_str(
        &core
            .plan_with_profiles(&graph, "", &[], None, &[], &[], &inputs)
            .unwrap(),
    )
    .unwrap();
    assert!(plan
        .as_array()
        .unwrap()
        .iter()
        .any(|n| n["nodeId"] == "q" && n["cached"] == true));
    let effective: Value = serde_json::from_str(
        &core
            .effective_params_with_profiles(&graph, "", None, &[], &[], &inputs)
            .unwrap(),
    )
    .unwrap();
    assert!(effective.is_object(), "{effective}");
    let metadata = json!({"frameId":"profile-frame","timestampNs":"100","frame":"sensor","calibrationVersion":"test","context":{}}).to_string();
    let frame = core.capture_frame(run.run_id(), &metadata, "{}").unwrap();
    let captured = frame.profile_input("q", "out").unwrap();
    let framed = frame.profile("q", "out", 0, 0).unwrap();
    assert_eq!(framed.cloud_id(), view.cloud_id());
    drop(frame);
    drop(run);
    let mut captured_inputs = inputs.clone();
    captured_inputs[0].retained = Some(captured);
    let evicted: Value = serde_json::from_str(
        &core
            .cache_evict_with_profiles(
                &graph,
                "",
                &["q".into()],
                false,
                None,
                &[],
                &[],
                &captured_inputs,
            )
            .unwrap(),
    )
    .unwrap();
    assert_eq!(evicted["removed"], 1);
    assert_eq!(view.source_indices(), &[0, 2, 3]);

    let boundary_descriptor = json!({"schemaVersion":1,"frame":"\u{1}".repeat(670),"unit":"m","plane":"XZ","handedness":"right","sampleOrder":"acquisition","sourceId":"\u{1}".repeat(670),"sourceCount":0});
    let mut boundary = RunProfileInput {
        node_id: "p".into(),
        port: "out".into(),
        xyz: vec![],
        intensity: vec![],
        normals: vec![],
        rgb: vec![],
        descriptor_json: boundary_descriptor.to_string(),
        validity: vec![],
        source_indices: vec![],
        retained: None,
    };
    assert_eq!(boundary.descriptor_json.len(), 8173);
    boundary.retained = Some(core.retain_profile_input(&boundary).unwrap());
    let boundary_inputs = [boundary.clone()];
    let mut boundary_spec = RunSpec::new(&graph, "rust-profile-envelope-boundary", "", &[]);
    boundary_spec.profile_inputs = &boundary_inputs;
    let boundary_run =
        unsafe { RunHandle::start(core.clone(), boundary_spec, event, Box::new(())) }.unwrap();
    boundary_run.join();
    let boundary_view = core
        .output_profile(boundary_run.run_id(), "q", "out", 0, 0)
        .unwrap();
    let boundary_wire = encode_profile(&boundary_view).unwrap();
    let metadata_size = u32::from_le_bytes(boundary_wire[24..28].try_into().unwrap()) as usize;
    assert!(metadata_size > 8192 && metadata_size <= 16384);
    let encoded_metadata: Value =
        serde_json::from_slice(&boundary_wire[32..32 + metadata_size]).unwrap();
    assert_eq!(encoded_metadata["descriptor"], boundary_descriptor);
    assert_eq!(
        encoded_metadata["cloudId"],
        boundary_view.cloud_id().to_string()
    );
    boundary.retained = None;
    let mut oversized_descriptor = boundary_descriptor;
    oversized_descriptor["sourceId"] = json!("\u{1}".repeat(674));
    boundary.descriptor_json = oversized_descriptor.to_string();
    assert!(boundary.descriptor_json.len() > 8192);
    assert!(core.retain_profile_input(&boundary).is_err());
}

#[test]
fn cli_dump_profile_preserves_source_slots_and_descriptor() {
    let dir = std::env::temp_dir().join(format!("lyflow-profile-cli-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let graph = json!({"schemaVersion":1,"id":"cli-profile","name":"CLI Profile","nodes":[{"id":"source","op":"util.reroute","params":{}},{"id":"profile","op":"profile.from_cloud","params":{"frame":"sensor","unit":"mm","plane":"XZ","handedness":"right","sampleOrder":"acquisition","sourceId":"cli-source"}}],"edges":[{"id":"e","from":{"node":"source","port":"out"},"to":{"node":"profile","port":"cloud"}}]}).to_string();
    let graph_path = dir.join("graph.lyflow.json");
    let cloud_path = dir.join("input.pcd");
    let output_path = dir.join("profile.lypr");
    std::fs::write(&graph_path, graph).unwrap();
    std::fs::write(&cloud_path, "VERSION .7\nFIELDS x y z\nSIZE 4 4 4\nTYPE F F F\nCOUNT 1 1 1\nWIDTH 3\nHEIGHT 1\nPOINTS 3\nDATA ascii\n0 0 1\nnan nan nan\n3 0 4\n").unwrap();
    let output = std::process::Command::new(env!("CARGO_BIN_EXE_lyflow"))
        .arg("dump")
        .arg(&graph_path)
        .arg("profile:profile")
        .arg(&output_path)
        .arg("--format")
        .arg("profile")
        .arg("--input")
        .arg(format!("source.out={}", cloud_path.display()))
        .env_remove("LYFLOW_CACHE_DIR")
        .output()
        .unwrap();
    assert!(
        output.status.success(),
        "{}\n{}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
    let bytes = std::fs::read(&output_path).unwrap();
    assert_eq!(&bytes[..4], b"LYPR");
    let metadata_size = u32::from_le_bytes(bytes[24..28].try_into().unwrap()) as usize;
    let metadata: Value = serde_json::from_slice(&bytes[32..32 + metadata_size]).unwrap();
    assert_eq!(metadata["descriptor"]["sourceCount"], 3);
    assert_eq!(metadata["descriptor"]["unit"], "mm");
    let xyz_start = (32 + metadata_size + 3) & !3;
    assert!(f32::from_le_bytes(bytes[xyz_start + 12..xyz_start + 16].try_into().unwrap()).is_nan());
    assert_eq!(&bytes[xyz_start + 36..xyz_start + 39], &[1, 0, 1]);
    assert_eq!(
        &bytes[bytes.len() - 12..],
        &[0, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0]
    );
    let snapshot_dir = dir.join("snapshot");
    let captured = std::process::Command::new(env!("CARGO_BIN_EXE_lyflow"))
        .arg("run")
        .arg(&graph_path)
        .arg("--snapshot-dir")
        .arg(&snapshot_dir)
        .arg("--capture-policy")
        .arg("all")
        .arg("--input")
        .arg(format!("source.out={}", cloud_path.display()))
        .env_remove("LYFLOW_CACHE_DIR")
        .output()
        .unwrap();
    assert!(
        captured.status.success(),
        "{}\n{}",
        String::from_utf8_lossy(&captured.stdout),
        String::from_utf8_lossy(&captured.stderr)
    );
    let snapshot: Value =
        serde_json::from_slice(&std::fs::read(snapshot_dir.join("snapshot.json")).unwrap())
            .unwrap();
    let profile = snapshot["outputs"]
        .as_array()
        .unwrap()
        .iter()
        .find(|output| output["type"] == "Profile")
        .unwrap();
    let captured_bytes =
        std::fs::read(snapshot_dir.join(profile["file"].as_str().unwrap())).unwrap();
    assert_eq!(&captured_bytes[..4], b"LYPR");
    assert_eq!(
        &captured_bytes[captured_bytes.len() - 12..],
        &[0, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0]
    );
    let _ = std::fs::remove_dir_all(dir);
}

#[test]
fn cli_preserves_contract_upgrade_until_native_validation() {
    let path = std::env::temp_dir().join(format!(
        "lyflow-upgrade-forwarding-{}.lyflow.json",
        std::process::id()
    ));
    for request in [
        serde_json::json!({"schemaVersion":999,"rules":[]}),
        Value::Null,
        serde_json::json!([]),
    ] {
        let graph = serde_json::json!({"schemaVersion":1,"id":"upgrade-forwarding","nodes":[{"id":"n","op":"value.number","params":{}}],"edges":[],"contractUpgrade":request});
        std::fs::write(&path, graph.to_string()).unwrap();
        let result = std::process::Command::new(env!("CARGO_BIN_EXE_lyflow"))
            .arg("plan")
            .arg(&path)
            .env_remove("LYFLOW_CACHE_DIR")
            .output()
            .unwrap();
        assert!(
            !result.status.success(),
            "{}\n{}",
            String::from_utf8_lossy(&result.stdout),
            String::from_utf8_lossy(&result.stderr)
        );
        let diagnostics: Value = serde_json::from_slice(&result.stdout).unwrap();
        assert_eq!(diagnostics[0]["code"], "bad_input");
    }
    let mut graph: Value = serde_json::from_str(include_str!(
        "../../core/tests/fixtures/contract-upgrade/dts-mm.lyflow.json"
    ))
    .unwrap();
    let request = graph["contractUpgrade"].clone();
    let migrate = |document: &Value| {
        std::fs::write(&path, document.to_string()).unwrap();
        let result = std::process::Command::new(env!("CARGO_BIN_EXE_lyflow"))
            .arg("migrate")
            .arg(&path)
            .arg("--write")
            .env_remove("LYFLOW_CACHE_DIR")
            .output()
            .unwrap();
        assert!(
            result.status.success(),
            "{}",
            String::from_utf8_lossy(&result.stderr)
        );
        serde_json::from_slice::<Value>(&result.stdout).unwrap()
    };
    assert_eq!(migrate(&graph)["migrations"].as_array().unwrap().len(), 1);
    graph = serde_json::from_slice(&std::fs::read(&path).unwrap()).unwrap();
    assert!(graph.get("contractUpgrade").is_none());
    assert_eq!(
        graph["meta"]["contractUpgradeHistory"][0]["request"],
        request
    );
    assert!(graph["edges"]
        .as_array()
        .unwrap()
        .iter()
        .any(|edge| edge["id"] == "metric-edge"));
    graph["nodes"]
        .as_array_mut()
        .unwrap()
        .retain(|node| node["id"] != "metric");
    graph["edges"]
        .as_array_mut()
        .unwrap()
        .retain(|edge| edge["from"]["node"] != "metric" && edge["to"]["node"] != "metric");
    assert!(migrate(&graph)["migrations"].as_array().unwrap().is_empty());
    graph["subgraphs"] = serde_json::json!({"inside":{"nodes":graph["nodes"],"edges":graph["edges"],"inputs":[],"outputs":[],"params":[]}});
    graph["nodes"] = serde_json::json!([{"id":"group","op":"sub:inside","params":{}}]);
    graph["edges"] = serde_json::json!([]);
    assert!(migrate(&graph)["migrations"].as_array().unwrap().is_empty());
    let _ = std::fs::remove_file(path);
}
