// 2D 量测域算子的测试（ADR-0015）。这些算子的输入里有 Box2D / Transform，
// 拼图跑不方便，所以直接调 compute。
#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#include "algo/icp2d.h"
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

PointCloud cornerProfile(float dx = 0.0f, float dy = 0.0f) {
  PointCloud cloud;
  for (int i = 0; i < 120; ++i)
    cloud.push(static_cast<float>(i) * 0.0005f + dx, dy, 0.0f);
  for (int i = 1; i < 120; ++i)
    cloud.push(0.0595f + dx, static_cast<float>(i) * 0.0005f + dy, 0.0f);
  return cloud;
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
  CHECK(result->data["qualityEvaluated"] == false);
  CHECK_FALSE(result->data.contains("pointRmse"));

  const Transform original = *t;
  const auto previous = result->data;
  REQUIRE(c.run("register.icp_2d", {{"maxMatchingDist", Value::number(0.01)},
                                    {"fitnessDist", Value::number(0.0005)},
                                    {"maxIterations", Value::integer(200)},
                                    {"robustLoss", Value::text("none")},
                                    {"robustScale", Value::number(0.0)},
                                    {"multiScaleLevels", Value::integer(1)},
                                    {"coarseVoxelSize", Value::number(0.0)},
                                    {"reportQuality", Value::boolean(false)}})
              .ok);
  CHECK(std::memcmp(original.m, c.outputs["transform"].asTransform()->m, sizeof original.m) == 0);
  const auto& explicitDefaults = c.outputs["result"].asRecord()->data;
  CHECK(explicitDefaults["fitness"] == previous["fitness"]);
  CHECK(explicitDefaults["iterations"] == previous["iterations"]);
  CHECK(explicitDefaults["converged"] == previous["converged"]);
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

TEST_CASE("Icp2D 鲁棒权重抑制超过阈值的平面残差，默认选项仍是最小二乘") {
  PointCloud source;
  PointCloud target;
  for (int i = 0; i < 30; ++i) {
    source.push(0.0002f, static_cast<float>(i) * 0.001f, 0.0f);
    target.push(0.0f, static_cast<float>(i) * 0.001f, 0.0f);
  }
  std_pc::Icp2DOptions options;
  options.maxMatchingDistance = 0.01;
  options.maxIterationCount = 1;
  options.robustScale = 0.0001;
  const auto qualityOf = [&](std_pc::Icp2DRobustLoss loss) {
    options.robustLoss = loss;
    std_pc::Icp2D icp(options);
    icp.setSource(std_pc::toCloud2D(source));
    icp.setTarget(std_pc::toCloud2D(target));
    return icp.evaluatePose(Eigen::Matrix3f::Identity());
  };
  const auto plain = qualityOf(std_pc::Icp2DRobustLoss::None);
  const auto huber = qualityOf(std_pc::Icp2DRobustLoss::Huber);
  const auto tukey = qualityOf(std_pc::Icp2DRobustLoss::Tukey);
  CHECK(plain.matchedCount == 30);
  CHECK(plain.weightSum == doctest::Approx(30.0));
  CHECK(huber.weightSum == doctest::Approx(15.0).epsilon(1e-5));
  CHECK(huber.weightedPlaneRmse == doctest::Approx(0.0002).epsilon(1e-5));
  CHECK(tukey.matchedCount == 30);
  CHECK(tukey.effectiveMatchedCount == 0);
  CHECK(tukey.weightSum == 0.0);
  CHECK(std::isnan(tukey.weightedPlaneRmse));

  std_pc::Icp2DOptions legacy;
  legacy.maxMatchingDistance = 0.01;
  legacy.maxIterationCount = 1;
  std_pc::Icp2D leastSquares(legacy);
  leastSquares.setSource(std_pc::toCloud2D(source));
  leastSquares.setTarget(std_pc::toCloud2D(target));
  const auto moved = leastSquares.align();
  CHECK(moved.transform(0, 2) == doctest::Approx(-0.0002).epsilon(1e-4));
  options.robustLoss = std_pc::Icp2DRobustLoss::Tukey;
  std_pc::Icp2D rejected(options);
  rejected.setSource(std_pc::toCloud2D(source));
  rejected.setTarget(std_pc::toCloud2D(target));
  const auto unchanged = rejected.align();
  CHECK(unchanged.transform.isIdentity());
  CHECK_FALSE(unchanged.converged);
}

TEST_CASE("register.icp_2d 多尺度从粗层位姿继续细化，最终质量在全量点上评估") {
  Call c;
  c.inputs["source"] = Data::cloud(cornerProfile(0.003f, -0.002f));
  c.inputs["target"] = Data::cloud(cornerProfile());
  const std::unordered_map<std::string, Value> base = {{"maxMatchingDist", Value::number(0.0005)},
                                                       {"fitnessDist", Value::number(0.0005)},
                                                       {"maxIterations", Value::integer(200)}};
  REQUIRE(c.run("register.icp_2d", base).ok);
  CHECK(c.outputs["result"].asRecord()->data["fitness"] == 0.0);
  auto scaled = base;
  scaled["multiScaleLevels"] = Value::integer(4);
  scaled["coarseVoxelSize"] = Value::number(0.004);
  scaled["reportQuality"] = Value::boolean(true);
  REQUIRE(c.run("register.icp_2d", scaled).ok);
  const auto first = c.outputs["result"].asRecord()->data;
  const Transform pose = *c.outputs["transform"].asTransform();
  CHECK(pose.m[3] == doctest::Approx(-0.003).epsilon(0.05));
  CHECK(pose.m[7] == doctest::Approx(0.002).epsilon(0.05));
  REQUIRE(first["levels"].size() == 4);
  CHECK(first["levels"][0]["maxMatchingDist"] == doctest::Approx(0.004));
  CHECK(first["levels"][0]["voxelSize"] == doctest::Approx(0.004));
  CHECK(first["levels"][3]["voxelSize"] == 0.0);
  CHECK(first["levels"][3]["sourceCount"] == 239);
  CHECK(first["sourceCount"] == 239);
  CHECK(first["matchedCount"].get<int>() > 220);
  CHECK(first["pointRmse"].get<double>() < 0.00015);
  CHECK(first["totalIterations"].get<int>() >= first["iterations"].get<int>());
  REQUIRE(c.run("register.icp_2d", scaled).ok);
  CHECK(std::memcmp(pose.m, c.outputs["transform"].asTransform()->m, sizeof pose.m) == 0);
  CHECK(c.outputs["result"].asRecord()->data == first);
}

TEST_CASE("register.icp_2d 质量默认不扫描，无匹配报告 null，质量门拒绝时不发布输出") {
  Call c;
  c.inputs["source"] = Data::cloud(cornerProfile(10.0f, 10.0f));
  c.inputs["target"] = Data::cloud(cornerProfile());
  REQUIRE(c.run("register.icp_2d").ok);
  const auto ordinary = c.outputs["result"].asRecord()->data;
  CHECK(ordinary["qualityEvaluated"] == false);
  CHECK_FALSE(ordinary.contains("matchedCount"));
  CHECK_FALSE(ordinary.contains("planeRmse"));
  REQUIRE(c.run("register.icp_2d", {{"reportQuality", Value::boolean(true)}}).ok);
  const auto reported = c.outputs["result"].asRecord()->data;
  CHECK(reported["qualityEvaluated"] == true);
  CHECK(reported["matchedCount"] == 0);
  CHECK(reported["pointRmse"].is_null());
  CHECK(reported["planeRmse"].is_null());
  CHECK(reported["weightedPlaneRmse"].is_null());
  const Status rejected = c.run("register.icp_2d", {{"qualityGate", Value::boolean(true)}});
  CHECK_FALSE(rejected.ok);
  CHECK(rejected.code == "registration_rejected");
  CHECK(rejected.message.find("effectiveMatches=0") != std::string::npos);
  CHECK(rejected.message.find("converged=false") != std::string::npos);
  CHECK(c.outputs.empty());

  c.inputs["source"] = Data::cloud(cornerProfile());
  REQUIRE(c.run("register.icp_2d", {{"qualityGate", Value::boolean(true)}}).ok);
  CHECK(c.outputs["result"].asRecord()->data["qualityEvaluated"] == true);
  CHECK(c.outputs["result"].asRecord()->data["qualityGateEnabled"] == true);
}

TEST_CASE("register.icp_2d 可选参数的跨约束在加载期诊断，单尺度不要求粗网格为正") {
  using Json = nlohmann::json;
  const auto diagnostics = [](const Json& params) {
    const auto doc = test::makeGraph({{"g", "gen.synthetic"}, {"n", "register.icp_2d", params}},
                                     {{"g.cloud", "n.source"}, {"g.cloud", "n.target"}});
    return Json::parse(exec::validateGraphJson(doc.dump(), {}));
  };
  CHECK(diagnostics(Json{{"coarseVoxelSize", 0.0}, {"robustScale", 0.0}}).empty());
  const std::vector<std::pair<Json, std::string>> invalid = {
      {Json{{"robustLoss", "huber"}, {"robustScale", 0.0}}, "robustScale"},
      {Json{{"multiScaleLevels", 2}, {"coarseVoxelSize", 0.0}}, "coarseVoxelSize"},
      {Json{{"multiScaleLevels", 7}}, "multiScaleLevels"},
      {Json{{"maxIterations", 4294967297LL}}, "maxIterations"},
      {Json{{"normalKnn", 2147483648LL}}, "normalKnn"},
      {Json{{"smoothLength", 4294967297LL}}, "smoothLength"},
      {Json{{"multiScaleLevels", 6}, {"maxMatchingDist", 1e308}}, "maxMatchingDist"}};
  for (const auto& entry : invalid) {
    CAPTURE(entry.second);
    const auto found = diagnostics(entry.first);
    REQUIRE(found.size() == 1);
    CHECK(found[0]["phase"] == "validate");
    CHECK(found[0]["code"] == "bad_param");
    CHECK(found[0]["paramPath"] == entry.second);
  }
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
