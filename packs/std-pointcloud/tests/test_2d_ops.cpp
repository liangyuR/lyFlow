// 2D 量测域算子的测试（ADR-0015）。这些算子的输入里有 Box2D / Transform，
// 拼图跑不方便，所以直接调 compute。
#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "algo/region_growing2d.h"
#include "data/region_growing_2d_cases.h"
#include "helpers.h"
#include "lyflow/operator.h"
#include "lyflow/registry.h"

using namespace lyflow;

namespace {

using Call = test::OpCall;

Box2D box(float xMin, float yMin, float xMax, float yMax) {
  Box2D b;
  b.min[0] = xMin;
  b.min[1] = yMin;
  b.max[0] = xMax;
  b.max[1] = yMax;
  return b;
}

float fromBits(std::uint32_t bits) {
  float value;
  std::memcpy(&value, &bits, sizeof value);
  return value;
}

std::uint32_t toBits(float value) {
  std::uint32_t bits;
  std::memcpy(&bits, &value, sizeof bits);
  return bits;
}

} // namespace

TEST_CASE("filter.crop_box2d 的开闭区间在边界点上分得开") {
  PointCloud cloud;
  cloud.push(0.0f, 0.0f, 0.0f);          // 角上
  cloud.push(1.0f, 0.5f, 0.0f);          // 右边上
  cloud.push(0.5f, 0.5f, 0.0f);          // 正中间
  cloud.push(2.0f, 2.0f, 0.0f);          // 框外
  cloud.push(std::nanf(""), 0.5f, 0.0f); // 非有限：两种口径都丢

  SUBCASE("closed 留下边界点") {
    Call c;
    c.inputs["cloud"] = Data::cloud(cloud);
    c.inputs["box"] = Data::box2d(box(0.0f, 0.0f, 1.0f, 1.0f));
    REQUIRE(c.run("filter.crop_box2d", {{"bounds", Value::text("closed")}}).ok);
    CHECK(c.outputs["cloud"].asCloud()->pointCount() == 3);
  }
  SUBCASE("open 把边界点丢掉") {
    Call c;
    c.inputs["cloud"] = Data::cloud(cloud);
    c.inputs["box"] = Data::box2d(box(0.0f, 0.0f, 1.0f, 1.0f));
    REQUIRE(c.run("filter.crop_box2d", {{"bounds", Value::text("open")}}).ok);
    CHECK(c.outputs["cloud"].asCloud()->pointCount() == 1);
  }
  SUBCASE("裁空报 roi_empty") {
    Call c;
    c.inputs["cloud"] = Data::cloud(cloud);
    c.inputs["box"] = Data::box2d(box(10.0f, 10.0f, 11.0f, 11.0f));
    const Status s = c.run("filter.crop_box2d");
    CHECK_FALSE(s.ok);
    CHECK(s.code == "roi_empty");
    CHECK(s.portName == "box");
  }
}

TEST_CASE("fit.line_2d 找回合成直线的方向与内点") {
  // y = 0.5x + 0.1 上的 200 个点，外加 20 个明显偏离的
  PointCloud cloud;
  for (int i = 0; i < 200; ++i) {
    const float x = static_cast<float>(i) * 0.001f;
    cloud.push(x, 0.5f * x + 0.1f, 0.0f);
  }
  for (int i = 0; i < 20; ++i) {
    cloud.push(static_cast<float>(i) * 0.001f, 0.9f, 0.0f);
  }

  Call c;
  c.inputs["cloud"] = Data::cloud(cloud);
  const Status s = c.run("fit.line_2d", {{"distThresh", Value::number(0.0005)}});
  CAPTURE(s.message);
  REQUIRE(s.ok);

  const Line2D* line = c.outputs["line"].asLine2D();
  REQUIRE(line != nullptr);
  const float slope = line->dir[1] / line->dir[0];
  CHECK(std::fabs(slope) == doctest::Approx(0.5).epsilon(0.01));
  CHECK_FALSE(line->hasSegment); // 没接 clipTo 就不带端点

  const Indices* inliers = c.outputs["inliers"].asIndices();
  REQUIRE(inliers != nullptr);
  CHECK(inliers->sourceCloudId == cloud.id);
  CHECK(inliers->values.size() == 200);
}

TEST_CASE("fit.line_2d 接了 clipTo 就带上与框的两个端点") {
  PointCloud cloud;
  for (int i = 0; i < 100; ++i) {
    const float x = static_cast<float>(i) * 0.001f;
    cloud.push(x, 0.2f, 0.0f);
  }
  Call c;
  c.inputs["cloud"] = Data::cloud(cloud);
  c.inputs["clipTo"] = Data::box2d(box(0.0f, 0.0f, 0.05f, 1.0f));
  REQUIRE(c.run("fit.line_2d", {{"distThresh", Value::number(0.0005)}}).ok);

  const Line2D* line = c.outputs["line"].asLine2D();
  REQUIRE(line != nullptr);
  REQUIRE(line->hasSegment);
  CHECK(std::fabs(line->start[0] - line->end[0]) == doctest::Approx(0.05).epsilon(0.01));
  CHECK(line->start[1] == doctest::Approx(0.2).epsilon(0.01));
  CHECK(line->end[1] == doctest::Approx(0.2).epsilon(0.01));
}

