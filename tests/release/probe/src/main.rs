use lyflow_client::{Core, RunHandle, RunSpec, ABI_VERSION};
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
unsafe extern "C" fn profile_event(_: *const c_char, _: *mut c_void) {}

fn profile_contract(core: &Arc<Core>) -> Result<Value> {
    let nan = f32::from_bits(0x7fc01234);
    let xyz = [
        0., 0., 0., 1., 0., 0., nan, 0., nan, 2., 0., 0., 4., 0., 2., 5., 0., 2., 6., 0., 2., 77.,
        0., 88.,
    ];
    let intensity = [1., 2., 3., 4., 5., 6., 7., 8.];
    let normals = [0., 1., 0.].repeat(8);
    let rgb: Vec<u8> = (0..24).collect();
    let metadata = json!({"descriptor":{"schemaVersion":1,"frame":"fixture-sensor","unit":"mm","plane":"XZ","handedness":"right","sampleOrder":"acquisition","sourceId":"release-probe","sourceCount":16},"cloudId":"18446744073709551615"}).to_string();
    let mut wire = Vec::new();
    for value in [0x5250594cu32, 1, 8, 8, 0, 7, metadata.len() as u32, 0] {
        wire.extend_from_slice(&value.to_le_bytes());
    }
    wire.extend_from_slice(metadata.as_bytes());
    wire.resize((wire.len() + 3) & !3, 0);
    for channel in [&xyz[..], &intensity[..], &normals[..]] {
        for value in channel {
            wire.extend_from_slice(&value.to_le_bytes());
        }
    }
    wire.extend_from_slice(&rgb);
    wire.resize((wire.len() + 3) & !3, 0);
    wire.extend_from_slice(&[1, 1, 0, 1, 1, 1, 1, 0]);
    wire.resize((wire.len() + 3) & !3, 0);
    for index in [0u32, 2, 4, 6, 8, 10, 12, 14] {
        wire.extend_from_slice(&index.to_le_bytes());
    }
    let mut input = core.profile_input_from_lypr(&wire, "source", "profile", 1024 * 1024)?;
    let mut invalid = wire.clone();
    invalid[16..20].copy_from_slice(&1u32.to_le_bytes());
    require(
        core.profile_input_from_lypr(&invalid, "source", "profile", 1024 * 1024)
            .is_err(),
        "Preview Profile source must be rejected",
    )?;
    input.xyz.clear();
    input.validity.clear();
    input.source_indices.clear();
    input.intensity.clear();
    input.normals.clear();
    input.rgb.clear();
    let inputs = [input];
    let graph = include_str!("../../../../examples/m3-native-profile.lyflow.json");
    let mut spec = RunSpec::new(graph, "release-profile-contract", "", &[]);
    spec.profile_inputs = &inputs;
    let run = unsafe { RunHandle::start(Arc::clone(core), spec, profile_event, Box::new(())) }?;
    run.join();
    let output = core.output_profile(run.run_id(), "clean", "clean", 0, 0)?;
    require(
        output.point_count() == 8 && output.flags() == 7,
        "Profile source/clean lost slots or channels",
    )?;
    require(
        output.validity() == [1, 1, 0, 1, 1, 1, 1, 0]
            && output.source_indices() == [0, 2, 4, 6, 8, 10, 12, 14],
        "Profile source mask/index mapping changed",
    )?;
    require(
        output
            .xyz()
            .iter()
            .zip(&xyz)
            .all(|(actual, expected)| actual.to_bits() == expected.to_bits()),
        "Profile coordinate bits changed",
    )?;
    require(
        output.intensity() == intensity && output.normals() == normals && output.rgb() == rgb,
        "Profile optional channels changed",
    )?;
    let descriptor: Value = serde_json::from_str(output.descriptor_json())?;
    require(
        descriptor["unit"] == "mm"
            && descriptor["sourceId"] == "release-probe"
            && descriptor["sourceCount"] == 16,
        "Profile descriptor changed",
    )?;
    let outputs: Value = serde_json::from_str(&core.run_outputs(run.run_id())?)?;
    for name in ["gap", "flush"] {
        let measurement = &outputs[name]["value"];
        require(
            outputs[name]["type"] == "Measurement"
                && measurement["ok"] == true
                && measurement["unit"] == "m"
                && measurement["value"]
                    .as_f64()
                    .is_some_and(|v| (v - 0.002).abs() < 1e-7),
            format!("Profile typed {name} is not 0.002 m: {outputs}"),
        )?;
    }
    Ok(
        json!({"lyprInput":true,"slots":8,"validSlots":6,"optionalChannelsPreserved":true,"sourceMappingPreserved":true,"badInputRejected":true,"gap":outputs["gap"]["value"],"flush":outputs["flush"]["value"]}),
    )
}

