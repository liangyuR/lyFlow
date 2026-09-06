#include "exec/executor.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <queue>
#include <unordered_map>
#include <unordered_set>

#include "exec/graph.h"
#include "exec/plan.h"
#include "exec/result_store.h"
#include "exec/subgraph.h"
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

void writeError(JsonWriter& w, const Status& s) {
  w.beginObject();
  w.field("phase", std::string(toString(s.phase)));
  w.field("code", s.code);
  w.field("message", s.message);
  w.fieldIfSet("paramPath", s.paramPath);
  w.fieldIfSet("portName", s.portName);
  w.endObject();
}

/// 事件序列化 + 回调。seq 全局单调，多个 worker 同时发事件所以整个入口加锁（E2）。
class EventSink {
 public:
  EventSink(std::string runId, lyflow_event_cb cb, void* user)
      : runId_(std::move(runId)), cb_(cb), user_(user) {}

  void runStarted(const Plan& plan, const std::vector<std::string>& targets, int maxParallel,
                  bool preview, std::uint32_t previewMaxPoints) {
    JsonWriter w;
    std::lock_guard<std::mutex> lock(mu_);
    begin(w, "run_started");
    w.field("nodeCount", static_cast<std::int64_t>(plan.nodes.size()));
    w.field("maxParallel", static_cast<std::int64_t>(maxParallel));
    w.field("mode", std::string(preview ? "preview" : "full"));
    if (preview) w.field("previewMaxPoints", static_cast<std::int64_t>(previewMaxPoints));
    w.key("plan");
    w.beginArray();
    for (const auto& n : plan.nodes) w.value(n.id);
    w.endArray();
    w.key("targets");
    w.beginArray();
    for (const auto& t : targets) w.value(t);
    w.endArray();
    // 前端的精确 stale 标记与「将重算 N 个节点」全靠这一段（ADR-0007）。
    w.key("nodes");
    w.beginArray();
    for (const auto& n : plan.nodes) {
      w.beginObject();
      w.field("id", n.id);
      w.field("cacheKey", n.cacheKey);
      w.field("level", static_cast<std::int64_t>(n.level));
      if (n.bypass) w.field("bypass", true);
      w.endObject();
    }
    w.endArray();
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

  /// done / skipped 共用。cached 与 bypassed 让用户分得清「不用跑」和「被静音」。
  void nodeFinished(const std::string& nodeId, const char* state, double durationMs,
                    std::size_t elementCount, std::size_t byteSize,
                    const std::vector<OutputInfo>& outputs, bool cached, bool bypassed) {
    JsonWriter w;
    std::lock_guard<std::mutex> lock(mu_);
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

  void runFinished(const char* status, double durationMs, const Status* error) {
    JsonWriter w;
    std::lock_guard<std::mutex> lock(mu_);
    begin(w, "run_finished");
    w.field("status", std::string(status));
    w.field("durationMs", durationMs);
    if (error) {
      w.key("error");
      writeError(w, *error);
    }
    end(w);
  }

 private:
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
            const RunOptions& options, ResultStore& store, int workers)
      : plan_(plan), sink_(sink), cancelled_(cancelled), options_(options), store_(store),
        workers_(workers), threadBudget_(threadBudgetFor(workers)), n_(plan.nodes.size()),
        remaining_(n_, 0), verdict_(n_, Verdict::Ok), done_(n_, 0), poisoned_(n_, 0),
        poisonCancelled_(n_, 0), blockedBy_(n_) {
    for (std::size_t i = 0; i < n_; ++i) {
      remaining_[i] = static_cast<int>(plan_.nodes[i].upstream.size());
      if (remaining_[i] == 0) ready_.push(i);
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

    // 依赖计数坏掉时的兜底。正常路径永远走不到这里 —— 有环在编译期就拦下了。
    for (std::size_t i = 0; i < n_; ++i) {
      if (done_[i]) continue;
      sink_.nodeFailed(plan_.nodes[i].id, "error",
                       {Status::Error(Phase::Execute, "internal", "调度器没有排到这个节点")}, -1);
      anyError_ = true;
      failed_ += 1;
    }
  }

  bool anyError() const { return anyError_; }
  bool sawCancel() const { return sawCancel_; }
  std::size_t failedCount() const { return failed_; }

 private:
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

      {
        std::lock_guard<std::mutex> lock(mu_);
        verdict_[index] = v;
        done_[index] = 1;
        inflight_ -= 1;
        if (v != Verdict::Ok) {
          failed_ += 1;
          if (v == Verdict::Failed) anyError_ = true;
          else sawCancel_ = true;
        }
        for (int d : plan_.nodes[index].downstream) {
          const auto di = static_cast<std::size_t>(d);
          if (v != Verdict::Ok && !poisoned_[di]) {
            poisoned_[di] = 1;
            poisonCancelled_[di] = v == Verdict::Cancelled ? 1 : 0;
            blockedBy_[di] = plan_.nodes[index].id;
          }
          if (--remaining_[di] == 0) ready_.push(di);
        }
      }
      cv_.notify_all();
    }
  }