TEST_CASE("fit.circle_2d 找回合成圆的圆心与半径") {
  // 圆心 (0.01, 0.02)、半径 0.003 的整圆
  PointCloud cloud;
  for (int i = 0; i < 180; ++i) {
    const double a = i * 3.14159265358979 / 90.0;
    cloud.push(static_cast<float>(0.01 + 0.003 * std::cos(a)),
               static_cast<float>(0.02 + 0.003 * std::sin(a)), 0.0f);
  }

  SUBCASE("自由半径") {
    Call c;
    c.inputs["cloud"] = Data::cloud(cloud);
    const Status s = c.run("fit.circle_2d", {{"distThresh", Value::number(0.0001)},
                                             {"rMin", Value::number(0.001)},
                                             {"rMax", Value::number(0.01)}});
    CAPTURE(s.message);
    REQUIRE(s.ok);
    const Circle2D* circle = c.outputs["circle"].asCircle2D();
    REQUIRE(circle != nullptr);
    CHECK(circle->center[0] == doctest::Approx(0.01).epsilon(0.02));
    CHECK(circle->center[1] == doctest::Approx(0.02).epsilon(0.02));
    CHECK(circle->radius == doctest::Approx(0.003).epsilon(0.02));
  }
  SUBCASE("固定半径把半径钉死，圆心按最小二乘重定") {
    Call c;
    c.inputs["cloud"] = Data::cloud(cloud);
    const Status s = c.run("fit.circle_2d", {{"distThresh", Value::number(0.0001)},
                                             {"rMin", Value::number(0.001)},
                                             {"rMax", Value::number(0.01)},
                                             {"fixedRadius", Value::number(0.00305)}});
    CAPTURE(s.message);
    REQUIRE(s.ok);
    CHECK(c.outputs["circle"].asCircle2D()->radius == doctest::Approx(0.00305));
    CHECK(c.outputs["circle"].asCircle2D()->center[0] == doctest::Approx(0.01).epsilon(0.05));
    CHECK_FALSE(c.outputs["inliers"].asIndices()->values.empty());
  }
  SUBCASE("上下限反了报 bad_param") {
    Call c;
    c.inputs["cloud"] = Data::cloud(cloud);
    const Status s =
        c.run("fit.circle_2d", {{"rMin", Value::number(0.01)}, {"rMax", Value::number(0.001)}});
    CHECK_FALSE(s.ok);
    CHECK(s.code == "bad_param");
    CHECK(s.paramPath == "rMax");
  }
}

TEST_CASE("register.icp_2d 恢复出已知的平移") {
  // 一段带拐角的折线：直线段在 point-to-plane 下沿切向不可观测
  PointCloud target;
  for (int i = 0; i < 120; ++i) {
    const float t = static_cast<float>(i) * 0.0005f;
    target.push(t, 0.0f, 0.0f);
  }
  for (int i = 1; i < 120; ++i) {
    const float t = static_cast<float>(i) * 0.0005f;
    target.push(0.0595f, t, 0.0f);
  }
  const float dx = 0.002f, dy = -0.0015f;
  PointCloud source;
  for (std::size_t i = 0; i < target.pointCount(); ++i) {
    source.push(target.xyz[i * 3] + dx, target.xyz[i * 3 + 1] + dy, 0.0f);
  }

  Call c;
  c.inputs["source"] = Data::cloud(source);
  c.inputs["target"] = Data::cloud(target);
  const Status s = c.run("register.icp_2d", {{"maxMatchingDist", Value::number(0.01)},
                                             {"fitnessDist", Value::number(0.0005)},
                                             {"maxIterations", Value::integer(200)}});
  CAPTURE(s.message);
  REQUIRE(s.ok);

  const Transform* t = c.outputs["transform"].asTransform();
  REQUIRE(t != nullptr);
  // 变换把源变回目标，所以平移是 -dx / -dy
  CHECK(t->m[3] == doctest::Approx(-dx).epsilon(0.05));
  CHECK(t->m[7] == doctest::Approx(-dy).epsilon(0.05));
  CHECK(t->m[10] == doctest::Approx(1.0));

  const Record* result = c.outputs["result"].asRecord();
  REQUIRE(result != nullptr);
  CHECK(result->type == "Icp2DResult");
  CHECK(result->data["fitness"].get<double>() > 0.9);
  CHECK(result->data["iterations"].get<int>() > 0);
}

TEST_CASE("register.icp_2d 对空云报 bad_input 而不是崩") {
  Call c;
  c.inputs["source"] = Data::cloud(PointCloud{});
  c.inputs["target"] = Data::cloud(PointCloud{});
  const Status s = c.run("register.icp_2d");
  CHECK_FALSE(s.ok);
  CHECK(s.code == "bad_input");
  CHECK(s.portName == "source");
}

