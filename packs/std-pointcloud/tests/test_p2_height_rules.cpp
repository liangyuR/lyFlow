#include <doctest/doctest.h>
#include <cmath>
#include <limits>
#include <set>

#include "helpers.h"
#include "lyflow/cloud_groups.h"
#include "lyflow/pose.h"
#include "lyflow/value.h"

namespace lyflow::ops {
void registerP2HeightRules(Registry&);
}
namespace {
using namespace lyflow;
Registry heightRegistry() {
  Registry r;
  ops::registerP2HeightRules(r);
  return r;
}
Record object(double z = .5, double h = 1) {
  return poses::make3DList(
      nlohmann::json::array(
          {poses::make3D("box", "input", {0, 0, z}, {0, 0, 0, 1}, {1, 1, h}).data}),
      "input");
}
PointCloud plane(double z, int side = 11, double step = .1) {
  PointCloud c;
  for (int y = 0; y < side; ++y)
    for (int x = 0; x < side; ++x)
      c.push(x * step, y * step, z);
  return c;
}
class Cancelled : public ExecContext {
public:
  bool cancelled() const override { return true; }
  void progress(float, std::string_view) override {}
  void log(LogLevel, std::string) override {}
  const std::filesystem::path& baseDir() const override { return base; }
  int threadBudget() const override { return 1; }
  std::filesystem::path base;
};
}

TEST_CASE("P2 top-layer retains original grouped identity, channels and unsupported validity") {
  auto registry = heightRegistry();
  test::OpCall call(registry);
  PointCloud c;
  for (int i = 0; i < 6; ++i) {
    c.push(i * .1, 0, i < 3 ? .4 : 1);
    c.intensity.push_back(i);
    c.rgb.insert(c.rgb.end(), {1, 2, 3});
    c.normals.insert(c.normals.end(), {0, 0, 1});
  }
  c.push(0, 0, 7);
  c.intensity.push_back(6);
  c.rgb.insert(c.rgb.end(), {1, 2, 3});
  c.normals.insert(c.normals.end(), {0, 0, 1});
  const auto id = c.id;
  call.inputs["cloud"] = Data::cloud(c);
  REQUIRE(call.run("cloud.top_layer",
                   {{"minPoints", Value::integer(3)}, {"layerThickness", Value::number(.02)}})
              .ok);
  CHECK(std::abs(call.out("height").asMeasurement()->value - 1) < 1e-7);
  const auto* top = call.out("top").asCloud();
  REQUIRE(top);
  CHECK(top->pointCount() == 3);
  CHECK(top->hasNormals());
  CHECK(top->hasIntensity());
  CHECK(top->hasRgb());
  CHECK(call.out("topIndices").asIndices()->sourceCloudId == id);
  CHECK(call.out("topIndices").asIndices()->values == std::vector<std::int32_t>{3, 4, 5});
  CHECK(top->id != id);
  CHECK(call.inputs["cloud"].asCloud()->xyz == c.xyz);
  call.inputs["indices"] = Data::indices(Indices{id, {0, 1, 2, 3, 4, 5, 6}});
  call.inputs["groups"] = Data::record(
      cloud_groups::make(c, {{"floor", 0, 3}, {"object", 3, 3}, {"outlier", 6, 1}}, "input"));
  REQUIRE(call.run("cloud.top_layer", {{"minPoints", Value::integer(3)}}).ok);
  const auto items = call.out("heights").asRecord()->data["items"];
  CHECK(items[0]["id"] == "floor");
  CHECK(items[2]["valid"] == false);
  CHECK(items[2]["height"].is_null());
  CHECK(call.out("topGroups").asRecord()->data["groups"][2]["count"] == 0);
  call.inputs.clear();
  call.inputs["cloud"] = Data::cloud(PointCloud{});
  REQUIRE(call.run("cloud.top_layer").ok);
  CHECK_FALSE(call.out("height").asMeasurement()->ok);
  PointCloud sideways;
  for (int i = 0; i < 4; ++i)
    sideways.push(3, i, 0);
  sideways.push(9, 0, 0);
  call.inputs["cloud"] = Data::cloud(sideways);
  REQUIRE(call.run("cloud.top_layer", {{"heightAxis", Value::vec({1, 0, 0})},
                                       {"origin", Value::vec({1, 0, 0})},
                                       {"minPoints", Value::integer(3)}})
              .ok);
  CHECK(std::abs(call.out("height").asMeasurement()->value - 2) < 1e-7);
}

