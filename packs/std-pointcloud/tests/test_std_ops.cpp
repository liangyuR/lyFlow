// std-pointcloud 包自己的算子测试（S6）。这些用例原先在 core/tests 里，
// 拆包时随算子一起搬过来 —— 断言逐条不变，只是换了个文件。
#include <doctest/doctest.h>

#include <pcl/features/normal_3d.h>
#include <pcl/filters/radius_outlier_removal.h>
#include <pcl/filters/statistical_outlier_removal.h>
#include <pcl/search/kdtree.h>
#include <pcl/segmentation/sac_segmentation.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <unordered_map>
#include <cstring>
#include <limits>
#include <numeric>
#include <random>

#include "algo/fit2d.h"
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

TEST_CASE("体素栅格：按线程预算并行之后与原来的单线程写法逐字节相同（质心 / 最近点、全部通道与 NaN、1 / 3 / 8 个线程）") {
  // 参照就是改并行之前的那一版：按点的顺序一趟扫完，体素按第一次出现的顺序编号，累加都在 double 里
  test::OpCall gen;
  REQUIRE(gen.run("gen.synthetic", {{"pointCount", Value::integer(60000)}, {"seed", Value::integer(11)}}).ok);
  PointCloud in = *gen.out("cloud").asCloud();
  const std::size_t n = in.pointCount();
  std::mt19937 rng(5);
  std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
  in.normals.resize(n * 3);
  in.rgb.resize(n * 3);
  for (std::size_t i = 0; i < n * 3; ++i) {
    in.normals[i] = unit(rng);
    in.rgb[i] = static_cast<std::uint8_t>(rng() & 0xFF);
  }
  for (std::size_t k = 0; k < 20; ++k) in.xyz[((k * 997) % n) * 3 + 1] = std::numeric_limits<float>::quiet_NaN();

  struct Key {
    std::int64_t i, j, k;
    bool operator==(const Key& o) const { return i == o.i && j == o.j && k == o.k; }
  };
  struct KeyHash {
    std::size_t operator()(const Key& v) const { return std::hash<std::int64_t>()(v.i * 73856093 ^ v.j * 19349663 ^ v.k * 83492791); }
  };
  struct Acc {
    double sx = 0, sy = 0, sz = 0, si = 0, snx = 0, sny = 0, snz = 0, sr = 0, sg = 0, sb = 0;
    std::int32_t count = 0;
  };
  const auto keyOf = [](float x, float y, float z, float leaf) {
    return Key{static_cast<std::int64_t>(std::floor(static_cast<double>(x) / leaf)),
               static_cast<std::int64_t>(std::floor(static_cast<double>(y) / leaf)),
               static_cast<std::int64_t>(std::floor(static_cast<double>(z) / leaf))};
  };
  const auto reference = [&](float leaf, std::int32_t minPts, bool nearest) {
    std::unordered_map<Key, std::size_t, KeyHash> index;
    std::vector<Acc> acc;
    for (std::size_t p = 0; p < n; ++p) {
      const float x = in.xyz[p * 3], y = in.xyz[p * 3 + 1], z = in.xyz[p * 3 + 2];
      if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) continue;
      const auto [it, fresh] = index.try_emplace(keyOf(x, y, z, leaf), acc.size());
      if (fresh) acc.emplace_back();
      Acc& v = acc[it->second];
      v.sx += x; v.sy += y; v.sz += z; v.count += 1;
      v.si += in.intensity[p];
      v.snx += in.normals[p * 3]; v.sny += in.normals[p * 3 + 1]; v.snz += in.normals[p * 3 + 2];
      v.sr += in.rgb[p * 3]; v.sg += in.rgb[p * 3 + 1]; v.sb += in.rgb[p * 3 + 2];
    }
    if (nearest) {
      std::vector<std::int32_t> best(acc.size(), -1);
      std::vector<double> bestDist(acc.size(), 1e300);
      for (std::size_t p = 0; p < n; ++p) {
        const float x = in.xyz[p * 3], y = in.xyz[p * 3 + 1], z = in.xyz[p * 3 + 2];
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) continue;
        const std::size_t vi = index.at(keyOf(x, y, z, leaf));
        const Acc& v = acc[vi];
        const double cx = v.sx / v.count, cy = v.sy / v.count, cz = v.sz / v.count;
        const double d = (x - cx) * (x - cx) + (y - cy) * (y - cy) + (z - cz) * (z - cz);
        if (d < bestDist[vi]) {
          bestDist[vi] = d;
          best[vi] = static_cast<std::int32_t>(p);
        }
      }
      std::vector<std::int32_t> keep;
      for (std::size_t vi = 0; vi < acc.size(); ++vi) {
        if (acc[vi].count >= minPts && best[vi] >= 0) keep.push_back(best[vi]);
      }
      return in.select(keep);
    }
    PointCloud out;
    for (const Acc& v : acc) {
      if (v.count < minPts) continue;
      const double inv = 1.0 / v.count;
      out.push(static_cast<float>(v.sx * inv), static_cast<float>(v.sy * inv), static_cast<float>(v.sz * inv));
      out.intensity.push_back(static_cast<float>(v.si * inv));
      double nx = v.snx * inv, ny = v.sny * inv, nz = v.snz * inv;
      const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
      if (len > 1e-9) { nx /= len; ny /= len; nz /= len; }
      out.normals.push_back(static_cast<float>(nx));
      out.normals.push_back(static_cast<float>(ny));
      out.normals.push_back(static_cast<float>(nz));
      out.rgb.push_back(static_cast<std::uint8_t>(v.sr * inv));
      out.rgb.push_back(static_cast<std::uint8_t>(v.sg * inv));
      out.rgb.push_back(static_cast<std::uint8_t>(v.sb * inv));
    }
    return out;
  };
  const auto sameBytes = [](const auto& a, const auto& b) {
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(a[0])) == 0);
  };

  // 叶大小按这片云自己的尺寸取：细的（体素多、多数只有一两个点）与粗的（每个体素几十个点）各一档
  float extent = 0;
  for (int axis = 0; axis < 3; ++axis) {
    float lo = std::numeric_limits<float>::max(), hi = -lo;
    for (std::size_t i = 0; i < n; ++i) {
      const float v = in.xyz[i * 3 + axis];
      if (!std::isfinite(v)) continue;
      lo = std::min(lo, v);
      hi = std::max(hi, v);
    }
    extent = std::max(extent, hi - lo);
  }
  REQUIRE(extent > 0);
  for (const float leaf : {extent / 120.0f, extent / 25.0f}) {
    for (const std::int32_t minPts : {1, 3}) {
      for (const bool nearest : {false, true}) {
        const PointCloud want = reference(leaf, minPts, nearest);
        REQUIRE(want.pointCount() > 10);
        for (const int threads : {1, 3, 8}) {
          CAPTURE(leaf);
          CAPTURE(minPts);
          CAPTURE(nearest);
          CAPTURE(threads);
          test::OpCall call;
          call.threads = threads;
          call.inputs["cloud"] = Data::cloud(in);
          REQUIRE(call.run("filter.voxel_grid", {{"leafSize", Value::vec({leaf, leaf, leaf})},
                                                 {"minPointsPerVoxel", Value::integer(minPts)},
                                                 {"representative", Value::text(nearest ? "nearest" : "centroid")}})
                      .ok);
          const PointCloud& got = *call.out("cloud").asCloud();
          CHECK(sameBytes(got.xyz, want.xyz));
          CHECK(sameBytes(got.intensity, want.intensity));
          CHECK(sameBytes(got.normals, want.normals));
          CHECK(sameBytes(got.rgb, want.rgb));
        }
      }
    }
  }
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

