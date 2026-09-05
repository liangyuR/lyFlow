use std::path::{Path, PathBuf};

/// 递归收集 core 的 .cpp，同时记下走过的目录。
///
/// 目录也要 rerun-if-changed：新增一个 src/ops/xxx.cpp 时目录 mtime 会变，
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
        } else if path.extension().and_then(|s| s.to_str()) == Some("cpp") {
            files.push(path);
        }
    }
}

fn main() {
    // core 直接用 cc 编，不走 CMake：这样 `cargo build` 在只装了 Rust + MSVC
    // 的机器上就能跑通，不必额外准备 CMake。core/CMakeLists.txt 保留给单独
    // 调试 core 用（scripts/build-core.ps1）。
    let core = PathBuf::from("../core");
    let src = core.join("src");
    let include = core.join("include");

    let mut files = Vec::new();
    let mut dirs = Vec::new();
    collect(&src, &mut files, &mut dirs);
    files.sort();
    assert!(!files.is_empty(), "在 {} 下没找到任何 .cpp", src.display());

    for f in &files {
        println!("cargo:rerun-if-changed={}", f.display());
    }
    for d in &dirs {
        println!("cargo:rerun-if-changed={}", d.display());
    }
    println!("cargo:rerun-if-changed={}", include.display());

    let mut build = cc::Build::new();
    build.cpp(true).include(&include).include(&src).files(&files);

    if build.get_compiler().is_like_msvc() {
        // /utf-8 不是可选项：算子的 doc/label 里有中文字面量，
        // 缺了它 MSVC 会按系统代码页解释源文件，manifest 里的中文会变成乱码，
        // 而且是在前端才看得出来的那种乱码。
        build.flag("/std:c++17").flag("/utf-8").flag("/W4").flag("/permissive-");
    } else {
        build.flag("-std=c++17").flag("-Wall").flag("-Wextra");
    }
    build.compile("lyflow_core");

    tauri_build::build();
}
