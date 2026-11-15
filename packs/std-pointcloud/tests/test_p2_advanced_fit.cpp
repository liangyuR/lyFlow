#include <doctest/doctest.h>
#include <Eigen/Geometry>
#include <limits>
#include <random>

#include "helpers.h"
#include "../algo/p2_fit.h"
#include "../ops/p1_common.h"
#include "lyflow/pose.h"
#include "lyflow/kernel/constraints.h"

namespace lyflow::ops {
void registerAdvancedFit3D(Registry&);
}

namespace {
using namespace lyflow;
using namespace lyflow::ops::p1;
using Json = nlohmann::json;

const Registry& advancedRegistry() {
  static Registry registry;
  static const bool initialized = [] {
    ops::registerAdvancedFit3D(registry);
    return true;
  }();
  (void)initialized;
  return registry;
}

PointCloud advancedCloud() {
  PointCloud cloud;
  for (const Vec3& p : {Vec3(0, 0, 0), Vec3(2, 0, 0), Vec3(0, 3, 0), Vec3(0, 0, 4), Vec3(1, 2, 3),
                        Vec3(-2, 1, 0.5), Vec3(3, -2, 1)}) {
    cloud.push(static_cast<float>(p.x()), static_cast<float>(p.y()), static_cast<float>(p.z()));
    cloud.intensity.push_back(static_cast<float>(cloud.pointCount()));
    cloud.normals.insert(cloud.normals.end(), {0, 0, 1});
    cloud.rgb.insert(cloud.rgb.end(), {5, 15, 25});
  }
  return cloud;
}

Mat4 knownPose(const Vec3& t = Vec3(0.12, -0.08, 0.05), const Vec3& r = Vec3(0.08, -0.04, 0.12)) {
  Mat4 pose = Mat4::Identity();
  pose.topLeftCorner<3, 3>() =
      (Eigen::AngleAxisd(r.z(), Vec3::UnitZ()) * Eigen::AngleAxisd(r.y(), Vec3::UnitY()) *
       Eigen::AngleAxisd(r.x(), Vec3::UnitX()))
          .toRotationMatrix();
  pose.topRightCorner<3, 1>() = t;
  return pose;
}

Transform nativePose(const Mat4& pose) {
  Transform result;
  REQUIRE(transform(pose, result));
  return result;
}

PointCloud cloudAt(const PointCloud& source, const Mat4& pose) {
  PointCloud cloud;
  test::StubContext context;
  REQUIRE(transformed(source, pose, cloud, context));
  return cloud;
}

const Json& advancedQuality(const test::OpCall& call) {
  REQUIRE(call.outputs.count("quality") == 1);
  REQUIRE(call.outputs.at("quality").asRecord() != nullptr);
  return call.outputs.at("quality").asRecord()->data;
}

Record constraintRecord(Json items) {
  return {"cloud.Constraints3D.v1",
          {{"sourceFrame", "source"},
           {"targetFrame", "target"},
           {"unit", "m"},
           {"items", std::move(items)}}};
}

Json pointConstraint(const std::string& id, const Vec3& source, const Vec3& target,
                     double weight = 1) {
  return {{"id", id},
          {"type", "point"},
          {"source", array(source)},
          {"target", array(target)},
          {"weight", weight}};
}

void closeTransform(const Transform& actual, const Mat4& expected, double tolerance = 2e-6) {
  for (int i = 0; i < 16; ++i)
    CHECK(std::abs(actual.m[i] - expected(i / 4, i % 4)) <= tolerance);
}

class AdvancedCancel final : public ExecContext {
public:
  mutable unsigned polls = 0;
  bool cancelled() const override { return ++polls >= 3; }
  void progress(float, std::string_view) override {}
  void log(LogLevel, std::string) override {}
  const std::filesystem::path& baseDir() const override { return base; }
  int threadBudget() const override { return 1; }
  std::filesystem::path base;
};
} // namespace

