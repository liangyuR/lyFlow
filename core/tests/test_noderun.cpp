// 节点运行（docs/node-run-plan.md）：R1–R7 与修订一 V1–V2（验收 1–6、6b、14–16）。
// isolate = 上游只许命中缓存、不齐就在开跑前整次失败；force = 跳过缓存强制重算（V1 起两者正交）；
// 带 targets 的运行把计划外、仍有当前结果的节点挂进来（R7 / V2）。执行计数全靠 test.tally 的按 tag 计数。
#include <doctest/doctest.h>

#include <algorithm>
#include <memory>
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
               exec::RunMode mode = exec::RunMode::Full, std::vector<std::string> targets = {},
               std::vector<std::string> force = {}) {
  static int counter = 0;
  RunLog log;
  log.runId = "noderun-" + std::to_string(counter++);
  exec::RunOptions options;
  options.runId = log.runId;
  options.isolate = std::move(isolate);
  options.targets = std::move(targets);
  options.force = std::move(force);
  options.mode = mode;
  {
    exec::Run run(doc.dump(), options, &detail::collect, &log);
    run.join();
  }
  return log;
}

/// 同上，但 Run 活到调用方放手为止：R7 要在运行结束之后按这次的 runId 取输出。
struct HeldRun {
  RunLog log;
  std::unique_ptr<exec::Run> run;
};

std::unique_ptr<HeldRun> holdRun(const Json& doc, std::vector<std::string> isolate,
                                 std::vector<std::string> targets = {}) {
  static int counter = 0;
  auto held = std::make_unique<HeldRun>();
  held->log.runId = "noderun-held-" + std::to_string(counter++);
  exec::RunOptions options;
  options.runId = held->log.runId;
  options.isolate = std::move(isolate);
  options.targets = std::move(targets);
  held->run = std::make_unique<exec::Run>(doc.dump(), options, &detail::collect, &held->log);
  held->run->join();
  return held;
}

