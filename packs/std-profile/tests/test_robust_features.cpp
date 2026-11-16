#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>

#include "helpers.h"
#include "lyflow/profile.h"
#include "lyflow_profile/context.h"

namespace lyflow::packs::std_profile {
void registerRobustProfileFeatures(Registry&);
}
using namespace lyflow;
namespace {
const Registry& robustRegistry() {
  static const Registry registry = [] {
    Registry value;
    packs::std_profile::registerRobustProfileFeatures(value);
    return value;
  }();
  return registry;
}

Data profileOf(const std::vector<std::array<float, 2>>& points, const char* unit = "m",
               const char* plane = "XY", bool masks = false) {
  PointCloud cloud;
  std::vector<std::uint8_t> validity;
  std::vector<std::uint32_t> sources;
  const bool mm = std::string(unit) == "mm";
  const bool xy = std::string(plane) == "XY";
  for (std::size_t i = 0; i < points.size(); ++i) {
    const float a = mm ? points[i][0] * 1000.0F : points[i][0];
    const float b = mm ? points[i][1] * 1000.0F : points[i][1];
    const float third = mm ? 17.0F : 0.017F;
    if (xy)
      cloud.push(a, b, third);
    else
      cloud.push(a, third, b);
    cloud.intensity.push_back(static_cast<float>(i + 1));
    cloud.normals.insert(cloud.normals.end(), {0, 0, 1});
    cloud.rgb.insert(cloud.rgb.end(), {static_cast<std::uint8_t>(i), 20, 30});
    validity.push_back(masks && (i == 2 || i == 7) ? 0 : 1);
    sources.push_back(static_cast<std::uint32_t>(3 * i + 1));
  }
  if (masks && points.size() > 7) {
    cloud.xyz[3 * 2] = 123;
    cloud.xyz[3 * 2 + (xy ? 1 : 2)] = 987;
    cloud.xyz[3 * 7] = std::numeric_limits<float>::quiet_NaN();
  }
  Profile output;
  std::string why;
  REQUIRE(makeProfile(
      std::make_shared<const PointCloud>(std::move(cloud)),
      {1, "measurement", unit, plane, "right", "acquisition", "scanner-A", 3 * points.size() + 2},
      std::move(validity), std::move(sources), output, why));
  return Data::profile(std::move(output));
}
Data lineProfile(const char* unit = "m", const char* plane = "XY", bool masks = false) {
  std::vector<std::array<float, 2>> points;
  for (int i = 0; i < 16; ++i)
    points.push_back({i * 0.001F, 0.002F + i * 0.0001F});
  return profileOf(points, unit, plane, masks);
}
Data arcProfile(const char* unit = "m", const char* plane = "XY", bool masks = false) {
  std::mt19937 generator(11);
  std::normal_distribution<double> jitter(0.0, 0.000015);
  std::vector<std::array<float, 2>> points;
  for (int i = 0; i < 50; ++i) {
    const double angle = (30.0 + 50.0 * i / 49) * 3.14159265358979323846 / 180;
    points.push_back({static_cast<float>(0.004 * std::sin(angle) + jitter(generator)),
                      static_cast<float>(0.004 - 0.004 * std::cos(angle) + jitter(generator))});
  }
  return profileOf(points, unit, plane, masks);
}
Data context(const Data& profile, const Data& geometry, bool v1 = false) {
  return Data::record(
      Record{v1 ? "profile.Geometry.v1" : "profile.Geometry.v2",
             v1 ? profile_contract::geometryContext(profile, geometry)
                : profile_contract::geometryContextV2(profile, geometry, "declared")});
}
void attachLine(test::OpCall& call, const Data& profile, bool reference = false) {
  Box2D bounds;
  bounds.min[0] = -0.01F;
  bounds.min[1] = -0.02F;
  bounds.max[0] = 0.05F;
  bounds.max[1] = 0.02F;
  Point2D toward;
  toward.p[0] = 0.03F;
  toward.p[1] = 0.004F;
  const auto box = Data::box2d(bounds), point = Data::point2d(toward);
  call.inputs = {{"profile", profile},
                 {"box", box},
                 {"boxContext", context(profile, box)},
                 {"toward", point},
                 {"towardContext", context(profile, point, true)}};
  if (reference) {
    const auto line = Data::line2d(Line2D{});
    call.inputs["refLine"] = line;
    call.inputs["refLineContext"] = context(profile, line);
  }
}
void checkSelection(test::OpCall& call, const Data& source) {
  const auto* input = source.asProfile();
  const auto* output = call.out("profile").asProfile();
  REQUIRE(output == input);
  const auto* unique = call.out("inliers").asIndices();
  const auto* weighted = call.out("weightedInliers").asIndices();
  REQUIRE(unique);
  REQUIRE(weighted);
  CHECK(unique->sourceCloudId == input->cloud->id);
  CHECK(weighted->sourceCloudId == input->cloud->id);
  CHECK(std::is_sorted(unique->values.begin(), unique->values.end()));
  CHECK(std::adjacent_find(unique->values.begin(), unique->values.end()) == unique->values.end());
  const auto* map = call.out("indexMap").asRecord();
  REQUIRE(map);
  CHECK(map->type == "profile.Selection.v1");
  CHECK(map->data.size() == 7);
  CHECK(map->data["context"] == profile_contract::context(source));
  CHECK(map->data["sourceCloudId"] == std::to_string(input->cloud->id));
  CHECK(map->data["indexSpace"] == "profile_slots");
  CHECK(map->data["indexCount"] == unique->values.size());
  CHECK(map->data["weightedInlierCount"] == weighted->values.size());
  CHECK(map->data["sourceIndexMapping"] == "profile.sourceIndices");
  CHECK(map->data.dump().size() < 1024);
}
struct PollContext final : ExecContext {
  mutable int calls = 0;
  int stopAt = std::numeric_limits<int>::max();
  std::filesystem::path base;
  bool cancelled() const override { return ++calls >= stopAt; }
  void progress(float, std::string_view) override {}
  void log(LogLevel, std::string) override {}
  const std::filesystem::path& baseDir() const override { return base; }
  int threadBudget() const override { return 1; }
};
} // namespace

