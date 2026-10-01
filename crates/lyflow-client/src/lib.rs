//! C ABI 边界的 Rust 侧：加载 core DLL、调用、把 C 的堆内存拷成 Rust 的、立刻还回去。
//! 所有调用都经 `Core` 里那张函数表。
//!
//! 这是 `lyflow/client.hpp`（C++ header-only 客户端，见 docs/embedding.md）的 Rust 对应物：
//! 不构建 core、不链接任何库、不含 build.rs，core 是运行时按给定路径加载的 DLL（ADR-0004）。
//! `ABI_VERSION` 必须与 core 的 `LYFLOW_ABI_VERSION` 一致。
//!
//! 进程级单例、DLL 路径解析与热重载**不在这里** —— 那是宿主自己的事，桥接层的那一份在
//! `bridge/src/core_ffi.rs`。

use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int, c_void};
use std::path::Path;
use std::sync::Arc;

use libloading::{Library, Symbol};

// ------------- C ABI 的类型镜像，与 core/include/lyflow/c_api.h 一一对应

pub type EventCb = unsafe extern "C" fn(*const c_char, *mut c_void);

/// C ABI 的版本号。与 core/include/lyflow/c_api.h 的 LYFLOW_ABI_VERSION 必须一致。
pub const ABI_VERSION: u32 = 15;

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

/// 运行时注入一张图像（v15）。另开结构体：`RunInputRaw` 是数组元素，改大小就破坏 ABI。
#[repr(C)]
pub struct RunImageInputRaw {
    pub node_id: *const c_char,
    pub port: *const c_char,
    pub width: u32,
    pub height: u32,
    pub channels: u32,
    pub depth: u32,
    pub row_bytes: u32,
    pub reserved: u32,
    pub pixels: *const u8,
}

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
    /// v10：顶层图参数的取值，JSON 对象；NULL = 全用 default。
    pub params_json: *const c_char,
    /// v11：只运行这些节点（docs/node-run-plan.md R1）；NULL/0 = 普通运行。
    pub isolate: *const *const c_char,
    pub isolate_count: usize,
    /// v11：强制重算这些节点（修订一 V1）；NULL/0 = 没有。
    pub force: *const *const c_char,
    pub force_count: usize,
    /// v15：运行时注入的图像；NULL/0 = 没有。
    pub image_inputs: *const RunImageInputRaw,
    pub image_input_count: usize,
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
    /// 3 * point_count 个 0..255，或 null（v12，加在 handle 之前）。
    pub rgb: *const u8,
    pub handle: *mut c_void,
}

pub const CLOUD_HAS_INTENSITY: u32 = 1;
pub const CLOUD_HAS_NORMALS: u32 = 2;
pub const CLOUD_HAS_RGB: u32 = 4;

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

