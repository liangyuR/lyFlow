use super::*;
use std::ptr::NonNull;

#[repr(C)]
pub(crate) struct FrameOpaque {
    _private: [u8; 0],
}
#[repr(C)]
struct SessionOpaque {
    _private: [u8; 0],
}
type Capture = unsafe extern "C" fn(
    *const c_char,
    *const c_char,
    *const c_char,
    *mut *mut FrameOpaque,
) -> *mut c_char;
type Load = unsafe extern "C" fn(*const c_char, u64, *mut *mut FrameOpaque) -> *mut c_char;
type Save = unsafe extern "C" fn(*mut FrameOpaque, *const c_char, i32) -> *mut c_char;
type CancelCb = unsafe extern "C" fn(*mut c_void) -> i32;
type LoadEx = unsafe extern "C" fn(
    *const c_char,
    u64,
    Option<CancelCb>,
    *mut c_void,
    *mut *mut FrameOpaque,
) -> *mut c_char;
type SaveEx = unsafe extern "C" fn(
    *mut FrameOpaque,
    *const c_char,
    i32,
    Option<CancelCb>,
    *mut c_void,
) -> *mut c_char;
type PushEx = unsafe extern "C" fn(
    *mut SessionOpaque,
    *mut FrameOpaque,
    i32,
    Option<CancelCb>,
    *mut c_void,
) -> *mut c_char;
type CancelCheck<'a> = &'a (dyn Fn() -> bool + Sync);
unsafe extern "C" fn cancelled(user: *mut c_void) -> i32 {
    let check = &*(user as *const CancelCheck<'_>);
    i32::from(std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| check())).unwrap_or(true))
}
type Metadata = unsafe extern "C" fn(*mut FrameOpaque) -> *mut c_char;
type RunId = unsafe extern "C" fn(*mut FrameOpaque) -> *const c_char;
type Free = unsafe extern "C" fn(*mut FrameOpaque);
pub(crate) type Start = unsafe extern "C" fn(
    *const c_char,
    *const RunOptionsRaw,
    *mut FrameOpaque,
    *const c_char,
    Option<EventCb>,
    *mut c_void,
) -> *mut RunOpaque;
type Create = unsafe extern "C" fn(*const c_char, *mut *mut SessionOpaque) -> *mut c_char;
type Push = unsafe extern "C" fn(*mut SessionOpaque, *mut FrameOpaque, i32) -> *mut c_char;
type Front = unsafe extern "C" fn(*mut SessionOpaque, i32, *mut *mut FrameOpaque) -> *mut c_char;
type SessionJson = unsafe extern "C" fn(*mut SessionOpaque) -> *mut c_char;
type SessionFree = unsafe extern "C" fn(*mut SessionOpaque);
type Bindings = unsafe extern "C" fn(*const c_char) -> *mut c_char;
type EffectiveInputs = unsafe extern "C" fn(
    *const c_char,
    *const c_char,
    *const c_char,
    *const RunInputRaw,
    usize,
    *const RunImageInputRaw,
    usize,
) -> *mut c_char;

pub(crate) struct Functions {
    capture: Option<Capture>,
    load: Option<Load>,
    save: Option<Save>,
    load_ex: Option<LoadEx>,
    save_ex: Option<SaveEx>,
    push_ex: Option<PushEx>,
    metadata: Option<Metadata>,
    run_id: Option<RunId>,
    free: Option<Free>,
    pub(crate) run_start: Option<Start>,
    create: Option<Create>,
    push: Option<Push>,
    front: Option<Front>,
    reset: Option<SessionJson>,
    info: Option<SessionJson>,
    session_free: Option<SessionFree>,
    capabilities: Option<FnJson>,
    bindings: Option<Bindings>,
    effective_inputs: Option<EffectiveInputs>,
}

