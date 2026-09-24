// 单节点运行（docs/node-run-plan.md R1–R5，验收 1–6）：只重算 isolate 里的节点，上游只许命中缓存，
// 上游不齐就在开跑前整次失败，下游一概不进计划。执行计数全靠 test.tally 的按 tag 计数。
#include <doctest/doctest.h>

#include <algorithm>
#include <set>

#include "exec/executor.h"
#include "exec/result_store.h"
#include "helpers.h"
#include "test_ops.h"

using namespace lyflow;
using namespace lyflow::test;
using lyflow::test::ops::tallyOf;

namespace {

/// 跑一次，Run 在返回前析构（freeRun 只丢索引，内容寻址层留着给下一次命中）。
RunLog runWith(const Json& doc, std::vector<std::string> isolate,
               exec::RunMode mode = exec::RunMode::Full) {
  static int counter = 0;
  RunLog log;
  log.runId = "noderun-" + std::to_string(counter++);
  exec::RunOptions options;
  options.runId = log.runId;
  options.isolate = std::move(isolate);
  options.mode = mode;
  {
    exec::Run run(doc.dump(), options, &detail::collect, &log);
    run.join();
  }
  return log;
}

/// a → b → c，三个 tally 各带自己的 tag。prefix 让每个用例的计数互不干扰。
Json chain(const std::string& prefix, int pointCount = 4) {
  return makeGraph(
      {
          {"a", "test.tally", Json{{"tag", prefix + "a"}, {"pointCount", pointCount}}},
          {"b", "test.tally", Json{{"tag", prefix + "b"}}},
          {"c", "test.tally", Json{{"tag", prefix + "c"}}},
      },
      {{"a.cloud", "b.cloud"}, {"b.cloud", "c.cloud"}});
}

std::vector<std::string> statesOf(const RunLog& log, const std::string& nodeId) {
  std::vector<std::string> out;
  for (const Json& e : log.events) {
    if (e.value("kind", "") == "node_state" && e.value("nodeId", "") == nodeId) {
      out.push_back(e.value("state", ""));
    }
  }
  return out;
}

std::vector<std::string> planOf(const RunLog& log) {
  const std::vector<Json> started = log.ofKind("run_started");
  if (started.empty()) return {};
  return started.front()["plan"].get<std::vector<std::string>>();
}

bool contains(const std::vector<std::string>& v, const std::string& x) {
  return std::find(v.begin(), v.end(), x) != v.end();
}

/// 任何节点进过 running 吗。「一个算子都没执行」除了计数，还要事件层面也干净。
bool anyRunning(const RunLog& log) {
  for (const Json& e : log.events) {
    if (e.value("kind", "") == "node_state" && e.value("state", "") == "running") return true;
  }
  return false;
}

Json finished(const RunLog& log) {
  const std::vector<Json> f = log.ofKind("run_finished");
  return f.empty() ? Json::object() : f.back();
}

std::set<std::string> missingNodes(const RunLog& log) {
  std::set<std::string> out;
  const Json f = finished(log);
  for (const Json& d : f.value("diagnostics", Json::array())) {
    if (d.value("code", "") == "upstream_not_ready") out.insert(d.value("nodeId", ""));
  }
  return out;
}

}  // namespace

TEST_CASE("验收 1：全跑一遍后 isolate [b] —— 只有 b 执行，a 命中缓存，c 不在计划里") {
  ensureTestOps();
  exec::ResultStore::instance().clear();
  const Json doc = chain("v1");
  REQUIRE(runWith(doc, {}).runStatus() == "ok");
  REQUIRE(tallyOf("v1a") == 1);
  REQUIRE(tallyOf("v1b") == 1);
  REQUIRE(tallyOf("v1c") == 1);

  const RunLog log = runWith(doc, {"b"});
  REQUIRE(log.runStatus() == "ok");
  CHECK(tallyOf("v1a") == 1);  // 上游用已有结果，没被重跑
  CHECK(tallyOf("v1b") == 2);  // 自己真跑了一遍
  CHECK(tallyOf("v1c") == 1);  // 下游不动

  const Json b = log.nodeEvent("b", "done");
  REQUIRE(b.contains("stats"));
  CHECK(b["stats"].value("cached", false) == false);
  const Json a = log.nodeEvent("a", "skipped");
  REQUIRE(a.contains("stats"));
  CHECK(a["stats"].value("cached", false) == true);

  const auto plan = planOf(log);
  CHECK(contains(plan, "a"));
  CHECK(contains(plan, "b"));
  CHECK_FALSE(contains(plan, "c"));
  CHECK(statesOf(log, "c").empty());
  CHECK(log.seqIsDense());
}

