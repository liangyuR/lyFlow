//! execution.rs 的单元测试（从 execution.rs 里搬出来：原文件六成是测试）。

use super::*;
use crate::core_ffi::CLOUD_HAS_INTENSITY;
use std::sync::Mutex;
use std::time::{Duration, Instant};

// ------------------------------------------------------------ 抢占的状态机（ADR-0027）

/// 假 run 开跑之后的样子。
#[derive(Clone, Copy, PartialEq)]
enum Behaves {
    /// 取消即退出（执行器在节点之间看取消标志）
    Cooperative,
    /// 不理取消，`finish()` 之前一直不退出：停在一次 OpenCV 调用里的算子
    Stuck,
    /// 根本起不来（core 返回空句柄）
    FailsToStart,
}

struct FakeRun {
    id: String,
    behaves: Behaves,
    cancelled: std::sync::atomic::AtomicBool,
    done: Mutex<bool>,
    exited: std::sync::Condvar,
}

impl FakeRun {
    fn finish(&self) {
        *self.done.lock().unwrap() = true;
        self.exited.notify_all();
    }
    fn was_cancelled(&self) -> bool {
        self.cancelled.load(std::sync::atomic::Ordering::SeqCst)
    }
}

impl LiveRun for FakeRun {
    fn run_id(&self) -> &str {
        &self.id
    }
    fn cancel(&self) {
        self.cancelled.store(true, std::sync::atomic::Ordering::SeqCst);
        if self.behaves == Behaves::Cooperative {
            self.finish();
        }
    }
    fn join(&self) {
        let mut done = self.done.lock().unwrap();
        while !*done {
            done = self.exited.wait(done).unwrap();
        }
    }
}

/// 记下状态机对每个请求做了什么：开跑了哪些、给哪些补发了什么。
#[derive(Default)]
struct Script {
    launched: Mutex<Vec<String>>,
    notified: Mutex<Vec<(String, &'static str, Option<String>)>>,
    runs: Mutex<std::collections::HashMap<String, Arc<FakeRun>>>,
}

impl Script {
    /// 提交一个请求，与 `RunManager::start` 走同一个 `submit`：能开跑就当场开，否则排队。
    fn submit(self: &Arc<Self>, manager: &RunManager, id: &str, behaves: Behaves) {
        let launch = {
            let script = Arc::clone(self);
            let id = id.to_string();
            move || -> Result<Run, String> {
                if behaves == Behaves::FailsToStart {
                    return Err("线程都没起来".to_string());
                }
                let run = Arc::new(FakeRun {
                    id: id.clone(),
                    behaves,
                    cancelled: Default::default(),
                    done: Mutex::new(false),
                    exited: Default::default(),
                });
                script.launched.lock().unwrap().push(id.clone());
                script.runs.lock().unwrap().insert(id, Arc::clone(&run));
                Ok(run)
            }
        };
        let notify = {
            let script = Arc::clone(self);
            let id = id.to_string();
            move |status: &'static str, error: Option<String>| {
                script.notified.lock().unwrap().push((id, status, error));
            }
        };
        manager
            .submit(launch.clone(), || Pending::new(id, launch, notify))
            .expect("submit 不该失败");
    }
    fn run(&self, id: &str) -> Arc<FakeRun> {
        Arc::clone(&self.runs.lock().unwrap()[id])
    }
    fn launched(&self) -> Vec<String> {
        self.launched.lock().unwrap().clone()
    }
    fn notified(&self) -> Vec<(String, &'static str, Option<String>)> {
        self.notified.lock().unwrap().clone()
    }
}

fn finished_id(manager: &RunManager) -> Option<String> {
    lock(&manager.inner).finished.as_ref().map(|r| r.run_id().to_string())
}

/// 收尾线程是异步的：轮询到条件成立，2 s 还不成立就失败。
fn eventually(what: &str, cond: impl Fn() -> bool) {
    let deadline = Instant::now() + Duration::from_secs(2);
    while !cond() {
        assert!(Instant::now() < deadline, "等了 2 s 还没有：{what}");
        std::thread::sleep(Duration::from_millis(1));
    }
}