// 随机采样把下标排回原顺序：留得多时改用标记表顺着扫（std::sort 在 200 万点留一半时要 60 ms 以上）。
// 参照就是原来的做法（同一个种子的部分 Fisher-Yates，再 std::sort）：两条路、两种模式都要逐点相同
TEST_CASE("随机采样：标记表与排序两条路给出与原实现逐点相同的结果") {
  test::OpCall gen;
  REQUIRE(gen.run("gen.synthetic", {{"pointCount", Value::integer(50000)}, {"seed", Value::integer(8)}}).ok);
  const PointCloud& in = *gen.out("cloud").asCloud();
  const std::size_t n = in.pointCount();
  const auto reference = [&](std::size_t want, std::uint32_t seed) {
    std::vector<std::int32_t> keep(n);
    std::iota(keep.begin(), keep.end(), 0);
    std::mt19937 rng(seed);
    for (std::size_t i = 0; i < want; ++i) {
      std::uniform_int_distribution<std::size_t> pick(i, n - 1);
      std::swap(keep[i], keep[pick(rng)]);
    }
    keep.resize(want);
    std::sort(keep.begin(), keep.end());
    return in.select(keep);
  };
  struct Case {
    const char* mode;
    std::int64_t count;
    double ratio;
    std::size_t want;
  };
  // 1000 个（want × 16 < n：排序那条路）、一半与九成（标记表那条路）、全留（不抽）
  const Case cases[] = {{"count", 1000, 0.0, 1000}, {"ratio", 0, 0.5, n / 2}, {"ratio", 0, 0.9, n * 9 / 10},
                        {"count", static_cast<std::int64_t>(n) + 5, 0.0, n}};
  for (const Case& c : cases) {
    for (std::int64_t seed : {1, 42}) {
      CAPTURE(c.mode);
      CAPTURE(c.want);
      CAPTURE(seed);
      test::OpCall call;
      call.inputs["cloud"] = gen.out("cloud");
      REQUIRE(call.run("filter.random_sample", {{"mode", Value::text(c.mode)},
                                                {"keepCount", Value::integer(c.count)},
                                                {"keepRatio", Value::number(c.ratio)},
                                                {"seed", Value::integer(seed)}})
                  .ok);
      const PointCloud& got = *call.out("cloud").asCloud();
      if (c.want < n) {
        const PointCloud want = reference(c.want, static_cast<std::uint32_t>(seed));
        CHECK(got.xyz == want.xyz);
        CHECK(got.intensity == want.intensity);
      } else {
        CHECK(got.xyz == in.xyz);  // 全留：不抽
      }
    }
  }
}

