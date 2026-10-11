use lyflow_client::{Core, RunHandle, RunImageInput, RunSpec, ABI_VERSION};
use serde_json::{json, Value};
use std::{
    collections::BTreeSet,
    ffi::{c_char, c_void},
    fs,
    path::{Path, PathBuf},
    sync::Arc,
};
type Result<T> = std::result::Result<T, Box<dyn std::error::Error>>;

fn require(ok: bool, message: impl Into<String>) -> Result<()> {
    if ok {
        Ok(())
    } else {
        Err(message.into().into())
    }
}

fn node(id: &str, op: &str, params: Value) -> Value {
    json!({"id":id,"op":op,"params":params})
}
fn graph(nodes: Vec<Value>, edges: &[(&str, &str)]) -> Value {
    let edges: Vec<Value> = edges
        .iter()
        .enumerate()
        .map(|(i, (a, b))| {
            let (an, ap) = a.split_once('.').unwrap();
            let (bn, bp) = b.split_once('.').unwrap();
            json!({"id":format!("e{i}"),"from":{"node":an,"port":ap},"to":{"node":bn,"port":bp}})
        })
        .collect();
    json!({"schemaVersion":1,"id":"release-probe","name":"release-probe","nodes":nodes,"edges":edges})
}
unsafe extern "C" fn event(_: *const c_char, _: *mut c_void) {}
fn run(core: &Arc<Core>, doc: Value, id: &str, images: &[RunImageInput]) -> Result<RunHandle> {
    let text = doc.to_string();
    let issues: Value = serde_json::from_str(&core.validate(&text, "")?)?;
    require(
        !issues
            .as_array()
            .unwrap()
            .iter()
            .any(|i| i["severity"] == "error"),
        format!("validation: {issues}"),
    )?;
    let mut spec = RunSpec::new(&text, id, "", &[]);
    spec.no_reuse = true;
    spec.image_inputs = images;
    let handle = unsafe { RunHandle::start(core.clone(), spec, event, Box::new(()))? };
    handle.join();
    let summary: Value =
        serde_json::from_str(&core.run_summary(id)?.ok_or("missing run summary")?)?;
    require(summary["status"] == "ok", format!("{id}: {summary}"))?;
    Ok(handle)
}
fn image(path: &Path, w: u32, h: u32) -> Result<RunImageInput> {
    let pixels = fs::read(path)?;
    require(pixels.len() == (w * h) as usize, "fixture image dimensions")?;
    Ok(RunImageInput {
        node_id: "src".into(),
        port: "image".into(),
        width: w,
        height: h,
        channels: 1,
        depth: 1,
        pixels,
    })
}
fn value(core: &Core, id: &str, node: &str, port: &str) -> Result<Value> {
    let info: Value = serde_json::from_str(&core.output_info(id, node)?)?;
    Ok(info
        .as_array()
        .ok_or("invalid output list")?
        .iter()
        .find(|v| v["port"] == port)
        .ok_or("missing output port")?["value"]
        .clone())
}
fn number(value: &Value) -> Result<f64> {
    value
        .as_f64()
        .ok_or_else(|| format!("expected number: {value}").into())
}
fn inside(path: &Path, root: &Path) -> bool {
    path.to_string_lossy()
        .to_lowercase()
        .starts_with(&(root.to_string_lossy().trim_end_matches('\\').to_lowercase() + "\\"))
}