TEST_CASE("segment.region_growing_2d 与 xyz-gap-inspector 的 split.rs 逐位一致") {
  // 期望值由 split.rs（标定左右分割的行为基准，与 PCL RegionGrowing 等价）对同一批输入跑出：
  // split.rs 自己的单元测试输入 + 现场标定圆柱两个头（经 filter.crop_box2d 同一条裁剪）在几组参数下。
  namespace rg = lyflow::test::region_growing_2d;
  for (const rg::Case& tc : rg::kCases) {
    CAPTURE(tc.name);
    std::vector<float> xy(2 * tc.pointCount);
    PointCloud cloud;
    for (std::size_t i = 0; i < tc.pointCount; ++i) {
      xy[2 * i] = fromBits(tc.xy[2 * i]);
      xy[2 * i + 1] = fromBits(tc.xy[2 * i + 1]);
      cloud.push(xy[2 * i], xy[2 * i + 1], 0.0f);
      cloud.intensity.push_back(static_cast<float>(i));
    }

    std_pc::RegionGrowing2DOptions options;
    options.neighbors = tc.neighbor;
    options.angleThresholdDeg = tc.angleDeg;
    const std_pc::RegionGrowing2DResult result =
        std_pc::regionGrowing2D(xy.data(), tc.pointCount, options);
    CHECK(result.clusters.size() == static_cast<std::size_t>(tc.clusterCount));
    CHECK(result.left == std::vector<std::int32_t>(tc.left, tc.left + tc.leftCount));
    CHECK(result.right == std::vector<std::int32_t>(tc.right, tc.right + tc.rightCount));

    Call c;
    c.inputs["cloud"] = Data::cloud(cloud);
    const Status s =
        c.run("segment.region_growing_2d", {{"neighbors", Value::integer(tc.neighbor)},
                                            {"angleThresh", Value::number(tc.angleDeg)}});
    if (tc.clusterCount < 2) {
      CHECK_FALSE(s.ok);
      CHECK(s.code == (tc.neighbor < 2 ? "bad_param" : "split_failed"));
      continue;
    }
    CAPTURE(s.message);
    REQUIRE(s.ok);
    // 算子输出的两片云是输入按下标取的子集：坐标逐位相同，通道（这里用强度记原下标）跟着走。
    struct Side {
      const char* port;
      const std::int32_t* expected;
      std::size_t count;
    };
    for (const Side side :
         {Side{"left", tc.left, tc.leftCount}, Side{"right", tc.right, tc.rightCount}}) {
      CAPTURE(side.port);
      const PointCloud* out = c.outputs[side.port].asCloud();
      REQUIRE(out != nullptr);
      REQUIRE(out->pointCount() == side.count);
      for (std::size_t m = 0; m < side.count; ++m) {
        const auto i = static_cast<std::size_t>(side.expected[m]);
        CHECK(toBits(out->xyz[3 * m]) == tc.xy[2 * i]);
        CHECK(toBits(out->xyz[3 * m + 1]) == tc.xy[2 * i + 1]);
        CHECK(out->intensity[m] == static_cast<float>(i));
      }
    }
    const Record* record = c.outputs["result"].asRecord();
    REQUIRE(record != nullptr);
    CHECK(record->type == "RegionGrowing2DResult");
    CHECK(record->data["clusterCount"] == tc.clusterCount);
    CHECK(record->data["leftCount"] == tc.leftCount);
    CHECK(record->data["rightCount"] == tc.rightCount);
  }
}

TEST_CASE("segment.region_growing_2d 的 eigen22 保留 PCL 对角分支的原样，参数反了报 bad_param") {
  // 互协方差近 0 时恒取 else：特征值 = 互协方差、向量 (0, 1)（split.rs 的同名用例）。
  const double diagonal[4] = {4.0, 0.0, 0.0, 1.0};
  double vector[2];
  CHECK(std_pc::eigen22(diagonal, vector) == 0.0);
  CHECK(vector[0] == 0.0);
  CHECK(vector[1] == 1.0);
  const double general[4] = {2.0, 1.0, 1.0, 2.0};
  CHECK(std_pc::eigen22(general, vector) == doctest::Approx(1.0).epsilon(1e-12));
  CHECK(std::abs(std::abs(vector[0] - vector[1]) - std::sqrt(2.0)) < 1e-12);

  Call c;
  PointCloud cloud;
  for (int i = 0; i < 10; ++i)
    cloud.push(static_cast<float>(i) * 0.001f, 0.0f, 0.0f);
  c.inputs["cloud"] = Data::cloud(cloud);
  const Status s = c.run("segment.region_growing_2d", {{"minClusterSize", Value::integer(50)},
                                                       {"maxClusterSize", Value::integer(10)}});
  CHECK_FALSE(s.ok);
  CHECK(s.code == "bad_param");
  CHECK(s.paramPath == "maxClusterSize");
}