TEST_CASE("P2 advanced fit observes mixed weighted point line and plane constraints") {
  const PointCloud source = advancedCloud();
  const Mat4 truth = knownPose();
  Json items = Json::array();
  for (std::size_t i = 0; i < source.pointCount(); ++i) {
    const Vec3 p = point(source, i),
               q = truth.topLeftCorner<3, 3>() * p + truth.topRightCorner<3, 1>();
    Json item = pointConstraint("constraint-" + std::to_string(i), p, q, i + 1);
    if (i == 4) {
      item["type"] = "line";
      item["direction"] = {0, 0, 1};
    }
    if (i >= 5) {
      item["type"] = "plane";
      item["direction"] = i == 5 ? Json{1, 0, 0} : Json{0, 1, 0};
      item["offset"] = 0.025;
      item["target"] = array(q - Vec3(i == 5 ? 0.025 : 0, i == 6 ? 0.025 : 0, 0));
    }
    items.push_back(item);
  }
  test::OpCall call(advancedRegistry());
  call.inputs = {{"constraints", Data::record(constraintRecord(items))},
                 {"cloud", Data::cloud(source)}};
  REQUIRE(call.run("register.constrained_fit_3d",
                   {{"maxRmse", Value::number(2e-6)}, {"maxError", Value::number(3e-6)}})
              .ok);
  closeTransform(*call.out("transform").asTransform(), truth);
  const auto& q = advancedQuality(call);
  CHECK(q["accepted"] == true);
  CHECK(q["converged"] == true);
  CHECK(q["conditionRatio"].get<double>() > 1e-10);
  CHECK(q["weightedRmse"].get<double>() < 2e-6);
  CHECK(q["maxError"].get<double>() < 3e-6);
  REQUIRE(q["items"].size() == items.size());
  double sum = 0, weights = 0, maximum = 0;
  const auto& aligned = *call.out("aligned").asCloud();
  CHECK(aligned.intensity == source.intensity);
  CHECK(aligned.rgb == source.rgb);
  CHECK(source.xyz == advancedCloud().xyz);
  for (std::size_t i = 0; i < items.size(); ++i) {
    Vec3 error = point(aligned, i) -
                 Vec3(items[i]["target"][0], items[i]["target"][1], items[i]["target"][2]);
    if (i == 4)
      error.z() = 0;
    if (i == 5)
      error = Vec3(error.x() - 0.025, 0, 0);
    if (i == 6)
      error = Vec3(0, error.y() - 0.025, 0);
    CHECK(q["items"][i]["after"].get<double>() == doctest::Approx(error.norm()).epsilon(1e-9));
    const double weight = q["items"][i]["weight"];
    sum += weight * error.squaredNorm();
    weights += weight;
    maximum = std::max(maximum, error.norm());
  }
  CHECK(q["weightedRmse"].get<double>() == doctest::Approx(std::sqrt(sum / weights)).epsilon(1e-9));
  CHECK(q["maxError"].get<double>() == doctest::Approx(maximum).epsilon(1e-9));
}