TEST_CASE("Robust Profile line operators preserve source slots across metric units and planes") {
  for (const auto* unit : {"m", "mm"}) {
    for (const auto* plane : {"XY", "XZ"}) {
      CAPTURE(unit);
      CAPTURE(plane);
      const auto source = lineProfile(unit, plane, true);
      const auto before = profile_contract::context(source);
      test::OpCall call(robustRegistry());
      attachLine(call, source);
      REQUIRE_MESSAGE(
          call.run("profile.fit_line_robust", {{"segmentPoints", Value::integer(0)}}).ok,
          call.status.message);
      checkSelection(call, source);
      const auto* indices = call.out("inliers").asIndices();
      CHECK(indices->values.size() == 14);
      CHECK(std::find(indices->values.begin(), indices->values.end(), 2) == indices->values.end());
      CHECK(std::find(indices->values.begin(), indices->values.end(), 7) == indices->values.end());
      CHECK(call.out("profile").asProfile()->sourceIndices[3] == 10);
      CHECK(profile_contract::context(source) == before);
      const auto& line = *call.out("line").asLine2D();
      CHECK(line.unit == Unit2D::Meter);
      CHECK(line.dir[1] / line.dir[0] == doctest::Approx(0.1).epsilon(1e-4));
      const auto* point = call.out("innerEnd").asPoint2D();
      REQUIRE(point);
      CHECK(point->p[0] == doctest::Approx(0.015).epsilon(1e-5));
      CHECK(point->p[1] == doctest::Approx(0.0035).epsilon(1e-5));
      ProfileDescriptor descriptor;
      std::string why;
      REQUIRE(profile_contract::geometryMatchesPhysical(
          call.out("line"), call.out("lineContext").asRecord(), descriptor, why));
      CHECK(descriptor.frame == "measurement");
      CHECK(descriptor.plane == plane);
      REQUIRE(profile_contract::geometryMatchesPhysical(
          call.out("innerEnd"), call.out("pointContext").asRecord(), descriptor, why));
      const auto& quality = call.out("quality").asRecord()->data;
      CHECK(quality["uniqueInlierCount"] == 14);
      CHECK(quality["weightedInlierCount"] == 14);
      CHECK(quality["unit"] == "m");
    }
  }
}