impl Functions {
    pub(crate) fn load(lib: &Library) -> Self {
        unsafe fn optional<T: Copy>(lib: &Library, name: &[u8]) -> Option<T> {
            lib.get::<T>(name).ok().map(|s| *s)
        }
        unsafe {
            Self {
                capture: optional(lib, b"lyflow_frame_capture\0"),
                load: optional(lib, b"lyflow_frame_load\0"),
                save: optional(lib, b"lyflow_frame_save\0"),
                load_ex: optional(lib, b"lyflow_frame_load_ex\0"),
                save_ex: optional(lib, b"lyflow_frame_save_ex\0"),
                push_ex: optional(lib, b"lyflow_session_push_ex\0"),
                metadata: optional(lib, b"lyflow_frame_metadata\0"),
                run_id: optional(lib, b"lyflow_frame_run_id\0"),
                free: optional(lib, b"lyflow_frame_free\0"),
                run_start: optional(lib, b"lyflow_run_start_frame\0"),
                create: optional(lib, b"lyflow_session_create\0"),
                push: optional(lib, b"lyflow_session_push\0"),
                front: optional(lib, b"lyflow_session_front\0"),
                reset: optional(lib, b"lyflow_session_reset\0"),
                info: optional(lib, b"lyflow_session_info\0"),
                session_free: optional(lib, b"lyflow_session_free\0"),
                capabilities: optional(lib, b"lyflow_runtime_capabilities\0"),
                bindings: optional(lib, b"lyflow_run_bindings\0"),
                effective_inputs: optional(lib, b"lyflow_effective_params_inputs\0"),
            }
        }
    }
    fn frame_lifetime(&self) -> Result<(RunId, Free), CoreError> {
        Ok((
            self.run_id
                .ok_or(CoreError::MissingSymbol("lyflow_frame_run_id"))?,
            self.free
                .ok_or(CoreError::MissingSymbol("lyflow_frame_free"))?,
        ))
    }
}

struct FrameInner {
    raw: NonNull<FrameOpaque>,
    core: Arc<Core>,
    run_id: String,
    free: Free,
}
impl Drop for FrameInner {
    fn drop(&mut self) {
        unsafe { (self.free)(self.raw.as_ptr()) }
    }
}
// The core owns immutable frame payloads; native view access and reference ownership are synchronized.
unsafe impl Send for FrameInner {}
unsafe impl Sync for FrameInner {}

/// Immutable results with independent native ownership, including the loaded runtime library.
#[derive(Clone)]
pub struct Frame {
    inner: Arc<FrameInner>,
}
impl Frame {
    unsafe fn adopt(
        core: Arc<Core>,
        raw: *mut FrameOpaque,
        run_id: RunId,
        free: Free,
    ) -> Result<Self, CoreError> {
        let raw = NonNull::new(raw).ok_or(CoreError::NullReturn)?;
        let id = run_id(raw.as_ptr());
        let result = if id.is_null() {
            Err(CoreError::NullReturn)
        } else {
            CStr::from_ptr(id)
                .to_str()
                .map(str::to_owned)
                .map_err(CoreError::from)
        };
        match result {
            Ok(run_id) => Ok(Self {
                inner: Arc::new(FrameInner {
                    raw,
                    core,
                    run_id,
                    free,
                }),
            }),
            Err(e) => {
                free(raw.as_ptr());
                Err(e)
            }
        }
    }
    pub(crate) fn raw(&self) -> *mut FrameOpaque {
        self.inner.raw.as_ptr()
    }
    pub fn core(&self) -> &Arc<Core> {
        &self.inner.core
    }
    pub fn run_id(&self) -> &str {
        &self.inner.run_id
    }
    pub fn metadata(&self) -> Result<String, CoreError> {
        let call = self
            .core()
            .frames
            .metadata
            .ok_or(CoreError::MissingSymbol("lyflow_frame_metadata"))?;
        unsafe { self.core().take_owned(call(self.raw())) }
    }
    pub fn save(&self, path: &Path, overwrite: bool) -> Result<(), CoreError> {
        let call = self
            .core()
            .frames
            .save
            .ok_or(CoreError::MissingSymbol("lyflow_frame_save"))?;
        let path = path
            .to_str()
            .ok_or_else(|| CoreError::Operation("archive path must be UTF-8".into()))?;
        let path = CString::new(path)?;
        unsafe {
            self.core()
                .frame_status(call(self.raw(), path.as_ptr(), i32::from(overwrite)))
        }
    }
    pub fn save_cancellable(
        &self,
        path: &Path,
        overwrite: bool,
        check: CancelCheck<'_>,
    ) -> Result<(), CoreError> {
        let call = self
            .core()
            .frames
            .save_ex
            .ok_or(CoreError::MissingSymbol("lyflow_frame_save_ex"))?;
        let path = CString::new(
            path.to_str()
                .ok_or_else(|| CoreError::Operation("archive path must be UTF-8".into()))?,
        )?;
        unsafe {
            self.core().frame_status(call(
                self.raw(),
                path.as_ptr(),
                i32::from(overwrite),
                Some(cancelled),
                &check as *const _ as *mut c_void,
            ))
        }
    }
}

