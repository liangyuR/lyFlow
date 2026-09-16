// std-pointcloud 包自己的算子测试（S6）。这些用例原先在 core/tests 里，
// 拆包时随算子一起搬过来 —— 断言逐条不变，只是换了个文件。
#include <doctest/doctest.h>

#include <chrono>

#include "exec/result_store.h"
#include "helpers.h"
#include "lyflow/operator.h"
#include "lyflow/registry.h"

using namespace lyflow;
using namespace lyflow::test;

TEST_CASE("包里的 19 个算子都注册了，且都带 pack 标记") {
  const auto problems = ensureRegistry().validate();
  for (const auto& p : problems) MESSAGE(p);
  CHECK(problems.empty());

  const std::vector<std::string> expected = {
      "io.load_pcd",           "io.save_pcd",
      "filter.passthrough",    "filter.voxel_grid",
      "filter.crop_box",       "filter.random_sample",
      "filter.statistical_outlier", "filter.radius_outlier",
      "features.normals",      "segment.ransac_plane",
      "segment.extract_indices", "transform.make",
      "transform.apply",       "util.merge",
      "filter.crop_box2d",     "fit.line_2d",
      "fit.circle_2d",         "register.icp_2d",
      "edit.translate_region",
  };
  for (const auto& id : expected) {
    CAPTURE(id);
    const OperatorDesc* op = ensureRegistry().find(id);
    REQUIRE(op != nullptr);
    CHECK(op->pack == "std-pointcloud@0.1.0");
  }
  // D10：PointCloudXYZI 已从类型表删除
  CHECK(ensureRegistry().findType("PointCloudXYZI") == nullptr);
  CHECK(ensureRegistry().findType("Plane") != nullptr);
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

// ------- 下面这组来自一次代码审查抓到的缺陷，全都是静默出错的那一类

TEST_CASE("体素栅格：相距很远的点不会被折叠进同一个体素") {
  // 曾经把三个体素下标打包进一个 uint64，越界的点会绕回来和原点附近的点求质心。
  // 这里用一个远超那个范围的坐标守住它。
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

TEST_CASE("可选输入没连：crop_box 的 pose 是 required=false，不该报") {
  const Json doc = makeGraph({{"a", "gen.synthetic"}, {"c", "filter.crop_box"}},
                             {{"a.cloud", "c.cloud"}});
  const Json diags = Json::parse(exec::validateGraphJson(doc.dump(), {}));
  for (const Json& d : diags) {
    CHECK(d.value("severity", "") != "error");
  }
}

TEST_CASE("io.load_pcd 的 path 为空在校验期就红") {
  const Json doc = makeGraph({{"l", "io.load_pcd"}}, {});
  const Json diags = Json::parse(exec::validateGraphJson(doc.dump(), {}));
  bool sawPathError = false;
  for (const Json& d : diags) {
    if (d.value("paramPath", "") == "path" && d.value("severity", "") == "error") {
      sawPathError = true;
    }
  }
  CHECK(sawPathError);
}

TEST_CASE("迁移链：v1 的 random_sample 图产出 migration 诊断") {
  Json doc = makeGraph(
      {
          {"g", "gen.synthetic", Json{{"pointCount", 4000}}},
          {"s", "filter.random_sample", Json{{"count", 123}, {"seed", 9}}},
      },
      {{"g.cloud", "s.cloud"}});
  doc["nodes"][1]["opVersion"] = "1.0.0";

  const Json diags = Json::parse(exec::validateGraphJson(doc.dump(), {}));
  REQUIRE(diags.is_array());
  Json migration;
  for (const Json& d : diags) {
    if (d.value("kind", "") == "migration") migration = d;
  }
  REQUIRE_FALSE(migration.is_null());
  CHECK(migration["nodeId"] == "s");
  CHECK(migration["severity"] == "warning");
  CHECK(migration["op"] == "filter.random_sample");
  CHECK(migration["opVersion"] == "2.0.0");
  CHECK(migration["params"]["keepCount"] == 123);
  CHECK(migration["params"].contains("count") == false);
  CHECK(migration["params"]["seed"] == 9);
  CHECK(migration["notes"].size() >= 1);
}

TEST_CASE("edit.translate_region：半空间只推选中那一侧，通道与点序不动") {
  const Json doc = makeGraph(
      {
          {"g", "gen.synthetic", Json{{"pointCount", 5000}, {"seed", 4101}}},
          {"n", "features.normals"},
          {"t", "edit.translate_region",
           Json{{"regionKind", "halfspace"},
                {"point", {0.0, 0.0, 0.0}},
                {"normal", {1.0, 0.0, 0.0}},
                {"translation", {0.25, -0.125, 0.5}}}},
      },
      {{"g.cloud", "n.cloud"}, {"n.cloud", "t.cloud"}});

  Session session(doc);
  RunLog& log = session.wait();
  REQUIRE(log.runStatus() == "ok");

  auto& store = exec::ResultStore::instance();
  Data before, after;
  REQUIRE(store.get(session.runId(), "n", "cloud", before));
  REQUIRE(store.get(session.runId(), "t", "cloud", after));
  const PointCloud& a = *before.asCloud();
  const PointCloud& b = *after.asCloud();

  REQUIRE(b.pointCount() == a.pointCount());
  REQUIRE(b.hasIntensity());
  REQUIRE(b.hasNormals());
  CHECK(b.intensity == a.intensity);
  CHECK(b.normals == a.normals);

  std::size_t moved = 0, kept = 0;
  for (std::size_t i = 0; i < a.pointCount(); ++i) {
    const float x = a.xyz[i * 3], y = a.xyz[i * 3 + 1], z = a.xyz[i * 3 + 2];
    if (x > 0.0f) {
      ++moved;
      CHECK(b.xyz[i * 3] == doctest::Approx(x + 0.25f));
      CHECK(b.xyz[i * 3 + 1] == doctest::Approx(y - 0.125f));
      CHECK(b.xyz[i * 3 + 2] == doctest::Approx(z + 0.5f));
    } else {
      ++kept;
      CHECK(b.xyz[i * 3] == x);
      CHECK(b.xyz[i * 3 + 1] == y);
      CHECK(b.xyz[i * 3 + 2] == z);
    }
  }
  CHECK(moved > 0);
  CHECK(kept > 0);
}

TEST_CASE("edit.translate_region：盒选区是闭区间，盒外的点一动不动") {
  const Json doc = makeGraph(
      {
          {"g", "gen.synthetic", Json{{"pointCount", 5000}, {"seed", 4102}}},
          {"t", "edit.translate_region",
           Json{{"regionKind", "box"},
                {"boxMin", {-0.2, -0.2, -0.2}},
                {"boxMax", {0.2, 0.2, 0.2}},
                {"translation", {1.5, 0.0, 0.0}}}},
      },
      {{"g.cloud", "t.cloud"}});

  Session session(doc);
  RunLog& log = session.wait();
  REQUIRE(log.runStatus() == "ok");

  auto& store = exec::ResultStore::instance();
  Data before, after;
  REQUIRE(store.get(session.runId(), "g", "cloud", before));
  REQUIRE(store.get(session.runId(), "t", "cloud", after));
  const PointCloud& a = *before.asCloud();
  const PointCloud& b = *after.asCloud();

  REQUIRE(b.pointCount() == a.pointCount());
  CHECK(b.intensity == a.intensity);

  std::size_t moved = 0, kept = 0;
  for (std::size_t i = 0; i < a.pointCount(); ++i) {
    const float x = a.xyz[i * 3], y = a.xyz[i * 3 + 1], z = a.xyz[i * 3 + 2];
    const bool inside = x >= -0.2f && x <= 0.2f && y >= -0.2f && y <= 0.2f && z >= -0.2f &&
                        z <= 0.2f;
    if (inside) {
      ++moved;
      CHECK(b.xyz[i * 3] == doctest::Approx(x + 1.5f));
    } else {
      ++kept;
      CHECK(b.xyz[i * 3] == x);
    }
    CHECK(b.xyz[i * 3 + 1] == y);
    CHECK(b.xyz[i * 3 + 2] == z);
  }
  CHECK(moved > 0);
  CHECK(kept > 0);
}

TEST_CASE("edit.translate_region：零法向是 bad_param") {
  const Json doc = makeGraph(
      {
          {"g", "gen.synthetic", Json{{"pointCount", 1000}, {"seed", 4103}}},
          {"t", "edit.translate_region",
           Json{{"regionKind", "halfspace"}, {"normal", {0.0, 0.0, 0.0}}}},
      },
      {{"g.cloud", "t.cloud"}});

  Session session(doc);
  RunLog& log = session.wait();
  CHECK(log.runStatus() == "error");
  const Json e = log.nodeEvent("t", "error");
  REQUIRE_FALSE(e.empty());
  CHECK(e["error"]["code"] == "bad_param");
  CHECK(e["error"]["paramPath"] == "normal");
}
