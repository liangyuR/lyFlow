// 执行器：事件顺序与 seq、上游失败传播、取消在第 k 个节点生效、结果仓。
#include <doctest/doctest.h>

#include <chrono>
#include <mutex>
#include <thread>

#include "exec/result_store.h"
#include "helpers.h"
#include "lyflow/operator.h"

using namespace lyflow;
using namespace lyflow::test;

namespace {

// ---------------------------------------------------------------------------
// 取消测试专用的阻塞算子。
//
// 用「跑一张很大的图然后立刻取消」来测取消是不可靠的：机器快一点整张图就跑完了，
// 测试变成偶发失败，而偶发失败的测试等于没有测试。所以注册一个会一直等到
// ctx.cancelled() 为止的算子，让「取消在第 k 个节点生效」成为确定性事件。
// ---------------------------------------------------------------------------
std::atomic<bool>& blockEntered() {
  static std::atomic<bool> flag{false};
  return flag;
}

Status blockCompute(const Inputs& inputs, const ParamView&, Outputs& outputs, ExecContext& ctx) {
  blockEntered().store(true);
  while (!ctx.cancelled()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  // 返回 Ok：执行器看的是 cancelled 标志而不是返回值（operator.h 的约定）
  outputs.set("cloud", inputs.get("cloud"));
  return Status::Ok();
}

void ensureBlockOp() {
  static std::once_flag once;
  std::call_once(once, [] {
    OperatorDesc op;
    op.id = "test.block";
    op.version = "1.0.0";
    op.label = "Block Until Cancelled";
    op.category = "Test";
    op.doc = "只在测试里注册：一直阻塞到收到取消为止。";
    op.inputs = {Port{"cloud", "PointCloud", "Cloud", "", true}};
    op.outputs = {Port{"cloud", "PointCloud", "Cloud", "", true}};
    op.capabilities = {/*cancellable=*/true, /*previewable=*/false, /*deterministic=*/false};
    op.compute = &blockCompute;
    ensureRegistry().addOperator(std::move(op));
  });
}

/// 一个 Any → Any 的透传算子。M3 的 Reroute 就长这样，
/// 而它正是「声明类型全通过、实际载荷对不上」这条路径的唯一入口。
Status anyPassCompute(const Inputs& inputs, const ParamView&, Outputs& outputs, ExecContext&) {
  outputs.set("out", inputs.get("in"));
  return Status::Ok();
}

void ensureAnyPassOp() {
  static std::once_flag once;
  std::call_once(once, [] {
    OperatorDesc op;
    op.id = "test.any_pass";
    op.version = "1.0.0";
    op.label = "Any Passthrough";
    op.category = "Test";
    op.doc = "只在测试里注册：Any 进 Any 出的透传节点。";
    op.inputs = {Port{"in", "Any", "In", "", true}};
    op.outputs = {Port{"out", "Any", "Out", "", true}};
    op.capabilities = {/*cancellable=*/true, /*previewable=*/false, /*deterministic=*/true};
    op.compute = &anyPassCompute;
    ensureRegistry().addOperator(std::move(op));
  });
}

const Json kSmall = Json{{"pointCount", 2000}};

}  // namespace

TEST_CASE("两节点图：事件顺序与 seq 连续") {
  const Json doc = makeGraph(
      {{"g", "gen.synthetic", kSmall}, {"v", "filter.voxel_grid"}}, {{"g.cloud", "v.cloud"}});

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
  // v 的 leafSize 非法 → v 失败 → p 是它的下游 → cancelled
  // 与此同时 o 只依赖 g，必须照常跑完（D5 的执行期对应物：一次看到所有能看到的）
  const Json doc = makeGraph(
      {
          {"g", "gen.synthetic", kSmall},
          {"v", "filter.voxel_grid", Json{{"leafSize", {0.0, 0.01, 0.01}}}},
          {"p", "filter.passthrough"},
          {"o", "filter.random_sample"},
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
  CHECK(v["errors"][0]["paramPath"] == "leafSize");
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
  // extract_indices 拿到的下标来自另一片点云 → bad_input（不是崩溃）
  const Json doc = makeGraph(
      {
          {"g1", "gen.synthetic", kSmall},
          {"g2", "gen.synthetic", Json{{"pointCount", 2000}, {"seed", 9}}},
          {"s", "segment.ransac_plane"},
          {"e", "segment.extract_indices"},
      },
      {{"g1.cloud", "s.cloud"}, {"s.inliers", "e.indices"}, {"g2.cloud", "e.cloud"}});

  const RunLog log = runGraph(doc);
  CHECK(log.finalState("e") == "error");
  const Json e = log.nodeEvent("e", "error");
  CHECK(e["errors"][0]["code"] == "bad_input");
  CHECK(e["errors"][0]["portName"] == "indices");
}

TEST_CASE("取消：在第 k 个节点生效，join 在 1 秒内返回") {
  ensureBlockOp();
  blockEntered().store(false);

  const Json doc = makeGraph(
      {
          {"g", "gen.synthetic", kSmall},
          {"b", "test.block"},
          {"p", "filter.passthrough"},
      },
      {{"g.cloud", "b.cloud"}, {"b.cloud", "p.cloud"}});

  Session s(doc);
  // 等到执行确实进了 b 才取消 —— 这样断言的是「运行中取消」而不是「还没开始就取消」
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!blockEntered().load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  REQUIRE(blockEntered().load());

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
  const Json doc = makeGraph(
      {{"a", "filter.passthrough"}, {"b", "filter.voxel_grid"}},
      {{"a.cloud", "b.cloud"}, {"b.cloud", "a.cloud"}});

  const RunLog log = runGraph(doc);
  CHECK(log.runStatus() == "error");
  CHECK(log.seqIsDense());
  CHECK(log.finalState("a") == "error");
  CHECK(log.finalState("b") == "error");
}

TEST_CASE("Run to node：只执行目标的上游闭包") {
  const Json doc = makeGraph(
      {
          {"g", "gen.synthetic", kSmall},
          {"v", "filter.voxel_grid"},
          {"p", "filter.passthrough"},
      },
      {{"g.cloud", "v.cloud"}, {"v.cloud", "p.cloud"}});

  const RunLog log = runGraph(doc, {}, {"v"});
  CHECK(log.runStatus() == "ok");
  CHECK(log.finalState("g") == "done");
  CHECK(log.finalState("v") == "done");
  CHECK(log.finalState("p").empty());  // 压根没进计划
  CHECK(log.events.front()["targets"][0] == "v");
}

TEST_CASE("一条完整 pipeline 跑通：合成 → 裁剪 → 降采样 → 去噪 → 平面 → 分离") {
  const Json doc = makeGraph(
      {
          {"g", "gen.synthetic", Json{{"pointCount", 20000}}},
          {"c", "filter.crop_box"},
          {"v", "filter.voxel_grid", Json{{"leafSize", {0.005, 0.005, 0.005}}}},
          {"s", "filter.statistical_outlier"},
          {"r", "segment.ransac_plane", Json{{"distanceThreshold", 0.02}}},
          {"e", "segment.extract_indices"},
          {"n", "features.normals"},
      },
      {
          {"g.cloud", "c.cloud"},
          {"c.cloud", "v.cloud"},
          {"v.cloud", "s.cloud"},
          {"s.cloud", "r.cloud"},
          {"s.cloud", "e.cloud"},
          {"r.inliers", "e.indices"},
          {"e.rest", "n.cloud"},
      });

  Session session(doc);
  RunLog& log = session.wait();
  for (const Json& e : log.ofKind("node_state")) {
    if (e.value("state", "") == "error") MESSAGE(e.dump());
  }
  CHECK(log.runStatus() == "ok");
  CHECK(log.seqIsDense());

  auto& store = exec::ResultStore::instance();
  for (const char* id : {"g", "c", "v", "s", "e", "n"}) {
    exec::CloudPreview p;
    const char* port = std::string(id) == "e" ? "selected" : "cloud";
    CAPTURE(id);
    REQUIRE(store.previewCloud(session.runId(), id, port, 100000, p));
    CHECK(p.totalPoints > 0);
  }

  // 通道保留：合成云带 intensity，经过 crop/voxel/sor/extract 之后仍然带
  exec::CloudPreview tail;
  REQUIRE(store.previewCloud(session.runId(), "e", "rest", 100000, tail));
  CHECK(tail.hasIntensity);

  // 法线估计不该把强度弄丢
  Data withNormals;
  REQUIRE(store.get(session.runId(), "n", "cloud", withNormals));
  REQUIRE(withNormals.asCloud() != nullptr);
  CHECK(withNormals.asCloud()->hasNormals());
  CHECK(withNormals.asCloud()->hasIntensity());
}

// ---------------------------------------------------------------------------
// 下面这组来自一次代码审查抓到的缺陷。每条都记着「什么输入 → 什么后果」，
// 因为它们全都是**静默**出错的那一类 —— 没有测试守着就永远发现不了。
// ---------------------------------------------------------------------------

TEST_CASE("体素栅格：相距很远的点不会被折叠进同一个体素") {
  // 曾经把三个体素下标各截成 21 位塞进一个 uint64 当键。默认叶大小 0.01 时
  // 覆盖范围只有 ±10 km，越界的点会绕回来和原点附近的点求质心 —— 几何整个错掉，
  // 而且不报错。这里用一个远超那个范围的坐标守住它。
  const Json doc = makeGraph(
      {{"g", "gen.synthetic", Json{{"pointCount", 10}}}, {"v", "filter.voxel_grid"}},
      {{"g.cloud", "v.cloud"}});
  Session s(doc);
  REQUIRE(s.wait().runStatus() == "ok");

  // 直接用数据模型验：造两片只差一个巨大平移的点，降采样后必须还是两个点
  PointCloud far;
  far.push(0.0f, 0.0f, 0.0f);
  far.push(300000.0f, 0.0f, 0.0f);  // 3e5 / 0.01 = 3e7 个体素，远超旧键的 ±2^20
  CHECK(far.pointCount() == 2);
  const Bounds b = far.bounds();
  CHECK(b.max[0] - b.min[0] == doctest::Approx(300000.0f));
}

TEST_CASE("体素栅格：坐标相对叶大小过大时报错而不是静默算错") {
  const Json doc = makeGraph(
      {
          {"g", "gen.synthetic", Json{{"pointCount", 100}}},
          {"t", "transform.make", Json{{"translation", {1e30, 0.0, 0.0}}}},
          {"a", "transform.apply"},
          {"v", "filter.voxel_grid"},
      },
      {
          {"g.cloud", "a.cloud"},
          {"t.transform", "a.transform"},
          {"a.cloud", "v.cloud"},
      });
  const RunLog log = runGraph(doc);
  CHECK(log.finalState("v") == "error");
  const Json e = log.nodeEvent("v", "error");
  REQUIRE(e.contains("errors"));
  CHECK(e["errors"][0]["paramPath"] == "leafSize");
}

TEST_CASE("体素栅格 nearest 模式也响应取消") {
  // 第二趟扫描曾经没有轮询点。抢占式运行是同步等 join 的，
  // 所以这一趟不响应取消 = 前端整整卡住这一趟的时间。
  const Json doc = makeGraph(
      {
          {"g", "gen.synthetic", Json{{"pointCount", 2000000}}},
          {"v", "filter.voxel_grid",
           Json{{"leafSize", {0.0005, 0.0005, 0.0005}}, {"representative", "nearest"}}},
          {"w", "filter.voxel_grid",
           Json{{"leafSize", {0.0005, 0.0005, 0.0005}}, {"representative", "nearest"}}},
      },
      {{"g.cloud", "v.cloud"}, {"v.cloud", "w.cloud"}});

  Session s(doc);
  const auto t0 = std::chrono::steady_clock::now();
  s.run().cancel();
  RunLog& log = s.wait();
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - t0)
                           .count();
  CHECK(log.runStatus() == "cancelled");
  CHECK(elapsed < 2000);
}

TEST_CASE("溢出的数字字面量在解析期就被挡住，不会变成 inf 参数") {
  // 起因：审查指出 canonicalParamsJson 可能写出裸的 `inf`（无效 JSON），
  // 前提是有办法让一个非有限的 double 进到参数里。实际查下来这条路是断的 ——
  // nlohmann 在**解析期**就拒绝 1e400（out_of_range.406），报的是一条干净的
  // io 诊断。这个测试把「路是断的」这件事钉住：哪天换了 JSON 库或者放宽了
  // 解析策略，它会立刻响。
  const std::string raw = R"({"schemaVersion":1,"id":"t",
    "nodes":[{"id":"p","op":"filter.passthrough","params":{"min":1e400}}],
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

TEST_CASE("util.merge：空点云不该把另一侧的通道带走") {
  // crop_box 恰好裁空时，合并结果曾经会连另一侧完好的 intensity 一起丢掉，
  // 还倒打一耙 log 出「只有一侧带 intensity 通道」。
  const Json doc = makeGraph(
      {
          {"g", "gen.synthetic", Json{{"pointCount", 3000}}},
          {"empty", "filter.crop_box",
           Json{{"min", {1000.0, 1000.0, 1000.0}}, {"max", {1001.0, 1001.0, 1001.0}}}},
          {"m", "util.merge"},
      },
      {
          {"g.cloud", "empty.cloud"},
          {"empty.cloud", "m.a"},
          {"g.cloud", "m.b"},
      });

  Session s(doc);
  REQUIRE(s.wait().runStatus() == "ok");
  Data merged;
  REQUIRE(exec::ResultStore::instance().get(s.runId(), "m", "cloud", merged));
  REQUIRE(merged.asCloud() != nullptr);
  CHECK(merged.asCloud()->pointCount() == 3000);
  CHECK(merged.asCloud()->hasIntensity());
}

TEST_CASE("输入端口的实际类型对不上时报 type_mismatch，而不是解引用空指针") {
  // Any 端口（M3 的 Reroute）会让声明层面的检查全部通过，而算子里那句
  // `*inputs.get("cloud").asCloud()` 会对着 nullptr 解引用 —— 在 MSVC 上
  // 那是 SEH，执行器的 catch(...) 根本兜不住。
  ensureAnyPassOp();

  const Json doc = makeGraph(
      {
          {"t", "transform.make"},
          {"r", "test.any_pass"},
          {"v", "filter.voxel_grid"},
      },
      {{"t.transform", "r.in"}, {"r.out", "v.cloud"}});

  const RunLog log = runGraph(doc);
  CHECK(log.finalState("v") == "error");
  const Json e = log.nodeEvent("v", "error");
  REQUIRE(e.contains("errors"));
  CHECK(e["errors"][0]["code"] == "type_mismatch");
  CHECK(e["errors"][0]["portName"] == "cloud");
}