TEST_CASE("P2 extracted constraint directions retain Eigen stable norm semantics") {
  std::mt19937_64 random(20261116);
  std::uniform_real_distribution<double> component(-1, 1);
  std::vector<std::array<double, 3>> directions{
      {0, 0, 0},
      {0, 3, 4},
      {1e308, 0, 0},
      {std::numeric_limits<double>::denorm_min(), 0, 0},
      {std::numeric_limits<double>::denorm_min(), std::numeric_limits<double>::denorm_min(), 0},
      {std::numeric_limits<double>::max(), std::numeric_limits<double>::max(), 0},
      {1, 1e-8, 0},
      {1, 1e-200, 1e-100}};
  for (unsigned i = 0; i < 512; ++i) {
    const double scale = std::pow(10.0, static_cast<int>(random() % 617) - 308);
    directions.push_back(
        {component(random) * scale, component(random) * scale, component(random) * scale});
  }
  for (const auto& direction : directions) {
    const Vec3 before(direction[0], direction[1], direction[2]);
    const double magnitude = before.stableNorm();
    Json item = pointConstraint("line", Vec3::Zero(), Vec3::Zero());
    item["type"] = "line";
    item["direction"] = direction;
    kernel::constraints::Error error;
    std::vector<kernel::constraints::Constraint> parsed;
    const bool accepted = kernel::constraints::parse3D(constraintRecord(Json::array({item})).data,
                                                       "source", "target", "mixed", parsed, error);
    CHECK(accepted == (magnitude > 0 && std::isfinite(magnitude)));
    if (accepted) {
      REQUIRE(parsed.size() == 1);
      const Vec3 expected = before / magnitude;
      for (int axis = 0; axis < 3; ++axis)
        CHECK(parsed[0].direction[axis] == expected[axis]);
    }
  }
}

TEST_CASE("P2 weighted 321 datum constraints recover all six degrees with signed offsets") {
  const Mat4 truth = knownPose();
  const std::array<Vec3, 6> points{Vec3(0, 0, 0), Vec3(2, 0, 0), Vec3(0, 3, 0),
                                   Vec3(0, 0, 0), Vec3(2, 0, 0), Vec3(0, 0, 0)};
  Json items = Json::array();
  for (std::size_t i = 0; i < points.size(); ++i) {
    const int datum = i < 3 ? 0 : (i < 5 ? 1 : 2);
    const Vec3 axis = datum == 0   ? Vec3::UnitZ().eval()
                      : datum == 1 ? Vec3::UnitY().eval()
                                   : Vec3::UnitX().eval();
    const Vec3 q = truth.topLeftCorner<3, 3>() * points[i] + truth.topRightCorner<3, 1>();
    const double offset = (i + 1) * 0.001;
    items.push_back({{"id", "datum-" + std::to_string(i)},
                     {"type", "axis"},
                     {"source", array(points[i])},
                     {"target", array(q - offset * axis)},
                     {"direction", array(axis)},
                     {"offset", offset},
                     {"weight", i + 1},
                     {"datum", datum}});
  }
  test::OpCall call(advancedRegistry());
  call.inputs = {{"constraints", Data::record(constraintRecord(items))}};
  REQUIRE(call.run("register.constrained_fit_3d", {{"mode", Value::text("weighted_321")}}).ok);
  closeTransform(*call.out("transform").asTransform(), truth);
  for (const auto& item : advancedQuality(call)["items"])
    CHECK(std::abs(item["signedAfter"].get<double>()) < 1e-6);
  items[5]["direction"] = {0, 0, 1};
  call.inputs["constraints"] = Data::record(constraintRecord(items));
  CHECK_FALSE(call.run("register.constrained_fit_3d", {{"mode", Value::text("weighted_321")}}).ok);
  CHECK(call.outputs.empty());
}

