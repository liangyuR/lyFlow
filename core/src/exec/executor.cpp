#include "exec/executor.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <map>
#include <queue>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "exec/graph.h"
#include "exec/hash.h"
#include "exec/plan.h"
#include "exec/result_store.h"
#include "exec/subgraph.h"
#include "lyflow/contract.h"
#include "lyflow/json_writer.h"
#include "lyflow/operator.h"
#include "lyflow/registry.h"

namespace lyflow::exec {
namespace {

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

std::string isoNow() {
  const auto now = std::chrono::system_clock::now();
  const auto t = std::chrono::system_clock::to_time_t(now);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
  char frac[8];
  std::snprintf(frac, sizeof(frac), ".%03dZ", static_cast<int>(ms.count()));
  return std::string(buf) + frac;
}

/// 参数的生效值。与 canonicalParamsJson 同一套形态，只是这里带缩进控制权在外面。
void writeParamValue(JsonWriter& w, const Value& v) {
  switch (v.kind()) {
    case Value::Kind::Null:   w.valueNull(); break;
    case Value::Kind::Bool:   w.value(v.boolValue()); break;
    case Value::Kind::Int:    w.value(v.intValue()); break;
    case Value::Kind::Float:  w.value(v.floatValue()); break;
    case Value::Kind::String: w.value(v.stringValue()); break;
    case Value::Kind::FloatVec:
      w.beginArray();
      for (double d : v.vecValue()) w.value(d);
      w.endArray();
      break;
  }
}

void writeError(JsonWriter& w, const Status& s) {
  w.beginObject();
  w.field("phase", std::string(toString(s.phase)));
  w.field("code", s.code);
  w.field("message", s.message);
  w.fieldIfSet("paramPath", s.paramPath);
  w.fieldIfSet("portName", s.portName);
  w.endObject();
}

/// 一个节点的收尾状态（ADR-0022）。EventSink 在发终态事件时顺手记一份 ——
/// 那三处（nodeFinished / nodeFailed / nodeNotDemanded）正是「这个节点最终
/// 怎么样了」唯一被决定的地方，从事件流反推只会漏。
struct NodeOutcome {
  std::string state;   ///< done / skipped / error / cancelled
  std::string code;    ///< 失败时 errors[0].code
  std::string reason;  ///< skipped 的机器可读原因，目前只有 not_demanded
  double durationMs = -1;
  bool cached = false;
  bool bypassed = false;
  bool provided = false;
  bool outputsAvailable = false;
};

/// 事件序列化 + 回调。seq 全局单调，多个 worker 同时发事件所以整个入口加锁（E2）。
class EventSink {
 public:
  EventSink(std::string runId, lyflow_event_cb cb, void* user)
      : runId_(std::move(runId)), cb_(cb), user_(user) {}

  /// deferred 节点不进 run_started：它们要么被 demand 时经 plan_extended 追加，
  /// 要么在 run_finished 前以 skipped/not_demanded 收场（ADR-0016）。
  void runStarted(const Plan& plan, const std::vector<std::string>& targets, int maxParallel,
                  bool preview, std::uint32_t previewMaxPoints) {
    JsonWriter w;
    std::lock_guard<std::mutex> lock(mu_);
    std::size_t eager = 0;
    for (const auto& n : plan.nodes) {
      if (!n.deferred) eager += 1;
    }
    begin(w, "run_started");
    w.field("nodeCount", static_cast<std::int64_t>(eager));
    w.field("maxParallel", static_cast<std::int64_t>(maxParallel));
    w.field("mode", std::string(preview ? "preview" : "full"));
    if (preview) w.field("previewMaxPoints", static_cast<std::int64_t>(previewMaxPoints));
    w.key("plan");
    w.beginArray();
    for (const auto& n : plan.nodes) {
      if (!n.deferred) w.value(n.id);
    }
    w.endArray();
    w.key("targets");
    w.beginArray();
    for (const auto& t : targets) w.value(t);
    w.endArray();
    // 前端的精确 stale 标记与「将重算 N 个节点」全靠这一段（ADR-0007）。
    w.key("nodes");
    w.beginArray();
    for (const auto& n : plan.nodes) {
      if (n.deferred) continue;
      writePlanNode(w, n);
    }
    w.endArray();
    if (!plan.outputs.empty()) {
      w.key("outputs");
      w.beginArray();
      for (const auto& o : plan.outputs) {
        w.beginObject();
        w.field("name", o.name);
        w.field("node", o.nodeId);
        w.field("port", o.port);
        w.endObject();
      }
      w.endArray();
    }
    end(w);
  }

  /// 惰性闭包被 demand 时追加进计划。nodes[] 与 run_started.nodes 同构。
  void planExtended(const Plan& plan, const std::vector<std::size_t>& added,
                    const std::string& demandedBy, const std::string& port) {
    JsonWriter w;
    std::lock_guard<std::mutex> lock(mu_);
    begin(w, "plan_extended");
    w.field("demandedBy", demandedBy);
    w.field("port", port);
    w.key("nodes");
    w.beginArray();
    for (std::size_t i : added) writePlanNode(w, plan.nodes[i]);
    w.endArray();
    end(w);
  }

  /// 惰性闭包没被 demand。前端把这些节点画成半透明（A2-5）。
  void nodeNotDemanded(const std::string& nodeId) {
    JsonWriter w;
    std::lock_guard<std::mutex> lock(mu_);
    NodeOutcome& outcome = outcomes_[nodeId];
    outcome = NodeOutcome{};
    outcome.state = "skipped";
    outcome.reason = "not_demanded";
    begin(w, "node_state");
    w.field("nodeId", nodeId);
    w.field("state", std::string("skipped"));
    w.key("stats");
    w.beginObject();
    w.field("elementCount", static_cast<std::int64_t>(0));
    w.field("byteSize", static_cast<std::int64_t>(0));
    w.field("outputsAvailable", false);
    w.field("reason", std::string("not_demanded"));
    w.endObject();
    end(w);
  }

  void nodeState(const std::string& nodeId, const char* state) {
    JsonWriter w;
    std::lock_guard<std::mutex> lock(mu_);
    begin(w, "node_state");
    w.field("nodeId", nodeId);
    w.field("state", std::string(state));
    end(w);
  }

  /// done / skipped 共用。cached / bypassed / provided 让用户分得清
  /// 「不用跑」「被静音」和「由宿主注入」。
  void nodeFinished(const std::string& nodeId, const char* state, double durationMs,
                    std::size_t elementCount, std::size_t byteSize,
                    const std::vector<OutputInfo>& outputs, bool cached, bool bypassed,
                    bool provided = false) {
    JsonWriter w;
    std::lock_guard<std::mutex> lock(mu_);
    NodeOutcome& outcome = outcomes_[nodeId];
    outcome = NodeOutcome{};
    outcome.state = state;
    outcome.durationMs = durationMs;
    outcome.cached = cached;
    outcome.bypassed = bypassed;
    outcome.provided = provided;
    outcome.outputsAvailable = true;
    begin(w, "node_state");
    w.field("nodeId", nodeId);
    w.field("state", std::string(state));
    w.field("durationMs", durationMs);
    w.key("stats");
    w.beginObject();
    w.field("elementCount", static_cast<std::int64_t>(elementCount));
    w.field("byteSize", static_cast<std::int64_t>(byteSize));
    if (cached) w.field("cached", true);
    if (bypassed) w.field("bypassed", true);
    if (provided) w.field("provided", true);
    w.field("outputsAvailable", true);
    w.key("outputs");
    w.beginArray();
    for (const auto& o : outputs) {
      w.beginObject();
      w.field("port", o.port);
      w.field("type", o.type);
      w.field("elementCount", static_cast<std::int64_t>(o.elementCount));
      // 非点云输出把值一并带上：A/B 脚本与 Inspector 靠它，不必再回头查结果仓。
      if (!o.valueJson.empty()) {
        w.key("value");
        w.raw(o.valueJson);
      }
      w.endObject();
    }
    w.endArray();
    w.endObject();
    end(w);
  }

  /// 失败/取消。errors 是**全部**诊断，error 是 errors[0] 的快捷方式（D5）。
  void nodeFailed(const std::string& nodeId, const char* state,
                  const std::vector<Status>& errors, double durationMs) {
    JsonWriter w;
    std::lock_guard<std::mutex> lock(mu_);
    NodeOutcome& outcome = outcomes_[nodeId];
    outcome = NodeOutcome{};
    outcome.state = state;
    outcome.durationMs = durationMs;
    if (!errors.empty()) outcome.code = errors.front().code;
    begin(w, "node_state");
    w.field("nodeId", nodeId);
    w.field("state", std::string(state));
    if (durationMs >= 0) w.field("durationMs", durationMs);
    if (!errors.empty()) {
      w.key("error");
      writeError(w, errors.front());
      w.key("errors");
      w.beginArray();
      for (const auto& e : errors) writeError(w, e);
      w.endArray();
    }
    end(w);
  }

