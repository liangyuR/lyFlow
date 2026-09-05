#include "exec/executor.h"

#include <chrono>
#include <ctime>
#include <unordered_map>
#include <unordered_set>

#include "exec/graph.h"
#include "exec/plan.h"
#include "exec/result_store.h"
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

/// 事件序列化 + 回调。seq 单调递增，前端据此检测丢包与乱序。
///
/// 事件全部在**同一个工作线程**上产生，所以这里不用加锁；
/// 一旦将来并行执行（M3），入口就要串行化。
class EventSink {
 public:
  EventSink(std::string runId, lyflow_event_cb cb, void* user)
      : runId_(std::move(runId)), cb_(cb), user_(user) {}

  void runStarted(const Plan& plan, const std::vector<std::string>& targets) {
    JsonWriter w;
    begin(w, "run_started");
    w.field("nodeCount", static_cast<std::int64_t>(plan.nodes.size()));
    w.key("plan");
    w.beginArray();
    for (const auto& n : plan.nodes) w.value(n.id);
    w.endArray();
    w.key("targets");
    w.beginArray();
    for (const auto& t : targets) w.value(t);
    w.endArray();
    // M3 的「将重算 N 个节点」提示与精确 stale 标记全靠这一段。
    // 现在就发出去，前端可以先忽略，接口不用再改一次。
    w.key("nodes");
    w.beginArray();
    for (const auto& n : plan.nodes) {
      w.beginObject();
      w.field("id", n.id);
      w.field("cacheKey", n.cacheKey);
      w.field("level", static_cast<std::int64_t>(n.level));
      w.endObject();
    }
    w.endArray();
    end(w);
  }

  void nodeState(const std::string& nodeId, const char* state) {
    JsonWriter w;
    begin(w, "node_state");
    w.field("nodeId", nodeId);
    w.field("state", std::string(state));
    end(w);
  }

  void nodeDone(const std::string& nodeId, double durationMs, std::size_t elementCount,
                std::size_t byteSize, const std::vector<OutputInfo>& outputs) {
    JsonWriter w;
    begin(w, "node_state");
    w.field("nodeId", nodeId);
    w.field("state", std::string("done"));
    w.field("durationMs", durationMs);
    w.key("stats");
    w.beginObject();
    w.field("elementCount", static_cast<std::int64_t>(elementCount));
    w.field("byteSize", static_cast<std::int64_t>(byteSize));
    w.key("outputs");
    w.beginArray();
    for (const auto& o : outputs) {
      w.beginObject();
      w.field("port", o.port);
      w.field("type", o.type);
      w.field("elementCount", static_cast<std::int64_t>(o.elementCount));
      w.endObject();
    }
    w.endArray();
    w.endObject();
    end(w);
  }

  /// 失败/取消。errors 是**全部**诊断，error 是 errors[0] 的快捷方式 ——
  /// 前端老代码只看 error 也不会瞎，新代码一次标出所有红框（D5）。
  void nodeFailed(const std::string& nodeId, const char* state,
                  const std::vector<Status>& errors, double durationMs) {
    JsonWriter w;
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
    begin(w, "node_progress");
    w.field("nodeId", nodeId);
    w.field("progress", static_cast<double>(ratio < 0 ? 0.0f : (ratio > 1 ? 1.0f : ratio)));
    w.fieldIfSet("message", message);
    end(w);
  }

  void log(const char* level, const std::string& nodeId, const std::string& message) {
    JsonWriter w;
    begin(w, "log");
    w.field("level", std::string(level));
    w.fieldIfSet("nodeId", nodeId);
    w.field("message", message);
    end(w);
  }

  void runFinished(const char* status, double durationMs, const Status* error) {
    JsonWriter w;
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

  std::string runId_;
  lyflow_event_cb cb_ = nullptr;
  void* user_ = nullptr;
  std::int64_t seq_ = 0;
};

/// 算子看到的 ExecContext。
class NodeContext final : public ExecContext {
 public:
  NodeContext(EventSink& sink, const std::atomic<bool>& cancelled, std::string nodeId,
              const std::filesystem::path& baseDir)
      : sink_(sink), cancelled_(cancelled), nodeId_(std::move(nodeId)), baseDir_(baseDir) {}

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

 private:
  EventSink& sink_;
  const std::atomic<bool>& cancelled_;
  std::string nodeId_;
  const std::filesystem::path& baseDir_;
  Clock::time_point lastProgress_{};
};

}  // namespace

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
  // 必须自己加锁：两个线程同时 join 是数据竞争，而且对已经 join 过的 thread
  // 再 join 一次是 UD。Rust 侧的 RunHandle 也有一把锁，但 C ABI 不能把正确性
  // 押在「调用方一定加了锁」上 —— headless CLI（M4）就是另一个调用方。
  std::lock_guard<std::mutex> lock(joinMutex_);
  if (joined_) return;
  if (thread_.joinable()) thread_.join();
  joined_ = true;
}