/// 修前：`start` 里同步 join 被抢占的 run，而 run_graph 在 Tauri 主线程上 —— 真 app 里抢占一次
/// 正在跑霍夫找圆的运行，窗口卡了 7.5 s、被 Windows 判「未响应」（docs/large-image-plan.md §0）。
#[test]
fn preempting_a_stuck_run_returns_at_once_and_only_the_latest_request_runs_after_it_drains() {
    let manager = RunManager::new();
    let s = Arc::new(Script::default());
    s.submit(&manager, "a", Behaves::Stuck);

    let started = Instant::now();
    s.submit(&manager, "b", Behaves::Cooperative); // 抢占 a：a 收到取消，但停不下来
    s.submit(&manager, "c", Behaves::Cooperative); // b 还没开跑就被顶掉
    s.submit(&manager, "d", Behaves::Cooperative); // c 同理
    assert!(
        started.elapsed() < Duration::from_millis(200),
        "抢占一个停不下来的 run 不该等它：{:?}",
        started.elapsed()
    );
    assert!(s.run("a").was_cancelled(), "被抢占的要收到取消");
    assert_eq!(s.launched(), ["a"], "a 退出之前一个都不开跑：同一时刻最多一个 run 在算");

    s.run("a").finish();
    eventually("a 退出后开跑最后一个请求", || s.launched() == ["a", "d"]);
    assert!(s.notified().is_empty(), "被顶掉的请求不补发事件：{:?}", s.notified());
    assert_eq!(manager.active_run_id().as_deref(), Some("d"));

    // d 正常跑完进 finished；再来一个不用排队，当场开跑
    s.run("d").finish();
    eventually("d 跑完", || manager.active_run_id().is_none());
    s.submit(&manager, "e", Behaves::Cooperative);
    assert_eq!(s.launched(), ["a", "d", "e"]);
}

/// 被抢占的 run 收到取消就退出（绝大多数情况）：排队的那一个紧接着开跑。
#[test]
fn preempting_a_cooperative_run_starts_the_new_one_as_soon_as_it_exits() {
    let manager = RunManager::new();
    let s = Arc::new(Script::default());
    s.submit(&manager, "a", Behaves::Cooperative);
    s.submit(&manager, "b", Behaves::Cooperative);
    eventually("a 一取消就退出，b 随即开跑", || s.launched() == ["a", "b"]);
    assert!(s.run("a").was_cancelled());
    assert!(s.notified().is_empty());
}

#[test]
fn cancelling_a_queued_request_drops_it_and_reports_it_cancelled() {
    let manager = RunManager::new();
    let s = Arc::new(Script::default());
    s.submit(&manager, "a", Behaves::Stuck);
    s.submit(&manager, "b", Behaves::Cooperative);
    manager.cancel("b");
    assert_eq!(s.notified(), [("b".to_string(), "cancelled", None)]);

    s.run("a").finish();
    // a 被抢占后在 draining 里，不在 active 里：要等的是全部收场（review 修正：修前等的条件一开始就成立）
    eventually("a 排干", || manager.is_idle());
    assert_eq!(s.launched(), ["a"], "取消掉的排队请求不该再开跑");
    // 没有人顶替它：界面上留着的就是 a 的结果，a 进 finished 而不是被放掉（review 第二轮）
    assert_eq!(finished_id(&manager).as_deref(), Some("a"));

    // 维护窗口里排着队的那个同样能取消：补发 cancelled，窗口结束时什么都不开跑
    let paused = manager.pause(PauseMode::KeepResults);
    s.submit(&manager, "c", Behaves::Cooperative);
    manager.cancel("c");
    drop(paused);
    assert_eq!(s.notified().last(), Some(&("c".to_string(), "cancelled", None)));
    assert_eq!(s.launched(), ["a"]);
    assert!(manager.is_idle());
}

