#include <doctest/doctest.h>

#include <Eigen/Geometry>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <tuple>

#include "helpers.h"
#include "lyflow/cloud_groups.h"
#include "lyflow/c_api.h"
#include "../ops/p1_common.h"
#include "../ops/ops.h"

namespace {
using namespace lyflow;
using namespace lyflow::ops::p1;

PointCloud fixture() {
  PointCloud cloud;
  for (const Vec3& p : {Vec3(0, 0, 0), Vec3(1, 0, 0), Vec3(0, 2, 0), Vec3(0, 0, 3), Vec3(1, 2, 3),
                        Vec3(-1, 0.5, 0.7)}) {
    cloud.push(static_cast<float>(p.x()), static_cast<float>(p.y()), static_cast<float>(p.z()));
    cloud.intensity.push_back(static_cast<float>(cloud.pointCount()));
    cloud.normals.insert(cloud.normals.end(), {0, 0, 1});
    cloud.rgb.insert(cloud.rgb.end(), {1, 2, 3});
  }
  return cloud;
}

Transform native(const Mat4& matrix) {
  Transform result;
  REQUIRE(transform(matrix, result));
  return result;
}

PointCloud moved(const PointCloud& source, const Mat4& matrix) {
  test::StubContext context;
  PointCloud result;
  REQUIRE(transformed(source, matrix, result, context));
  return result;
}

const nlohmann::json& data(const test::OpCall& call, const char* port) {
  const auto* record = call.outputs.at(port).asRecord();
  REQUIRE(record != nullptr);
  return record->data;
}

void checkTransform(const Transform& transform, const Mat4& expected, double tolerance = 1e-5) {
  for (int i = 0; i < 16; ++i)
    CHECK(std::abs(transform.m[i] - expected(i / 4, i % 4)) <= tolerance);
}

struct Scratch {
  std::filesystem::path path;
  Scratch() {
    static std::atomic<unsigned> counter{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (;;) {
      path = std::filesystem::temp_directory_path() /
             ("lyflow-p1-cloud-" + std::to_string(stamp) + "-" + std::to_string(counter++));
      if (std::filesystem::create_directory(path))
        break;
    }
  }
  ~Scratch() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};

class CancelContext final : public ExecContext {
public:
  mutable int polls = 0;
  bool cancelled() const override { return ++polls >= 3; }
  void progress(float, std::string_view) override {}
  void log(LogLevel, std::string) override {}
  const std::filesystem::path& baseDir() const override { return base_; }
  int threadBudget() const override { return 1; }

private:
  std::filesystem::path base_;
};
}

TEST_CASE("P1 cloud statistics expose finite height PCA boxes and degeneracy") {
  test::OpCall call;
  PointCloud cloud = fixture();
  call.inputs = {{"cloud", Data::cloud(cloud)}};
  for (const char* id : {"cloud.statistics", "cloud.pca", "cloud.bounds_obb"}) {
    REQUIRE(call.run(id).ok);
    const auto& stats = data(call, "statistics");
    CHECK(stats["count"] == cloud.pointCount());
    CHECK(stats["valid"] == true);
    CHECK(call.outputs.at("height").asMeasurement()->value == doctest::Approx(3));
    Mat4 box;
    REQUIRE(rigid(*call.outputs.at("pose").asTransform(), box));
    for (std::size_t i = 0; i < cloud.pointCount(); ++i) {
      const Vec3 local =
          box.topLeftCorner<3, 3>().transpose() * (point(cloud, i) - box.topRightCorner<3, 1>());
      for (int j = 0; j < 3; ++j)
        CHECK(std::abs(local[j]) <= stats["dimensions"][j].get<double>() / 2 + 1e-5);
    }
  }
  call.inputs = {{"cloud", Data::cloud(PointCloud{})}};
  REQUIRE(call.run("cloud.pca").ok);
  CHECK(data(call, "statistics")["valid"] == false);
  CHECK_FALSE(call.outputs.at("height").asMeasurement()->ok);
  PointCloud line;
  for (int i = 0; i < 4; ++i)
    line.push(float(i), 0, 0);
  call.inputs = {{"cloud", Data::cloud(line)}};
  REQUIRE(call.run("cloud.pca").ok);
  CHECK(data(call, "statistics")["orientationStable"] == false);
  line.xyz[0] = std::numeric_limits<float>::quiet_NaN();
  call.inputs = {{"cloud", Data::cloud(line)}};
  CHECK_FALSE(call.run("cloud.statistics").ok);
}

TEST_CASE("P1 cloud groups preserve ordered identities channels and native source ownership") {
  test::OpCall call;
  const PointCloud original = fixture();
  call.inputs = {{"a", Data::cloud(original)}, {"b", Data::cloud(original)}};
  REQUIRE(call.run("cloud.group_concat").ok);
  const auto grouped = call.outputs;
  CHECK(data(call, "groups")["groups"].size() == 2);
  call.inputs = grouped;
  REQUIRE(call.run("cloud.group_get", {{"index", Value::integer(1)}}).ok);
  const auto* extracted = call.outputs.at("cloud").asCloud();
  CHECK(extracted->xyz == original.xyz);
  CHECK(extracted->rgb == original.rgb);
  CHECK(extracted->normals == original.normals);
  CHECK(extracted->intensity == original.intensity);
  CHECK(call.outputs.at("indices").asIndices()->sourceCloudId == grouped.at("cloud").asCloud()->id);
  call.inputs = grouped;
  REQUIRE(call.run("cloud.group_subset", {{"selection", Value::text("[1,0]")}}).ok);
  CHECK(data(call, "groups")["groups"][0]["id"] == "b/group-0000");
  CHECK_FALSE(call.run("cloud.group_subset", {{"selection", Value::text("[0,0]")}}).ok);
  CHECK_FALSE(call.run("cloud.group_subset", {{"selection", Value::text(std::string(20000, '[') +
                                                                        std::string(20000, ']'))}})
                  .ok);
  CHECK_FALSE(call.run("cloud.group_get", {{"index", Value::integer(2)}}).ok);
  Indices bad = *grouped.at("indices").asIndices();
  bad.sourceCloudId = original.id;
  call.inputs["indices"] = Data::indices(bad);
  CHECK_FALSE(call.run("cloud.group_merge").ok);
  call.inputs = grouped;
  REQUIRE(call.run("cloud.group_merge").ok);
  CHECK(call.outputs.at("cloud").asCloud()->pointCount() == 2 * original.pointCount());
  call.inputs = {{"a", grouped.at("cloud")},
                 {"b", Data::cloud(original)},
                 {"indicesA", grouped.at("indices")},
                 {"groupsA", grouped.at("groups")}};
  REQUIRE(call.run("cloud.group_concat").ok);
  CHECK(data(call, "groups")["groups"].size() == 3);
  CHECK(data(call, "groups")["groups"][0]["id"] == "a/a/group-0000");
  call.inputs.erase("indicesA");
  CHECK_FALSE(call.run("cloud.group_concat").ok);
  PointCloud missing = original;
  missing.rgb.clear();
  call.inputs = {{"a", Data::cloud(original)}, {"b", Data::cloud(missing)}};
  CHECK_FALSE(call.run("cloud.group_concat").ok);
  auto record = *grouped.at("groups").asRecord();
  record.data["groups"][0]["count"] = 100000;
  call.inputs = grouped;
  call.inputs["groups"] = Data::record(record);
  CHECK_FALSE(call.run("cloud.group_get").ok);
}

TEST_CASE(
    "P1 Euclidean clusters distinguish Z and multi-plane extraction retains original indices") {
  test::OpCall call;
  PointCloud cloud;
  for (float z : {0.0f, 1.0f})
    for (int x = 0; x < 7; ++x)
      for (int y = 0; y < 7; ++y)
        cloud.push(x * 0.01f, y * 0.01f, z);
  call.inputs = {{"cloud", Data::cloud(cloud)}};
  REQUIRE(call.run("segment.euclidean_clusters", {{"tolerance", Value::number(0.02)}}).ok);
  CHECK(data(call, "groups")["groups"].size() == 2);
  const auto groups = call.outputs;
  call.inputs = groups;
  REQUIRE(call.run("cloud.group_statistics").ok);
  CHECK(data(call, "statistics")["groups"][0]["center"][2] == 0);
  CHECK(data(call, "statistics")["groups"][1]["center"][2] == 1);
  const auto expectedGroups = data(call, "statistics")["groups"];
  std::vector<std::int32_t> reverse(cloud.pointCount());
  std::iota(reverse.rbegin(), reverse.rend(), 0);
  call.inputs = {{"cloud", Data::cloud(cloud.select(reverse))}};
  REQUIRE(call.run("segment.euclidean_clusters", {{"tolerance", Value::number(0.02)}}).ok);
  call.inputs = call.outputs;
  REQUIRE(call.run("cloud.group_statistics").ok);
  CHECK(data(call, "statistics")["groups"][0]["center"] == expectedGroups[0]["center"]);
  CHECK(data(call, "statistics")["groups"][1]["center"] == expectedGroups[1]["center"]);
  PointCloud concentric;
  for (double radius : {1.0, 3.0}) {
    std::vector<Vec3> half;
    for (int i = 0; i < 8; ++i) {
      const double angle = i * std::acos(-1.0) / 8;
      half.emplace_back(static_cast<float>(radius * std::cos(angle)),
                        static_cast<float>(radius * std::sin(angle)), 0);
    }
    for (double sign : {1.0, -1.0})
      for (const auto& v : half)
        concentric.push(static_cast<float>(sign * v.x()), static_cast<float>(sign * v.y()), 0);
  }
  nlohmann::json ringOrder;
  for (int permutation = 0; permutation < 3; ++permutation) {
    std::vector<std::int32_t> order(concentric.pointCount());
    std::iota(order.begin(), order.end(), 0);
    if (permutation == 1)
      std::rotate(order.begin(), order.begin() + 16, order.end());
    if (permutation == 2)
      std::reverse(order.begin(), order.end());
    call.inputs = {{"cloud", Data::cloud(concentric.select(order))}};
    REQUIRE(call.run("segment.euclidean_clusters",
                     {{"tolerance", Value::number(1.2)}, {"minPoints", Value::integer(1)}})
                .ok);
    call.inputs = call.outputs;
    REQUIRE(call.run("cloud.group_statistics").ok);
    const auto& stats = data(call, "statistics")["groups"];
    REQUIRE(stats.size() == 2);
    if (permutation == 0)
      ringOrder = stats;
    for (int group = 0; group < 2; ++group) {
      CHECK(stats[group]["id"] == ringOrder[group]["id"]);
      CHECK(stats[group]["center"] == ringOrder[group]["center"]);
      CHECK(stats[group]["minimum"] == ringOrder[group]["minimum"]);
      CHECK(stats[group]["maximum"] == ringOrder[group]["maximum"]);
    }
  }
  call.inputs = {{"cloud", Data::cloud(cloud)}};
  CHECK_FALSE(call.run("segment.euclidean_clusters",
                       {{"tolerance", Value::number(0.02)}, {"maxGroups", Value::integer(1)}})
                  .ok);
  REQUIRE(call.run("segment.multi_planes", {{"distanceThreshold", Value::number(1e-5)},
                                            {"minPoints", Value::integer(30)},
                                            {"maxPlanes", Value::integer(2)}})
              .ok);
  CHECK(data(call, "planes")["planes"].size() == 2);
  CHECK(call.outputs.at("rest").asCloud()->pointCount() == 0);
  CHECK(call.outputs.at("indices").asIndices()->sourceCloudId ==
        call.outputs.at("cloud").asCloud()->id);
}

TEST_CASE("P1 corresponding fit and ICP recover a nonplanar source-to-target SE3 transform") {
  test::OpCall call;
  const auto source = fixture();
  Mat4 expected = Mat4::Identity();
  expected.topLeftCorner<3, 3>() =
      (Eigen::AngleAxisd(0.015, Vec3::UnitX()) * Eigen::AngleAxisd(-0.02, Vec3::UnitY()) *
       Eigen::AngleAxisd(0.01, Vec3::UnitZ()))
          .toRotationMatrix();
  expected.topRightCorner<3, 1>() = Vec3(0.01, -0.02, 0.03);
  const auto target = moved(source, expected);
  call.inputs = {{"source", Data::cloud(source)}, {"target", Data::cloud(target)}};
  for (const char* id : {"register.rigid_fit", "register.icp_3d", "register.local_search_3d"}) {
    REQUIRE_MESSAGE(call.run(id, {{"maxRmse", Value::number(1e-5)}}).ok, call.status.message);
    checkTransform(*call.outputs.at("transform").asTransform(), expected);
    CHECK(data(call, "quality")["accepted"] == true);
    CHECK(data(call, "quality")["inlierRatio"] == 1);
    CHECK(call.outputs.at("aligned").asCloud()->rgb == source.rgb);
    const auto aligned = *call.outputs.at("aligned").asCloud();
    const auto quality = data(call, "quality");
    test::OpCall apply;
    apply.inputs = {{"cloud", Data::cloud(source)}, {"transform", call.outputs.at("transform")}};
    REQUIRE(apply.run("transform.apply").ok);
    CHECK(apply.outputs.at("cloud").asCloud()->xyz == aligned.xyz);
    double squared = 0;
    for (std::size_t i = 0; i < source.pointCount(); ++i)
      squared += (point(aligned, i) - point(target, i)).squaredNorm();
    CHECK(quality["rmse"].get<double>() ==
          doctest::Approx(std::sqrt(squared / source.pointCount())).epsilon(1e-10));
  }
  CHECK_FALSE(call.run("register.icp_3d",
                       {{"maxIterations", Value::integer(1)}, {"maxRmse", Value::number(1e-5)}})
                  .ok);
  PointCloud partial = target.select({0, 1, 2, 3, 4});
  for (std::size_t i = 0; i < partial.xyz.size(); ++i)
    partial.xyz[i] += static_cast<float>((static_cast<int>(i % 3) - 1) * 1e-4);
  call.inputs = {{"source", Data::cloud(source)}, {"target", Data::cloud(partial)}};
  REQUIRE(call.run("register.icp_3d", {{"maxRmse", Value::number(0.002)}}).ok);
  CHECK(data(call, "quality")["inlierRatio"].get<double>() == doctest::Approx(5.0 / 6));
  CHECK(data(call, "quality")["rmse"].get<double>() < 0.002);
  checkTransform(*call.outputs.at("transform").asTransform(), expected, 0.001);
  CHECK_FALSE(call.run("register.icp_3d", {{"minInlierRatio", Value::number(0.95)}}).ok);
  PointCloud line;
  for (int i = 0; i < 8; ++i)
    line.push(float(i), 0, 0);
  call.inputs = {{"source", Data::cloud(line)}, {"target", Data::cloud(line)}};
  CHECK_FALSE(call.run("register.rigid_fit").ok);
  CHECK_FALSE(call.run("register.icp_3d").ok);
  auto bad = native(expected);
  bad.m[0] = 2;
  call.inputs = {{"source", Data::cloud(source)},
                 {"target", Data::cloud(target)},
                 {"initial", Data::transform(bad)}};
  CHECK_FALSE(call.run("register.icp_3d").ok);
  Mat4 distant = Mat4::Identity();
  distant(0, 3) = 100;
  call.inputs["initial"] = Data::transform(native(distant));
  CHECK_FALSE(call.run("register.icp_3d").ok);
  PointCloud huge;
  huge.push(0, 0, 0);
  huge.push(1e30f, 0, 0);
  huge.push(0, 1e30f, 1e30f);
  call.inputs = {{"source", Data::cloud(huge)}, {"target", Data::cloud(huge)}};
  CHECK_FALSE(call.run("register.icp_3d").ok);
  huge.xyz[0] = std::numeric_limits<float>::infinity();
  call.inputs["source"] = Data::cloud(huge);
  CHECK_FALSE(call.run("register.icp_3d").ok);
  const auto* op = ensureRegistry().find("register.icp_3d");
  REQUIRE(op != nullptr);
  ParamMap parameters;
  for (const auto& parameter : op->params)
    parameters[parameter.name] = parameter.def;
  std::filesystem::path base;
  ParamView view(parameters, base);
  call.inputs = {{"source", Data::cloud(source)}, {"target", Data::cloud(target)}};
  call.outputs.clear();
  Inputs input(call.inputs);
  Outputs output(call.outputs);
  CancelContext context;
  CHECK(op->compute(input, view, output, context).ok);
  CHECK(context.polls >= 3);
  CHECK(call.outputs.empty());
}

TEST_CASE("P1 registration gates the delivered float transform and aligned coordinates") {
  PointCloud source, target;
  const auto rotation = Eigen::AngleAxisd(0.37, Vec3::UnitZ()).toRotationMatrix();
  for (const Vec3& offset : {Vec3(0, 0, 0), Vec3(8, 0, 0), Vec3(0, 16, 0), Vec3(0, 0, 24),
                             Vec3(8, 16, 24), Vec3(-8, 8, -16)}) {
    const Vec3 destination = rotation * offset;
    source.push(static_cast<float>(1e8 + offset.x()), static_cast<float>(1e8 + offset.y()),
                static_cast<float>(1e8 + offset.z()));
    target.push(static_cast<float>(destination.x()), static_cast<float>(destination.y()),
                static_cast<float>(destination.z()));
  }
  test::OpCall fit;
  fit.inputs = {{"source", Data::cloud(source)}, {"target", Data::cloud(target)}};
  const auto rejected = fit.run("register.rigid_fit", {{"maxRmse", Value::number(0.001)}});
  CHECK_FALSE(rejected.ok);
  CHECK(rejected.code == "registration_rejected");
  REQUIRE(fit.run("register.rigid_fit", {{"maxRmse", Value::number(20)}}).ok);
  const auto nativeTransform = fit.outputs.at("transform");
  const auto aligned = *fit.outputs.at("aligned").asCloud();
  double squared = 0;
  for (std::size_t i = 0; i < target.pointCount(); ++i)
    squared += (point(aligned, i) - point(target, i)).squaredNorm();
  const double actual = std::sqrt(squared / target.pointCount());
  CHECK(actual > 1);
  CHECK(data(fit, "quality")["rmse"].get<double>() == doctest::Approx(actual).epsilon(1e-12));
  test::OpCall apply;
  apply.inputs = {{"cloud", Data::cloud(source)}, {"transform", nativeTransform}};
  REQUIRE(apply.run("transform.apply").ok);
  CHECK(apply.outputs.at("cloud").asCloud()->xyz == aligned.xyz);
  for (const char* id : {"register.icp_3d", "register.local_search_3d"}) {
    fit.inputs["initial"] = nativeTransform;
    CHECK_FALSE(fit.run(id, {{"maxRmse", Value::number(0.001)},
                             {"maxCorrespondenceDistance", Value::number(100)},
                             {"minInlierRatio", Value::number(1)}})
                    .ok);
  }
}

TEST_CASE("P1 point-to-plane ICP validates observability and normals") {
  test::OpCall call;
  PointCloud target;
  for (int axis = 0; axis < 3; ++axis)
    for (int i = 0; i < 5; ++i)
      for (int j = 0; j < 5; ++j) {
        Vec3 p = Vec3::Zero(), n = Vec3::Zero();
        p[axis] = 0.5;
        p[(axis + 1) % 3] = i * 0.1;
        p[(axis + 2) % 3] = j * 0.1;
        n[axis] = 1;
        target.push(float(p.x()), float(p.y()), float(p.z()));
        for (int k = 0; k < 3; ++k)
          target.normals.push_back(float(n[k]));
      }
  Mat4 expected = Mat4::Identity();
  expected.topLeftCorner<3, 3>() =
      Eigen::AngleAxisd(0.005, Vec3(1, 2, 3).normalized()).toRotationMatrix();
  expected.topRightCorner<3, 1>() = Vec3(0.003, -0.002, 0.001);
  const auto source = moved(target, expected.inverse());
  call.inputs = {{"source", Data::cloud(source)}, {"target", Data::cloud(target)}};
  REQUIRE_MESSAGE(call.run("register.icp_3d", {{"mode", Value::text("point_to_plane")},
                                               {"maxRmse", Value::number(1e-5)}})
                      .ok,
                  call.status.message);
  checkTransform(*call.outputs.at("transform").asTransform(), expected);
  target.normals.clear();
  call.inputs["target"] = Data::cloud(target);
  CHECK_FALSE(call.run("register.icp_3d", {{"mode", Value::text("point_to_plane")}}).ok);
  PointCloud plane;
  for (int i = 0; i < 10; ++i) {
    plane.push(float(i % 3), float(i / 3), 0);
    plane.normals.insert(plane.normals.end(), {0, 0, 1});
  }
  call.inputs = {{"source", Data::cloud(plane)}, {"target", Data::cloud(plane)}};
  CHECK_FALSE(call.run("register.icp_3d", {{"mode", Value::text("point_to_plane")}}).ok);
}

TEST_CASE("P1 single-profile analysis measures gap flush and rejects unsupported extrapolation") {
  PointCloud cloud;
  for (int i = 0; i < 8; ++i) {
    cloud.push(-0.02f + i * 0.001f, 0, 0);
    cloud.push(0.01f + i * 0.001f, 0, 0.003f);
  }
  test::OpCall call;
  call.inputs = {{"cloud", Data::cloud(cloud)}};
  REQUIRE_MESSAGE(call.run("profile.gap_flush").ok, call.status.message);
  CHECK(call.outputs.at("gap").asMeasurement()->value == doctest::Approx(0.023).epsilon(1e-6));
  CHECK(call.outputs.at("flush").asMeasurement()->value == doctest::Approx(0.003));
  CHECK_FALSE(call.run("profile.gap_flush", {{"maxExtrapolation", Value::number(0)}}).ok);
  CHECK_FALSE(call.run("profile.gap_flush", {{"rightRange", Value::vec({-0.015, 0.02})}}).ok);
  PointCloud line;
  for (int i = 0; i < 12; ++i)
    line.push(i * 0.001f, 0, i * 0.002f);
  call.inputs = {{"cloud", Data::cloud(line)}};
  REQUIRE(call.run("profile.analyze").ok);
  CHECK(data(call, "statistics")["count"] == 12);
  CHECK(data(call, "statistics")["rms"].get<double>() < 1e-9);
  PointCloud large;
  for (float x : {0.0f, 8.0f, 24.0f})
    large.push(x, 0, 1e8f + x);
  call.inputs = {{"cloud", Data::cloud(large)}};
  CHECK_FALSE(call.run("profile.analyze", {{"maxResidual", Value::number(0.001)}}).ok);
  REQUIRE(call.run("profile.analyze", {{"maxResidual", Value::number(3)}}).ok);
  const auto& nativeLine = *call.outputs.at("line").asLine2D();
  const Eigen::Vector2d anchor(nativeLine.point[0], nativeLine.point[1]);
  const Eigen::Vector2d direction(nativeLine.dir[0], nativeLine.dir[1]);
  double squared = 0;
  for (std::size_t i = 0; i < large.pointCount(); ++i) {
    const Eigen::Vector2d delta = Eigen::Vector2d(large.xyz[i * 3], large.xyz[i * 3 + 2]) - anchor;
    const double cross = delta.x() * direction.y() - delta.y() * direction.x();
    squared += cross * cross / direction.squaredNorm();
  }
  CHECK(data(call, "statistics")["rms"].get<double>() ==
        doctest::Approx(std::sqrt(squared / large.pointCount())).epsilon(1e-12));
  CHECK(data(call, "statistics")["rms"].get<double>() > 1);
  for (float x : {40.0f, 48.0f, 64.0f})
    large.push(x, 0, 1e8f + x);
  call.inputs = {{"cloud", Data::cloud(large)}};
  CHECK_FALSE(call.run("profile.gap_flush", {{"leftRange", Value::vec({0, 25})},
                                             {"rightRange", Value::vec({39, 65})},
                                             {"referenceX", Value::number(30)},
                                             {"maxExtrapolation", Value::number(30)}})
                  .ok);
}

TEST_CASE("P1 profile projector composes explicit extrinsics without mutating inputs") {
  const PointCloud input = fixture();
  Mat4 master = Mat4::Identity(), slave = Mat4::Identity(), common = Mat4::Identity();
  master(0, 3) = 1;
  slave(2, 3) = 2;
  common.topLeftCorner<3, 3>() = Eigen::AngleAxisd(0.5, Vec3::UnitY()).toRotationMatrix();
  test::OpCall call;
  call.inputs = {{"master", Data::cloud(input)},
                 {"slave", Data::cloud(input)},
                 {"masterToCommon", Data::transform(native(master))},
                 {"slaveToCommon", Data::transform(native(slave))},
                 {"commonToOutput", Data::transform(native(common))}};
  REQUIRE(call.run("profile.project_pair", {{"outputFrame", Value::text("measurement")}}).ok);
  const auto expected = moved(input, common * slave);
  const auto* actual = call.outputs.at("slave").asCloud();
  for (std::size_t i = 0; i < expected.xyz.size(); ++i)
    CHECK(actual->xyz[i] == doctest::Approx(expected.xyz[i]));
  CHECK(actual->rgb == input.rgb);
  CHECK(call.inputs.at("slave").asCloud()->xyz == input.xyz);
  call.inputs.erase("commonToOutput");
  CHECK_FALSE(call.run("profile.project_pair", {{"outputFrame", Value::text("measurement")}}).ok);
}

TEST_CASE("P1 file loader samples ASCII binary STL deterministically and validates PLY bounds") {
  Scratch scratch;
  const auto stl = scratch.path / std::filesystem::u8path(u8"三角形.stl");
  {
    std::ofstream out(stl);
    out << "solid triangle\nfacet normal 0 0 1\nouter loop\nvertex 0 0 0\nvertex 1000 0 0\nvertex 0 1000 0\nendloop\nendfacet\nendsolid triangle\n";
  }
  test::OpCall call;
  const ParamMap params{{"path", Value::text(stl.u8string())},
                        {"unit", Value::text("mm")},
                        {"samples", Value::integer(100)}};
  REQUIRE_MESSAGE(call.run("io.load_cloud", params).ok, call.status.message);
  const auto first = call.outputs.at("cloud");
  REQUIRE(call.run("io.load_cloud", params).ok);
  CHECK(first.asCloud()->xyz == call.outputs.at("cloud").asCloud()->xyz);
  CHECK(first.asCloud()->pointCount() == 100);
  for (std::size_t i = 0; i < 100; ++i) {
    CHECK(first.asCloud()->xyz[3 * i] >= 0);
    CHECK(first.asCloud()->xyz[3 * i + 1] >= 0);
    CHECK(first.asCloud()->xyz[3 * i] + first.asCloud()->xyz[3 * i + 1] <= 1.000001);
    CHECK(first.asCloud()->normals[3 * i + 2] == 1);
  }
  const auto binary = scratch.path / "binary.stl";
  {
    std::array<unsigned char, 134> bytes{};
    bytes[80] = 1;
    const float one = 1;
    std::memcpy(bytes.data() + 108, &one, sizeof(one));
    std::memcpy(bytes.data() + 124, &one, sizeof(one));
    std::ofstream out(binary, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }
  REQUIRE_MESSAGE(call.run("io.load_cloud", {{"path", Value::text(binary.u8string())},
                                             {"samples", Value::integer(100)}})
                      .ok,
                  call.status.message);
  CHECK(call.outputs.at("cloud").asCloud()->pointCount() == 100);
  CHECK(call.outputs.at("cloud").asCloud()->normals[2] == 1);
  for (const char* format : {"ascii", "binary", "binary_compressed"}) {
    const auto file = scratch.path / (std::string(format) + ".pcd");
    const auto expected = fixture();
    REQUIRE(lyflow::ops::saveCloudToFile(expected, file, format).ok);
    REQUIRE_MESSAGE(call.run("io.load_cloud", {{"path", Value::text(file.u8string())}}).ok,
                    call.status.message);
    const auto* actual = call.outputs.at("cloud").asCloud();
    CHECK(actual->xyz == expected.xyz);
    CHECK(actual->normals == expected.normals);
    CHECK(actual->rgb == expected.rgb);
    CHECK(actual->intensity == expected.intensity);
  }
  {
    const auto compressed = scratch.path / "binary_compressed.pcd";
    std::ifstream input(compressed, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(input)), {});
    const auto body = bytes.find('\n', bytes.find("DATA binary_compressed")) + 1;
    REQUIRE(body > 0);
    REQUIRE(body + 8 <= bytes.size());
    std::fill(bytes.begin() + body + 4, bytes.begin() + body + 8, '\xff');
    input.close();
    std::ofstream output(compressed, std::ios::binary);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    CHECK_FALSE(call.run("io.load_cloud", {{"path", Value::text(compressed.u8string())}}).ok);
  }
  const auto typed = scratch.path / "typed.pcd";
  for (const auto& specification :
       std::vector<std::tuple<int, char, std::string, bool>>{{1, 'U', "255", true},
                                                             {1, 'U', "256", false},
                                                             {1, 'U', "-1", false},
                                                             {1, 'U', "1.5", false},
                                                             {1, 'I', "-128", true},
                                                             {1, 'I', "-129", false},
                                                             {1, 'I', "128", false},
                                                             {2, 'U', "65535", true},
                                                             {2, 'U', "65536", false},
                                                             {2, 'I', "32768", false},
                                                             {4, 'U', "4294967296", false},
                                                             {4, 'I', "2147483648", false},
                                                             {4, 'F', "1e39", false}}) {
    const auto [size, type, value, accepted] = specification;
    std::ofstream out(typed);
    out << "VERSION 0.7\nFIELDS x y z intensity\nSIZE 4 4 4 " << size << "\nTYPE F F F " << type
        << "\nCOUNT 1 1 1 1\nWIDTH 1\nHEIGHT 1\nPOINTS 1\nDATA ascii\n1 2 3 " << value << '\n';
    out.close();
    const auto status = call.run("io.load_cloud", {{"path", Value::text(typed.u8string())}});
    CHECK_MESSAGE(status.ok == accepted, "typed PCD size=", size, " type=", type, " value=", value,
                  " diagnostic=", status.message);
  }
  {
    std::ofstream out(typed);
    out << "VERSION 0.7\nFIELDS x y z\nSIZE 4 4 4\nTYPE F F F\nCOUNT 1 1 1\nWIDTH 2\nHEIGHT 1\nPOINTS 1\nDATA ascii\n1 2 3\n";
  }
  CHECK_FALSE(call.run("io.load_cloud", {{"path", Value::text(typed.u8string())}}).ok);
  const auto ply = scratch.path / "cloud.ply";
  {
    std::ofstream out(ply);
    out << "ply\nformat ascii 1.0\nelement vertex 2\nproperty float x\nproperty float y\nproperty float z\nend_header\n1 2 3\n4 5 6\n";
  }
  REQUIRE_MESSAGE(call.run("io.load_cloud", {{"path", Value::text(ply.u8string())}}).ok,
                  call.status.message);
  CHECK((call.outputs.at("cloud").asCloud()->xyz == std::vector<float>{1, 2, 3, 4, 5, 6}));
  CHECK_FALSE(call.run("io.load_cloud",
                       {{"path", Value::text(ply.u8string())}, {"maxPoints", Value::integer(1)}})
                  .ok);
  {
    std::ofstream out(ply);
    out << "ply\nformat ascii 1.0\nelement vertex 1\nproperty float x\nproperty float y\nproperty float z\nproperty float nx\nproperty float ny\nproperty float nz\nproperty uchar red\nproperty uchar green\nproperty uchar blue\nend_header\n1 2 3 0 0 1 4 5 6\n";
  }
  REQUIRE_MESSAGE(call.run("io.load_cloud", {{"path", Value::text(ply.u8string())}}).ok,
                  call.status.message);
  CHECK((call.outputs.at("cloud").asCloud()->rgb == std::vector<std::uint8_t>{4, 5, 6}));
  CHECK((call.outputs.at("cloud").asCloud()->normals == std::vector<float>{0, 0, 1}));
  for (const char* value : {"256", "-1", "1.5"}) {
    std::ofstream out(ply);
    out << "ply\nformat ascii 1.0\nelement vertex 1\nproperty float x\nproperty float y\nproperty float z\nproperty uchar red\nproperty uchar green\nproperty uchar blue\nend_header\n1 2 3 "
        << value << " 5 6\n";
    out.close();
    CHECK_FALSE(call.run("io.load_cloud", {{"path", Value::text(ply.u8string())}}).ok);
  }
  {
    std::ofstream out(ply);
    out << "ply\nformat ascii 1.0\nelement vertex 2\nproperty float x\nproperty float y\nproperty float z\nend_header\n1 2 3\n";
  }
  CHECK_FALSE(call.run("io.load_cloud", {{"path", Value::text(ply.u8string())}}).ok);
  {
    std::ofstream out(ply);
    out << "ply\nformat ascii 1.0\nelement vertex 1000000000\nproperty float x\nproperty float y\nproperty float z\nend_header\n";
  }
  CHECK_FALSE(call.run("io.load_cloud", {{"path", Value::text(ply.u8string())}}).ok);
  {
    std::ofstream out(stl);
    out << "solid broken\nfacet normal 0 0 1\n";
  }
  CHECK_FALSE(call.run("io.load_cloud", params).ok);
}

TEST_CASE("P1 cloud example executes clustering and source-to-target fit through the C ABI") {
  std::ifstream file(std::filesystem::path(LYFLOW_EXAMPLES_DIR) / "p1-cloud.lyflow.json");
  REQUIRE(file.good());
  const auto graph = nlohmann::json::parse(file).dump();
  const auto base = std::filesystem::path(LYFLOW_EXAMPLES_DIR).u8string();
  lyflow_run_options options{};
  options.run_id = "p1-cloud-example-c-abi";
  options.base_dir = base.c_str();
  auto* run = lyflow_run_start(graph.c_str(), &options, nullptr, nullptr);
  REQUIRE(run != nullptr);
  lyflow_run_join(run);
  char* raw = lyflow_run_outputs(options.run_id);
  REQUIRE(raw != nullptr);
  const auto outputs = nlohmann::json::parse(raw);
  lyflow_string_free(raw);
  lyflow_run_free(run);
  REQUIRE_MESSAGE(outputs.contains("quality"), outputs.dump());
  const auto& quality = outputs["quality"]["value"]["data"];
  CHECK(quality["accepted"] == true);
  CHECK(quality["direction"] == "source_to_target");
  CHECK(quality["inlierRatio"] == 1);
  CHECK(quality["rmse"].get<double>() < 1e-5);
  CHECK(outputs["transform"]["value"]["m"][3].get<double>() == doctest::Approx(0.01));
  CHECK(outputs["transform"]["value"]["m"][7].get<double>() == doctest::Approx(-0.02));
  CHECK(outputs["transform"]["value"]["m"][11].get<double>() == doctest::Approx(0.03));
  const auto& groups = outputs["clusters"]["value"]["data"]["groups"];
  REQUIRE(groups.size() == 2);
  CHECK(groups[0]["count"] == 4);
  CHECK(groups[1]["count"] == 4);
  CHECK(groups[1]["center"][2].get<double>() - groups[0]["center"][2].get<double>() ==
        doctest::Approx(1));
}