// 法线与两个离群点滤波改成按线程预算分段并行（ops/parallel.h；vcpkg 的 PCL 没开 OpenMP，它们原来是单线程的
// PCL 实现，200 万点上 12 s / 14 s / 7 s）。参照就是原来直接调的那三个 PCL 类：逐点结果要相同，换几个线程也一样
TEST_CASE("法线与两个离群点滤波：分段并行的结果与 PCL 原实现逐点相同，与线程数无关") {
  test::OpCall gen;
  REQUIRE(gen.run("gen.synthetic", {{"pointCount", Value::integer(60000)}, {"seed", Value::integer(5)}}).ok);
  // 两份云：原样的（is_dense，半径离群点走 K 近邻那一支），与掺了 NaN 点的（三个算子都走「非有限点」那一支）
  PointCloud withNaN = *gen.out("cloud").asCloud();
  for (std::size_t i = 0; i < withNaN.pointCount(); i += 997) withNaN.xyz[i * 3] = std::numeric_limits<float>::quiet_NaN();
  for (const Data& cloud : {gen.out("cloud"), Data::cloud(std::move(withNaN))}) {
    const PointCloud& in = *cloud.asCloud();
    const auto pc = ops::adapter::toPcl(in);
    CAPTURE(pc->is_dense);

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
          // 邻域退化（或点本身不是有限的）时 PCL 填 NaN，算子置零
          const float want = std::isfinite(r.normal[a]) ? r.normal[a] : 0.0f;
          worst = std::max(worst, std::abs(got.normals[i * 3 + static_cast<std::size_t>(a)] - want));
        }
      }
      CHECK(worst == 0.0f);
    }
  }

  // 顺带修的：flipTowardsViewpoint 关掉就不翻 —— PCL 的 NormalEstimation 不论如何都朝视点（没设就是原点）翻，
  // 以前这个开关关了也照翻。开着时每个法线都朝着视点，关掉时朝向就是 PCA 给的那样，有正有负
  const auto facingAway = [&](bool flip) {
    test::OpCall c;
    c.inputs["cloud"] = gen.out("cloud");
    REQUIRE(c.run("features.normals", {{"kSearch", Value::integer(20)}, {"flipTowardsViewpoint", Value::boolean(flip)}}).ok);
    const PointCloud& got = *c.out("cloud").asCloud();
    std::size_t away = 0;
    for (std::size_t i = 0; i < got.pointCount(); ++i) {
      const float* p = &got.xyz[i * 3];
      const float* n = &got.normals[i * 3];
      if (-p[0] * n[0] - p[1] * n[1] - p[2] * n[2] < 0.0f) ++away;
    }
    return away;
  };
  CHECK(facingAway(true) == 0);
  CHECK(facingAway(false) > 0);
}

