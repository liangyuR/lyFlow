use lyflow_lib::core_ffi::{self, RunHandle, RunSpec};
use serde_json::{json, Value};
use std::ffi::{c_char, c_void};
use std::process::Command;

unsafe extern "C" fn event(_: *const c_char, _: *mut c_void) {}

#[test]
fn python_cli_and_embedded_execute_identical_graph_and_refresh_without_touching_native_nodes() {
    let source = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .unwrap();
    let root = std::env::temp_dir().join(format!("lyflow-python-client-{}", std::process::id()));
    std::fs::create_dir_all(&root).unwrap();
    let core = core_ffi::core().unwrap();
    assert!(core.supports_python());
    let executable = std::env::var("LYFLOW_TEST_PYTHON").unwrap_or_else(|_| "python".into());
    let signature_file = root.join("signature.py");
    std::fs::write(&signature_file, "from lyflow_python import node, Record\nimport hashlib\n@node(id='py.test.signature', version='1.0.0', label='Signature', inputs={'cloud':'PointCloud'}, outputs={'signature':'Record'})\ndef signature(inputs, params, ctx):\n    return {'signature': Record({'sha256':hashlib.sha256(inputs['cloud'].xyz.tobytes()).hexdigest()})}\n").unwrap();
    let config = json!({ "executable": executable,
        "packagePaths": [source.join("python/examples/offset.py"), signature_file],
        "timeoutMs": 60000, "sdkDir": source.join("python") });
    let status: Value =
        serde_json::from_str(&core.configure_python(&config.to_string()).unwrap()).unwrap();
    assert_eq!(status["ready"], true, "{status}");
    assert_eq!(status["operatorCount"], 2);
    let graph = json!({"schemaVersion":1,"id":"01J8XQZ4K7N3M2R5V8W1YB6DC1","name":"Python parity",
        "nodes":[{"id":"source","op":"gen.synthetic","params":{"pointCount":1000,"seed":42,"withIntensity":true}},
                 {"id":"offset","op":"py.example.offset","opVersion":"1.0.0","params":{"offset":0.25}},
                 {"id":"signature","op":"py.test.signature","params":{}}],
        "edges":[{"id":"edge","from":{"node":"source","port":"cloud"},"to":{"node":"offset","port":"cloud"}},
                 {"id":"signature-edge","from":{"node":"offset","port":"cloud"},"to":{"node":"signature","port":"cloud"}}],
        "outputs":{"signature":{"node":"signature","port":"signature"}}});
    let graph_json = graph.to_string();
    let run = unsafe {
        RunHandle::start(
            core.clone(),
            RunSpec::new(&graph_json, "python-parity", root.to_str().unwrap(), &[]),
            event,
            Box::new(()),
        )
    }
    .unwrap();
    run.join();
    let original = core
        .output_cloud(run.run_id(), "source", "cloud", 0)
        .unwrap();
    let shifted = core
        .output_cloud(run.run_id(), "offset", "cloud", 0)
        .unwrap();
    assert_eq!(shifted.point_count(), 1000);
    assert_eq!(original.intensity(), shifted.intensity());
    for (before, after) in original
        .xyz()
        .chunks_exact(3)
        .zip(shifted.xyz().chunks_exact(3))
    {
        assert_eq!(&before[..2], &after[..2]);
        assert_eq!(after[2], before[2] + 0.25f32);
    }
    let expected: Value = serde_json::from_str(&core.run_outputs(run.run_id()).unwrap()).unwrap();
    drop(original);
    drop(shifted);
    drop(run);
    let settings_file = root.join("settings.json");
    let graph_file = root.join("graph.lyflow.json");
    std::fs::write(&settings_file, config.to_string()).unwrap();
    std::fs::write(&graph_file, graph_json).unwrap();
    let result = Command::new(env!("CARGO_BIN_EXE_lyflow"))
        .arg("run")
        .arg(&graph_file)
        .arg("--outputs")
        .env("LYFLOW_PYTHON_CONFIG", &settings_file)
        .env_remove("LYFLOW_CACHE_DIR")
        .output()
        .unwrap();
    assert!(
        result.status.success(),
        "{}\n{}",
        String::from_utf8_lossy(&result.stdout),
        String::from_utf8_lossy(&result.stderr)
    );
    let actual: Value = String::from_utf8_lossy(&result.stdout)
        .lines()
        .filter_map(|line| serde_json::from_str::<Value>(line).ok())
        .last()
        .unwrap();
    assert!(
        expected["signature"]["value"]["data"]["sha256"]
            .as_str()
            .is_some_and(|value| value.len() == 64),
        "{expected}"
    );
    assert_eq!(actual, expected);
    let changed_file = root.join("changed.lyflow.json");
    let mut changed = graph.clone();
    changed["nodes"][1]["params"]["offset"] = json!(0.5);
    std::fs::write(&changed_file, changed.to_string()).unwrap();
    let comparison = Command::new(env!("CARGO_BIN_EXE_lyflow"))
        .arg("diff")
        .arg(&graph_file)
        .arg(&changed_file)
        .arg("--json")
        .env("LYFLOW_PYTHON_CONFIG", &settings_file)
        .output()
        .unwrap();
    assert!(
        comparison.status.success(),
        "{}",
        String::from_utf8_lossy(&comparison.stderr)
    );
    let difference: Value = serde_json::from_slice(&comparison.stdout).unwrap();
    assert_eq!(difference["empty"], false, "{difference}");
    assert!(difference.to_string().contains("offset"), "{difference}");
    let marker = root.join("imported.txt");
    let query_script = root.join("query.py");
    std::fs::write(&query_script, format!("from pathlib import Path\nPath({}).touch()\nraise RuntimeError('queries must not import this file')\n", serde_json::to_string(&marker.to_string_lossy()).unwrap())).unwrap();
    let mut query_config = config.clone();
    query_config["packagePaths"] = json!([query_script]);
    std::fs::write(&settings_file, query_config.to_string()).unwrap();
    for arguments in [
        &["python", "settings"][..],
        &["--help"][..],
        &["--version"][..],
    ] {
        let query = Command::new(env!("CARGO_BIN_EXE_lyflow"))
            .args(arguments)
            .env("LYFLOW_PYTHON_CONFIG", &settings_file)
            .output()
            .unwrap();
        assert!(
            query.status.success(),
            "{}",
            String::from_utf8_lossy(&query.stderr)
        );
        assert!(!marker.exists());
    }
    let refreshed: Value = serde_json::from_str(&core.refresh_python().unwrap()).unwrap();
    assert_eq!(refreshed["ready"], true, "{refreshed}");
    core.configure_python("{}").unwrap();
    let manifest: Value = serde_json::from_str(&core.manifest_json().unwrap()).unwrap();
    assert!(manifest["operators"]
        .as_array()
        .unwrap()
        .iter()
        .any(|op| op["id"] == "gen.synthetic"));
    assert!(!manifest["operators"]
        .as_array()
        .unwrap()
        .iter()
        .any(|op| op["id"] == "py.example.offset"));
    std::fs::remove_dir_all(&root).unwrap();
}