TEST_CASE("P2 neighboring support strategies honor local directions and fixed top anchor") {
  auto registry = heightRegistry();
  test::OpCall call(registry), apply(registry);
  PointCloud c;
  for (int y = 0; y < 8; ++y)
    for (int x = 0; x < 8; ++x) {
      c.push(-.4 + x * .1, .6 + y * .05, 0);
      c.push(.6 + y * .05, -.4 + x * .1, .2);
    }
  call.inputs["cloud"] = Data::cloud(c);
  call.inputs["instances"] = Data::record(object());
  apply.inputs["instances"] = call.inputs.at("instances");
  for (const char* method : {"point_plane", "axis_intercept", "top_layer", "mean", "farthest"}) {
    REQUIRE(call.run("cloud.neighbor_height", {{"method", Value::text(method)},
                                               {"direction", Value::text("top")},
                                               {"minPoints", Value::integer(10)}})
                .ok);
    const auto result = call.out("heights").asRecord()->data["items"][0];
    CHECK(result["valid"] == true);
    CHECK(std::abs(result["height"].get<double>() - 1) < 1e-6);
    apply.inputs["heights"] = call.out("heights");
    REQUIRE(apply.run("pose.apply_height").ok);
    const auto item = apply.out("instances").asRecord()->data["items"][0];
    CHECK(std::abs(item["position"][2].get<double>() + item["size"][2].get<double>() / 2 - 1) <
          1e-8);
  }
  REQUIRE(call.run("cloud.neighbor_height", {{"direction", Value::text("right")}}).ok);
  CHECK(std::abs(call.out("heights").asRecord()->data["items"][0]["height"].get<double>() - .8) <
        1e-6);
  REQUIRE(call.run("cloud.neighbor_height", {{"direction", Value::text("left")}}).ok);
  CHECK(call.out("heights").asRecord()->data["items"][0]["valid"] == false);
  apply.inputs["heights"] = call.out("heights");
  REQUIRE(apply.run("pose.apply_height").ok);
  CHECK(apply.out("instances").asRecord()->data == object().data);
  const auto measuredCloud = call.inputs.at("cloud");
  call.inputs.erase("cloud");
  REQUIRE(call.run("cloud.neighbor_height", {{"method", Value::text("original")}}).ok);
  CHECK(call.out("heights").asRecord()->data["items"][0]["axisHeight"] == 1);
  REQUIRE(call.run("cloud.neighbor_height",
                   {{"method", Value::text("provided")}, {"providedHeight", Value::number(.3)}})
              .ok);
  apply.inputs["heights"] = call.out("heights");
  REQUIRE(apply.run("pose.apply_height").ok);
  CHECK(std::abs(apply.out("instances").asRecord()->data["items"][0]["size"][2].get<double>() -
                 .3) < 1e-12);
  REQUIRE(apply.run("pose.apply_height", {{"source", Value::text("maximum")}}).ok);
  CHECK(apply.out("instances").asRecord()->data["items"][0]["size"][2] == 1);
  call.inputs["cloud"] = measuredCloud;
  call.inputs["workspace"] =
      Data::record(poses::make3D("ws", "input", {0, .8, 0}, {0, 0, 0, 1}, {1, 1, .1}));
  REQUIRE(call.run("cloud.neighbor_height", {{"direction", Value::text("right")}}).ok);
  CHECK(call.out("heights").asRecord()->data["items"][0]["valid"] == false);
  CHECK_FALSE(call.run("cloud.neighbor_height", {{"marginX", Value::number(2)}}).ok);
}

