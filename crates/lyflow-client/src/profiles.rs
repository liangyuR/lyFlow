use super::*;

#[repr(C)]
pub struct ProfileInputOpaque {
    _private: [u8; 0],
}

#[repr(C)]
pub struct RunProfileInputRaw {
    pub node_id: *const c_char,
    pub port: *const c_char,
    pub count: u32,
    pub xyz: *const f32,
    pub intensity: *const f32,
    pub normals: *const f32,
    pub rgb: *const u8,
    pub descriptor_json: *const c_char,
    pub validity: *const u8,
    pub source_indices: *const u32,
    pub handle: *const ProfileInputOpaque,
}

#[derive(Clone, Debug)]
pub struct RunProfileInput {
    pub node_id: String,
    pub port: String,
    pub xyz: Vec<f32>,
    pub intensity: Vec<f32>,
    pub normals: Vec<f32>,
    pub rgb: Vec<u8>,
    pub descriptor_json: String,
    pub validity: Vec<u8>,
    pub source_indices: Vec<u32>,
    pub retained: Option<ProfileInputHandle>,
}

impl RunProfileInput {
    pub fn validate_buffers(&self) -> Result<(), CoreError> {
        if self.retained.is_some() {
            return Ok(());
        }
        let n = self.xyz.len() / 3;
        if self.xyz.len() % 3 != 0
            || n > u32::MAX as usize
            || self.validity.len() != n
            || self.source_indices.len() != n
            || (!self.intensity.is_empty() && self.intensity.len() != n)
            || (!self.normals.is_empty() && self.normals.len() != 3 * n)
            || (!self.rgb.is_empty() && self.rgb.len() != 3 * n)
        {
            return Err(CoreError::Operation(
                "Profile input buffer lengths do not agree".into(),
            ));
        }
        Ok(())
    }
}

struct InputHandleInner {
    raw: std::ptr::NonNull<ProfileInputOpaque>,
    _core: Arc<Core>,
    free: FreeInput,
}
unsafe impl Send for InputHandleInner {}
unsafe impl Sync for InputHandleInner {}
impl Drop for InputHandleInner {
    fn drop(&mut self) {
        unsafe { (self.free)(self.raw.as_ptr()) }
    }
}
#[derive(Clone)]
pub struct ProfileInputHandle {
    inner: Arc<InputHandleInner>,
}
impl std::fmt::Debug for ProfileInputHandle {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("ProfileInputHandle")
            .field("raw", &self.inner.raw)
            .finish()
    }
}
impl ProfileInputHandle {
    pub(crate) fn raw(&self) -> *const ProfileInputOpaque {
        self.inner.raw.as_ptr()
    }
}
type CreateInput =
    unsafe extern "C" fn(*const RunProfileInputRaw, *mut *mut ProfileInputOpaque) -> *mut c_char;
type CaptureInput = unsafe extern "C" fn(
    *const c_char,
    *const c_char,
    *const c_char,
    *mut *mut ProfileInputOpaque,
) -> *mut c_char;
type FreeInput = unsafe extern "C" fn(*mut ProfileInputOpaque);

#[repr(C)]
pub(crate) struct ProfileViewRaw {
    point_count: u32,
    total_points: u32,
    offset: u32,
    flags: u32,
    cloud_id: u64,
    descriptor_json: *const c_char,
    xyz: *const f32,
    intensity: *const f32,
    normals: *const f32,
    rgb: *const u8,
    validity: *const u8,
    source_indices: *const u32,
    handle: *mut c_void,
}

type Output = unsafe extern "C" fn(
    *const c_char,
    *const c_char,
    *const c_char,
    u32,
    u32,
    *mut ProfileViewRaw,
) -> i32;
type Free = unsafe extern "C" fn(*mut ProfileViewRaw);
type Plan = unsafe extern "C" fn(
    *const c_char,
    *const c_char,
    *const *const c_char,
    usize,
    *const c_char,
    *const RunInputRaw,
    usize,
    *const RunImageInputRaw,
    usize,
    *const RunProfileInputRaw,
    usize,
) -> *mut c_char;
type Evict = unsafe extern "C" fn(
    *const c_char,
    *const c_char,
    *const *const c_char,
    usize,
    i32,
    *const c_char,
    *const RunInputRaw,
    usize,
    *const RunImageInputRaw,
    usize,
    *const RunProfileInputRaw,
    usize,
) -> *mut c_char;
type Effective = unsafe extern "C" fn(
    *const c_char,
    *const c_char,
    *const c_char,
    *const RunInputRaw,
    usize,
    *const RunImageInputRaw,
    usize,
    *const RunProfileInputRaw,
    usize,
) -> *mut c_char;

pub(crate) struct Functions {
    create_input: Option<CreateInput>,
    capture_input: Option<CaptureInput>,
    free_input: Option<FreeInput>,
    output: Option<Output>,
    free: Option<Free>,
    plan: Option<Plan>,
    evict: Option<Evict>,
    effective: Option<Effective>,
}