  void nodeProgress(const std::string& nodeId, float ratio, const std::string& message) {
    JsonWriter w;
    std::lock_guard<std::mutex> lock(mu_);
    begin(w, "node_progress");
    w.field("nodeId", nodeId);
    w.field("progress", static_cast<double>(ratio < 0 ? 0.0f : (ratio > 1 ? 1.0f : ratio)));
    w.fieldIfSet("message", message);
    end(w);
  }

  void log(const char* level, const std::string& nodeId, const std::string& message) {
    JsonWriter w;
    std::lock_guard<std::mutex> lock(mu_);
    begin(w, "log");
    w.field("level", std::string(level));
    w.fieldIfSet("nodeId", nodeId);
    w.field("message", message);
    end(w);
  }

  /// summaryJson 非空时原样嵌进 `summary` 字段（H1：事件里那份与
  /// `lyflow_run_summary` 拿到的是同一个对象）。
  void runFinished(const char* status, double durationMs, const Status* error,
                   const std::string& summaryJson = {}) {
    JsonWriter w;
    std::lock_guard<std::mutex> lock(mu_);
    begin(w, "run_finished");
    w.field("status", std::string(status));
    w.field("durationMs", durationMs);
    if (error) {
      w.key("error");
      writeError(w, *error);
    }
    if (!summaryJson.empty()) {
      w.key("summary");
      w.raw(summaryJson);
    }
    end(w);
  }

  /// 全部节点的收尾状态。scheduler.run() 返回之后（worker 都 join 了）才可以读。
  const std::map<std::string, NodeOutcome>& outcomes() const { return outcomes_; }

  /// 一条端口契约违反（ADR-0024）。与 outcomes_ 同一个锁 —— 多个 worker 可能同时撞上。
  void contractViolation(std::string node, std::string port, nlohmann::json expected,
                         nlohmann::json actual) {
    std::lock_guard<std::mutex> lock(mu_);
    violations_.push_back(
        ContractViolation{std::move(node), std::move(port), std::move(expected), std::move(actual)});
  }

  /// 本轮全部契约违反。顺序是撞上的先后，进 summary 之前会按拓扑序重排。
  const std::vector<ContractViolation>& contractViolations() const { return violations_; }

 private:
  static void writePlanNode(JsonWriter& w, const PlanNode& n) {
    w.beginObject();
    w.field("id", n.id);
    w.field("cacheKey", n.cacheKey);
    w.field("level", static_cast<std::int64_t>(n.level));
    if (n.bypass) w.field("bypass", true);
    w.endObject();
  }

  void begin(JsonWriter& w, const char* kind) {
    w.setIndent(0);
    w.beginObject();
    w.field("schemaVersion", 1);
    w.field("runId", runId_);
    w.field("seq", static_cast<std::int64_t>(seq_++));
    w.field("at", isoNow());
    w.field("kind", std::string(kind));
  }
  void end(JsonWriter& w) {
    w.endObject();
    if (!cb_) return;
    const std::string json = w.str();
    cb_(json.c_str(), user_);
  }

  std::mutex mu_;
  std::string runId_;
  lyflow_event_cb cb_ = nullptr;
  void* user_ = nullptr;
  std::int64_t seq_ = 0;
  /// 节点 id -> 收尾状态（ADR-0022）。map 而不是 unordered_map：summary 里
  /// 计划外的节点按 id 排序输出，两次跑同一张图的 JSON 才逐字节一样。
  std::map<std::string, NodeOutcome> outcomes_;
  /// 端口契约违反（ADR-0024）。节点会因此 error，但 summary 要的是「期望 vs 实际」
  /// 这一对，而 Status 只留得下一句话。
  std::vector<ContractViolation> violations_;
};

/// 算子看到的 ExecContext。每个节点一份，所以本身不用加锁。
class NodeContext final : public ExecContext {
 public:
  NodeContext(EventSink& sink, const std::atomic<bool>& cancelled, std::string nodeId,
              const std::filesystem::path& baseDir, int threadBudget)
      : sink_(sink), cancelled_(cancelled), nodeId_(std::move(nodeId)), baseDir_(baseDir),
        threadBudget_(threadBudget) {}

  bool cancelled() const override { return cancelled_.load(std::memory_order_relaxed); }

  void progress(float ratio, std::string_view message) override {
    // 限流：算子在百万点的循环里每千点报一次进度是常态，
    // 全发出去的话前端一次运行要处理几万条事件，比计算本身还贵。
    const auto now = Clock::now();
    if (lastProgress_ != Clock::time_point{} &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - lastProgress_).count() < 50) {
      return;
    }
    lastProgress_ = now;
    sink_.nodeProgress(nodeId_, ratio, std::string(message));
  }

  void log(LogLevel level, std::string message) override {
    sink_.log(toString(level), nodeId_, message);
  }

  const std::filesystem::path& baseDir() const override { return baseDir_; }
  int threadBudget() const override { return threadBudget_; }

 private:
  EventSink& sink_;
  const std::atomic<bool>& cancelled_;
  std::string nodeId_;
  const std::filesystem::path& baseDir_;
  int threadBudget_ = 1;
  Clock::time_point lastProgress_{};
};

const Port* findPortByName(const std::vector<Port>& ports, const std::string& name) {
  for (const Port& p : ports) {
    if (p.name == name) return &p;
  }
  return nullptr;
}

std::vector<std::string> outputPortNames(const OperatorDesc& op) {
  std::vector<std::string> names;
  names.reserve(op.outputs.size());
  for (const Port& p : op.outputs) names.push_back(p.name);
  return names;
}

/// 等步长抽稀（F5）。走 PointCloud::select，intensity/normals/rgb 一起搬。
PointCloud decimateCloud(const PointCloud& src, std::size_t maxPoints) {
  const std::size_t n = src.pointCount();
  std::vector<std::int32_t> keep;
  keep.reserve(maxPoints);
  const double step = static_cast<double>(n) / static_cast<double>(maxPoints);
  for (std::size_t i = 0; i < maxPoints; ++i) {
    const auto idx = static_cast<std::size_t>(static_cast<double>(i) * step);
    keep.push_back(static_cast<std::int32_t>(idx < n ? idx : n - 1));
  }
  return src.select(keep);
}

/// 预览结果的缓存命名空间（F5）。full 模式返回空串，键与 M3 完全一致。
std::string previewNamespace(const RunOptions& o) {
  if (o.mode != RunMode::Preview) return {};
  const std::uint32_t cap = o.previewMaxPoints ? o.previewMaxPoints : kDefaultPreviewMaxPoints;
  return "preview:" + std::to_string(cap);
}

// ------------------------------------------------------------------- 调度器

enum class Verdict { Ok, Failed, Cancelled };

/// 依赖计数驱动的线程池（E2）。没有层同步屏障 —— 屏障会让一层里最慢的节点
/// 拖住全部，而依赖计数天然就是最优调度。
class Scheduler {
 public:
  Scheduler(Plan& plan, EventSink& sink, const std::atomic<bool>& cancelled,
            const RunOptions& options, ResultStore& store, int workers,
            const std::unordered_map<std::string, std::unordered_map<std::string, Data>>& injected)
      : plan_(plan), sink_(sink), cancelled_(cancelled), options_(options), store_(store),
        injected_(injected), workers_(workers), threadBudget_(threadBudgetFor(workers)),
        n_(plan.nodes.size()), remaining_(n_, 0), verdict_(n_, Verdict::Ok), done_(n_, 0),
        failure_(n_), releases_(n_) {
    // 惰性节点不进就绪队列，但它们的**非惰性**祖先仍要在 demand 发生前跑完 ——
    // 否则 satisfyDemand 会撞上一个还没轮到的普通节点。所以每个非惰性节点的依赖
    // 是「直接上游穿过惰性节点后落到的那圈非惰性节点」（ADR-0016）。
    // Plan::nodes 是拓扑序，上游下标一定更小，所以升序算一遍就够。
    std::vector<std::vector<int>> frontier(n_);
    auto absorb = [&](std::set<int>& into, int u) {
      const auto ui = static_cast<std::size_t>(u);
      if (!plan_.nodes[ui].deferred) {
        into.insert(u);
        return;
      }
      into.insert(frontier[ui].begin(), frontier[ui].end());
    };
    for (std::size_t i = 0; i < n_; ++i) {
      std::set<int> deps;
      for (int u : plan_.nodes[i].upstream) absorb(deps, u);
      if (plan_.nodes[i].deferred) {
        frontier[i].assign(deps.begin(), deps.end());
        continue;
      }
      remaining_[i] = static_cast<int>(deps.size());
      for (int d : deps) releases_[static_cast<std::size_t>(d)].push_back(static_cast<int>(i));
      if (deps.empty()) ready_.push(i);
    }
  }