TEST_CASE("P2 weighted fit uses weights and refuses unobservable or invalid constraints") {
  const PointCloud cloud = advancedCloud();
  Json items = Json::array();
  for (std::size_t i = 0; i < cloud.pointCount(); ++i)
    items.push_back(pointConstraint(std::to_string(i), point(cloud, i), point(cloud, i)));
  items.push_back(pointConstraint("outlier", Vec3(0, 0, 0), Vec3(1, 0, 0), 1e-5));
  test::OpCall call(advancedRegistry());
  call.inputs = {{"constraints", Data::record(constraintRecord(items))}};
  REQUIRE(call.run("register.constrained_fit_3d",
                   {{"maxRmse", Value::number(1)}, {"maxError", Value::number(2)}})
              .ok);
  CHECK(std::abs(call.out("transform").asTransform()->m[3]) < 1e-4);
  const double weighted = std::abs(call.out("transform").asTransform()->m[3]);
  items.back()["weight"] = 1;
  call.inputs["constraints"] = Data::record(constraintRecord(items));
  REQUIRE(call.run("register.constrained_fit_3d",
                   {{"maxRmse", Value::number(1)}, {"maxError", Value::number(2)}})
              .ok);
  CHECK(std::abs(call.out("transform").asTransform()->m[3]) > weighted * 100);
  REQUIRE(call.run("register.constrained_fit_3d", {{"huberDelta", Value::number(0.001)},
                                                   {"maxRmse", Value::number(1)},
                                                   {"maxError", Value::number(2)}})
              .ok);
  CHECK(std::abs(call.out("transform").asTransform()->m[3]) < 0.01);
  for (const char* issue :
       {"zero-weight", "duplicate", "direction", "unknown", "unit", "unobservable"}) {
    SUBCASE(issue) {
      Json copy = items;
      Record record = constraintRecord(copy);
      if (issue == std::string("zero-weight"))
        record.data["items"][0]["weight"] = 0;
      if (issue == std::string("duplicate"))
        record.data["items"][1]["id"] = "0";
      if (issue == std::string("direction")) {
        record.data["items"][0]["type"] = "plane";
        record.data["items"][0]["direction"] = {0, 0, 0};
      }
      if (issue == std::string("unknown"))
        record.data["items"][0]["typo"] = true;
      if (issue == std::string("unit"))
        record.data["unit"] = "mm";
      if (issue == std::string("unobservable"))
        for (auto& item : record.data["items"]) {
          item["type"] = "plane";
          item["direction"] = {0, 0, 1};
        }
      call.inputs["constraints"] = Data::record(std::move(record));
      CHECK_FALSE(call.run("register.constrained_fit_3d").ok);
      CHECK(call.outputs.empty());
    }
  }
}

TEST_CASE("P2 six axis Cartesian grid finds combined XYZ translation and rotation") {
  const PointCloud source = advancedCloud();
  const Vec3 translation(0.1, 0.2, 0.3), rotation(0.08, 0.12, 0.16);
  const Mat4 truth = knownPose(translation, rotation);
  test::OpCall call(advancedRegistry());
  call.inputs = {{"source", Data::cloud(source)}, {"target", Data::cloud(cloudAt(source, truth))}};
  REQUIRE(
      call.run("register.pose_search_3d", {{"translationRadius", Value::vec(array(translation))},
                                           {"rotationRadius", Value::vec(array(rotation))},
                                           {"maxCorrespondenceDistance", Value::number(0.05)},
                                           {"maxRmse", Value::number(2e-6)},
                                           {"minInlierRatio", Value::number(1)}})
          .ok);
  const auto& quality = advancedQuality(call)["items"][0];
  CHECK(quality["evaluated"] == 729);
  CHECK(quality["candidates"].size() == 729);
  CHECK(quality["rmse"].get<double>() < 2e-6);
  CHECK(quality["inlierRatio"] == 1);
  for (int i = 0; i < 16; ++i)
    CHECK(quality["transform"][i].get<double>() ==
          doctest::Approx(truth(i / 4, i % 4)).epsilon(1e-6));
  const auto& aligned = *call.out("aligned").asCloud();
  CHECK(aligned.rgb == source.rgb);
  CHECK(aligned.intensity == source.intensity);
  const auto& target = *call.inputs.at("target").asCloud();
  double sum = 0;
  for (std::size_t i = 0; i < source.pointCount(); ++i)
    sum += (point(aligned, i) - point(target, i)).squaredNorm();
  CHECK(quality["rmse"].get<double>() ==
        doctest::Approx(std::sqrt(sum / source.pointCount())).epsilon(1e-9));
}

