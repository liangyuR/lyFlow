// 子图（ADR-0010）与 live preview（ADR-0011）的 C++ 侧行为。
// 关键断言：展开成平图之后，执行器/缓存/事件一律看不见子图这回事（F1）。
#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "exec/executor.h"
#include "exec/library.h"
#include "exec/plan.h"
#include "exec/result_store.h"
#include "exec/subgraph.h"
#include "helpers.h"
#include "lyflow/registry.h"
#include "test_ops.h"

using namespace lyflow;
using namespace lyflow::test;

namespace {

/// 三节点直链的平版本：生成 → 抽稀 → 直通。
Json flatGraph(int seed) {
  Json doc;
  doc["schemaVersion"] = 1;
  doc["id"] = "01FLAT";
  doc["nodes"] = Json::array({
      Json{{"id", "g"}, {"op", "gen.synthetic"}, {"params", {{"pointCount", 20000}, {"seed", seed}}}},
      Json{{"id", "v"},
           {"op", "test.thin"},
           {"params", {{"leaf", Json::array({0.02, 0.02, 0.02})}}}},
      Json{{"id", "p"}, {"op", "test.thin"}, {"params", {{"leaf", Json::array({0.01, 0.01, 0.01})}}}},
  });
  doc["edges"] = Json::array({
      Json{{"id", "e1"}, {"from", {{"node", "g"}, {"port", "cloud"}}},
           {"to", {{"node", "v"}, {"port", "cloud"}}}},
      Json{{"id", "e2"}, {"from", {{"node", "v"}, {"port", "cloud"}}},
           {"to", {{"node", "p"}, {"port", "cloud"}}}},
  });
  return doc;
}

/// 同一条链，中间两个节点收进一个子图。参数 leaf 提升成子图的对外参数。
Json subgraphGraph(int seed, double leaf = 0.02) {
  Json doc;
  doc["schemaVersion"] = 1;
  doc["id"] = "01SUB";
  doc["nodes"] = Json::array({
      Json{{"id", "g"}, {"op", "gen.synthetic"}, {"params", {{"pointCount", 20000}, {"seed", seed}}}},
      Json{{"id", "s"},
           {"op", "sub:clean"},
           {"params", {{"leaf", Json::array({leaf, leaf, leaf})}}}},
  });
  doc["edges"] = Json::array({
      Json{{"id", "e1"}, {"from", {{"node", "g"}, {"port", "cloud"}}},
           {"to", {{"node", "s"}, {"port", "cloud"}}}},
  });
  doc["subgraphs"] = Json::object();
  doc["subgraphs"]["clean"] = Json{
      {"name", "去噪"},
      {"nodes", Json::array({
                    Json{{"id", "v"}, {"op", "test.thin"}},
                    Json{{"id", "p"},
                         {"op", "test.thin"},
                         {"params", {{"leaf", Json::array({0.01, 0.01, 0.01})}}}},
                })},
      {"edges", Json::array({Json{{"id", "ei"},
                                  {"from", {{"node", "v"}, {"port", "cloud"}}},
                                  {"to", {{"node", "p"}, {"port", "cloud"}}}}})},
      {"inputs", Json::array({Json{{"name", "cloud"},
                                   {"type", "PointCloud"},
                                   {"to", Json::array({Json{{"node", "v"}, {"port", "cloud"}}})}}})},
      {"outputs", Json::array({Json{{"name", "cloud"},
                                    {"type", "PointCloud"},
                                    {"from", {{"node", "p"}, {"port", "cloud"}}}}})},
      {"params", Json::array({Json{{"name", "leaf"},
                                   {"type", "vec3f"},
                                   {"default", Json::array({0.02, 0.02, 0.02})},
                                   {"min", 0.0001},
                                   {"binds", Json::array({Json{{"node", "v"}, {"param", "leaf"}}})}}})},
  };
  return doc;
}

std::size_t elementCountOf(const RunLog& log, const std::string& nodeId) {
  const Json e = log.nodeEvent(nodeId, "done");
  if (e.contains("stats")) return e["stats"].value("elementCount", 0);
  const Json s = log.nodeEvent(nodeId, "skipped");
  return s.contains("stats") ? s["stats"].value("elementCount", 0) : 0;
}

std::vector<std::string> planIds(const RunLog& log) {
  std::vector<std::string> out;
  // ofKind 返回临时 vector：range-for 不会延长它的寿命，必须先落地成局部变量
  const std::vector<Json> started = log.ofKind("run_started");
  if (started.empty()) return out;
  for (const Json& n : started.front()["nodes"]) out.push_back(n["id"].get<std::string>());
  return out;
}

}  // namespace

