use lyflow_client::{Core, ABI_VERSION};
use serde_json::{json, Value};
use std::{
    collections::BTreeSet,
    ffi::{c_char, c_void},
    fs,
    path::{Path, PathBuf},
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
fn main() -> Result<()> {
    let args: Vec<_> = std::env::args().collect();
    require(args.len() == 3, "usage: probe runtime report")?;
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
        pe_x64(&root.join(name))?;
    }
    let core = Core::load_from(&dll)?;
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
    let required = release["requiredOperators"]
        .as_array()
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
    let report = json!({"status":"passed","engineVersion":version,"abiVersion":abi,"sourceRevision":source_revision,"manifest":manifest,"loadedModules":loaded,"onnxRuntimeVersion":ort_version});
    fs::write(&args[2], serde_json::to_string_pretty(&report)?)?;
    Ok(())
}
