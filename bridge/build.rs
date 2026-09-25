use std::path::{Path, PathBuf};

/// 递归收集需要监听变更的文件与目录。目录本身也要 rerun-if-changed：
/// 新增 src/ops/xxx.cpp 时只有目录 mtime 变，否则加算子要手动 touch 才生效。
fn collect(dir: &Path, files: &mut Vec<PathBuf>, dirs: &mut Vec<PathBuf>) {
    dirs.push(dir.to_path_buf());
    let entries = match std::fs::read_dir(dir) {
        Ok(e) => e,
        Err(e) => panic!("无法读取 {}: {e}", dir.display()),
    };
    for entry in entries.flatten() {
        let path = entry.path();
        if path.is_dir() {
            collect(&path, files, dirs);
        } else {
            match path.extension().and_then(|s| s.to_str()) {
                Some("cpp") | Some("h") | Some("hpp") | Some("manifest") => files.push(path),
                _ => {}
            }
        }
    }
}

/// 在 PATH 上找一个可执行文件。
fn which(name: &str) -> Option<PathBuf> {
    let exe = if cfg!(windows) {
        format!("{name}.exe")
    } else {
        name.to_string()
    };
    std::env::var_os("PATH")?
        .to_string_lossy()
        .split(if cfg!(windows) { ';' } else { ':' })
        .map(|dir| Path::new(dir).join(&exe))
        .find(|p| p.is_file())
}

/// Ninja 的位置。cargo 没跑过 vcvars，PATH 上通常没有 ninja；
/// 依次试 LYFLOW_NINJA → PATH → VS 自带的那份 → vswhere，都没有就退回默认生成器。
fn find_ninja() -> Option<PathBuf> {
    if let Some(p) = std::env::var_os("LYFLOW_NINJA").map(PathBuf::from) {
        if p.is_file() {
            return Some(p);
        }
    }
    if let Some(p) = which("ninja") {
        return Some(p);
    }
    if let Some(cmake) = which("cmake") {
        let ninja = cmake
            .parent() // .../CMake/bin
            .and_then(Path::parent) // .../CMake
            .and_then(Path::parent) // .../Microsoft/CMake
            .map(|d| d.join("Ninja").join("ninja.exe"));
        if let Some(n) = ninja.filter(|p| p.is_file()) {
            return Some(n);
        }
    }
    // 最后一招：问 vswhere 要 VS 的安装路径。scripts/build-core.ps1 走的也是这条，
    // 两边找同一个 ninja，才不会出现「命令行能编、cargo 编不了」。
    let vswhere = PathBuf::from(
        std::env::var("ProgramFiles(x86)").unwrap_or_else(|_| "C:/Program Files (x86)".into()),
    )
    .join("Microsoft Visual Studio/Installer/vswhere.exe");
    if !vswhere.is_file() {
        return None;
    }
    let out = std::process::Command::new(&vswhere)
        .args([
            "-latest",
            "-products",
            "*",
            "-requires",
            "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
            "-property",
            "installationPath",
        ])
        .output()
        .ok()?;
    let install = String::from_utf8_lossy(&out.stdout).trim().to_string();
    if install.is_empty() {
        return None;
    }
    let ninja = PathBuf::from(install)
        .join("Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe");
    ninja.is_file().then_some(ninja)
}

/// CMake 的 binary dir 一旦用某个生成器配置过就不能换（「Does not match the
/// generator used previously」）。发现不一致就把目录清掉重来。
fn drop_incompatible_cache(build_dir: &Path, generator: Option<&str>) {
    let cache = build_dir.join("CMakeCache.txt");
    let Ok(text) = std::fs::read_to_string(&cache) else {
        return;
    };
    let cached = text
        .lines()
        .find_map(|l| l.strip_prefix("CMAKE_GENERATOR:INTERNAL="))
        .unwrap_or("");
    let wanted = generator.unwrap_or(cached);
    let toolchain_ok = text.lines().any(|l| {
        l.starts_with("CMAKE_TOOLCHAIN_FILE:") && l.to_ascii_lowercase().contains("vcpkg.cmake")
    });
    if cached != wanted || !toolchain_ok {
        let _ = std::fs::remove_dir_all(build_dir);
    }
}

/// vcpkg 的位置。约定装在 C:\vcpkg，`VCPKG_ROOT` 可以覆盖。
fn vcpkg_toolchain() -> Option<PathBuf> {
    let root = std::env::var("VCPKG_ROOT").unwrap_or_else(|_| "C:/vcpkg".to_string());
    let file = PathBuf::from(root).join("scripts/buildsystems/vcpkg.cmake");
    file.exists().then_some(file)
}