  void run() {
    if (n_ == 0) return;
    const int count = std::max(1, std::min<int>(workers_, static_cast<int>(n_)));
    std::vector<std::thread> pool;
    pool.reserve(static_cast<std::size_t>(count) - 1);
    for (int i = 1; i < count; ++i) pool.emplace_back([this] { worker(); });
    worker();
    for (auto& t : pool) t.join();

    for (std::size_t i = 0; i < n_; ++i) {
      if (done_[i]) continue;
      // 没被 demand 的惰性闭包：这不是错误，是它该有的结局（ADR-0016）。
      if (plan_.nodes[i].deferred) {
        sink_.nodeNotDemanded(plan_.nodes[i].id);
        continue;
      }
      // 依赖计数坏掉时的兜底。正常路径永远走不到这里 —— 有环在编译期就拦下了。
      sink_.nodeFailed(plan_.nodes[i].id, "error",
                       {Status::Error(Phase::Execute, "internal", "调度器没有排到这个节点")}, -1);
      verdict_[i] = Verdict::Failed;
      done_[i] = 1;
    }
    tally();
  }

  bool anyError() const { return anyError_; }
  bool sawCancel() const { return sawCancel_; }
  std::size_t failedCount() const { return failed_; }

 private:
  /// 整轮跑完后清点。一个失败若被下游的 acceptsError 端口全数接住，就不算整体失败 ——
  /// 「模型路径失败、回退到模板路径、量出结果」这条路必须以 run_finished: ok 收场
  /// （ADR-0016）。
  void tally() {
    struct Edge {
      std::size_t consumer;
      bool acceptsError;
    };
    std::vector<std::vector<Edge>> consumers(n_);
    for (std::size_t i = 0; i < n_; ++i) {
      for (const InputBinding& b : plan_.nodes[i].inputs) {
        if (b.fromNode < 0) continue;
        consumers[static_cast<std::size_t>(b.fromNode)].push_back(Edge{i, b.acceptsError});
      }
    }
    // 逆拓扑序一遍：消费者的下标一定更大，所以倒着算，absorbed 现成可用。
    // 「被接住」是传递的 —— 一个失败先连坐了几个中间节点、最后才撞上
    // acceptsError 端口，整条级联都算被接住（ADR-0016）。
    std::vector<char> absorbed(n_, 0);
    for (std::size_t k = n_; k-- > 0;) {
      if (!done_[k] || verdict_[k] != Verdict::Failed) continue;
      if (consumers[k].empty()) continue;
      bool all = true;
      for (const Edge& e : consumers[k]) {
        if (e.acceptsError) continue;
        if (verdict_[e.consumer] == Verdict::Failed && done_[e.consumer] && absorbed[e.consumer]) {
          continue;
        }
        all = false;
        break;
      }
      absorbed[k] = all ? 1 : 0;
    }
    for (std::size_t i = 0; i < n_; ++i) {
      if (!done_[i] || verdict_[i] == Verdict::Ok) continue;
      if (verdict_[i] == Verdict::Cancelled) {
        sawCancel_ = true;
        failed_ += 1;
        continue;
      }
      if (absorbed[i]) continue;
      anyError_ = true;
      failed_ += 1;
    }
  }

  void worker() {
    for (;;) {
      std::size_t index = 0;
      {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [this] { return !ready_.empty() || inflight_ == 0; });
        if (ready_.empty()) {
          cv_.notify_all();  // 叫醒其余 worker 一起收工
          return;
        }
        index = ready_.top();
        ready_.pop();
        inflight_ += 1;
      }

      const Verdict v = execute(index);
      finish(index, v, /*releaseDownstream=*/true);
      cv_.notify_all();
    }
  }

