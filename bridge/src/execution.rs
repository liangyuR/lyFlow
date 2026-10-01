//! 运行的生命周期管理。D3：同一时刻一个活跃 run，新 run 抢占旧 run。
//! 抢占不阻塞（ADR-0027）：被抢占的 run 在后台排干，它退出之前来的请求只留最新一个 ——
//! 同一时刻最多一个 run 在算。最多同时持有两份 run 的索引（在算的 + 上一次完成的），详见 bridge/README.md。

use std::ffi::CStr;
use std::os::raw::{c_char, c_void};
use std::sync::{Arc, Mutex, MutexGuard};

use tauri::{AppHandle, Emitter, Runtime};

use crate::core_ffi::{self, Core, RunHandle, RunSpec};
use crate::ulid;

/// 事件推流的前端事件名。前端 `onExecutionEvent` 监听的就是它。
pub const EVENT_NAME: &str = "execution-event";

/// 回调里要用到的东西。裸指针传给 C++，生命期由 `RunHandle` 持有 ——
/// C ABI 保证 join 返回后不再回调，所以 `RunHandle::drop` 里 join 完才释放它。
struct EmitCtx<R: Runtime> {
    app: AppHandle<R>,
    run_id: String,
}

/// C++ 工作线程调过来的回调。`catch_unwind` 不是摆设：panic 展开穿过 C++ 栈帧
/// 是未定义行为，而丢一条事件只会让前端的 seq 检查 warn 一句。
unsafe extern "C" fn trampoline<R: Runtime>(event_json: *const c_char, user: *mut c_void) {
    let _ = std::panic::catch_unwind(|| {
        if event_json.is_null() || user.is_null() {
            return;
        }
        let ctx = &*(user as *const EmitCtx::<R>);
        let Ok(text) = CStr::from_ptr(event_json).to_str() else {
            eprintln!("execution event 不是合法 UTF-8，已丢弃");
            return;
        };
        match serde_json::from_str::<serde_json::Value>(text) {
            Ok(value) => {
                if let Err(e) = ctx.app.emit(EVENT_NAME, value) {
                    eprintln!("推送 execution event 失败 (run {}): {e}", ctx.run_id);
                }
            }
            Err(e) => eprintln!("execution event 不是合法 JSON: {e}\n{text}"),
        }
    });
}

/// live preview 的两个旋钮（ADR-0011）。0 表示交给 core 用它的默认值。
#[derive(Clone, Copy, Default)]
pub struct PreviewOptions {
    pub max_points: u32,
    pub budget_ms: u32,
}

/// 一次运行的范围与模式。写成结构体是因为 `start` 的位置参数已经排不下了。
#[derive(Clone, Copy, Default)]
pub struct StartOptions<'a> {
    /// Run to node 的目标。空 = 全图。
    pub targets: &'a [String],
    /// 只运行这些节点（docs/node-run-plan.md R1–R2）。非空时 core 忽略 targets、改用同一组 id。
    pub isolate: &'a [String],
    /// 强制重算这些节点（修订一 V1）：跳过缓存真跑一遍。
    pub force: &'a [String],
    /// Some = live preview（ADR-0011）。与 isolate 同时给是参数错误，由 core 判（R5）。
    pub preview: Option<PreviewOptions>,
    /// 顶层图参数的取值（C ABI 的 `params_json`，param-recipe K3）。None = 全用 default。
    pub params_json: Option<&'a str>,
    /// 宿主注入的源数据（ADR-0017，`run_graph` 的 `sceneId`）。空 = 不注入。
    pub inputs: &'a [core_ffi::RunInput],
}

/// 排队的请求自己持有的那份启动参数：它要活到被抢占的那个退出（ADR-0027）。
struct OwnedStart {
    graph_json: String,
    base_dir: String,
    targets: Vec<String>,
    isolate: Vec<String>,
    force: Vec<String>,
    preview: Option<PreviewOptions>,
    params_json: Option<String>,
    inputs: Vec<core_ffi::RunInput>,
}

impl OwnedStart {
    fn copy_of(graph_json: &str, base_dir: &str, o: &StartOptions<'_>) -> Self {
        OwnedStart {
            graph_json: graph_json.to_string(),
            base_dir: base_dir.to_string(),
            targets: o.targets.to_vec(),
            isolate: o.isolate.to_vec(),
            force: o.force.to_vec(),
            preview: o.preview,
            params_json: o.params_json.map(str::to_string),
            inputs: o.inputs.to_vec(),
        }
    }