namespace {

/// 圆心 (0, r) 往下看的一段弧：从正上方（y 最小）往 +x 转 spanDeg 度，加高斯噪声。米。
std_pc::Cloud2D noisyArc(double r, double fromDeg, double spanDeg, int n, double noise,
                         unsigned seed) {
  std::mt19937 rng(seed);
  std::normal_distribution<double> jitter(0.0, noise);
  std_pc::Cloud2D cloud;
  for (int i = 0; i < n; ++i) {
    const double a = (fromDeg + spanDeg * i / (n - 1)) * 3.14159265358979323846 / 180.0;
    std_pc::Point2DT p;
    p.x = static_cast<float>(r * std::sin(a) + jitter(rng));
    p.y = static_cast<float>(r - r * std::cos(a) + jitter(rng));
    p.z = 0.0F;
    cloud.push_back(p);
  }
  return cloud;
}

/// 改之前的那一行 seg.segment，当参照。
bool referenceSegment(const std_pc::Cloud2D& cloud, const std_pc::Circle2DFitOptions& o,
                      Eigen::VectorXf* circle, pcl::Indices* inliers) {
  pcl::ModelCoefficients coefficients;
  pcl::PointIndices pointInliers;
  pcl::SACSegmentation<std_pc::Point2DT> seg(false);
  seg.setOptimizeCoefficients(true);
  seg.setModelType(pcl::SACMODEL_CIRCLE2D);
  seg.setMethodType(pcl::SAC_RANSAC);
  seg.setMaxIterations(o.maxIterations);
  seg.setDistanceThreshold(o.distThresh);
  seg.setRadiusLimits(o.minRadius, o.maxRadius);
  seg.setInputCloud(cloud.makeShared());
  seg.segment(pointInliers, coefficients);
  *inliers = pointInliers.indices;
  if (coefficients.values.size() < 3 || inliers->size() < 3) return false;
  *circle = Eigen::VectorXf(3);
  for (int i = 0; i < 3; ++i) (*circle)[i] = coefficients.values[static_cast<std::size_t>(i)];
  return true;
}

std_pc::Circle2DFitOptions circleOpts(double minMm, double maxMm) {
  std_pc::Circle2DFitOptions o;
  o.distThresh = 0.00003F;
  o.minRadius = minMm / 1000.0;
  o.maxRadius = maxMm / 1000.0;
  return o;
}

}  // namespace

