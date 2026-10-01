// std-pointcloud 包自己的算子测试（S6）。这些用例原先在 core/tests 里，
// 拆包时随算子一起搬过来 —— 断言逐条不变，只是换了个文件。
#include <doctest/doctest.h>

#include <pcl/features/normal_3d.h>
#include <pcl/filters/radius_outlier_removal.h>
#include <pcl/filters/statistical_outlier_removal.h>
#include <pcl/search/kdtree.h>

#include <chrono>
#include <cmath>

#include "exec/result_store.h"
#include "helpers.h"
#include "lyflow/operator.h"
#include "lyflow/registry.h"
#include "lyflow_pcl/adapter.h"

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
  // 这里把一片只差巨大平移的点真的喂给 voxel_grid（默认叶大小 0.01），降采样后必须一点一格。
  PointCloud far;
  far.push(0.0f, 0.0f, 0.0f);
  far.push(300000.0f, 0.0f, 0.0f);  // 3e5 / 0.01 = 3e7 个体素，远超旧键的 ±2^20
  // 正好差 2^20、2^21 个体素（取体素中点，免得浮点落在格边上）：
  // 按 20 / 21 位打包的键绕回来恰好就落在原点那一格
  far.push(10485.765f, 0.0f, 0.0f);
  far.push(20971.525f, 0.0f, 0.0f);
  const std::vector<float> xs = {0.0f, 300000.0f, 10485.765f, 20971.525f};

  // voxel_grid 的输入与输出端口同名（cloud），直接注入会被当成整节点注入、compute 被跳过；
  // 所以经 util.merge 的输入端口注入（另一侧给空云），让 voxel_grid 真跑一遍
  const Json doc = makeGraph({{"m", "util.merge"}, {"v", "filter.voxel_grid"}},
                             {{"m.cloud", "v.cloud"}});
  std::vector<exec::InjectedInput> inputs{
      exec::InjectedInput{"m", "a", Data::cloud(far)},
      exec::InjectedInput{"m", "b", Data::cloud(PointCloud{})}};
  Session s(doc, {}, {}, /*keepCache=*/false, /*maxParallel=*/0, /*noReuse=*/false,
            std::move(inputs));
  RunLog& log = s.wait();
  for (const Json& e : log.ofKind("node_state")) {
    if (e.value("state", "") == "error") MESSAGE(e.dump());
  }
  REQUIRE(log.runStatus() == "ok");
  REQUIRE(log.finalState("v") == "done");

  Data out;
  REQUIRE(exec::ResultStore::instance().get(s.runId(), "v", "cloud", out));
  REQUIRE(out.asCloud() != nullptr);
  const PointCloud& voxels = *out.asCloud();
  REQUIRE(voxels.pointCount() == xs.size());
  // 一格一个点，质心就是那个点本身：谁也没被拉去和原点求平均
  for (std::size_t i = 0; i < xs.size(); ++i) {
    CAPTURE(i);
    CHECK(voxels.xyz[i * 3] == doctest::Approx(xs[i]));
    CHECK(voxels.xyz[i * 3 + 1] == doctest::Approx(0.0f));
    CHECK(voxels.xyz[i * 3 + 2] == doctest::Approx(0.0f));
  }
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