impl Functions {
    pub(crate) fn load(lib: &Library) -> Self {
        unsafe fn optional<T: Copy>(lib: &Library, name: &[u8]) -> Option<T> {
            lib.get::<T>(name).ok().map(|symbol| *symbol)
        }
        unsafe {
            Self {
                create_input: optional(lib, b"lyflow_profile_input_create\0"),
                capture_input: optional(lib, b"lyflow_profile_input_capture\0"),
                free_input: optional(lib, b"lyflow_profile_input_free\0"),
                output: optional(lib, b"lyflow_output_profile\0"),
                free: optional(lib, b"lyflow_profile_view_free\0"),
                plan: optional(lib, b"lyflow_plan_inputs_ex\0"),
                evict: optional(lib, b"lyflow_cache_evict_inputs_ex\0"),
                effective: optional(lib, b"lyflow_effective_params_inputs_ex\0"),
            }
        }
    }
}

pub struct ProfileView {
    raw: ProfileViewRaw,
    core: Arc<Core>,
}

impl ProfileView {
    pub fn point_count(&self) -> u32 {
        self.raw.point_count
    }
    pub fn total_points(&self) -> u32 {
        self.raw.total_points
    }
    pub fn offset(&self) -> u32 {
        self.raw.offset
    }
    pub fn flags(&self) -> u32 {
        self.raw.flags
    }
    pub fn cloud_id(&self) -> u64 {
        self.raw.cloud_id
    }
    pub fn descriptor_json(&self) -> &str {
        if self.raw.descriptor_json.is_null() {
            return "";
        }
        unsafe {
            CStr::from_ptr(self.raw.descriptor_json)
                .to_str()
                .unwrap_or("")
        }
    }
    pub fn xyz(&self) -> &[f32] {
        unsafe { self.slice(self.raw.xyz, 3) }
    }
    pub fn intensity(&self) -> &[f32] {
        unsafe { self.slice(self.raw.intensity, 1) }
    }
    pub fn normals(&self) -> &[f32] {
        unsafe { self.slice(self.raw.normals, 3) }
    }
    pub fn rgb(&self) -> &[u8] {
        unsafe { self.slice(self.raw.rgb, 3) }
    }
    pub fn validity(&self) -> &[u8] {
        unsafe { self.slice(self.raw.validity, 1) }
    }
    pub fn source_indices(&self) -> &[u32] {
        unsafe { self.slice(self.raw.source_indices, 1) }
    }
    unsafe fn slice<T>(&self, data: *const T, width: usize) -> &[T] {
        if data.is_null() || self.raw.point_count == 0 {
            &[]
        } else {
            std::slice::from_raw_parts(data, self.raw.point_count as usize * width)
        }
    }
}

impl Drop for ProfileView {
    fn drop(&mut self) {
        if let Some(free) = self.core.profiles.free {
            unsafe { free(&mut self.raw) }
        }
    }
}

impl Core {
    pub fn retain_profile_input(
        self: &Arc<Self>,
        input: &RunProfileInput,
    ) -> Result<ProfileInputHandle, CoreError> {
        let create = self
            .profiles
            .create_input
            .ok_or(CoreError::MissingSymbol("lyflow_profile_input_create"))?;
        let free = self
            .profiles
            .free_input
            .ok_or(CoreError::MissingSymbol("lyflow_profile_input_free"))?;
        let raw = RawInputs::with_profiles(&[], &[], std::slice::from_ref(input))?;
        let mut handle = std::ptr::null_mut();
        let message = unsafe { self.take_owned(create(raw.profiles_ptr(), &mut handle))? };
        if !message.is_empty() {
            return Err(CoreError::Operation(message));
        }
        let raw = std::ptr::NonNull::new(handle).ok_or(CoreError::NullReturn)?;
        Ok(ProfileInputHandle {
            inner: Arc::new(InputHandleInner {
                raw,
                _core: Arc::clone(self),
                free,
            }),
        })
    }
    pub fn capture_profile_input(
        self: &Arc<Self>,
        run: &str,
        node: &str,
        port: &str,
    ) -> Result<ProfileInputHandle, CoreError> {
        let capture = self
            .profiles
            .capture_input
            .ok_or(CoreError::MissingSymbol("lyflow_profile_input_capture"))?;
        let free = self
            .profiles
            .free_input
            .ok_or(CoreError::MissingSymbol("lyflow_profile_input_free"))?;
        let r = CString::new(run)?;
        let n = CString::new(node)?;
        let p = CString::new(port)?;
        let mut handle = std::ptr::null_mut();
        let message =
            unsafe { self.take_owned(capture(r.as_ptr(), n.as_ptr(), p.as_ptr(), &mut handle))? };
        if !message.is_empty() {
            return Err(CoreError::Operation(message));
        }
        let raw = std::ptr::NonNull::new(handle).ok_or(CoreError::NullReturn)?;
        Ok(ProfileInputHandle {
            inner: Arc::new(InputHandleInner {
                raw,
                _core: Arc::clone(self),
                free,
            }),
        })
    }
    pub fn supports_profiles(&self) -> bool {
        self.profiles.output.is_some() && self.profiles.free.is_some()
    }
    pub fn output_profile(
        self: &Arc<Self>,
        run_id: &str,
        node_id: &str,
        port: &str,
        offset: u32,
        count: u32,
    ) -> Result<ProfileView, CoreError> {
        let call = self
            .profiles
            .output
            .ok_or(CoreError::MissingSymbol("lyflow_output_profile"))?;
        self.profiles
            .free
            .ok_or(CoreError::MissingSymbol("lyflow_profile_view_free"))?;
        let r = CString::new(run_id)?;
        let n = CString::new(node_id)?;
        let p = CString::new(port)?;
        let mut raw: ProfileViewRaw = unsafe { std::mem::zeroed() };
        if unsafe { call(r.as_ptr(), n.as_ptr(), p.as_ptr(), offset, count, &mut raw) } != 0 {
            return Err(CoreError::NoSuchOutput);
        }
        Ok(ProfileView {
            raw,
            core: Arc::clone(self),
        })
    }

