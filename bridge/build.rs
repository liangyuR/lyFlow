use std::collections::HashSet;
use std::ffi::OsString;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};

fn main() {
    println!("cargo:rerun-if-env-changed=LYFLOW_NATIVE_SDK");
    let sdk = std::env::var_os("LYFLOW_NATIVE_SDK")
        .map(PathBuf::from)
        .unwrap_or_else(|| Path::new(env!("CARGO_MANIFEST_DIR")).join("../build/native-sdk"));
    let bin = sdk.join("bin");
    assert!(
        sdk.is_absolute() && bin.join("lyflow_core.dll").is_file(),
        "找不到原生 SDK：{}。运行 pnpm sdk:restore，或把 LYFLOW_NATIVE_SDK 设为包含 bin/lyflow_core.dll 的 SDK 绝对路径",
        sdk.display()
    );
    println!("cargo:rerun-if-changed={}", bin.display());
    let target = target_profile_dir();
    for dest in [&target, &target.join("deps")] {
        stage_runtime(&bin, dest, false).expect("暂存 SDK 运行库失败");
    }
    #[cfg(feature = "desktop")]
    stage_runtime(
        &bin,
        &Path::new(env!("CARGO_MANIFEST_DIR")).join("target/bundle-core"),
        true,
    )
    .expect("暂存桌面运行库失败");
    utf8_manifest_step(&sdk.join("examples/lyflow-utf8.manifest"));
    tauri_step();
}

/// 只复制大小或修改时间与 SDK 不同的文件。python/ 总是与 SDK 对齐，删掉 SDK 里已经没有的文件；
/// `exclusive` 的目录（bundle-core）整个归这里管，顶层多出来的 DLL 也删掉，
/// 其余目录里还有 cargo 自己的产物，只增不删。
fn stage_runtime(bin: &Path, destination: &Path, exclusive: bool) -> io::Result<()> {
    fs::create_dir_all(destination)?;
    let mut staged = HashSet::from([OsString::from("python")]);
    for entry in fs::read_dir(bin)? {
        let entry = entry?;
        let path = entry.path();
        if path
            .extension()
            .is_some_and(|ext| ext.eq_ignore_ascii_case("dll"))
        {
            sync_file(&path, &destination.join(entry.file_name()))?;
            staged.insert(entry.file_name());
        }
    }
    if exclusive {
        prune(destination, &staged)?;
    }
    sync_python(&bin.join("python"), &destination.join("python"))
}

fn sync_python(source: &Path, destination: &Path) -> io::Result<()> {
    fs::create_dir_all(destination)?;
    let mut staged = HashSet::new();
    for entry in fs::read_dir(source)? {
        let entry = entry?;
        let path = entry.path();
        if entry.file_name() == "__pycache__" || entry.file_name() == "tests" {
            continue;
        }
        if entry.file_type()?.is_dir() {
            sync_python(&path, &destination.join(entry.file_name()))?;
        } else if path
            .extension()
            .is_some_and(|ext| ext == "py" || ext == "md" || ext == "toml")
        {
            sync_file(&path, &destination.join(entry.file_name()))?;
        } else {
            continue;
        }
        staged.insert(entry.file_name());
    }
    prune(destination, &staged)
}

fn sync_file(source: &Path, destination: &Path) -> io::Result<()> {
    let from = fs::metadata(source)?;
    let modified = from.modified()?;
    if let Ok(to) = fs::metadata(destination) {
        if to.is_file() && to.len() == from.len() && to.modified()? == modified {
            return Ok(());
        }
    }
    fs::copy(source, destination)?;
    fs::File::options()
        .write(true)
        .open(destination)?
        .set_modified(modified)
}

fn prune(directory: &Path, keep: &HashSet<OsString>) -> io::Result<()> {
    for entry in fs::read_dir(directory)? {
        let entry = entry?;
        if keep.contains(&entry.file_name()) {
            continue;
        }
        if entry.file_type()?.is_dir() {
            fs::remove_dir_all(entry.path())?;
        } else {
            fs::remove_file(entry.path())?;
        }
    }
    Ok(())
}

fn utf8_manifest_step(manifest: &Path) {
    if std::env::var("CARGO_CFG_TARGET_ENV").as_deref() != Ok("msvc") {
        return;
    }
    println!("cargo:rerun-if-changed={}", manifest.display());
    println!("cargo:rustc-link-arg=/MANIFEST:EMBED");
    println!("cargo:rustc-link-arg=/MANIFESTINPUT:{}", manifest.display());
    let app_manifest = app_manifest_path();
    println!("cargo:rerun-if-changed={}", app_manifest.display());
    println!(
        "cargo:rustc-link-arg-tests=/MANIFESTINPUT:{}",
        app_manifest.display()
    );
}

fn app_manifest_path() -> PathBuf {
    PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR"))
        .join("lyflow-app.manifest")
}

/// lyflow-app 在 UTF-8 那份之外再合并 lyflow-app.manifest（Common-Controls v6）。
/// Tauri 自己的资源清单必须关掉（new_without_app_manifest）：一是 link.exe 已经
/// 生成了清单资源，再来一份会撞资源 ID；二是 tauri_build 把清单内联进 .rc 交给
/// rc.exe，按系统 ANSI 代码页转码，936 的机器上中文注释被转坏，exe 报 14001 起不来。
#[cfg(feature = "desktop")]
fn tauri_step() {
    if std::env::var("CARGO_CFG_TARGET_ENV").as_deref() == Ok("msvc") {
        println!(
            "cargo:rustc-link-arg-bin=lyflow-app=/MANIFESTINPUT:{}",
            app_manifest_path().display()
        );
    }
    let windows = tauri_build::WindowsAttributes::new_without_app_manifest();
    tauri_build::try_build(tauri_build::Attributes::new().windows_attributes(windows))
        .expect("tauri_build 失败");
}

/// 关掉 desktop feature 时只构建 CLI，Tauri 一行都不碰（F6）。
#[cfg(not(feature = "desktop"))]
fn tauri_step() {}

fn target_profile_dir() -> PathBuf {
    let out = PathBuf::from(std::env::var("OUT_DIR").expect("OUT_DIR"));
    out.ancestors()
        .nth(3)
        .expect("OUT_DIR 层级异常")
        .to_path_buf()
}