TEST_CASE("P2 sloped support separates perpendicular measurement from axial box dimension") {
  auto registry = heightRegistry();
  test::OpCall call(registry), apply(registry);
  PointCloud c;
  for (int y = 0; y < 10; ++y)
    for (int x = 0; x < 10; ++x) {
      double xx = -.4 + x * .08;
      c.push(xx, .6 + y * .04, .1 * xx);
    }
  call.inputs["cloud"] = Data::cloud(c);
  call.inputs["instances"] = Data::record(object());
  apply.inputs["instances"] = call.inputs.at("instances");
  REQUIRE(call.run("cloud.neighbor_height", {{"direction", Value::text("top")}}).ok);
  auto result = call.out("heights").asRecord()->data["items"][0];
  CHECK(std::abs(result["height"].get<double>() - 1 / std::sqrt(1.01)) < 1e-6);
  CHECK(std::abs(result["axisHeight"].get<double>() - 1) < 1e-6);
  apply.inputs["heights"] = call.out("heights");
  REQUIRE(apply.run("pose.apply_height").ok);
  CHECK(std::abs(apply.out("instances").asRecord()->data["items"][0]["size"][2].get<double>() - 1) <
        1e-6);
}

TEST_CASE("P2 height application matches instance IDs and preserves tilted top faces") {
  auto registry = heightRegistry();
  test::OpCall call(registry);
  const double half = std::sqrt(.5);
  const auto a = poses::make3D("a", "input", {2, 3, 4}, {0, half, 0, half}, {1, 1, 1});
  const auto b = poses::make3D("b", "input", {0, 0, 0}, {0, 0, 0, 1}, {1, 1, 1});
  call.inputs["instances"] =
      Data::record(poses::make3DList(nlohmann::json::array({a.data, b.data}), "input"));
  call.inputs["heights"] = Data::record(
      {"cloud.NeighborHeights.v1",
       {{"unit", "m"},
        {"frame", "input"},
        {"items", nlohmann::json::array({{{"id", "b"}, {"valid", false}, {"axisHeight", nullptr}},
                                         {{"id", "a"}, {"valid", true}, {"axisHeight", .5}}})}}});
  REQUIRE(call.run("pose.apply_height").ok);
  const auto& items = call.out("instances").asRecord()->data["items"];
  CHECK(items[0]["size"][2] == .5);
  CHECK(items[0]["position"][0].get<double>() == doctest::Approx(2.25));
  CHECK(items[0]["position"][2].get<double>() == doctest::Approx(4));
  CHECK(items[1]["position"] == b.data["position"]);
}

TEST_CASE("P2 neighbor statistical strategies measure distinct supported depth distributions") {
  auto registry = heightRegistry();
  test::OpCall call(registry);
  PointCloud c;
  for (int i = 0; i < 40; ++i)
    c.push(-.4 + (i % 8) * .1, .6 + (i / 8) * .05, i < 20 ? .5 : 0);
  call.inputs["cloud"] = Data::cloud(c);
  call.inputs["instances"] = Data::record(object());
  ParamMap params{{"direction", Value::text("top")},
                  {"sizeMode", Value::text("absolute")},
                  {"width", Value::number(1)},
                  {"length", Value::number(1)},
                  {"method", Value::text("mean")}};
  REQUIRE(call.run("cloud.neighbor_height", params).ok);
  CHECK(std::abs(call.out("heights").asRecord()->data["items"][0]["height"].get<double>() - .75) <
        1e-7);
  params["method"] = Value::text("farthest");
  REQUIRE(call.run("cloud.neighbor_height", params).ok);
  CHECK(std::abs(call.out("heights").asRecord()->data["items"][0]["height"].get<double>() - 1) <
        1e-7);
  params["method"] = Value::text("top_layer");
  REQUIRE(call.run("cloud.neighbor_height", params).ok);
  CHECK(std::abs(call.out("heights").asRecord()->data["items"][0]["height"].get<double>() - .5) <
        1e-7);
}