  /// 记账：裁决、完成标记、放行下游。惰性闭包里的节点不放行下游 —— 它的下游要么
  /// 也在闭包里（由 satisfyDemand 顺序跑），要么是正在等它的那个 demand 发起者。
  void finish(std::size_t index, Verdict v, bool releaseDownstream) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      verdict_[index] = v;
      done_[index] = 1;
      if (releaseDownstream) inflight_ -= 1;
      // 惰性闭包里的节点从不是别人的 blocker（frontier 只收非惰性节点），
      // releases_ 为空，所以这一段对它们天然是空转。
      for (int d : releases_[index]) {
        const auto di = static_cast<std::size_t>(d);
        if (--remaining_[di] == 0) ready_.push(di);
      }
    }
  }

  Verdict verdictOf(std::size_t i) const {
    std::lock_guard<std::mutex> lock(mu_);
    return done_[i] ? verdict_[i] : Verdict::Ok;
  }

  bool doneOf(std::size_t i) const {
    std::lock_guard<std::mutex> lock(mu_);
    return done_[i] != 0;
  }

  Status failureOf(std::size_t i) const {
    std::lock_guard<std::mutex> lock(mu_);
    return failure_[i];
  }

  void recordFailure(std::size_t i, const Status& s) {
    std::lock_guard<std::mutex> lock(mu_);
    failure_[i] = s;
  }

  Verdict execute(std::size_t i) {
    PlanNode& node = plan_.nodes[i];
    sink_.nodeState(node.id, "pending");

    if (cancelled_.load(std::memory_order_relaxed)) {
      const Status s = Status::Error(Phase::Execute, "cancelled", "运行已取消");
      recordFailure(i, s);
      sink_.nodeFailed(node.id, "cancelled", {s}, -1);
      return Verdict::Cancelled;
    }

    // 自身校验没过：报它全部的诊断，然后当作失败往下传。其余节点照常跑 ——
    // 一次看到所有能看到的（D5 的执行期对应物）。
    if (!node.valid || !node.op) {
      std::vector<Status> errors;
      for (const auto& d : node.errors) errors.push_back(d.status);
      if (errors.empty()) {
        errors.push_back(Status::Error(Phase::Validate, "internal", "节点校验未通过"));
      }
      recordFailure(i, errors.front());
      sink_.nodeFailed(node.id, "error", errors, -1);
      return Verdict::Failed;
    }

    // 上游失败 → cancelled + upstream_failed。严格不用 skipped：后者保留给缓存命中。
    // 例外是声明了 acceptsError 的端口：它收到一个 Error Data，本节点照跑（ADR-0016）。
    Status poison = Status::Ok();
    bool byCancel = false;
    if (!upstreamOk(node, poison, byCancel)) {
      recordFailure(i, poison);
      sink_.nodeFailed(node.id, "cancelled", {poison}, -1);
      return byCancel ? Verdict::Cancelled : Verdict::Failed;
    }

    const auto nodeStart = Clock::now();

    // 缓存命中：直接把仓里那份挂到本次运行，不调 compute（ADR-0007）。
    // 没有输出端口的算子（io.save_pcd 这类纯副作用）永远不复用。
    // provided 节点不复用：注入摘要已经进了 cacheKey，但直接装配比查仓更省事。
    if (!options_.noReuse && !node.bypass && !node.provided &&
        node.op->capabilities.deterministic && !node.op->outputs.empty()) {
      std::vector<OutputInfo> infos;
      if (store_.reuse(options_.runId, node.id, node.cacheKey, outputPortNames(*node.op), infos)) {
        std::size_t bytes = 0;
        for (const auto& o : infos) bytes += o.byteSize;
        const std::size_t primary = infos.empty() ? 0 : infos.front().elementCount;
        sink_.nodeFinished(node.id, "skipped", msSince(nodeStart), primary, bytes, infos,
                           /*cached=*/true, /*bypassed=*/false);
        return Verdict::Ok;
      }
    }

    sink_.nodeState(node.id, "running");

    std::unordered_map<std::string, Data> inputValues;
    std::unordered_map<std::string, Data> outputValues;
    bool bypassed = false;
    Status status = Status::Ok();

    if (node.provided) {
      // 宿主注入：整个 compute 被跳过，输出直接从注入数据装配（ADR-0017）。
      auto it = injected_.find(node.id);
      if (it != injected_.end()) {
        for (const auto& kv : it->second) outputValues[kv.first] = kv.second;
      }
    } else if (node.bypass) {
      bypassed = true;
      if (const Status bad = collectInputs(node, inputValues); !bad.ok) {
        recordFailure(i, bad);
        sink_.nodeFailed(node.id, "error", {bad}, msSince(nodeStart));
        return Verdict::Failed;
      }
      passThrough(node, inputValues, outputValues);
    } else {
      // 惰性端口（ADR-0016）：compute 返回 Demand 就把那条闭包编进计划、跑完、再调一次。
      // 上限按惰性端口个数 +1 —— 一个算子最多把每个惰性端口 demand 一遍。
      int budget = 1;
      for (const Port& p : node.op->inputs) {
        if (p.lazy) budget += 1;
      }
      for (int attempt = 0; attempt < budget; ++attempt) {
        inputValues.clear();
        outputValues.clear();
        if (const Status bad = collectInputs(node, inputValues); !bad.ok) {
          recordFailure(i, bad);
          sink_.nodeFailed(node.id, "error", {bad}, msSince(nodeStart));
          return Verdict::Failed;
        }
        Inputs inputs(inputValues);
        Outputs outputs(outputValues);
        ParamView params(node.params, options_.baseDir);
        NodeContext ctx(sink_, cancelled_, node.id, options_.baseDir, threadBudget_);
        try {
          status = node.op->compute(inputs, params, outputs, ctx);
        } catch (const std::exception& e) {
          // 算子抛出的任何异常都在这里兜住。绝不让它穿到 C ABI ——
          // 跨 DLL 边界抛异常一旦有 Rust 栈帧介入就是未定义行为。
          status = Status::Error(Phase::Execute, "internal",
                                 std::string("算子内部异常: ") + e.what());
        } catch (...) {
          status = Status::Error(Phase::Execute, "internal", "算子内部异常（未知类型）");
        }
        if (!status.isDemand()) break;
        if (attempt + 1 >= budget) {
          status = Status::Error(Phase::Execute, "internal",
                                 "算子反复 demand 同一批端口，超过 " + std::to_string(budget) +
                                     " 次");
          break;
        }
        if (const Status bad = satisfyDemand(i, status.portName); !bad.ok) {
          status = bad;
          break;
        }
        // demand 之后上游可能刚失败：不接受 Error 的端口在这里连坐。
        Status latePoison = Status::Ok();
        bool lateCancel = false;
        if (!upstreamOk(node, latePoison, lateCancel)) {
          recordFailure(i, latePoison);
          sink_.nodeFailed(node.id, "cancelled", {latePoison}, msSince(nodeStart));
          return lateCancel ? Verdict::Cancelled : Verdict::Failed;
        }
      }
    }

    const double durationMs = msSince(nodeStart);

    if (cancelled_.load(std::memory_order_relaxed)) {
      const Status s = Status::Error(Phase::Execute, "cancelled", "运行已取消");
      recordFailure(i, s);
      sink_.nodeFailed(node.id, "cancelled", {s}, durationMs);
      return Verdict::Cancelled;
    }
    if (!status.ok) {
      recordFailure(i, status);
      sink_.nodeFailed(node.id, "error", {status}, durationMs);
      return Verdict::Failed;
    }

    if (const Status contract = checkOutputs(node, outputValues, bypassed); !contract.ok) {
      recordFailure(i, contract);
      sink_.nodeFailed(node.id, "error", {contract}, durationMs);
      return Verdict::Failed;
    }

    // 预览抽稀只在源头做一次：无输入的节点抽完，整条链自然都变快（F5）。
    if (options_.mode == RunMode::Preview && node.inputs.empty()) {
      const std::size_t cap = options_.previewMaxPoints ? options_.previewMaxPoints
                                                        : kDefaultPreviewMaxPoints;
      for (auto& kv : outputValues) {
        const PointCloud* c = kv.second.asCloud();
        if (!c || c->pointCount() <= cap) continue;
        kv.second = Data::cloud(decimateCloud(*c, cap));
      }
    }

    std::size_t totalBytes = 0;
    std::size_t primaryElements = 0;
    std::vector<OutputInfo> infos;
    bool first = true;
    for (const Port& p : node.op->outputs) {
      auto it = outputValues.find(p.name);
      if (it == outputValues.end() || it->second.empty()) continue;  // bypass 找不到源
      Data& d = it->second;
      totalBytes += d.byteSize();
      if (first) {
        primaryElements = d.elementCount();
        first = false;
      }
      infos.push_back(
          OutputInfo{p.name, d.typeName(), d.elementCount(), d.byteSize(), d.valueJson()});
      appendBundleFieldInfos(p.name, d, infos);
      store_.put(options_.runId, node.id, p.name, node.cacheKey, d);
    }

    sink_.nodeFinished(node.id, bypassed ? "skipped" : "done", durationMs, primaryElements,
                       totalBytes, infos, /*cached=*/false, bypassed, node.provided);
    return Verdict::Ok;
  }

  /// 把某个惰性端口的上游闭包编进计划、按拓扑序跑完（ADR-0016）。
  /// 整个过程串行且加锁：两个 fallback 共用一条备用闭包时，那条闭包只跑一遍。
  Status satisfyDemand(std::size_t i, const std::string& port) {
    std::lock_guard<std::mutex> lock(demandMu_);
    const PlanNode& node = plan_.nodes[i];
    const InputBinding* binding = nullptr;
    for (const InputBinding& b : node.inputs) {
      if (b.port == port) {
        binding = &b;
        break;
      }
    }
    if (!binding || binding->fromNode < 0) {
      return Status::Error(Phase::Execute, "missing_input",
                           "算子 demand 了端口 '" + port + "'，但它没有连线", {}, port);
    }

    std::vector<std::size_t> closure;
    std::vector<char> seen(n_, 0);
    std::vector<std::size_t> stack{static_cast<std::size_t>(binding->fromNode)};
    while (!stack.empty()) {
      const std::size_t id = stack.back();
      stack.pop_back();
      if (seen[id]) continue;
      seen[id] = 1;
      if (doneOf(id)) continue;
      closure.push_back(id);
      for (int u : plan_.nodes[id].upstream) stack.push_back(static_cast<std::size_t>(u));
    }
    if (closure.empty()) return Status::Ok();
    // Plan::nodes 本身是拓扑序，下标升序就是执行顺序。
    std::sort(closure.begin(), closure.end());
    sink_.planExtended(plan_, closure, node.id, port);
    for (std::size_t id : closure) {
      if (doneOf(id)) continue;
      finish(id, execute(id), /*releaseDownstream=*/false);
    }
    return Status::Ok();
  }

  /// 上游裁决。全 Ok（或失败但落在 acceptsError 端口上）返回 true；
  /// 否则填 poison 并说明是取消还是失败连坐。
  bool upstreamOk(const PlanNode& node, Status& poison, bool& byCancel) const {
    for (const InputBinding& b : node.inputs) {
      if (b.fromNode < 0) continue;
      const auto ui = static_cast<std::size_t>(b.fromNode);
      if (!doneOf(ui)) continue;  // 惰性且尚未 demand：端口缺席，不是失败
      const Verdict v = verdictOf(ui);
      if (v == Verdict::Ok) continue;
      if (v == Verdict::Failed && b.acceptsError) continue;
      byCancel = v == Verdict::Cancelled;
      poison = byCancel ? Status::Error(Phase::Execute, "cancelled", "运行已取消")
                        : Status::Error(Phase::Execute, "upstream_failed",
                                        "上游节点 " + plan_.nodes[ui].id + " 失败，未执行", {},
                                        b.port);
      return false;
    }
    return true;
  }

  Status collectInputs(const PlanNode& node, std::unordered_map<std::string, Data>& values) const {
    for (const InputBinding& b : node.inputs) {
      if (b.fromNode < 0) continue;
      const auto ui = static_cast<std::size_t>(b.fromNode);
      // 惰性上游还没被 demand：端口就是缺席的，算子据此返回 Status::Demand。
      if (!doneOf(ui)) continue;
      // 失败的上游落在 acceptsError 端口上：这里把那条 Status 变成一个 Error Data。
      if (verdictOf(ui) == Verdict::Failed && b.acceptsError) {
        values[b.port] = Data::error(failureOf(ui));
        continue;
      }
      const PlanNode& up = plan_.nodes[ui];
      Data d;
      if (!store_.get(options_.runId, up.id, b.fromPort, d)) {
        // 上游是被静音的节点、而它那个输出没找到可透传的源（E5）。
        const bool bypassSource = up.bypass;
        return Status::Error(
            Phase::Execute, bypassSource ? "bypassed_no_source" : "internal",
            bypassSource ? "上游 " + up.id + " 已静音，且它的 " + b.fromPort +
                               " 端口找不到类型兼容的输入可以透传"
                         : "上游 " + up.id + "." + b.fromPort + " 没有产出结果",
            {}, b.port);
      }
      // 声明类型编译期查过了，但流过来的 Data 是什么 Kind 还没人查。Any 端口会让
      // 声明层面的检查全过，而算子里的 asCloud() 返回 nullptr，解引用是 SEH，兜不住。
      const Port* declared = findPortByName(node.op->inputs, b.port);
      if (declared) {
        const std::string& want = effectiveType(node.inputTypes, b.port, declared->type);
        const Data::Kind expected = kindFromTypeName(want);
        // Bundle 还要 kind 对得上：Any 端口（flow.fallback 这类）会把两种 Bundle 都放过来。
        if ((expected != Data::Kind::None && d.kind() != expected) ||
            (expected == Data::Kind::Bundle && want != d.typeName())) {
          return Status::Error(Phase::Execute, "type_mismatch",
                               std::string("输入端口 '") + b.port + "' 需要 " + want +
                                   "，实际收到 " + d.typeName(),
                               {}, b.port);
        }
        // 端口契约（ADR-0024）：值刚到端口上就查，第一帧就报，不等算子自己发现。
        // 静音节点的透传不查（它只是搬运，不声称自己算得对），Error 值也不查
        // （那条端口上流的是失败本身）。没声明契约的端口一行都不跑。
        if (!node.bypass && !d.isError()) {
          nlohmann::json contractWant;
          nlohmann::json contractGot;
          std::string message;
          if (!checkPortContract(declared->contract, d, contractWant, contractGot, message)) {
            sink_.contractViolation(node.id, b.port, std::move(contractWant),
                                    std::move(contractGot));
            return Status::Error(Phase::Execute, "contract_violation", message, {}, b.port);
          }
        }
      }
      values[b.port] = std::move(d);
    }
    // 宿主注入的输入端口（m8-plan L18）：这些端口没有连线，值直接取注入数据。
    // 与连线来的值过同一道门：Kind 对不上报 type_mismatch，契约照查。
    if (!node.injectedInputs.empty()) {
      auto byNode = injected_.find(node.id);
      for (const std::string& port : node.injectedInputs) {
        if (byNode == injected_.end()) break;
        auto hit = byNode->second.find(port);
        if (hit == byNode->second.end()) continue;
        const Data& d = hit->second;
        const Port* declared = findPortByName(node.op->inputs, port);
        if (!declared) continue;
        const Data::Kind expected = kindFromTypeName(declared->type);
        if (expected != Data::Kind::None && d.kind() != expected) {
          return Status::Error(Phase::Execute, "type_mismatch",
                               "注入的输入端口 '" + port + "' 需要 " + declared->type +
                                   "，实际收到 " + d.typeName(),
                               {}, port);
        }
        if (!node.bypass) {
          nlohmann::json contractWant;
          nlohmann::json contractGot;
          std::string message;
          if (!checkPortContract(declared->contract, d, contractWant, contractGot, message)) {
            sink_.contractViolation(node.id, port, std::move(contractWant), std::move(contractGot));
            return Status::Error(Phase::Execute, "contract_violation", message, {}, port);
          }
        }
        values[port] = d;
      }
    }
    return Status::Ok();
  }

  /// 静音节点的透传（E5）：每个输出取第一个类型兼容且已连线的输入。
  void passThrough(const PlanNode& node, const std::unordered_map<std::string, Data>& inputs,
                   std::unordered_map<std::string, Data>& outputs) const {
    for (const Port& out : node.op->outputs) {
      const std::string& want = effectiveType(node.outputTypes, out.name, out.type);
      const Data::Kind wantKind = kindFromTypeName(want);
      for (const InputBinding& b : node.inputs) {
        auto it = inputs.find(b.port);
        if (it == inputs.end() || it->second.empty()) continue;
        if (wantKind != Data::Kind::None && it->second.kind() != wantKind) continue;
        if (wantKind == Data::Kind::Bundle && want != it->second.typeName()) continue;
        outputs[out.name] = it->second;
        break;
      }
    }
  }

  Status checkOutputs(const PlanNode& node, std::unordered_map<std::string, Data>& values,
                      bool bypassed) const {
    for (const Port& p : node.op->outputs) {
      auto it = values.find(p.name);
      if (it == values.end() || it->second.empty()) {
        // 静音节点找不到源时该输出就是空的，报错留给下游（E5）。
        if (bypassed) continue;
        // 算子必须把声明过的输出端口都填上。少填了是算子的 bug，早点炸在这里，
        // 好过让下游收到一个空 Data 再报「上游没有产出」。
        return Status::Error(Phase::Execute, "output_not_written",
                             "算子没有写输出端口 '" + p.name +
                                 "'：声明过的每个输出端口都必须写，Port.required 只对输入有效",
                             {}, p.name);
      }
      const std::string& want = effectiveType(node.outputTypes, p.name, p.type);
      const Data::Kind expected = kindFromTypeName(want);
      if (expected == Data::Kind::Bundle) {
        // 按 manifest 的字段表查（m8-plan L2）：字段缺了或类型不对，下游按 `<port>.<field>`
        // 取到的就是空的 —— 在写出端口的这一刻报，而不是等消费方撞上。
        nlohmann::json want2;
        nlohmann::json got;
        const std::string problem = ensureRegistry().checkBundle(want, it->second, &want2, &got);
        if (!problem.empty()) {
          sink_.contractViolation(node.id, p.name, std::move(want2), std::move(got));
          return Status::Error(Phase::Execute, "contract_violation",
                               "输出端口 '" + p.name + "'：" + problem, {}, p.name);
        }
        continue;
      }
      if (expected != Data::Kind::None && it->second.kind() != expected) {
        return Status::Error(Phase::Execute, "internal",
                             std::string("输出端口 '") + p.name + "' 声明为 " + want +
                                 "，实际是 " + it->second.typeName(),
                             {}, p.name);
      }
      if (const PointCloud* c = it->second.asCloud()) {
        if (!c->channelsConsistent()) {
          return Status::Error(Phase::Execute, "internal",
                               std::string("输出端口 '") + p.name +
                                   "' 的点云通道长度不一致（intensity/normals/rgb "
                                   "必须为空或与点数对齐）",
                               {}, p.name);
        }
      }
    }
    return Status::Ok();
  }

  struct ByLevelThenIndex {
    const Plan* plan;
    bool operator()(std::size_t a, std::size_t b) const {
      const int la = plan->nodes[a].level, lb = plan->nodes[b].level;
      if (la != lb) return la > lb;  // 小的先出队
      return a > b;
    }
  };

  Plan& plan_;
  EventSink& sink_;
  const std::atomic<bool>& cancelled_;
  const RunOptions& options_;
  ResultStore& store_;
  const std::unordered_map<std::string, std::unordered_map<std::string, Data>>& injected_;
  int workers_ = 1;
  int threadBudget_ = 1;
  std::size_t n_ = 0;

  mutable std::mutex mu_;
  std::mutex demandMu_;
  std::condition_variable cv_;
  std::priority_queue<std::size_t, std::vector<std::size_t>, ByLevelThenIndex> ready_{
      ByLevelThenIndex{&plan_}};
  std::vector<int> remaining_;
  std::vector<Verdict> verdict_;
  std::vector<char> done_;
  /// 每个节点的失败原因。acceptsError 端口据此造 Error Data。
  std::vector<Status> failure_;
  /// 某节点跑完时该给谁减依赖计数（穿过惰性节点后的实际依赖）。
  std::vector<std::vector<int>> releases_;
  int inflight_ = 0;
  bool anyError_ = false;
  bool sawCancel_ = false;
  std::size_t failed_ = 0;
};