fn main() -> Result<()> {
    let args: Vec<_> = std::env::args().collect();
    require(
        args.len() == 3 || (args.len() == 4 && args[3] == "--profile-only"),
        "usage: probe runtime report [--profile-only]",
    )?;
    let profile_only = args.len() == 4;
    let root = PathBuf::from(&args[1]).canonicalize()?;
    let dll = root.join("lyflow_core.dll");
    for name in [
        "lyflow_core.dll",
        "onnxruntime.dll",
        "onnxruntime_providers_shared.dll",
        "vcruntime140.dll",
        "vcruntime140_1.dll",
        "msvcp140.dll",
        "vcomp140.dll",
    ] {
        if !profile_only || name == "lyflow_core.dll" {
            pe_x64(&root.join(name))?;
        }
    }
    let core = Arc::new(Core::load_from(&dll)?);
    require(
        core.manifest_problems()?.is_empty(),
        "manifest self-check failed",
    )?;
    let library = unsafe { libloading::Library::new(&dll)? };
    let abi_pointer = unsafe {
        library.get::<unsafe extern "C" fn() -> *const c_char>(b"lyflow_abi_version_string\0")?()
    };
    require(!abi_pointer.is_null(), "DLL ABI string is null")?;
    let abi = unsafe { std::ffi::CStr::from_ptr(abi_pointer).to_str()? };
    require(
        abi == ABI_VERSION,
        format!("client ABI {ABI_VERSION} != runtime ABI {abi}"),
    )?;
    let revision_pointer = unsafe {
        library.get::<unsafe extern "C" fn() -> *const c_char>(b"lyflow_source_revision\0")?()
    };
    require(!revision_pointer.is_null(), "DLL source revision is null")?;
    let source_revision = unsafe { std::ffi::CStr::from_ptr(revision_pointer).to_str()? };
    require(
        source_revision.is_empty()
            || (source_revision.len() == 40
                && source_revision.bytes().all(|c| c.is_ascii_hexdigit())),
        "invalid compiled source revision",
    )?;
    let manifest: Value = serde_json::from_str(&core.manifest_json()?)?;
    let release: Value = serde_json::from_str(include_str!("../../../../packaging/release.json"))?;
    let profile_operators = json!([
        "profile.input",
        "profile.clean",
        "profile.fit_line",
        "metrology.profile_gap_flush"
    ]);
    let required = if profile_only {
        profile_operators.as_array()
    } else {
        release["requiredOperators"].as_array()
    }
    .ok_or("release configuration missing required operators")?;
    let operators = manifest["operators"]
        .as_array()
        .ok_or("runtime manifest missing operators")?;
    for required in required {
        let id = required.as_str().ok_or("invalid required operator ID")?;
        require(
            operators.iter().any(|op| op["id"].as_str() == Some(id)),
            format!("release runtime missing required operator: {id}"),
        )?;
    }
    let version = core.version();
    require(
        version == ABI_VERSION,
        "engine version must exactly equal client ABI/product version",
    )?;
    let mut ort_library = None;
    let ort_version = if profile_only {
        None
    } else {
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
        let ort_version =
            unsafe { std::ffi::CStr::from_ptr(((*ort_base).get_version)()).to_str()? };
        require(
            ort_version
                == serde_json::from_str::<Value>(include_str!(
                    "../../../../packaging/release.json"
                ))?["onnxruntimeVersion"]
                    .as_str()
                    .ok_or("release.json missing onnxruntimeVersion")?,
            format!("unexpected ONNX Runtime {ort_version}"),
        )?;
        let version = ort_version.to_string();
        ort_library = Some(ort);
        Some(version)
    };
    let profile = profile_contract(&core)?;
    let loaded = modules(&root)?;
    let report = json!({"status":"passed","scope":if profile_only {"native-profile"} else {"full-runtime"},"engineVersion":version,"abiVersion":abi,"sourceRevision":source_revision,"manifest":manifest,"loadedModules":loaded,"onnxRuntimeVersion":ort_version,"profile":profile});
    fs::write(&args[2], serde_json::to_string_pretty(&report)?)?;
    drop(ort_library);
    Ok(())
}
