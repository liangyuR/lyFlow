// 执行器：事件顺序与 seq、上游失败传播、取消在第 k 个节点生效、结果仓。
// 算子一律用 gen.synthetic 与 test.*（S6）—— 点云算法的测试在 packs/std-pointcloud/tests。
#include <doctest/doctest.h>

#include <chrono>
#include <thread>

#include "exec/result_store.h"
#include "helpers.h"
#include "lyflow/operator.h"
#include "test_ops.h"

using namespace lyflow;
using namespace lyflow::test;

namespace {

const Json kSmall = Json{{"pointCount", 2000}};
const Json kBadLeaf = Json{{"leaf", {0.0, 0.01, 0.01}}};

}  // namespace

TEST_CASE("两节点图：事件顺序与 seq 连续") {
  ensureTestOps();
  const Json doc =
      makeGraph({{"g", "gen.synthetic", kSmall}, {"v", "test.thin"}}, {{"g.cloud", "v.cloud"}});

  Session s(doc);
  RunLog& log = s.wait();

  CHECK(log.seqIsDense());
  REQUIRE(log.events.size() >= 6);
  CHECK(log.events.front().value("kind", "") == "run_started");
  CHECK(log.events.back().value("kind", "") == "run_finished");
  CHECK(log.runStatus() == "ok");

  // run_started 带上 M3 要用的编译结果：cacheKey 与 level
  const Json started = log.events.front();
  CHECK(started.value("nodeCount", 0) == 2);
  REQUIRE(started["nodes"].size() == 2);
  CHECK(started["nodes"][0]["id"] == "g");
  CHECK(started["nodes"][0]["cacheKey"].get<std::string>().size() == 32);
  CHECK(started["nodes"][1]["level"].get<int>() == 1);
  CHECK(started["targets"].empty());

  // 每个节点 pending → running → done，且 g 全程排在 v 之前
  std::vector<std::string> trace;
  for (const Json& e : log.events) {
    if (e.value("kind", "") == "node_state") {
      trace.push_back(e.value("nodeId", "") + ":" + e.value("state", ""));
    }
  }
  const std::vector<std::string> expected = {"g:pending", "g:running", "g:done",
                                             "v:pending", "v:running", "v:done"};
  CHECK(trace == expected);

  const Json done = log.nodeEvent("v", "done");
  CHECK(done["stats"]["elementCount"].get<int>() > 0);
  CHECK(done["stats"]["outputs"][0]["type"] == "PointCloud");
  CHECK(done["durationMs"].get<double>() >= 0.0);
}

TEST_CASE("结果仓：抽样点数、总数与 bounds") {
  const Json doc = makeGraph({{"g", "gen.synthetic", Json{{"pointCount", 5000}}}}, {});
  Session s(doc);
  s.wait();

  auto& store = exec::ResultStore::instance();

  exec::CloudPreview full;
  REQUIRE(store.previewCloud(s.runId(), "g", "cloud", 0, full));
  CHECK(full.totalPoints == 5000);
  CHECK(full.pointCount == 5000);
  CHECK(full.hasIntensity);
  CHECK(full.intensity.size() == full.pointCount);

  exec::CloudPreview sampled;
  REQUIRE(store.previewCloud(s.runId(), "g", "cloud", 1000, sampled));
  CHECK(sampled.totalPoints == 5000);
  CHECK(sampled.pointCount <= 1000);
  CHECK(sampled.pointCount > 900);
  CHECK(sampled.xyz.size() == sampled.pointCount * 3u);

  // bounds 必须用全量点云算：抽稀不该让 fit-to-bounds 漏掉边角
  for (int i = 0; i < 3; ++i) {
    CHECK(sampled.bounds[i] == doctest::Approx(full.bounds[i]));
    CHECK(sampled.bounds[i + 3] == doctest::Approx(full.bounds[i + 3]));
    CHECK(sampled.bounds[i + 3] > sampled.bounds[i]);
  }

  // 不存在的节点 / 端口
  exec::CloudPreview none;
  CHECK_FALSE(store.previewCloud(s.runId(), "nope", "cloud", 100, none));
  CHECK_FALSE(store.previewCloud("no-such-run", "g", "cloud", 100, none));

  const auto infos = store.outputsOf(s.runId(), "g");
  REQUIRE(infos.size() == 1);
  CHECK(infos[0].port == "cloud");
  CHECK(infos[0].type == "PointCloud");
  CHECK(infos[0].elementCount == 5000);
}