// ------------------------------------------------------------------ summary

/// `failed` 输出的 `from`：从产出节点沿边逆着走，找**最近**的出错节点（ADR-0022）。
/// 同一跳距上有多个候选时取拓扑序最早的那个 —— Plan::nodes 本身是拓扑序，
/// 所以就是下标最小的那个。先找 state=error，一个都没有再退而找 cancelled
/// （整轮被取消时全图都是 cancelled，没有「真正出错」的那一个）。
std::string traceBackToError(const Plan& plan, std::size_t start,
                             const std::map<std::string, NodeOutcome>& outcomes,
                             std::string& codeOut) {
  const auto n = plan.nodes.size();
  for (const char* want : {"error", "cancelled"}) {
    std::vector<char> seen(n, 0);
    std::vector<std::size_t> frontier{start};
    seen[start] = 1;
    while (!frontier.empty()) {
      // 同一跳距的候选先收齐，再按拓扑序（下标）挑最早的
      std::size_t best = n;
      for (std::size_t i : frontier) {
        auto it = outcomes.find(plan.nodes[i].id);
        if (it == outcomes.end() || it->second.state != want) continue;
        if (i < best) best = i;
      }
      if (best < n) {
        codeOut = outcomes.at(plan.nodes[best].id).code;
        return plan.nodes[best].id;
      }
      std::vector<std::size_t> next;
      for (std::size_t i : frontier) {
        for (int u : plan.nodes[i].upstream) {
          const auto ui = static_cast<std::size_t>(u);
          if (seen[ui]) continue;
          seen[ui] = 1;
          next.push_back(ui);
        }
      }
      frontier.swap(next);
    }
  }
  codeOut.clear();
  return {};
}

/// `failed` 输出的 `root`：从 `from` 继续沿**失败链**往上游走，取拓扑序最早的 error
/// 节点（m6-plan §10 第 4 条）。`from` 是最近的那一个，实测里它常常就是 fallback 自己，
/// 而真正要看的根因在两跳外；两个都给，消费者按需取。
///
/// 只穿过 error / cancelled 的节点：一个跑成功了的上游不可能是这条失败链的一环，
/// 从它继续往上会追到另一处无关的失败上去。
std::string traceRootError(const Plan& plan, const std::string& fromId,
                           const std::map<std::string, NodeOutcome>& outcomes,
                           std::string& codeOut) {
  std::unordered_map<std::string, std::size_t> indexOf;
  for (std::size_t i = 0; i < plan.nodes.size(); ++i) indexOf[plan.nodes[i].id] = i;
  auto at = indexOf.find(fromId);
  if (at == indexOf.end()) return {};

  auto stateOf = [&](std::size_t i) -> const std::string* {
    auto it = outcomes.find(plan.nodes[i].id);
    return it == outcomes.end() ? nullptr : &it->second.state;
  };

  const auto n = plan.nodes.size();
  std::vector<char> seen(n, 0);
  std::vector<std::size_t> stack{at->second};
  seen[at->second] = 1;
  std::size_t bestError = n;
  std::size_t bestCancelled = n;
  while (!stack.empty()) {
    const std::size_t i = stack.back();
    stack.pop_back();
    const std::string* state = stateOf(i);
    if (state == nullptr) continue;
    if (*state == "error") {
      if (i < bestError) bestError = i;
    } else if (*state == "cancelled") {
      if (i < bestCancelled) bestCancelled = i;
    } else {
      continue;  // 跑成功的节点不是这条失败链的一环
    }
    for (int u : plan.nodes[i].upstream) {
      const auto ui = static_cast<std::size_t>(u);
      if (seen[ui]) continue;
      seen[ui] = 1;
      stack.push_back(ui);
    }
  }
  const std::size_t best = bestError < n ? bestError : bestCancelled;
  if (best >= n) return {};
  codeOut = outcomes.at(plan.nodes[best].id).code;
  return plan.nodes[best].id;
}