TEST_CASE("P2 tote evidence distinguishes unknown occupied unrecognized and valid empty") {
  auto registry = heightRegistry();
  test::OpCall measure(registry), apply(registry);
  measure.inputs["cloud"] = Data::cloud(plane(0));
  measure.inputs["instances"] = Data::record(poses::make3DList(nlohmann::json::array(), "input"));
  REQUIRE(measure.run("cloud.tote_observation").ok);
  apply.inputs["counts"] = measure.out("counts");
  apply.inputs["observation"] = Data::record(values::boolean(false));
  REQUIRE(apply.run("cloud.empty_tote").ok);
  CHECK(apply.out("status").asRecord()->data["state"] == "unknown");
  CHECK(apply.out("empty").asRecord()->data["value"] == false);
  apply.inputs["observation"] = Data::record(values::boolean(true));
  REQUIRE(apply.run("cloud.empty_tote").ok);
  CHECK(apply.out("status").asRecord()->data["state"] == "unrecognized");
  const auto limit = static_cast<int>(measure.inputs.at("cloud").asCloud()->pointCount());
  REQUIRE(apply.run("cloud.empty_tote", {{"maxUndetectedPoints", Value::integer(limit)}}).ok);
  CHECK(apply.out("status").asRecord()->data["state"] == "empty");
  measure.inputs["instances"] = Data::record(object());
  REQUIRE(measure.run("cloud.tote_observation").ok);
  apply.inputs["counts"] = measure.out("counts");
  REQUIRE(apply.run("cloud.empty_tote").ok);
  CHECK(apply.out("status").asRecord()->data["state"] == "occupied");
  apply.inputs["observation"] = Data::record(values::number(1));
  CHECK_FALSE(apply.run("cloud.empty_tote").ok);
}

TEST_CASE("P2 finite plane bounds and inscribed occupied patches do not fill holes") {
  auto registry = heightRegistry();
  test::OpCall call(registry);
  PointCloud c;
  for (int y = 0; y < 11; ++y)
    for (int x = 0; x < 11; ++x)
      if (x <= 3 || y <= 3)
        c.push(x * .1, y * .1, 0);
  const auto id = c.id;
  call.inputs["cloud"] = Data::cloud(c);
  const ParamMap common{{"cellSize", Value::number(.1)},
                        {"minPoints", Value::integer(8)},
                        {"minSide", Value::number(.1)},
                        {"minArea", Value::number(.02)},
                        {"minCoverage", Value::number(.1)}};
  auto params = common;
  params["mode"] = Value::text("bounds");
  REQUIRE(call.run("cloud.plane_patches", params).ok);
  const auto bounds = call.out("patches").asRecord()->data["items"];
  REQUIRE(bounds.size() == 1);
  CHECK(std::abs(bounds[0]["area"].get<double>() - 1) < 1e-6);
  CHECK(bounds[0]["coverage"].get<double>() < .7);
  CHECK(bounds[0]["corners"].size() == 4);
  CHECK(call.out("indices").asIndices()->sourceCloudId == id);
  params["mode"] = Value::text("inscribed");
  params["minCoverage"] = Value::number(1);
  REQUIRE(call.run("cloud.plane_patches", params).ok);
  const auto inscribed = call.out("instances").asRecord()->data["items"];
  REQUIRE(inscribed.size() >= 2);
  for (const auto& item : inscribed) {
    const auto center = item["position"];
    CHECK_FALSE((center[0].get<double>() > .5 && center[1].get<double>() > .5));
  }
  const auto* selected = call.out("indices").asIndices();
  const auto& metrics = call.out("patches").asRecord()->data["items"];
  std::vector<cloud_groups::Group> groups;
  std::string frame;
  std::string why;
  REQUIRE(cloud_groups::parse(*call.out("groups").asRecord(), c, *selected, groups, frame, why));
  REQUIRE(groups.size() == inscribed.size());
  std::set<std::int32_t> assigned;
  for (std::size_t group = 0; group < groups.size(); ++group) {
    const auto& pose = inscribed[group];
    const auto matrix = poses::rotationMatrix(pose["rotation"].get<poses::Quaternion>());
    CHECK(metrics[group]["coverage"] == 1);
    CHECK(metrics[group]["support"] == groups[group].count);
    for (std::size_t member = groups[group].offset;
         member < groups[group].offset + groups[group].count; ++member) {
      const auto index = selected->values[member];
      CHECK(assigned.insert(index).second);
      for (int component = 0; component < 3; ++component) {
        double local = 0;
        for (int axis = 0; axis < 3; ++axis)
          local += matrix[3 * axis + component] *
                   (c.xyz[3 * index + axis] - pose["position"][axis].get<double>());
        CHECK(std::abs(local) <= pose["size"][component].get<double>() * .5 + 1e-12);
      }
    }
  }
  CHECK(assigned.size() == c.pointCount());
  CHECK(call.out("uncoveredIndices").asIndices()->values.empty());
  CHECK(poses::validate(*call.out("instances").asRecord(), why));
  const auto before = call.out("patches").asRecord()->data;
  REQUIRE(call.run("cloud.plane_patches", params).ok);
  CHECK(call.out("patches").asRecord()->data == before);
}