TEST_CASE("run_free 之后结果仓不再持有该 run 的数据") {
  std::string runId;
  {
    const Json doc = makeGraph({{"g", "gen.synthetic", kSmall}}, {});
    Session s(doc);
    s.wait();
    runId = s.runId();
    exec::CloudPreview alive;
    CHECK(exec::ResultStore::instance().previewCloud(runId, "g", "cloud", 10, alive));
  }
  // Session 析构 → Run 析构 → freeRun。内存上界靠的就是这一条。
  exec::CloudPreview gone;
  CHECK_FALSE(exec::ResultStore::instance().previewCloud(runId, "g", "cloud", 10, gone));
}

TEST_CASE("上游失败：下游标 cancelled + upstream_failed，旁支照常执行") {
  ensureTestOps();
  // v 的 leaf 非法 → v 失败 → p 是它的下游 → cancelled
  // 与此同时 o 只依赖 g，必须照常跑完（D5 的执行期对应物：一次看到所有能看到的）
  const Json doc = makeGraph(
      {
          {"g", "gen.synthetic", kSmall},
          {"v", "test.thin", kBadLeaf},
          {"p", "test.thin"},
          {"o", "test.half_indices"},
      },
      {{"g.cloud", "v.cloud"}, {"v.cloud", "p.cloud"}, {"g.cloud", "o.cloud"}});

  const RunLog log = runGraph(doc);
  CHECK(log.seqIsDense());
  CHECK(log.finalState("g") == "done");
  CHECK(log.finalState("v") == "error");
  CHECK(log.finalState("p") == "cancelled");
  CHECK(log.finalState("o") == "done");
  CHECK(log.runStatus() == "error");

  const Json v = log.nodeEvent("v", "error");
  REQUIRE(v.contains("errors"));
  CHECK(v["errors"][0]["paramPath"] == "leaf");
  // error 是 errors[0] 的快捷方式，两者必须一致
  CHECK(v["error"] == v["errors"][0]);

  const Json p = log.nodeEvent("p", "cancelled");
  CHECK(p["errors"][0]["code"] == "upstream_failed");
  // skipped 严格保留给缓存命中（M3），这里绝不能出现
  for (const Json& e : log.events) {
    CHECK(e.value("state", "") != "skipped");
  }
}

TEST_CASE("算子内部异常被兜住，转成 internal 而不是穿过 ABI") {
  ensureTestOps();
  // split 拿到的下标来自另一片点云 → bad_input（不是崩溃）
  const Json doc = makeGraph(
      {
          {"g1", "gen.synthetic", kSmall},
          {"g2", "gen.synthetic", Json{{"pointCount", 2000}, {"seed", 9}}},
          {"s", "test.half_indices"},
          {"e", "test.split"},
      },
      {{"g1.cloud", "s.cloud"}, {"s.indices", "e.indices"}, {"g2.cloud", "e.cloud"}});

  const RunLog log = runGraph(doc);
  CHECK(log.finalState("e") == "error");
  const Json e = log.nodeEvent("e", "error");
  CHECK(e["errors"][0]["code"] == "bad_input");
  CHECK(e["errors"][0]["portName"] == "indices");
}

TEST_CASE("声明了输出端口却不写：output_not_written，消息带端口名") {
  ensureTestOps();
  const Json doc = makeGraph(
      {
          {"g", "gen.synthetic", kSmall},
          {"n", "test.no_output"},
      },
      {{"g.cloud", "n.cloud"}});

  const RunLog log = runGraph(doc);
  CHECK(log.finalState("n") == "error");
  const Json e = log.nodeEvent("n", "error");
  CHECK(e["errors"][0]["code"] == "output_not_written");
  CHECK(e["errors"][0]["portName"] == "cloud");
  CHECK(e["errors"][0]["message"].get<std::string>().find("cloud") != std::string::npos);
}

