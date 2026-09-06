#pragma once
// 测试用的小工具：拼 GraphDoc、跑一次 run、把事件收成数组。
// 刻意不读任何数据文件，点云一律由 gen.synthetic 现生成（仓库不进二进制数据）。
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "exec/executor.h"
#include "exec/result_store.h"
#include "lyflow/registry.h"

namespace lyflow::test {

using Json = nlohmann::json;

/// 一个节点的描述，拼图时用。
struct N {
  std::string id;
  std::string op;
  Json params = Json::object();
};

/// from "node.port" -> to "node.port"
struct E {
  std::string from;
  std::string to;
};

inline Json makeGraph(const std::vector<N>& nodes, const std::vector<E>& edges) {
  Json doc;
  doc["schemaVersion"] = 1;
  doc["id"] = "01TESTTESTTESTTESTTESTTEST";
  doc["nodes"] = Json::array();
  doc["edges"] = Json::array();
  for (const N& n : nodes) {
    Json jn;
    jn["id"] = n.id;
    jn["op"] = n.op;
    jn["params"] = n.params;
    doc["nodes"].push_back(std::move(jn));
  }
  int i = 0;
  for (const E& e : edges) {
    const auto dot1 = e.from.find('.');
    const auto dot2 = e.to.find('.');
    Json je;
    je["id"] = "e" + std::to_string(i++);
    je["from"] = Json{{"node", e.from.substr(0, dot1)}, {"port", e.from.substr(dot1 + 1)}};
    je["to"] = Json{{"node", e.to.substr(0, dot2)}, {"port", e.to.substr(dot2 + 1)}};
    doc["edges"].push_back(std::move(je));
  }
  return doc;
}

/// 一次运行收集到的全部事件。
struct RunLog {
  std::string runId;
  std::vector<Json> events;

  std::vector<Json> ofKind(const std::string& kind) const {
    std::vector<Json> out;
    for (const Json& e : events) {
      if (e.value("kind", "") == kind) out.push_back(e);
    }
    return out;
  }

  /// 某节点最后一次 node_state 的 state。没有事件时返回空串。
  std::string finalState(const std::string& nodeId) const {
    std::string state;
    for (const Json& e : events) {
      if (e.value("kind", "") == "node_state" && e.value("nodeId", "") == nodeId) {
        state = e.value("state", "");
      }
    }
    return state;
  }

  Json nodeEvent(const std::string& nodeId, const std::string& state) const {
    for (const Json& e : events) {
      if (e.value("kind", "") == "node_state" && e.value("nodeId", "") == nodeId &&
          e.value("state", "") == state) {
        return e;
      }
    }
    return Json::object();
  }

  std::string runStatus() const {
    const auto finished = ofKind("run_finished");
    return finished.empty() ? std::string{} : finished.back().value("status", "");
  }

  /// seq 从 0 开始且连续。丢一条事件前端就会漏掉一个状态，所以每个测试都值得查一次。
  bool seqIsDense() const {
    for (std::size_t i = 0; i < events.size(); ++i) {
      if (events[i].value("seq", -1) != static_cast<int>(i)) return false;
    }
    return true;
  }
};

namespace detail {

inline void collect(const char* json, void* user) {
  auto* log = static_cast<RunLog*>(user);
  log->events.push_back(Json::parse(json));
}

}  // namespace detail

/// 跑一张图的完整上下文。结果仓的索引挂在 Run 上（析构即 freeRun），
/// 所以想断言输出的测试必须让 Run 活着 —— Run 和事件日志捆在同一个对象里。
class Session {
 public:
  /// keepCache=false 时先清空结果仓。缓存是进程级的，不清的话「第二个用同一张图的
  /// 测试」会拿到 skipped 而不是 done —— 那是真实行为，但会让断言测的是运行顺序。
  Session(const Json& doc, std::filesystem::path baseDir = {},
          std::vector<std::string> targets = {}, bool keepCache = false,
          int maxParallel = 0, bool noReuse = false) {
    static std::atomic<int> counter{0};
    if (!keepCache) exec::ResultStore::instance().clear();
    log_.runId = "test-run-" + std::to_string(counter.fetch_add(1));
    exec::RunOptions options;
    options.runId = log_.runId;
    options.baseDir = std::move(baseDir);
    options.targets = std::move(targets);
    options.maxParallel = maxParallel;
    options.noReuse = noReuse;
    run_ = std::make_unique<exec::Run>(doc.dump(), options, &detail::collect, &log_);
  }

  exec::Run& run() { return *run_; }
  /// 等执行结束。返回后事件不会再增加。
  RunLog& wait() {
    run_->join();
    return log_;
  }
  const std::string& runId() const { return log_.runId; }

 private:
  RunLog log_;
  std::unique_ptr<exec::Run> run_;
};

/// 只关心事件、不关心结果时的快捷方式。
inline RunLog runGraph(const Json& doc, const std::filesystem::path& baseDir = {},
                       const std::vector<std::string>& targets = {}) {
  Session s(doc, baseDir, targets);
  return s.wait();
}

/// 不清缓存地跑一次。缓存复用的测试要靠它跑第二遍。
inline RunLog runGraphCached(const Json& doc, const std::filesystem::path& baseDir = {}) {
  Session s(doc, baseDir, {}, /*keepCache=*/true);
  return s.wait();
}

/// 结果仓照旧，但本次运行不吃缓存（CLI 的 --no-cache）。
inline RunLog runGraphNoReuse(const Json& doc, const std::filesystem::path& baseDir = {}) {
  Session s(doc, baseDir, {}, /*keepCache=*/true, /*maxParallel=*/0, /*noReuse=*/true);
  return s.wait();
}

}  // namespace lyflow::test