    fn options(&self) -> StartOptions<'_> {
        StartOptions {
            targets: &self.targets,
            isolate: &self.isolate,
            force: &self.force,
            preview: self.preview,
            params_json: self.params_json.as_deref(),
            inputs: &self.inputs,
        }
    }
}

/// RunManager 管着的一次运行。真实实现是 `RunHandle`；状态机的测试用可控的假 run。
pub(crate) trait LiveRun: Send + Sync {
    fn run_id(&self) -> &str;
    /// 协作式取消：执行器在节点之间、可取消的算子在循环里看这个标志。
    fn cancel(&self);
    /// 等它退出。可以有好几个线程同时等，都等到同一次退出。
    fn join(&self);
}

impl LiveRun for RunHandle {
    fn run_id(&self) -> &str {
        RunHandle::run_id(self)
    }
    fn cancel(&self) {
        RunHandle::cancel(self)
    }
    fn join(&self) {
        RunHandle::join(self)
    }
}

pub(crate) type Run = Arc<dyn LiveRun>;

/// 从没开跑就作废的请求怎么收场：`"cancelled"`，或启动失败时的 `"error"` + 原因。
type Notify = Box<dyn FnOnce(&'static str, Option<String>) + Send>;

/// 等被抢占的那个退出再开跑的请求（ADR-0027）。
pub(crate) struct Pending {
    run_id: String,
    launch: Box<dyn FnOnce() -> Result<Run, String> + Send>,
    notify: Notify,
}

impl Pending {
    pub(crate) fn new(
        run_id: impl Into<String>,
        launch: impl FnOnce() -> Result<Run, String> + Send + 'static,
        notify: impl FnOnce(&'static str, Option<String>) + Send + 'static,
    ) -> Self {
        Pending {
            run_id: run_id.into(),
            launch: Box::new(launch),
            notify: Box::new(notify),
        }
    }
}

#[derive(Default)]
struct State {
    /// 正在跑的那个。
    active: Option<Run>,
    /// 被抢占、已取消、还没退出的那个：停不下来的算子还在算。有它时 active 一定为空。
    draining: Option<Run>,
    /// 等 draining 退出再开跑的请求，只留最新一个。
    pending: Option<Pending>,
    /// 上一次跑完的那个。留着是为了 3D 视图还能取到它的点云。
    finished: Option<Run>,
    /// 维护窗口里（重扫库目录、热重载，`RunManager::pause`）：新请求只排队，不开跑。
    paused: bool,
}

/// 维护窗口怎么处置运行（`RunManager::pause`）。
#[derive(Clone, Copy, PartialEq, Debug)]
pub enum PauseMode {
    /// 重扫库目录：注册表要重建，在算的 run 手里握着 OperatorDesc 指针，得停下、等它退出；
    /// DLL 不换代，跑完的照样安全 —— 被停掉的那一次留作「上一次完成的」（界面上留着的就是它的结果）。
    KeepResults,
    /// 热重载：上一次跑完的也放掉。只要还有一个 RunHandle 活着，旧 DLL 的引用计数就归不了零（ADR-0009）。
    ReleaseAll,
}

fn lock(inner: &Mutex<State>) -> MutexGuard<'_, State> {
    // 收尾线程里出过一次 panic，不该让此后所有的运行都起不来
    inner.lock().unwrap_or_else(|e| e.into_inner())
}

fn holds(slot: &Option<Run>, run: &Run) -> bool {
    slot.as_ref().is_some_and(|r| r.run_id() == run.run_id())
}

/// 把 run 设成 active，并起它的收尾线程：join 完持锁看自己现在是谁（`settle`）。
fn activate(inner: &Arc<Mutex<State>>, st: &mut State, run: Run) {
    st.active = Some(Arc::clone(&run));
    let inner = Arc::clone(inner);
    // 不能在 command 线程上等 —— 那就退化成同步执行了。
    std::thread::spawn(move || {
        run.join();
        let failed = settle(&inner, &mut lock(&inner), &run);
        // 补发的事件不在锁里发
        if let Some((notify, reason)) = failed {
            notify("error", Some(reason));
        }
    });
}

/// 一个 run 退出之后的状态转移。返回启动失败的那个排队请求，由调用方在锁外补发 error。
fn settle(inner: &Arc<Mutex<State>>, st: &mut State, run: &Run) -> Option<(Notify, String)> {
    if holds(&st.active, run) {
        // 旧 finished 在这一行被 drop → lyflow_run_free → 结果仓回收。
        st.finished = st.active.take();
        return None;
    }
    if !holds(&st.draining, run) {
        // 被维护窗口（pause）拿走的：Paused 自己 join，这里什么都不用做
        return None;
    }
    st.draining = None;
    let Some(Pending { launch, notify, .. }) = st.pending.take() else {
        // 排队的那个已经被取消、没有人顶替它：界面上留着的正是这一次的结果（前端的 resultRunId 还指着它，
        // 节点表也是它的），放掉的话那些「完成」的节点就取不到输出了（review 第二轮）
        st.finished = Some(Arc::clone(run));
        return None;
    };
    // 有人顶替：被抢占的不进 finished 槽 —— 它的结果是残缺的，新的那次一开跑界面就跟过去了
    match launch() {
        Ok(next) => {
            activate(inner, st, next);
            None
        }
        Err(reason) => Some((notify, reason)),
    }
}

/// 启动一次运行（不排队的那一步）。`options` 只需活到这里返回 —— core 在 run_start 里拷一份。
fn launch<R: Runtime>(
    app: &AppHandle<R>,
    core: Arc<Core>,
    graph_json: &str,
    base_dir: &str,
    options: StartOptions<'_>,
    run_id: &str,
) -> Result<Run, String> {
    let ctx = Box::new(EmitCtx {
        app: app.clone(),
        run_id: run_id.to_string(),
    });
    let mut spec = RunSpec::new(graph_json, run_id, base_dir, options.targets);
    spec.isolate = options.isolate;
    spec.force = options.force;
    spec.params_json = options.params_json;
    spec.inputs = options.inputs;
    if let Some(p) = options.preview {
        spec.mode = 1;
        spec.preview_max_points = p.max_points;
        spec.preview_budget_ms = p.budget_ms;
    }
    let handle = unsafe { RunHandle::start(core, spec, trampoline::<R>, ctx) }
        .map_err(|e| e.to_string())?;
    Ok(Arc::new(handle))
}

/// 从没开跑就作废的请求补一条 run_finished（ADR-0027）：前端拿到 runId 就进了「运行中」，
/// 等的就是这一条。
fn emit_never_started<R: Runtime>(
    app: &AppHandle<R>,
    run_id: &str,
    status: &str,
    error: Option<&str>,
) {
    if let Err(e) = app.emit(EVENT_NAME, never_started_event(run_id, status, error)) {
        eprintln!("补发 run_finished 失败 (run {run_id}): {e}");
    }
}

/// 这个 run 不会再有别的事件，所以它就是 seq 0。
fn never_started_event(run_id: &str, status: &str, error: Option<&str>) -> serde_json::Value {
    let mut event = serde_json::json!({
        "schemaVersion": 1,
        "runId": run_id,
        "seq": 0,
        "kind": "run_finished",
        "status": status,
        "durationMs": 0,
    });
    if let Some(message) = error {
        event["error"] = serde_json::json!({
            "phase": "execute", "code": "internal", "message": message,
        });
    }
    event
}

/// Tauri managed state。
#[derive(Clone, Default)]
pub struct RunManager {
    inner: Arc<Mutex<State>>,
    /// 维护窗口结束时通知（`pause` 在这里等上一个窗口）。
    resumed: Arc<std::sync::Condvar>,
}

impl RunManager {
    pub fn new() -> Self {
        Self::default()
    }

    /// 启动一次运行，立即返回 run id —— 不等任何别的 run（ADR-0027）。
    /// 有 run 在算时：取消它、挪进 draining，这一次排队，等它退出再开跑；排队中的旧请求被顶掉。
    /// 前端拿到新 runId 时，被抢占的那个可能还没发出 run_finished(cancelled)：事件按 runId 分流，晚到的无害。
    ///
    /// `options.inputs` 是运行时注入的源数据（ADR-0017）。当场能开跑时只借用；
    /// 要排队的话请求自己拷一份，因为它要活到被抢占的那个退出。
    pub fn start<R: Runtime>(
        &self,
        app: &AppHandle<R>,
        core: Arc<Core>,
        graph_json: &str,
        base_dir: &str,
        options: StartOptions<'_>,
    ) -> Result<String, String> {
        let run_id = ulid::new();
        self.submit(
            || launch(app, Arc::clone(&core), graph_json, base_dir, options, &run_id),
            || {
                let owned = OwnedStart::copy_of(graph_json, base_dir, &options);
                let (launch_app, notify_app) = (app.clone(), app.clone());
                let (launch_id, notify_id) = (run_id.clone(), run_id.clone());
                Pending::new(
                    run_id.clone(),
                    move || {
                        // 开跑时再取 core：排队期间可能热重载换了一代。在提交时那一代上开跑，
                        // 它就一直卸载不掉，跑的还是旧的算子（ADR-0009）
                        let core = core_ffi::core()?;
                        let options = owned.options();
                        launch(&launch_app, core, &owned.graph_json, &owned.base_dir, options, &launch_id)
                    },
                    move |status, error| {
                        emit_never_started(&notify_app, &notify_id, status, error.as_deref())
                    },
                )
            },
        )?;
        Ok(run_id)
    }

    /// 状态机本体（`start` 给它配上 Tauri 的启动与补发）。`now` 在当场能开跑时调，`later` 只在要排队时调 ——
    /// 拷一份注入数据的代价只花在真要排队的那一次。全程不 join。
    pub(crate) fn submit(
        &self,
        now: impl FnOnce() -> Result<Run, String>,
        later: impl FnOnce() -> Pending,
    ) -> Result<(), String> {
        let replaced = {
            let mut st = lock(&self.inner);
            if !st.paused && st.active.is_none() && st.draining.is_none() {
                let run = now()?;
                activate(&self.inner, &mut st, run);
                return Ok(());
            }
            if let Some(active) = st.active.take() {
                active.cancel();
                st.draining = Some(active);
            }
            st.pending.replace(later())
        };
        // 被顶掉的排队请求不补发事件：发起方手里已经是这一次的 runId（前端只认最后发起的那次），
        // 补一条 cancelled 反而会让工具栏闪一下「已取消」。
        drop(replaced);
        Ok(())
    }

    /// 取消指定的运行。id 对不上就什么都不做 —— 用户按 Esc 的那一刻，
    /// 他想取消的可能已经自己跑完了。排队中的那个直接作废，补发 cancelled。
    pub fn cancel(&self, run_id: &str) {
        let dropped = {
            let mut st = lock(&self.inner);
            if let Some(active) = st.active.as_ref().filter(|r| r.run_id() == run_id) {
                active.cancel();
                None
            } else if st.pending.as_ref().is_some_and(|p| p.run_id == run_id) {
                st.pending.take()
            } else {
                None
            }
        };
        if let Some(p) = dropped {
            (p.notify)("cancelled", None);
        }
    }

    /// 进入维护窗口（重扫库目录、热重载，ADR-0027）：在算的与排干中的取消掉，交给返回的 `Paused` 去等；
    /// 窗口里来的请求只排队（只留最新一个）—— `Paused` 放掉时才开跑，开跑时取的是那一刻的 core。
    /// 排队中的请求不作废：它是用户最后一次要的，库重扫完、换完代照样该跑。
    ///
    /// 同一时刻只有一个窗口：已经有一个时在这里等它结束（重扫与热重载在不同线程上，不能交错着改注册表）。
    /// 不要在 Tauri 主线程上调 —— 等的可能是一个停不下来的算子（`Paused::drain`）。
    pub fn pause(&self, mode: PauseMode) -> Paused {
        let (taken, finished) = {
            let mut st = lock(&self.inner);
            while st.paused {
                st = self.resumed.wait(st).unwrap_or_else(|e| e.into_inner());
            }
            st.paused = true;
            let taken: Vec<Run> = [st.active.take(), st.draining.take()].into_iter().flatten().collect();
            let finished = if mode == PauseMode::ReleaseAll { st.finished.take() } else { None };
            (taken, finished)
        };
        for run in &taken {
            run.cancel();
        }
        Paused {
            manager: self.clone(),
            mode,
            taken,
            finished,
            drained: false,
        }
    }

    /// 把一个跑完的 run 放进 finished 槽。`start` 要 AppHandle 推事件，测试里没有 ——
    /// 测试自己起 run、收事件，再交给这里，模拟「上一次跑完、留着给 3D 视图取」的状态。
    #[cfg(test)]
    pub fn adopt_finished(&self, handle: RunHandle) {
        lock(&self.inner).finished = Some(Arc::new(handle));
    }

    /// 没有在算的、排干中的、排队的（测试等「全部收场」用：被抢占的 run 不在 active 里）。
    #[cfg(test)]
    fn is_idle(&self) -> bool {
        let st = lock(&self.inner);
        st.active.is_none() && st.draining.is_none() && st.pending.is_none()
    }

    /// 当前是否还有活跃的运行。
    // 这个类型唯一的只读窗口。目前没有调用方，但删掉再加回来只会让人重新想一遍锁的边界。
    #[allow(dead_code)]
    pub fn active_run_id(&self) -> Option<String> {
        lock(&self.inner)
            .active
            .as_ref()
            .map(|h| h.run_id().to_string())
    }
}

/// 一个维护窗口（`RunManager::pause`）。先 `drain` 等被停掉的那几个退出，做完要做的事，再放掉它 ——
/// 放掉时恢复：窗口里排着队的那个开跑。
pub struct Paused {
    manager: RunManager,
    mode: PauseMode,
    /// 被停掉的那一个（在算的或排干中的，最多一个）。
    taken: Vec<Run>,
    /// ReleaseAll 时一起拿走的上一次完成的那个。
    finished: Option<Run>,
    drained: bool,
}

impl Paused {
    /// 等被停掉的那几个退出。停不下来的算子要算完才会退出，所以不在 Tauri 主线程上调。
    /// ReleaseAll 时等完就放掉它们（连同上一次完成的）：换代之前旧 DLL 的引用要归零。
    pub fn drain(&mut self) {
        for run in &self.taken {
            run.join();
        }
        if self.mode == PauseMode::ReleaseAll {
            self.taken.clear();
            self.finished = None;
        }
        self.drained = true;
    }
}

impl Drop for Paused {
    fn drop(&mut self) {
        if !self.drained {
            self.drain();
        }
        let (failed, replaced) = {
            let mut st = lock(&self.manager.inner);
            // KeepResults：界面上留着的正是被停掉的那一次（前端的 resultRunId 还指着它），留作上一次完成的 ——
            // 放掉的话那些「完成」的节点就取不到输出了
            let replaced = match self.taken.pop() {
                Some(run) => st.finished.replace(run),
                None => None,
            };
            st.paused = false;
            self.manager.resumed.notify_all();
            let failed = match st.pending.take() {
                Some(Pending { launch, notify, .. }) => match launch() {
                    Ok(next) => {
                        activate(&self.manager.inner, &mut st, next);
                        None
                    }
                    Err(reason) => Some((notify, reason)),
                },
                None => None,
            };
            (failed, replaced)
        };
        // 旧的 finished 在锁外放掉（lyflow_run_free），补发的事件也不在锁里发
        drop(replaced);
        if let Some((notify, reason)) = failed {
            notify("error", Some(reason));
        }
    }
}

// 二进制点云载荷（ADR-0006）。布局见 bridge/README.md「二进制点云」。
/// magic 不是装饰：没有它，一段 JSON 错误文本会被前端当成坐标画出来。
pub const CLOUD_MAGIC: u32 = 0x4350_594C; // 'LYPC' 小端

/// f32 切片 → 小端字节。逐个 `to_le_bytes` 在 debug 构建里是一百万次未内联的调用，
/// 3D 视图「事件到渲染」的延迟有一大半花在那上面。
fn append_f32(out: &mut Vec<u8>, values: &[f32]) {
    #[cfg(target_endian = "little")]
    {
        // 小端机器上 f32 的内存布局就是我们要的字节序，整段拷过去即可
        let bytes = unsafe {
            std::slice::from_raw_parts(
                values.as_ptr().cast::<u8>(),
                std::mem::size_of_val(values),
            )
        };
        out.extend_from_slice(bytes);
    }
    #[cfg(not(target_endian = "little"))]
    for v in values {
        out.extend_from_slice(&v.to_le_bytes());
    }
}

pub fn encode_cloud(view: &core_ffi::CloudView) -> Vec<u8> {
    let n = view.point_count() as usize;
    let has_intensity = view.has_intensity();
    let has_normals = view.has_normals();
    let has_rgb = view.has_rgb();
    let rgb_len = if has_rgb { (n * 3 + 3) & !3 } else { 0 };
    let extra =
        if has_intensity { n * 4 } else { 0 } + if has_normals { n * 12 } else { 0 } + rgb_len;
    let mut out = Vec::with_capacity(16 + 24 + n * 12 + extra);
    // flags 按实际写出的通道算，不照抄 core 的：载荷里有什么、位就是什么
    let flags = if has_intensity { core_ffi::CLOUD_HAS_INTENSITY } else { 0 }
        | if has_normals { core_ffi::CLOUD_HAS_NORMALS } else { 0 }
        | if has_rgb { core_ffi::CLOUD_HAS_RGB } else { 0 };

    out.extend_from_slice(&CLOUD_MAGIC.to_le_bytes());
    out.extend_from_slice(&view.point_count().to_le_bytes());
    out.extend_from_slice(&view.total_points().to_le_bytes());
    out.extend_from_slice(&flags.to_le_bytes());
    append_f32(&mut out, &view.bounds());
    append_f32(&mut out, view.xyz());
    if has_intensity {
        append_f32(&mut out, view.intensity());
    }
    // 法线排在强度之后：前端按 flags 里的位依次算偏移
    if has_normals {
        append_f32(&mut out, view.normals());
    }
    // rgb（v12）放在最后、补齐到 4 字节：老解码器不认第 3 位也不看尾部，照样解得开；
    // 以后再加 float 通道也还落在对齐的偏移上
    if has_rgb {
        out.extend_from_slice(view.rgb());
        out.resize(out.len() + (rgb_len - n * 3), 0);
    }
    out
}

pub const TENSOR_MAGIC: u32 = 0x4E54_594C;
pub const INDICES_MAGIC: u32 = 0x5849_594C;

fn append_i64(out: &mut Vec<u8>, values: &[i64]) {
    #[cfg(target_endian = "little")]
    {
        let bytes = unsafe {
            std::slice::from_raw_parts(
                values.as_ptr().cast::<u8>(),
                std::mem::size_of_val(values),
            )
        };
        out.extend_from_slice(bytes);
    }
    #[cfg(not(target_endian = "little"))]
    for v in values {
        out.extend_from_slice(&v.to_le_bytes());
    }
}

fn append_i32(out: &mut Vec<u8>, values: &[i32]) {
    #[cfg(target_endian = "little")]
    {
        let bytes = unsafe {
            std::slice::from_raw_parts(
                values.as_ptr().cast::<u8>(),
                std::mem::size_of_val(values),
            )
        };
        out.extend_from_slice(bytes);
    }
    #[cfg(not(target_endian = "little"))]
    for v in values {
        out.extend_from_slice(&v.to_le_bytes());
    }
}

pub fn encode_tensor(view: &core_ffi::TensorView) -> Vec<u8> {
    let shape = view.shape();
    let data = view.data();
    let mut out = Vec::with_capacity(32 + shape.len() * 8 + data.len() * 4);

    out.extend_from_slice(&TENSOR_MAGIC.to_le_bytes());
    out.extend_from_slice(&view.rank().to_le_bytes());
    out.extend_from_slice(&0u32.to_le_bytes());
    out.extend_from_slice(&view.count().to_le_bytes());
    out.extend_from_slice(&view.offset().to_le_bytes());
    out.extend_from_slice(&view.total().to_le_bytes());
    append_i64(&mut out, shape);
    append_f32(&mut out, data);
    out
}

pub fn encode_indices(view: &core_ffi::IndicesView) -> Vec<u8> {
    let values = view.values();
    let mut out = Vec::with_capacity(24 + values.len() * 4);

    out.extend_from_slice(&INDICES_MAGIC.to_le_bytes());
    out.extend_from_slice(&view.count().to_le_bytes());
    out.extend_from_slice(&view.total().to_le_bytes());
    out.extend_from_slice(&0u32.to_le_bytes());
    out.extend_from_slice(&view.source_cloud_id().to_le_bytes());
    append_i32(&mut out, values);
    out
}

/// 图像载荷（ABI v15，docs/http-transport.md「图像」）。`.lyim` 文件（`lyflow dump`）是同一布局。
pub const IMAGE_MAGIC: u32 = 0x4D49_594C; // 'LYIM' 小端
/// 帧头 12 个 u32。48 是 4 的倍数：u16 / f32 像素在前端能零拷贝开类型化数组。
pub const IMAGE_HEADER_BYTES: usize = 48;

pub fn encode_image(view: &core_ffi::ImageView) -> Vec<u8> {
    let pixels = view.pixels();
    let padded = (pixels.len() + 3) & !3;
    let mut out = Vec::with_capacity(IMAGE_HEADER_BYTES + padded);
    for v in [
        IMAGE_MAGIC,
        view.width(),
        view.height(),
        view.channels(),
        view.depth(),
        view.level(),
        view.full_width(),
        view.full_height(),
        view.row_offset(),
        view.row_count(),
        view.row_bytes(),
        0,
    ] {
        out.extend_from_slice(&v.to_le_bytes());
    }
    // 像素本来就是小端字节（core 按本机字节序存，桥接只跑在小端机器上）
    out.extend_from_slice(pixels);
    out.resize(IMAGE_HEADER_BYTES + padded, 0);
    out
}

#[cfg(test)]
mod tests;
