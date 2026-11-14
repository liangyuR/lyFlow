#include <doctest/doctest.h>

#include <Eigen/Geometry>
#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <set>

#include "helpers.h"
#include "lyflow/cloud_groups.h"
#include "lyflow/pose.h"
#include "../ops/p1_common.h"

namespace {
using namespace lyflow;
using namespace lyflow::ops::p1;
using Json = nlohmann::json;

const Json& record(const test::OpCall& call, const char* port) {
  const auto* value = call.outputs.at(port).asRecord();
  REQUIRE(value != nullptr);
  return value->data;
}

PointCloud boxFixture() {
  PointCloud cloud;
  for (float x : {-0.03f, 0.05f})
    for (float y : {0.01f, 0.04f})
      for (float z : {0.02f, 0.08f}) {
        cloud.push(x, y, z);
        cloud.intensity.push_back(static_cast<float>(cloud.pointCount()));
        cloud.rgb.insert(cloud.rgb.end(), {1, 2, 3});
        cloud.normals.insert(cloud.normals.end(), {0, 0, 1});
      }
  return cloud;
}

PointCloud surfaceFixture() {
  struct Box {
    Vec3 lo, hi;
  };
  const std::vector<Box> boxes{{{-.04, -.02, -.009}, {.02, .002, .009}},
                               {{.01, -.004, -.009}, {.035, .022, .009}},
                               {{-.0015, -.006, .009}, {.0135, .006, .027}}};
  std::map<std::array<std::int64_t, 3>, Vec3> points;
  for (std::size_t box = 0; box < boxes.size(); ++box)
    for (int axis = 0; axis < 3; ++axis)
      for (bool high : {false, true}) {
        const int u = (axis + 1) % 3, v = (axis + 2) % 3;
        const int nu =
            std::max(2, static_cast<int>(std::ceil((boxes[box].hi[u] - boxes[box].lo[u]) / .0018)));
        const int nv =
            std::max(2, static_cast<int>(std::ceil((boxes[box].hi[v] - boxes[box].lo[v]) / .0018)));
        for (int i = 0; i <= nu; ++i)
          for (int j = 0; j <= nv; ++j) {
            Vec3 q;
            q[axis] = high ? boxes[box].hi[axis] : boxes[box].lo[axis];
            q[u] = boxes[box].lo[u] + (boxes[box].hi[u] - boxes[box].lo[u]) * i / nu;
            q[v] = boxes[box].lo[v] + (boxes[box].hi[v] - boxes[box].lo[v]) * j / nv;
            bool inside = false;
            for (std::size_t other = 0; other < boxes.size(); ++other)
              if (other != box && (q.array() > boxes[other].lo.array() + 1e-10).all() &&
                  (q.array() < boxes[other].hi.array() - 1e-10).all())
                inside = true;
            if (!inside)
              points[{std::llround(q.x() * 1e9), std::llround(q.y() * 1e9),
                      std::llround(q.z() * 1e9)}] = q;
          }
      }
  PointCloud source;
  for (const auto& item : points)
    source.push(static_cast<float>(item.second.x()), static_cast<float>(item.second.y()),
                static_cast<float>(item.second.z()));
  return source;
}

Mat4 truth(const Vec3& t = Vec3(.17, -.09, .43)) {
  Mat4 pose = Mat4::Identity();
  pose.topLeftCorner<3, 3>() =
      (Eigen::AngleAxisd(1.1, Vec3::UnitZ()) * Eigen::AngleAxisd(-.6, Vec3::UnitY()) *
       Eigen::AngleAxisd(.3, Vec3::UnitX()))
          .toRotationMatrix();
  pose.topRightCorner<3, 1>() = t;
  return pose;
}

PointCloud movedCloud(const PointCloud& cloud, const Mat4& pose) {
  PointCloud output;
  test::StubContext ctx;
  REQUIRE(transformed(cloud, pose, output, ctx));
  return output;
}

void checkGroup(const Record& groups, const PointCloud& source, const Indices& indices,
                std::size_t count) {
  std::vector<cloud_groups::Group> parsed;
  std::string frame, why;
  REQUIRE_MESSAGE(cloud_groups::parse(groups, source, indices, parsed, frame, why), why);
  CHECK(parsed.size() == count);
}

class CancelledContext : public ExecContext {
public:
  bool cancelled() const override { return true; }
  void progress(float, std::string_view) override {}
  void log(LogLevel, std::string) override {}
  const std::filesystem::path& baseDir() const override {
    static std::filesystem::path base;
    return base;
  }
  int threadBudget() const override { return 1; }
};
} // namespace

