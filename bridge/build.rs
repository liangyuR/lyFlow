use std::path::{Path, PathBuf};

fn main() {
    println!("cargo:rerun-if-env-changed=LYFLOW_NATIVE_SDK");
    let sdk = std::env::var_os("LYFLOW_NATIVE_SDK")
        .map(PathBuf::from)
        .expect("请先按 docs/getting-started.md 恢复原生 SDK 并设置 LYFLOW_NATIVE_SDK");
    let bin = sdk.join("bin");
    assert!(
        sdk.is_absolute() && bin.join("lyflow_core.dll").is_file(),
        "LYFLOW_NATIVE_SDK 应指向包含 bin/lyflow_core.dll 的 SDK 绝对路径"
    );
    println!("cargo:rerun-if-changed={}", bin.display());
    let target = target_profile_dir();
    for dest in [&target, &target.join("deps")] {
        stage_runtime(&bin, dest).expect("暂存 SDK 运行库失败");
    }
    #[cfg(feature = "desktop")]
    {
        let bundle = Path::new(env!("CARGO_MANIFEST_DIR")).join("target/bundle-core");
        if bundle.exists() {
            std::fs::remove_dir_all(&bundle).expect("清理桌面运行库暂存目录失败");
        }
        stage_runtime(&bin, &bundle).expect("暂存桌面运行库失败");
    }
    utf8_manifest_step(&sdk.join("examples/lyflow-utf8.manifest"));
    tauri_step();
}

fn stage_runtime(bin: &Path, destination: &Path) -> std::io::Result<()> {
    std::fs::create_dir_all(destination)?;
    for entry in std::fs::read_dir(bin)? {
        let entry = entry?;
        let path = entry.path();
        if path
            .extension()
            .is_some_and(|ext| ext.eq_ignore_ascii_case("dll"))
        {
            std::fs::copy(&path, destination.join(entry.file_name()))?;
        }
    }
    stage_python(&bin.join("python"), destination)
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

fn stage_python(source: &Path, destination: &Path) -> std::io::Result<()> {
    println!("cargo:rerun-if-changed={}", source.display());
    fn copy_tree(source: &Path, destination: &Path) -> std::io::Result<()> {
        std::fs::create_dir_all(destination)?;
        for entry in std::fs::read_dir(source)? {
            let entry = entry?;
            let path = entry.path();
            if entry.file_name() == "__pycache__" || entry.file_name() == "tests" {
                continue;
            }
            if entry.file_type()?.is_dir() {
                copy_tree(&path, &destination.join(entry.file_name()))?;
            } else if path
                .extension()
                .is_some_and(|ext| ext == "py" || ext == "md" || ext == "toml")
            {
                std::fs::copy(path, destination.join(entry.file_name()))?;
            }
        }
        Ok(())
    }
    copy_tree(source, &destination.join("python"))
}