TEST_CASE("P2 noisy finite plane support satisfies final model and delivered geometry") {
  auto registry = heightRegistry();
  test::OpCall call(registry);
  PointCloud c;
  for (int y = 0; y < 15; ++y)
    for (int x = 0; x < 15; ++x)
      c.push(x * .05, y * .05,
             .004 * std::sin(2.3 * x + 1.7 * y + 4) + .002 * std::cos(.3 * x * y + 4));
  call.inputs["cloud"] = Data::cloud(c);
  for (const auto settings :
       {std::pair<const char*, double>{"none", .01}, {"quantile", .01}, {"quantile", .003}}) {
    REQUIRE(call.run("cloud.plane_patches", {{"planeDistance", Value::number(.005)},
                                             {"minPoints", Value::integer(20)},
                                             {"maxPlanes", Value::integer(1)},
                                             {"maxIterations", Value::integer(128)},
                                             {"mode", Value::text("bounds")},
                                             {"cellSize", Value::number(.05)},
                                             {"minCoverage", Value::number(0)},
                                             {"surfaceOffset", Value::text(settings.first)},
                                             {"thickness", Value::number(settings.second)}})
                .ok);
    const auto& patches = call.out("patches").asRecord()->data["items"];
    const auto& instances = call.out("instances").asRecord()->data["items"];
    const auto* support = call.out("indices").asIndices();
    const auto* uncovered = call.out("uncoveredIndices").asIndices();
    std::vector<cloud_groups::Group> groups;
    std::string frame, why;
    REQUIRE(cloud_groups::parse(*call.out("groups").asRecord(), c, *support, groups, frame, why));
    REQUIRE_FALSE(patches.empty());
    REQUIRE(patches.size() == instances.size());
    REQUIRE(groups.size() == patches.size());
    CHECK(support->sourceCloudId == c.id);
    CHECK(uncovered->sourceCloudId == c.id);
    std::set<std::int32_t> assigned;
    for (std::size_t patch = 0; patch < patches.size(); ++patch) {
      const auto& metadata = patches[patch];
      const auto& pose = instances[patch];
      const auto& group = groups[patch];
      CHECK(metadata["id"] == pose["id"]);
      CHECK(metadata["id"] == group.id);
      CHECK(metadata["support"] == group.count);
      const auto matrix = poses::rotationMatrix(pose["rotation"].get<poses::Quaternion>());
      double squared = 0;
      std::set<std::pair<int, int>> cells;
      const auto& corner = metadata["corners"][0];
      for (std::size_t member = group.offset; member < group.offset + group.count; ++member) {
        const auto index = support->values[member];
        CHECK(assigned.insert(index).second);
        double residual = metadata["offset"].get<double>();
        double local[3] = {}, grid[2] = {};
        for (int axis = 0; axis < 3; ++axis) {
          const double coordinate = c.xyz[3 * index + axis];
          residual += metadata["normal"][axis].get<double>() * coordinate;
          for (int component = 0; component < 3; ++component)
            local[component] +=
                matrix[3 * axis + component] * (coordinate - pose["position"][axis].get<double>());
          for (int component = 0; component < 2; ++component)
            grid[component] +=
                matrix[3 * axis + component] * (coordinate - corner[axis].get<double>());
        }
        CHECK(std::abs(residual) <= .005 + 1e-12);
        squared += residual * residual;
        for (int component = 0; component < 3; ++component)
          CHECK(std::abs(local[component]) <= pose["size"][component].get<double>() * .5 + 1e-12);
        cells.emplace(static_cast<int>(std::floor(grid[0] / .05 + 1e-6)),
                      static_cast<int>(std::floor(grid[1] / .05 + 1e-6)));
      }
      CHECK(std::abs(metadata["rms"].get<double>() - std::sqrt(squared / group.count)) < 1e-12);
      CHECK(metadata["occupiedCells"] == cells.size());
      const double expected =
          static_cast<double>(cells.size()) /
          (metadata["gridSize"][0].get<int>() * metadata["gridSize"][1].get<int>());
      CHECK(std::abs(metadata["coverage"].get<double>() - expected) < 1e-12);
      CHECK(metadata["boundaryTolerance"].get<double>() <= 1e-12);
    }
    for (auto index : uncovered->values)
      CHECK(assigned.insert(index).second);
    CHECK(assigned.size() == c.pointCount());
    CHECK(call.out("uncovered").asCloud()->pointCount() == uncovered->values.size());
    CHECK(call.inputs.at("cloud").asCloud()->xyz == c.xyz);
  }
}