TEST_CASE(
    "P2 hierarchical local search refines coarse rejected candidates within original bounds") {
  const PointCloud source = advancedCloud();
  const Mat4 truth = knownPose(Vec3(0.0375, 0, 0), Vec3::Zero());
  test::OpCall call(advancedRegistry());
  call.inputs = {{"source", Data::cloud(source)}, {"target", Data::cloud(cloudAt(source, truth))}};
  REQUIRE(call.run("register.pose_search_3d", {{"translationRadius", Value::vec({0.1, 0, 0})},
                                               {"rotationRadius", Value::vec({0, 0, 0})},
                                               {"searchMode", Value::text("hierarchical")},
                                               {"levels", Value::integer(4)},
                                               {"maxCorrespondenceDistance", Value::number(0.2)},
                                               {"maxRmse", Value::number(2e-6)},
                                               {"minInlierRatio", Value::number(1)}})
              .ok);
  const auto& quality = advancedQuality(call)["items"][0];
  CHECK(quality["evaluated"] == 12);
  CHECK(quality["transform"][3].get<double>() == doctest::Approx(0.0375).epsilon(1e-6));
  for (const auto& candidate : quality["candidates"])
    CHECK(std::abs(candidate["offset"][0].get<double>()) <= 0.1);
}

TEST_CASE(
    "P2 local search source composition rotates translation axis and keeps object metadata groups") {
  const PointCloud source = advancedCloud();
  const Mat4 initial = knownPose(Vec3::Zero(), Vec3(0, 0, poses::kPi / 2));
  Mat4 local = Mat4::Identity();
  local(0, 3) = 0.1;
  test::OpCall call(advancedRegistry());
  call.inputs = {{"source", Data::cloud(source)},
                 {"target", Data::cloud(cloudAt(source, initial * local))},
                 {"initial", Data::transform(nativePose(initial))}};
  REQUIRE(call.run("register.pose_search_3d", {{"translationRadius", Value::vec({0.1, 0, 0})},
                                               {"rotationRadius", Value::vec({0, 0, 0})},
                                               {"composition", Value::text("source")},
                                               {"maxRmse", Value::number(2e-6)},
                                               {"maxCorrespondenceDistance", Value::number(0.01)},
                                               {"minInlierRatio", Value::number(1)}})
              .ok);
  CHECK(advancedQuality(call)["items"][0]["transform"][7].get<double>() ==
        doctest::Approx(0.1).epsilon(1e-6));
  PointCloud scene;
  Json items = Json::array();
  for (int n = 0; n < 2; ++n) {
    const Vec3 position(n == 0 ? 5 : -3, n == 0 ? 1 : 2, n);
    const PointCloud moved = cloudAt(source, knownPose(position, Vec3::Zero()));
    scene.xyz.insert(scene.xyz.end(), moved.xyz.begin(), moved.xyz.end());
    auto item =
        poses::make3D("part-" + std::to_string(n), "target",
                      {position.x() - 0.1, position.y(), position.z()}, {0, 0, 0, 1}, {2, 3, 4})
            .data;
    item["classLabel"] = "fixture";
    item["attributes"] = {{"serial", n}};
    items.push_back(std::move(item));
  }
  call.inputs.erase("initial");
  call.inputs["poses"] = Data::record(poses::make3DList(items, "target"));
  call.inputs["target"] = Data::cloud(scene);
  REQUIRE(call.run("register.pose_search_3d", {{"translationRadius", Value::vec({0.1, 0, 0})},
                                               {"rotationRadius", Value::vec({0, 0, 0})},
                                               {"maxRmse", Value::number(3e-6)},
                                               {"maxCorrespondenceDistance", Value::number(0.02)},
                                               {"minInlierRatio", Value::number(1)}})
              .ok);
  const auto& poses = call.out("poses").asRecord()->data["items"];
  REQUIRE(poses.size() == 2);
  for (std::size_t i = 0; i < 2; ++i) {
    CHECK(poses[i]["id"] == "part-" + std::to_string(i));
    CHECK(poses[i]["classLabel"] == "fixture");
    CHECK(poses[i]["attributes"]["serial"] == i);
    CHECK(poses[i]["pickable"] == false);
  }
  std::vector<cloud_groups::Group> groups;
  std::string frame, why;
  const auto& aligned = *call.out("aligned").asCloud();
  REQUIRE(cloud_groups::parse(*call.out("groups").asRecord(), aligned,
                              *call.out("indices").asIndices(), groups, frame, why));
  CHECK(frame == "target");
  REQUIRE(groups.size() == 2);
  CHECK(groups[0].id == "part-0");
  CHECK(groups[1].offset == source.pointCount());
  CHECK(aligned.pointCount() == 2 * source.pointCount());
  CHECK(aligned.rgb.size() == 2 * source.rgb.size());
}

