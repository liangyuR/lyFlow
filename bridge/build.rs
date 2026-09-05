use std::path::{Path, PathBuf};

/// 递归收集需要监听变更的文件与目录。
///
/// 目录本身也要 `rerun-if-changed`：新增一个 `src/ops/xxx.cpp` 时目录 mtime 会变，
/// cargo 才会重跑 build.rs 把新算子编进去。否则「加算子」要手动 touch 才生效，
/// 正好毁掉 ADR-0003 想要的那个流畅反馈循环。
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

/// Ninja 的位置。
///
/// cargo 不像开发者的命令行那样跑过 vcvars，所以 PATH 上通常**没有** ninja ——
/// 只写 `.generator("Ninja")` 的话，在一台干净机器上会得到一句
/// 「CMake was unable to find a build program corresponding to Ninja」。
/// Visual Studio 自带一份 ninja，就在它自带的那个 cmake 旁边：
///   Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe
///   Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe
/// 找不到就退回 CMake 的默认生成器（Visual Studio），慢一点但能用。
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

/// CMake 的 binary dir 一旦用某个生成器配置过，就不能换 —— 换了只会得到
/// 「Does not match the generator used previously」。这种情况下把目录清掉
/// 重来，比让开发者去猜该删哪个目录强。
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
    // ADR-0004：core 是 CMake 构建的 DLL，Rust 用 libloading 运行时加载。
    //
    // 这里**不发任何 rustc-link-lib**：不链 import 库，也就不需要在 build 期
    // 解析 PCL 那二十多个依赖。构建脚本的全部工作是「让 CMake 把 DLL 编出来，
    // 再把整个输出目录拷到 cargo 的 target 下」。
    // 用 CARGO_MANIFEST_DIR 拼绝对路径，**不要** canonicalize()：
    // Windows 上它会返回 `\\?\D:\...` 这种扩展长度路径，而 CMake 的
    // file(GLOB) 在这种路径下一个文件都匹配不到，最后报的是
    // 「No SOURCES given to target」—— 离真正的原因十万八千里。
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

    let mut cfg = cmake::Config::new(&core);
    // D8：core 永远 RelWithDebInfo，**不跟 cargo profile 走**。
    // Rust 总是 /MD，而 vcpkg 的 debug 库是 /MDd —— 混用两种 CRT
    // 不会链接失败，会在完全无关的地方运行时崩溃。
    cfg.profile("RelWithDebInfo")
        .define("CMAKE_BUILD_TYPE", "RelWithDebInfo")
        // cmake crate 默认会 `--build . --target install`。core 没有 install 规则；
        // 而且这里只需要 DLL —— dump-manifest 和 core-tests 两个 exe 归
        // scripts/build-core.ps1 管，在这儿再编一遍只是白等。
        // vcpkg 的 applocal 对 SHARED 目标同样生效，PCL 那堆依赖 DLL 照样会
        // 被拷到 bin/ 旁边。
        .build_target("lyflow_core");
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

    // CMake 的所有产物（lyflow_core.dll、两个 exe，以及 vcpkg applocal 拷来的
    // PCL/boost/flann/lz4 等依赖）都在 <build>/bin。整目录拷到 target/<profile>/
    // 和它的 deps/ —— 前者给 tauri dev / tauri build 用，后者给 cargo test 用
    // （测试可执行文件跑在 deps/ 里，LoadLibrary 只看它自己那个目录）。
    let bin = out.join("build").join("bin");
    let bin = if bin.exists() { bin } else { out.join("bin") };
    println!("cargo:rustc-env=LYFLOW_CORE_BIN={}", bin.display());

    let target_dir = target_profile_dir();
    for dest in [target_dir.clone(), target_dir.join("deps")] {
        if let Err(e) = copy_dir_contents(&bin, &dest) {
            println!("cargo:warning=拷贝 core 运行时到 {} 失败: {e}", dest.display());
        }
    }

    // D9：给 exe 贴一份带 activeCodePage=UTF-8 的清单。
    //
    // 注意 app_manifest 是**整份替换**而不是合并（tauri-build 2.6.3 的
    // `res.set_manifest`），所以 lyflow-app.manifest 里必须自带 Tauri 原来的
    // Common-Controls v6 依赖，否则文件对话框会掉回旧样式且不报错。
    println!("cargo:rerun-if-changed=lyflow-app.manifest");
    let windows = tauri_build::WindowsAttributes::new().app_manifest(include_str!(
        "lyflow-app.manifest"
    ));
    tauri_build::try_build(tauri_build::Attributes::new().windows_attributes(windows))
        .expect("tauri_build 失败");
}

/// 从 OUT_DIR 反推 `target/<profile>/`。
///
/// OUT_DIR 形如 `target/debug/build/lyflow-<hash>/out`，往上数四层就是
/// `target/debug`。cargo 没有提供这个路径的官方变量，但这个布局十几年没变过，
/// 而且拷贝失败只是 warning + 运行时报「找不到 lyflow_core.dll」，不是静默错误。
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
        // 目标文件正被占用（比如上一次 tauri dev 还开着）时不要让整个构建挂掉，
        // 旧的那份多半就是同一个文件。
        if let Err(e) = std::fs::copy(&src, &dst) {
            if e.kind() != std::io::ErrorKind::PermissionDenied {
                return Err(e);
            }
        }
    }
    Ok(())
}