TEST_CASE("子图展开：结果与平图逐点一致") {
  ensureTestOps();
  const RunLog flat = runGraph(flatGraph(11));
  REQUIRE(flat.runStatus() == "ok");

  const RunLog sub = runGraph(subgraphGraph(11));
  REQUIRE(sub.runStatus() == "ok");

  // 展开后的 id 是路径（F2），子图这个节点本身不在计划里
  const auto ids = planIds(sub);
  CHECK(std::find(ids.begin(), ids.end(), "s") == ids.end());
  CHECK(std::find(ids.begin(), ids.end(), "s/v") != ids.end());
  CHECK(std::find(ids.begin(), ids.end(), "s/p") != ids.end());

  CHECK(elementCountOf(sub, "s/v") == elementCountOf(flat, "v"));
  CHECK(elementCountOf(sub, "s/p") == elementCountOf(flat, "p"));
  CHECK(elementCountOf(sub, "s/p") > 0);
}

TEST_CASE("子图参数：外参覆盖内参，一个外参可绑多个内参") {
  ensureTestOps();
  const RunLog coarse = runGraph(subgraphGraph(12, 0.05));
  const RunLog fine = runGraph(subgraphGraph(12, 0.005));
  REQUIRE(coarse.runStatus() == "ok");
  REQUIRE(fine.runStatus() == "ok");
  CHECK(elementCountOf(coarse, "s/v") < elementCountOf(fine, "s/v"));

  // 一个外参绑两个内参：两级抽稀都跟着它走
  Json doc = subgraphGraph(13, 0.04);
  doc["subgraphs"]["clean"]["nodes"].push_back(
      Json{{"id", "v2"}, {"op", "test.thin"}});
  doc["subgraphs"]["clean"]["edges"] = Json::array({
      Json{{"id", "ei"}, {"from", {{"node", "v"}, {"port", "cloud"}}},
           {"to", {{"node", "v2"}, {"port", "cloud"}}}},
      Json{{"id", "ej"}, {"from", {{"node", "v2"}, {"port", "cloud"}}},
           {"to", {{"node", "p"}, {"port", "cloud"}}}},
  });
  doc["subgraphs"]["clean"]["params"][0]["binds"].push_back(
      Json{{"node", "v2"}, {"param", "leaf"}});

  const RunLog two = runGraph(doc);
  REQUIRE(two.runStatus() == "ok");
  // leaf=0.04 是四取一：两级都真的拿到了外参，点数就该连着除两次 4
  CHECK(elementCountOf(two, "s/v") == 5000);
  CHECK(elementCountOf(two, "s/v2") == 1250);
}

TEST_CASE("子图嵌套两层：cacheKey 稳定，重跑全部 skipped") {
  ensureTestOps();
  Json doc = subgraphGraph(14);
  // outer 只包着 clean 这一个子图节点
  doc["subgraphs"]["outer"] = Json{
      {"name", "外层"},
      {"nodes", Json::array({Json{{"id", "inner"}, {"op", "sub:clean"}}})},
      {"edges", Json::array()},
      {"inputs", Json::array({Json{{"name", "cloud"},
                                   {"type", "PointCloud"},
                                   {"to", Json::array({Json{{"node", "inner"}, {"port", "cloud"}}})}}})},
      {"outputs", Json::array({Json{{"name", "cloud"},
                                    {"type", "PointCloud"},
                                    {"from", {{"node", "inner"}, {"port", "cloud"}}}}})},
      {"params", Json::array()},
  };
  doc["nodes"][1] = Json{{"id", "s"}, {"op", "sub:outer"}};

  exec::ResultStore::instance().clear();
  const RunLog first = runGraphCached(doc);
  REQUIRE(first.runStatus() == "ok");
  const auto ids = planIds(first);
  CHECK(std::find(ids.begin(), ids.end(), "s/inner/v") != ids.end());
  CHECK(std::find(ids.begin(), ids.end(), "s/inner/p") != ids.end());

  const RunLog second = runGraphCached(doc);
  REQUIRE(second.runStatus() == "ok");
  for (const std::string& id : planIds(second)) {
    CHECK_MESSAGE(second.finalState(id) == "skipped", id);
  }
  // 两次编译的 cacheKey 必须逐个相同，否则 stale 标记会自己闪
  const Json a = first.ofKind("run_started").front()["nodes"];
  const Json b = second.ofKind("run_started").front()["nodes"];
  CHECK(a == b);
}