TEST_CASE("P2 advanced cloud fitting shares native quality and unique target support") {
  const PointCloud source = advancedCloud();
  const Mat4 truth = knownPose(Vec3(0.02, -0.01, 0.03), Vec3(0.01, 0.02, -0.01));
  const PointCloud target = cloudAt(source, truth);
  test::StubContext context;
  algo::p2::TargetIndex index;
  REQUIRE(algo::p2::prepareTarget(target, index, context).ok);
  algo::p2::CloudFitOptions options;
  options.maxCorrespondenceDistance = 0.5;
  options.maxRmse = 1e-5;
  options.minInlierRatio = 1;
  algo::p2::FitBudget budget;
  algo::p2::CloudFitResult fit;
  REQUIRE(
      algo::p2::refineCloudFit(source, index, Mat4::Identity(), options, context, budget, fit).ok);
  REQUIRE(fit.accepted);
  CHECK(fit.converged);
  closeTransform(fit.transform, truth);
  CHECK(fit.targetSupport.size() == source.pointCount());
  CHECK(fit.targetIndices.size() == source.pointCount());
  double sum = 0;
  for (std::size_t i = 0; i < source.pointCount(); ++i) {
    CHECK(fit.targetIndices[i] == static_cast<std::int32_t>(i));
    sum += (point(fit.aligned, i) - point(target, i)).squaredNorm();
  }
  CHECK(fit.rmse == doctest::Approx(std::sqrt(sum / source.pointCount())).epsilon(1e-9));
  budget.maximumQueries = budget.queries;
  CHECK_FALSE(algo::p2::evaluateCloudFit(source, index, truth, options, context, budget, fit).ok);
  AdvancedCancel cancel;
  budget = {};
  CHECK_FALSE(
      algo::p2::refineCloudFit(source, index, Mat4::Identity(), options, cancel, budget, fit).ok);
}

TEST_CASE(
    "P2 local search rejects budget invalid input and batch failures without fabricated poses") {
  test::OpCall call(advancedRegistry());
  const PointCloud cloud = advancedCloud();
  call.inputs = {{"source", Data::cloud(cloud)}, {"target", Data::cloud(cloud)}};
  CHECK_FALSE(call.run("register.pose_search_3d", {{"translationRadius", Value::vec({1, 1, 1})},
                                                   {"rotationRadius", Value::vec({1, 1, 1})},
                                                   {"steps", Value::integer(9)}})
                  .ok);
  CHECK(call.outputs.empty());
  CHECK_FALSE(call.run("register.pose_search_3d", {{"steps", Value::integer(1)}}).ok);
  CHECK_FALSE(call.run("register.pose_search_3d", {{"translationRadius", Value::vec({0, 0, 0})},
                                                   {"rotationRadius", Value::vec({0, 0, 0})},
                                                   {"maxEvaluations", Value::integer(0)}})
                  .ok);
  auto far = poses::make3D("failed", "target", {100, 0, 0}, {0, 0, 0, 1}, {1, 1, 1});
  call.inputs["poses"] = Data::record(poses::make3DList(Json::array({far.data}), "target"));
  CHECK_FALSE(call.run("register.pose_search_3d").ok);
  CHECK(call.outputs.empty());
  call.inputs["initial"] = Data::transform(Transform{});
  CHECK_FALSE(call.run("register.pose_search_3d").ok);
  call.inputs.erase("initial");
  call.inputs.erase("poses");
  PointCloud broken = cloud;
  broken.xyz[0] = std::numeric_limits<float>::quiet_NaN();
  call.inputs["source"] = Data::cloud(broken);
  CHECK_FALSE(call.run("register.pose_search_3d").ok);
  CHECK(call.outputs.empty());
}

