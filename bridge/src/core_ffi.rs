//! 进程级的 core：DLL 路径解析、单例、热重载（ADR-0004 / ADR-0009）。
//!
//! C ABI 边界本身在 `lyflow-client` crate 里 —— 它是 `lyflow/client.hpp` 的 Rust 对应物
//! （docs/embedding.md）：不含路径解析、不含全局状态、没有 build.rs，宿主自己给 DLL 路径。
//! 这里把它整个 re-export 出去，桥接层其余模块写 `core_ffi::Core` / `core_ffi::RunSpec`
//! 照旧，不用关心它住在哪个 crate。

use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::{Arc, OnceLock, RwLock};

pub use lyflow_client::*;

const DLL_NAME: &str = if cfg!(windows) {
    "lyflow_core.dll"
} else {
    "liblyflow_core.so"
};

/// exe 同目录。不搜 PATH（会加载到无关的同名 DLL），也不看工作目录
/// （双击启动时那不是安装目录）。
fn exe_dir() -> PathBuf {
    std::env::current_exe()
        .ok()
        .and_then(|p| p.parent().map(PathBuf::from))
        .unwrap_or_else(|| PathBuf::from("."))
}

/// 开发构建先用本配置自己的 cmake 产物（build.rs 的 `LYFLOW_CORE_BIN`）：cargo 按
/// feature 集把同一个 crate 建好几遍，而 target/<profile>/ 是共用的，谁后拷谁说了算。
fn dll_path() -> PathBuf {
    if cfg!(debug_assertions) {
        let own = Path::new(env!("LYFLOW_CORE_BIN")).join(DLL_NAME);
        if own.is_file() {
            return own;
        }
    }
    exe_dir().join(DLL_NAME)
}

/// 开发期热重载的源头：`scripts/core-watch.ps1` 就往这里构建（ADR-0009）。
/// 安装包里这个路径不存在，watcher 于是不启动。
pub fn watch_source() -> Option<PathBuf> {
    let repo = Path::new(env!("CARGO_MANIFEST_DIR")).parent()?;
    let path = repo.join("build").join("core").join("bin").join(DLL_NAME);
    path.exists().then_some(path)
}

/// 带 pid：`deps/` 是所有 cargo 测试进程共用的，重名会撞上另一个进程还映射着的
/// 那一份，copy 直接 os error 32（sharing violation）。
fn generation_path(n: u32) -> PathBuf {
    let stem = DLL_NAME.trim_end_matches(".dll").trim_end_matches(".so");
    let ext = if cfg!(windows) { "dll" } else { "so" };
    exe_dir().join(format!("{stem}.gen{n}.p{}.{ext}", std::process::id()))
}

/// 上一代的 gen DLL 还被自己锁着，删不掉是常态 —— 所以清理放在下次启动。
pub fn cleanup_old_generations() -> usize {
    cleanup_generations_in(&exe_dir())
}

/// 还被哪个进程映射着的那些删不掉，`remove_file` 会失败，跳过就是了。
fn cleanup_generations_in(dir: &Path) -> usize {
    let stem = DLL_NAME.trim_end_matches(".dll").trim_end_matches(".so");
    let prefix = format!("{stem}.gen");
    let mut removed = 0;
    let Ok(entries) = std::fs::read_dir(dir) else {
        return 0;
    };
    for entry in entries.flatten() {
        let name = entry.file_name().to_string_lossy().into_owned();
        if name.starts_with(&prefix) && std::fs::remove_file(entry.path()).is_ok() {
            removed += 1;
        }
    }
    removed
}

struct Slot {
    current: RwLock<Result<Arc<Core>, String>>,
    generation: AtomicU32,
}

fn slot() -> &'static Slot {
    static SLOT: OnceLock<Slot> = OnceLock::new();
    SLOT.get_or_init(|| Slot {
        current: RwLock::new(
            Core::load_from(&dll_path())
                .map(Arc::new)
                .map_err(|e| e.to_string()),
        ),
        generation: AtomicU32::new(0),
    })
}

/// 进程内当前那一份 core。热重载只是换掉这个 Arc（ADR-0004 / ADR-0009）。
pub fn core() -> Result<Arc<Core>, String> {
    slot()
        .current
        .read()
        .unwrap_or_else(|e| e.into_inner())
        .clone()
}

/// 当前是第几代。manifest 缓存靠它判断要不要重读。
pub fn generation() -> u32 {
    slot().generation.load(Ordering::Acquire)
}

