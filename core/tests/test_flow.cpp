// A1 的四条新执行语义：Error 作为值、惰性端口、图级命名输出、运行时注入。
#include <doctest/doctest.h>

#include <algorithm>
#include <map>
#include <thread>

#include "exec/executor.h"
#include "exec/result_store.h"
#include "helpers.h"
#include "test_ops.h"

namespace lyflow::test {
namespace {

using exec::ResultStore;

Json graphWithOutputs(const std::vector<N>& nodes, const std::vector<E>& edges,
                      const Json& outputs) {
  Json doc = makeGraph(nodes, edges);
  doc["outputs"] = outputs;
  return doc;
}

/// n_a 失败 → fallback demand b；n_a 成功 → b 的闭包一次也不跑。
Json fallbackGraph(bool primaryFails) {
  return graphWithOutputs(
      {N{"n_a", primaryFails ? "test.fail" : "test.counted",
         primaryFails ? Json::object() : Json{{"pointCount", 4}}},
       N{"n_b", "test.counted", Json{{"pointCount", 7}}},
       N{"n_b2", "test.thin", Json{{"leaf", Json::array({0.01, 0.01, 0.01})}}},
       N{"n_fb", "flow.fallback", Json::object()}},
      {E{"n_b.cloud", "n_b2.cloud"}, E{"n_a.cloud", "n_fb.a"}, E{"n_b2.cloud", "n_fb.b"}},
      Json{{"result", Json{{"node", "n_fb"}, {"port", "out"}}}});
}

int callsDuring(const Json& doc) {
  ops::computeCalls().store(0);
  Session s(doc);
  s.wait();
  return ops::computeCalls().load();
}

}  // namespace

TEST_CASE("主路径成功时备用闭包一次 compute 都不调") {
  ensureTestOps();
  ops::computeCalls().store(0);
  Session s(fallbackGraph(/*primaryFails=*/false));
  RunLog& log = s.wait();

  CHECK(log.runStatus() == "ok");
  CHECK(log.seqIsDense());
  // n_a 是 test.counted，跑了一次；n_b 是备用闭包的源，绝不该被调到
  CHECK(ops::computeCalls().load() == 1);
  CHECK(log.finalState("n_fb") == "done");
  CHECK(log.finalState("n_b") == "skipped");
  CHECK(log.finalState("n_b2") == "skipped");
  CHECK(log.nodeEvent("n_b", "skipped")["stats"]["reason"] == "not_demanded");
  CHECK(log.nodeEvent("n_b2", "skipped")["stats"]["reason"] == "not_demanded");
  CHECK(log.nodeEvent("n_b", "skipped")["stats"]["outputsAvailable"] == false);
  CHECK(log.nodeEvent("n_fb", "done")["stats"]["outputsAvailable"] == true);
  CHECK(log.ofKind("plan_extended").empty());

  // run_started 只列非 deferred 节点
  const Json started = log.ofKind("run_started").front();
  CHECK(started["nodeCount"] == 2);
  std::vector<std::string> planned;
  for (const auto& id : started["plan"]) planned.push_back(id.get<std::string>());
  CHECK(std::find(planned.begin(), planned.end(), "n_b") == planned.end());
  CHECK(std::find(planned.begin(), planned.end(), "n_a") != planned.end());
}

TEST_CASE("plan 标出惰性节点与「谁的哪个惰性端口在管着它」") {
  ensureTestOps();
  // manifest 早就有端口的 lazy（ADR-0016），缺的是「这张图上到底哪些节点因此不跑」——
  // 真实任务里有人翻了源码才发现整条备用分支是惰性的（m6-plan §5 / H9）。
  exec::ResultStore::instance().clear();
  const Json plan =
      Json::parse(exec::planGraphJson(fallbackGraph(/*primaryFails=*/false).dump(), {}, {}));
  REQUIRE(plan.is_array());
  std::map<std::string, Json> byId;
  for (const Json& n : plan) byId[n["nodeId"].get<std::string>()] = n;
  REQUIRE(byId.size() == 4);

  CHECK(byId["n_a"]["lazy"] == false);
  CHECK(byId["n_a"]["demandedBy"] == Json::array());
  CHECK(byId["n_fb"]["lazy"] == false);

  // 直接挂在惰性端口上的那个
  CHECK(byId["n_b2"]["lazy"] == true);
  CHECK(byId["n_b2"]["demandedBy"] == Json::array({"n_fb:b"}));
  // 闭包深处的：沿着惰性节点往下继承，指得回那个真正的闸门
  CHECK(byId["n_b"]["lazy"] == true);
  CHECK(byId["n_b"]["demandedBy"] == Json::array({"n_fb:b"}));

  // 与执行期对得上：lazy 的那两个正是 not_demanded 的那两个
  Session s(fallbackGraph(/*primaryFails=*/false));
  RunLog& log = s.wait();
  for (const auto& kv : byId) {
    const bool lazy = kv.second["lazy"].get<bool>();
    const Json skipped = log.nodeEvent(kv.first, "skipped");
    const bool notDemanded = skipped.contains("stats") &&
                             skipped["stats"].value("reason", "") == "not_demanded";
    CHECK(lazy == notDemanded);
  }
}

TEST_CASE("主路径失败时 demand 备用闭包，plan_extended 与 run_started.nodes 同构") {
  ensureTestOps();
  ops::computeCalls().store(0);
  Session s(fallbackGraph(/*primaryFails=*/true));
  RunLog& log = s.wait();

  CHECK(log.runStatus() == "ok");
  CHECK(log.seqIsDense());
  CHECK(ops::computeCalls().load() == 1);  // 这次只有 n_b 被调
  CHECK(log.finalState("n_a") == "error");
  CHECK(log.finalState("n_b") == "done");
  CHECK(log.finalState("n_b2") == "done");
  CHECK(log.finalState("n_fb") == "done");

  const auto extended = log.ofKind("plan_extended");
  REQUIRE(extended.size() == 1);
  CHECK(extended[0]["demandedBy"] == "n_fb");
  CHECK(extended[0]["port"] == "b");
  const Json& added = extended[0]["nodes"];
  REQUIRE(added.size() == 2);
  for (const Json& n : added) {
    // 与 run_started.nodes 同构：id / cacheKey / level
    CHECK(n.contains("id"));
    CHECK(n["cacheKey"].get<std::string>().size() == 32);
    CHECK(n.contains("level"));
  }
  CHECK(added[0]["id"] == "n_b");
  CHECK(added[1]["id"] == "n_b2");

  // 透传的是 b 那一路：7 个点抽稀步长 1，还是 7 个
  Data out;
  REQUIRE(ResultStore::instance().get(s.runId(), "n_fb", "out", out));
  REQUIRE(out.asCloud() != nullptr);
  CHECK(out.asCloud()->pointCount() == 7);

  Data choice;
  REQUIRE(ResultStore::instance().get(s.runId(), "n_fb", "choice", choice));
  REQUIRE(choice.asRecord() != nullptr);
  CHECK(choice.asRecord()->data["choice"] == "b");
}

TEST_CASE("acceptsError 的端口收到 Error Data 而不是被连坐") {
  ensureTestOps();
  // fallback 的 a 声明了 acceptsError：上游失败时它照跑，不报 upstream_failed
  Session s(fallbackGraph(/*primaryFails=*/true));
  RunLog& log = s.wait();
  CHECK(log.finalState("n_fb") != "cancelled");
  CHECK(log.nodeEvent("n_fb", "cancelled").empty());

  Data choice;
  REQUIRE(ResultStore::instance().get(s.runId(), "n_fb", "choice", choice));
  // reason 里带着上游那条 Status 的 code 与 message
  const std::string reason = choice.asRecord()->data["reason"].get<std::string>();
  CHECK(reason.find("io") != std::string::npos);
  CHECK(reason.find("测试用的失败") != std::string::npos);
}

TEST_CASE("失败先连坐几个中间节点、最后撞上 acceptsError，整轮仍然是 ok") {
  ensureTestOps();
  // n_a 失败 → n_t1 → n_t2 连坐 → 才落到 fallback 的 acceptsError 端口上。
  // 这条级联整个算被接住，run_finished 不该因此变成 error（ADR-0016）。
  const Json doc = makeGraph(
      {N{"n_a", "test.fail", Json::object()},
       N{"n_t1", "test.thin", Json::object()},
       N{"n_t2", "test.thin", Json::object()},
       N{"n_b", "test.counted", Json{{"pointCount", 5}}},
       N{"n_fb", "flow.fallback", Json::object()}},
      {E{"n_a.cloud", "n_t1.cloud"}, E{"n_t1.cloud", "n_t2.cloud"},
       E{"n_t2.cloud", "n_fb.a"}, E{"n_b.cloud", "n_fb.b"}});
  Session s(doc);
  RunLog& log = s.wait();

  CHECK(log.runStatus() == "ok");
  CHECK(log.finalState("n_a") == "error");
  CHECK(log.finalState("n_t1") == "cancelled");
  CHECK(log.finalState("n_t2") == "cancelled");
  CHECK(log.finalState("n_fb") == "done");

  Data out;
  REQUIRE(ResultStore::instance().get(s.runId(), "n_fb", "out", out));
  CHECK(out.asCloud()->pointCount() == 5);
}

TEST_CASE("没有声明 acceptsError 的端口仍然被上游失败连坐") {
  ensureTestOps();
  const Json doc = makeGraph({N{"n_a", "test.fail", Json::object()},
                              N{"n_t", "test.thin", Json::object()}},
                             {E{"n_a.cloud", "n_t.cloud"}});
  RunLog log = runGraph(doc);
  CHECK(log.finalState("n_t") == "cancelled");
  CHECK(log.nodeEvent("n_t", "cancelled")["error"]["code"] == "upstream_failed");
}

TEST_CASE("两条都失败时 fallback 自己报错，并带上两边的信息") {
  ensureTestOps();
  const Json doc = makeGraph(
      {N{"n_a", "test.fail", Json{{"message", "主路径炸了"}}},
       N{"n_b", "test.fail", Json{{"message", "备用也炸了"}}},
       N{"n_fb", "flow.fallback", Json::object()}},
      {E{"n_a.cloud", "n_fb.a"}, E{"n_b.cloud", "n_fb.b"}});
  RunLog log = runGraph(doc);
  CHECK(log.runStatus() == "error");
  CHECK(log.finalState("n_fb") == "error");
  const std::string message =
      log.nodeEvent("n_fb", "error")["error"]["message"].get<std::string>();
  CHECK(message.find("主路径炸了") != std::string::npos);
  CHECK(message.find("备用也炸了") != std::string::npos);
}

TEST_CASE("图级命名输出：lyflow_run_outputs 按名字给出类型与值") {
  ensureTestOps();
  Session s(fallbackGraph(/*primaryFails=*/false));
  RunLog& log = s.wait();
  CHECK(log.runStatus() == "ok");

  const Json outputs = Json::parse(exec::runOutputsJson(s.runId()));
  REQUIRE(outputs.contains("result"));
  CHECK(outputs["result"]["node"] == "n_fb");
  CHECK(outputs["result"]["port"] == "out");
  CHECK(outputs["result"]["type"] == "PointCloud");
  CHECK(outputs["result"]["elementCount"] == 4);
  // 点云不给 value，走二进制通道（D4）
  CHECK_FALSE(outputs["result"].contains("value"));

  // run_started 也把 outputs 声明带出去，前端不必再解析图
  const Json started = log.ofKind("run_started").front();
  REQUIRE(started.contains("outputs"));
  CHECK(started["outputs"][0]["name"] == "result");
}

TEST_CASE("图输出指向不存在的节点或端口是校验错误") {
  ensureTestOps();
  {
    const Json doc = graphWithOutputs({N{"g", "gen.synthetic", Json::object()}}, {},
                                      Json{{"a", Json{{"node", "nope"}, {"port", "cloud"}}}});
    const std::string diags = exec::validateGraphJson(doc.dump(), {});
    CHECK(diags.find("unknown_node") != std::string::npos);
  }
  {
    const Json doc = graphWithOutputs({N{"g", "gen.synthetic", Json::object()}}, {},
                                      Json{{"a", Json{{"node", "g"}, {"port", "nope"}}}});
    const std::string diags = exec::validateGraphJson(doc.dump(), {});
    CHECK(diags.find("unknown_port") != std::string::npos);
  }
}

TEST_CASE("运行时注入：源节点的 compute 被跳过，输出就是注入的那片云") {
  ensureTestOps();
  const Json doc = graphWithOutputs(
      {N{"n_src", "test.counted", Json{{"pointCount", 5}}},
       N{"n_t", "test.thin", Json{{"leaf", Json::array({0.01, 0.01, 0.01})}}}},
      {E{"n_src.cloud", "n_t.cloud"}},
      Json{{"thinned", Json{{"node", "n_t"}, {"port", "cloud"}}}});

  PointCloud injected;
  for (int i = 0; i < 3; ++i) {
    injected.push(static_cast<float>(i), 0.0F, 0.0F);
  }
  std::vector<exec::InjectedInput> inputs{
      exec::InjectedInput{"n_src", "cloud", Data::cloud(std::move(injected))}};

  ops::computeCalls().store(0);
  Session s(doc, {}, {}, /*keepCache=*/false, /*maxParallel=*/0, /*noReuse=*/false,
            std::move(inputs));
  RunLog& log = s.wait();

  CHECK(log.runStatus() == "ok");
  CHECK(ops::computeCalls().load() == 0);  // 注入的节点整个 compute 都没跑
  CHECK(log.nodeEvent("n_src", "done")["stats"]["provided"] == true);

  const Json outputs = Json::parse(exec::runOutputsJson(s.runId()));
  CHECK(outputs["thinned"]["elementCount"] == 3);
}

TEST_CASE("注入的数据进 cacheKey：换一片云就不会命中旧结果") {
  ensureTestOps();
  const Json doc = makeGraph({N{"n_src", "test.counted", Json::object()}}, {});
  auto keyOf = [&](float x) {
    PointCloud c;
    c.push(x, 0.0F, 0.0F);
    std::vector<exec::InjectedInput> inputs{
        exec::InjectedInput{"n_src", "cloud", Data::cloud(std::move(c))}};
    Session s(doc, {}, {}, /*keepCache=*/true, /*maxParallel=*/0, /*noReuse=*/false,
              std::move(inputs));
    RunLog& log = s.wait();
    return log.ofKind("run_started").front()["nodes"][0]["cacheKey"].get<std::string>();
  };
  CHECK(keyOf(1.0F) != keyOf(2.0F));
  CHECK(keyOf(1.0F) == keyOf(1.0F));
}

TEST_CASE("并发：八个 run 同时跑，事件按 runId 隔离，结果与串行一致") {
  ensureTestOps();
  ResultStore::instance().clear();
  constexpr int kRuns = 8;

  const Json doc = makeGraph(
      {N{"g", "gen.synthetic", Json{{"pointCount", 20000}}},
       N{"t", "test.thin", Json{{"leaf", Json::array({0.03, 0.03, 0.03})}}},
       N{"m", "test.merge2", Json::object()}},
      {E{"g.cloud", "t.cloud"}, E{"t.cloud", "m.a"}, E{"g.cloud", "m.b"}});

  // 先串行跑一遍拿基准
  std::size_t expected = 0;
  {
    Session s(doc);
    s.wait();
    Data d;
    REQUIRE(ResultStore::instance().get(s.runId(), "m", "cloud", d));
    expected = d.asCloud()->pointCount();
  }

  std::vector<std::unique_ptr<Session>> sessions;
  sessions.reserve(kRuns);
  for (int i = 0; i < kRuns; ++i) {
    // keepCache=true：八个 run 共用结果仓，正是要压的那条路
    sessions.push_back(std::make_unique<Session>(doc, std::filesystem::path{},
                                                 std::vector<std::string>{}, true, 0, true));
  }
  for (auto& s : sessions) {
    RunLog& log = s->wait();
    CHECK(log.runStatus() == "ok");
    CHECK(log.seqIsDense());
    // 每条事件的 runId 都必须是自己的
    for (const Json& e : log.events) CHECK(e["runId"] == s->runId());
    Data d;
    REQUIRE(ResultStore::instance().get(s->runId(), "m", "cloud", d));
    CHECK(d.asCloud()->pointCount() == expected);
  }
}

TEST_CASE("flow.select 按条件只调度选中的那一路") {
  ensureTestOps();
  // cond 用一个 Record：ok=false 就选 b
  const Json doc = makeGraph(
      {N{"n_cond", "test.counted", Json::object()},
       N{"n_a", "test.counted", Json{{"pointCount", 3}}},
       N{"n_b", "test.counted", Json{{"pointCount", 9}}},
       N{"n_sel", "flow.select", Json::object()}},
      {E{"n_cond.cloud", "n_sel.cond"}, E{"n_a.cloud", "n_sel.a"}, E{"n_b.cloud", "n_sel.b"}});
  // cond 端口收到 PointCloud，算子据此报 bad_input —— 说明 cond 与 a/b 不共用类型变量
  RunLog log = runGraph(doc);
  CHECK(log.finalState("n_sel") == "error");
  CHECK(log.nodeEvent("n_sel", "error")["error"]["code"] == "bad_input");
  // a / b 两路都没被 demand
  CHECK(log.nodeEvent("n_a", "skipped")["stats"]["reason"] == "not_demanded");
  CHECK(log.nodeEvent("n_b", "skipped")["stats"]["reason"] == "not_demanded");
}

TEST_CASE("导入器：注册一种 kind，lyflow_import 走它") {
  Registry& r = ensureRegistry();
  ImporterDesc d;
  d.kind = "test.echo";
  d.label = "Echo";
  d.fn = [](const std::string& text, const std::filesystem::path&, std::string& out) {
    if (text.empty()) {
      return Status::Error(Phase::Validate, "bad_input", "空文本");
    }
    Json doc = makeGraph({N{text, "gen.synthetic", Json::object()}}, {});
    out = doc.dump();
    return Status::Ok();
  };
  r.addImporter(d);

  const std::string ok = exec::importGraphJson("test.echo", "hello", {});
  REQUIRE(ok.front() == '{');
  CHECK(Json::parse(ok)["nodes"][0]["id"] == "hello");

  const std::string bad = exec::importGraphJson("test.echo", "", {});
  CHECK(bad.front() == '[');
  CHECK(bad.find("bad_input") != std::string::npos);

  const std::string missing = exec::importGraphJson("nope", "x", {});
  CHECK(missing.front() == '[');
  CHECK(missing.find("unknown_importer") != std::string::npos);
}

TEST_CASE("缓存命中的 fallback 不会去 demand 备用闭包") {
  ensureTestOps();
  ResultStore::instance().clear();
  // test.counted 是 deterministic=false，换个确定性的源才能命中缓存
  const Json doc = makeGraph(
      {N{"n_a", "gen.synthetic", Json{{"pointCount", 16}}},
       N{"n_b", "test.counted", Json{{"pointCount", 21}}},
       N{"n_fb", "flow.fallback", Json::object()}},
      {E{"n_a.cloud", "n_fb.a"}, E{"n_b.cloud", "n_fb.b"}});

  ops::computeCalls().store(0);
  CHECK(runGraphCached(doc).runStatus() == "ok");
  RunLog second = runGraphCached(doc);
  CHECK(second.runStatus() == "ok");
  CHECK(second.nodeEvent("n_fb", "skipped")["stats"]["cached"] == true);
  CHECK(second.nodeEvent("n_b", "skipped")["stats"]["reason"] == "not_demanded");
  CHECK(ops::computeCalls().load() == 0);
}

}  // namespace lyflow::test