TEST_CASE(
    "P2 final search winner comes from accepted candidates rather than rejected higher coverage") {
  PointCloud source;
  for (const Vec3& p : {Vec3(0, 0, 0), Vec3(10, 0, 0), Vec3(0, 10, 0), Vec3(0, 0, 10),
                        Vec3(10, 10, 10), Vec3(20, 0, 0), Vec3(0, 20, 0)})
    source.push(static_cast<float>(p.x()), static_cast<float>(p.y()), static_cast<float>(p.z()));
  PointCloud target = source;
  target.xyz[3 * 5] += 0.2f;
  target.xyz[3 * 6] += 0.2f;
  test::OpCall call(advancedRegistry());
  call.inputs = {{"source", Data::cloud(source)}, {"target", Data::cloud(target)}};
  for (const char* mode : {"grid", "hierarchical"}) {
    REQUIRE(call.run("register.pose_search_3d", {{"translationRadius", Value::vec({0.1, 0, 0})},
                                                 {"rotationRadius", Value::vec({0, 0, 0})},
                                                 {"searchMode", Value::text(mode)},
                                                 {"levels", Value::integer(3)},
                                                 {"maxCorrespondenceDistance", Value::number(0.11)},
                                                 {"maxRmse", Value::number(0.05)},
                                                 {"minInlierRatio", Value::number(0.7)}})
                .ok);
    const auto& quality = advancedQuality(call)["items"][0];
    CHECK(quality["accepted"] == true);
    CHECK(quality["rmse"].get<double>() == doctest::Approx(0).epsilon(1e-9));
    CHECK(quality["inlierRatio"].get<double>() == doctest::Approx(5.0 / 7));
    CHECK(quality["transform"][3].get<double>() == doctest::Approx(0).epsilon(1e-9));
    bool rejectedHigherCoverage = false;
    for (const auto& candidate : quality["candidates"])
      if (!candidate["accepted"].get<bool>() && candidate["inlierRatio"].get<double>() == 1)
        rejectedHigherCoverage = true;
    CHECK(rejectedHigherCoverage);
  }
}

TEST_CASE("P2 delivered fit rejects coincident collinear and float32 collapsed supports") {
  for (int fixture = 0; fixture < 5; ++fixture) {
    PointCloud source, target;
    Mat4 pose = Mat4::Identity();
    if (fixture == 0) {
      for (int i = 0; i < 3; ++i)
        source.push(static_cast<float>(i), 0, 0);
      target = source;
    } else if (fixture == 1) {
      for (int i = 0; i < 3; ++i)
        source.push(0, 0, 0);
      target = source;
    } else if (fixture == 2) {
      source.push(0, 0, 0);
      source.push(0.1f, 0, 0);
      source.push(0, 0.1f, 0);
      pose.topRightCorner<3, 1>() = Vec3::Constant(1e8);
      for (int i = 0; i < 3; ++i)
        target.push(1e8f, 1e8f, 1e8f);
    } else {
      source.push(0, 0, 0);
      source.push(1, fixture == 3 ? 0.01f : 0, 0);
      source.push(2, 0, 0);
      target.push(0, 0, 0);
      target.push(1, fixture == 4 ? 0.01f : 0, 0);
      target.push(2, 0, 0);
    }
    test::StubContext ctx;
    algo::p2::TargetIndex index;
    REQUIRE(algo::p2::prepareTarget(target, index, ctx).ok);
    algo::p2::CloudFitOptions options;
    options.maxCorrespondenceDistance = 0.1;
    options.maxRmse = 0.1;
    options.minInlierRatio = 1;
    algo::p2::FitBudget budget;
    algo::p2::CloudFitResult result;
    REQUIRE(algo::p2::evaluateCloudFit(source, index, pose, options, ctx, budget, result).ok);
    CHECK(result.inlierRatio == 1);
    CHECK_FALSE(result.observable);
    CHECK_FALSE(result.accepted);
    CHECK_FALSE(result.converged);
    if (fixture <= 2)
      CHECK(result.rmse == 0);
    if (fixture == 2)
      CHECK(result.targetSupport.size() == 1);
  }
}