TEST_CASE("验收 2：改 a 的参数后 isolate [b] —— 开跑前失败，upstream_not_ready 指向 a，零执行") {
  ensureTestOps();
  exec::ResultStore::instance().clear();
  REQUIRE(runWith(chain("v2"), {}).runStatus() == "ok");
  const int a0 = tallyOf("v2a");
  const int b0 = tallyOf("v2b");
  const int c0 = tallyOf("v2c");

  // pointCount 进 cacheKey：a 的键变了，b 能取到的上游结果就对不上了
  const RunLog log = runWith(chain("v2", 9), {"b"});
  CHECK(log.runStatus() == "error");
  const Json f = finished(log);
  CHECK(f["error"].value("code", "") == "upstream_not_ready");
  CHECK(f["error"].value("message", "").find("上游 a 还没有可用结果") != std::string::npos);
  CHECK(missingNodes(log) == std::set<std::string>{"a"});

  CHECK(tallyOf("v2a") == a0);
  CHECK(tallyOf("v2b") == b0);
  CHECK(tallyOf("v2c") == c0);
  CHECK_FALSE(anyRunning(log));
  // 缺结果的上游不是「失败了」：一条节点事件都不该有，免得编辑器把它标红
  CHECK(log.ofKind("node_state").empty());
  CHECK(log.seqIsDense());
}

TEST_CASE("验收 3：从没跑过时 isolate [b] 同样失败；isolate [a]（源节点）照常执行") {
  ensureTestOps();
  exec::ResultStore::instance().clear();
  const Json doc = chain("v3");

  const RunLog never = runWith(doc, {"b"});
  CHECK(never.runStatus() == "error");
  CHECK(missingNodes(never) == std::set<std::string>{"a"});
  CHECK(tallyOf("v3a") == 0);
  CHECK(tallyOf("v3b") == 0);
  CHECK_FALSE(anyRunning(never));

  const RunLog source = runWith(doc, {"a"});
  CHECK(source.runStatus() == "ok");
  CHECK(tallyOf("v3a") == 1);
  CHECK(tallyOf("v3b") == 0);
  CHECK(planOf(source) == std::vector<std::string>{"a"});
  CHECK(statesOf(source, "a") == std::vector<std::string>{"pending", "running", "done"});

  // 源节点跑过之后，b 的上游就齐了
  const RunLog then = runWith(doc, {"b"});
  CHECK(then.runStatus() == "ok");
  CHECK(tallyOf("v3a") == 1);
  CHECK(tallyOf("v3b") == 1);
}

TEST_CASE("验收 4：b 连续两次 isolate [b]，两次都真执行") {
  ensureTestOps();
  exec::ResultStore::instance().clear();
  const Json doc = chain("v4");
  REQUIRE(runWith(doc, {}).runStatus() == "ok");
  REQUIRE(tallyOf("v4b") == 1);

  const RunLog first = runWith(doc, {"b"});
  const RunLog second = runWith(doc, {"b"});
  CHECK(first.runStatus() == "ok");
  CHECK(second.runStatus() == "ok");
  CHECK(tallyOf("v4b") == 3);  // 全图 1 次 + 单独 2 次：命中缓存也照跑
  CHECK(tallyOf("v4a") == 1);
  CHECK(statesOf(second, "b") == std::vector<std::string>{"pending", "running", "done"});

  // 强制重算的结果照常写回：之后的普通运行拿它命中缓存
  const RunLog full = runWith(doc, {});
  CHECK(full.runStatus() == "ok");
  CHECK(tallyOf("v4b") == 3);
  CHECK(full.nodeEvent("b", "skipped")["stats"].value("cached", false) == true);
}