struct SessionInner {
    raw: NonNull<SessionOpaque>,
    core: Arc<Core>,
    free: SessionFree,
}
impl Drop for SessionInner {
    fn drop(&mut self) {
        unsafe { (self.free)(self.raw.as_ptr()) }
    }
}
// Queue operations and frame retention are synchronized in the native session.
unsafe impl Send for SessionInner {}
unsafe impl Sync for SessionInner {}
#[derive(Clone)]
pub struct Session {
    inner: Arc<SessionInner>,
}
impl Session {
    pub fn push_cancellable(
        &self,
        frame: &Frame,
        reset: bool,
        check: CancelCheck<'_>,
    ) -> Result<String, CoreError> {
        if !Arc::ptr_eq(&self.inner.core, frame.core()) {
            return Err(CoreError::Operation(
                "frame belongs to a different Core".into(),
            ));
        }
        let call = self
            .inner
            .core
            .frames
            .push_ex
            .ok_or(CoreError::MissingSymbol("lyflow_session_push_ex"))?;
        unsafe {
            self.inner.core.take_owned(call(
                self.inner.raw.as_ptr(),
                frame.raw(),
                i32::from(reset),
                Some(cancelled),
                &check as *const _ as *mut c_void,
            ))
        }
    }
    pub fn push(&self, frame: &Frame, reset: bool) -> Result<String, CoreError> {
        if !Arc::ptr_eq(&self.inner.core, frame.core()) {
            return Err(CoreError::Operation(
                "frame belongs to a different Core".into(),
            ));
        }
        let call = self
            .inner
            .core
            .frames
            .push
            .ok_or(CoreError::MissingSymbol("lyflow_session_push"))?;
        unsafe {
            self.inner
                .core
                .take_owned(call(self.inner.raw.as_ptr(), frame.raw(), i32::from(reset)))
        }
    }
    pub fn front(&self, remove: bool) -> Result<Option<Frame>, CoreError> {
        let core = &self.inner.core;
        let call = core
            .frames
            .front
            .ok_or(CoreError::MissingSymbol("lyflow_session_front"))?;
        let (id, free) = core.frames.frame_lifetime()?;
        let mut raw = std::ptr::null_mut();
        unsafe {
            core.frame_status(call(self.inner.raw.as_ptr(), i32::from(remove), &mut raw))?;
            if raw.is_null() {
                Ok(None)
            } else {
                Frame::adopt(Arc::clone(core), raw, id, free).map(Some)
            }
        }
    }
    pub fn reset(&self) -> Result<(), CoreError> {
        let call = self
            .inner
            .core
            .frames
            .reset
            .ok_or(CoreError::MissingSymbol("lyflow_session_reset"))?;
        unsafe { self.inner.core.frame_status(call(self.inner.raw.as_ptr())) }
    }
    pub fn info(&self) -> Result<String, CoreError> {
        let call = self
            .inner
            .core
            .frames
            .info
            .ok_or(CoreError::MissingSymbol("lyflow_session_info"))?;
        unsafe { self.inner.core.take_owned(call(self.inner.raw.as_ptr())) }
    }
}