TEST_CASE("P2 final point plane quality verifies six dimensional native support observability") {
  PointCloud plane;
  for (int x : {-1, 0, 1})
    for (int y : {-1, 0, 1}) {
      plane.push(static_cast<float>(x), static_cast<float>(y), 0);
      plane.normals.insert(plane.normals.end(), {0, 0, 1});
    }
  test::StubContext ctx;
  algo::p2::TargetIndex index;
  REQUIRE(algo::p2::prepareTarget(plane, index, ctx).ok);
  algo::p2::CloudFitOptions options;
  options.pointToPlane = true;
  algo::p2::FitBudget budget;
  algo::p2::CloudFitResult result;
  REQUIRE(
      algo::p2::evaluateCloudFit(plane, index, Mat4::Identity(), options, ctx, budget, result).ok);
  CHECK(result.targetSupport.size() == plane.pointCount());
  CHECK(result.inlierRatio == 1);
  CHECK(result.rmse == 0);
  CHECK_FALSE(result.observable);
  CHECK_FALSE(result.accepted);
  options.pointToPlane = false;
  REQUIRE(
      algo::p2::evaluateCloudFit(plane, index, Mat4::Identity(), options, ctx, budget, result).ok);
  CHECK(result.observable);
  CHECK(result.accepted);
}

TEST_CASE("P2 alignment cancellation keeps cancelled classification and atomic outputs") {
  const PointCloud source = advancedCloud();
  test::StubContext stable;
  algo::p2::TargetIndex index;
  REQUIRE(algo::p2::prepareTarget(source, index, stable).ok);
  algo::p2::CloudFitOptions options;
  algo::p2::FitBudget budget;
  algo::p2::CloudFitResult result;
  AdvancedCancel cancelled;
  cancelled.polls = 2;
  const auto status = algo::p2::evaluateCloudFit(source, index, Mat4::Identity(), options,
                                                 cancelled, budget, result);
  CHECK_FALSE(status.ok);
  CHECK(status.code == "cancelled");
  Json items = Json::array();
  for (std::size_t i = 0; i < source.pointCount(); ++i)
    items.push_back(pointConstraint(std::to_string(i), point(source, i), point(source, i)));
  PointCloud large;
  large.xyz.resize(3 * 8193, 0);
  std::unordered_map<std::string, Data> inputs{
      {"constraints", Data::record(constraintRecord(items))}, {"cloud", Data::cloud(large)}};
  std::unordered_map<std::string, Data> outputs;
  const auto* op = advancedRegistry().find("register.constrained_fit_3d");
  REQUIRE(op != nullptr);
  ParamMap params;
  for (const auto& param : op->params)
    params.emplace(param.name, param.def);
  AdvancedCancel duringAlignment;
  std::filesystem::path base;
  Outputs out(outputs);
  const auto stopped = op->compute(Inputs(inputs), ParamView(params, base), out, duringAlignment);
  CHECK_FALSE(stopped.ok);
  CHECK(stopped.code == "cancelled");
  CHECK(outputs.empty());
}