/// 换一代 core：复制成 `lyflow_core.gen<N>.dll` 再加载（E4，Windows 会锁住原文件）。
/// 自检不过就保留旧代返回 Err —— 半坏的一代比旧的一代难查得多。
pub fn reload_from(source: &Path) -> Result<u32, String> {
    // 名字带 pid 之后每个进程留下自己的一份，不顺手扫 deps/ 会越攒越多
    cleanup_generations_in(&exe_dir());
    let next = generation().wrapping_add(1);
    let staged = generation_path(next);
    std::fs::copy(source, &staged)
        .map_err(|e| format!("复制 {} → {} 失败: {e}", source.display(), staged.display()))?;

    let candidate = Core::load_from(&staged).map_err(|e| e.to_string())?;
    candidate.self_check()?;

    let mut guard = slot().current.write().unwrap_or_else(|e| e.into_inner());
    // 旧的 Arc<Core> 在这一行被丢掉。调用方必须已经放掉所有 RunHandle，
    // 否则旧 DLL 只是引用计数没归零，不会真的卸载。
    *guard = Ok(Arc::new(candidate));
    slot().generation.store(next, Ordering::Release);
    Ok(next)
}

// 下面四个是给不需要拿 Arc 的调用点用的便捷包装。

pub fn version() -> String {
    core().map(|c| c.version()).unwrap_or_default()
}

pub fn manifest_json() -> Result<String, String> {
    core()?.manifest_json().map_err(|e| e.to_string())
}