TEST_CASE("P2 height rules reject invalid identities and never publish cancelled results") {
  auto registry = heightRegistry();
  test::OpCall call(registry);
  auto c = plane(1);
  call.inputs["cloud"] = Data::cloud(c);
  CHECK_FALSE(call.run("cloud.top_layer", {{"heightAxis", Value::vec({0, 0, 0})}}).ok);
  call.inputs["groups"] = Data::record(cloud_groups::make(c, {{"a", 0, 1}}, "input"));
  call.inputs["indices"] = Data::indices(Indices{c.id + 1, {0}});
  CHECK_FALSE(call.run("cloud.top_layer").ok);
  call.inputs.clear();
  c.xyz[0] = std::numeric_limits<float>::quiet_NaN();
  call.inputs["cloud"] = Data::cloud(c);
  CHECK_FALSE(call.run("cloud.plane_patches").ok);
  call.inputs.clear();
  const auto huge = poses::make3D("huge", "input", {0, 0, 1.5e308}, {0, 0, 0, 1}, {1, 1, 1e308});
  const auto valid = poses::make3D("valid", "input", {0, 0, 1}, {0, 0, 0, 1}, {1, 1, 1});
  const auto batch = poses::make3DList(nlohmann::json::array({valid.data, huge.data}), "input");
  std::string why;
  REQUIRE(poses::validate(batch, why));
  call.inputs = {{"instances", Data::record(batch)}};
  REQUIRE(call.run("cloud.neighbor_height",
                   {{"method", Value::text("provided")}, {"providedHeight", Value::number(1)}})
              .ok);
  call.inputs["heights"] = call.out("heights");
  for (const char* policy : {"measured", "maximum"}) {
    const auto result = call.run("pose.apply_height", {{"source", Value::text(policy)}});
    CHECK_FALSE(result.ok);
    CHECK(result.code == "bad_input");
    CHECK(call.outputs.empty());
    CHECK(call.inputs.at("instances").asRecord()->data == batch.data);
  }
  auto wide = poses::make3D("wide", "input", {0, 0, 1}, {0, 0, 0, 1}, {1e308, 1, 1});
  call.inputs["instances"] =
      Data::record(poses::make3DList(nlohmann::json::array({wide.data}), "input"));
  call.inputs["cloud"] = Data::cloud(PointCloud{});
  CHECK_FALSE(call.run("cloud.neighbor_height", {{"widthRatio", Value::number(100)}}).ok);
  CHECK(call.outputs.empty());
  for (const char* id : {"cloud.top_layer", "cloud.neighbor_height", "cloud.tote_observation",
                         "cloud.plane_patches"}) {
    const auto* op = registry.find(id);
    REQUIRE(op);
    ParamMap params;
    for (const auto& p : op->params)
      params[p.name] = p.def;
    std::unordered_map<std::string, Data> inputs{
        {"cloud", Data::cloud(plane(1))},
        {"instances", Data::record(object())},
        {"observation", Data::record(values::boolean(true))}},
        outputs;
    Inputs in(inputs);
    Outputs out(outputs);
    std::filesystem::path base;
    ParamView view(params, base);
    Cancelled ctx;
    op->compute(in, view, out, ctx);
    CHECK(outputs.empty());
  }
}