  bool poisonOf(std::size_t i, bool& byCancel, std::string& blockedBy) const {
    std::lock_guard<std::mutex> lock(mu_);
    if (!poisoned_[i]) return false;
    byCancel = poisonCancelled_[i] != 0;
    blockedBy = blockedBy_[i];
    return true;
  }

  Verdict execute(std::size_t i) {
    PlanNode& node = plan_.nodes[i];
    sink_.nodeState(node.id, "pending");

    if (cancelled_.load(std::memory_order_relaxed)) {
      sink_.nodeFailed(node.id, "cancelled",
                       {Status::Error(Phase::Execute, "cancelled", "运行已取消")}, -1);
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
      sink_.nodeFailed(node.id, "error", errors, -1);
      return Verdict::Failed;
    }

    // 上游失败 → cancelled + upstream_failed。严格不用 skipped：
    // 后者保留给缓存命中，混用的话用户分不清「没跑」和「不用跑」。
    bool byCancel = false;
    std::string blockedBy;
    if (poisonOf(i, byCancel, blockedBy)) {
      const Status s =
          byCancel ? Status::Error(Phase::Execute, "cancelled", "运行已取消")
                   : Status::Error(Phase::Execute, "upstream_failed",
                                   "上游节点 " + blockedBy + " 失败，未执行");
      sink_.nodeFailed(node.id, "cancelled", {s}, -1);
      return byCancel ? Verdict::Cancelled : Verdict::Failed;
    }

    const auto nodeStart = Clock::now();

    // 缓存命中：直接把仓里那份挂到本次运行，不调 compute（ADR-0007）。
    // 没有输出端口的算子（io.save_pcd 这类纯副作用）永远不复用。
    if (!options_.noReuse && !node.bypass && node.op->capabilities.deterministic &&
        !node.op->outputs.empty()) {
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
    if (const Status bad = collectInputs(node, inputValues); !bad.ok) {
      sink_.nodeFailed(node.id, "error", {bad}, msSince(nodeStart));
      return Verdict::Failed;
    }

    std::unordered_map<std::string, Data> outputValues;
    bool bypassed = false;
    Status status = Status::Ok();
    if (node.bypass) {
      bypassed = true;
      passThrough(node, inputValues, outputValues);
    } else {
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
    }

    const double durationMs = msSince(nodeStart);

    if (cancelled_.load(std::memory_order_relaxed)) {
      sink_.nodeFailed(node.id, "cancelled",
                       {Status::Error(Phase::Execute, "cancelled", "运行已取消")}, durationMs);
      return Verdict::Cancelled;
    }
    if (!status.ok) {
      sink_.nodeFailed(node.id, "error", {status}, durationMs);
      return Verdict::Failed;
    }

    if (const Status contract = checkOutputs(node, outputValues, bypassed); !contract.ok) {
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
      store_.put(options_.runId, node.id, p.name, node.cacheKey, d);
    }

    sink_.nodeFinished(node.id, bypassed ? "skipped" : "done", durationMs, primaryElements,
                       totalBytes, infos, /*cached=*/false, bypassed);
    return Verdict::Ok;
  }

  Status collectInputs(const PlanNode& node, std::unordered_map<std::string, Data>& values) const {
    for (const InputBinding& b : node.inputs) {
      if (b.fromNode < 0) continue;
      const PlanNode& up = plan_.nodes[static_cast<std::size_t>(b.fromNode)];
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
        if (expected != Data::Kind::None && d.kind() != expected) {
          return Status::Error(Phase::Execute, "type_mismatch",
                               std::string("输入端口 '") + b.port + "' 需要 " + want +
                                   "，实际收到 " + d.typeName(),
                               {}, b.port);
        }
      }
      values[b.port] = std::move(d);
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
        return Status::Error(Phase::Execute, "internal",
                             "算子没有写输出端口 '" + p.name + "'", {}, p.name);
      }
      const std::string& want = effectiveType(node.outputTypes, p.name, p.type);
      const Data::Kind expected = kindFromTypeName(want);
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
  int workers_ = 1;
  int threadBudget_ = 1;
  std::size_t n_ = 0;

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::priority_queue<std::size_t, std::vector<std::size_t>, ByLevelThenIndex> ready_{
      ByLevelThenIndex{&plan_}};
  std::vector<int> remaining_;
  std::vector<Verdict> verdict_;
  std::vector<char> done_;
  std::vector<char> poisoned_;
  std::vector<char> poisonCancelled_;
  std::vector<std::string> blockedBy_;
  int inflight_ = 0;
  bool anyError_ = false;
  bool sawCancel_ = false;
  std::size_t failed_ = 0;
};

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