fn main() {
    // ADR-0004：core 是 CMake 构建的 DLL，这里不发任何 rustc-link-lib。
    // 路径别 canonicalize()——扩展长度前缀会让 CMake 的 file(GLOB) 一个文件都匹配不到。
    let core = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR"))
        .parent()
        .expect("bridge 应当有父目录")
        .join("core");
    assert!(core.is_dir(), "找不到 {}", core.display());

    let mut files = Vec::new();
    let mut dirs = Vec::new();
    collect(&core.join("src"), &mut files, &mut dirs);
    collect(&core.join("include"), &mut files, &mut dirs);
    // tests/ 不进监听列表：cargo 只构建 lyflow_core 这一个目标，
    // 改测试不该触发 core 重编（core 的测试由 scripts/build-core.ps1 跑）。
    files.push(core.join("CMakeLists.txt"));
    files.push(core.join("lyflow-utf8.manifest"));
    files.sort();
    for f in &files {
        println!("cargo:rerun-if-changed={}", f.display());
    }
    for d in &dirs {
        println!("cargo:rerun-if-changed={}", d.display());
    }
    println!("cargo:rerun-if-env-changed=VCPKG_ROOT");

    // 仓库内的标准算子包（ADR-0014）。默认 1；LYFLOW_STD_PACKS=0 是纯平台构建。
    // 空串按「没设」处理：PowerShell 里 $env:X="" 留下的是空串而不是删除
    let std_packs = match std::env::var("LYFLOW_STD_PACKS") {
        Ok(v) if !v.trim().is_empty() => v,
        _ => "1".to_string(),
    };
    println!("cargo:rerun-if-env-changed=LYFLOW_STD_PACKS");
    // 纯平台构建里要标准包算子的测试记 ignored（cargo 汇总行里有数），默认构建照跑。
    // Rust 的测试没有运行时 skip，提前 return 会记成 passed（docs/op-packs.md「纯平台构建」）
    println!("cargo:rustc-check-cfg=cfg(std_packs_off)");
    if std_packs == "0" {
        println!("cargo:rustc-cfg=std_packs_off");
    }
    let packs_root = core.parent().expect("core 应当有父目录").join("packs");
    if std_packs != "0" && packs_root.is_dir() {
        let (mut pf, mut pd) = (Vec::new(), Vec::new());
        collect(&packs_root, &mut pf, &mut pd);
        for f in pf.iter().chain(pd.iter()) {
            println!("cargo:rerun-if-changed={}", f.display());
        }
    }

    // 仓库内默认关闭的包按名字打开（ADR-0015），分号分隔的包名。
    let repo_packs = std::env::var("LYFLOW_PACKS").unwrap_or_default();
    println!("cargo:rerun-if-env-changed=LYFLOW_PACKS");
    // onnxruntime 的位置（T9）。缺省 third_party/onnxruntime/，由 std-ml 包自己兜底。
    let ort_root = std::env::var("LYFLOW_ONNXRUNTIME_ROOT").unwrap_or_default();
    println!("cargo:rerun-if-env-changed=LYFLOW_ONNXRUNTIME_ROOT");

    // 算子包（ADR-0013）。包目录也进 rerun 列表，改包里的算子才会重新构建 core。
    let packs = std::env::var("LYFLOW_OP_PACKS").unwrap_or_default();
    println!("cargo:rerun-if-env-changed=LYFLOW_OP_PACKS");
    for dir in packs.split(';').filter(|s| !s.is_empty()) {
        let path = Path::new(dir);
        if !path.is_dir() {
            println!("cargo:warning=算子包目录不存在: {dir}");
            continue;
        }
        let (mut pf, mut pd) = (Vec::new(), Vec::new());
        collect(path, &mut pf, &mut pd);
        for f in pf.iter().chain(pd.iter()) {
            println!("cargo:rerun-if-changed={}", f.display());
        }
        println!("cargo:rerun-if-changed={}", path.join("lyflow_op_pack.cmake").display());
    }

    let mut cfg = cmake::Config::new(&core);
    // D8：core 永远 RelWithDebInfo，不跟 cargo profile 走。
    // Rust 总是 /MD 而 vcpkg 的 debug 库是 /MDd，混用会在无关的地方崩。
    cfg.profile("RelWithDebInfo")
        .define("CMAKE_BUILD_TYPE", "RelWithDebInfo")
        // 只构建 lyflow_core：core 没有 install 规则，两个 exe 归 build-core.ps1 管。
        // vcpkg 的 applocal 对 SHARED 目标同样生效，PCL 的依赖 DLL 照样落到 bin/。
        .build_target("lyflow_core")
        .define("LYFLOW_OP_PACKS", &packs)
        .define("LYFLOW_STD_PACKS", &std_packs)
        .define("LYFLOW_PACKS", &repo_packs)
        .define("LYFLOW_ONNXRUNTIME_ROOT", &ort_root);
    let ninja = find_ninja();
    if let Some(ninja) = &ninja {
        cfg.generator("Ninja").define("CMAKE_MAKE_PROGRAM", ninja);
    } else {
        println!("cargo:warning=没找到 ninja，退回 CMake 默认生成器（会慢不少）");
    }
    let out_dir = PathBuf::from(std::env::var("OUT_DIR").expect("OUT_DIR"));
    drop_incompatible_cache(
        &out_dir.join("build"),
        ninja.as_ref().map(|_| "Ninja"),
    );
    if let Some(toolchain) = vcpkg_toolchain() {
        cfg.define("CMAKE_TOOLCHAIN_FILE", &toolchain);
    } else {
        println!("cargo:warning=没找到 vcpkg 工具链，CMake 将自行寻找 PCL");
    }
    let out = cfg.build();

    // CMake 的产物都在 <build>/bin。开发构建直接从这里加载（下面这个 LYFLOW_CORE_BIN），
    // 再整目录拷到共享的 target/<profile>/ 与 deps/，供 tauri build 与打包取用。
    let bin = out.join("build").join("bin");
    let bin = if bin.exists() { bin } else { out.join("bin") };
    println!("cargo:rustc-env=LYFLOW_CORE_BIN={}", bin.display());

    let target_dir = target_profile_dir();
    for dest in [target_dir.clone(), target_dir.join("deps")] {
        if let Err(e) = copy_dir_contents(&bin, &dest) {
            println!("cargo:warning=拷贝 core 运行时到 {} 失败: {e}", dest.display());
        }
    }

    println!("cargo:rerun-if-changed=lyflow-app.manifest");
    tauri_step();
}