/// v15：图像的一级、一段行（docs/image-plan.md §5.1）。
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ImageViewRaw {
    pub width: u32,
    pub height: u32,
    pub channels: u32,
    pub depth: u32,
    pub level: u32,
    pub full_width: u32,
    pub full_height: u32,
    pub row_offset: u32,
    pub row_count: u32,
    pub row_bytes: u32,
    pub pixels: *const u8,
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

/// 一次运行注入的一张图像（v15）。像素行主序、通道交错、RGB 顺序、行紧排；
/// `depth` 是每通道字节数（1 = u8，2 = u16，4 = f32）。
#[derive(Clone, Debug)]
pub struct RunImageInput {
    pub node_id: String,
    pub port: String,
    pub width: u32,
    pub height: u32,
    pub channels: u32,
    pub depth: u32,
    pub pixels: Vec<u8>,
}

impl RunImageInput {
    /// 交给 core 的像素指针。`pixels` 比 宽 × 高 × 通道 × 位深 短（或乘出来溢出）时给 null：
    /// core 按紧排读满 `width * channels * depth * height` 字节，短了就是越界读；给 null 则把它当成
    /// 不完整的图像，报在那个节点上（bad_input）。多给的尾巴不读。
    pub fn pixels_ptr(&self) -> *const u8 {
        let need = (self.width as u64)
            .checked_mul(self.height as u64)
            .and_then(|n| n.checked_mul(self.channels as u64))
            .and_then(|n| n.checked_mul(self.depth as u64));
        match need {
            Some(n) if n > 0 && (self.pixels.len() as u64) >= n => self.pixels.as_ptr(),
            _ => std::ptr::null(),
        }
    }
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
    /// 运行时注入的图像（v15）。
    pub image_inputs: &'a [RunImageInput],
    /// 顶层图参数的取值（v10），JSON 对象 `{ 名字: 值 }`。None = 全用图里的 default。
    pub params_json: Option<&'a str>,
    /// 只运行这些节点（v11，docs/node-run-plan.md R1–R2）。空 = 普通运行。给了它 core 就忽略
    /// `targets`、改用同一组 id；上游只许命中缓存，缺结果时整次运行以 upstream_not_ready 失败。
    /// 它们自己照常查缓存，要真跑一遍另给 `force`。
    pub isolate: &'a [String],
    /// 强制重算这些节点（v11，修订一 V1）：跳过缓存、真跑、结果覆盖写回。可与 targets / isolate / preview 组合。
    pub force: &'a [String],
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
            image_inputs: &[],
            params_json: None,
            isolate: &[],
            force: &[],
        }
    }

    /// 给顶层图参数传值：宿主从此传值，而不是改图 JSON（m7-plan J8）。
    pub fn with_params_json(mut self, json: &'a str) -> Self {
        self.params_json = Some(json);
        self
    }
}

/// `lyflow_run` 是 core 内部类型，这边只当成不透明指针。
#[repr(C)]
pub struct RunOpaque {
    _private: [u8; 0],
}