void Run::work() {
  // c_api.h 的承诺是「异常绝不跨 ABI」，而这个函数是一个 std::thread 的函数体：
  // 从这里逃出去的异常直接 std::terminate，整个 app 无声无息地没了。
  // compute() 已经单独兜过一层，这一层兜的是 parse / buildPlan / externalKey 钩子 /
  // 事件序列化 / 结果仓写入 —— 它们同样可能抛 bad_alloc。
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

  const bool parsed = parseGraph(graphJson_, raw, diags);
  if (parsed) {
    BuildOptions build;
    build.runId = options_.runId;
    build.baseDir = options_.baseDir;
    build.targets = options_.targets;
    buildPlan(ensureRegistry(), raw, build, plan, diags);
  }

  sink.runStarted(plan, options_.targets);

  // warning 走 log 通道：不阻断执行，但用户必须看得见。
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
  std::unordered_set<std::string> failed;  // 自身失败或上游失败的节点
  bool anyError = false;
  bool wasCancelled = false;

  for (std::size_t i = 0; i < plan.nodes.size(); ++i) {
    PlanNode& node = plan.nodes[i];

    if (cancelled_.load(std::memory_order_relaxed)) {
      wasCancelled = true;
      for (std::size_t j = i; j < plan.nodes.size(); ++j) {
        sink.nodeFailed(plan.nodes[j].id, "cancelled",
                        {Status::Error(Phase::Execute, "cancelled", "运行已取消")}, -1);
      }
      break;
    }

    sink.nodeState(node.id, "pending");

    // 自身校验没过：报它全部的诊断，然后当作失败往下传。
    // 注意其余节点照常跑 —— 一次看到所有能看到的（D5 的执行期对应物）。
    if (!node.valid || !node.op) {
      std::vector<Status> errors;
      for (const auto& d : node.errors) errors.push_back(d.status);
      if (errors.empty()) {
        errors.push_back(Status::Error(Phase::Validate, "internal", "节点校验未通过"));
      }
      sink.nodeFailed(node.id, "error", errors, -1);
      failed.insert(node.id);
      anyError = true;
      continue;
    }

    // 上游失败 → cancelled + upstream_failed。
    // 严格不用 skipped：skipped 保留给缓存命中（M3），两者混用的话
    // 用户永远分不清「没跑」和「不用跑」。
    std::string blockedBy;
    for (const InputBinding& b : node.inputs) {
      if (b.fromNode < 0) continue;
      const std::string& upstream = plan.nodes[static_cast<std::size_t>(b.fromNode)].id;
      if (failed.count(upstream)) { blockedBy = upstream; break; }
    }
    if (!blockedBy.empty()) {
      sink.nodeFailed(node.id, "cancelled",
                      {Status::Error(Phase::Execute, "upstream_failed",
                                     "上游节点 " + blockedBy + " 失败，未执行")},
                      -1);
      failed.insert(node.id);
      continue;
    }

    sink.nodeState(node.id, "running");
    const auto nodeStart = Clock::now();

    std::unordered_map<std::string, Data> inputValues;
    bool inputsOk = true;
    for (const InputBinding& b : node.inputs) {
      if (b.fromNode < 0) continue;
      Data d;
      const std::string& upstream = plan.nodes[static_cast<std::size_t>(b.fromNode)].id;
      if (!store.get(options_.runId, upstream, b.fromPort, d)) {
        sink.nodeFailed(node.id, "error",
                        {Status::Error(Phase::Execute, "internal",
                                       "上游 " + upstream + "." + b.fromPort + " 没有产出结果",
                                       {}, b.port)},
                        msSince(nodeStart));
        inputsOk = false;
        break;
      }
      // 端口**声明的**类型在编译期查过了，但真正流过来的 Data 是什么 Kind
      // 还没人查。Any 类型（Reroute、Debug View）会让声明层面的检查全部通过，
      // 而算子里那句 `*inputs.get("cloud").asCloud()` 会对着 nullptr 解引用 ——
      // 在 MSVC 上那是 SEH，下面那个 catch(...) 根本兜不住。
      const Port* declared = nullptr;
      for (const Port& p : node.op->inputs) {
        if (p.name == b.port) { declared = &p; break; }
      }
      const Data::Kind expected =
          declared ? kindFromTypeName(declared->type) : Data::Kind::None;
      if (expected != Data::Kind::None && d.kind() != expected) {
        sink.nodeFailed(node.id, "error",
                        {Status::Error(Phase::Execute, "type_mismatch",
                                       std::string("输入端口 '") + b.port + "' 需要 " +
                                           declared->type + "，实际收到 " + d.typeName(),
                                       {}, b.port)},
                        msSince(nodeStart));
        inputsOk = false;
        break;
      }

      inputValues[b.port] = std::move(d);
    }
    if (!inputsOk) {
      failed.insert(node.id);
      anyError = true;
      continue;
    }

    std::unordered_map<std::string, Data> outputValues;
    Inputs inputs(inputValues);
    Outputs outputs(outputValues);
    ParamView params(node.params, options_.baseDir);
    NodeContext ctx(sink, cancelled_, node.id, options_.baseDir);

    Status status;
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

    const double durationMs = msSince(nodeStart);

    if (cancelled_.load(std::memory_order_relaxed)) {
      wasCancelled = true;
      sink.nodeFailed(node.id, "cancelled",
                      {Status::Error(Phase::Execute, "cancelled", "运行已取消")}, durationMs);
      for (std::size_t j = i + 1; j < plan.nodes.size(); ++j) {
        sink.nodeFailed(plan.nodes[j].id, "cancelled",
                        {Status::Error(Phase::Execute, "cancelled", "运行已取消")}, -1);
      }
      break;
    }

    if (!status.ok) {
      sink.nodeFailed(node.id, "error", {status}, durationMs);
      failed.insert(node.id);
      anyError = true;
      continue;
    }

    // 算子必须把声明过的输出端口都填上。少填了是算子的 bug，
    // 早点炸在这里，好过让下游收到一个空 Data 再报「上游没有产出」。
    Status contract = Status::Ok();
    for (const Port& p : node.op->outputs) {
      auto it = outputValues.find(p.name);
      if (it == outputValues.end() || it->second.empty()) {
        contract = Status::Error(Phase::Execute, "internal",
                                 "算子没有写输出端口 '" + p.name + "'", {}, p.name);
        break;
      }
      const Data::Kind expected = kindFromTypeName(p.type);
      if (expected != Data::Kind::None && it->second.kind() != expected) {
        contract = Status::Error(Phase::Execute, "internal",
                                 std::string("输出端口 '") + p.name + "' 声明为 " + p.type +
                                     "，实际是 " + it->second.typeName(),
                                 {}, p.name);
        break;
      }
      if (const PointCloud* c = it->second.asCloud()) {
        if (!c->channelsConsistent()) {
          contract = Status::Error(Phase::Execute, "internal",
                                   std::string("输出端口 '") + p.name +
                                       "' 的点云通道长度不一致（intensity/normals/rgb "
                                       "必须为空或与点数对齐）",
                                   {}, p.name);
          break;
        }
      }
    }
    if (!contract.ok) {
      sink.nodeFailed(node.id, "error", {contract}, durationMs);
      failed.insert(node.id);
      anyError = true;
      continue;
    }

    std::size_t totalBytes = 0;
    std::size_t primaryElements = 0;
    std::vector<OutputInfo> infos;
    bool first = true;
    for (const Port& p : node.op->outputs) {
      Data& d = outputValues[p.name];
      totalBytes += d.byteSize();
      if (first) {
        primaryElements = d.elementCount();
        first = false;
      }
      infos.push_back(OutputInfo{p.name, d.typeName(), d.elementCount(), d.byteSize()});
      store.put(options_.runId, node.id, p.name, node.cacheKey, d);
    }

    sink.nodeDone(node.id, durationMs, primaryElements, totalBytes, infos);
  }

  const double total = msSince(t0);
  if (wasCancelled) {
    Status s = Status::Error(Phase::Execute, "cancelled", "运行已取消");
    sink.runFinished("cancelled", total, &s);
  } else if (anyError || !failed.empty()) {
    Status s = Status::Error(Phase::Execute, "internal",
                             std::to_string(failed.size()) + " 个节点未能完成");
    sink.runFinished("error", total, &s);
  } else {
    sink.runFinished("ok", total, nullptr);
  }
}

// ------------------------------------------------------------------ validate

std::string validateGraphJson(const std::string& graphJson,
                              const std::filesystem::path& baseDir) {
  Diagnostics diags;
  RawGraph raw;
  if (parseGraph(graphJson, raw, diags)) {
    Plan plan;
    BuildOptions build;
    build.runId = "validate";
    build.baseDir = baseDir;
    buildPlan(ensureRegistry(), raw, build, plan, diags);
  }
  return diags.toJson();
}

}  // namespace lyflow::exec