/// 重扫库目录的维护窗口（ADR-0027）：在算的停下、等它退出；窗口里来的请求只排队、不开跑。
/// 修前重扫之前 stop_active 一下，两步之间主线程上来的 run_graph 当场开跑，手里的算子描述随即被重建注册表释放。
/// 窗口结束时只开跑最新的那一个；被停掉的那一次留作上一次完成的（界面上留着的是它的结果）。
#[test]
fn a_pause_queues_requests_until_it_ends_and_then_runs_only_the_latest() {
    let manager = RunManager::new();
    let s = Arc::new(Script::default());
    s.submit(&manager, "a", Behaves::Stuck);

    let mut paused = manager.pause(PauseMode::KeepResults);
    assert!(s.run("a").was_cancelled(), "进窗口时在算的那个被取消");
    s.submit(&manager, "b", Behaves::Cooperative);
    s.submit(&manager, "c", Behaves::Cooperative);
    assert_eq!(s.launched(), ["a"], "窗口里来的请求一个都不开跑");

    let waiter = std::thread::spawn(move || {
        paused.drain();
        paused
    });
    std::thread::sleep(Duration::from_millis(30));
    assert!(!waiter.is_finished(), "drain 要等停不下来的那个真的退出（它还握着算子描述）");
    s.run("a").finish();
    let paused = waiter.join().unwrap();
    assert_eq!(s.launched(), ["a"], "等完了也还在窗口里：重扫这一步还没做");

    drop(paused);
    assert_eq!(s.launched(), ["a", "c"], "窗口结束，最新的那个开跑");
    assert!(s.notified().is_empty(), "被顶掉的 b 不补发（与抢占同一条规矩）");
    assert_eq!(finished_id(&manager).as_deref(), Some("a"), "被停掉的那一次留作上一次完成的");
}

/// 热重载的窗口：上一次完成的也放掉（只要还有一个 RunHandle 活着，旧 DLL 就卸载不掉），
/// 排队的请求不作废，换完代才开跑（开跑时取的是新的一代，见 `start` 里的 Pending）。
#[test]
fn a_hot_reload_pause_releases_every_run_and_starts_the_queued_one_afterwards() {
    let manager = RunManager::new();
    let s = Arc::new(Script::default());
    s.submit(&manager, "done", Behaves::Cooperative);
    s.run("done").finish();
    eventually("done 进 finished", || finished_id(&manager).as_deref() == Some("done"));
    s.submit(&manager, "a", Behaves::Stuck);
    s.submit(&manager, "b", Behaves::Cooperative);

    let mut paused = manager.pause(PauseMode::ReleaseAll);
    s.run("a").finish();
    paused.drain();
    // 除了测试自己记的那一份（和这里临时借的一份），没有人再握着它们。收尾线程 join 返回之后
    // 才放掉它那一份，所以等一下：真 DLL 的最后一个引用同样是在那里落下的
    for id in ["done", "a"] {
        eventually(&format!("{id} 被放掉"), || Arc::strong_count(&s.run(id)) == 2);
    }
    assert_eq!(s.launched(), ["done", "a"], "排队的 b 不作废，也还没开跑");
    drop(paused);
    assert_eq!(s.launched(), ["done", "a", "b"]);
    assert!(s.notified().is_empty());
    assert!(finished_id(&manager).is_none(), "热重载不留被停掉的那个");
}

/// 重扫（线程池上）与热重载（watcher 线程上）可能同时来：同一时刻只有一个窗口，后来的等前一个结束。
#[test]
fn a_second_pause_waits_for_the_first_one_to_end() {
    let manager = RunManager::new();
    let first = manager.pause(PauseMode::KeepResults);
    let second = {
        let manager = manager.clone();
        std::thread::spawn(move || drop(manager.pause(PauseMode::ReleaseAll)))
    };
    std::thread::sleep(Duration::from_millis(30));
    assert!(!second.is_finished(), "第一个窗口还没结束");
    drop(first);
    second.join().unwrap();
    assert!(!lock(&manager.inner).paused);
}