/// 类型为 FallbackChoice 的 Record 输出全收上来（H4）。按 Record 的 `type` 判断，
/// 不按算子 id —— 任何包的决策算子照这个类型声明就自动进 summary。
nlohmann::json collectDecisions(const Plan& plan,
                                const std::map<std::string, NodeOutcome>& outcomes,
                                const std::string& runId, ResultStore& store) {
  nlohmann::json out = nlohmann::json::object();
  for (const PlanNode& n : plan.nodes) {
    auto it = outcomes.find(n.id);
    if (it == outcomes.end() || !it->second.outputsAvailable || !n.op) continue;
    for (const Port& p : n.op->outputs) {
      Data d;
      if (!store.get(runId, n.id, p.name, d)) continue;
      const Record* r = d.asRecord();
      if (!r || r->type != "FallbackChoice") continue;
      nlohmann::json item = r->data.is_object() ? r->data : nlohmann::json::object();
      item["port"] = p.name;
      item["type"] = r->type;
      // 一个节点通常只产出一个 choice，键就是节点 id（读起来像 §1 的样子）；
      // 真有第二个时退成 node:port，免得后一个把前一个顶掉。
      const std::string key = out.contains(n.id) ? n.id + ":" + p.name : n.id;
      out[key] = std::move(item);
    }
  }
  return out;
}

/// 整个 run summary（ADR-0022 / m6-plan §1）。执行器手上有全部节点状态、
/// 声明输出与结果仓，所以只能在这一层拼 —— 消费者从事件流重建必错。
std::string buildSummaryJson(const std::string& runId, const Plan& plan,
                             const std::map<std::string, NodeOutcome>& outcomes,
                             double durationMs, bool forceFailed, ResultStore* store,
                             const std::vector<ContractViolation>& violations) {
  std::unordered_map<std::string, std::size_t> indexOf;
  for (std::size_t i = 0; i < plan.nodes.size(); ++i) indexOf[plan.nodes[i].id] = i;

  bool anyError = false;
  bool anyCancelled = false;
  for (const auto& kv : outcomes) {
    if (kv.second.state == "error") anyError = true;
    if (kv.second.state == "cancelled") anyCancelled = true;
  }

  // 先把每个声明输出定成三态（H3），status 要靠它们（H2）。
  struct OutputState {
    const PlanOutput* decl = nullptr;
    std::string state;   ///< value / inactive / failed
    std::string reason;  ///< inactive 的原因
    std::string from;    ///< failed 时回溯到的**最近**出错节点
    std::string code;    ///< from 的错误码
    std::string root;    ///< 沿失败链继续追到的、拓扑序最早的出错节点
    std::string rootCode;
    bool hasInfo = false;
    OutputInfo info;
  };
  std::vector<OutputState> outputs;
  outputs.reserve(plan.outputs.size());
  for (const auto& o : plan.outputs) {
    OutputState s;
    s.decl = &o;
    if (store && store->outputInfo(runId, o.nodeId, o.port, s.info)) {
      s.state = "value";
      s.hasInfo = true;
      outputs.push_back(std::move(s));
      continue;
    }
    auto oc = outcomes.find(o.nodeId);
    if (oc == outcomes.end()) {
      // 节点一条终态事件都没发过：它根本没进这一轮（比如 --to 把它裁掉了）。
      s.state = "inactive";
      s.reason = "not_run";
    } else if (oc->second.reason == "not_demanded") {
      s.state = "inactive";
      s.reason = "not_demanded";
    } else if (oc->second.bypassed) {
      // 静音节点没找到类型兼容的源可以透传（E5）：这一维「本来就没有」。
      s.state = "inactive";
      s.reason = "bypassed_no_source";
    } else {
      s.state = "failed";
      auto at = indexOf.find(o.nodeId);
      if (at != indexOf.end()) s.from = traceBackToError(plan, at->second, outcomes, s.code);
      if (s.from.empty()) {
        s.from = o.nodeId;
        s.code = oc->second.code.empty() ? std::string("no_result") : oc->second.code;
      }
      // 根因（m6-plan §10 第 4 条）。from 常常是 fallback 自己，而这一维之所以没有
      // 是因为两跳外那个 fit 崩了 —— 两个都给，消费者按需取。
      s.root = traceRootError(plan, s.from, outcomes, s.rootCode);
      if (s.root.empty()) {
        s.root = s.from;
        s.rootCode = s.code;
      }
    }
    outputs.push_back(std::move(s));
  }

  bool anyOutputFailed = false;
  for (const auto& s : outputs) {
    if (s.state == "failed") anyOutputFailed = true;
  }

  // H2 的三态。forceFailed 是额外一条：整轮被取消、或图根本没编译过 ——
  // 这两种情况下什么都不能信，直接 failed。
  const char* status = "ok";
  if (forceFailed || anyOutputFailed || (plan.outputs.empty() && anyError)) {
    status = "failed";
  } else if (anyError || anyCancelled) {
    status = "degraded";
  }

  JsonWriter w;
  w.setIndent(0);
  w.beginObject();
  w.field("runId", runId);
  w.field("status", std::string(status));
  w.field("durationMs", durationMs);

  w.key("nodes");
  w.beginObject();
  auto writeNode = [&w](const std::string& id, const NodeOutcome& o) {
    w.key(id);
    w.beginObject();
    w.field("state", o.state);
    w.fieldIfSet("code", o.code);
    w.fieldIfSet("reason", o.reason);
    if (o.durationMs >= 0) w.field("durationMs", o.durationMs);
    if (o.state == "done" || o.state == "skipped") w.field("cached", o.cached);
    if (o.bypassed) w.field("bypassed", true);
    if (o.provided) w.field("provided", true);
    w.field("outputsAvailable", o.outputsAvailable);
    w.endObject();
  };
  // 先按计划的拓扑序，再补上计划之外的（整图编译失败时只有后者）
  std::set<std::string> written;
  for (const PlanNode& n : plan.nodes) {
    auto it = outcomes.find(n.id);
    if (it == outcomes.end()) continue;
    writeNode(n.id, it->second);
    written.insert(n.id);
  }
  for (const auto& kv : outcomes) {
    if (written.count(kv.first)) continue;
    writeNode(kv.first, kv.second);
  }
  w.endObject();

  w.key("outputs");
  w.beginObject();
  for (const OutputState& s : outputs) {
    w.key(s.decl->name);
    w.beginObject();
    w.field("state", s.state);
    w.field("node", s.decl->nodeId);
    w.field("port", s.decl->port);
    if (s.hasInfo) {
      w.field("type", s.info.type);
      w.field("elementCount", static_cast<std::int64_t>(s.info.elementCount));
      // 点云只给元信息，二进制仍走 lyflow_output_cloud（D4）。
      if (!s.info.valueJson.empty()) {
        w.key("value");
        w.raw(s.info.valueJson);
      }
    }
    w.fieldIfSet("reason", s.reason);
    w.fieldIfSet("from", s.from);
    w.fieldIfSet("code", s.code);
    w.fieldIfSet("root", s.root);
    w.fieldIfSet("rootCode", s.rootCode);
    w.endObject();
  }
  w.endObject();

  w.key("decisions");
  if (store) {
    w.raw(collectDecisions(plan, outcomes, runId, *store).dump());
  } else {
    w.beginObject();
    w.endObject();
  }

  // 端口契约违反（ADR-0024）。按拓扑序（再按端口名）排，同一张图同一份数据两次运行
  // 必须给出逐字节一样的 JSON。没有违反时是空数组，而不是这一项不出现。
  w.key("contractViolations");
  w.beginArray();
  {
    std::vector<const ContractViolation*> sorted;
    sorted.reserve(violations.size());
    for (const auto& v : violations) sorted.push_back(&v);
    const std::size_t end = plan.nodes.size();
    auto rank = [&](const ContractViolation* v) {
      auto it = indexOf.find(v->node);
      return it == indexOf.end() ? end : it->second;
    };
    std::sort(sorted.begin(), sorted.end(),
              [&](const ContractViolation* a, const ContractViolation* b) {
                const auto ra = rank(a), rb = rank(b);
                if (ra != rb) return ra < rb;
                if (a->node != b->node) return a->node < b->node;
                return a->port < b->port;
              });
    for (const ContractViolation* v : sorted) {
      w.beginObject();
      w.field("node", v->node);
      w.field("port", v->port);
      w.key("expected");
      w.raw(v->expected.dump());
      w.key("actual");
      w.raw(v->actual.dump());
      w.endObject();
    }
  }
  w.endArray();

  w.endObject();
  return w.str();
}

}  // namespace