TEST_CASE(
    "P2 matching template centers a volumetric model with preserved channels and rejects planar anchors") {
  const auto source = boxFixture();
  test::OpCall call;
  call.inputs = {{"cloud", Data::cloud(source)}};
  REQUIRE(call.run("cloud.template_3d", {{"sampleDistance", Value::number(0)}}).ok);
  const auto& model = *call.outputs.at("model").asCloud();
  CHECK(model.id != source.id);
  CHECK(model.pointCount() == source.pointCount());
  CHECK(model.rgb == source.rgb);
  CHECK(model.intensity.size() == source.intensity.size());
  CHECK(model.normals == source.normals);
  const auto& info = record(call, "template");
  CHECK(info["modelCloudId"] == std::to_string(model.id));
  CHECK(info["unit"] == "m");
  const auto bounds = model.bounds();
  for (int axis = 0; axis < 3; ++axis)
    CHECK(std::abs(bounds.min[axis] + bounds.max[axis]) < 1e-7);
  CHECK(source.xyz == boxFixture().xyz);
  PointCloud plane;
  plane.push(0, 0, 0);
  plane.push(1, 0, 0);
  plane.push(0, 1, 0);
  plane.push(1, 1, 0);
  call.inputs["cloud"] = Data::cloud(plane);
  CHECK(call.run("cloud.template_3d").code == "degenerate_geometry");
  CHECK(call.outputs.empty());
}

TEST_CASE(
    "P2 matching organized edges separate nearSide farSide holes and curvature without guessing layout") {
  PointCloud source;
  for (int y = 0; y < 5; ++y)
    for (int x = 0; x < 6; ++x) {
      source.push(x * .01f, y * .01f, x < 3 ? 1.0f : 1.1f);
      source.intensity.push_back(static_cast<float>(source.pointCount()));
      source.normals.insert(source.normals.end(), x < 3 ? std::initializer_list<float>{0, 0, 1}
                                                        : std::initializer_list<float>{0, 1, 0});
    }
  Indices valid{source.id, {}};
  for (std::int32_t i = 0; i < 30; ++i)
    if (i != 7)
      valid.values.push_back(i);
  test::OpCall call;
  call.inputs = {{"cloud", Data::cloud(source)}, {"validIndices", Data::indices(valid)}};
  const ParamMap params{
      {"mode", Value::text("organized")},      {"organizedWidth", Value::integer(6)},
      {"occluding", Value::boolean(true)},     {"occluded", Value::boolean(true)},
      {"highCurvature", Value::boolean(true)}, {"searchNeighbors", Value::integer(1)}};
  REQUIRE(call.run("cloud.edges_3d", params).ok);
  const auto& nearSide = call.outputs.at("occludingIndices").asIndices()->values;
  const auto& farSide = call.outputs.at("occludedIndices").asIndices()->values;
  CHECK(std::find(nearSide.begin(), nearSide.end(), 14) != nearSide.end());
  CHECK(std::find(farSide.begin(), farSide.end(), 15) != farSide.end());
  CHECK(!call.outputs.at("curvatureIndices").asIndices()->values.empty());
  const auto& boundary = call.outputs.at("boundaryIndices").asIndices()->values;
  CHECK(std::find(boundary.begin(), boundary.end(), 8) != boundary.end());
  const auto& indices = *call.outputs.at("indices").asIndices();
  CHECK(indices.sourceCloudId == source.id);
  CHECK(source.select(indices.values).intensity == call.outputs.at("cloud").asCloud()->intensity);
  std::vector<std::int32_t> every(source.pointCount());
  std::iota(every.begin(), every.end(), 0);
  auto hole = source.select(every);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  hole.xyz[7 * 3] = nan;
  hole.intensity[7] = nan;
  hole.normals[7 * 3] = nan;
  Indices holeValid{hole.id, valid.values};
  call.inputs = {{"cloud", Data::cloud(hole)}, {"validIndices", Data::indices(holeValid)}};
  REQUIRE(call.run("cloud.edges_3d", params).ok);
  for (float value : call.outputs.at("cloud").asCloud()->xyz)
    CHECK(std::isfinite(value));
  for (float value : call.outputs.at("cloud").asCloud()->normals)
    CHECK(std::isfinite(value));
  for (float value : call.outputs.at("cloud").asCloud()->intensity)
    CHECK(std::isfinite(value));
  CHECK(record(call, "quality")["validCount"] == 29);
  call.inputs.erase("validIndices");
  REQUIRE(call.run("cloud.edges_3d", params).ok);
  CHECK(record(call, "quality")["validCount"] == 29);
  holeValid.values.push_back(7);
  call.inputs["validIndices"] = Data::indices(holeValid);
  CHECK(call.run("cloud.edges_3d", params).code == "bad_input");
  CHECK(call.outputs.empty());
  call.inputs = {{"cloud", Data::cloud(hole)}};
  CHECK(call.run("cloud.edges_3d").code == "bad_input");
  CHECK(call.outputs.empty());
  call.inputs = {{"cloud", Data::cloud(source)}, {"validIndices", Data::indices(valid)}};
  auto bad = params;
  bad["organizedWidth"] = Value::integer(7);
  CHECK_FALSE(call.run("cloud.edges_3d", bad).ok);
  CHECK(call.outputs.empty());
  valid.sourceCloudId++;
  call.inputs["validIndices"] = Data::indices(valid);
  CHECK_FALSE(call.run("cloud.edges_3d", params).ok);
}