TEST_CASE("prepareGraph 挡下的子图错误：递归引用、不存在的定义、未知参数") {
  ensureTestOps();
  struct Case {
    const char* name;
    Json doc;
    const char* code;
    const char* nodeId;     // 空 = 不看
    const char* paramPath;  // 空 = 不看
  };
  Json recursive = subgraphGraph(15);
  recursive["subgraphs"]["clean"]["nodes"].push_back(Json{{"id", "self"}, {"op", "sub:clean"}});
  Json missingDef = flatGraph(16);
  missingDef["nodes"].push_back(Json{{"id", "x"}, {"op", "sub:nope"}});
  Json unknownParam = subgraphGraph(19);
  unknownParam["nodes"][1]["params"]["nosuch"] = 1;
  const std::vector<Case> cases = {
      {"子图递归引用被拒", recursive, "recursive_subgraph", "", ""},
      {"子图引用了不存在的定义 → unknown_op", missingDef, "unknown_op", "x", ""},
      {"子图的未知参数会被报出来", unknownParam, "unknown_param", "", "nosuch"},
  };
  for (const Case& c : cases) {
    CAPTURE(c.name);
    Diagnostics diags;
    exec::RawGraph raw;
    CHECK_FALSE(exec::prepareGraph(c.doc.dump(), raw, diags));
    bool found = false;
    for (const auto& d : diags.items()) {
      if (d.status.code != c.code) continue;
      if (*c.nodeId && d.nodeId != c.nodeId) continue;
      if (*c.paramPath && d.status.paramPath != c.paramPath) continue;
      found = true;
    }
    CHECK(found);
  }

  // 递归引用走完整条运行路径也一样：整图级失败，不是崩溃
  const RunLog log = runGraph(recursive);
  CHECK(log.runStatus() == "error");
}

TEST_CASE("子图节点静音：整棵子树透传") {
  ensureTestOps();
  Json doc = subgraphGraph(17);
  doc["nodes"][1]["bypass"] = true;
  const RunLog log = runGraph(doc);
  REQUIRE(log.runStatus() == "ok");
  CHECK(log.finalState("s/v") == "skipped");
  CHECK(log.finalState("s/p") == "skipped");
  CHECK(log.nodeEvent("s/v", "skipped")["stats"]["bypassed"] == true);
  // 透传之后下游拿到的就是源头那份
  CHECK(elementCountOf(log, "s/p") == 20000);
}

TEST_CASE("Run to node 的目标可以是子图节点：按路径前缀收编整棵子树") {
  ensureTestOps();
  Json doc = subgraphGraph(18);
  const RunLog log = runGraph(doc, {}, {"s"});
  REQUIRE(log.runStatus() == "ok");
  const auto ids = planIds(log);
  CHECK(std::find(ids.begin(), ids.end(), "s/v") != ids.end());
  CHECK(std::find(ids.begin(), ids.end(), "s/p") != ids.end());
  CHECK(std::find(ids.begin(), ids.end(), "g") != ids.end());
}