// ------------------------------------------------------------------ 并行度

int resolveMaxParallel(int requested) {
  if (requested > 0) return requested;
  const unsigned cores = std::thread::hardware_concurrency();
  return std::max(1, std::min<int>(4, cores == 0 ? 1 : static_cast<int>(cores)));
}

int threadBudgetFor(int maxParallel) {
  const unsigned cores = std::thread::hardware_concurrency();
  const int total = cores == 0 ? 1 : static_cast<int>(cores);
  return std::max(1, total / std::max(1, maxParallel));
}

// --------------------------------------------------------------------- Run

Run::Run(std::string graphJson, RunOptions options, lyflow_event_cb cb, void* user)
    : graphJson_(std::move(graphJson)), options_(std::move(options)), cb_(cb), user_(user) {
  thread_ = std::thread([this] { work(); });
}

Run::~Run() {
  cancel();
  join();
  ResultStore::instance().freeRun(options_.runId);
}

void Run::cancel() { cancelled_.store(true, std::memory_order_relaxed); }

void Run::join() {
  // 必须自己加锁：并发 join 是数据竞争，重复 join 已 join 过的 thread 是 UB。
  // C ABI 不能把正确性押在「调用方一定加了锁」上。
  std::lock_guard<std::mutex> lock(joinMutex_);
  if (joined_) return;
  if (thread_.joinable()) thread_.join();
  joined_ = true;
}

void Run::work() {
  // 这是 std::thread 的函数体：逃出去的异常直接 std::terminate，整个 app 无声无息地没了。
  // compute() 另有一层，这里兜的是 parse / buildPlan / externalKey / 事件序列化 / 写结果仓。
  try {
    workImpl();
  } catch (const std::exception& e) {
    EventSink sink(options_.runId, cb_, user_);
    Status s = Status::Error(Phase::Execute, "internal",
                             std::string("执行器内部异常: ") + e.what());
    sink.runFinished("error", 0.0, &s);
  } catch (...) {
    EventSink sink(options_.runId, cb_, user_);
    Status s = Status::Error(Phase::Execute, "internal", "执行器内部异常（未知类型）");
    sink.runFinished("error", 0.0, &s);
  }
}

void Run::workImpl() {
  EventSink sink(options_.runId, cb_, user_);
  const auto t0 = Clock::now();

  Diagnostics diags;
  RawGraph raw;
  Plan plan;
  plan.runId = options_.runId;

  // 注入的数据按节点归拢，顺手算出进 cacheKey 的摘要（ADR-0017）。
  std::unordered_map<std::string, std::unordered_map<std::string, Data>> injected;
  std::unordered_map<std::string, std::string> providedDigest;
  std::unordered_map<std::string, std::set<std::string>> injectedPorts;
  {
    std::map<std::string, Hasher> digests;
    for (const InjectedInput& in : options_.inputs) {
      if (in.nodeId.empty() || in.port.empty()) continue;
      injected[in.nodeId][in.port] = in.data;
      injectedPorts[in.nodeId].insert(in.port);
      Hasher& h = digests[in.nodeId];
      h.add(in.port);
      h.add(std::string(in.data.typeName()));
      if (const PointCloud* c = in.data.asCloud()) {
        h.addBytes(c->xyz.data(), c->xyz.size() * sizeof(float));
        h.addBytes(c->intensity.data(), c->intensity.size() * sizeof(float));
      }
    }
    for (auto& kv : digests) providedDigest[kv.first] = kv.second.hex();
  }

  const bool parsed = prepareGraph(graphJson_, raw, diags, options_.paramsJson);
  if (parsed) {
    BuildOptions build;
    build.runId = options_.runId;
    build.baseDir = options_.baseDir;
    build.targets = options_.targets;
    build.cacheNamespace = previewNamespace(options_);
    build.providedDigest = providedDigest;
    build.injectedPorts = injectedPorts;
    buildPlan(ensureRegistry(), raw, build, plan, diags);
  }

  const int workers = resolveMaxParallel(options_.maxParallel);
  const bool preview = options_.mode == RunMode::Preview;
  sink.runStarted(plan, options_.targets, workers, preview,
                  options_.previewMaxPoints ? options_.previewMaxPoints
                                            : kDefaultPreviewMaxPoints);

  // warning 与迁移走 log 通道：不阻断执行，但用户必须看得见。
  // 静默的兼容性降级（「默认值悄悄变了」）是最难排查的一类问题。
  for (const auto& d : diags.items()) {
    if (d.severity == Severity::Warning) sink.log("warn", d.nodeId, d.status.message);
  }

  if (!plan.ok) {
    // 整图级失败：有环、JSON 读不了、Run to node 的目标不存在。
    // 节点级诊断照样按节点发出去，前端还能标红。
    std::unordered_map<std::string, std::vector<Status>> byNode;
    for (const auto& d : diags.items()) {
      if (d.severity == Severity::Error && !d.nodeId.empty()) byNode[d.nodeId].push_back(d.status);
    }
    for (const auto& kv : byNode) {
      sink.nodeState(kv.first, "pending");
      sink.nodeFailed(kv.first, "error", kv.second, -1);
    }
    Status first = Status::Error(Phase::Compile, "internal", "图无法编译");
    for (const auto& d : diags.items()) {
      if (d.severity == Severity::Error) { first = d.status; break; }
    }
    // 编译就没过也给一份 summary：宿主的「这次到底成没成」只读这一个对象（H1）。
    const double failedAt = msSince(t0);
    const std::string summary = buildSummaryJson(options_.runId, plan, sink.outcomes(), failedAt,
                                                 /*forceFailed=*/true, /*store=*/nullptr,
                                                 sink.contractViolations());
    ResultStore::instance().setSummary(options_.runId, summary);
    sink.runFinished("error", failedAt, &first, summary);
    return;
  }

  ResultStore& store = ResultStore::instance();
  if (options_.cacheBudgetBytes) store.setBudget(options_.cacheBudgetBytes);

  // 图级命名输出：编译完就登记，宿主 join 之后按名字取（ADR-0017）。
  {
    std::vector<NamedOutput> named;
    named.reserve(plan.outputs.size());
    for (const auto& o : plan.outputs) named.push_back(NamedOutput{o.name, o.nodeId, o.port});
    store.setNamedOutputs(options_.runId, std::move(named));
  }

  // 本次计划涉及的键全部钉住：正在算的那份不能被 LRU 从下游脚下抽走。
  std::vector<std::string> keys;
  keys.reserve(plan.nodes.size());
  for (const auto& n : plan.nodes) {
    if (!n.cacheKey.empty()) keys.push_back(n.cacheKey);
  }
  CachePin pinned(store, std::move(keys));

  Scheduler scheduler(plan, sink, cancelled_, options_, store, workers, injected);
  scheduler.run();

  const double total = msSince(t0);
  // 超预算的预览要说出来：用户看到的「拖不动」在这里有个可读的名字（F5）。
  if (options_.mode == RunMode::Preview) {
    const std::uint32_t budget =
        options_.previewBudgetMs ? options_.previewBudgetMs : kDefaultPreviewBudgetMs;
    if (total > static_cast<double>(budget)) {
      sink.log("warn", std::string(),
               "预览耗时 " + std::to_string(static_cast<long long>(total)) + " ms，超过预算 " +
                   std::to_string(budget) + " ms；建议降低预览点数");
    }
  }
  const bool cancelled = scheduler.sawCancel() || cancelled_.load(std::memory_order_relaxed);
  // summary 在发 run_finished 之前登记：宿主 join 返回时 lyflow_run_summary
  // 一定拿得到，事件里那份与它是同一个对象（H1）。
  const std::string summary = buildSummaryJson(options_.runId, plan, sink.outcomes(), total,
                                               cancelled, &store, sink.contractViolations());
  store.setSummary(options_.runId, summary);

  if (cancelled) {
    Status s = Status::Error(Phase::Execute, "cancelled", "运行已取消");
    sink.runFinished("cancelled", total, &s, summary);
  } else if (scheduler.anyError()) {
    Status s = Status::Error(Phase::Execute, "internal",
                             std::to_string(scheduler.failedCount()) + " 个节点未能完成");
    sink.runFinished("error", total, &s, summary);
  } else {
    sink.runFinished("ok", total, nullptr, summary);
  }
}

// ------------------------------------------------------------------ validate

bool prepareGraph(const std::string& graphJson, RawGraph& out, Diagnostics& diags,
                  const std::string& paramsJson) {
  RawGraph parsed;
  if (!parseGraph(graphJson, parsed, diags)) return false;
  if (!paramsJson.empty()) {
    nlohmann::json values;
    try {
      values = nlohmann::json::parse(paramsJson);
    } catch (const std::exception& e) {
      diags.error("", Phase::Validate, "bad_input",
                  std::string("顶层参数的取值不是合法 JSON: ") + e.what());
      return false;
    }
    if (!applyGraphParamValues(values, parsed, diags)) return false;
  }
  if (!expandGraph(parsed, out, diags)) return false;
  return true;
}