#[test]
fn a_queued_request_that_fails_to_start_is_reported_as_an_error() {
    let manager = RunManager::new();
    let s = Arc::new(Script::default());
    s.submit(&manager, "a", Behaves::Stuck);
    s.submit(&manager, "b", Behaves::FailsToStart);
    s.run("a").finish();
    eventually("补发 error", || !s.notified().is_empty());
    assert_eq!(
        s.notified(),
        [("b".to_string(), "error", Some("线程都没起来".to_string()))]
    );
    assert!(manager.active_run_id().is_none());

    // 维护窗口结束时才开跑的那个起不来：同样补发 error
    let paused = manager.pause(PauseMode::KeepResults);
    s.submit(&manager, "c", Behaves::FailsToStart);
    drop(paused);
    assert_eq!(
        s.notified().last(),
        Some(&("c".to_string(), "error", Some("线程都没起来".to_string())))
    );
    assert!(manager.is_idle());
}

/// 补发的那条要能被前端当成一条普通的 run_finished：schema 要的字段齐、status 是三种之一。
#[test]
fn the_run_finished_for_a_run_that_never_started_has_the_fields_the_schema_requires() {
    let cancelled = never_started_event("r1", "cancelled", None);
    for key in ["schemaVersion", "runId", "seq", "kind", "status"] {
        assert!(!cancelled[key].is_null(), "缺 {key}: {cancelled}");
    }
    assert_eq!(cancelled["kind"], "run_finished");
    assert_eq!(cancelled["seq"], 0);
    assert!(cancelled.get("error").is_none());

    let failed = never_started_event("r2", "error", Some("线程都没起来"));
    assert_eq!(failed["error"]["phase"], "execute");
    assert_eq!(failed["error"]["message"], "线程都没起来");
}

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
            .rfind(|e| e["kind"] == "node_state" && e["nodeId"] == node)
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
    run_spec(doc, base_dir, &[], &[], 0, during)
}