TEST_CASE(
    "P2 matching unorganized tangent edges keep the boundary and reject a full interior neighborhood") {
  PointCloud plane;
  for (int y = -5; y <= 5; ++y)
    for (int x = -5; x <= 5; ++x)
      plane.push(x * .005f, y * .005f, 1);
  test::OpCall call;
  call.inputs = {{"cloud", Data::cloud(plane)}};
  REQUIRE(call.run("cloud.edges_3d",
                   {{"neighborRadius", Value::number(.012)}, {"kSearch", Value::integer(24)}})
              .ok);
  const auto& edges = call.outputs.at("indices").asIndices()->values;
  CHECK(std::find(edges.begin(), edges.end(), 60) == edges.end());
  CHECK(std::find(edges.begin(), edges.end(), 5) != edges.end());
  CHECK(record(call, "quality")["mode"] == "unorganized");
  CHECK_FALSE(call.run("cloud.edges_3d", {{"occluding", Value::boolean(true)}}).ok);
  CHECK(call.outputs.empty());
}

TEST_CASE(
    "P2 matching view sampling performs depth occlusion and maintains both source and concatenated identities") {
  PointCloud source;
  source.push(0, 0, 1);
  source.push(0, 0, 2);
  source.push(.1f, 0, 1);
  source.push(.2f, 0, -1);
  source.intensity = {10, 20, 30, 40};
  const auto a = poses::make3D("a", "camera", {0, 0, 0}, {0, 0, 0, 1}, {1, 1, 1});
  const auto b = poses::make3D("b", "camera", {0, 0, 0}, {0, 1, 0, 0}, {1, 1, 1});
  auto list = poses::make3DList(Json::array({a.data, b.data}), "camera");
  test::OpCall call;
  call.inputs = {{"model", Data::cloud(source)}, {"poses", Data::record(list)}};
  REQUIRE(call.run("cloud.view_sample", {{"depthTolerance", Value::number(0)}}).ok);
  const auto& indices = *call.outputs.at("indices").asIndices();
  CHECK(indices.values == std::vector<std::int32_t>{0, 2, 3});
  checkGroup(*call.outputs.at("groups").asRecord(), source, indices, 2);
  const auto& combined = *call.outputs.at("cloud").asCloud();
  CHECK(combined.intensity == std::vector<float>{10, 30, 40});
  checkGroup(*call.outputs.at("cloudGroups").asRecord(), combined,
             *call.outputs.at("cloudIndices").asIndices(), 2);
  REQUIRE(call.run("cloud.view_sample", {{"transformByPose", Value::boolean(true)},
                                         {"depthTolerance", Value::number(0)}})
              .ok);
  const auto& moved = *call.outputs.at("cloud").asCloud();
  CHECK(moved.xyz[8] > 0);
  CHECK(source.xyz[11] == -1);
  list.data["frame"] = "world";
  call.inputs["poses"] = Data::record(list);
  CHECK_FALSE(call.run("cloud.view_sample").ok);
  CHECK(call.outputs.empty());
}

