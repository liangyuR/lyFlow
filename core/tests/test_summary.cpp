// run summary（ADR-0022 / m6-plan §1）：status 三态、outputs 三态、decisions 收集、
// failed 输出的 from 回溯。只用 core 自带的算子与 test.* 造图。
#include <doctest/doctest.h>

#include "exec/executor.h"
#include "exec/result_store.h"
#include "helpers.h"
#include "test_ops.h"

namespace lyflow::test {
namespace {

Json withOutputs(const std::vector<N>& nodes, const std::vector<E>& edges, const Json& outputs) {
  Json doc = makeGraph(nodes, edges);
  doc["outputs"] = outputs;
  return doc;
}

/// summary 有两个出口：run_finished 事件里那一份、lyflow_run_summary 那一份。
/// H1 说它们是同一个对象，所以每个用例都顺手对一次。
Json summaryOf(Session& s, RunLog& log) {
  const auto finished = log.ofKind("run_finished");
  REQUIRE(finished.size() == 1);
  REQUIRE(finished.back().contains("summary"));
  const Json fromEvent = finished.back()["summary"];

  std::string raw;
  REQUIRE(exec::runSummaryJson(s.runId(), raw));
  CHECK(Json::parse(raw) == fromEvent);
  return fromEvent;
}

}  // namespace

TEST_CASE("summary：全绿的一轮是 ok，声明输出是 value") {
  ensureTestOps();
  const Json doc = withOutputs(
      {N{"n_src", "test.counted", Json{{"pointCount", 6}}},
       N{"n_thin", "test.thin", Json{{"leaf", Json::array({0.01, 0.01, 0.01})}}}},
      {E{"n_src.cloud", "n_thin.cloud"}},
      Json{{"cloud", Json{{"node", "n_thin"}, {"port", "cloud"}}}});
  Session s(doc);
  RunLog& log = s.wait();
  CHECK(log.runStatus() == "ok");

  const Json summary = summaryOf(s, log);
  CHECK(summary["runId"] == s.runId());
  CHECK(summary["status"] == "ok");
  CHECK(summary["durationMs"].is_number());
  CHECK(summary["nodes"]["n_src"]["state"] == "done");
  CHECK(summary["nodes"]["n_src"]["cached"] == false);
  CHECK(summary["nodes"]["n_thin"]["outputsAvailable"] == true);
  CHECK(summary["outputs"]["cloud"]["state"] == "value");
  CHECK(summary["outputs"]["cloud"]["node"] == "n_thin");
  CHECK(summary["outputs"]["cloud"]["port"] == "cloud");
  CHECK(summary["outputs"]["cloud"]["type"] == "PointCloud");
  CHECK(summary["outputs"]["cloud"]["elementCount"] == 6);
  CHECK(summary["decisions"].empty());
  CHECK(summary["contractViolations"].is_array());
  CHECK(summary["contractViolations"].empty());
}

TEST_CASE("summary：有节点失败但声明输出都拿到了值 → degraded") {
  ensureTestOps();
  // 主路径炸了，fallback 走 b 把结果量出来：run_finished 是 ok，
  // 但「这一轮有东西坏了」必须说出来 —— 这正是 degraded 存在的理由（H2）。
  const Json doc = withOutputs(
      {N{"n_a", "test.fail", Json{{"message", "主路径炸了"}}},
       N{"n_b", "test.counted", Json{{"pointCount", 5}}},
       N{"n_fb", "flow.fallback", Json::object()}},
      {E{"n_a.cloud", "n_fb.a"}, E{"n_b.cloud", "n_fb.b"}},
      Json{{"result", Json{{"node", "n_fb"}, {"port", "out"}}}});
  Session s(doc);
  RunLog& log = s.wait();
  CHECK(log.runStatus() == "ok");

  const Json summary = summaryOf(s, log);
  CHECK(summary["status"] == "degraded");
  CHECK(summary["nodes"]["n_a"]["state"] == "error");
  CHECK(summary["nodes"]["n_a"]["code"] == "io");
  CHECK(summary["outputs"]["result"]["state"] == "value");
  CHECK(summary["outputs"]["result"]["elementCount"] == 5);

  // decisions 按 Record 的 type 收，不按算子 id（H4）
  REQUIRE(summary["decisions"].contains("n_fb"));
  CHECK(summary["decisions"]["n_fb"]["type"] == "FallbackChoice");
  CHECK(summary["decisions"]["n_fb"]["port"] == "choice");
  CHECK(summary["decisions"]["n_fb"]["choice"] == "b");
  CHECK(summary["decisions"]["n_fb"]["reason"].get<std::string>().find("主路径炸了") !=
        std::string::npos);
}

TEST_CASE("summary：声明输出崩了 → failed，from 沿边回溯到最近的 error 节点；只有一个错时 root 就是 from") {
  ensureTestOps();
  // n_a 失败 → n_t1 → n_t2 连坐（cancelled/upstream_failed）。
  // 输出挂在 n_t2 上，from 必须指回真正出错的 n_a 而不是它自己。
  const Json doc = withOutputs(
      {N{"n_a", "test.fail", Json{{"code", "insufficient_points"}, {"message", "点太少"}}},
       N{"n_t1", "test.thin", Json::object()},
       N{"n_t2", "test.thin", Json::object()}},
      {E{"n_a.cloud", "n_t1.cloud"}, E{"n_t1.cloud", "n_t2.cloud"}},
      Json{{"flush", Json{{"node", "n_t2"}, {"port", "cloud"}}}});
  Session s(doc);
  RunLog& log = s.wait();
  CHECK(log.runStatus() == "error");

  const Json summary = summaryOf(s, log);
  CHECK(summary["status"] == "failed");
  CHECK(summary["outputs"]["flush"]["state"] == "failed");
  CHECK(summary["outputs"]["flush"]["from"] == "n_a");
  CHECK(summary["outputs"]["flush"]["code"] == "insufficient_points");
  // 整条链只有一个错时 root 就是 from
  CHECK(summary["outputs"]["flush"]["root"] == "n_a");
  CHECK(summary["outputs"]["flush"]["rootCode"] == "insufficient_points");
  CHECK_FALSE(summary["outputs"]["flush"].contains("value"));
  // nodes 里全部 error 都留着，不因为回溯只取一个就丢信息（§9）
  CHECK(summary["nodes"]["n_t1"]["state"] == "cancelled");
  CHECK(summary["nodes"]["n_t2"]["state"] == "cancelled");
}

TEST_CASE("summary：from 是最近的出错节点，root 是失败链上拓扑序最早的那个") {
  ensureTestOps();
  // 真实那张图的形状（m6-plan §10 第 4 条）：两路都炸了，fallback 自己也 error。
  //   n_a(fail) → n_t1(cancelled) ┐
  //                               ├→ n_fb(error) → 输出
  //   n_b(fail) → n_t2(cancelled) ┘
  // `from` 是 n_fb（0 跳，它自己就是 error），而真正要查的根因在两跳外。
  const Json doc = withOutputs(
      {N{"n_a", "test.fail", Json{{"code", "insufficient_points"}, {"message", "主路径点太少"}}},
       N{"n_b", "test.fail", Json{{"code", "icp_score_low"}, {"message", "模板配不上"}}},
       N{"n_t1", "test.thin", Json::object()},
       N{"n_t2", "test.thin", Json::object()},
       N{"n_fb", "flow.fallback", Json::object()}},
      {E{"n_a.cloud", "n_t1.cloud"}, E{"n_b.cloud", "n_t2.cloud"},
       E{"n_t1.cloud", "n_fb.a"}, E{"n_t2.cloud", "n_fb.b"}},
      Json{{"gap", Json{{"node", "n_fb"}, {"port", "out"}}}});
  Session s(doc);
  RunLog& log = s.wait();

  const Json summary = summaryOf(s, log);
  CHECK(summary["status"] == "failed");
  const Json gap = summary["outputs"]["gap"];
  CHECK(gap["state"] == "failed");
  // 最近的：fallback 自己
  CHECK(gap["from"] == "n_fb");
  // 根因：沿失败链（只穿 error/cancelled）往上走，取拓扑序最早的 error。
  // n_a 与 n_b 都是 error，n_a 的下标更小。
  CHECK(gap["root"] == "n_a");
  CHECK(gap["rootCode"] == "insufficient_points");
  CHECK(gap["from"] != gap["root"]);
  // 中间那两个是 cancelled：回溯穿过它们，但不会挑中它们当 root
  CHECK(summary["nodes"]["n_t1"]["state"] == "cancelled");
  CHECK(summary["nodes"]["n_t2"]["state"] == "cancelled");
  CHECK(summary["nodes"]["n_b"]["state"] == "error");
}

TEST_CASE("summary：没被 demand 的惰性分支上的输出是 inactive，不是 failed") {
  ensureTestOps();
  // 主路径成功 → b 的闭包一次都不跑。挂在 b 上的那一维「本来就没有」，
  // 与「本该有、崩了」是两件事（H3）。
  const Json doc = withOutputs(
      {N{"n_a", "test.counted", Json{{"pointCount", 4}}},
       N{"n_b", "test.counted", Json{{"pointCount", 7}}},
       N{"n_fb", "flow.fallback", Json::object()}},
      {E{"n_a.cloud", "n_fb.a"}, E{"n_b.cloud", "n_fb.b"}},
      Json{{"gap", Json{{"node", "n_fb"}, {"port", "out"}}},
           {"flush", Json{{"node", "n_b"}, {"port", "cloud"}}}});
  Session s(doc);
  RunLog& log = s.wait();
  CHECK(log.runStatus() == "ok");

  const Json summary = summaryOf(s, log);
  CHECK(summary["outputs"]["gap"]["state"] == "value");
  CHECK(summary["outputs"]["flush"]["state"] == "inactive");
  CHECK(summary["outputs"]["flush"]["reason"] == "not_demanded");
  CHECK(summary["nodes"]["n_b"]["state"] == "skipped");
  CHECK(summary["nodes"]["n_b"]["reason"] == "not_demanded");
  CHECK(summary["nodes"]["n_b"]["outputsAvailable"] == false);
  // 一个节点都没 error/cancelled：inactive 不拉低 status（H2）
  CHECK(summary["status"] == "ok");
}

TEST_CASE("summary：decisions 收全图每一个 FallbackChoice") {
  ensureTestOps();
  // 两级 fallback：第一级走 b（主路径炸了），第二级走 a（上一级给出了结果）。
  const Json doc = withOutputs(
      {N{"n_a", "test.fail", Json::object()},
       N{"n_b", "test.counted", Json{{"pointCount", 3}}},
       N{"n_fb1", "flow.fallback", Json::object()},
       N{"n_r", "util.reroute", Json::object()},
       N{"n_c", "test.counted", Json{{"pointCount", 9}}},
       N{"n_fb2", "flow.fallback", Json::object()}},
      {E{"n_a.cloud", "n_fb1.a"}, E{"n_b.cloud", "n_fb1.b"}, E{"n_fb1.out", "n_r.in"},
       E{"n_r.out", "n_fb2.a"}, E{"n_c.cloud", "n_fb2.b"}},
      Json{{"result", Json{{"node", "n_fb2"}, {"port", "out"}}}});
  Session s(doc);
  RunLog& log = s.wait();

  const Json summary = summaryOf(s, log);
  CHECK(summary["decisions"].size() == 2);
  CHECK(summary["decisions"]["n_fb1"]["choice"] == "b");
  CHECK(summary["decisions"]["n_fb2"]["choice"] == "a");
  CHECK(summary["outputs"]["result"]["state"] == "value");
  CHECK(summary["outputs"]["result"]["elementCount"] == 3);
  CHECK(summary["status"] == "degraded");
}

TEST_CASE("summary：图没声明 outputs 时，有节点 error 就是 failed") {
  ensureTestOps();
  const Json doc = makeGraph({N{"n_a", "test.fail", Json::object()}}, {});
  Session s(doc);
  RunLog& log = s.wait();

  const Json summary = summaryOf(s, log);
  CHECK(summary["status"] == "failed");
  CHECK(summary["outputs"].empty());
  CHECK(summary["nodes"]["n_a"]["state"] == "error");
}

TEST_CASE("summary：run 结束之前取不到，free 之后也取不到") {
  ensureTestOps();
  const Json doc = makeGraph({N{"n_b", "test.block", Json::object()},
                              N{"n_src", "test.counted", Json{{"pointCount", 2}}}},
                             {E{"n_src.cloud", "n_b.cloud"}});
  ops::blockEntered().store(false);
  std::string raw;
  std::string runId;
  {
    Session s(doc);
    runId = s.runId();
    // 5 秒兜底：执行没进 b 就直接判失败，而不是整个测试进程挂在这里
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!ops::blockEntered().load() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(ops::blockEntered().load());
    CHECK_FALSE(exec::runSummaryJson(runId, raw));
    s.run().cancel();
    s.wait();
    CHECK(exec::runSummaryJson(runId, raw));
    // 整轮被取消：什么都不能信，status 直接 failed
    CHECK(Json::parse(raw)["status"] == "failed");
  }
  // Session 析构 → Run 析构 → freeRun：summary 跟着索引一起走
  CHECK_FALSE(exec::runSummaryJson(runId, raw));
}

}  // namespace lyflow::test