#[derive(Debug, thiserror::Error)]
pub enum CoreError {
    #[error("加载 core DLL 失败: {0}")]
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
type FnValidateParams =
    unsafe extern "C" fn(*const c_char, *const c_char, *const c_char) -> *mut c_char;
type FnPlan = unsafe extern "C" fn(
    *const c_char,
    *const c_char,
    *const *const c_char,
    usize,
) -> *mut c_char;
type FnPlanParams = unsafe extern "C" fn(
    *const c_char,
    *const c_char,
    *const *const c_char,
    usize,
    *const c_char,
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
type FnOutputImage = unsafe extern "C" fn(
    *const c_char,
    *const c_char,
    *const c_char,
    u32,
    u32,
    u32,
    *mut ImageViewRaw,
) -> c_int;
type FnImageViewFree = unsafe extern "C" fn(*mut ImageViewRaw);
type FnOutputInfo = unsafe extern "C" fn(*const c_char, *const c_char) -> *mut c_char;
type FnRunOutputs = unsafe extern "C" fn(*const c_char) -> *mut c_char;
type FnRunSummary = unsafe extern "C" fn(*const c_char) -> *mut c_char;
type FnImport =
    unsafe extern "C" fn(*const c_char, *const c_char, *const c_char) -> *mut c_char;
type FnSetLibraryDirs = unsafe extern "C" fn(*const *const c_char, usize) -> *mut c_char;
type FnLibraryCount = unsafe extern "C" fn() -> usize;
type FnLibraryDefinition = unsafe extern "C" fn(*const c_char) -> *mut c_char;
type FnCacheSetDir = unsafe extern "C" fn(*const c_char, *const c_char) -> *mut c_char;
type FnCacheEvict = unsafe extern "C" fn(
    *const c_char,
    *const c_char,
    *const *const c_char,
    usize,
    c_int,
    *const c_char,
) -> *mut c_char;
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
    validate_params: FnValidateParams,
    plan: FnPlan,
    plan_params: FnPlanParams,
    effective_params: FnValidate,
    cache_clear: FnVoid,
    cache_evict: FnCacheEvict,
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
    output_image: FnOutputImage,
    image_view_free: FnImageViewFree,
    output_info: FnOutputInfo,
    run_outputs: FnRunOutputs,
    run_summary: FnRunSummary,
    import: FnImport,
    output_save: FnOutputSave,
    set_library_dirs: FnSetLibraryDirs,
    library_count: FnLibraryCount,
    library_definition: FnLibraryDefinition,
    cache_set_dir: FnCacheSetDir,
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
    /// 按给定路径加载 core DLL。宿主自己决定这个路径（install 布局的 `bin/`）。
    pub fn load_from(path: &Path) -> Result<Self, CoreError> {
        let lib = open_library(path)
            .map_err(|e| CoreError::Load(format!("{} —— {e}", path.display())))?;

        Ok(Core {
            version: sym!(lib, "lyflow_version", FnVersion),
            manifest_json: sym!(lib, "lyflow_manifest_json", FnJson),
            manifest_problems: sym!(lib, "lyflow_manifest_problems", FnJson),
            string_free: sym!(lib, "lyflow_string_free", FnStringFree),
            validate: sym!(lib, "lyflow_validate", FnValidate),
            validate_params: sym!(lib, "lyflow_validate_params", FnValidateParams),
            plan: sym!(lib, "lyflow_plan", FnPlan),
            plan_params: sym!(lib, "lyflow_plan_params", FnPlanParams),
            // 与 validate 同签名（graph_json, base_dir）→ JSON 文本。
            effective_params: sym!(lib, "lyflow_effective_params", FnValidate),
            cache_clear: sym!(lib, "lyflow_cache_clear", FnVoid),
            cache_evict: sym!(lib, "lyflow_cache_evict", FnCacheEvict),
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
            output_image: sym!(lib, "lyflow_output_image", FnOutputImage),
            image_view_free: sym!(lib, "lyflow_image_view_free", FnImageViewFree),
            output_info: sym!(lib, "lyflow_output_info", FnOutputInfo),
            run_outputs: sym!(lib, "lyflow_run_outputs", FnRunOutputs),
            run_summary: sym!(lib, "lyflow_run_summary", FnRunSummary),
            import: sym!(lib, "lyflow_import", FnImport),
            output_save: sym!(lib, "lyflow_output_save", FnOutputSave),
            set_library_dirs: sym!(lib, "lyflow_set_library_dirs", FnSetLibraryDirs),
            library_count: sym!(lib, "lyflow_library_count", FnLibraryCount),
            library_definition: sym!(lib, "lyflow_library_definition", FnLibraryDefinition),
            cache_set_dir: sym!(lib, "lyflow_cache_set_dir", FnCacheSetDir),
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

    /// 同 [`Core::validate`]，另带顶层图参数的取值（`params_json`，与 [`RunSpec::params_json`]
    /// 同义；None = 全用 default）。编辑器校验的是「default + 当前配方」合成的那一组（K5）。
    pub fn validate_with_params(
        &self,
        graph_json: &str,
        base_dir: &str,
        params_json: Option<&str>,
    ) -> Result<String, CoreError> {
        let g = CString::new(graph_json)?;
        let b = CString::new(base_dir)?;
        let p = params_json.map(CString::new).transpose()?;
        let pp = p.as_ref().map_or(std::ptr::null(), |c| c.as_ptr());
        unsafe { self.take_owned((self.validate_params)(g.as_ptr(), b.as_ptr(), pp)) }
    }

    /// 编译一次，报告每节点的 cacheKey 与是否已缓存（ADR-0007）。
    pub fn plan(
        &self,
        graph_json: &str,
        base_dir: &str,
        targets: &[String],
    ) -> Result<String, CoreError> {
        self.plan_with_params(graph_json, base_dir, targets, None)
    }

    /// 同 [`Core::plan`]，另带顶层图参数的取值：cacheKey 与 cached 反映的是这一组。
    pub fn plan_with_params(
        &self,
        graph_json: &str,
        base_dir: &str,
        targets: &[String],
        params_json: Option<&str>,
    ) -> Result<String, CoreError> {
        let g = CString::new(graph_json)?;
        let b = CString::new(base_dir)?;
        let p = params_json.map(CString::new).transpose()?;
        let pp = p.as_ref().map_or(std::ptr::null(), |c| c.as_ptr());
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
        unsafe {
            match p {
                // 不带取值就是老入口的语义，直接调它
                None => self.take_owned((self.plan)(g.as_ptr(), b.as_ptr(), head, ptrs.len())),
                Some(_) => self.take_owned((self.plan_params)(
                    g.as_ptr(),
                    b.as_ptr(),
                    head,
                    ptrs.len(),
                    pp,
                )),
            }
        }
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

    /// 按「图 + 节点」清缓存（v12）。返回 `{ removed, skippedPinned, nodes }` 的 JSON 文本；
    /// 图校验不过时是诊断数组（'[' 开头），与 `plan` 同一套区分办法。节点 id 语义同 targets。
    pub fn cache_evict(
        &self,
        graph_json: &str,
        base_dir: &str,
        node_ids: &[String],
        include_downstream: bool,
        params_json: Option<&str>,
    ) -> Result<String, CoreError> {
        let g = CString::new(graph_json)?;
        let b = CString::new(base_dir)?;
        let p = params_json.map(CString::new).transpose()?;
        let owned: Vec<CString> = node_ids
            .iter()
            .map(|t| CString::new(t.as_str()))
            .collect::<Result<_, _>>()?;
        let ptrs: Vec<*const c_char> = owned.iter().map(|c| c.as_ptr()).collect();
        let head = if ptrs.is_empty() { std::ptr::null() } else { ptrs.as_ptr() };
        unsafe {
            self.take_owned((self.cache_evict)(
                g.as_ptr(),
                b.as_ptr(),
                head,
                ptrs.len(),
                c_int::from(include_downstream),
                p.as_ref().map_or(std::ptr::null(), |c| c.as_ptr()),
            ))
        }
    }

    pub fn cache_stats(&self) -> Result<String, CoreError> {
        unsafe { self.take_owned((self.cache_stats)()) }
    }

    /// 加载之后立刻验一遍契约。坏的一代绝不能顶掉好的那一代（ADR-0009）。
    /// 启动自检：算子描述干净、manifest 是合法 JSON。宿主该在加载后立刻调它。
    pub fn self_check(&self) -> Result<(), String> {
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

    /// 落盘缓存（v14，docs/disk-cache-plan.md）：结果放在 `<dir>/<fingerprint>/` 下，`dir` 空串 = 关。
    /// `fingerprint` 是构建指纹 —— 变了整个目录不再复用。Err 里是一句人话的原因。
    pub fn cache_set_dir(&self, dir: &str, fingerprint: &str) -> Result<(), String> {
        let cs = |s: &str| CString::new(s).map_err(|e| e.to_string());
        let (d, f) = (cs(dir)?, cs(fingerprint)?);
        let message = unsafe { self.take_owned((self.cache_set_dir)(d.as_ptr(), f.as_ptr())) }
            .map_err(|e| e.to_string())?;
        if message.is_empty() {
            Ok(())
        } else {
            Err(message)
        }
    }

    /// 库算子 `lib.<id>` 的定义（v13）：库文件原样去掉 `id`，形状同图文档里的 `subgraphs.<id>`。
    /// 找不到返回 None。
    pub fn library_definition(&self, op_id: &str) -> Result<Option<String>, CoreError> {
        let id = CString::new(op_id)?;
        let raw = unsafe { self.take_owned((self.library_definition)(id.as_ptr())) }?;
        Ok(if raw.is_empty() { None } else { Some(raw) })
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
            rgb: std::ptr::null(),
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

    /// 图像的第 `level` 级（0 = 原图、零拷贝；k = 2^k 倍块均值缩小），
    /// 从 `row_offset` 起取 `row_count` 行（0 = 到底）。
    pub fn output_image(
        self: &Arc<Self>,
        run_id: &str,
        node_id: &str,
        port: &str,
        level: u32,
        row_offset: u32,
        row_count: u32,
    ) -> Result<ImageView, CoreError> {
        let r = CString::new(run_id)?;
        let n = CString::new(node_id)?;
        let p = CString::new(port)?;
        let mut raw = ImageViewRaw::empty();
        let rc = unsafe {
            (self.output_image)(
                r.as_ptr(),
                n.as_ptr(),
                p.as_ptr(),
                level,
                row_offset,
                row_count,
                &mut raw,
            )
        };
        if rc != 0 {
            return Err(CoreError::NoSuchOutput);
        }
        Ok(ImageView {
            raw,
            core: Arc::clone(self),
        })
    }
}

impl ImageViewRaw {
    fn empty() -> Self {
        ImageViewRaw {
            width: 0,
            height: 0,
            channels: 0,
            depth: 0,
            level: 0,
            full_width: 0,
            full_height: 0,
            row_offset: 0,
            row_count: 0,
            row_bytes: 0,
            pixels: std::ptr::null(),
            handle: std::ptr::null_mut(),
        }
    }
}

/// core 借出来的一段图像行，Drop 时还回去。level 0 直接指进结果仓。
pub struct ImageView {
    raw: ImageViewRaw,
    core: Arc<Core>,
}

impl ImageView {
    pub fn width(&self) -> u32 {
        self.raw.width
    }
    pub fn height(&self) -> u32 {
        self.raw.height
    }
    pub fn channels(&self) -> u32 {
        self.raw.channels
    }
    pub fn depth(&self) -> u32 {
        self.raw.depth
    }
    pub fn level(&self) -> u32 {
        self.raw.level
    }
    pub fn full_width(&self) -> u32 {
        self.raw.full_width
    }
    pub fn full_height(&self) -> u32 {
        self.raw.full_height
    }
    pub fn row_offset(&self) -> u32 {
        self.raw.row_offset
    }
    pub fn row_count(&self) -> u32 {
        self.raw.row_count
    }
    pub fn row_bytes(&self) -> u32 {
        self.raw.row_bytes
    }
    /// row_count * row_bytes 字节。
    pub fn pixels(&self) -> &[u8] {
        if self.raw.pixels.is_null() || self.raw.row_count == 0 {
            return &[];
        }
        unsafe {
            std::slice::from_raw_parts(
                self.raw.pixels,
                self.raw.row_count as usize * self.raw.row_bytes as usize,
            )
        }
    }

    /// 借一段调用方的内存当 ImageView 用，不走 core —— 给宿主测自己的编码路径。
    ///
    /// # Safety
    ///
    /// 视图只记下 `pixels` 的裸指针，不带生命周期：视图（以及从它取出的切片）在用的期间，
    /// `pixels` 必须一直活着、不被改动。行数按 `pixels.len() / 每行字节数` 算，不满一行的尾巴不算。
    #[cfg(any(test, feature = "test-util"))]
    #[allow(clippy::too_many_arguments)]
    pub unsafe fn borrowed(
        core: Arc<Core>,
        width: u32,
        height: u32,
        channels: u32,
        depth: u32,
        level: u32,
        full: (u32, u32),
        row_offset: u32,
        pixels: &[u8],
    ) -> Self {
        let row_bytes = width * channels * depth;
        ImageView {
            raw: ImageViewRaw {
                width,
                height,
                channels,
                depth,
                level,
                full_width: full.0,
                full_height: full.1,
                row_offset,
                row_count: (pixels.len() as u32).checked_div(row_bytes).unwrap_or(0),
                row_bytes,
                pixels: pixels.as_ptr(),
                handle: std::ptr::null_mut(),
            },
            core,
        }
    }
}

impl Drop for ImageView {
    fn drop(&mut self) {
        unsafe { (self.core.image_view_free)(&mut self.raw) };
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
    pub fn has_rgb(&self) -> bool {
        self.raw.flags & CLOUD_HAS_RGB != 0 && !self.raw.rgb.is_null()
    }
    /// 3 * point_count 个 0..255（v12）。
    pub fn rgb(&self) -> &[u8] {
        if !self.has_rgb() {
            return &[];
        }
        unsafe { std::slice::from_raw_parts(self.raw.rgb, self.raw.point_count as usize * 3) }
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

    /// 借一段调用方的内存当 TensorView 用，不走 core —— 给宿主测自己的编码路径。
    /// 挂在 `test-util` 后面：它绕过了 core 的所有权约定，不该出现在生产构建里。
    ///
    /// # Safety
    ///
    /// 视图只记下 `shape` 与 `data` 的裸指针，不带生命周期：视图在用的期间，两者必须一直活着、不被改动。
    #[cfg(any(test, feature = "test-util"))]
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
    /// 启动一次运行。`user` 由本函数接管，join 之后才会被释放。
    ///
    /// # Safety
    ///
    /// - `cb` 会在 core 的工作线程上被调用，几个节点并行时还会同时被调用，直到 join 返回为止：
    ///   它必须能跨线程安全地执行，而且不能让 panic 展开穿过它（展开穿过 C++ 栈帧是未定义行为，
    ///   bridge 的回调用 `catch_unwind` 兜着）。
    /// - `cb` 拿到的 user 指针只能按 `T` 解引用、只读或自带同步；它在 join 返回之后才被释放。
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
        let isolate_cstrings: Vec<CString> = spec
            .isolate
            .iter()
            .map(|t| CString::new(t.as_str()))
            .collect::<Result<_, _>>()?;
        let isolate_ptrs: Vec<*const c_char> =
            isolate_cstrings.iter().map(|c| c.as_ptr()).collect();
        let force_cstrings: Vec<CString> = spec
            .force
            .iter()
            .map(|t| CString::new(t.as_str()))
            .collect::<Result<_, _>>()?;
        let force_ptrs: Vec<*const c_char> = force_cstrings.iter().map(|c| c.as_ptr()).collect();

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

        let image_names: Vec<(CString, CString)> = spec
            .image_inputs
            .iter()
            .map(|i| Ok((CString::new(i.node_id.as_str())?, CString::new(i.port.as_str())?)))
            .collect::<Result<_, std::ffi::NulError>>()?;
        let image_raw: Vec<RunImageInputRaw> = spec
            .image_inputs
            .iter()
            .zip(image_names.iter())
            .map(|(i, (node, port))| RunImageInputRaw {
                node_id: node.as_ptr(),
                port: port.as_ptr(),
                width: i.width,
                height: i.height,
                channels: i.channels,
                depth: i.depth,
                row_bytes: 0,
                reserved: 0,
                pixels: i.pixels_ptr(),
            })
            .collect();

        let params_c = spec.params_json.map(CString::new).transpose()?;
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
            params_json: params_c.as_ref().map_or(std::ptr::null(), |c| c.as_ptr()),
            isolate: if isolate_ptrs.is_empty() {
                std::ptr::null()
            } else {
                isolate_ptrs.as_ptr()
            },
            isolate_count: isolate_ptrs.len(),
            force: if force_ptrs.is_empty() {
                std::ptr::null()
            } else {
                force_ptrs.as_ptr()
            },
            force_count: force_ptrs.len(),
            image_inputs: if image_raw.is_empty() {
                std::ptr::null()
            } else {
                image_raw.as_ptr()
            },
            image_input_count: image_raw.len(),
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

