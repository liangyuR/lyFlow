#include <doctest/doctest.h>

#include <cmath>
#include <set>

#include "helpers.h"
#include "lyflow/cloud_groups.h"
#include "lyflow/pose.h"

namespace lyflow::ops {
void registerP5CloudInspection(Registry&);
}
namespace {
using namespace lyflow;
Registry boxRegistry() {
  Registry r;
  ops::registerP5CloudInspection(r);
  return r;
}
Record boxPoses() {
  return poses::make3DList(
      nlohmann::json::array(
          {poses::make3D("a", "input", {.2, .3, 1}, {0, 0, std::sqrt(.5), std::sqrt(.5)},
                         {.08, .04, .02})
               .data,
           poses::make3D("b", "input", {-.1, 0, 1}, {0, 0, 0, 1}, {.03, .05, .07}).data}),
      "input");
}
class BoxCancelled : public ExecContext {
public:
  explicit BoxCancelled(int limit = 1) : limit(limit) {}
  bool cancelled() const override { return ++calls >= limit; }
  void progress(float, std::string_view) override {}
  void log(LogLevel, std::string) override {}
  const std::filesystem::path& baseDir() const override { return base; }
  int threadBudget() const override { return 1; }
  std::filesystem::path base;
  int limit;
  mutable int calls = 0;
};
}
TEST_CASE(
    "P5 box surface and wire samples preserve transformed finite geometry and stable groups") {
  auto registry = boxRegistry();
  test::OpCall call(registry);
  const auto input = boxPoses();
  call.inputs["boxes"] = Data::record(input);
  REQUIRE(call.run("cloud.box_sample",
                   {{"pointsPerFace", Value::integer(100)}, {"wireSpacing", Value::number(.01)}})
              .ok);
  const auto* cloud = call.outputs.at("cloud").asCloud();
  REQUIRE(cloud);
  CHECK(cloud->pointCount() == 1200);
  CHECK(cloud->channelsConsistent());
  const auto xyz = cloud->xyz, normals = cloud->normals;
  const auto id = cloud->id;
  const auto* wire = call.outputs.at("wireframe").asCloud();
  REQUIRE(wire);
  CHECK(wire->pointCount() > 0);
  CHECK(wire->id != id);
  std::vector<cloud_groups::Group> groups;
  std::string frame, why;
  REQUIRE(cloud_groups::parse(*call.outputs.at("groups").asRecord(), *cloud,
                              *call.outputs.at("indices").asIndices(), groups, frame, why));
  REQUIRE(groups.size() == 2);
  CHECK(groups[0].id == "a");
  CHECK(groups[1].id == "b");
  CHECK(frame == "input");
  for (const auto& group : groups) {
    const auto& pose = input.data["items"][group.id == "a" ? 0 : 1];
    const auto m = poses::rotationMatrix(pose["rotation"].get<poses::Quaternion>());
    for (std::size_t i = group.offset; i < group.offset + group.count; ++i) {
      std::array<double, 3> local{};
      double normalLength = 0;
      for (int axis = 0; axis < 3; ++axis) {
        for (int k = 0; k < 3; ++k)
          local[axis] +=
              m[k * 3 + axis] * (cloud->xyz[3 * i + k] - pose["position"][k].get<double>());
        normalLength += cloud->normals[3 * i + axis] * cloud->normals[3 * i + axis];
      }
      bool surface = false;
      for (int axis = 0; axis < 3; ++axis) {
        const double half = pose["size"][axis].get<double>() / 2;
        CHECK(std::abs(local[axis]) <= half + 1e-7);
        surface |= std::abs(std::abs(local[axis]) - half) < 1e-7;
      }
      CHECK(surface);
      CHECK(std::abs(normalLength - 1) < 1e-6);
    }
  }
  REQUIRE(call.run("cloud.box_sample",
                   {{"pointsPerFace", Value::integer(100)}, {"wireSpacing", Value::number(.01)}})
              .ok);
  CHECK(call.outputs.at("cloud").asCloud()->xyz == xyz);
  CHECK(call.outputs.at("cloud").asCloud()->normals == normals);
  CHECK(call.outputs.at("cloud").asCloud()->id != id);
  CHECK(call.inputs.at("boxes").asRecord()->data == input.data);
}
TEST_CASE("P5 box local volume and camera-facing surface modes have explicit physical semantics") {
  auto registry = boxRegistry();
  test::OpCall call(registry);
  call.inputs["boxes"] = Data::record(poses::make3DList(
      nlohmann::json::array(
          {poses::make3D("one", "input", {0, 0, 1}, {0, 0, 0, 1}, {.1, .1, .1}).data}),
      "input"));
  REQUIRE(call.run("cloud.box_sample",
                   {{"pointsPerFace", Value::integer(20)}, {"visibleOnly", Value::boolean(true)}})
              .ok);
  const auto* cloud = call.outputs.at("cloud").asCloud();
  REQUIRE(cloud);
  CHECK(cloud->pointCount() == 20);
  for (std::size_t i = 0; i < cloud->pointCount(); ++i) {
    CHECK(std::abs(cloud->xyz[3 * i + 2] - .95) < 1e-7);
    CHECK(cloud->normals[3 * i + 2] == -1);
  }
  REQUIRE(call.run("cloud.box_sample", {{"pointsPerFace", Value::integer(20)},
                                        {"sampling", Value::text("volume")},
                                        {"normals", Value::boolean(false)},
                                        {"coordinates", Value::text("local")}})
              .ok);
  cloud = call.outputs.at("cloud").asCloud();
  REQUIRE(cloud);
  CHECK(cloud->pointCount() == 120);
  CHECK(cloud->normals.empty());
  for (float v : cloud->xyz)
    CHECK(std::abs(v) < .05);
  CHECK(call.outputs.at("groups").asRecord()->data["frame"] == "box_local");
  CHECK(call.outputs.at("quality").asRecord()->data["measured"] == false);
  CHECK_FALSE(call.run("cloud.box_sample", {{"sampling", Value::text("volume")}}).ok);
  CHECK(call.outputs.empty());
}
TEST_CASE("P5 box budgets and cancellation do not publish partial clouds") {
  auto registry = boxRegistry();
  test::OpCall call(registry);
  call.inputs["boxes"] = Data::record(boxPoses());
  CHECK_FALSE(call.run("cloud.box_sample", {{"wireSpacing", Value::number(1e-12)}}).ok);
  CHECK(call.outputs.empty());
  CHECK_FALSE(call.run("cloud.box_sample", {{"color", Value::vec({0, 256, 0})}}).ok);
  CHECK(call.outputs.empty());
  auto* op = registry.find("cloud.box_sample");
  REQUIRE(op);
  std::unordered_map<std::string, Value> params;
  for (const auto& item : op->params)
    params[item.name] = item.def;
  BoxCancelled ctx;
  Inputs inputs(call.inputs);
  Outputs outputs(call.outputs);
  ParamView view(params, ctx.base);
  CHECK(op->compute(inputs, view, outputs, ctx).code == "cancelled");
  CHECK(call.outputs.empty());
  BoxCancelled mid(3);
  CHECK(op->compute(inputs, view, outputs, mid).code == "cancelled");
  CHECK(mid.calls >= 3);
  CHECK(call.outputs.empty());
  call.inputs["boxes"] = Data::record(poses::make3DList(nlohmann::json::array(), "input"));
  REQUIRE(call.run("cloud.box_sample").ok);
  CHECK(call.outputs.at("cloud").asCloud()->pointCount() == 0);
}
TEST_CASE(
    "P5 box float32 precision loss is diagnosed without rejecting legal nullable attributes") {
  auto registry = boxRegistry();
  test::OpCall call(registry);
  auto pose = poses::make3D("precision", "input", {1e8, 1e8, 1e8}, {0, 0, 0, 1}, {.1, .1, .1});
  call.inputs["boxes"] =
      Data::record(poses::make3DList(nlohmann::json::array({pose.data}), "input"));
  CHECK(call.run("cloud.box_sample",
                 {{"pointsPerFace", Value::integer(10)}, {"wireSpacing", Value::number(.02)}})
            .code == "precision_loss");
  CHECK(call.outputs.empty());
  REQUIRE(call.run("cloud.box_sample", {{"pointsPerFace", Value::integer(10)},
                                        {"wireSpacing", Value::number(.02)},
                                        {"coordinates", Value::text("local")}})
              .ok);
  const auto bounds = call.outputs.at("cloud").asCloud()->bounds();
  CHECK(bounds.valid);
  for (int axis = 0; axis < 3; ++axis)
    CHECK(bounds.max[axis] - bounds.min[axis] > .09);
  pose.data["position"] = {0, 0, 0};
  pose.data["attributes"] = {{"nullable", nullptr}};
  call.inputs["boxes"] =
      Data::record(poses::make3DList(nlohmann::json::array({pose.data}), "input"));
  REQUIRE_MESSAGE(call.run("cloud.box_sample", {{"pointsPerFace", Value::integer(10)},
                                                {"wireSpacing", Value::number(.02)}})
                      .ok,
                  call.status.message);
  CHECK(call.outputs.at("quality")
            .asRecord()
            ->data["items"][0]["sourcePose"]["attributes"]["nullable"]
            .is_null());
}