TEST_CASE(
    "P2 matching global SE3 recovers two physical instances and explicit empty ROI never falls back") {
  test::OpCall modelCall;
  modelCall.inputs = {{"cloud", Data::cloud(surfaceFixture())}};
  REQUIRE(modelCall.run("cloud.template_3d", {{"sampleDistance", Value::number(0)}}).ok);
  const auto modelData = modelCall.outputs.at("model");
  const auto& model = *modelData.asCloud();
  auto scene = movedCloud(model, truth());
  const auto second = movedCloud(model, truth(Vec3(.30, -.08, .43)));
  scene.xyz.insert(scene.xyz.end(), second.xyz.begin(), second.xyz.end());
  test::OpCall call;
  call.inputs = {{"model", modelData},
                 {"template", modelCall.outputs.at("template")},
                 {"scene", Data::cloud(scene)}};
  const ParamMap params{
      {"maxRmse", Value::number(.0005)},          {"normalRadiusFactor", Value::number(6)},
      {"featureRadiusFactor", Value::number(10)}, {"maxCorrespondences", Value::integer(1024)},
      {"maxInstances", Value::integer(4)},        {"maxPasses", Value::integer(8)}};
  REQUIRE_MESSAGE(call.run("register.global_3d", params).ok, call.status.message);
  const auto& instances = record(call, "instances")["items"];
  REQUIRE(instances.size() == 2);
  for (std::size_t i = 0; i < 2; ++i) {
    const auto expected = i == 0 ? truth() : truth(Vec3(.30, -.08, .43));
    const auto position = instances[i]["position"].get<std::array<double, 3>>();
    CHECK((Vec3(position[0], position[1], position[2]) - expected.topRightCorner<3, 1>()).norm() <
          .0002);
    const auto rotation = poses::rotationMatrix(instances[i]["rotation"].get<poses::Quaternion>());
    Mat3 actual;
    for (int row = 0; row < 3; ++row)
      for (int col = 0; col < 3; ++col)
        actual(row, col) = rotation[row * 3 + col];
    CHECK(Eigen::AngleAxisd(actual.transpose() * expected.topLeftCorner<3, 3>()).angle() < .003);
    CHECK(instances[i]["pickable"] == false);
  }
  checkGroup(*call.outputs.at("groups").asRecord(), scene, *call.outputs.at("indices").asIndices(),
             2);
  CHECK(record(call, "quality")["deterministic"] == false);
  Mat4 badPose = truth(Vec3(.10, -.08, .43));
  badPose.topLeftCorner<3, 3>() =
      truth().topLeftCorner<3, 3>() *
      Eigen::AngleAxisd(poses::kPi / 2, Vec3::UnitX()).toRotationMatrix();
  const Mat4 goodPose = truth(Vec3(.30, -.08, .43));
  auto directionalScene = movedCloud(model, badPose);
  const auto directionalGood = movedCloud(model, goodPose);
  directionalScene.xyz.insert(directionalScene.xyz.end(), directionalGood.xyz.begin(),
                              directionalGood.xyz.end());
  call.inputs["scene"] = Data::cloud(directionalScene);
  for (const Mat4& expected : {badPose, goodPose}) {
    auto directional = params;
    directional["directionAxis"] = Value::text("z");
    directional["directionTolerance"] = Value::number(.05);
    directional["targetDirection"] = Value::vec({expected(0, 2), expected(1, 2), expected(2, 2)});
    REQUIRE_MESSAGE(call.run("register.global_3d", directional).ok, call.status.message);
    const auto& allowed = record(call, "instances")["items"];
    REQUIRE(allowed.size() == 1);
    const auto position = allowed[0]["position"].get<std::array<double, 3>>();
    CHECK((Vec3(position[0], position[1], position[2]) - expected.topRightCorner<3, 1>()).norm() <
          .0002);
    CHECK(record(call, "quality").value("directionRejectedCandidates", 0) >= 1);
  }
  call.inputs["scene"] = Data::cloud(scene);
  Indices empty{scene.id, {}};
  call.inputs["sceneIndices"] = Data::indices(empty);
  REQUIRE(call.run("register.global_3d", params).ok);
  CHECK(record(call, "instances")["items"].empty());
  CHECK(record(call, "quality")["attempts"] == 0);
  const auto validTemplate = *modelCall.outputs.at("template").asRecord();
  std::vector<Record> malformed;
  for (const char* field : {"sourceFrame", "modelFrame", "sourceCenter", "sourceToModel",
                            "sampleDistance", "maxModelPoints", "method"}) {
    auto bad = validTemplate;
    bad.data.erase(field);
    malformed.push_back(std::move(bad));
  }
  auto changed = [&](const char* key, Json value) {
    auto bad = validTemplate;
    bad.data[key] = std::move(value);
    malformed.push_back(std::move(bad));
  };
  changed("sourceFrame", 7);
  changed("modelFrame", "");
  changed("sourceCenter", Json::array({0, 0}));
  changed("sourceCenter", Json::array({0, std::numeric_limits<double>::quiet_NaN(), 0}));
  changed("sourceToModel", Json::array({1, 0}));
  changed("sampleDistance", -1);
  changed("sampleDistance", "1mm");
  changed("maxModelPoints", 3);
  changed("maxModelPoints", 8193);
  changed("maxModelPoints", 4096.5);
  changed("method", "unrecognized-method");
  changed("diameter", validTemplate.data["diameter"].get<double>() * 2);
  changed("unexpected", true);
  auto nonRigid = validTemplate;
  nonRigid.data["sourceToModel"][0] = 2;
  malformed.push_back(nonRigid);
  auto nonCanonical = validTemplate;
  nonCanonical.data["sourceToModel"][1] = static_cast<double>(1e-13f);
  malformed.push_back(nonCanonical);
  nonCanonical = validTemplate;
  nonCanonical.data["sourceToModel"][12] = static_cast<double>(1e-8f);
  malformed.push_back(nonCanonical);
  auto mismatchedCenter = validTemplate;
  mismatchedCenter.data["sourceCenter"][0] = 1;
  malformed.push_back(mismatchedCenter);
  for (const auto& bad : malformed) {
    call.inputs["template"] = Data::record(bad);
    CHECK(call.run("register.global_3d", params).code == "bad_input");
    CHECK(call.status.portName == "template");
    CHECK(call.outputs.empty());
  }
  call.inputs["template"] = Data::record(validTemplate);
  empty.sourceCloudId++;
  call.inputs["sceneIndices"] = Data::indices(empty);
  CHECK_FALSE(call.run("register.global_3d", params).ok);
  CHECK(call.outputs.empty());
}