// 法线与两个离群点滤波改成按线程预算分段并行（ops/parallel.h；vcpkg 的 PCL 没开 OpenMP，它们原来是单线程的
// PCL 实现，200 万点上 12 s / 14 s / 7 s）。参照就是原来直接调的那三个 PCL 类：逐点结果要相同，换几个线程也一样
TEST_CASE("法线与两个离群点滤波：分段并行的结果与 PCL 原实现逐点相同，与线程数无关") {
  test::OpCall gen;
  REQUIRE(gen.run("gen.synthetic", {{"pointCount", Value::integer(60000)}, {"seed", Value::integer(5)}}).ok);
  const Data cloud = gen.out("cloud");
  const PointCloud& in = *cloud.asCloud();
  const auto pc = ops::adapter::toPcl(in);

  pcl::Indices sorKept;
  {
    pcl::StatisticalOutlierRemoval<pcl::PointXYZ> f;
    f.setInputCloud(pc);
    f.setMeanK(30);
    f.setStddevMulThresh(1.0);
    f.filter(sorKept);
  }
  pcl::Indices rorKept;
  {
    pcl::RadiusOutlierRemoval<pcl::PointXYZ> f;
    f.setInputCloud(pc);
    f.setRadiusSearch(0.05);
    f.setMinNeighborsInRadius(5);
    f.filter(rorKept);
  }
  pcl::PointCloud<pcl::Normal> ref;
  {
    pcl::NormalEstimation<pcl::PointXYZ, pcl::Normal> ne;
    ne.setInputCloud(pc);
    ne.setSearchMethod(pcl::make_shared<pcl::search::KdTree<pcl::PointXYZ>>());
    ne.setKSearch(20);
    ne.setViewPoint(0.0f, 0.0f, 0.0f);
    ne.compute(ref);
  }
  const auto keptOf = [&](const test::OpCall& c) {
    std::vector<char> removed(in.pointCount(), 0);
    for (std::int32_t i : c.outputs.at("removed").asIndices()->values) removed[static_cast<std::size_t>(i)] = 1;
    pcl::Indices kept;
    for (std::size_t i = 0; i < removed.size(); ++i) {
      if (!removed[i]) kept.push_back(static_cast<pcl::index_t>(i));
    }
    return kept;
  };
  REQUIRE(sorKept.size() < in.pointCount());  // 参照本身确实剔掉了离群点，下面的比较才有意义
  REQUIRE(rorKept.size() < in.pointCount());

  for (int threads : {1, 3, 8}) {
    CAPTURE(threads);
    test::OpCall c;
    c.threads = threads;
    c.inputs["cloud"] = cloud;
    REQUIRE(c.run("filter.statistical_outlier", {{"meanK", Value::integer(30)}, {"stddevMul", Value::number(1.0)}}).ok);
    CHECK(keptOf(c) == sorKept);
    REQUIRE(c.run("filter.radius_outlier", {{"radius", Value::number(0.05)}, {"minNeighbors", Value::integer(5)}}).ok);
    CHECK(keptOf(c) == rorKept);

    REQUIRE(c.run("features.normals", {{"kSearch", Value::integer(20)}}).ok);
    const PointCloud& got = *c.out("cloud").asCloud();
    REQUIRE(got.normals.size() == in.pointCount() * 3);
    float worst = 0.0f;
    for (std::size_t i = 0; i < in.pointCount(); ++i) {
      const pcl::Normal& r = ref[i];
      for (int a = 0; a < 3; ++a) {
        // 邻域退化时 PCL 填 NaN，算子置零
        const float want = std::isfinite(r.normal[a]) ? r.normal[a] : 0.0f;
        worst = std::max(worst, std::abs(got.normals[i * 3 + static_cast<std::size_t>(a)] - want));
      }
    }
    CHECK(worst == 0.0f);
  }

  // 顺带修的：flipTowardsViewpoint 关掉就不翻 —— PCL 的 NormalEstimation 不论如何都朝视点（没设就是原点）翻，
  // 以前这个开关关了也照翻。开着时每个法线都朝着视点，关掉时朝向就是 PCA 给的那样，有正有负
  const auto facingAway = [&](bool flip) {
    test::OpCall c;
    c.inputs["cloud"] = cloud;
    REQUIRE(c.run("features.normals", {{"kSearch", Value::integer(20)}, {"flipTowardsViewpoint", Value::boolean(flip)}}).ok);
    const PointCloud& got = *c.out("cloud").asCloud();
    std::size_t away = 0;
    for (std::size_t i = 0; i < in.pointCount(); ++i) {
      const float* p = &got.xyz[i * 3];
      const float* n = &got.normals[i * 3];
      if (-p[0] * n[0] - p[1] * n[1] - p[2] * n[2] < 0.0f) ++away;
    }
    return away;
  };
  CHECK(facingAway(true) == 0);
  CHECK(facingAway(false) > 0);
}