TEST_CASE("库目录：*.lyflow-op.json 注册成 lib.<id>，和内置算子无差别") {
  ensureTestOps();
  const auto dir = std::filesystem::temp_directory_path() / "lyflow-lib-test";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);

  Json def = subgraphGraph(21)["subgraphs"]["clean"];
  def["id"] = "clean";
  def["category"] = "Cleanup";
  {
    std::ofstream out(dir / "clean.lyflow-op.json");
    out << def.dump(2);
  }

  const auto problems = exec::Library::instance().setDirs({dir});
  const std::string firstProblem = problems.empty() ? std::string() : problems.front();
  CHECK_MESSAGE(problems.empty(), firstProblem);
  CHECK(exec::Library::instance().size() == 1);

  const OperatorDesc* op = ensureRegistry().find("lib.clean");
  REQUIRE(op != nullptr);
  CHECK(op->category == "Library/Cleanup");
  // 合成出来的 OperatorDesc 通得过注册表自检，外参形状照子图定义
  CHECK(op->inputs.size() == 1);
  CHECK(op->outputs.size() == 1);
  REQUIRE(op->params.size() == 1);
  CHECK(op->params[0].name == "leaf");
  CHECK(std::string(toString(op->params[0].type)) == "vec3f");
  CHECK(ensureRegistry().validate().empty());

  Json doc = flatGraph(21);
  doc["nodes"] = Json::array({
      Json{{"id", "g"}, {"op", "gen.synthetic"}, {"params", {{"pointCount", 20000}, {"seed", 21}}}},
      Json{{"id", "s"},
           {"op", "lib.clean"},
           {"params", {{"leaf", Json::array({0.02, 0.02, 0.02})}}}},
  });
  doc["edges"] = Json::array({Json{{"id", "e1"},
                                   {"from", {{"node", "g"}, {"port", "cloud"}}},
                                   {"to", {{"node", "s"}, {"port", "cloud"}}}}});
  const RunLog log = runGraph(doc);
  REQUIRE(log.runStatus() == "ok");
  CHECK(log.finalState("s/v") == "done");
  CHECK(elementCountOf(log, "s/p") > 0);

  // 收尾：库必须清空，否则后面的测试会看到一个多出来的算子
  exec::Library::instance().setDirs({});
  CHECK(ensureRegistry().find("lib.clean") == nullptr);
  std::filesystem::remove_all(dir);
}

TEST_CASE("preview 模式：源头抽稀，且不污染正式缓存") {
  ensureTestOps();
  Json doc = flatGraph(22);
  doc["nodes"][0]["params"]["pointCount"] = 500000;

  exec::ResultStore::instance().clear();
  const std::size_t before = exec::ResultStore::instance().liveEntryCount();
  CHECK(before == 0);

  exec::RunOptions opts;
  opts.runId = "preview-run";
  opts.mode = exec::RunMode::Preview;
  opts.previewMaxPoints = 20000;
  RunLog preview;
  preview.runId = opts.runId;
  {
    exec::Run run(doc.dump(), opts, &detail::collect, &preview);
    run.join();
  }
  REQUIRE(preview.runStatus() == "ok");
  CHECK(elementCountOf(preview, "g") == 20000);
  CHECK(elementCountOf(preview, "v") > 0);

  // 正式跑一遍：因为命名空间不同，一条都命不中，源头也不是抽稀后的
  const RunLog full = runGraphCached(doc);
  REQUIRE(full.runStatus() == "ok");
  CHECK(full.finalState("g") == "done");
  CHECK(elementCountOf(full, "g") == 500000);

  const Json pk = preview.ofKind("run_started").front()["nodes"];
  const Json fk = full.ofKind("run_started").front()["nodes"];
  CHECK(pk[0]["cacheKey"] != fk[0]["cacheKey"]);
}

TEST_CASE("preview 超预算会发一条 warn 日志") {
  ensureTestOps();
  Json doc = flatGraph(23);
  doc["nodes"][0]["params"]["pointCount"] = 400000;

  exec::RunOptions opts;
  opts.runId = "preview-budget";
  opts.mode = exec::RunMode::Preview;
  opts.previewMaxPoints = 200000;
  opts.previewBudgetMs = 1;  // 一定超
  RunLog log;
  log.runId = opts.runId;
  {
    exec::Run run(doc.dump(), opts, &detail::collect, &log);
    run.join();
  }
  REQUIRE(log.runStatus() == "ok");
  bool warned = false;
  for (const Json& e : log.ofKind("log")) {
    if (e.value("level", "") == "warn" && e.value("message", "").find("预览耗时") == 0) {
      warned = true;
    }
  }
  CHECK(warned);
}