pub fn manifest_problems() -> Result<Vec<String>, String> {
    core()?.manifest_problems().map_err(|e| e.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    /// 契约的核心断言：C++ 侧的自检必须是干净的。
    /// 挂了说明有人写错了算子描述，在这里失败远好过在前端表现成怪现象。
    #[test]
    fn core_self_check_is_clean() {
        let problems = manifest_problems().expect("读不到自检结果");
        assert!(problems.is_empty(), "算子描述自检有问题: {problems:#?}");
    }

    #[test]
    fn manifest_is_valid_json_with_expected_shape() {
        let raw = manifest_json().expect("读不到 manifest");
        let v: serde_json::Value = serde_json::from_str(&raw).expect("manifest 不是合法 JSON");

        assert_eq!(v["schemaVersion"], 1);
        let ops = v["operators"].as_array().expect("operators 不是数组");
        assert!(ops.len() >= 15, "M2 应当有 15 个算子，实际 {}", ops.len());

        // 每个算子引用的端口类型都必须在类型表里 —— C++ 侧 validate() 已经查过，
        // 这里再查一遍是为了确认「查过的那份」和「导出的那份」是同一份。
        let types: Vec<&str> = v["types"]
            .as_array()
            .expect("types 不是数组")
            .iter()
            .map(|t| t["name"].as_str().unwrap())
            .collect();
        for op in ops {
            for side in ["inputs", "outputs"] {
                for port in op[side].as_array().unwrap() {
                    let ty = port["type"].as_str().unwrap();
                    assert!(types.contains(&ty), "{} 的端口类型 {ty} 不在类型表里", op["id"]);
                }
            }
        }
        // D10：PointCloudXYZI 已经删除，Plane 已经加入
        assert!(!types.contains(&"PointCloudXYZI"));
        assert!(types.contains(&"Plane"));
        // 2D 量测域的六种通用载荷（ADR-0013）。算子包的端口全指着它们，
        // 少一个的表现是「包里的算子注册不上」，而报错离现场很远。
        for ty in [
            "Box2D",
            "Line2D",
            "Circle2D",
            "Point2D",
            "Measurement",
            "Record",
            "Tensor",
        ] {
            assert!(types.contains(&ty), "类型表里少了 {ty}");
        }
        // 每个类型都得有颜色：前端给端口和连线着色时没有兜底
        for t in v["types"].as_array().unwrap() {
            let color = t["color"].as_str().unwrap_or("");
            assert!(
                color.starts_with('#') && color.len() == 7,
                "{} 的颜色不合法: {color:?}",
                t["name"]
            );
        }
    }

    /// /utf-8 编译开关掉了的话，中文 doc 会变成乱码 —— 而且只在前端才看得出来。
    /// 这条测试把它挡在 Rust 层。
    #[test]
    fn chinese_doc_strings_survive_the_ffi_boundary() {
        let raw = manifest_json().unwrap();
        assert!(
            raw.contains("降采样"),
            "manifest 里找不到预期的中文，多半是 MSVC 少了 /utf-8"
        );
    }

    #[test]
    fn version_is_non_empty() {
        assert!(!version().is_empty());
    }

    /// ADR-0009 的一整轮：复制成 gen<N>.dll → 加载 → 自检 → 换掉全局那份。
    /// 复制而不是直接加载，是因为 Windows 锁住已加载的 DLL，CMake 就写不回去了。
    #[test]
    fn hot_reload_swaps_in_a_fresh_generation() {
        let before = generation();
        let old = core().expect("加载 core 失败");
        let old_ops = serde_json::from_str::<serde_json::Value>(&old.manifest_json().unwrap())
            .unwrap()["operators"]
            .as_array()
            .unwrap()
            .len();

        // 源就用当前这份 DLL：本测试要验的是换代机制，不是「新代码生效了没有」
        let generation_id = reload_from(&dll_path()).expect("热重载失败");
        assert_eq!(generation_id, before.wrapping_add(1));
        assert_eq!(generation(), generation_id);
        assert!(
            generation_path(generation_id).exists(),
            "没有生成 {}",
            generation_path(generation_id).display()
        );

        let fresh = core().expect("换代后拿不到 core");
        assert!(!Arc::ptr_eq(&old, &fresh), "还是旧的那一份");
        assert!(fresh.manifest_problems().unwrap().is_empty());
        let fresh_ops = serde_json::from_str::<serde_json::Value>(&fresh.manifest_json().unwrap())
            .unwrap()["operators"]
            .as_array()
            .unwrap()
            .len();
        assert_eq!(fresh_ops, old_ops);

        // 新一代真的能干活：编译一张图并拿到 cacheKey
        let doc = serde_json::json!({
            "schemaVersion": 1, "id": "t",
            "nodes": [{"id": "g", "op": "gen.synthetic"}], "edges": []
        });
        let plan: serde_json::Value =
            serde_json::from_str(&fresh.plan(&doc.to_string(), "", &[]).unwrap()).unwrap();
        assert_eq!(plan[0]["nodeId"], "g");
        assert_eq!(plan[0]["cacheKey"].as_str().unwrap().len(), 32);

        // 旧代的 gen DLL 删不掉是常态（自己还锁着），所以清理只在启动时做一次
        drop(old);
    }

    #[test]
    fn generation_dll_names_are_distinct_and_cleanup_is_safe() {
        assert_ne!(generation_path(1), generation_path(2));
        let name = generation_path(7).file_name().unwrap().to_string_lossy().into_owned();
        assert!(name.contains("gen7"), "{name}");
        // 同一个 deps/ 下可能有别的测试进程在换代，名字必须带 pid 才不会撞上
        assert!(name.contains(&format!("p{}", std::process::id())), "{name}");

        // 扫的是本测试自己的空目录：cleanup 是进程外可见的动作，
        // 对着共用的 deps/ 扫会删掉并行跑着的另一个测试刚落地的那一代。
        let dir = std::env::temp_dir().join(format!("lyflow-gen-cleanup-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        assert_eq!(cleanup_generations_in(&dir), 0);
        std::fs::write(dir.join(generation_path(9).file_name().unwrap()), b"x").unwrap();
        assert_eq!(cleanup_generations_in(&dir), 1);
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn validate_reports_all_diagnostics_not_just_the_first() {
        let core = core().unwrap();
        let doc = serde_json::json!({
            "schemaVersion": 1, "id": "t",
            "nodes": [
                {"id": "a", "op": "gen.synthetic", "params": {"pointCount": -1}},
                {"id": "b", "op": "no.such.op"}
            ],
            "edges": []
        });
        let raw = core.validate(&doc.to_string(), "").unwrap();
        let diags: Vec<serde_json::Value> = serde_json::from_str(&raw).unwrap();
        assert!(diags.len() >= 2, "D5 要求一次返回全部诊断: {raw}");
        assert!(diags.iter().any(|d| d["code"] == "bad_param"));
        assert!(diags.iter().any(|d| d["code"] == "unknown_op"));
    }

    #[test]
    fn validate_accepts_a_good_graph() {
        let core = core().unwrap();
        let doc = serde_json::json!({
            "schemaVersion": 1, "id": "t",
            "nodes": [
                {"id": "a", "op": "gen.synthetic"},
                {"id": "b", "op": "filter.voxel_grid"}
            ],
            "edges": [
                {"id": "e", "from": {"node": "a", "port": "cloud"},
                            "to": {"node": "b", "port": "cloud"}}
            ]
        });
        let raw = core.validate(&doc.to_string(), "").unwrap();
        assert_eq!(raw.trim(), "[]", "干净的图不该有诊断: {raw}");
    }
}