/// D9：给 exe 贴带 activeCodePage=UTF-8 的清单。app_manifest 是整份替换而非合并，
/// 所以 lyflow-app.manifest 必须自带 Common-Controls v6 依赖（bridge/README.md）。
#[cfg(feature = "desktop")]
fn tauri_step() {
    let windows = tauri_build::WindowsAttributes::new().app_manifest(include_str!(
        "lyflow-app.manifest"
    ));
    tauri_build::try_build(tauri_build::Attributes::new().windows_attributes(windows))
        .expect("tauri_build 失败");
}

/// 关掉 desktop feature 时只构建 CLI，Tauri 一行都不碰（F6）。
#[cfg(not(feature = "desktop"))]
fn tauri_step() {}

/// 从 OUT_DIR 反推 `target/<profile>/`（往上数四层）。cargo 没有官方变量，
/// 但拷贝失败只是 warning + 运行时报「找不到 lyflow_core.dll」，不是静默错误。
fn target_profile_dir() -> PathBuf {
    let out = PathBuf::from(std::env::var("OUT_DIR").expect("OUT_DIR"));
    out.ancestors()
        .nth(3)
        .expect("OUT_DIR 层级异常")
        .to_path_buf()
}

fn copy_dir_contents(from: &Path, to: &Path) -> std::io::Result<()> {
    std::fs::create_dir_all(to)?;
    for entry in std::fs::read_dir(from)? {
        let entry = entry?;
        if !entry.file_type()?.is_file() {
            continue;
        }
        let src = entry.path();
        // 只拷运行时需要的东西。.lib/.exp 是链接期产物，跟着走只会让
        // 安装包体积莫名其妙地大一圈。
        match src.extension().and_then(|s| s.to_str()) {
            Some("dll") | Some("exe") | Some("pdb") => {}
            _ => continue,
        }
        let dst = to.join(entry.file_name());
        // 目标文件正被占用（比如上一次 tauri dev 还开着）时不让整个构建挂掉，
        // 但必须说出来 —— 静默跳过等于「悄悄用了别人留下的那一份」。
        if let Err(e) = std::fs::copy(&src, &dst) {
            if e.kind() != std::io::ErrorKind::PermissionDenied {
                return Err(e);
            }
            println!("cargo:warning=被占用，没能覆盖 {}（那里还是旧的一份）", dst.display());
        }
    }
    Ok(())
}