std::string validateGraphJson(const std::string& graphJson,
                              const std::filesystem::path& baseDir,
                              const std::string& paramsJson) {
  Diagnostics diags;
  RawGraph raw;
  if (prepareGraph(graphJson, raw, diags, paramsJson)) {
    Plan plan;
    BuildOptions build;
    build.runId = "validate";
    build.baseDir = baseDir;
    buildPlan(ensureRegistry(), raw, build, plan, diags);
  }
  return diags.toJson();
}

// ---------------------------------------------------------------------- plan

std::string planGraphJson(const std::string& graphJson, const std::filesystem::path& baseDir,
                          const std::vector<std::string>& targets,
                          const std::string& paramsJson) {
  Diagnostics diags;
  RawGraph raw;
  Plan plan;
  if (!prepareGraph(graphJson, raw, diags, paramsJson)) return diags.toJson();

  BuildOptions build;
  build.runId = "plan";
  build.baseDir = baseDir;
  build.targets = targets;
  buildPlan(ensureRegistry(), raw, build, plan, diags);
  if (diags.hasErrors() || !plan.ok) return diags.toJson();

  ResultStore& store = ResultStore::instance();
  std::vector<char> cached(plan.nodes.size(), 0);
  for (std::size_t i = 0; i < plan.nodes.size(); ++i) {
    const PlanNode& n = plan.nodes[i];
    if (!n.op || n.op->outputs.empty() || !n.op->capabilities.deterministic || n.bypass) continue;
    cached[i] = store.peek(n.cacheKey, outputPortNames(*n.op)) ? 1 : 0;
  }

  // 惰性标记（m6-plan §5 / H9）。manifest 早就有端口的 `lazy`，缺的只是「这张图上
  // 到底哪些节点因此不跑」—— 真实任务里有人翻了源码才发现整条 b 分支是惰性的。
  // `demandedBy` 给的是「谁的哪个惰性端口在管着它」：直接的写那一条，闭包深处的
  // 沿着惰性节点往下继承，所以一条链上的每个节点都指得回那个真正的闸门。
  const std::size_t nodeCount = plan.nodes.size();
  struct Consumer {
    std::size_t node;
    std::string port;
    bool lazy;
  };
  std::vector<std::vector<Consumer>> consumersOf(nodeCount);
  for (std::size_t i = 0; i < nodeCount; ++i) {
    for (const InputBinding& b : plan.nodes[i].inputs) {
      if (b.fromNode < 0) continue;
      consumersOf[static_cast<std::size_t>(b.fromNode)].push_back(Consumer{i, b.port, b.lazy});
    }
  }
  std::vector<std::set<std::string>> gates(nodeCount);
  for (std::size_t k = nodeCount; k-- > 0;) {
    for (const Consumer& c : consumersOf[k]) {
      if (c.lazy) {
        gates[k].insert(plan.nodes[c.node].id + ":" + c.port);
      } else if (plan.nodes[c.node].deferred) {
        gates[k].insert(gates[c.node].begin(), gates[c.node].end());
      }
    }
  }

  JsonWriter w;
  w.setIndent(0);
  w.beginArray();
  for (std::size_t i = 0; i < plan.nodes.size(); ++i) {
    const PlanNode& n = plan.nodes[i];
    bool upstreamMissing = false;
    for (int u : n.upstream) {
      if (!cached[static_cast<std::size_t>(u)]) { upstreamMissing = true; break; }
    }
    w.beginObject();
    w.field("nodeId", n.id);
    w.field("cacheKey", n.cacheKey);
    w.field("cached", cached[i] != 0);
    w.field("level", static_cast<std::int64_t>(n.level));
    w.field("upstreamMissing", upstreamMissing);
    w.field("bypass", n.bypass);
    // 只被惰性端口依赖：主路径成功时它一次都不跑（ADR-0016）。
    w.field("lazy", n.deferred);
    w.key("demandedBy");
    w.beginArray();
    for (const std::string& g : gates[i]) w.value(g);
    w.endArray();
    w.endObject();
  }
  w.endArray();
  return w.str();
}

// --------------------------------------------------------------- 生效参数视图

std::string effectiveParamsJson(const std::string& graphJson,
                                const std::filesystem::path& baseDir,
                                const std::string& paramsJson) {
  Diagnostics diags;
  RawGraph raw;
  Plan plan;
  if (!prepareGraph(graphJson, raw, diags, paramsJson)) return diags.toJson();

  BuildOptions build;
  build.runId = "params";
  build.baseDir = baseDir;
  buildPlan(ensureRegistry(), raw, build, plan, diags);
  if (diags.hasErrors() || !plan.ok) return diags.toJson();

  JsonWriter w;
  w.setIndent(0);
  w.beginObject();
  w.key("nodes");
  w.beginArray();
  for (const PlanNode& n : plan.nodes) {
    if (!n.op) continue;
    w.beginObject();
    w.field("node", n.id);
    w.field("op", n.op->id);
    w.key("params");
    w.beginArray();
    for (const Param& p : n.op->params) {
      // 生效值来自 Plan：合并默认值、规整类型、跑完迁移之后的那一份。
      // 在 Rust 里重算一遍必然与执行器漂开，那正是 `params` 要消掉的问题。
      auto it = n.params.find(p.name);
      if (it == n.params.end()) continue;
      const char* source = "default";
      auto viaGraph = n.graphParams.find(p.name);
      if (viaGraph != n.graphParams.end()) {
        source = "graph";
      } else if (n.boundParams.count(p.name)) {
        source = "bound";
      } else if (n.explicitParams.count(p.name)) {
        source = "explicit";
      }
      w.beginObject();
      w.field("param", p.name);
      w.key("value");
      writeParamValue(w, it->second);
      w.field("source", std::string(source));
      if (viaGraph != n.graphParams.end()) w.field("graphParam", viaGraph->second);
      w.fieldIfSet("label", p.label);
      w.fieldIfSet("unit", p.unit);
      if (p.min) w.field("min", *p.min);
      if (p.max) w.field("max", *p.max);
      w.endObject();
    }
    w.endArray();
    w.endObject();
  }
  w.endArray();
  w.endObject();
  return w.str();
}

// ------------------------------------------------------------------- 图级输出

std::string runOutputsJson(const std::string& runId) {
  ResultStore& store = ResultStore::instance();
  JsonWriter w;
  w.setIndent(0);
  w.beginObject();
  for (const NamedOutput& o : store.namedOutputs(runId)) {
    w.key(o.name);
    w.beginObject();
    w.field("node", o.nodeId);
    w.field("port", o.port);
    OutputInfo info;
    if (store.outputInfo(runId, o.nodeId, o.port, info)) {
      w.field("type", info.type);
      w.field("elementCount", static_cast<std::int64_t>(info.elementCount));
      w.field("byteSize", static_cast<std::int64_t>(info.byteSize));
      // 点云只给元信息，二进制仍走 lyflow_output_cloud（D4）。
      if (!info.valueJson.empty()) {
        w.key("value");
        w.raw(info.valueJson);
      }
    } else {
      w.field("type", std::string("None"));
      w.field("elementCount", static_cast<std::int64_t>(0));
      w.field("byteSize", static_cast<std::int64_t>(0));
      w.field("missing", true);
    }
    w.endObject();
  }
  w.endObject();
  return w.str();
}

// ------------------------------------------------------------------ run summary

bool runSummaryJson(const std::string& runId, std::string& out) {
  return ResultStore::instance().summary(runId, out);
}

// --------------------------------------------------------------------- 导入器

std::string importGraphJson(const std::string& kind, const std::string& text,
                            const std::filesystem::path& baseDir) {
  auto asDiagnostics = [](const Status& s) {
    Diagnostics d;
    d.add("", s, Severity::Error);
    return d.toJson();
  };
  const ImporterDesc* importer = ensureRegistry().findImporter(kind);
  if (!importer || !importer->fn) {
    return asDiagnostics(Status::Error(Phase::Validate, "unknown_importer",
                                       "没有注册 '" + kind + "' 这种导入器"));
  }
  std::string graphJson;
  Status s = Status::Ok();
  try {
    s = importer->fn(text, baseDir, graphJson);
  } catch (const std::exception& e) {
    s = Status::Error(Phase::Validate, "internal", std::string("导入时内部异常: ") + e.what());
  } catch (...) {
    s = Status::Error(Phase::Validate, "internal", "导入时内部异常（未知类型）");
  }
  if (!s.ok) return asDiagnostics(s);
  if (graphJson.empty() || graphJson.front() != '{') {
    return asDiagnostics(Status::Error(Phase::Validate, "internal",
                                       "导入器 '" + kind + "' 没有产出一个 GraphDoc 对象"));
  }
  return graphJson;
}

}  // namespace lyflow::exec