TEST_CASE(
    "Robust Profile line direction methods preserve free results and expose weighted fallback separately") {
  const auto source = lineProfile();
  test::OpCall free(robustRegistry()), band(robustRegistry()), fixed(robustRegistry());
  attachLine(free, source, true);
  attachLine(band, source, true);
  attachLine(fixed, source, true);
  REQUIRE(free.run("profile.fit_line_robust").ok);
  REQUIRE(band.run("profile.fit_line_robust",
                   {{"dirMode", Value::text("band")}, {"dirTolDeg", Value::number(10)}})
              .ok);
  CHECK(free.out("line").valueJson() == band.out("line").valueJson());
  CHECK(band.out("quality").asRecord()->data["directionPinned"] == false);
  REQUIRE(fixed
              .run("profile.fit_line_robust",
                   {{"dirMode", Value::text("fixed")}, {"dirNominalDeg", Value::number(3)}})
              .ok);
  CHECK(fixed.out("quality").asRecord()->data["directionPinned"] == true);
  const auto* fixedLine = fixed.out("line").asLine2D();
  CHECK(std::atan2(fixedLine->dir[1], fixedLine->dir[0]) * 180 / 3.14159265358979323846 ==
        doctest::Approx(3).epsilon(1e-5));
  std::vector<std::array<float, 2>> points;
  for (int i = 0; i < 6; ++i)
    points.push_back({i * 0.01F, i * 0.001F});
  const auto weightedSource = profileOf(points);
  attachLine(fixed, weightedSource, true);
  REQUIRE(fixed
              .run("profile.fit_line_robust", {{"dirMode", Value::text("fixed")},
                                               {"distanceThreshold", Value::number(1e-8)},
                                               {"segmentPoints", Value::integer(0)},
                                               {"endpoints", Value::text("inlier_ends")}})
              .ok);
  checkSelection(fixed, weightedSource);
  CHECK(fixed.out("inliers").asIndices()->values == std::vector<std::int32_t>{2, 3, 4});
  CHECK(fixed.out("weightedInliers").asIndices()->values == std::vector<std::int32_t>{2, 3, 3, 4});
  CHECK(fixed.out("quality").asRecord()->data["uniqueInlierCount"] == 3);
  CHECK(fixed.out("quality").asRecord()->data["weightedInlierCount"] == 4);
  CHECK(fixed.out("quality").asRecord()->data["uniqueInlierRatio"] == 0.5);
}

TEST_CASE(
    "Robust Profile geometry inputs reject frame plane handedness and content mismatches atomically") {
  const auto source = lineProfile();
  for (const auto* field : {"frame", "plane", "handedness"}) {
    CAPTURE(field);
    auto changed = *source.asProfile();
    if (std::string(field) == "frame")
      changed.descriptor.frame = "other";
    if (std::string(field) == "plane")
      changed.descriptor.plane = "XZ";
    if (std::string(field) == "handedness")
      changed.descriptor.handedness = "reflected";
    const auto other = Data::profile(changed);
    test::OpCall call(robustRegistry());
    attachLine(call, source);
    call.inputs["boxContext"] = context(other, call.inputs["box"]);
    CHECK_FALSE(call.run("profile.fit_line_robust").ok);
    CHECK(call.outputs.empty());
    CHECK(call.status.portName == "boxContext");
  }
  test::OpCall call(robustRegistry());
  attachLine(call, source);
  auto point = *call.inputs["toward"].asPoint2D();
  point.p[0] += 1;
  call.inputs["toward"] = Data::point2d(point);
  CHECK_FALSE(call.run("profile.fit_line_robust").ok);
  CHECK(call.outputs.empty());
  CHECK(call.status.portName == "towardContext");
  attachLine(call, source);
  CHECK_FALSE(call.run("profile.fit_line_robust", {{"dirMode", Value::text("fixed")}}).ok);
  CHECK(call.outputs.empty());
  CHECK_FALSE(call.validate("profile.fit_line_robust", {{"dirMode", Value::text("band")}},
                            {"profile", "box", "boxContext", "toward", "towardContext"})
                  .empty());
}