std::vector<std::string> attachedOf(const RunLog& log) {
  const std::vector<Json> f = log.ofKind("run_finished");
  if (f.empty() || !f.back().contains("attached")) return {};
  return f.back()["attached"].get<std::vector<std::string>>();
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

TEST_CASE("验收 1 / 15：全跑一遍后 isolate [b]（不带 force）—— b 命中缓存；再加 force [b] 才只有 b 执行") {
  ensureTestOps();
  exec::ResultStore::instance().clear();
  const Json doc = chain("v1");
  REQUIRE(runWith(doc, {}).runStatus() == "ok");
  REQUIRE(tallyOf("v1a") == 1);
  REQUIRE(tallyOf("v1b") == 1);
  REQUIRE(tallyOf("v1c") == 1);

  // 修订一 V1：isolate 不再隐含强制重算 —— b 自己也命中缓存，一个算子都不调
  const RunLog cached = runWith(doc, {"b"});
  REQUIRE(cached.runStatus() == "ok");
  CHECK(tallyOf("v1a") == 1);
  CHECK(tallyOf("v1b") == 1);
  CHECK(cached.nodeEvent("b", "skipped")["stats"].value("cached", false) == true);
  CHECK_FALSE(anyRunning(cached));

  const RunLog log = runWith(doc, {"b"}, exec::RunMode::Full, {}, {"b"});
  REQUIRE(log.runStatus() == "ok");
  CHECK(tallyOf("v1a") == 1);  // 上游用已有结果，没被重跑
  CHECK(tallyOf("v1b") == 2);  // force：自己真跑了一遍
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

  // 源节点跑过之后，b 的上游就齐了（b 自己从没跑过，没缓存可取，照常执行）
  const RunLog then = runWith(doc, {"b"});
  CHECK(then.runStatus() == "ok");
  CHECK(tallyOf("v3a") == 1);
  CHECK(tallyOf("v3b") == 1);
}

TEST_CASE("验收 4 / 15（按修订一改写）：isolate [b] 两次都命中缓存；isolate + force [b] 两次都真执行") {
  ensureTestOps();
  exec::ResultStore::instance().clear();
  const Json doc = chain("v4");
  REQUIRE(runWith(doc, {}).runStatus() == "ok");
  REQUIRE(tallyOf("v4b") == 1);

  CHECK(runWith(doc, {"b"}).runStatus() == "ok");
  CHECK(runWith(doc, {"b"}).runStatus() == "ok");
  CHECK(tallyOf("v4b") == 1);  // 不带 force：计数不变

  const RunLog first = runWith(doc, {"b"}, exec::RunMode::Full, {}, {"b"});
  const RunLog second = runWith(doc, {"b"}, exec::RunMode::Full, {}, {"b"});
  CHECK(first.runStatus() == "ok");
  CHECK(second.runStatus() == "ok");
  CHECK(tallyOf("v4b") == 3);  // 全图 1 次 + 强制 2 次：命中缓存也照跑
  CHECK(tallyOf("v4a") == 1);
  CHECK(statesOf(second, "b") == std::vector<std::string>{"pending", "running", "done"});

  // 强制重算的结果照常写回：之后的普通运行拿它命中缓存
  const RunLog full = runWith(doc, {});
  CHECK(full.runStatus() == "ok");
  CHECK(tallyOf("v4b") == 3);
  CHECK(full.nodeEvent("b", "skipped")["stats"].value("cached", false) == true);
}

TEST_CASE("验收 5：子图节点作 isolate + force —— 内部节点全部强制执行，子图外的上游只取缓存") {
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

  // force 的 id 语义同 targets：给子图节点等于给它全部内部节点（V1）
  const RunLog log = runWith(doc, {"s"}, exec::RunMode::Full, {}, {"s"});
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
  REQUIRE(s.contains("force"));
  CHECK(s["force"] == Json::array());

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

// ------------------------------------------------------------- R7：计划外节点挂结果

/// a → b → c，外加兄弟支路 a → d。
Json forked(const std::string& prefix, const std::string& cTag = "c") {
  return makeGraph(
      {
          {"a", "test.tally", Json{{"tag", prefix + "a"}}},
          {"b", "test.tally", Json{{"tag", prefix + "b"}}},
          {"c", "test.tally", Json{{"tag", prefix + cTag}}},
          {"d", "test.tally", Json{{"tag", prefix + "d"}}},
      },
      {{"a.cloud", "b.cloud"}, {"b.cloud", "c.cloud"}, {"a.cloud", "d.cloud"}});
}

TEST_CASE("验收 6b：计划外的 c、d 不执行，但按新 runId 取得到输出，run_finished.attached 列出它们") {
  ensureTestOps();
  exec::ResultStore::instance().clear();
  const Json doc = forked("r7");
  REQUIRE(runWith(doc, {}).runStatus() == "ok");

  auto held = holdRun(doc, {"b"});
  const RunLog& log = held->log;
  REQUIRE(log.runStatus() == "ok");
  CHECK(tallyOf("r7b") == 1);  // 修订一 V1：isolate 不带 force，b 也只是命中缓存
  CHECK(tallyOf("r7c") == 1);  // 挂结果不执行
  CHECK(tallyOf("r7d") == 1);
  CHECK(statesOf(log, "c").empty());  // 不发任何节点事件，更没有 running
  CHECK(statesOf(log, "d").empty());
  const auto plan = planOf(log);
  CHECK_FALSE(contains(plan, "c"));
  CHECK_FALSE(contains(plan, "d"));

  const auto attached = attachedOf(log);
  CHECK(contains(attached, "c"));
  CHECK(contains(attached, "d"));
  // 在计划里、这次有过收场事件的不算「挂上」
  CHECK_FALSE(contains(attached, "a"));
  CHECK_FALSE(contains(attached, "b"));

  // 按这次的 runId 真取得到：结果仓的索引里有它们，点数与源头一致
  exec::ResultStore& store = exec::ResultStore::instance();
  for (const char* id : {"c", "d"}) {
    Data data;
    REQUIRE(store.get(log.runId, id, "cloud", data));
    REQUIRE(data.asCloud() != nullptr);
    CHECK(data.asCloud()->pointCount() == 4);
    CHECK_FALSE(store.outputsOf(log.runId, id).empty());
  }
  // 普通运行不带这个字段
  const RunLog plain = runWith(doc, {});
  CHECK_FALSE(plain.ofKind("run_finished").back().contains("attached"));
}

TEST_CASE("验收 6b：结果仓里没有 c 当前 cacheKey 的结果（改了 c 的参数 / c 从没跑过）→ 不挂") {
  ensureTestOps();
  exec::ResultStore::instance().clear();
  REQUIRE(runWith(forked("r7x"), {}).runStatus() == "ok");

  // c 的参数一改，它的键就变了：旧结果对不上，不挂；d 照挂
  auto changed = holdRun(forked("r7x", "c2"), {"b"});
  REQUIRE(changed->log.runStatus() == "ok");
  const auto a1 = attachedOf(changed->log);
  CHECK_FALSE(contains(a1, "c"));
  CHECK(contains(a1, "d"));
  Data data;
  CHECK_FALSE(exec::ResultStore::instance().get(changed->log.runId, "c", "cloud", data));
  CHECK(tallyOf("r7xc2") == 0);

  // c 从没跑过：只跑到 b（targets），再单独跑 b
  exec::ResultStore::instance().clear();
  const Json fresh = forked("r7y");
  REQUIRE(runWith(fresh, {}, exec::RunMode::Full, {"b"}).runStatus() == "ok");
  auto never = holdRun(fresh, {"b"});
  REQUIRE(never->log.runStatus() == "ok");
  const auto a2 = attachedOf(never->log);
  CHECK_FALSE(contains(a2, "c"));
  CHECK_FALSE(contains(a2, "d"));
  CHECK(tallyOf("r7yc") == 0);
}

TEST_CASE("R7：上游不齐、开跑前就失败时，已有的结果照样挂上") {
  ensureTestOps();
  exec::ResultStore::instance().clear();
  const Json doc = forked("r7z");
  // 只跑过 d 那一支：a、d 有结果，b、c 没有
  REQUIRE(runWith(doc, {}, exec::RunMode::Full, {"d"}).runStatus() == "ok");

  // isolate c：它的上游 b 没有结果 → 开跑前失败；但这次运行接替了上一次，a、d 的结果要挂上
  auto failed = holdRun(doc, {"c"});
  CHECK(failed->log.runStatus() == "error");
  CHECK(missingNodes(failed->log) == std::set<std::string>{"b"});
  const auto attached = attachedOf(failed->log);
  CHECK(contains(attached, "a"));
  CHECK(contains(attached, "d"));
  CHECK_FALSE(contains(attached, "b"));
  CHECK_FALSE(contains(attached, "c"));
  Data data;
  CHECK(exec::ResultStore::instance().get(failed->log.runId, "d", "cloud", data));
  CHECK(tallyOf("r7za") == 1);
  CHECK(tallyOf("r7zd") == 1);

  // isolate 一个不存在的 id 是整图级失败（编译都没过），不在 R7 的范围里：不挂、不带这个字段
  auto bad = holdRun(doc, {"nosuch"});
  CHECK(bad->log.runStatus() == "error");
  CHECK_FALSE(bad->log.ofKind("run_finished").back().contains("attached"));
}

// ---------------------------------------------------------------- 修订一 V1 / V2

TEST_CASE("验收 14：targets [b]（智能运行）—— 全就绪时零执行；加 force 只有 b 执行；改 a 参数后 a、b 执行、c 不执行") {
  ensureTestOps();
  exec::ResultStore::instance().clear();
  REQUIRE(runWith(chain("v14"), {}).runStatus() == "ok");

  const RunLog ready = runWith(chain("v14"), {}, exec::RunMode::Full, {"b"});
  CHECK(ready.runStatus() == "ok");
  CHECK(tallyOf("v14a") == 1);
  CHECK(tallyOf("v14b") == 1);
  CHECK_FALSE(anyRunning(ready));
  CHECK(ready.nodeEvent("b", "skipped")["stats"].value("cached", false) == true);

  const RunLog forced = runWith(chain("v14"), {}, exec::RunMode::Full, {"b"}, {"b"});
  CHECK(forced.runStatus() == "ok");
  CHECK(tallyOf("v14a") == 1);
  CHECK(tallyOf("v14b") == 2);
  CHECK(forced.nodeEvent("a", "skipped")["stats"].value("cached", false) == true);
  const Json b = forced.nodeEvent("b", "done");
  REQUIRE(b.contains("stats"));
  CHECK(b["stats"].value("cached", false) == false);
  CHECK(finished(forced)["summary"]["nodes"]["b"].value("cached", true) == false);
  CHECK(forced.ofKind("run_started").front()["force"] == Json::array({"b"}));

  // 改 a 的参数：a、b 的键都变了，智能运行把它们一起跑；c 不在计划里
  const RunLog changed = runWith(chain("v14", 9), {}, exec::RunMode::Full, {"b"});
  CHECK(changed.runStatus() == "ok");
  CHECK(tallyOf("v14a") == 2);
  CHECK(tallyOf("v14b") == 3);
  CHECK(tallyOf("v14c") == 1);
  CHECK(statesOf(changed, "a") == std::vector<std::string>{"pending", "running", "done"});
  CHECK(statesOf(changed, "b") == std::vector<std::string>{"pending", "running", "done"});
  CHECK_FALSE(contains(planOf(changed), "c"));
}

TEST_CASE("验收 15：isolate 上游不齐时仍是 upstream_not_ready，带 force 也一样（force 只管本节点）") {
  ensureTestOps();
  exec::ResultStore::instance().clear();
  REQUIRE(runWith(chain("v15"), {}).runStatus() == "ok");
  const RunLog log = runWith(chain("v15", 9), {"b"}, exec::RunMode::Full, {}, {"b"});
  CHECK(log.runStatus() == "error");
  CHECK(missingNodes(log) == std::set<std::string>{"a"});
  CHECK(tallyOf("v15b") == 1);
  CHECK_FALSE(anyRunning(log));
}

TEST_CASE("验收 16：运行到此 targets [b] 之后 c、d 在 attached 里、按新 runId 取得到；force + preview 进预览命名空间") {
  ensureTestOps();
  exec::ResultStore::instance().clear();
  const Json doc = forked("v16");
  REQUIRE(runWith(doc, {}).runStatus() == "ok");

  auto held = holdRun(doc, {}, {"b"});
  REQUIRE(held->log.runStatus() == "ok");
  const auto attached = attachedOf(held->log);
  CHECK(contains(attached, "c"));
  CHECK(contains(attached, "d"));
  CHECK_FALSE(contains(attached, "a"));
  CHECK_FALSE(contains(attached, "b"));
  CHECK(tallyOf("v16c") == 1);
  CHECK(tallyOf("v16d") == 1);
  for (const char* id : {"c", "d"}) {
    Data data;
    CHECK(exec::ResultStore::instance().get(held->log.runId, id, "cloud", data));
  }
  held.reset();

  // 全图运行（没有 targets）不带 attached
  CHECK_FALSE(finished(runWith(doc, {}))["attached"].is_array());

  // force + preview：可以组合，b 真跑一遍，结果写进预览命名空间 ——
  // 之后不带 force 的预览运行命中它，正式运行仍命中正式那一份，计数都不再变
  const RunLog preview = runWith(doc, {}, exec::RunMode::Preview, {"b"}, {"b"});
  CHECK(preview.runStatus() == "ok");
  CHECK(preview.ofKind("run_started").front().value("mode", "") == "preview");
  const int afterForce = tallyOf("v16b");
  CHECK(afterForce == 2);
  const RunLog previewAgain = runWith(doc, {}, exec::RunMode::Preview, {"b"});
  CHECK(previewAgain.nodeEvent("b", "skipped")["stats"].value("cached", false) == true);
  const RunLog fullAgain = runWith(doc, {}, exec::RunMode::Full, {"b"});
  CHECK(fullAgain.nodeEvent("b", "skipped")["stats"].value("cached", false) == true);
  CHECK(tallyOf("v16b") == afterForce);
}

TEST_CASE("force 给了不存在的 id：整图级失败") {
  ensureTestOps();
  exec::ResultStore::instance().clear();
  const RunLog log = runWith(chain("v17"), {}, exec::RunMode::Full, {}, {"nosuch"});
  CHECK(log.runStatus() == "error");
  CHECK(finished(log)["error"].value("code", "") == "unknown_node");
  CHECK(tallyOf("v17a") == 0);
}