/// isolate / mode 是单节点运行那几条用例要的（docs/node-run-plan.md）。
fn run_spec(
    doc: serde_json::Value,
    base_dir: &str,
    isolate: &[String],
    force: &[String],
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
    spec.force = force;
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

/// 事件流：seq 稠密、每条带 runId，节点状态按序推进。
/// ADR-0022：同一份 summary 有两个出口 —— run_finished 事件里那一份，
/// 和按 runId 从 C ABI 取回来的那一份。它们必须逐字段相等。
#[test]
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
fn run_summary_comes_back_over_the_abi_and_in_the_event() {
    let f = run(two_node_graph(104), "");
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
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
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
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
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

/// v12：rgb 进了视图与载荷。放在最后、补齐到 4 字节 —— 老解码器不看第 3 位也不看尾部。
#[test]
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
fn cloud_payload_carries_rgb_last_and_padded() {
    let dir = std::env::temp_dir().join(format!("lyflow-rgb-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let pcd = dir.join("rgb.pcd");
    // 三个点：红、绿、蓝（打包成 0x00RRGGBB 的 uint32）
    std::fs::write(
        &pcd,
        "# .PCD v0.7
VERSION 0.7
FIELDS x y z rgb
SIZE 4 4 4 4
TYPE F F F U
COUNT 1 1 1 1
             WIDTH 3
HEIGHT 1
VIEWPOINT 0 0 0 1 0 0 0
POINTS 3
DATA ascii
             0 0 0 16711680
1 0 0 65280
2 0 0 255
",
    )
    .unwrap();
    let doc = serde_json::json!({
        "schemaVersion": 1, "id": "t",
        "nodes": [{"id": "r", "op": "io.load_pcd", "params": {"path": pcd.to_string_lossy()}}],
        "edges": []
    });
    let f = run(doc, "");
    assert_eq!(f.run_status(), "ok", "{:#?}", f.events);

    let view = f.core.output_cloud(&f.run_id, "r", "cloud", 0).unwrap();
    assert!(view.has_rgb(), "PCD 里的 rgb 应当一路带到视图");
    assert_eq!(view.rgb(), &[255, 0, 0, 0, 255, 0, 0, 0, 255]);

    let bytes = encode_cloud(&view);
    let flags = u32_at(&bytes, 12);
    assert_eq!(flags & crate::core_ffi::CLOUD_HAS_RGB, crate::core_ffi::CLOUD_HAS_RGB);
    // 头 40 + xyz 36 + rgb 9 补齐到 12；这片云没有强度与法线
    assert_eq!(bytes.len(), 40 + 36 + 12, "rgb 块没放在最后或没补齐");
    assert_eq!(&bytes[76..85], view.rgb());
    let _ = std::fs::remove_dir_all(&dir);
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
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
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
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
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

    // 端口类型不对、端口不存在、run 不存在：都是错误，不是空切片
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
fn encode_image_frame_matches_the_documented_layout() {
    let core = crate::core_ffi::core().expect("加载 core 失败");
    // 3x2 RGB u8 的第 1 行（9 字节）：载荷补齐到 12
    let row: Vec<u8> = (0..9).collect();
    let view = unsafe {
        core_ffi::ImageView::borrowed(Arc::clone(&core), 3, 2, 3, 1, 1, (6, 4), 1, &row)
    };
    assert_eq!(view.row_count(), 1);
    let bytes = encode_image(&view);
    let head: Vec<u32> = (0..12).map(|i| u32_at(&bytes, i * 4)).collect();
    assert_eq!(head, vec![IMAGE_MAGIC, 3, 2, 3, 1, 1, 6, 4, 1, 1, 9, 0], "帧头逐项");
    assert_eq!(&bytes[48..57], &row[..]);
    assert_eq!(bytes.len(), 48 + 12, "像素补齐到 4 字节");
    assert_eq!(IMAGE_HEADER_BYTES % 4, 0, "像素必须 4 字节对齐，前端才能零拷贝开 Float32Array");

    let empty = unsafe { core_ffi::ImageView::borrowed(core, 3, 2, 3, 1, 0, (3, 2), 9, &[]) };
    assert_eq!(encode_image(&empty).len(), 48, "空切片只有帧头");

    // 注入图像：像素比 宽×高×通道×位深 短就不交指针（修前 core 会越界读，review 修正 PR #1）
    let input = |len: usize| core_ffi::RunImageInput {
        node_id: "n".into(),
        port: "image".into(),
        width: 3,
        height: 2,
        channels: 3,
        depth: 2,
        pixels: vec![0; len],
    };
    assert!(input(35).pixels_ptr().is_null(), "差一个字节");
    assert!(!input(36).pixels_ptr().is_null(), "刚好够");
    assert!(!input(40).pixels_ptr().is_null(), "多给的尾巴不读");
    assert!(input(0).pixels_ptr().is_null());
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

    // 取消一个不存在的 run 不该 panic —— 用户按 Esc 时那个 run 可能刚好跑完了
    RunManager::new().cancel("no-such-run");
}

#[test]
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
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
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
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

    // 修订一 V1：isolate 不再隐含强制重算，要 v 真跑一遍得另给 force
    let isolate = vec!["v".to_string()];
    let f = run_spec(doc, "", &isolate, &isolate, 0, |_| {});
    assert_eq!(f.run_status(), "ok", "{:#?}", f.events);
    let started = f.kind("run_started")[0].clone();
    assert_eq!(started["isolate"], serde_json::json!(["v"]));
    assert_eq!(started["targets"], serde_json::json!(["v"]), "给了 isolate，targets 取同一组 id");
    assert_eq!(started["mode"], "full");
    assert_eq!(started["force"], serde_json::json!(["v"]));

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

    // R7：下游 p 不执行，但它上一次的结果挂进了这次运行，按新 runId 取得到
    let finished = f.kind("run_finished")[0].clone();
    assert_eq!(finished["attached"], serde_json::json!(["p"]), "{finished}");
    let view = f
        .core
        .output_cloud(&f.run_id, "p", "cloud", 0)
        .expect("挂上的下游按新 runId 应当取得到点云");
    assert!(view.total_points() > 0);
}

/// isolate 在开跑前就被拒的两种情形，都是一个节点都不动。纯平台构建里 ABI 的 isolate
/// 字段只有这一条在走，所以它不能挂 std_packs_off。
/// R2：上游没有当前 cacheKey 的结果 —— 整次失败，诊断指向缺结果的上游。
/// R5：预览与 isolate 不组合，core 报参数错误。
#[test]
fn isolate_is_refused_before_running_anything() {
    let isolate = vec!["v".to_string()];
    let f = run_spec(two_node_graph(7302), "", &isolate, &[], 0, |_| {});
    assert_eq!(f.run_status(), "error");
    let finished = f.kind("run_finished")[0].clone();
    assert_eq!(finished["error"]["code"], "upstream_not_ready", "{finished}");
    let diags = finished["diagnostics"].as_array().expect("run_finished 应带 diagnostics");
    assert_eq!(diags.len(), 1, "{finished}");
    assert_eq!(diags[0]["nodeId"], "g");
    assert_eq!(diags[0]["code"], "upstream_not_ready");
    assert!(f.kind("node_state").is_empty(), "一个节点都不该动: {:#?}", f.events);

    let preview = run_spec(two_node_graph(7303), "", &isolate, &[], 1, |_| {});
    assert_eq!(preview.run_status(), "error");
    assert_eq!(preview.kind("run_finished")[0]["error"]["code"], "bad_input");
    assert!(preview.kind("node_state").is_empty(), "{:#?}", preview.events);
}

/// 修订一 V2：「运行到此」（只给 targets）也把计划外、仍有当前结果的下游挂进来。
#[test]
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
fn run_to_node_attaches_unplanned_downstream() {
    let doc = serde_json::json!({
        "schemaVersion": 1, "id": "t",
        "nodes": [
            {"id": "g", "op": "gen.synthetic", "params": {"pointCount": 3000, "seed": 7304}},
            {"id": "v", "op": "filter.voxel_grid", "params": {"leafSize": [0.02, 0.02, 0.02]}},
            {"id": "p", "op": "filter.passthrough"}
        ],
        "edges": [
            {"id": "e1", "from": {"node": "g", "port": "cloud"}, "to": {"node": "v", "port": "cloud"}},
            {"id": "e2", "from": {"node": "v", "port": "cloud"}, "to": {"node": "p", "port": "cloud"}}
        ]
    });
    let full = run(doc.clone(), "");
    assert_eq!(full.run_status(), "ok");
    assert!(full.kind("run_finished")[0].get("attached").is_none(), "全图运行不带 attached");

    let core = crate::core_ffi::core().expect("加载 core 失败");
    let run_id = ulid::new();
    let ctx = Box::new(Collector { events: Mutex::new(Vec::new()) });
    let ptr = &*ctx as *const Collector;
    let graph = doc.to_string();
    let targets = vec!["v".to_string()];
    let spec = RunSpec::new(&graph, &run_id, "", &targets);
    let handle = unsafe { RunHandle::start(Arc::clone(&core), spec, collect, ctx) }.unwrap();
    handle.join();
    let events = unsafe { (*ptr).events.lock().unwrap().clone() };
    let finished = events.iter().find(|e| e["kind"] == "run_finished").unwrap();
    assert_eq!(finished["status"], "ok");
    assert_eq!(finished["attached"], serde_json::json!(["p"]), "{finished}");
    let view = core.output_cloud(&run_id, "p", "cloud", 0).expect("挂上的 p 按新 runId 取得到");
    assert!(view.total_points() > 0);
    drop(handle);
}

/// param-recipe P1.5：运行走 C ABI 的 params_json（编辑器合成的「default + 配方覆盖」）。
/// 与 default 不同的值真的进了 compute；越过图参数自己硬限位的值整次失败、一个节点都不跑。
#[test]
fn graph_param_values_reach_the_core_through_params_json() {
    let doc = serde_json::json!({
        "schemaVersion": 1, "id": "t",
        "nodes": [{"id": "g", "op": "gen.synthetic", "params": {"seed": 7305}}],
        "edges": [],
        "params": {"count": {"type": "int", "min": 10, "max": 5000, "default": 3000,
                             "binds": ["g.pointCount"]}}
    });
    let run = |params: &str| {
        let core = crate::core_ffi::core().expect("加载 core 失败");
        let run_id = ulid::new();
        let ctx = Box::new(Collector {
            events: Mutex::new(Vec::new()),
        });
        let ptr = &*ctx as *const Collector;
        let graph = doc.to_string();
        let mut spec = RunSpec::new(&graph, &run_id, "", &[]);
        spec.params_json = Some(params);
        let handle = unsafe { RunHandle::start(Arc::clone(&core), spec, collect, ctx) }
            .expect("启动运行失败");
        handle.join();
        let events = unsafe { (*ptr).events.lock().unwrap().clone() };
        Fixture {
            events,
            run_id,
            _handle: handle,
            core,
        }
    };
    let ok = run(r#"{"count": 1234}"#);
    assert_eq!(ok.run_status(), "ok");
    let done = ok
        .kind("node_state")
        .into_iter()
        .find(|e| e["nodeId"] == "g" && e["state"] == "done")
        .expect("g 没有 done");
    assert_eq!(done["stats"]["outputs"][0]["elementCount"], 1234);

    let bad = run(r#"{"count": 9}"#);
    assert_eq!(bad.run_status(), "error");
    assert_eq!(bad.kind("run_finished")[0]["error"]["code"], "bad_param");
    assert!(
        !bad.kind("node_state").iter().any(|e| e["state"] == "running"),
        "越界的图参数值不该让任何节点开跑"
    );
}

/// 重扫库目录不能放掉上一次跑完的 run（param-recipe-p2-acceptance「P2 之外发现的问题」）。
/// 存库之后 400 ms，库目录 watcher 会再扫一遍；那时刚跑完的 run 已经进了 finished 槽，
/// 以前这里 drop_all → lyflow_run_free → 索引没了、数据还在：界面显示「完成」，按这个 runId
/// 取点云却是「core 没有该结果」，下一次运行又全部命中缓存。
#[test]
#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]
fn library_rescan_keeps_the_finished_run_readable() {
    let core = crate::core_ffi::core().expect("加载 core 失败");
    let manager = RunManager::new();
    let run_once = || {
        let run_id = ulid::new();
        let ctx = Box::new(Collector {
            events: Mutex::new(Vec::new()),
        });
        let ptr = &*ctx as *const Collector;
        let graph = two_node_graph(7401).to_string();
        let spec = RunSpec::new(&graph, &run_id, "", &[]);
        let handle =
            unsafe { RunHandle::start(Arc::clone(&core), spec, collect, ctx) }.unwrap();
        handle.join();
        let events = unsafe { (*ptr).events.lock().unwrap().clone() };
        (run_id, handle, events)
    };

    let (first, handle, _) = run_once();
    manager.adopt_finished(handle);
    assert!(core.output_cloud(&first, "v", "cloud", 0).is_ok());

    // 空目录：注册表不增不减。往进程级注册表里真加一个库算子会让并行跑着的
    // 别的测试手里的 OperatorDesc* 失效 —— 要验的是 RunManager 这一侧，与库里有什么无关
    let dir = std::env::temp_dir().join(format!("lyflow-lib-rescan-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    // 与 rescan_library_paused 同一个窗口；这里没有 Tauri 主线程，就地扫
    let mut paused = manager.pause(PauseMode::KeepResults);
    paused.drain();
    crate::commands::rescan_library_dirs(vec![dir.to_string_lossy().into_owned()])
        .expect("重扫失败");
    drop(paused);
    let _ = std::fs::remove_dir_all(&dir);

    let view = core
        .output_cloud(&first, "v", "cloud", 0)
        .expect("重扫之后，上一次跑完的 run 按它的 runId 仍应取得到点云");
    assert!(view.total_points() > 0);

    // 下一次运行命中缓存，按新 runId 同样取得到
    let (second, handle, events) = run_once();
    let skipped = events
        .iter()
        .filter(|e| e["kind"] == "node_state" && e["state"] == "skipped")
        .count();
    assert_eq!(skipped, 2, "两个节点都应命中缓存: {events:#?}");
    assert!(core.output_cloud(&second, "v", "cloud", 0).is_ok());
    drop(handle);
}