TEST_CASE(
    "Robust Profile circles preserve full source identity under free fixed and prior methods") {
  for (const auto* unit : {"m", "mm"}) {
    for (const auto* plane : {"XY", "XZ"}) {
      const auto source = arcProfile(unit, plane, true);
      const auto before = profile_contract::context(source);
      for (const auto* mode : {"free", "fixed", "prior"}) {
        CAPTURE(unit);
        CAPTURE(plane);
        CAPTURE(mode);
        test::OpCall call(robustRegistry());
        call.inputs = {{"profile", source}};
        REQUIRE_MESSAGE(
            call.run("profile.fit_circle_robust", {{"radiusMode", Value::text(mode)},
                                                   {"minRadius", Value::number(0.001)},
                                                   {"maxRadius", Value::number(0.008)},
                                                   {"fixedRadius", Value::number(0.004)},
                                                   {"radiusPrior", Value::number(0.004)}})
                .ok,
            call.status.message);
        checkSelection(call, source);
        const auto* circle = call.out("circle").asCircle2D();
        REQUIRE(circle);
        CHECK(circle->unit == Unit2D::Meter);
        CHECK(circle->radius > 0.001F);
        CHECK(circle->radius < 0.008F);
        if (std::string(mode) == "fixed") {
          CHECK(circle->radius == static_cast<float>(0.004));
          CHECK(call.out("quality").asRecord()->data["radiusOutcome"] == "fixed");
          CHECK(call.out("quality").asRecord()->data.contains("seedRadiusOutcome"));
        }
        const auto& quality = call.out("quality").asRecord()->data;
        CHECK(quality["radiusMode"] == mode);
        if (std::string(mode) == "prior")
          CHECK(quality["radiusOutcome"] != "free");
        CHECK(quality["uniqueInlierCount"] == quality["weightedInlierCount"]);
        CHECK(profile_contract::context(source) == before);
        ProfileDescriptor descriptor;
        std::string why;
        REQUIRE(profile_contract::geometryMatchesPhysical(
            call.out("circle"), call.out("circleContext").asRecord(), descriptor, why));
        CHECK(descriptor.plane == plane);
        const auto& indices = call.out("inliers").asIndices()->values;
        CHECK(std::find(indices.begin(), indices.end(), 2) == indices.end());
        CHECK(std::find(indices.begin(), indices.end(), 7) == indices.end());
      }
    }
  }
}

TEST_CASE(
    "Robust Profile fitting invalid numeric modes and quality gates publish no partial outputs") {
  test::OpCall call(robustRegistry());
  call.inputs = {{"profile", arcProfile()}};
  CHECK_FALSE(call.run("profile.fit_circle_robust",
                       {{"radiusMode", Value::text("fixed")}, {"fixedRadius", Value::number(0)}})
                  .ok);
  CHECK(call.status.paramPath == "fixedRadius");
  CHECK(call.outputs.empty());
  CHECK_FALSE(call.run("profile.fit_circle_robust", {{"radiusMode", Value::text("prior")},
                                                     {"radiusPriorSigma", Value::number(0)}})
                  .ok);
  CHECK(call.status.paramPath == "radiusPriorSigma");
  CHECK(call.outputs.empty());
  CHECK_FALSE(call.run("profile.fit_circle_robust", {{"distanceThreshold", Value::number(0)}}).ok);
  CHECK(call.outputs.empty());
  CHECK_FALSE(call.run("profile.fit_circle_robust", {{"minInliers", Value::integer(1000)}}).ok);
  CHECK(call.status.code == "insufficient_points");
  CHECK(call.outputs.empty());
  attachLine(call, lineProfile());
  CHECK_FALSE(call.run("profile.fit_line_robust", {{"minInliers", Value::integer(1000)}}).ok);
  CHECK(call.outputs.empty());
  CHECK_FALSE(call.run("profile.fit_line_robust", {{"distanceThreshold", Value::number(-1)}}).ok);
  CHECK(call.outputs.empty());
}

TEST_CASE("Robust Profile cancellation before final publication preserves an empty output set") {
  for (const auto* id : {"profile.fit_line_robust", "profile.fit_circle_robust"}) {
    const auto* op = robustRegistry().find(id);
    REQUIRE(op);
    test::OpCall call(robustRegistry());
    if (std::string(id) == "profile.fit_line_robust")
      attachLine(call, lineProfile());
    else
      call.inputs = {{"profile", arcProfile()}};
    ParamMap params;
    for (const auto& parameter : op->params)
      params[parameter.name] = parameter.def;
    std::unordered_map<std::string, Data> values;
    Outputs outputs(values);
    Inputs inputs(call.inputs);
    PollContext completed;
    const ParamView view(params, completed.base);
    REQUIRE(op->compute(inputs, view, outputs, completed).ok);
    REQUIRE_FALSE(values.empty());
    const int finalPoll = completed.calls;
    values.clear();
    PollContext cancelled;
    cancelled.stopAt = finalPoll;
    CHECK_FALSE(op->compute(inputs, view, outputs, cancelled).ok);
    CHECK(values.empty());
  }
}