TEST_CASE("fitCircle2D：细化后半径在界内时与 SACSegmentation 逐位相同") {
  // 弧长 / 短弧 / 噪声大小各一份。走的是同一套 PCL 对象与种子，圆与内点都要一模一样。
  struct Row { double r, from, span; int n; double noise; unsigned seed; };
  for (const Row row : {Row{0.0015, -60, 300, 120, 0.000005, 1}, Row{0.004, 10, 80, 60, 0.000015, 2},
                        Row{0.0009, -30, 160, 40, 0.00001, 3}}) {
    CAPTURE(row.r);
    CAPTURE(row.span);
    const auto cloud = noisyArc(row.r, row.from, row.span, row.n, row.noise, row.seed);
    const auto o = circleOpts(0.5, 6.0);
    Eigen::VectorXf want, got;
    pcl::Indices wantIn, gotIn;
    std_pc::Circle2DFitReport report;
    REQUIRE(referenceSegment(cloud, o, &want, &wantIn));
    REQUIRE(std_pc::fitCircle2D(cloud, &got, &gotIn, o, &report));
    CHECK(report.radius == std_pc::Circle2DRadiusOutcome::Free);
    for (int i = 0; i < 3; ++i) CHECK(got[i] == want[i]);
    CHECK(gotIn == wantIn);
  }
}

TEST_CASE("fitCircle2D：细化把半径推出界时钉在界上重定圆心，不再整个失败") {
  // 真半径 5 mm、只看到 60° 的弧，上界给 4 mm：RANSAC 能找到界内的圆，LM 细化把它推回
  // 5 mm 附近 —— SACSegmentation 拿越界的圆去收内点，收到 0 个而失败（现场天幕 R3/R5/R6
  // 那四帧就是这样丢的）。
  const auto cloud = noisyArc(0.005, 20, 60, 70, 0.000012, 7);
  const auto o = circleOpts(3.0, 4.0);
  Eigen::VectorXf ref, got;
  pcl::Indices refIn, gotIn;
  REQUIRE_FALSE(referenceSegment(cloud, o, &ref, &refIn));

  std_pc::Circle2DFitReport report;
  REQUIRE(std_pc::fitCircle2D(cloud, &got, &gotIn, o, &report));
  CHECK(report.radius == std_pc::Circle2DRadiusOutcome::ClampedMax);
  CHECK(report.unclampedRadius > o.maxRadius);
  CHECK(got[2] == static_cast<float>(o.maxRadius));
  CHECK(gotIn.size() >= 10);

  SUBCASE("下界同理") {
    const auto small = noisyArc(0.0012, 20, 60, 70, 0.000012, 8);
    const auto lo = circleOpts(1.6, 3.0);
    REQUIRE_FALSE(referenceSegment(small, lo, &ref, &refIn));
    REQUIRE(std_pc::fitCircle2D(small, &got, &gotIn, lo, &report));
    CHECK(report.radius == std_pc::Circle2DRadiusOutcome::ClampedMin);
    CHECK(got[2] == static_cast<float>(lo.minRadius));
  }
}

TEST_CASE("fitCircle2D 的半径先验：短弧往先验收，长弧几乎不动") {
  auto o = circleOpts(1.0, 8.0);
  Eigen::VectorXf free, withPrior;
  pcl::Indices in;
  std_pc::Circle2DFitReport report;

  // 50° 的短弧、真半径 4 mm：自由拟合的半径被噪声带偏，先验（4 mm ± 0.3）把它拉回来。
  const auto shortArc = noisyArc(0.004, 30, 50, 50, 0.000015, 11);
  REQUIRE(std_pc::fitCircle2D(shortArc, &free, &in, o));
  o.radiusPrior = 0.004;
  o.radiusPriorSigma = 0.0003;
  REQUIRE(std_pc::fitCircle2D(shortArc, &withPrior, &in, o, &report));
  CHECK(report.radius == std_pc::Circle2DRadiusOutcome::Prior);
  CAPTURE(free[2]);
  CAPTURE(withPrior[2]);
  CHECK(std::fabs(withPrior[2] - 0.004) < std::fabs(free[2] - 0.004));

  // 300° 的长弧：先验故意给错 0.5 mm，读数几乎不受影响。
  const auto longArc = noisyArc(0.004, -150, 300, 150, 0.000015, 12);
  o.radiusPrior = 0.0045;
  REQUIRE(std_pc::fitCircle2D(longArc, &withPrior, &in, o));
  CHECK(withPrior[2] == doctest::Approx(0.004).epsilon(0.005));
}