TEST_CASE("验收 5：子图节点作 isolate —— 内部节点全部强制执行，子图外的上游只取缓存") {
  ensureTestOps();
  exec::ResultStore::instance().clear();
  // g → s(x → y) → t
  Json doc = makeGraph(
      {
          {"g", "test.tally", Json{{"tag", "v5g"}}},
          {"s", "sub:pair"},
          {"t", "test.tally", Json{{"tag", "v5t"}}},
      },
      {{"g.cloud", "s.cloud"}, {"s.cloud", "t.cloud"}});
  doc["subgraphs"] = Json::object();
  doc["subgraphs"]["pair"] = Json{
      {"name", "两步"},
      {"nodes", Json::array({Json{{"id", "x"}, {"op", "test.tally"}, {"params", {{"tag", "v5x"}}}},
                             Json{{"id", "y"}, {"op", "test.tally"}, {"params", {{"tag", "v5y"}}}}})},
      {"edges", Json::array({Json{{"id", "ei"},
                                  {"from", {{"node", "x"}, {"port", "cloud"}}},
                                  {"to", {{"node", "y"}, {"port", "cloud"}}}}})},
      {"inputs", Json::array({Json{{"name", "cloud"},
                                   {"type", "PointCloud"},
                                   {"to", Json::array({Json{{"node", "x"}, {"port", "cloud"}}})}}})},
      {"outputs", Json::array({Json{{"name", "cloud"},
                                    {"type", "PointCloud"},
                                    {"from", {{"node", "y"}, {"port", "cloud"}}}}})},
      {"params", Json::array()},
  };
  REQUIRE(runWith(doc, {}).runStatus() == "ok");
  REQUIRE(tallyOf("v5x") == 1);

  const RunLog log = runWith(doc, {"s"});
  REQUIRE(log.runStatus() == "ok");
  CHECK(tallyOf("v5x") == 2);
  CHECK(tallyOf("v5y") == 2);
  CHECK(tallyOf("v5g") == 1);
  CHECK(tallyOf("v5t") == 1);
  CHECK(log.nodeEvent("g", "skipped")["stats"].value("cached", false) == true);
  // 真算的 done：stats 里不带 cached（只有命中缓存时才写 cached: true）
  for (const char* inner : {"s/x", "s/y"}) {
    const Json e = log.nodeEvent(inner, "done");
    REQUIRE(e.contains("stats"));
    CHECK(e["stats"].value("cached", false) == false);
  }
  CHECK_FALSE(contains(planOf(log), "t"));
}

TEST_CASE("验收 6：isolate + preview 是参数错误；run_started.isolate 按原样带出") {
  ensureTestOps();
  exec::ResultStore::instance().clear();
  const Json doc = chain("v6");
  REQUIRE(runWith(doc, {}).runStatus() == "ok");

  const RunLog bad = runWith(doc, {"b"}, exec::RunMode::Preview);
  CHECK(bad.runStatus() == "error");
  CHECK(finished(bad)["error"].value("code", "") == "bad_input");
  CHECK(tallyOf("v6b") == 1);
  CHECK_FALSE(anyRunning(bad));

  const RunLog ok = runWith(doc, {"b"});
  const std::vector<Json> started = ok.ofKind("run_started");
  REQUIRE(started.size() == 1);
  const Json& s = started.front();
  // schema：isolate 是字符串数组；给了 isolate 时 targets 取同一组 id，mode 仍是 full
  REQUIRE(s.contains("isolate"));
  REQUIRE(s["isolate"].is_array());
  CHECK(s["isolate"] == Json::array({"b"}));
  CHECK(s["targets"] == Json::array({"b"}));
  CHECK(s.value("mode", "") == "full");

  // 普通运行也带这个字段，只是空数组 —— 与 targets 同一个约定
  const RunLog plain = runWith(doc, {});
  const std::vector<Json> plainStarted = plain.ofKind("run_started");
  REQUIRE(plainStarted.size() == 1);
  CHECK(plainStarted.front()["isolate"] == Json::array());
}

TEST_CASE("isolate 的节点不存在：整图级失败，零执行") {
  ensureTestOps();
  exec::ResultStore::instance().clear();
  const RunLog log = runWith(chain("v7"), {"nosuch"});
  CHECK(log.runStatus() == "error");
  CHECK(finished(log)["error"].value("code", "") == "unknown_node");
  CHECK(tallyOf("v7a") == 0);
}

TEST_CASE("isolate 节点静音：照静音语义透传，上游仍只取缓存") {
  ensureTestOps();
  exec::ResultStore::instance().clear();
  Json doc = chain("v8");
  REQUIRE(runWith(doc, {}).runStatus() == "ok");
  doc["nodes"][1]["bypass"] = true;
  const RunLog log = runWith(doc, {"b"});
  CHECK(log.runStatus() == "ok");
  CHECK(tallyOf("v8a") == 1);
  CHECK(tallyOf("v8b") == 1);  // 静音不调 compute
  CHECK(log.nodeEvent("b", "skipped")["stats"].value("bypassed", false) == true);
}
