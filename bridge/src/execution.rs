//! 运行的生命周期管理。
//!
//! D3：同一时刻只有一个活跃 run，**新 run 抢占旧 run**。这是 live preview
//! （M4）的前提 —— 拖参数时每一帧都要能打断上一帧，所以接口从第一天就是
//! 异步 + 可取消，而不是先做个同步版本再回来改。
//!
//! 内存上界由这里决定：最多同时持有两份 run 的结果 ——
//! 正在跑的那个，和上一次跑完的那个（用户可能正在 3D 视图里看它）。
//! 第三个出现时，最老的那个 `RunHandle` 被 drop，`lyflow_run_free` 顺手
//! 把它在结果仓里的东西全删掉。

use std::ffi::CStr;
use std::os::raw::{c_char, c_void};
use std::sync::{Arc, Mutex};

use tauri::{AppHandle, Emitter};

use crate::core_ffi::{self, Core, RunHandle};
use crate::ulid;

/// 事件推流的前端事件名。前端 `onExecutionEvent` 监听的就是它。
pub const EVENT_NAME: &str = "execution-event";

/// 回调里要用到的东西。裸指针传给 C++，生命期由 `RunHandle` 持有 ——
/// C ABI 保证 join 返回后不再回调，所以 `RunHandle::drop` 里 join 完才释放它。
struct EmitCtx {
    app: AppHandle,
    run_id: String,
}

/// C++ 工作线程调过来的回调。
///
/// `catch_unwind` 不是防御性编程的摆设：这里是**跨语言边界**，
/// panic 展开穿过 C++ 栈帧是未定义行为。序列化失败、emit 失败都不该
/// 让整个进程死掉 —— 丢一条事件，前端的 seq 检查会 warn 出来。
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

    /// 启动一次运行，返回 run id。
    ///
    /// 抢占是**同步**的：旧 run 的 cancel + join 在本函数里做完才返回。
    /// 这样前端拿到新 run id 的时候，旧 run 的 `run_finished(cancelled)`
    /// 一定已经发出去了，前端的「丢弃过期 runId 的事件」规则不会漏掉它。
    /// 代价是：如果旧 run 卡在一个不可取消的 PCL 算子里，这里会等它跑完。
    pub fn start(
        &self,
        app: &AppHandle,
        core: Arc<Core>,
        graph_json: &str,
        base_dir: &str,
        targets: &[String],
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
        let handle = unsafe {
            RunHandle::start(
                core,
                graph_json,
                &run_id,
                base_dir,
                targets,
                trampoline,
                ctx,
            )
        }
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

    /// 当前是否还有活跃的运行。
    // 目前只有测试在用。留着是因为 M3 的「将重算 N 个节点」提示要查它，
    // 而它是这个类型唯一的只读窗口 —— 删掉再加回来只会让人重新想一遍锁的边界。
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

/// 二进制点云载荷（ADR-0006）。
///
/// 布局（小端）：
/// ```text
/// u32 magic 'LYPC' | u32 pointCount | u32 totalPoints | u32 flags
/// f32 bounds[6] | f32 xyz[3n] | [f32 intensity[n]]
/// ```
/// 前端 `new Float32Array(buffer, offset, len)` 零拷贝进 attribute。
/// magic 不是装饰：IPC 上游出错时返回的可能是一段 JSON 错误文本，
/// 没有 magic 的话前端会把它当点云画出来，然后花很久怀疑是数据的问题。
pub const CLOUD_MAGIC: u32 = 0x4350_594C; // 'LYPC' 小端

pub fn encode_cloud(view: &core_ffi::CloudView) -> Vec<u8> {
    let n = view.point_count() as usize;
    let has_intensity = view.has_intensity();
    let mut out = Vec::with_capacity(16 + 24 + n * 12 + if has_intensity { n * 4 } else { 0 });

    out.extend_from_slice(&CLOUD_MAGIC.to_le_bytes());
    out.extend_from_slice(&view.point_count().to_le_bytes());
    out.extend_from_slice(&view.total_points().to_le_bytes());
    out.extend_from_slice(&view.flags().to_le_bytes());
    for b in view.bounds() {
        out.extend_from_slice(&b.to_le_bytes());
    }
    for v in view.xyz() {
        out.extend_from_slice(&v.to_le_bytes());
    }
    if has_intensity {
        for v in view.intensity() {
            out.extend_from_slice(&v.to_le_bytes());
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::core_ffi::CLOUD_HAS_INTENSITY;
    use std::sync::Mutex;
    use std::time::{Duration, Instant};

    /// 测试侧的事件收集器。
    ///
    /// 这里不经 Tauri：`trampoline` 除了 `app.emit` 之外没有别的逻辑，
    /// 而起一个真实的 AppHandle 会把这些测试变成需要窗口系统的集成测试。
    /// 真正值得测的是**下面这一整条**：libloading 加载 DLL → C ABI 启动 run →
    /// 工作线程回调 → 事件 JSON → cancel/join/free 的生命周期。
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
        let core = crate::core_ffi::core().expect("加载 core 失败");
        let run_id = ulid::new();
        let ctx = Box::new(Collector {
            events: Mutex::new(Vec::new()),
        });
        let ptr = &*ctx as *const Collector;
        let handle = unsafe {
            RunHandle::start(
                Arc::clone(&core),
                &doc.to_string(),
                &run_id,
                base_dir,
                &[],
                collect,
                ctx,
            )
        }
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

    fn two_node_graph() -> serde_json::Value {
        serde_json::json!({
            "schemaVersion": 1, "id": "t",
            "nodes": [
                {"id": "g", "op": "gen.synthetic", "params": {"pointCount": 20000}},
                {"id": "v", "op": "filter.voxel_grid",
                 "params": {"leafSize": [0.02, 0.02, 0.02]}}
            ],
            "edges": [{"id": "e", "from": {"node": "g", "port": "cloud"},
                                  "to": {"node": "v", "port": "cloud"}}]
        })
    }

    #[test]
    fn two_node_run_emits_events_in_order_with_dense_seq() {
        let f = run(two_node_graph(), "");

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

    #[test]
    fn output_cloud_binary_header_is_correct() {
        let f = run(two_node_graph(), "");
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

    #[test]
    fn run_manager_reports_no_active_run_before_anything_starts() {
        let manager = RunManager::new();
        assert!(manager.active_run_id().is_none());
        // 取消一个不存在的 run 不该 panic —— 用户按 Esc 时那个 run 可能刚好跑完了
        manager.cancel("no-such-run");
    }
}