TEST_CASE(
    "P2 matching stage boundaries cancel atomically and budgets do not masquerade as empty results") {
  const auto source = boxFixture();
  const auto* op = ensureRegistry().find("cloud.template_3d");
  REQUIRE(op != nullptr);
  ParamMap params;
  for (const auto& parameter : op->params)
    params[parameter.name] = parameter.def;
  std::unordered_map<std::string, Data> input{{"cloud", Data::cloud(source)}}, output;
  const std::filesystem::path base;
  Inputs inputs(input);
  Outputs outputs(output);
  ParamView view(params, base);
  CancelledContext ctx;
  CHECK(op->compute(inputs, view, outputs, ctx).code == "cancelled");
  CHECK(output.empty());
  PointCloud excessive;
  excessive.xyz.resize(131073 * 3, 0);
  test::OpCall call;
  call.inputs = {{"cloud", Data::cloud(excessive)}};
  CHECK(call.run("cloud.template_3d").code == "resource_limit");
  CHECK(call.outputs.empty());
  auto views = poses::make3DList(Json::array(), "camera");
  for (int i = 0; i < 129; ++i)
    views.data["items"].push_back(
        poses::make3D(std::to_string(i), "camera", {0, 0, 1}, {0, 0, 0, 1}, {1, 1, 1}).data);
  call.inputs = {{"model", Data::cloud(source)}, {"poses", Data::record(views)}};
  CHECK(call.run("cloud.view_sample").code == "resource_limit");
  CHECK(call.outputs.empty());
  PointCloud dense;
  for (int z = 0; z < 16; ++z)
    for (int y = 0; y < 16; ++y)
      for (int x = 0; x < 16; ++x)
        dense.push(x * .0011f, y * .0011f, z * .0011f);
  call.inputs = {{"cloud", Data::cloud(dense)}};
  REQUIRE(call.run("cloud.template_3d",
                   {{"sampleDistance", Value::number(0)}, {"maxModelPoints", Value::integer(4096)}})
              .ok);
  const auto model = call.outputs.at("model");
  const auto description = call.outputs.at("template");
  REQUIRE(model.asCloud()->pointCount() == 4096);
  call.inputs = {{"model", model}, {"template", description}, {"scene", Data::cloud(dense)}};
  CHECK(call.run("register.global_3d", {{"voxelSize", Value::number(.001)},
                                        {"normalRadiusFactor", Value::number(10)},
                                        {"featureRadiusFactor", Value::number(20)}})
            .code == "resource_limit");
  CHECK(call.status.message.find("16Mi") != std::string::npos);
  CHECK(call.outputs.empty());
}