impl Core {
    pub fn effective_params_with_inputs(
        &self,
        graph_json: &str,
        base_dir: &str,
        params_json: Option<&str>,
        inputs: &[RunInput],
        images: &[RunImageInput],
    ) -> Result<String, CoreError> {
        let call = self
            .frames
            .effective_inputs
            .ok_or(CoreError::MissingSymbol("lyflow_effective_params_inputs"))?;
        let graph = CString::new(graph_json)?;
        let base = CString::new(base_dir)?;
        let params = params_json.map(CString::new).transpose()?;
        let raw = RawInputs::new(inputs, images)?;
        unsafe {
            self.take_owned(call(
                graph.as_ptr(),
                base.as_ptr(),
                params.as_ref().map_or(std::ptr::null(), |p| p.as_ptr()),
                raw.clouds_ptr(),
                raw.cloud_count(),
                raw.images_ptr(),
                raw.image_count(),
            ))
        }
    }
    unsafe fn frame_status(&self, raw: *mut c_char) -> Result<(), CoreError> {
        let text = self.take_owned(raw)?;
        if text.is_empty() {
            Ok(())
        } else {
            Err(CoreError::Operation(text))
        }
    }
    pub fn runtime_capabilities(&self) -> Result<String, CoreError> {
        let call = self
            .frames
            .capabilities
            .ok_or(CoreError::MissingSymbol("lyflow_runtime_capabilities"))?;
        unsafe { self.take_owned(call()) }
    }
    pub fn run_bindings(&self, run_id: &str) -> Result<String, CoreError> {
        let call = self
            .frames
            .bindings
            .ok_or(CoreError::MissingSymbol("lyflow_run_bindings"))?;
        let id = CString::new(run_id)?;
        unsafe { self.take_owned(call(id.as_ptr())) }
    }
    pub fn capture_frame(
        self: &Arc<Self>,
        run_id: &str,
        metadata_json: &str,
        options_json: &str,
    ) -> Result<Frame, CoreError> {
        let call = self
            .frames
            .capture
            .ok_or(CoreError::MissingSymbol("lyflow_frame_capture"))?;
        let (id, free) = self.frames.frame_lifetime()?;
        let (run, metadata, options) = (
            CString::new(run_id)?,
            CString::new(metadata_json)?,
            CString::new(options_json)?,
        );
        let mut raw = std::ptr::null_mut();
        unsafe {
            self.frame_status(call(
                run.as_ptr(),
                metadata.as_ptr(),
                options.as_ptr(),
                &mut raw,
            ))?;
            Frame::adopt(Arc::clone(self), raw, id, free)
        }
    }
    pub fn load_frame(self: &Arc<Self>, path: &Path, max_bytes: u64) -> Result<Frame, CoreError> {
        let call = self
            .frames
            .load
            .ok_or(CoreError::MissingSymbol("lyflow_frame_load"))?;
        let (id, free) = self.frames.frame_lifetime()?;
        let path = CString::new(
            path.to_str()
                .ok_or_else(|| CoreError::Operation("archive path must be UTF-8".into()))?,
        )?;
        let mut raw = std::ptr::null_mut();
        unsafe {
            self.frame_status(call(path.as_ptr(), max_bytes, &mut raw))?;
            Frame::adopt(Arc::clone(self), raw, id, free)
        }
    }
    pub fn load_frame_cancellable(
        self: &Arc<Self>,
        path: &Path,
        max_bytes: u64,
        check: CancelCheck<'_>,
    ) -> Result<Frame, CoreError> {
        let call = self
            .frames
            .load_ex
            .ok_or(CoreError::MissingSymbol("lyflow_frame_load_ex"))?;
        let (id, free) = self.frames.frame_lifetime()?;
        let path = CString::new(
            path.to_str()
                .ok_or_else(|| CoreError::Operation("archive path must be UTF-8".into()))?,
        )?;
        let mut raw = std::ptr::null_mut();
        unsafe {
            self.frame_status(call(
                path.as_ptr(),
                max_bytes,
                Some(cancelled),
                &check as *const _ as *mut c_void,
                &mut raw,
            ))?;
            Frame::adopt(Arc::clone(self), raw, id, free)
        }
    }
    pub fn create_session(self: &Arc<Self>, options_json: &str) -> Result<Session, CoreError> {
        let call = self
            .frames
            .create
            .ok_or(CoreError::MissingSymbol("lyflow_session_create"))?;
        let free = self
            .frames
            .session_free
            .ok_or(CoreError::MissingSymbol("lyflow_session_free"))?;
        let options = CString::new(options_json)?;
        let mut raw = std::ptr::null_mut();
        unsafe {
            self.frame_status(call(options.as_ptr(), &mut raw))?;
        }
        Ok(Session {
            inner: Arc::new(SessionInner {
                raw: NonNull::new(raw).ok_or(CoreError::NullReturn)?,
                core: Arc::clone(self),
                free,
            }),
        })
    }
    /// Start with typed results owned by a frame. The frame remains retained until run disposal.
    ///
    /// # Safety
    /// The callback and user value must satisfy the same contract as [`RunHandle::start`].
    pub unsafe fn run_with_frame<T>(
        self: &Arc<Self>,
        spec: RunSpec<'_>,
        frame: &Frame,
        bindings_json: &str,
        cb: EventCb,
        user: Box<T>,
    ) -> Result<RunHandle, CoreError> {
        RunHandle::start_inner(
            Arc::clone(self),
            spec,
            cb,
            user,
            Some((frame, bindings_json)),
        )
    }
}
