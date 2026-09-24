//! 运行的生命周期管理。D3：同一时刻一个活跃 run，新 run 抢占旧 run。
//! 最多同时持有两份 run 的索引（正在跑的 + 上一次完成的），详见 bridge/README.md。

use std::ffi::CStr;
use std::os::raw::{c_char, c_void};
use std::sync::{Arc, Mutex};

use tauri::{AppHandle, Emitter};

use crate::core_ffi::{self, Core, RunHandle, RunSpec};
use crate::ulid;

/// 事件推流的前端事件名。前端 `onExecutionEvent` 监听的就是它。
pub const EVENT_NAME: &str = "execution-event";

/// 回调里要用到的东西。裸指针传给 C++，生命期由 `RunHandle` 持有 ——
/// C ABI 保证 join 返回后不再回调，所以 `RunHandle::drop` 里 join 完才释放它。
struct EmitCtx {
    app: AppHandle,
    run_id: String,
}

/// C++ 工作线程调过来的回调。`catch_unwind` 不是摆设：panic 展开穿过 C++ 栈帧
/// 是未定义行为，而丢一条事件只会让前端的 seq 检查 warn 一句。
unsafe extern "C" fn trampoline(event_json: *const c_char, user: *mut c_void) {
    let _ = std::panic::catch_unwind(|| {
        if event_json.is_null() || user.is_null() {
            return;
        }
        let ctx = &*(user as *const EmitCtx);
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
    /// 只运行这些节点（docs/node-run-plan.md R1–R3）。非空时 core 忽略 targets、改用同一组 id。
    pub isolate: &'a [String],
    /// Some = live preview（ADR-0011）。与 isolate 同时给是参数错误，由 core 判（R5）。
    pub preview: Option<PreviewOptions>,
}

#[derive(Default)]
struct State {
    /// 正在跑的那个。
    active: Option<Arc<RunHandle>>,
    /// 上一次跑完的那个。留着是为了 3D 视图还能取到它的点云。
    finished: Option<Arc<RunHandle>>,
}

/// Tauri managed state。
#[derive(Clone, Default)]
pub struct RunManager {
    inner: Arc<Mutex<State>>,
}

impl RunManager {
    pub fn new() -> Self {
        Self::default()
    }

    /// 启动一次运行，返回 run id。抢占是同步的：旧 run 的 cancel+join 在本函数里做完，
    /// 前端拿到新 runId 时旧 run 的 run_finished(cancelled) 一定已经发出去了。
    pub fn start(
        &self,
        app: &AppHandle,
        core: Arc<Core>,
        graph_json: &str,
        base_dir: &str,
        options: StartOptions<'_>,
    ) -> Result<String, String> {
        let previous = self.inner.lock().unwrap().active.take();
        if let Some(prev) = previous {
            prev.cancel();
            prev.join();
            // 被抢占的 run 不进 finished 槽：它的结果是残缺的，
            // 留着只会让 3D 视图显示半张图。
        }

        let run_id = ulid::new();
        let ctx = Box::new(EmitCtx {
            app: app.clone(),
            run_id: run_id.clone(),
        });
        let mut spec = RunSpec::new(graph_json, &run_id, base_dir, options.targets);
        spec.isolate = options.isolate;
        if let Some(p) = options.preview {
            spec.mode = 1;
            spec.preview_max_points = p.max_points;
            spec.preview_budget_ms = p.budget_ms;
        }
        let handle = unsafe { RunHandle::start(core, spec, trampoline, ctx) }
            .map_err(|e| e.to_string())?;
        let handle = Arc::new(handle);

        self.inner.lock().unwrap().active = Some(Arc::clone(&handle));

        // 后台等它结束，然后挪进 finished 槽。
        // 不能在 command 线程上等 —— 那就退化成同步执行了。
        let inner = Arc::clone(&self.inner);
        std::thread::spawn(move || {
            handle.join();
            let mut st = inner.lock().unwrap();
            let still_active = st
                .active
                .as_ref()
                .map(|h| h.run_id() == handle.run_id())
                .unwrap_or(false);
            if still_active {
                st.active = None;
                // 旧 finished 在这一行被 drop → lyflow_run_free → 结果仓回收。
                st.finished = Some(handle);
            }
        });

        Ok(run_id)
    }

    /// 取消指定的运行。id 对不上就什么都不做 —— 用户按 Esc 的那一刻，
    /// 他想取消的可能已经自己跑完了。
    pub fn cancel(&self, run_id: &str) {
        let st = self.inner.lock().unwrap();
        if let Some(active) = st.active.as_ref() {
            if active.run_id() == run_id {
                active.cancel();
            }
        }
    }

    /// 放掉全部 run。热重载前必须调 —— 只要还有一个 RunHandle 活着，
    /// 旧 DLL 的引用计数就归不了零，新一代加载了也顶不掉它（ADR-0009）。
    pub fn drop_all(&self) {
        let (active, finished) = {
            let mut st = self.inner.lock().unwrap_or_else(|e| e.into_inner());
            (st.active.take(), st.finished.take())
        };
        if let Some(run) = &active {
            run.cancel();
            run.join();
        }
        drop(active);
        drop(finished);
    }

    /// 当前是否还有活跃的运行。
    // 这个类型唯一的只读窗口。目前只有测试在用，但删掉再加回来只会让人重新想一遍锁的边界。
    #[allow(dead_code)]
    pub fn active_run_id(&self) -> Option<String> {
        self.inner
            .lock()
            .unwrap()
            .active
            .as_ref()
            .map(|h| h.run_id().to_string())
    }
}

/// 二进制点云载荷（ADR-0006）。布局见 bridge/README.md「二进制点云」。
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
    let extra = if has_intensity { n * 4 } else { 0 } + if has_normals { n * 12 } else { 0 };
    let mut out = Vec::with_capacity(16 + 24 + n * 12 + extra);

    out.extend_from_slice(&CLOUD_MAGIC.to_le_bytes());
    out.extend_from_slice(&view.point_count().to_le_bytes());
    out.extend_from_slice(&view.total_points().to_le_bytes());
    out.extend_from_slice(&view.flags().to_le_bytes());
    append_f32(&mut out, &view.bounds());
    append_f32(&mut out, view.xyz());
    if has_intensity {
        append_f32(&mut out, view.intensity());
    }
    // 法线排在强度之后：前端按 flags 里的两个位依次算偏移
    if has_normals {
        append_f32(&mut out, view.normals());
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

#[cfg(test)]
mod tests {
    use super::*;
    use crate::core_ffi::CLOUD_HAS_INTENSITY;
    use std::sync::Mutex;
    use std::time::{Duration, Instant};

    /// 测试侧的事件收集器。不经 Tauri —— 值得测的是 libloading 加载 DLL →
    /// C ABI 启动 run → 工作线程回调 → 事件 JSON → cancel/join/free 这一整条。
    struct Collector {
        events: Mutex<Vec<serde_json::Value>>,
    }

    unsafe extern "C" fn collect(event_json: *const c_char, user: *mut c_void) {
        let ctx = &*(user as *const Collector);
        let text = CStr::from_ptr(event_json).to_str().expect("事件不是 UTF-8");
        let value: serde_json::Value = serde_json::from_str(text).expect("事件不是合法 JSON");
        ctx.events.lock().unwrap().push(value);
    }

    struct Fixture {
        events: Vec<serde_json::Value>,
        run_id: String,
        // 保持 handle 活着，结果仓才不会在断言之前被 freeRun 清掉
        _handle: RunHandle,
        core: Arc<crate::core_ffi::Core>,
    }

    impl Fixture {
        fn kind(&self, kind: &str) -> Vec<&serde_json::Value> {
            self.events
                .iter()
                .filter(|e| e["kind"] == kind)
                .collect()
        }
        fn final_state(&self, node: &str) -> String {
            self.events
                .iter()
                .filter(|e| e["kind"] == "node_state" && e["nodeId"] == node)
                .last()
                .map(|e| e["state"].as_str().unwrap_or("").to_string())
                .unwrap_or_default()
        }
        fn run_status(&self) -> String {
            self.kind("run_finished")
                .last()
                .map(|e| e["status"].as_str().unwrap_or("").to_string())
                .unwrap_or_default()
        }
        fn node_event(&self, node: &str, state: &str) -> serde_json::Value {
            self.events
                .iter()
                .find(|e| e["kind"] == "node_state" && e["nodeId"] == node && e["state"] == state)
                .cloned()
                .unwrap_or(serde_json::Value::Null)
        }
    }

    fn run(doc: serde_json::Value, base_dir: &str) -> Fixture {
        run_with(doc, base_dir, |_| {})
    }

    fn run_with(
        doc: serde_json::Value,
        base_dir: &str,
        during: impl FnOnce(&RunHandle),
    ) -> Fixture {
        run_spec(doc, base_dir, &[], 0, during)
    }

    /// isolate / mode 是单节点运行那几条用例要的（docs/node-run-plan.md）。
    fn run_spec(
        doc: serde_json::Value,
        base_dir: &str,
        isolate: &[String],
        mode: i32,
        during: impl FnOnce(&RunHandle),
    ) -> Fixture {
        let core = crate::core_ffi::core().expect("加载 core 失败");
        let run_id = ulid::new();
        let ctx = Box::new(Collector {
            events: Mutex::new(Vec::new()),
        });
        let ptr = &*ctx as *const Collector;
        let graph = doc.to_string();
        let mut spec = RunSpec::new(&graph, &run_id, base_dir, &[]);
        spec.isolate = isolate;
        spec.mode = mode;
        let handle = unsafe { RunHandle::start(Arc::clone(&core), spec, collect, ctx) }
            .expect("启动运行失败");
        during(&handle);
        handle.join();
        // join 返回后回调保证不再被触发，这时候读收集到的事件才是安全的。
        let events = unsafe { (*ptr).events.lock().unwrap().clone() };
        Fixture {
            events,
            run_id,
            _handle: handle,
            core,
        }
    }

    /// seed 参数化不是装饰：缓存是进程级的，两个测试用同一张图的话
    /// 后跑的那个会拿到 skipped 而不是 done，而 cargo 默认并行跑测试。
    fn two_node_graph(seed: i64) -> serde_json::Value {
        serde_json::json!({
            "schemaVersion": 1, "id": "t",
            "nodes": [
                {"id": "g", "op": "gen.synthetic",
                 "params": {"pointCount": 20000, "seed": seed}},
                {"id": "v", "op": "filter.voxel_grid",
                 "params": {"leafSize": [0.02, 0.02, 0.02]}}
            ],
            "edges": [{"id": "e", "from": {"node": "g", "port": "cloud"},
                                  "to": {"node": "v", "port": "cloud"}}]
        })
    }

    #[test]
    fn two_node_run_emits_events_in_order_with_dense_seq() {
        let f = run(two_node_graph(101), "");

        assert_eq!(f.run_status(), "ok");
        for (i, e) in f.events.iter().enumerate() {
            assert_eq!(e["seq"], i as i64, "seq 不连续：第 {i} 条是 {}", e["seq"]);
            assert_eq!(e["runId"], f.run_id.as_str());
            assert_eq!(e["schemaVersion"], 1);
        }

        let trace: Vec<String> = f
            .events
            .iter()
            .filter(|e| e["kind"] == "node_state")
            .map(|e| format!("{}:{}", e["nodeId"].as_str().unwrap(), e["state"].as_str().unwrap()))
            .collect();
        assert_eq!(
            trace,
            vec!["g:pending", "g:running", "g:done", "v:pending", "v:running", "v:done"]
        );
        assert_eq!(f.events.first().unwrap()["kind"], "run_started");
        assert_eq!(f.events.last().unwrap()["kind"], "run_finished");
    }

    /// ADR-0022：同一份 summary 有两个出口 —— run_finished 事件里那一份，
    /// 和按 runId 从 C ABI 取回来的那一份。它们必须逐字段相等。
    #[test]
    fn run_summary_comes_back_over_the_abi_and_in_the_event() {
        let f = run(two_node_graph(104), "");
        assert_eq!(f.run_status(), "ok");

        let finished = *f.kind("run_finished").last().expect("没有 run_finished");
        let in_event = finished["summary"].clone();
        assert!(in_event.is_object(), "run_finished 没带 summary: {finished}");
        assert_eq!(in_event["status"], "ok");
        assert_eq!(in_event["runId"], f.run_id.as_str());
        assert_eq!(in_event["nodes"]["g"]["state"], "done");
        assert_eq!(in_event["nodes"]["v"]["outputsAvailable"], true);
        // 这张图没声明 outputs，三态表就是空的 —— 不是 null
        assert!(in_event["outputs"].as_object().unwrap().is_empty());
        assert!(in_event["decisions"].as_object().unwrap().is_empty());

        let raw = f
            .core
            .run_summary(&f.run_id)
            .expect("run_summary 调用失败")
            .expect("run 已经结束了，不该是 None");
        let over_abi: serde_json::Value = serde_json::from_str(&raw).unwrap();
        assert_eq!(over_abi, in_event);

        // 不存在的 run 返回 None 而不是 "{}"：「没有」与「跑完了、什么都没有」是两件事
        assert!(f.core.run_summary("no-such-run").unwrap().is_none());
    }

    #[test]
    fn output_cloud_binary_header_is_correct() {
        let f = run(two_node_graph(102), "");
        assert_eq!(f.run_status(), "ok");

        let view = f
            .core
            .output_cloud(&f.run_id, "g", "cloud", 5_000)
            .expect("取不到点云");
        assert_eq!(view.total_points(), 20_000);
        assert!(view.point_count() <= 5_000 && view.point_count() > 4_000);
        assert!(view.has_intensity());

        let bytes = encode_cloud(&view);
        let u32_at = |off: usize| {
            u32::from_le_bytes(bytes[off..off + 4].try_into().unwrap())
        };
        let f32_at = |off: usize| f32::from_le_bytes(bytes[off..off + 4].try_into().unwrap());

        assert_eq!(u32_at(0), CLOUD_MAGIC, "magic 不对");
        assert_eq!(u32_at(4), view.point_count());
        assert_eq!(u32_at(8), 20_000);
        assert_eq!(u32_at(12) & CLOUD_HAS_INTENSITY, CLOUD_HAS_INTENSITY);

        // 16..40 是 bounds；合成云是有体积的，min 必须小于 max
        for i in 0..3 {
            assert!(f32_at(16 + i * 4) < f32_at(16 + (i + 3) * 4), "bounds[{i}] 不合法");
        }

        let n = view.point_count() as usize;
        assert_eq!(bytes.len(), 16 + 24 + n * 12 + n * 4, "载荷长度与头部对不上");

        // 降采样之后点数必须变少，但不能变成 0 —— 这两条一起才说明算子真的跑了
        let voxel = f.core.output_cloud(&f.run_id, "v", "cloud", 0).unwrap();
        assert!(voxel.total_points() > 0);
        assert!(voxel.total_points() < view.total_points());
    }

    /// M3 尾巴 c：法线进了点云载荷，3D 视图的「法线着色」才有东西可画。
    #[test]
    fn cloud_payload_carries_normals_when_the_op_produces_them() {
        let doc = serde_json::json!({
            "schemaVersion": 1, "id": "t",
            "nodes": [
                {"id": "g", "op": "gen.synthetic",
                 "params": {"pointCount": 4000, "seed": 909}},
                {"id": "n", "op": "features.normals", "params": {"kSearch": 10}}
            ],
            "edges": [{"id": "e", "from": {"node": "g", "port": "cloud"},
                                  "to": {"node": "n", "port": "cloud"}}]
        });
        let f = run(doc, "");
        assert_eq!(f.run_status(), "ok", "{:#?}", f.events);

        let plain = f.core.output_cloud(&f.run_id, "g", "cloud", 0).unwrap();
        assert!(!plain.has_normals(), "源头本来就没有法线");

        let view = f.core.output_cloud(&f.run_id, "n", "cloud", 0).unwrap();
        assert!(view.has_normals(), "features.normals 的输出应当带法线");
        assert_eq!(view.normals().len(), view.point_count() as usize * 3);

        let bytes = encode_cloud(&view);
        let n = view.point_count() as usize;
        let flags = u32::from_le_bytes(bytes[12..16].try_into().unwrap());
        assert_eq!(flags & crate::core_ffi::CLOUD_HAS_NORMALS, 2);
        // 布局：头 40 字节 + xyz + intensity + normals
        assert_eq!(bytes.len(), 16 + 24 + n * 12 + n * 4 + n * 12);
        let first = f32::from_le_bytes(bytes[16 + 24 + n * 12 + n * 4..][..4].try_into().unwrap());
        assert_eq!(first, view.normals()[0]);
    }

    fn u32_at(bytes: &[u8], off: usize) -> u32 {
        u32::from_le_bytes(bytes[off..off + 4].try_into().unwrap())
    }
    fn u64_at(bytes: &[u8], off: usize) -> u64 {
        u64::from_le_bytes(bytes[off..off + 8].try_into().unwrap())
    }
    fn i64_at(bytes: &[u8], off: usize) -> i64 {
        i64::from_le_bytes(bytes[off..off + 8].try_into().unwrap())
    }
    fn i32_at(bytes: &[u8], off: usize) -> i32 {
        i32::from_le_bytes(bytes[off..off + 4].try_into().unwrap())
    }
    fn f32_at(bytes: &[u8], off: usize) -> f32 {
        f32::from_le_bytes(bytes[off..off + 4].try_into().unwrap())
    }

    fn passthrough_graph(seed: i64, count: i64) -> serde_json::Value {
        serde_json::json!({
            "schemaVersion": 1, "id": "t",
            "nodes": [
                {"id": "g", "op": "gen.synthetic",
                 "params": {"pointCount": count, "seed": seed}},
                {"id": "p", "op": "filter.passthrough",
                 "params": {"field": "z", "min": -100.0, "max": 100.0}}
            ],
            "edges": [{"id": "e", "from": {"node": "g", "port": "cloud"},
                                  "to": {"node": "p", "port": "cloud"}}]
        })
    }

    #[test]
    fn indices_payload_header_is_correct_and_carries_source_cloud_id() {
        let f = run(passthrough_graph(4101, 5000), "");
        assert_eq!(f.run_status(), "ok", "{:#?}", f.events);

        let all = f
            .core
            .output_indices(&f.run_id, "p", "indices", 0, 0)
            .expect("取不到下标");
        assert_eq!(all.total(), 5_000);
        assert_eq!(all.count(), 5_000, "min/max 放到 ±100，所有点都该留下");
        assert_ne!(all.source_cloud_id(), 0, "sourceCloudId 应当指向上游那片云");
        assert_eq!(all.values().first().copied(), Some(0));
        assert_eq!(all.values().last().copied(), Some(4_999));

        let bytes = encode_indices(&all);
        assert_eq!(u32_at(&bytes, 0), INDICES_MAGIC, "magic 不对");
        assert_eq!(u32_at(&bytes, 4), 5_000);
        assert_eq!(u32_at(&bytes, 8), 5_000);
        assert_eq!(u32_at(&bytes, 12), 0, "flags 这一版保留 0");
        assert_eq!(u64_at(&bytes, 16), all.source_cloud_id());
        assert_eq!(bytes.len(), 24 + 5_000 * 4, "载荷长度与头部对不上");
        assert_eq!(i32_at(&bytes, 24), 0);
        assert_eq!(i32_at(&bytes, 24 + 4_999 * 4), 4_999);
    }

    #[test]
    fn indices_paging_keeps_total_and_clips_at_the_tail() {
        let f = run(passthrough_graph(4102, 1200), "");
        assert_eq!(f.run_status(), "ok", "{:#?}", f.events);

        let all = f.core.output_indices(&f.run_id, "p", "indices", 0, 0).unwrap();
        assert_eq!(all.count(), 1_200);

        let page = f.core.output_indices(&f.run_id, "p", "indices", 500, 64).unwrap();
        assert_eq!(page.count(), 64);
        assert_eq!(page.total(), 1_200, "total 是全量，不随切片变");
        assert_eq!(page.source_cloud_id(), all.source_cloud_id());
        assert_eq!(page.values(), &all.values()[500..564]);
        let page_bytes = encode_indices(&page);
        assert_eq!(page_bytes.len(), 24 + 64 * 4);
        assert_eq!(u32_at(&page_bytes, 4), 64);
        assert_eq!(u32_at(&page_bytes, 8), 1_200);
        assert_eq!(i32_at(&page_bytes, 24), all.values()[500]);

        let tail = f.core.output_indices(&f.run_id, "p", "indices", 1_190, 64).unwrap();
        assert_eq!(tail.count(), 10, "尾巴上要多少给多少，不越界");
        assert_eq!(tail.values(), &all.values()[1_190..1_200]);

        let past = f.core.output_indices(&f.run_id, "p", "indices", 9_999, 16).unwrap();
        assert_eq!(past.count(), 0);
        assert!(past.values().is_empty(), "count=0 时 C 侧给的是 nullptr");
        assert_eq!(past.total(), 1_200);
        let past_bytes = encode_indices(&past);
        assert_eq!(past_bytes.len(), 24, "空切片只有帧头");
        assert_eq!(u32_at(&past_bytes, 4), 0);
    }

    #[test]
    fn tensor_and_indices_reject_ports_of_the_wrong_type() {
        let f = run(passthrough_graph(4103, 800), "");
        assert_eq!(f.run_status(), "ok", "{:#?}", f.events);

        assert!(f.core.output_tensor(&f.run_id, "p", "cloud", 0, 0).is_err());
        assert!(f.core.output_tensor(&f.run_id, "p", "indices", 0, 0).is_err());
        assert!(f.core.output_indices(&f.run_id, "p", "cloud", 0, 0).is_err());
        assert!(f.core.output_indices(&f.run_id, "p", "nope", 0, 0).is_err());
        assert!(f.core.output_tensor("no-such-run", "p", "tensor", 0, 0).is_err());
    }

    #[test]
    fn encode_tensor_frame_matches_the_documented_layout() {
        let core = crate::core_ffi::core().expect("加载 core 失败");
        let shape: Vec<i64> = vec![2, 3, 4];
        let data: Vec<f32> = (0..6).map(|i| i as f32 * 0.25 - 1.0).collect();
        let view = unsafe { core_ffi::TensorView::borrowed(Arc::clone(&core), 6, 24, &shape, &data) };

        assert_eq!(view.rank(), 3);
        assert_eq!(view.count(), 6);
        assert_eq!(view.offset(), 6);
        assert_eq!(view.total(), 24);
        assert_eq!(view.shape(), &shape[..]);
        assert_eq!(view.data(), &data[..]);

        let bytes = encode_tensor(&view);
        assert_eq!(u32_at(&bytes, 0), TENSOR_MAGIC, "magic 不对");
        assert_eq!(u32_at(&bytes, 4), 3, "rank");
        assert_eq!(u32_at(&bytes, 8), 0, "flags 这一版保留 0");
        assert_eq!(u32_at(&bytes, 12), 6, "count");
        assert_eq!(u64_at(&bytes, 16), 6, "offset");
        assert_eq!(u64_at(&bytes, 24), 24, "total");
        assert_eq!(i64_at(&bytes, 32), 2);
        assert_eq!(i64_at(&bytes, 40), 3);
        assert_eq!(i64_at(&bytes, 48), 4);
        assert_eq!(bytes.len(), 32 + 3 * 8 + 6 * 4);
        for (i, expected) in data.iter().enumerate() {
            assert_eq!(f32_at(&bytes, 32 + 3 * 8 + i * 4), *expected, "data[{i}]");
        }
        assert_eq!(32 % 8, 0, "shape 必须 8 字节对齐，前端才能零拷贝开 BigInt64Array");
        assert_eq!((32 + 3 * 8) % 4, 0, "数据必须 4 字节对齐");

        let empty = unsafe { core_ffi::TensorView::borrowed(core, 24, 24, &shape, &[]) };
        assert_eq!(empty.count(), 0);
        assert!(empty.data().is_empty());
        let empty_bytes = encode_tensor(&empty);
        assert_eq!(empty_bytes.len(), 32 + 3 * 8, "空切片只有帧头加 shape");
        assert_eq!(u32_at(&empty_bytes, 12), 0);
        assert_eq!(u64_at(&empty_bytes, 16), 24);
    }

    #[test]
    fn bad_param_marks_the_exact_input_box() {
        let doc = serde_json::json!({
            "schemaVersion": 1, "id": "t",
            "nodes": [
                {"id": "g", "op": "gen.synthetic", "params": {"pointCount": 1000}},
                {"id": "v", "op": "filter.voxel_grid", "params": {"leafSize": [0, 0.01, 0.01]}},
                {"id": "p", "op": "filter.passthrough"}
            ],
            "edges": [
                {"id": "e1", "from": {"node": "g", "port": "cloud"}, "to": {"node": "v", "port": "cloud"}},
                {"id": "e2", "from": {"node": "v", "port": "cloud"}, "to": {"node": "p", "port": "cloud"}}
            ]
        });
        let f = run(doc, "");

        assert_eq!(f.run_status(), "error");
        assert_eq!(f.final_state("g"), "done");
        assert_eq!(f.final_state("v"), "error");
        // 下游是 cancelled + upstream_failed，不是 skipped（skipped 留给缓存命中）
        assert_eq!(f.final_state("p"), "cancelled");

        let v = f.node_event("v", "error");
        assert_eq!(v["errors"][0]["paramPath"], "leafSize");
        assert_eq!(v["errors"][0]["code"], "bad_param");
        assert_eq!(v["error"], v["errors"][0]);

        let p = f.node_event("p", "cancelled");
        assert_eq!(p["errors"][0]["code"], "upstream_failed");
    }

    #[test]
    fn cancel_stops_a_heavy_run_and_joins_within_a_second() {
        // 20 个节点、每个都要处理三百万点：不取消的话要跑好几秒。
        let mut nodes = vec![serde_json::json!(
            {"id": "n0", "op": "gen.synthetic", "params": {"pointCount": 3000000}}
        )];
        let mut edges = Vec::new();
        for i in 1..20 {
            nodes.push(serde_json::json!(
                {"id": format!("n{i}"), "op": "filter.voxel_grid",
                 "params": {"leafSize": [0.001, 0.001, 0.001]}}
            ));
            edges.push(serde_json::json!({
                "id": format!("e{i}"),
                "from": {"node": format!("n{}", i - 1), "port": "cloud"},
                "to": {"node": format!("n{i}"), "port": "cloud"}
            }));
        }
        let doc = serde_json::json!({
            "schemaVersion": 1, "id": "t", "nodes": nodes, "edges": edges
        });

        let started = Instant::now();
        let f = run_with(doc, "", |handle| handle.cancel());
        let elapsed = started.elapsed();

        assert_eq!(f.run_status(), "cancelled");
        assert!(
            elapsed < Duration::from_secs(1),
            "取消后 join 花了 {elapsed:?}，协作式取消的轮询间隔太大"
        );
        // 没有任何节点应当报 error —— 取消不是失败
        for e in f.kind("node_state") {
            assert_ne!(e["state"], "error", "取消不该产生 error: {e}");
        }
    }

    #[test]
    fn chinese_path_save_then_load_roundtrip() {
        let dir = std::env::temp_dir().join("lyflow 桥接 中文测试");
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let base = dir.to_string_lossy().into_owned();

        let save = serde_json::json!({
            "schemaVersion": 1, "id": "t",
            "nodes": [
                {"id": "g", "op": "gen.synthetic", "params": {"pointCount": 3000, "seed": 5}},
                {"id": "w", "op": "io.save_pcd", "params": {"path": "输出 点云.pcd"}}
            ],
            "edges": [{"id": "e", "from": {"node": "g", "port": "cloud"},
                                  "to": {"node": "w", "port": "cloud"}}]
        });
        let f = run(save, &base);
        assert_eq!(f.run_status(), "ok", "存盘失败: {:#?}", f.events);
        assert!(dir.join("输出 点云.pcd").exists());

        let load = serde_json::json!({
            "schemaVersion": 1, "id": "t",
            "nodes": [{"id": "r", "op": "io.load_pcd", "params": {"path": "输出 点云.pcd"}}],
            "edges": []
        });
        let f = run(load, &base);
        assert_eq!(f.run_status(), "ok", "读盘失败: {:#?}", f.events);
        let view = f.core.output_cloud(&f.run_id, "r", "cloud", 0).unwrap();
        assert_eq!(view.total_points(), 3000);
        assert!(view.has_intensity(), "强度通道在 PCD 往返中丢了");

        let _ = std::fs::remove_dir_all(&dir);
    }

    /// 单节点运行（docs/node-run-plan.md R1–R3）走的是 C ABI v11 的 isolate 字段：
    /// 上游命中缓存、自己强制重算、下游不进计划。
    #[test]
    fn isolate_reruns_only_that_node_over_the_abi() {
        // 三节点：g → v → p。先全图跑一遍，把 g 的结果放进结果仓
        let doc = serde_json::json!({
            "schemaVersion": 1, "id": "t",
            "nodes": [
                {"id": "g", "op": "gen.synthetic", "params": {"pointCount": 5000, "seed": 7301}},
                {"id": "v", "op": "filter.voxel_grid", "params": {"leafSize": [0.02, 0.02, 0.02]}},
                {"id": "p", "op": "filter.passthrough"}
            ],
            "edges": [
                {"id": "e1", "from": {"node": "g", "port": "cloud"}, "to": {"node": "v", "port": "cloud"}},
                {"id": "e2", "from": {"node": "v", "port": "cloud"}, "to": {"node": "p", "port": "cloud"}}
            ]
        });
        let full = run(doc.clone(), "");
        assert_eq!(full.run_status(), "ok", "{:#?}", full.events);

        let isolate = vec!["v".to_string()];
        let f = run_spec(doc, "", &isolate, 0, |_| {});
        assert_eq!(f.run_status(), "ok", "{:#?}", f.events);
        let started = f.kind("run_started")[0].clone();
        assert_eq!(started["isolate"], serde_json::json!(["v"]));
        assert_eq!(started["targets"], serde_json::json!(["v"]), "给了 isolate，targets 取同一组 id");
        assert_eq!(started["mode"], "full");

        let g = f.node_event("g", "skipped");
        assert_eq!(g["stats"]["cached"], true, "上游只取缓存: {g}");
        let v = f.node_event("v", "done");
        assert!(v.is_object(), "v 应当真跑一遍（done 而不是 skipped）: {:#?}", f.events);
        assert!(v["stats"]["cached"].is_null(), "强制重算的节点不带 cached: {v}");
        assert!(
            !started["plan"].as_array().unwrap().iter().any(|id| id == "p"),
            "下游不进计划: {started}"
        );
        assert_eq!(f.final_state("p"), "", "下游一条事件都不该有");
    }

    /// R2：上游没有当前 cacheKey 的结果 —— 开跑前整次失败，诊断指向缺结果的上游，零执行。
    #[test]
    fn isolate_without_upstream_results_fails_before_running_anything() {
        let isolate = vec!["v".to_string()];
        let f = run_spec(two_node_graph(7302), "", &isolate, 0, |_| {});
        assert_eq!(f.run_status(), "error");
        let finished = f.kind("run_finished")[0].clone();
        assert_eq!(finished["error"]["code"], "upstream_not_ready", "{finished}");
        let diags = finished["diagnostics"].as_array().expect("run_finished 应带 diagnostics");
        assert_eq!(diags.len(), 1, "{finished}");
        assert_eq!(diags[0]["nodeId"], "g");
        assert_eq!(diags[0]["code"], "upstream_not_ready");
        assert!(f.kind("node_state").is_empty(), "一个节点都不该动: {:#?}", f.events);
    }

    /// R5：预览与 isolate 不组合，core 报参数错误。
    #[test]
    fn isolate_with_preview_is_a_bad_input() {
        let isolate = vec!["v".to_string()];
        let f = run_spec(two_node_graph(7303), "", &isolate, 1, |_| {});
        assert_eq!(f.run_status(), "error");
        assert_eq!(f.kind("run_finished")[0]["error"]["code"], "bad_input");
        assert!(f.kind("node_state").is_empty());
    }

    #[test]
    fn run_manager_reports_no_active_run_before_anything_starts() {
        let manager = RunManager::new();
        assert!(manager.active_run_id().is_none());
        // 取消一个不存在的 run 不该 panic —— 用户按 Esc 时那个 run 可能刚好跑完了
        manager.cancel("no-such-run");
    }
}