TEST_CASE("取消：在第 k 个节点生效，join 在 1 秒内返回") {
  ensureTestOps();
  ops::blockEntered().store(false);

  const Json doc = makeGraph(
      {
          {"g", "gen.synthetic", kSmall},
          {"b", "test.block"},
          {"p", "test.thin"},
      },
      {{"g.cloud", "b.cloud"}, {"b.cloud", "p.cloud"}});

  Session s(doc);
  // 等到执行确实进了 b 才取消 —— 这样断言的是「运行中取消」而不是「还没开始就取消」
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!ops::blockEntered().load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  REQUIRE(ops::blockEntered().load());

  const auto t0 = std::chrono::steady_clock::now();
  s.run().cancel();
  RunLog& log = s.wait();
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - t0)
                           .count();

  CHECK(elapsed < 1000);
  CHECK(log.seqIsDense());
  CHECK(log.finalState("g") == "done");   // 取消之前已经跑完的节点保持 done
  CHECK(log.finalState("b") == "cancelled");
  CHECK(log.finalState("p") == "cancelled");
  CHECK(log.runStatus() == "cancelled");
}

TEST_CASE("整图级失败（环）：run_finished 是 error，节点仍然标红") {
  ensureTestOps();
  const Json doc = makeGraph({{"a", "test.thin"}, {"b", "test.thin"}},
                             {{"a.cloud", "b.cloud"}, {"b.cloud", "a.cloud"}});

  const RunLog log = runGraph(doc);
  CHECK(log.runStatus() == "error");
  CHECK(log.seqIsDense());
  CHECK(log.finalState("a") == "error");
  CHECK(log.finalState("b") == "error");
}

TEST_CASE("Run to node：只执行目标的上游闭包") {
  ensureTestOps();
  const Json doc = makeGraph(
      {
          {"g", "gen.synthetic", kSmall},
          {"v", "test.thin"},
          {"p", "test.thin", Json{{"leaf", {0.05, 0.05, 0.05}}}},
      },
      {{"g.cloud", "v.cloud"}, {"v.cloud", "p.cloud"}});

  const RunLog log = runGraph(doc, {}, {"v"});
  CHECK(log.runStatus() == "ok");
  CHECK(log.finalState("g") == "done");
  CHECK(log.finalState("v") == "done");
  CHECK(log.finalState("p").empty());  // 压根没进计划
  CHECK(log.events.front()["targets"][0] == "v");
}

TEST_CASE("溢出的数字字面量在解析期就被挡住，不会变成 inf 参数") {
  ensureTestOps();
  // nlohmann 在解析期就拒绝 1e400（out_of_range.406），所以非有限 double 进不到参数里。
  // 这条把「路是断的」钉住：换 JSON 库或放宽解析策略时它会立刻响。
  const std::string raw = R"({"schemaVersion":1,"id":"t",
    "nodes":[{"id":"p","op":"test.thin","params":{"leaf":1e400}}],
    "edges":[]})";
  const auto parsed = Json::parse(exec::validateGraphJson(raw, {}));
  REQUIRE(parsed.size() >= 1);
  CHECK(parsed[0]["code"] == "io");
  CHECK(parsed[0]["severity"] == "error");

  // 另一半防线在序列化侧：万一有算子作者把 min 写成 infinity，
  // 导出的 manifest 也必须是合法 JSON（见 test_data.cpp 的 jsonNumber 用例）。
  const std::string manifest = ensureRegistry().toManifestJson();
  CHECK(manifest.find("inf") == std::string::npos);
  CHECK(manifest.find("nan") == std::string::npos);
}

TEST_CASE("输入端口的实际类型对不上时报 type_mismatch，而不是解引用空指针") {
  // Any 端口会让声明层面的检查全部通过，而算子里的 asCloud() 会返回 nullptr。
  // 解引用它在 MSVC 上是 SEH，执行器的 catch(...) 兜不住。
  ensureTestOps();

  const Json doc = makeGraph(
      {
          {"g", "gen.synthetic", kSmall},
          {"h", "test.half_indices"},
          {"r", "test.any_pass"},
          {"v", "test.thin"},
      },
      {{"g.cloud", "h.cloud"}, {"h.indices", "r.in"}, {"r.out", "v.cloud"}});

  const RunLog log = runGraph(doc);
  CHECK(log.finalState("v") == "error");
  const Json e = log.nodeEvent("v", "error");
  REQUIRE(e.contains("errors"));
  CHECK(e["errors"][0]["code"] == "type_mismatch");
  CHECK(e["errors"][0]["portName"] == "cloud");
}