  const bool parsed = prepareGraph(graphJson_, raw, diags);
  if (parsed) {
    BuildOptions build;
    build.runId = options_.runId;
    build.baseDir = options_.baseDir;
    build.targets = options_.targets;
    build.cacheNamespace = previewNamespace(options_);
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
    sink.runFinished("error", msSince(t0), &first);
    return;
  }

  ResultStore& store = ResultStore::instance();
  if (options_.cacheBudgetBytes) store.setBudget(options_.cacheBudgetBytes);

  // 本次计划涉及的键全部钉住：正在算的那份不能被 LRU 从下游脚下抽走。
  std::vector<std::string> keys;
  keys.reserve(plan.nodes.size());
  for (const auto& n : plan.nodes) {
    if (!n.cacheKey.empty()) keys.push_back(n.cacheKey);
  }
  CachePin pinned(store, std::move(keys));

  Scheduler scheduler(plan, sink, cancelled_, options_, store, workers);
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
  if (scheduler.sawCancel() || cancelled_.load(std::memory_order_relaxed)) {
    Status s = Status::Error(Phase::Execute, "cancelled", "运行已取消");
    sink.runFinished("cancelled", total, &s);
  } else if (scheduler.anyError()) {
    Status s = Status::Error(Phase::Execute, "internal",
                             std::to_string(scheduler.failedCount()) + " 个节点未能完成");
    sink.runFinished("error", total, &s);
  } else {
    sink.runFinished("ok", total, nullptr);
  }
}

// ------------------------------------------------------------------ validate

bool prepareGraph(const std::string& graphJson, RawGraph& out, Diagnostics& diags) {
  RawGraph parsed;
  if (!parseGraph(graphJson, parsed, diags)) return false;
  if (!expandGraph(parsed, out, diags)) return false;
  return true;
}

std::string validateGraphJson(const std::string& graphJson,
                              const std::filesystem::path& baseDir) {
  Diagnostics diags;
  RawGraph raw;
  if (prepareGraph(graphJson, raw, diags)) {
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
                          const std::vector<std::string>& targets) {
  Diagnostics diags;
  RawGraph raw;
  Plan plan;
  if (!prepareGraph(graphJson, raw, diags)) return diags.toJson();

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
    w.endObject();
  }
  w.endArray();
  return w.str();
}

}  // namespace lyflow::exec
