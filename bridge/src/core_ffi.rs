//! C ABI 边界的 Rust 侧。
//!
//! 这一层只做三件事：调用、把 C 的堆内存拷成 Rust 的、立刻还回去。
//! **不在这里理解算子语义** —— 那是 ADR-0001 划的线：桥接层只转发。

use std::ffi::CStr;
use std::os::raw::c_char;

extern "C" {
    fn lyflow_version() -> *const c_char;
    fn lyflow_manifest_json() -> *mut c_char;
    fn lyflow_manifest_problems() -> *mut c_char;
    fn lyflow_string_free(s: *mut c_char);
}

#[derive(Debug, thiserror::Error)]
pub enum CoreError {
    #[error("core 返回了空指针（内存分配失败）")]
    NullReturn,
    #[error("core 返回的不是合法 UTF-8: {0}")]
    NotUtf8(#[from] std::str::Utf8Error),
}

/// 接管 core 返回的堆字符串：拷贝一份，然后立刻用 core 自己的 free 还回去。
///
/// 必须用 `lyflow_string_free` 而不是 Rust 的 dealloc —— 跨 ABI 边界时
/// 分配器可能不是同一个，用错了在 release 下才崩，还崩在无关的地方。
unsafe fn take_owned(ptr: *mut c_char) -> Result<String, CoreError> {
    if ptr.is_null() {
        return Err(CoreError::NullReturn);
    }
    let result = CStr::from_ptr(ptr).to_str().map(str::to_owned);
    lyflow_string_free(ptr);
    Ok(result?)
}

/// core 的版本号。静态存储，不需要释放。
pub fn version() -> String {
    unsafe {
        CStr::from_ptr(lyflow_version())
            .to_string_lossy()
            .into_owned()
    }
}

/// 全量算子描述的原始 JSON 文本。
pub fn manifest_json() -> Result<String, CoreError> {
    unsafe { take_owned(lyflow_manifest_json()) }
}

/// 注册表自检结果。空 vec = 没问题。
pub fn manifest_problems() -> Result<Vec<String>, CoreError> {
    let raw = unsafe { take_owned(lyflow_manifest_problems()) }?;
    if raw.is_empty() {
        return Ok(Vec::new());
    }
    Ok(raw.lines().map(str::to_owned).collect())
}

#[cfg(test)]
mod tests {
    use super::*;

    /// 契约的核心断言：C++ 侧的自检必须是干净的。
    /// 这条挂了说明有人写错了算子描述 —— 在这里失败远好过在前端表现成怪现象。
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
        assert!(!ops.is_empty(), "一个算子都没注册");

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
}