#[link(name = "kernel32")]
extern "system" {
    fn GetCurrentProcess() -> *mut c_void;
    fn K32EnumProcessModules(
        process: *mut c_void,
        modules: *mut *mut c_void,
        size: u32,
        needed: *mut u32,
    ) -> i32;
    fn K32GetModuleFileNameExW(
        process: *mut c_void,
        module: *mut c_void,
        path: *mut u16,
        size: u32,
    ) -> u32;
}
fn modules(root: &Path) -> Result<Vec<String>> {
    let mut handles = vec![std::ptr::null_mut(); 4096];
    let mut needed = 0;
    require(
        unsafe {
            K32EnumProcessModules(
                GetCurrentProcess(),
                handles.as_mut_ptr(),
                (handles.len() * std::mem::size_of::<*mut c_void>()) as u32,
                &mut needed,
            )
        } != 0,
        "module enumeration failed",
    )?;
    require(
        (needed as usize) <= handles.len() * std::mem::size_of::<*mut c_void>(),
        "module list overflow",
    )?;
    let system = PathBuf::from(std::env::var("SystemRoot")?).canonicalize()?;
    let mut shipped = BTreeSet::new();
    files(root, root, &mut shipped)?;
    let shipped: BTreeSet<String> = shipped
        .iter()
        .map(|p| p.rsplit('/').next().unwrap().to_lowercase())
        .collect();
    let mut out = vec![];
    let mut crt = false;
    for handle in handles
        .into_iter()
        .take(needed as usize / std::mem::size_of::<*mut c_void>())
    {
        let mut buffer = vec![0u16; 32768];
        let len = unsafe {
            K32GetModuleFileNameExW(
                GetCurrentProcess(),
                handle,
                buffer.as_mut_ptr(),
                buffer.len() as u32,
            )
        };
        require(
            len > 0 && (len as usize) < buffer.len(),
            "module path unavailable",
        )?;
        let path = PathBuf::from(String::from_utf16(&buffer[..len as usize])?).canonicalize()?;
        if path
            .extension()
            .and_then(|e| e.to_str())
            .is_some_and(|e| e.eq_ignore_ascii_case("dll"))
        {
            let name = path.file_name().unwrap().to_string_lossy().to_lowercase();
            let is_crt = name.starts_with("vcruntime")
                || name.starts_with("msvcp")
                || name.starts_with("concrt")
                || name.starts_with("vcomp");
            require(
                inside(&path, root)
                    || (!is_crt
                        && !shipped.contains(&name)
                        && !name.starts_with("onnxruntime")
                        && inside(&path, &system)),
                format!("DLL escaped package: {}", path.display()),
            )?;
            crt |= is_crt;
            out.push(path.display().to_string());
        }
    }
    require(crt, "no package-local VC++ runtime loaded")?;
    Ok(out)
}
fn pe_x64(path: &Path) -> Result<()> {
    let b = fs::read(path)?;
    require(
        b.len() > 64 && &b[..2] == b"MZ",
        format!("invalid PE {}", path.display()),
    )?;
    let offset = u32::from_le_bytes(b[60..64].try_into()?) as usize;
    require(
        offset + 6 <= b.len()
            && &b[offset..offset + 4] == b"PE\0\0"
            && u16::from_le_bytes(b[offset + 4..offset + 6].try_into()?) == 0x8664,
        format!("not x64: {}", path.display()),
    )
}
fn files(root: &Path, path: &Path, out: &mut BTreeSet<String>) -> Result<()> {
    for entry in fs::read_dir(path)? {
        let entry = entry?;
        let p = entry.path();
        require(!entry.file_type()?.is_symlink(), "symlink in runtime")?;
        if p.is_dir() {
            files(root, &p, out)?;
        } else {
            out.insert(p.strip_prefix(root)?.to_string_lossy().replace('\\', "/"));
        }
    }
    Ok(())
}
fn validate_metadata(
    root: &Path,
    version: &str,
    abi: u32,
    manifest: &Value,
    sdk: bool,
) -> Result<()> {
    let m: Value = serde_json::from_slice(&fs::read(root.join("lyflow-runtime.json"))?)?;
    require(
        m["schemaVersion"] == 1
            && m["engineVersion"] == version
            && m["abiVersion"] == abi
            && m["architecture"] == "x64"
            && m["buildConfiguration"] == "RelWithDebInfo",
        "metadata identity/ABI/architecture mismatch",
    )?;
    require(
        m["sourceRevision"]
            .as_str()
            .is_some_and(|s| s.len() == 40 && s.chars().all(|c| c.is_ascii_hexdigit())),
        "invalid source revision",
    )?;
    let mut expected = BTreeSet::new();
    for file in m["files"].as_array().ok_or("missing file list")? {
        let p = file["path"].as_str().ok_or("invalid file path")?;
        require(
            !p.is_empty()
                && !p.contains('\\')
                && !p.contains(':')
                && !p.starts_with('/')
                && p.split('/').all(|s| !s.is_empty() && s != "." && s != ".."),
            "unsafe package relative path",
        )?;
        require(expected.insert(p.to_string()), "duplicate file path")?;
    }
    let mut actual = BTreeSet::new();
    files(root, root, &mut actual)?;
    actual.remove("__lyflow_release_probe.exe");
    if sdk {
        actual.remove("lyflow.exe");
    }
    require(actual == expected, "package files differ from metadata")?;
    for p in &actual {
        if p.to_lowercase().ends_with(".dll") {
            pe_x64(&root.join(p))?;
        }
    }
    let op_key = |op: &Value| {
        format!(
            "{}|{}|{}",
            op["id"].as_str().unwrap_or(""),
            op["version"].as_str().unwrap_or(""),
            op["pack"].as_str().unwrap_or("core")
        )
    };
    let real: BTreeSet<_> = manifest["operators"]
        .as_array()
        .ok_or("missing manifest operators")?
        .iter()
        .map(op_key)
        .collect();
    let stated: BTreeSet<_> = m["operators"]
        .as_array()
        .ok_or("missing metadata operators")?
        .iter()
        .map(op_key)
        .collect();
    require(real == stated, "operator metadata mismatch")?;
    let packs: BTreeSet<_> = manifest["operators"]
        .as_array()
        .unwrap()
        .iter()
        .filter_map(|o| o["pack"].as_str())
        .map(str::to_string)
        .collect();
    let stated: BTreeSet<_> = m["packs"]
        .as_array()
        .ok_or("missing packs")?
        .iter()
        .map(|p| {
            format!(
                "{}@{}",
                p["name"].as_str().unwrap_or(""),
                p["version"].as_str().unwrap_or("")
            )
        })
        .collect();
    require(packs == stated, "pack metadata mismatch")?;
    let names: BTreeSet<_> = packs.iter().map(|p| p.split('@').next().unwrap()).collect();
    require(
        names == BTreeSet::from(["std-pointcloud", "std-image", "std-ml", "glue"]),
        "unexpected release pack selection",
    )?;
    require(
        m["dependencies"].as_array().is_some_and(|d| {
            !d.is_empty()
                && d.iter().all(|v| {
                    v["name"].as_str().is_some_and(|s| !s.is_empty())
                        && v["version"].as_str().is_some_and(|s| !s.is_empty())
                })
        }),
        "missing dependency versions",
    )?;
    Ok(())
}
fn scenarios(core: &Arc<Core>, fixtures: &Path) -> Result<Vec<&'static str>> {
    let _run = run(
        core,
        graph(
            vec![
                node("src", "gen.synthetic", json!({"pointCount":4096})),
                node("filter", "filter.voxel_grid", json!({})),
            ],
            &[("src.cloud", "filter.cloud")],
        ),
        "cloud",
        &[],
    )?;
    let cloud = core.output_cloud("cloud", "filter", "cloud", 5000)?;
    require(
        cloud.total_points() > 0 && cloud.total_points() <= 4096,
        "pointcloud filter count",
    )?;
    let pixels: Vec<u8> = (0..64).collect();
    let input = RunImageInput {
        node_id: "src".into(),
        port: "image".into(),
        width: 8,
        height: 8,
        channels: 1,
        depth: 1,
        pixels: pixels.clone(),
    };
    let _run = run(
        core,
        graph(
            vec![
                node("src", "io.load_image", json!({"source":"inputs"})),
                node(
                    "tensor",
                    "image.to_tensor",
                    json!({"layout":"NCHW","scale":1}),
                ),
                node(
                    "infer",
                    "ml.onnx_run",
                    json!({"modelPath":fixtures.join("identity.onnx")}),
                ),
            ],
            &[
                ("src.image", "tensor.image"),
                ("tensor.tensor", "infer.input"),
            ],
        ),
        "onnx",
        &[input],
    )?;
    let tensor = core.output_tensor("onnx", "infer", "output", 0, 64)?;
    require(
        tensor.shape() == [1, 1, 8, 8]
            && tensor.data() == pixels.iter().map(|v| *v as f32).collect::<Vec<_>>(),
        "ONNX CPU identity inference values",
    )?;
    let _run = run(
        core,
        graph(
            vec![
                node("src", "io.load_image", json!({"source":"inputs"})),
                node(
                    "calib",
                    "image.board_calib",
                    json!({"pattern":[9,6],"square":5}),
                ),
            ],
            &[("src.image", "calib.image")],
        ),
        "calib",
        &[image(&fixtures.join("board.raw"), 640, 480)?],
    )?;
    let calib = value(core, "calib", "calib", "calib")?;
    let data = &calib["data"];
    require(
        (number(&data["mmPerPx"])? - 0.125).abs() < 0.01 && number(&data["rms"])? < 0.05,
        "chessboard calibration numeric check",
    )?;
    for (id, file, expected) in [("bead", "bead.raw", 0), ("broken", "broken.raw", 1)] {
        let _run = run(
            core,
            graph(
                vec![
                    node("src", "io.load_image", json!({"source":"inputs"})),
                    node(
                        "path",
                        "glue.bead_path",
                        json!({"nozzle":[150,500],"sector":[-120,0]}),
                    ),
                    node("width", "glue.bead_width", json!({"form":"straight"})),
                    node("breaks", "glue.bead_breaks", json!({})),
                ],
                &[
                    ("src.image", "path.image"),
                    ("src.image", "width.image"),
                    ("path.path", "width.path"),
                    ("width.bead", "breaks.bead"),
                ],
            ),
            id,
            &[image(&fixtures.join(file), 640, 560)?],
        )?;
        let width = value(core, id, "width", "widthMean")?;
        require(
            (number(&width["value"])? - 20.0).abs() < 1.5 && width["unit"] == "px",
            format!("glue width: {width}"),
        )?;
        let breaks = value(core, id, "breaks", "breaks")?;
        require(
            breaks["data"]["count"] == expected,
            format!("glue break detection: {breaks}"),
        )?;
        if expected == 1 {
            let b = &breaks["data"]["breaks"][0];
            require(
                (number(&b["sStart"])? - 234.0).abs() <= 8.0
                    && (number(&b["sEnd"])? - 274.0).abs() <= 8.0,
                "glue gap location",
            )?;
        }
    }
    Ok(vec![
        "pointcloud",
        "image-injection",
        "onnx-cpu-inference",
        "board-calibration",
        "glue-width",
        "glue-break",
    ])
}
fn main() -> Result<()> {
    let args: Vec<_> = std::env::args().collect();
    require(
        args.len() >= 4,
        "usage: probe runtime report inspect|full [fixtures] [sdk]",
    )?;
    let root = PathBuf::from(&args[1]).canonicalize()?;
    let dll = root.join("lyflow_core.dll");
    if args[3] == "load-only" {
        let core = Core::load_from(&dll)?;
        fs::write(
            &args[2],
            serde_json::to_string_pretty(
                &json!({"status":"passed", "engineVersion":core.version()}),
            )?,
        )?;
        return Ok(());
    }
    for name in [
        "lyflow_core.dll",
        "onnxruntime.dll",
        "onnxruntime_providers_shared.dll",
        "vcruntime140.dll",
        "vcruntime140_1.dll",
        "msvcp140.dll",
        "vcomp140.dll",
    ] {
        pe_x64(&root.join(name))?;
    }
    let core = Arc::new(Core::load_from(&dll)?);
    require(
        core.manifest_problems()?.is_empty(),
        "manifest self-check failed",
    )?;
    let library = unsafe { libloading::Library::new(&dll)? };
    let abi = unsafe { library.get::<unsafe extern "C" fn() -> u32>(b"lyflow_abi_version\0")?() };
    require(
        abi == ABI_VERSION,
        format!("client ABI {ABI_VERSION} != runtime ABI {abi}"),
    )?;
    let manifest: Value = serde_json::from_str(&core.manifest_json()?)?;
    let version = core.version();
    let checks = if args[3] == "inspect" {
        vec![]
    } else {
        validate_metadata(
            &root,
            &version,
            abi,
            &manifest,
            args.get(5).is_some_and(|v| v == "sdk"),
        )?;
        scenarios(&core, Path::new(args.get(4).ok_or("fixtures required")?))?
    };
    let ort_path = root.join("onnxruntime.dll");
    let ort = unsafe { libloading::Library::new(&ort_path)? };
    #[repr(C)]
    struct OrtApiBase {
        get_api: usize,
        get_version: unsafe extern "system" fn() -> *const c_char,
    }
    let ort_base = unsafe {
        ort.get::<unsafe extern "system" fn() -> *const OrtApiBase>(b"OrtGetApiBase\0")?()
    };
    require(!ort_base.is_null(), "ORT API base is null")?;
    let ort_version = unsafe { std::ffi::CStr::from_ptr(((*ort_base).get_version)()).to_str()? };
    require(
        ort_version == "1.19.2",
        format!("unexpected ONNX Runtime {ort_version}"),
    )?;
    let loaded = modules(&root)?;
    let report = json!({"status":"passed","engineVersion":version,"abiVersion":abi,"manifest":manifest,"checks":checks,"loadedModules":loaded,"onnxRuntimeVersion":ort_version});
    fs::write(&args[2], serde_json::to_string_pretty(&report)?)?;
    Ok(())
}