    #[allow(clippy::too_many_arguments)]
    pub fn plan_with_profiles(
        &self,
        graph_json: &str,
        base_dir: &str,
        targets: &[String],
        params_json: Option<&str>,
        clouds: &[RunInput],
        images: &[RunImageInput],
        profiles: &[RunProfileInput],
    ) -> Result<String, CoreError> {
        let call = self
            .profiles
            .plan
            .ok_or(CoreError::MissingSymbol("lyflow_plan_inputs_ex"))?;
        let g = CString::new(graph_json)?;
        let b = CString::new(base_dir)?;
        let p = params_json.map(CString::new).transpose()?;
        let names = targets
            .iter()
            .map(|s| CString::new(s.as_str()))
            .collect::<Result<Vec<_>, _>>()?;
        let ids = names.iter().map(|s| s.as_ptr()).collect::<Vec<_>>();
        let raw = RawInputs::with_profiles(clouds, images, profiles)?;
        unsafe {
            self.take_owned(call(
                g.as_ptr(),
                b.as_ptr(),
                ids.as_ptr(),
                ids.len(),
                p.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
                raw.clouds_ptr(),
                raw.cloud_count(),
                raw.images_ptr(),
                raw.image_count(),
                raw.profiles_ptr(),
                raw.profile_count(),
            ))
        }
    }

    #[allow(clippy::too_many_arguments)]
    pub fn cache_evict_with_profiles(
        &self,
        graph_json: &str,
        base_dir: &str,
        nodes: &[String],
        downstream: bool,
        params_json: Option<&str>,
        clouds: &[RunInput],
        images: &[RunImageInput],
        profiles: &[RunProfileInput],
    ) -> Result<String, CoreError> {
        let call = self
            .profiles
            .evict
            .ok_or(CoreError::MissingSymbol("lyflow_cache_evict_inputs_ex"))?;
        let g = CString::new(graph_json)?;
        let b = CString::new(base_dir)?;
        let p = params_json.map(CString::new).transpose()?;
        let names = nodes
            .iter()
            .map(|s| CString::new(s.as_str()))
            .collect::<Result<Vec<_>, _>>()?;
        let ids = names.iter().map(|s| s.as_ptr()).collect::<Vec<_>>();
        let raw = RawInputs::with_profiles(clouds, images, profiles)?;
        unsafe {
            self.take_owned(call(
                g.as_ptr(),
                b.as_ptr(),
                ids.as_ptr(),
                ids.len(),
                i32::from(downstream),
                p.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
                raw.clouds_ptr(),
                raw.cloud_count(),
                raw.images_ptr(),
                raw.image_count(),
                raw.profiles_ptr(),
                raw.profile_count(),
            ))
        }
    }

    pub fn effective_params_with_profiles(
        &self,
        graph_json: &str,
        base_dir: &str,
        params_json: Option<&str>,
        clouds: &[RunInput],
        images: &[RunImageInput],
        profiles: &[RunProfileInput],
    ) -> Result<String, CoreError> {
        let call = self.profiles.effective.ok_or(CoreError::MissingSymbol(
            "lyflow_effective_params_inputs_ex",
        ))?;
        let g = CString::new(graph_json)?;
        let b = CString::new(base_dir)?;
        let p = params_json.map(CString::new).transpose()?;
        let raw = RawInputs::with_profiles(clouds, images, profiles)?;
        unsafe {
            self.take_owned(call(
                g.as_ptr(),
                b.as_ptr(),
                p.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
                raw.clouds_ptr(),
                raw.cloud_count(),
                raw.images_ptr(),
                raw.image_count(),
                raw.profiles_ptr(),
                raw.profile_count(),
            ))
        }
    }
}
