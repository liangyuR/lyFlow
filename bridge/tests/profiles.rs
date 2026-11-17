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
fn cli_requires_offline_migration_for_contract_upgrade_requests() {
    let dir = std::env::temp_dir().join(format!("lyflow-upgrade-offline-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let path = dir.join("graph.lyflow.json");
    let lyflow = |args: &[&str]| {
        std::process::Command::new(env!("CARGO_BIN_EXE_lyflow"))
            .args(args)
            .env_remove("LYFLOW_CACHE_DIR")
            .output()
            .unwrap()
    };
    let stdout = |result: &std::process::Output| -> Value {
        let text = String::from_utf8_lossy(&result.stdout);
        serde_json::from_str(text.lines().next().unwrap_or_default())
            .unwrap_or_else(|_| panic!("{text}\n{}", String::from_utf8_lossy(&result.stderr)))
    };
    let graph_arg = path.to_string_lossy().into_owned();
    for request in [
        json!({"schemaVersion":999,"rules":[]}),
        Value::Null,
        json!([]),
    ] {
        let graph = json!({"schemaVersion":1,"id":"upgrade-forwarding","nodes":[{"id":"n","op":"value.number","params":{}}],"edges":[],"contractUpgrade":request});
        let before = graph.to_string();
        std::fs::write(&path, &before).unwrap();
        for command in ["plan", "validate"] {
            let result = lyflow(&[command, &graph_arg]);
            assert!(!result.status.success(), "{command} accepted {request}");
            let diagnostics = stdout(&result);
            assert!(
                diagnostics.as_array().unwrap().iter().any(|d| {
                    d["code"] == "migration_required" && d["nodeId"].as_str() == Some("")
                }),
                "{command}: {diagnostics}"
            );
        }
        let migrated = lyflow(&["migrate", &graph_arg, "--write"]);
        assert!(!migrated.status.success(), "migrate accepted {request}");
        assert!(stdout(&migrated)["diagnostics"]
            .as_array()
            .unwrap()
            .iter()
            .any(|d| d["severity"] == "error"));
        assert_eq!(std::fs::read_to_string(&path).unwrap(), before);
    }

    // dts-mm 自带的 dtsGeometry 请求能执行，但旧 DTS 节点还缺 dtsProfile.v1 证据：整体失败，文件不动
    let dts_mm = std::fs::read_to_string(
        std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../solutions/dts/tests/fixtures/dts-mm.lyflow.json"),
    )
    .unwrap();
    std::fs::write(&path, &dts_mm).unwrap();
    let result = lyflow(&["migrate", &graph_arg, "--write"]);
    assert!(!result.status.success());
    assert_eq!(std::fs::read_to_string(&path).unwrap(), dts_mm);
    assert!(stdout(&result)["diagnostics"]
        .as_array()
        .unwrap()
        .iter()
        .any(|d| d["code"] == "migration_required"
            && d["message"]
                .as_str()
                .is_some_and(|m| m.contains("dtsProfile.v1"))));

    // 带着证据的显式请求：迁移后不再有 contractUpgrade，也不写任何迁移历史
    let profile = std::fs::read_to_string(
        std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../solutions/dts/tests/fixtures/dts-profile-v1.lyflow.json"),
    )
    .unwrap();
    std::fs::write(&path, &profile).unwrap();
    let request_path = dir.join("request.json");
    std::fs::write(
        &request_path,
        json!({"schemaVersion":1,"rules":[{"kind":"dtsProfile.v1",
            "source":{"node":"n_in","op":"dts.profile_in","opVersion":"1.0.0","port":"profile"},
            "profile":{"frame":"scan","sourceId":"scanner","unit":"mm","plane":"XZ","handedness":"right","sampleOrder":"acquisition"},
            "adapterId":"n_in_profile_contract"}]})
        .to_string(),
    )
    .unwrap();
    let request_arg = request_path.to_string_lossy().into_owned();
    let preview = lyflow(&["migrate", &graph_arg, "--request", &request_arg]);
    assert!(
        preview.status.success(),
        "{}",
        String::from_utf8_lossy(&preview.stderr)
    );
    assert_eq!(stdout(&preview)["graphMigrated"], true);
    assert_eq!(std::fs::read_to_string(&path).unwrap(), profile);
    let written = lyflow(&["migrate", &graph_arg, "--request", &request_arg, "--write"]);
    assert!(
        written.status.success(),
        "{}",
        String::from_utf8_lossy(&written.stderr)
    );
    let graph: Value = serde_json::from_str(&std::fs::read_to_string(&path).unwrap()).unwrap();
    assert!(graph.get("contractUpgrade").is_none());
    assert!(graph["meta"].get("contractUpgradeHistory").is_none());
    let validated = lyflow(&["validate", &graph_arg]);
    assert!(
        validated.status.success(),
        "{}\n{}",
        String::from_utf8_lossy(&validated.stdout),
        String::from_utf8_lossy(&validated.stderr)
    );
    for command in ["plan", "validate"] {
        let result = lyflow(&[command, &graph_arg]);
        let text = String::from_utf8_lossy(&result.stdout);
        assert!(!text.contains("migration_required"), "{command}: {text}");
        assert!(
            !text.contains("\"kind\":\"migration\""),
            "{command}: {text}"
        );
    }
    let _ = std::fs::remove_dir_all(dir);
}

fn lypr_fixture() -> Vec<u8> {
    let metadata = json!({"descriptor":{"schemaVersion":1,"frame":"fixture-sensor","unit":"mm","plane":"XZ","handedness":"reflected","sampleOrder":"acquisition","sourceId":"fixture-source","sourceCount":9},"cloudId":"18446744073709551615"}).to_string();
    let mut out = Vec::new();
    for value in [0x5250594cu32, 1, 3, 3, 0, 7, metadata.len() as u32, 0] {
        out.extend_from_slice(&value.to_le_bytes());
    }
    out.extend_from_slice(metadata.as_bytes());
    out.resize((out.len() + 3) & !3, 0);
    for value in [
        1.0f32,
        2.0,
        3.0,
        f32::from_bits(0x7fc01234),
        -0.0,
        f32::NAN,
        4.0,
        5.0,
        6.0,
        7.0,
        8.0,
        9.0,
        0.0,
        0.0,
        1.0,
        0.0,
        1.0,
        0.0,
        1.0,
        0.0,
        0.0,
    ] {
        out.extend_from_slice(&value.to_le_bytes());
    }
    out.extend_from_slice(&[1, 2, 3, 4, 5, 6, 7, 8, 9]);
    out.resize((out.len() + 3) & !3, 0);
    out.extend_from_slice(&[1, 0, 0]);
    out.resize((out.len() + 3) & !3, 0);
    for value in [1u32, 4, 8] {
        out.extend_from_slice(&value.to_le_bytes());
    }
    out
}

#[test]
fn lypr_client_reuses_native_validation_and_preserves_all_slot_bits() {
    let core = core_ffi::core().unwrap();
    let bytes = lypr_fixture();
    let input = core
        .profile_input_from_lypr(&bytes, "p", "out", bytes.len())
        .unwrap();
    assert_eq!(input.xyz[3].to_bits(), 0x7fc01234);
    assert_eq!(input.xyz[4].to_bits(), (-0.0f32).to_bits());
    assert_eq!(input.validity, [1, 0, 0]);
    assert_eq!(input.source_indices, [1, 4, 8]);
    assert_eq!(input.intensity, [7.0, 8.0, 9.0]);
    assert_eq!(input.normals.len(), 9);
    assert_eq!(input.rgb, [1, 2, 3, 4, 5, 6, 7, 8, 9]);
    assert!(input.retained.is_some());
    let graph = json!({"schemaVersion":1,"id":"decode-profile","nodes":[{"id":"p","op":"util.reroute","params":{}},{"id":"q","op":"util.reroute","params":{}}],"edges":[{"id":"e","from":{"node":"p","port":"out"},"to":{"node":"q","port":"in"}}]}).to_string();
    let inputs = [input];
    let mut spec = RunSpec::new(&graph, "decode-profile", "", &[]);
    spec.profile_inputs = &inputs;
    let run = unsafe { RunHandle::start(core.clone(), spec, event, Box::new(())) }.unwrap();
    run.join();
    let view = core.output_profile(run.run_id(), "q", "out", 0, 0).unwrap();
    assert_eq!(view.flags(), 7);
    for (actual, expected) in view.xyz().iter().zip(&inputs[0].xyz) {
        assert_eq!(actual.to_bits(), expected.to_bits());
    }
    assert_eq!(view.validity(), [1, 0, 0]);
    assert_eq!(view.source_indices(), [1, 4, 8]);
    assert_eq!(view.intensity(), inputs[0].intensity);
    assert_eq!(view.normals(), inputs[0].normals);
    assert_eq!(view.rgb(), inputs[0].rgb);
    assert_eq!(
        serde_json::from_str::<Value>(view.descriptor_json()).unwrap(),
        serde_json::from_str::<Value>(&inputs[0].descriptor_json).unwrap()
    );
    let metadata_size = u32::from_le_bytes(bytes[24..28].try_into().unwrap()) as usize;
    let start = (32 + metadata_size + 3) & !3;
    let mut cases = Vec::new();
    for (word, value) in [
        (0, 0),
        (1, 2),
        (3, 4),
        (4, 1),
        (5, 8),
        (6, 16385),
        (7, 1),
        (2, u32::MAX),
    ] {
        let mut bad = bytes.clone();
        bad[word * 4..word * 4 + 4].copy_from_slice(&u32::to_le_bytes(value));
        cases.push(bad);
    }
    cases.push(bytes[..31].to_vec());
    cases.push(bytes[..bytes.len() - 1].to_vec());
    let mut trailing = bytes.clone();
    trailing.push(0);
    cases.push(trailing);
    for position in [start + 84 + 9, start + 96 + 3] {
        let mut bad = bytes.clone();
        bad[position] = 1;
        cases.push(bad);
    }
    let mut invalid_nan = bytes.clone();
    invalid_nan[start + 96 + 1] = 1;
    cases.push(invalid_nan);
    let mut invalid_mask = bytes.clone();
    invalid_mask[start + 96 + 2] = 2;
    cases.push(invalid_mask);
    let mut invalid_inf = bytes.clone();
    invalid_inf[start + 12..start + 16].copy_from_slice(&f32::INFINITY.to_le_bytes());
    cases.push(invalid_inf);
    for index in [1u32, 9] {
        let mut bad = bytes.clone();
        let end = bad.len();
        bad[end - 4..].copy_from_slice(&index.to_le_bytes());
        cases.push(bad);
    }
    let envelope: Value = serde_json::from_slice(&bytes[32..32 + metadata_size]).unwrap();
    let replace_metadata = |text: &str| {
        let mut changed = bytes[..32].to_vec();
        changed[24..28].copy_from_slice(&(text.len() as u32).to_le_bytes());
        changed.extend_from_slice(text.as_bytes());
        changed.resize((changed.len() + 3) & !3, 0);
        changed.extend_from_slice(&bytes[start..]);
        changed
    };
    for (key, value) in [
        ("unit", json!("cm")),
        ("sourceCount", json!(2)),
        ("sourceCount", json!(9.0)),
        ("sourceCount", json!(4294967296u64)),
        ("schemaVersion", json!(1.0)),
        ("frame", json!("")),
        ("sourceId", json!("a\0b")),
        ("sampleOrder", json!("guessed")),
        ("plane", json!("AB")),
        ("handedness", json!("left")),
    ] {
        let mut changed = envelope.clone();
        changed["descriptor"][key] = value;
        cases.push(replace_metadata(&changed.to_string()));
    }
    let mut unknown = envelope.clone();
    unknown["unexpected"] = json!(1);
    cases.push(replace_metadata(&unknown.to_string()));
    let mut unknown = envelope.clone();
    unknown["descriptor"]["unexpected"] = json!(1);
    cases.push(replace_metadata(&unknown.to_string()));
    for cloud_id in [
        json!(12),
        json!("18446744073709551616"),
        json!("-1"),
        json!(""),
        json!("1e3"),
    ] {
        let mut changed = envelope.clone();
        changed["cloudId"] = cloud_id;
        cases.push(replace_metadata(&changed.to_string()));
    }
    let text = envelope.to_string();
    cases.push(replace_metadata(&text.replacen(
        "{",
        "{\"cloudId\":\"1\",",
        1,
    )));
    cases.push(replace_metadata(
        &text.replace("\"descriptor\":{", "\"descriptor\":{\"unit\":\"mm\","),
    ));
    let mut missing = envelope.clone();
    missing["descriptor"]
        .as_object_mut()
        .unwrap()
        .remove("frame");
    cases.push(replace_metadata(&missing.to_string()));
    let mut invalid_utf8 = bytes.clone();
    invalid_utf8[32] = 0xff;
    cases.push(invalid_utf8);
    let mut zero_padding = replace_metadata(&(text.clone() + " "));
    let metadata_end = 32 + text.len() + 1;
    if metadata_end % 4 != 0 {
        zero_padding[metadata_end] = 1;
        cases.push(zero_padding);
    }
    let mut unknown_order = envelope.clone();
    unknown_order["descriptor"]["sampleOrder"] = json!("unknown");
    let mut reordered = replace_metadata(&unknown_order.to_string());
    let end = reordered.len();
    for (position, index) in [8u32, 1, 4].iter().enumerate() {
        reordered[end - 12 + position * 4..end - 8 + position * 4]
            .copy_from_slice(&index.to_le_bytes());
    }
    let reordered_input = core
        .profile_input_from_lypr(&reordered, "p", "out", 1024 * 1024)
        .unwrap();
    assert_eq!(reordered_input.source_indices, [8, 1, 4]);
    reordered[end - 4..].copy_from_slice(&8u32.to_le_bytes());
    cases.push(reordered);
    let mut empty = bytes[..start].to_vec();
    empty[8..16].fill(0);
    empty[20..24].fill(0);
    let empty_input = core
        .profile_input_from_lypr(&empty, "p", "out", 1024 * 1024)
        .unwrap();
    assert!(empty_input.xyz.is_empty());
    assert!(empty_input.validity.is_empty());
    assert_eq!(
        serde_json::from_str::<Value>(&empty_input.descriptor_json).unwrap()["sourceCount"],
        9
    );
    let mut unit = bytes.clone();
    let position = unit.windows(4).position(|w| w == b"\"mm\"").unwrap();
    unit[position + 1..position + 3].copy_from_slice(b"cm");
    cases.push(unit);
    for (index, bad) in cases.iter().enumerate() {
        assert!(
            core.profile_input_from_lypr(bad, "p", "out", 1024 * 1024)
                .is_err(),
            "invalid case {index} was accepted"
        );
    }
    assert!(core
        .profile_input_from_lypr(&bytes, "p", "out", bytes.len() - 1)
        .is_err());
}

#[test]
fn cli_accepts_native_profiles_across_commands_and_rejects_bad_inputs_before_run() {
    let dir =
        std::env::temp_dir().join(format!("lyflow-native-profile-cli-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let graph_path = dir.join("graph.lyflow.json");
    let graph = json!({"schemaVersion":1,"id":"native-profile-cli","nodes":[{"id":"p","op":"util.reroute","params":{}},{"id":"q","op":"util.reroute","params":{}}],"edges":[{"id":"e","from":{"node":"p","port":"out"},"to":{"node":"q","port":"in"}}]});
    std::fs::write(&graph_path, graph.to_string()).unwrap();
    let input_path = dir.join("input.LYPR");
    let magic_path = dir.join("input.pcd");
    let output_path = dir.join("output.lypr");
    std::fs::write(&input_path, lypr_fixture()).unwrap();
    std::fs::write(&magic_path, lypr_fixture()).unwrap();
    let run = |args: Vec<String>| {
        std::process::Command::new(env!("CARGO_BIN_EXE_lyflow"))
            .args(args)
            .env_remove("LYFLOW_CACHE_DIR")
            .output()
            .unwrap()
    };
    let path = graph_path.to_string_lossy().into_owned();
    for command in ["validate", "plan", "params", "run"] {
        let result = run(vec![
            command.into(),
            path.clone(),
            "--input".into(),
            format!("p.out={}", input_path.display()),
        ]);
        assert!(
            result.status.success(),
            "{command}: {}\n{}",
            String::from_utf8_lossy(&result.stdout),
            String::from_utf8_lossy(&result.stderr)
        );
    }
    let result = run(vec![
        "dump".into(),
        path.clone(),
        "q:out".into(),
        output_path.to_string_lossy().into_owned(),
        "--format".into(),
        "profile".into(),
        "--input".into(),
        format!("p.out={}", magic_path.display()),
    ]);
    assert!(
        result.status.success(),
        "{}",
        String::from_utf8_lossy(&result.stderr)
    );
    let core = core_ffi::core().unwrap();
    let decoded = core
        .load_profile_input(&output_path, "p", "out", 1024 * 1024)
        .unwrap();
    assert_eq!(decoded.xyz[3].to_bits(), 0x7fc01234);
    assert_eq!(decoded.validity, [1, 0, 0]);
    assert_eq!(decoded.source_indices, [1, 4, 8]);
    assert_eq!(decoded.normals.len(), 9);
    assert_eq!(decoded.rgb, [1, 2, 3, 4, 5, 6, 7, 8, 9]);
    let recording = dir.join("recording");
    let recorded = run(vec![
        "record".into(),
        path.clone(),
        "--recording".into(),
        recording.to_string_lossy().into_owned(),
        "--input".into(),
        format!("p.out={}", input_path.display()),
    ]);
    assert!(
        recorded.status.success(),
        "{}",
        String::from_utf8_lossy(&recorded.stderr)
    );
    let replayed = run(vec![
        "replay".into(),
        recording.to_string_lossy().into_owned(),
        "--recompute".into(),
    ]);
    assert!(
        replayed.status.success(),
        "{}",
        String::from_utf8_lossy(&replayed.stderr)
    );
    let alternate = dir.join("alternate.lyflow.json");
    let mut alternate_graph = graph.clone();
    alternate_graph["nodes"]
        .as_array_mut()
        .unwrap()
        .push(json!({"id":"extra","op":"util.reroute","params":{}}));
    std::fs::write(&alternate, alternate_graph.to_string()).unwrap();
    let additional = run(vec![
        "replay".into(),
        recording.to_string_lossy().into_owned(),
        "--graph".into(),
        alternate.to_string_lossy().into_owned(),
        "--bind".into(),
        "p.out=p.out".into(),
        "--input".into(),
        format!("extra.out={}", input_path.display()),
    ]);
    assert!(
        additional.status.success(),
        "{}",
        String::from_utf8_lossy(&additional.stderr)
    );
    let bad_path = dir.join("bad.lypr");
    let mut bad = lypr_fixture();
    bad[16..20].copy_from_slice(&1u32.to_le_bytes());
    std::fs::write(&bad_path, bad).unwrap();
    for command in [
        "validate", "plan", "params", "run", "dump", "record", "replay",
    ] {
        let mut args = vec![command.into()];
        if command == "replay" {
            args.extend([
                recording.to_string_lossy().into_owned(),
                "--recompute".into(),
            ]);
        } else {
            args.push(path.clone());
        }
        if command == "record" {
            args.extend([
                "--recording".into(),
                dir.join("bad-recording").to_string_lossy().into_owned(),
            ]);
        }
        if command == "dump" {
            args.extend([
                "q:out".into(),
                dir.join("bad-output.lypr").to_string_lossy().into_owned(),
                "--format".into(),
                "profile".into(),
            ]);
        }
        args.extend([
            "--input".into(),
            format!("p.out={}", input_path.display()),
            "--input".into(),
            format!("q.out={}", bad_path.display()),
        ]);
        let result = run(args);
        assert!(
            !result.status.success(),
            "{command} accepted a sliced profile"
        );
        assert!(
            !String::from_utf8_lossy(&result.stdout).contains("run_started"),
            "{command} started a run"
        );
        assert!(
            String::from_utf8_lossy(&result.stderr).contains("full profile"),
            "{command}: {}",
            String::from_utf8_lossy(&result.stderr)
        );
    }
    assert!(!dir.join("bad-recording").exists());
    assert!(!dir.join("bad-output.lypr").exists());
    let _ = std::fs::remove_dir_all(dir);
}

#[test]
fn native_profile_source_clean_fit_and_typed_metrology_roundtrip_through_lypr() {
    let core = core_ffi::core().unwrap();
    let nan = f32::from_bits(0x7fc01234);
    let raw = RunProfileInput {
        node_id: "source".into(), port: "profile".into(),
        xyz: vec![0.,0.,0., 1.,0.,0., nan,0.,nan, 2.,0.,0., 4.,0.,2., 5.,0.,2., 6.,0.,2., 77.,0.,88.],
        intensity: vec![1.,2.,3.,4.,5.,6.,7.,8.], normals: [0.,1.,0.].repeat(8),
        rgb: (0..24).collect(), validity: vec![1,1,0,1,1,1,1,0], source_indices: vec![0,2,4,6,8,10,12,14],
        descriptor_json: json!({"schemaVersion":1,"frame":"fixture-sensor","unit":"mm","plane":"XZ","handedness":"right","sampleOrder":"acquisition","sourceId":"synthetic-lypr","sourceCount":16}).to_string(), retained: None,
    };
    let seed_graph = json!({"schemaVersion":1,"id":"lypr-seed","nodes":[{"id":"source","op":"profile.input","params":{}}],"edges":[]}).to_string();
    let input = [raw.clone()];
    let mut spec = RunSpec::new(&seed_graph, "lypr-seed", "", &[]);
    spec.profile_inputs = &input;
    let seed = unsafe { RunHandle::start(core.clone(), spec, event, Box::new(())) }.unwrap();
    seed.join();
    let view = core
        .output_profile(seed.run_id(), "source", "profile", 0, 0)
        .unwrap();
    let wire = encode_profile(&view).unwrap();
    let dir = std::env::temp_dir().join(format!(
        "lyflow-native-profile-chain-{}",
        std::process::id()
    ));
    std::fs::create_dir_all(&dir).unwrap();
    let graph = dir.join("m3-native-profile.lyflow.json");
    let source = dir.join("source.lypr");
    let clean = dir.join("clean.lypr");
    std::fs::write(
        &graph,
        include_str!("../../examples/m3-native-profile.lyflow.json"),
    )
    .unwrap();
    std::fs::write(
        dir.join("m3-native-profile.csv"),
        include_str!("../../examples/m3-native-profile.csv"),
    )
    .unwrap();
    std::fs::write(&source, &wire).unwrap();
    let run = |args: Vec<String>| {
        std::process::Command::new(env!("CARGO_BIN_EXE_lyflow"))
            .args(args)
            .env_remove("LYFLOW_CACHE_DIR")
            .output()
            .unwrap()
    };
    let require_success = |result: &std::process::Output| {
        assert!(
            result.status.success(),
            "{}\n{}",
            String::from_utf8_lossy(&result.stdout),
            String::from_utf8_lossy(&result.stderr)
        )
    };
    let check_measurements = |result: &std::process::Output| {
        require_success(result);
        let values: Vec<Value> = String::from_utf8_lossy(&result.stdout)
            .lines()
            .map(|line| serde_json::from_str(line).unwrap())
            .collect();
        let outputs = values
            .iter()
            .find(|value| value["gap"]["type"] == "Measurement")
            .expect("typed gap output");
        for name in ["gap", "flush"] {
            assert_eq!(outputs[name]["value"]["unit"], "m");
            assert_eq!(outputs[name]["value"]["ok"], true);
            assert!((outputs[name]["value"]["value"].as_f64().unwrap() - 0.002).abs() < 1e-7);
        }
    };
    check_measurements(&run(vec![
        "run".into(),
        graph.display().to_string(),
        "--outputs".into(),
    ]));
    check_measurements(&run(vec![
        "run".into(),
        graph.display().to_string(),
        "--outputs".into(),
        "--input".into(),
        format!("source.profile={}", source.display()),
    ]));
    let dumped = run(vec![
        "dump".into(),
        graph.display().to_string(),
        "clean:clean".into(),
        clean.display().to_string(),
        "--format".into(),
        "profile".into(),
        "--input".into(),
        format!("source.profile={}", source.display()),
    ]);
    require_success(&dumped);
    let restored = core
        .load_profile_input(&clean, "source", "profile", 1024 * 1024)
        .unwrap();
    assert_eq!(restored.validity, raw.validity);
    assert_eq!(restored.source_indices, raw.source_indices);
    assert_eq!(restored.intensity, raw.intensity);
    assert_eq!(restored.normals, raw.normals);
    assert_eq!(restored.rgb, raw.rgb);
    for (actual, expected) in restored.xyz.iter().zip(&raw.xyz) {
        assert_eq!(actual.to_bits(), expected.to_bits());
    }
    assert_eq!(
        serde_json::from_str::<Value>(&restored.descriptor_json).unwrap(),
        serde_json::from_str::<Value>(&raw.descriptor_json).unwrap()
    );
    check_measurements(&run(vec![
        "run".into(),
        graph.display().to_string(),
        "--outputs".into(),
        "--input".into(),
        format!("source.profile={}", clean.display()),
    ]));
    let mut metric_graph: Value =
        serde_json::from_str(include_str!("../../examples/m3-native-profile.lyflow.json")).unwrap();
    metric_graph["nodes"][0]["params"]["unit"] = json!("m");
    metric_graph["nodes"][0]["params"]["path"] = json!("metric.csv");
    let metric = dir.join("metric.lyflow.json");
    std::fs::write(&metric, metric_graph.to_string()).unwrap();
    std::fs::write(
        dir.join("metric.csv"),
        "x,z\n0,0\n.001,0\nNaN,NaN\n.002,0\n.004,.002\n.005,.002\n.006,.002\nNaN,.003\n",
    )
    .unwrap();
    check_measurements(&run(vec![
        "run".into(),
        metric.display().to_string(),
        "--outputs".into(),
    ]));
    let bad = dir.join("bad.lypr");
    let mut invalid = wire;
    invalid[16..20].copy_from_slice(&1u32.to_le_bytes());
    std::fs::write(&bad, invalid).unwrap();
    let rejected = run(vec![
        "run".into(),
        graph.display().to_string(),
        "--input".into(),
        format!("source.profile={}", source.display()),
        "--input".into(),
        format!("clean.clean={}", bad.display()),
    ]);
    assert!(!rejected.status.success());
    assert!(!String::from_utf8_lossy(&rejected.stdout).contains("run_started"));
    let bad_output = dir.join("bad-source.lypr");
    std::fs::write(dir.join("m3-native-profile.csv"), "x,z\n0,0\n1,0\n2,Inf\n").unwrap();
    let rejected = run(vec![
        "dump".into(),
        graph.display().to_string(),
        "source:profile".into(),
        bad_output.display().to_string(),
        "--format".into(),
        "profile".into(),
    ]);
    assert!(!rejected.status.success());
    assert!(String::from_utf8_lossy(&rejected.stderr).contains("第4行"));
    assert!(!bad_output.exists());
    let _ = std::fs::remove_dir_all(dir);
}
