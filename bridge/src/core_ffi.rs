//! C ABI 边界的 Rust 侧：加载 DLL、调用、把 C 的堆内存拷成 Rust 的、立刻还回去。
//! 所有调用都经 `Core` 里那张函数表，热重载因此只是换一个 Arc<Core>（ADR-0004/0009）。

use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int, c_void};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::{Arc, OnceLock, RwLock};

use libloading::{Library, Symbol};

// ------------- C ABI 的类型镜像，与 core/include/lyflow/c_api.h 一一对应

pub type EventCb = unsafe extern "C" fn(*const c_char, *mut c_void);

/// C ABI 的版本号。与 core/include/lyflow/c_api.h 的 LYFLOW_ABI_VERSION 必须一致。
pub const ABI_VERSION: u32 = 9;

/// 运行时注入一个源节点的输出（v7）。缓冲由调用方持有到 `lyflow_run_start` 返回。
#[repr(C)]
pub struct RunInputRaw {
    pub node_id: *const c_char,
    pub port: *const c_char,
    pub kind: i32,
    pub count: u32,
    pub xyz: *const f32,
    pub intensity: *const f32,
    pub normals: *const f32,
    pub rgb: *const u8,
}

pub const INPUT_POINT_CLOUD: i32 = 0;

#[repr(C)]
pub struct RunOptionsRaw {
    pub run_id: *const c_char,
    pub base_dir: *const c_char,
    pub targets: *const *const c_char,
    pub target_count: usize,
    pub max_parallel: i32,
    pub cache_budget_bytes: u64,
    pub mode: i32,
    pub preview_max_points: u32,
    pub preview_budget_ms: u32,
    pub no_reuse: i32,
    pub inputs: *const RunInputRaw,
    pub input_count: usize,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct CloudViewRaw {
    pub point_count: u32,
    pub total_points: u32,
    pub flags: u32,
    pub reserved: u32,
    pub bounds: [f32; 6],
    pub xyz: *const f32,
    pub intensity: *const f32,
    pub normals: *const f32,
    pub handle: *mut c_void,
}

pub const CLOUD_HAS_INTENSITY: u32 = 1;
pub const CLOUD_HAS_NORMALS: u32 = 2;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct TensorViewRaw {
    pub rank: u32,
    pub count: u32,
    pub offset: u64,
    pub total: u64,
    pub shape: *const i64,
    pub data: *const f32,
    pub handle: *mut c_void,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct IndicesViewRaw {
    pub count: u32,
    pub total: u32,
    pub source_cloud_id: u64,
    pub values: *const i32,
    pub handle: *mut c_void,
}

/// 一次运行注入的一片点云（v7）。Rust 侧持有缓冲，转成 RunInputRaw 交给 core。
#[derive(Clone, Debug)]
pub struct RunInput {
    pub node_id: String,
    pub port: String,
    /// 交错的 x,y,z。其余三个通道要么为空，要么与点数对齐。
    pub xyz: Vec<f32>,
    pub intensity: Vec<f32>,
    pub normals: Vec<f32>,
    pub rgb: Vec<u8>,
}

/// 一次运行的全部选项（C ABI v7）。写成位置参数就没人读得懂了。
#[derive(Clone)]
pub struct RunSpec<'a> {
    pub graph_json: &'a str,
    pub run_id: &'a str,
    pub base_dir: &'a str,
    pub targets: &'a [String],
    /// 0 = min(4, 核数)。
    pub max_parallel: i32,
    /// 0 = full，1 = preview（ADR-0011）。
    pub mode: i32,
    pub preview_max_points: u32,
    pub preview_budget_ms: u32,
    /// 本次运行不复用结果仓里的旧结果。不动别的 run 的结果（对比 `cache_clear`）。
    pub no_reuse: bool,
    /// 运行时注入的源数据（ADR-0017）。
    pub inputs: &'a [RunInput],
}

impl<'a> RunSpec<'a> {
    pub fn new(
        graph_json: &'a str,
        run_id: &'a str,
        base_dir: &'a str,
        targets: &'a [String],
    ) -> Self {
        RunSpec {
            graph_json,
            run_id,
            base_dir,
            targets,
            max_parallel: 0,
            mode: 0,
            preview_max_points: 0,
            preview_budget_ms: 0,
            no_reuse: false,
            inputs: &[],
        }
    }
}

/// `lyflow_run` 是 core 内部类型，这边只当成不透明指针。
#[repr(C)]
pub struct RunOpaque {
    _private: [u8; 0],
}

#[derive(Debug, thiserror::Error)]
pub enum CoreError {
    #[error("加载 lyflow_core.dll 失败: {0}\n（开发构建找本配置的 LYFLOW_CORE_BIN，其余情况找 exe 同目录）")]
    Load(String),
    #[error("lyflow_core.dll 里找不到符号 {0}（DLL 版本与本程序不匹配）")]
    MissingSymbol(&'static str),
    #[error("core 返回了空指针（内存分配失败）")]
    NullReturn,
    #[error("core 返回的不是合法 UTF-8: {0}")]
    NotUtf8(#[from] std::str::Utf8Error),
    #[error("参数里有 NUL 字节: {0}")]
    NulInArgument(#[from] std::ffi::NulError),
    #[error("core 没有该结果（节点没跑完，或该输出不是点云）")]
    NoSuchOutput,
}

/// 函数表的签名。写成 alias 是因为取符号的宏需要一个具名类型
/// （`Symbol<_>` 推不出来，`into_raw()` 断了推导链）。
type FnVersion = unsafe extern "C" fn() -> *const c_char;
type FnJson = unsafe extern "C" fn() -> *mut c_char;
type FnStringFree = unsafe extern "C" fn(*mut c_char);
type FnValidate = unsafe extern "C" fn(*const c_char, *const c_char) -> *mut c_char;
type FnPlan = unsafe extern "C" fn(
    *const c_char,
    *const c_char,
    *const *const c_char,
    usize,
) -> *mut c_char;
type FnVoid = unsafe extern "C" fn();
type FnRunStart = unsafe extern "C" fn(
    *const c_char,
    *const RunOptionsRaw,
    Option<EventCb>,
    *mut c_void,
) -> *mut RunOpaque;
type FnRunOp = unsafe extern "C" fn(*mut RunOpaque);
type FnOutputCloud = unsafe extern "C" fn(
    *const c_char,
    *const c_char,
    *const c_char,
    u32,
    *mut CloudViewRaw,
) -> c_int;
type FnCloudViewFree = unsafe extern "C" fn(*mut CloudViewRaw);
type FnOutputTensor = unsafe extern "C" fn(
    *const c_char,
    *const c_char,
    *const c_char,
    u64,
    u32,
    *mut TensorViewRaw,
) -> c_int;
type FnTensorViewFree = unsafe extern "C" fn(*mut TensorViewRaw);
type FnOutputIndices = unsafe extern "C" fn(
    *const c_char,
    *const c_char,
    *const c_char,
    u64,
    u32,
    *mut IndicesViewRaw,
) -> c_int;
type FnIndicesViewFree = unsafe extern "C" fn(*mut IndicesViewRaw);
type FnOutputInfo = unsafe extern "C" fn(*const c_char, *const c_char) -> *mut c_char;
type FnRunOutputs = unsafe extern "C" fn(*const c_char) -> *mut c_char;
type FnRunSummary = unsafe extern "C" fn(*const c_char) -> *mut c_char;
type FnImport =
    unsafe extern "C" fn(*const c_char, *const c_char, *const c_char) -> *mut c_char;
type FnSetLibraryDirs = unsafe extern "C" fn(*const *const c_char, usize) -> *mut c_char;
type FnLibraryCount = unsafe extern "C" fn() -> usize;
type FnOutputSave = unsafe extern "C" fn(
    *const c_char,
    *const c_char,
    *const c_char,
    *const c_char,
    *const c_char,
) -> *mut c_char;

/// 加载好的 core，外加一张函数表。存的是裸函数指针而不是借用 `Library` 的
/// `Symbol<'a>`（自引用结构难写对），由 `Arc<Core>` 保证 lib 活得比调用久。
pub struct Core {
    // 顺序有意义：把 lib 放最后，让「DLL 先于函数指针卸载」在类型层面不可能发生。
    version: FnVersion,
    manifest_json: FnJson,
    manifest_problems: FnJson,
    string_free: FnStringFree,
    validate: FnValidate,
    plan: FnPlan,
    effective_params: FnValidate,
    cache_clear: FnVoid,
    cache_stats: FnJson,
    run_start: FnRunStart,
    run_cancel: FnRunOp,
    run_join: FnRunOp,
    run_free: FnRunOp,
    output_cloud: FnOutputCloud,
    cloud_view_free: FnCloudViewFree,
    output_tensor: FnOutputTensor,
    tensor_view_free: FnTensorViewFree,
    output_indices: FnOutputIndices,
    indices_view_free: FnIndicesViewFree,
    output_info: FnOutputInfo,
    run_outputs: FnRunOutputs,
    run_summary: FnRunSummary,
    import: FnImport,
    output_save: FnOutputSave,
    set_library_dirs: FnSetLibraryDirs,
    library_count: FnLibraryCount,
    #[allow(dead_code)]
    lib: Library,
}

// Core 里只有函数指针和 Library。core 侧的每个入口自己加锁（结果仓有 mutex，
// Run 有 atomic），所以跨线程共享是安全的。
unsafe impl Send for Core {}
unsafe impl Sync for Core {}

macro_rules! sym {
    ($lib:expr, $name:literal, $ty:ty) => {{
        let s: Symbol<$ty> = unsafe { $lib.get(concat!($name, "\0").as_bytes()) }
            .map_err(|_| CoreError::MissingSymbol($name))?;
        unsafe { *s.into_raw() }
    }};
}

/// Windows：绝对路径 + LOAD_WITH_ALTERED_SEARCH_PATH，PCL / yaml-cpp 那些依赖
/// 从 DLL 自己的目录解析，而不是 exe 目录 —— 开发期这两处装的不是同一套。
#[cfg(windows)]
fn open_library(path: &Path) -> Result<Library, libloading::Error> {
    use libloading::os::windows::{Library as WinLibrary, LOAD_WITH_ALTERED_SEARCH_PATH};
    let abs = if path.is_absolute() {
        path.to_path_buf()
    } else {
        std::env::current_dir().unwrap_or_default().join(path)
    };
    unsafe { WinLibrary::load_with_flags(&abs, LOAD_WITH_ALTERED_SEARCH_PATH) }.map(Library::from)
}

#[cfg(not(windows))]
fn open_library(path: &Path) -> Result<Library, libloading::Error> {
    unsafe { Library::new(path) }
}

impl Core {
    fn load_from(path: &Path) -> Result<Self, CoreError> {
        let lib = open_library(path)
            .map_err(|e| CoreError::Load(format!("{} —— {e}", path.display())))?;

        Ok(Core {
            version: sym!(lib, "lyflow_version", FnVersion),
            manifest_json: sym!(lib, "lyflow_manifest_json", FnJson),
            manifest_problems: sym!(lib, "lyflow_manifest_problems", FnJson),
            string_free: sym!(lib, "lyflow_string_free", FnStringFree),
            validate: sym!(lib, "lyflow_validate", FnValidate),
            plan: sym!(lib, "lyflow_plan", FnPlan),
            // 与 validate 同签名（graph_json, base_dir）→ JSON 文本。
            effective_params: sym!(lib, "lyflow_effective_params", FnValidate),
            cache_clear: sym!(lib, "lyflow_cache_clear", FnVoid),
            cache_stats: sym!(lib, "lyflow_cache_stats", FnJson),
            run_start: sym!(lib, "lyflow_run_start", FnRunStart),
            run_cancel: sym!(lib, "lyflow_run_cancel", FnRunOp),
            run_join: sym!(lib, "lyflow_run_join", FnRunOp),
            run_free: sym!(lib, "lyflow_run_free", FnRunOp),
            output_cloud: sym!(lib, "lyflow_output_cloud", FnOutputCloud),
            cloud_view_free: sym!(lib, "lyflow_cloud_view_free", FnCloudViewFree),
            output_tensor: sym!(lib, "lyflow_output_tensor", FnOutputTensor),
            tensor_view_free: sym!(lib, "lyflow_tensor_view_free", FnTensorViewFree),
            output_indices: sym!(lib, "lyflow_output_indices", FnOutputIndices),
            indices_view_free: sym!(lib, "lyflow_indices_view_free", FnIndicesViewFree),
            output_info: sym!(lib, "lyflow_output_info", FnOutputInfo),
            run_outputs: sym!(lib, "lyflow_run_outputs", FnRunOutputs),
            run_summary: sym!(lib, "lyflow_run_summary", FnRunSummary),
            import: sym!(lib, "lyflow_import", FnImport),
            output_save: sym!(lib, "lyflow_output_save", FnOutputSave),
            set_library_dirs: sym!(lib, "lyflow_set_library_dirs", FnSetLibraryDirs),
            library_count: sym!(lib, "lyflow_library_count", FnLibraryCount),
            lib,
        })
    }

    /// 接管 core 返回的堆字符串：拷一份，再用 core 自己的 free 还回去。
    /// 必须用 lyflow_string_free —— 跨 DLL 边界时分配器不是同一个。
    unsafe fn take_owned(&self, ptr: *mut c_char) -> Result<String, CoreError> {
        if ptr.is_null() {
            return Err(CoreError::NullReturn);
        }
        let result = CStr::from_ptr(ptr).to_str().map(str::to_owned);
        (self.string_free)(ptr);
        Ok(result?)
    }

    pub fn version(&self) -> String {
        unsafe {
            CStr::from_ptr((self.version)())
                .to_string_lossy()
                .into_owned()
        }
    }

    pub fn manifest_json(&self) -> Result<String, CoreError> {
        unsafe { self.take_owned((self.manifest_json)()) }
    }

    pub fn manifest_problems(&self) -> Result<Vec<String>, CoreError> {
        let raw = unsafe { self.take_owned((self.manifest_problems)()) }?;
        if raw.is_empty() {
            return Ok(Vec::new());
        }
        Ok(raw.lines().map(str::to_owned).collect())
    }

    /// 只校验不执行。返回诊断 JSON 数组的文本。
    pub fn validate(&self, graph_json: &str, base_dir: &str) -> Result<String, CoreError> {
        let g = CString::new(graph_json)?;
        let b = CString::new(base_dir)?;
        unsafe { self.take_owned((self.validate)(g.as_ptr(), b.as_ptr())) }
    }

    /// 编译一次，报告每节点的 cacheKey 与是否已缓存（ADR-0007）。
    pub fn plan(
        &self,
        graph_json: &str,
        base_dir: &str,
        targets: &[String],
    ) -> Result<String, CoreError> {
        let g = CString::new(graph_json)?;
        let b = CString::new(base_dir)?;
        let owned: Vec<CString> = targets
            .iter()
            .map(|t| CString::new(t.as_str()))
            .collect::<Result<_, _>>()?;
        let ptrs: Vec<*const c_char> = owned.iter().map(|c| c.as_ptr()).collect();
        let head = if ptrs.is_empty() {
            std::ptr::null()
        } else {
            ptrs.as_ptr()
        };
        unsafe { self.take_owned((self.plan)(g.as_ptr(), b.as_ptr(), head, ptrs.len())) }
    }

    /// 每节点每参数的生效值与来源（m6-plan §2）。合并默认值这件事只在 core 做一次 ——
    /// 在这里重算一遍，迟早与执行器真正用的那份漂开。
    pub fn effective_params(&self, graph_json: &str, base_dir: &str) -> Result<String, CoreError> {
        let g = CString::new(graph_json)?;
        let b = CString::new(base_dir)?;
        unsafe { self.take_owned((self.effective_params)(g.as_ptr(), b.as_ptr())) }
    }

    pub fn cache_clear(&self) {
        unsafe { (self.cache_clear)() };
    }

    pub fn cache_stats(&self) -> Result<String, CoreError> {
        unsafe { self.take_owned((self.cache_stats)()) }
    }

    /// 加载之后立刻验一遍契约。坏的一代绝不能顶掉好的那一代（ADR-0009）。
    fn self_check(&self) -> Result<(), String> {
        let problems = self.manifest_problems().map_err(|e| e.to_string())?;
        if !problems.is_empty() {
            return Err(format!("算子描述自检有 {} 条问题：
  - {}", problems.len(),
                               problems.join("
  - ")));
        }
        let raw = self.manifest_json().map_err(|e| e.to_string())?;
        serde_json::from_str::<serde_json::Value>(&raw)
            .map_err(|e| format!("manifest 不是合法 JSON: {e}"))?;
        Ok(())
    }

    /// 重扫库算子目录（ADR-0010）。返回问题列表，空 = 干净。
    /// 调用方必须先放掉所有 RunHandle：它会重建注册表。
    pub fn set_library_dirs(&self, dirs: &[String]) -> Result<Vec<String>, CoreError> {
        let owned: Vec<CString> = dirs
            .iter()
            .map(|d| CString::new(d.as_str()))
            .collect::<Result<_, _>>()?;
        let ptrs: Vec<*const c_char> = owned.iter().map(|c| c.as_ptr()).collect();
        let head = if ptrs.is_empty() {
            std::ptr::null()
        } else {
            ptrs.as_ptr()
        };
        let raw = unsafe { self.take_owned((self.set_library_dirs)(head, ptrs.len())) }?;
        if raw.is_empty() {
            return Ok(Vec::new());
        }
        Ok(raw.lines().map(str::to_owned).collect())
    }

    pub fn library_count(&self) -> usize {
        unsafe { (self.library_count)() }
    }

    /// 把某个输出整份写盘。Err 里是一句人话的失败原因。
    pub fn output_save(
        &self,
        run_id: &str,
        node_id: &str,
        port: &str,
        path: &str,
        format: &str,
    ) -> Result<(), String> {
        let cs = |s: &str| CString::new(s).map_err(|e| e.to_string());
        let (r, n, p, f, fmt) = (cs(run_id)?, cs(node_id)?, cs(port)?, cs(path)?, cs(format)?);
        let message = unsafe {
            self.take_owned((self.output_save)(
                r.as_ptr(),
                n.as_ptr(),
                p.as_ptr(),
                f.as_ptr(),
                fmt.as_ptr(),
            ))
        }
        .map_err(|e| e.to_string())?;
        if message.is_empty() {
            Ok(())
        } else {
            Err(message)
        }
    }

    pub fn output_info(&self, run_id: &str, node_id: &str) -> Result<String, CoreError> {
        let r = CString::new(run_id)?;
        let n = CString::new(node_id)?;
        unsafe { self.take_owned((self.output_info)(r.as_ptr(), n.as_ptr())) }
    }

    /// 图级命名输出（ADR-0017）。返回 `{ 名字: { node, port, type, ... } }` 的 JSON 文本。
    pub fn run_outputs(&self, run_id: &str) -> Result<String, CoreError> {
        let r = CString::new(run_id)?;
        unsafe { self.take_owned((self.run_outputs)(r.as_ptr())) }
    }

    /// 一次运行的 run summary（ADR-0022）。返回
    /// `{ runId, status, durationMs, nodes, outputs, decisions, contractViolations }`。
    /// run 结束之前 core 返回 NULL，这里变成 `Ok(None)` —— 不是错误，只是还没有。
    pub fn run_summary(&self, run_id: &str) -> Result<Option<String>, CoreError> {
        let r = CString::new(run_id)?;
        let raw = unsafe { (self.run_summary)(r.as_ptr()) };
        if raw.is_null() {
            return Ok(None);
        }
        unsafe { self.take_owned(raw) }.map(Some)
    }

    /// 走注册好的导入器把一段文本变成图。Err 里是诊断数组的文本（'[' 开头）。
    pub fn import(&self, kind: &str, text: &str, base_dir: &str) -> Result<String, String> {
        let cs = |s: &str| CString::new(s).map_err(|e| e.to_string());
        let (k, t, b) = (cs(kind)?, cs(text)?, cs(base_dir)?);
        let raw = unsafe { self.take_owned((self.import)(k.as_ptr(), t.as_ptr(), b.as_ptr())) }
            .map_err(|e| e.to_string())?;
        // 成功是 GraphDoc 对象，失败是诊断数组 —— 与 lyflow_plan 同一套区分办法。
        if raw.starts_with('{') {
            Ok(raw)
        } else {
            Err(raw)
        }
    }

    /// 取某节点某端口的点云预览。返回的 `CloudView` 在 Drop 里还给 core。
    pub fn output_cloud(
        self: &Arc<Self>,
        run_id: &str,
        node_id: &str,
        port: &str,
        max_points: u32,
    ) -> Result<CloudView, CoreError> {
        let r = CString::new(run_id)?;
        let n = CString::new(node_id)?;
        let p = CString::new(port)?;
        let mut raw = CloudViewRaw {
            point_count: 0,
            total_points: 0,
            flags: 0,
            reserved: 0,
            bounds: [0.0; 6],
            xyz: std::ptr::null(),
            intensity: std::ptr::null(),
            normals: std::ptr::null(),
            handle: std::ptr::null_mut(),
        };
        let rc = unsafe {
            (self.output_cloud)(r.as_ptr(), n.as_ptr(), p.as_ptr(), max_points, &mut raw)
        };
        if rc != 0 {
            return Err(CoreError::NoSuchOutput);
        }
        Ok(CloudView {
            raw,
            core: Arc::clone(self),
        })
    }

    pub fn output_tensor(
        self: &Arc<Self>,
        run_id: &str,
        node_id: &str,
        port: &str,
        offset: u64,
        count: u32,
    ) -> Result<TensorView, CoreError> {
        let r = CString::new(run_id)?;
        let n = CString::new(node_id)?;
        let p = CString::new(port)?;
        let mut raw = TensorViewRaw {
            rank: 0,
            count: 0,
            offset: 0,
            total: 0,
            shape: std::ptr::null(),
            data: std::ptr::null(),
            handle: std::ptr::null_mut(),
        };
        let rc = unsafe {
            (self.output_tensor)(r.as_ptr(), n.as_ptr(), p.as_ptr(), offset, count, &mut raw)
        };
        if rc != 0 {
            return Err(CoreError::NoSuchOutput);
        }
        Ok(TensorView {
            raw,
            core: Arc::clone(self),
        })
    }

    pub fn output_indices(
        self: &Arc<Self>,
        run_id: &str,
        node_id: &str,
        port: &str,
        offset: u64,
        count: u32,
    ) -> Result<IndicesView, CoreError> {
        let r = CString::new(run_id)?;
        let n = CString::new(node_id)?;
        let p = CString::new(port)?;
        let mut raw = IndicesViewRaw {
            count: 0,
            total: 0,
            source_cloud_id: 0,
            values: std::ptr::null(),
            handle: std::ptr::null_mut(),
        };
        let rc = unsafe {
            (self.output_indices)(r.as_ptr(), n.as_ptr(), p.as_ptr(), offset, count, &mut raw)
        };
        if rc != 0 {
            return Err(CoreError::NoSuchOutput);
        }
        Ok(IndicesView {
            raw,
            core: Arc::clone(self),
        })
    }
}

/// core 借出来的点云缓冲，Drop 时还回去。桥接层里唯一持有 C++ 指针一段时间的地方，
/// 所以包成 RAII —— 点云动辄几十兆，忘了 free 几次内存就没了。
pub struct CloudView {
    raw: CloudViewRaw,
    core: Arc<Core>,
}

impl CloudView {
    pub fn point_count(&self) -> u32 {
        self.raw.point_count
    }
    pub fn total_points(&self) -> u32 {
        self.raw.total_points
    }
    pub fn flags(&self) -> u32 {
        self.raw.flags
    }
    pub fn bounds(&self) -> [f32; 6] {
        self.raw.bounds
    }
    pub fn has_intensity(&self) -> bool {
        self.raw.flags & CLOUD_HAS_INTENSITY != 0 && !self.raw.intensity.is_null()
    }
    pub fn xyz(&self) -> &[f32] {
        if self.raw.xyz.is_null() {
            return &[];
        }
        unsafe { std::slice::from_raw_parts(self.raw.xyz, self.raw.point_count as usize * 3) }
    }
    pub fn intensity(&self) -> &[f32] {
        if !self.has_intensity() {
            return &[];
        }
        unsafe { std::slice::from_raw_parts(self.raw.intensity, self.raw.point_count as usize) }
    }
    pub fn has_normals(&self) -> bool {
        self.raw.flags & CLOUD_HAS_NORMALS != 0 && !self.raw.normals.is_null()
    }
    pub fn normals(&self) -> &[f32] {
        if !self.has_normals() {
            return &[];
        }
        unsafe { std::slice::from_raw_parts(self.raw.normals, self.raw.point_count as usize * 3) }
    }
}

impl Drop for CloudView {
    fn drop(&mut self) {
        unsafe { (self.core.cloud_view_free)(&mut self.raw) };
    }
}

pub struct TensorView {
    raw: TensorViewRaw,
    core: Arc<Core>,
}

impl TensorView {
    pub fn rank(&self) -> u32 {
        self.raw.rank
    }
    pub fn count(&self) -> u32 {
        self.raw.count
    }
    pub fn offset(&self) -> u64 {
        self.raw.offset
    }
    pub fn total(&self) -> u64 {
        self.raw.total
    }
    pub fn shape(&self) -> &[i64] {
        if self.raw.shape.is_null() || self.raw.rank == 0 {
            return &[];
        }
        unsafe { std::slice::from_raw_parts(self.raw.shape, self.raw.rank as usize) }
    }
    pub fn data(&self) -> &[f32] {
        if self.raw.data.is_null() || self.raw.count == 0 {
            return &[];
        }
        unsafe { std::slice::from_raw_parts(self.raw.data, self.raw.count as usize) }
    }

    #[cfg(test)]
    pub unsafe fn borrowed(
        core: Arc<Core>,
        offset: u64,
        total: u64,
        shape: &[i64],
        data: &[f32],
    ) -> Self {
        TensorView {
            raw: TensorViewRaw {
                rank: shape.len() as u32,
                count: data.len() as u32,
                offset,
                total,
                shape: shape.as_ptr(),
                data: data.as_ptr(),
                handle: std::ptr::null_mut(),
            },
            core,
        }
    }
}

impl Drop for TensorView {
    fn drop(&mut self) {
        unsafe { (self.core.tensor_view_free)(&mut self.raw) };
    }
}

pub struct IndicesView {
    raw: IndicesViewRaw,
    core: Arc<Core>,
}

impl IndicesView {
    pub fn count(&self) -> u32 {
        self.raw.count
    }
    pub fn total(&self) -> u32 {
        self.raw.total
    }
    pub fn source_cloud_id(&self) -> u64 {
        self.raw.source_cloud_id
    }
    pub fn values(&self) -> &[i32] {
        if self.raw.values.is_null() || self.raw.count == 0 {
            return &[];
        }
        unsafe { std::slice::from_raw_parts(self.raw.values, self.raw.count as usize) }
    }
}

impl Drop for IndicesView {
    fn drop(&mut self) {
        unsafe { (self.core.indices_view_free)(&mut self.raw) };
    }
}

// --------------------------------------------------------------------- RunHandle

/// 一次运行的句柄。C ABI 契约：start → (cancel)* → join → free，
/// join 返回后不再回调 —— `user` 指针因此由本结构体持有，Drop 里 join 完才释放。
pub struct RunHandle {
    handle: *mut RunOpaque,
    core: Arc<Core>,
    /// 回调的 user 指针。join 之后才允许 drop。
    user: *mut c_void,
    /// 用来释放 user 的函数。类型擦除，免得 RunHandle 变成泛型。
    drop_user: unsafe fn(*mut c_void),
    /// join 只能真正发生一次。用 Mutex 而不是 AtomicBool：第二个调用者必须
    /// 等第一个 join 返回，否则「join 返回后不再回调」对它不成立。
    joined: std::sync::Mutex<bool>,
    run_id: String,
}

// 跨线程共享的依据：cancel 是 relaxed 原子写；join 在 core 侧自己持一把 mutex，
// 这一侧的 Mutex<bool> 只是省掉重复进 FFI；free 只在 Drop 里发生。
unsafe impl Send for RunHandle {}
unsafe impl Sync for RunHandle {}

impl RunHandle {
    /// 启动一次运行。`user` 由本函数接管，join 之后才会被释放。调用者须保证
    /// `cb` 能安全地在 core 的工作线程上被调用，且只解引用 `user`。
    pub unsafe fn start<T>(
        core: Arc<Core>,
        spec: RunSpec<'_>,
        cb: EventCb,
        user: Box<T>,
    ) -> Result<Self, CoreError> {
        let graph = CString::new(spec.graph_json)?;
        let rid = CString::new(spec.run_id)?;
        let base = CString::new(spec.base_dir)?;
        let target_cstrings: Vec<CString> = spec
            .targets
            .iter()
            .map(|t| CString::new(t.as_str()))
            .collect::<Result<_, _>>()?;
        let target_ptrs: Vec<*const c_char> =
            target_cstrings.iter().map(|c| c.as_ptr()).collect();

        // node_id / port 的 CString 与点云缓冲都必须活到 run_start 返回：core 在里面拷贝。
        let input_names: Vec<(CString, CString)> = spec
            .inputs
            .iter()
            .map(|i| Ok((CString::new(i.node_id.as_str())?, CString::new(i.port.as_str())?)))
            .collect::<Result<_, std::ffi::NulError>>()?;
        let input_raw: Vec<RunInputRaw> = spec
            .inputs
            .iter()
            .zip(input_names.iter())
            .map(|(i, (node, port))| RunInputRaw {
                node_id: node.as_ptr(),
                port: port.as_ptr(),
                kind: INPUT_POINT_CLOUD,
                count: (i.xyz.len() / 3) as u32,
                xyz: i.xyz.as_ptr(),
                intensity: if i.intensity.is_empty() {
                    std::ptr::null()
                } else {
                    i.intensity.as_ptr()
                },
                normals: if i.normals.is_empty() {
                    std::ptr::null()
                } else {
                    i.normals.as_ptr()
                },
                rgb: if i.rgb.is_empty() {
                    std::ptr::null()
                } else {
                    i.rgb.as_ptr()
                },
            })
            .collect();

        let options = RunOptionsRaw {
            run_id: rid.as_ptr(),
            base_dir: base.as_ptr(),
            targets: if target_ptrs.is_empty() {
                std::ptr::null()
            } else {
                target_ptrs.as_ptr()
            },
            target_count: target_ptrs.len(),
            max_parallel: spec.max_parallel,
            cache_budget_bytes: 0,
            mode: spec.mode,
            preview_max_points: spec.preview_max_points,
            preview_budget_ms: spec.preview_budget_ms,
            no_reuse: i32::from(spec.no_reuse),
            inputs: if input_raw.is_empty() {
                std::ptr::null()
            } else {
                input_raw.as_ptr()
            },
            input_count: input_raw.len(),
        };

        let user_ptr = Box::into_raw(user) as *mut c_void;
        unsafe fn drop_user<T>(p: *mut c_void) {
            drop(Box::from_raw(p as *mut T));
        }

        let handle = (core.run_start)(graph.as_ptr(), &options, Some(cb), user_ptr);
        if handle.is_null() {
            drop_user::<T>(user_ptr);
            return Err(CoreError::NullReturn);
        }
        Ok(RunHandle {
            handle,
            core,
            user: user_ptr,
            drop_user: drop_user::<T>,
            joined: std::sync::Mutex::new(false),
            run_id: spec.run_id.to_string(),
        })
    }

    pub fn run_id(&self) -> &str {
        &self.run_id
    }

    pub fn cancel(&self) {
        unsafe { (self.core.run_cancel)(self.handle) };
    }

    pub fn join(&self) {
        let mut joined = self.joined.lock().unwrap_or_else(|e| e.into_inner());
        if *joined {
            return;
        }
        unsafe { (self.core.run_join)(self.handle) };
        *joined = true;
    }
}

impl Drop for RunHandle {
    fn drop(&mut self) {
        // cancel → join → free 的顺序不能变：free 会释放该 run 在结果仓的
        // 全部结果，而工作线程还可能正在往里写。
        self.cancel();
        self.join();
        unsafe {
            (self.core.run_free)(self.handle);
            // 到这里 core 保证不会再回调，user 才可以走。
            (self.drop_user)(self.user);
        }
    }
}

// ----------------------------------------------------------------- 进程级单例

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
