#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>

#include "helpers.h"
#include "lyflow/profile.h"
#include "lyflow_profile/context.h"
#include "lyflow_profile/features.h"
#include "lyflow_profile/provenance.h"

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

TEST_CASE("Robust Profile box toward preserves the original double center and strict context") {
  const float upper = std::nextafter(1.0F, 2.0F);
  const auto source = profileOf({{upper, 0}, {1, 0}, {1.1F, 0}});
  test::OpCall call(robustRegistry());
  attachLine(call, source, true);
  Box2D toward;
  toward.min[0] = 1;
  toward.max[0] = upper;
  toward.min[1] = toward.max[1] = 0;
  const auto box = Data::box2d(toward);
  call.inputs["toward"] = box;
  call.inputs["towardContext"] = context(source, box);
  const std::unordered_map<std::string, Value> options{{"dirMode", Value::text("fixed")},
                                                       {"segmentPoints", Value::integer(0)},
                                                       {"endpoints", Value::text("inlier_ends")}};
  REQUIRE_MESSAGE(call.run("profile.fit_line_robust", options).ok, call.status.message);
  CHECK(call.out("innerEnd").asPoint2D()->p[0] == upper);
  const auto provenance = call.out("provenance");
  REQUIRE(provenance.asRecord());
  std::string why;
  nlohmann::json sources;
  CHECK(profile_contract::profileSources(source, provenance.asRecord(), sources, why));
  Point2D point;
  point.p[0] = 1;
  const auto rounded = Data::point2d(point);
  call.inputs["toward"] = rounded;
  call.inputs["towardContext"] = context(source, rounded, true);
  REQUIRE(call.run("profile.fit_line_robust", options).ok);
  CHECK(call.out("innerEnd").asPoint2D()->p[0] == 1.0F);
  call.inputs["toward"] = box;
  CHECK_FALSE(call.run("profile.fit_line_robust", options).ok);
  CHECK(call.outputs.empty());
  call.inputs["towardContext"] = context(source, box);
  toward.max[0] = 2;
  call.inputs["toward"] = Data::box2d(toward);
  CHECK_FALSE(call.run("profile.fit_line_robust", options).ok);
  CHECK(call.outputs.empty());
}

TEST_CASE(
    "Robust Profile fit provenance is bound to the exact input and preserved in geometry sources") {
  for (const bool circle : {false, true}) {
    const auto source = circle ? arcProfile() : lineProfile();
    Record provenance;
    std::string why;
    REQUIRE(profile_contract::rebindProvenance(source, source, nullptr, provenance, why));
    auto reference = *source.asProfile();
    reference.descriptor.sourceId = "external-ROI";
    const auto referenceData = Data::profile(std::move(reference));
    Record extended;
    REQUIRE(profile_contract::extendProvenanceSources(
        source, provenance, nlohmann::json::array({profile_contract::context(referenceData)}),
        extended, why));
    test::OpCall call(robustRegistry());
    if (circle)
      call.inputs = {{"profile", source}};
    else
      attachLine(call, source);
    call.inputs["provenance"] = Data::record(extended);
    const char* id = circle ? "profile.fit_circle_robust" : "profile.fit_line_robust";
    REQUIRE_MESSAGE(call.run(id).ok, call.status.message);
    const auto& sources =
        call.out(circle ? "circleContext" : "lineContext").asRecord()->data["sources"];
    CHECK(std::find(sources.begin(), sources.end(), profile_contract::context(referenceData)) !=
          sources.end());
    CHECK(call.out("provenance").asRecord()->data["profileContext"] ==
          profile_contract::context(source));
    extended.data["profileContext"]["profileHash"] = std::string(32, '0');
    call.inputs["provenance"] = Data::record(extended);
    CHECK_FALSE(call.run(id).ok);
    CHECK(call.outputs.empty());
  }
}

TEST_CASE(
    "Typed public Profile fit APIs share callback contracts and fail atomically without required inputs") {
  const auto source = lineProfile("m", "XY", true);
  test::OpCall call(robustRegistry());
  attachLine(call, source, true);
  REQUIRE(call.run("profile.fit_line_robust", {{"dirMode", Value::text("fixed")}}).ok);
  profile_contract::RobustLineInputs inputs;
  inputs.profile = source;
  inputs.box = call.inputs.at("box");
  inputs.boxContext = call.inputs.at("boxContext");
  inputs.toward = call.inputs.at("toward");
  inputs.towardContext = call.inputs.at("towardContext");
  inputs.refLine = call.inputs.at("refLine");
  inputs.refLineContext = call.inputs.at("refLineContext");
  profile_contract::RobustLineResult result;
  test::StubContext ctx;
  REQUIRE(
      profile_contract::fitRobustLine(inputs, ParamView(call.params, ctx.baseDir()), result, ctx)
          .ok);
  CHECK(result.profile.profilePtr() == source.profilePtr());
  CHECK(result.quality.asRecord()->data == call.out("quality").asRecord()->data);
  CHECK(result.inliers.asIndices()->values == call.out("inliers").asIndices()->values);
  CHECK(result.weightedInliers.asIndices()->values ==
        call.out("weightedInliers").asIndices()->values);
  result.quality = Data::record(Record{"sentinel", {}});
  auto invalid = inputs;
  invalid.boxContext = {};
  CHECK_FALSE(
      profile_contract::fitRobustLine(invalid, ParamView(call.params, ctx.baseDir()), result, ctx)
          .ok);
  CHECK(result.quality.asRecord()->type == "sentinel");
  invalid = inputs;
  invalid.refLine = {};
  CHECK_FALSE(
      profile_contract::fitRobustLine(invalid, ParamView(call.params, ctx.baseDir()), result, ctx)
          .ok);
  CHECK(result.quality.asRecord()->type == "sentinel");
  ParamMap missing;
  CHECK_FALSE(
      profile_contract::fitRobustLine(inputs, ParamView(missing, ctx.baseDir()), result, ctx).ok);
  call.params["minInliers"] = Value::number(std::numeric_limits<double>::infinity());
  CHECK_FALSE(
      profile_contract::fitRobustLine(inputs, ParamView(call.params, ctx.baseDir()), result, ctx)
          .ok);
  CHECK(result.quality.asRecord()->type == "sentinel");
  const auto arc = arcProfile();
  call.inputs = {{"profile", arc}};
  REQUIRE(call.run("profile.fit_circle_robust").ok);
  profile_contract::RobustCircleResult circle;
  REQUIRE(profile_contract::fitRobustCircle({arc, {}}, ParamView(call.params, ctx.baseDir()),
                                            circle, ctx)
              .ok);
  CHECK(circle.quality.asRecord()->data == call.out("quality").asRecord()->data);
  CHECK(circle.inliers.asIndices()->values == call.out("inliers").asIndices()->values);
  CHECK(circle.profile.profilePtr() == arc.profilePtr());
  circle.quality = Data::record(Record{"sentinel", {}});
  CHECK_FALSE(
      profile_contract::fitRobustCircle({}, ParamView(call.params, ctx.baseDir()), circle, ctx).ok);
  CHECK(circle.quality.asRecord()->type == "sentinel");
  const auto lineDescriptions = profile_contract::robustLineParams();
  const auto circleDescriptions = profile_contract::robustCircleParams();
  CHECK(lineDescriptions.size() == robustRegistry().find("profile.fit_line_robust")->params.size());
  CHECK(circleDescriptions[4].visibleWhen.param == "radiusMode");
  CHECK(circleDescriptions[4].visibleWhen.eq.stringValue() == "fixed");
}

TEST_CASE(
    "Public robust line preserves legacy toward permutations exact segment boundary and direction guards") {
  auto attach = [](test::OpCall& call, const std::vector<std::array<float, 2>>& points,
                   const Box2D& roi, const Box2D& toward, const Line2D* reference = nullptr) {
    const auto profile = profileOf(points);
    const auto box = Data::box2d(roi), anchor = Data::box2d(toward);
    call.inputs = {{"profile", profile},
                   {"box", box},
                   {"boxContext", context(profile, box)},
                   {"toward", anchor},
                   {"towardContext", context(profile, anchor)}};
    if (reference) {
      const auto line = Data::line2d(*reference);
      call.inputs["refLine"] = line;
      call.inputs["refLineContext"] = context(profile, line);
    }
  };
  SUBCASE("legacy:77 toward selection is independent of ascending or descending slots") {
    for (const bool ascend : {true, false})
      for (const bool left : {true, false}) {
        CAPTURE(ascend);
        CAPTURE(left);
        std::vector<std::array<float, 2>> points;
        for (int i = 0; i < 100; ++i) {
          const auto k = static_cast<float>(ascend ? i : 99 - i);
          points.push_back({k * .001F, .5F + k * .0001F});
        }
        test::OpCall call(robustRegistry());
        attach(call, points, {{-1, 0}, {1, 1}},
               left ? Box2D{{-.010F, .4F}, {-.005F, .6F}} : Box2D{{.105F, .4F}, {.110F, .6F}});
        REQUIRE(call.run("profile.fit_line_robust", {{"distanceThreshold", Value::number(.0005)},
                                                     {"segmentPoints", Value::integer(10)}})
                    .ok);
        const auto* inliers = call.out("inliers").asIndices();
        REQUIRE(inliers);
        REQUIRE(inliers->values.size() == 10);
        const bool low = ascend == left;
        CHECK(inliers->values.front() == (low ? 0 : 90));
        CHECK(inliers->values.back() == (low ? 9 : 99));
        CHECK(call.out("innerEnd").asPoint2D()->p[0] ==
              doctest::Approx(left ? 0.F : .099F).epsilon(.01));
        CHECK(call.out("quality").asRecord()->data["segmentApplied"] == true);
      }
  }
  SUBCASE("legacy:114 segment count equal to or above inlier count never trims") {
    std::vector<std::array<float, 2>> points;
    for (int i = 0; i < 20; ++i) {
      const auto k = static_cast<float>(i);
      points.push_back({k * .001F, .5F + k * .0001F});
    }
    for (const int limit : {20, 500}) {
      test::OpCall call(robustRegistry());
      attach(call, points, {{-1, 0}, {1, 1}}, {{.05F, .4F}, {.06F, .6F}});
      REQUIRE(call.run("profile.fit_line_robust", {{"distanceThreshold", Value::number(.0005)},
                                                   {"segmentPoints", Value::integer(limit)}})
                  .ok);
      CHECK(call.out("inliers").asIndices()->values.size() == 20);
      CHECK(call.out("quality").asRecord()->data["segmentApplied"] == false);
      CHECK(call.out("innerEnd").asPoint2D()->p[0] == doctest::Approx(.019F).epsilon(.01));
    }
  }
  SUBCASE(
      "legacy:1562 1575 fixed relative nominal and band inside outside retain original geometry policy") {
    auto fit = [&](double angle, const char* mode, double nominal = 0, double referenceAngle = 0) {
      std::vector<std::array<float, 2>> points;
      const double radians = angle * 3.14159265358979323846 / 180;
      for (int i = 0; i < 40; ++i) {
        const double t = .004 * static_cast<double>(i) / 39;
        points.push_back(
            {static_cast<float>(t * std::cos(radians)), static_cast<float>(t * std::sin(radians))});
      }
      Line2D reference;
      const double theta = referenceAngle * 3.14159265358979323846 / 180;
      reference.dir[0] = static_cast<float>(std::cos(theta));
      reference.dir[1] = static_cast<float>(std::sin(theta));
      auto call = std::make_unique<test::OpCall>(robustRegistry());
      attach(*call, points, {{-.001F, -.002F}, {.005F, .004F}}, {{.006F, -.001F}, {.007F, .001F}},
             &reference);
      REQUIRE(call->run("profile.fit_line_robust", {{"dirMode", Value::text(mode)},
                                                    {"dirNominalDeg", Value::number(nominal)},
                                                    {"dirTolDeg", Value::number(10)},
                                                    {"distanceThreshold", Value::number(.005)}})
                  .ok);
      return call;
    };
    const auto angle = [](const Line2D& line) {
      return std::atan2(line.dir[1], line.dir[0]) * 180 / 3.14159265358979323846;
    };
    auto fixed = fit(25, "fixed", 3);
    CHECK(angle(*fixed->out("line").asLine2D()) == doctest::Approx(3).epsilon(.02));
    auto band = fit(2, "band"), free = fit(2, "free");
    CHECK(band->out("line").valueJson() == free->out("line").valueJson());
    CHECK(angle(*fit(30, "band")->out("line").asLine2D()) == doctest::Approx(0).epsilon(.02));
    CHECK(angle(*fit(30, "free")->out("line").asLine2D()) == doctest::Approx(30).epsilon(.05));
    auto a = fit(25, "fixed", 5, 0), b = fit(25, "fixed", 5, 180);
    CHECK(a->out("line").asLine2D()->dir[0] > 0);
    CHECK(a->out("line").asLine2D()->dir[0] ==
          doctest::Approx(b->out("line").asLine2D()->dir[0]).epsilon(1e-6));
    CHECK(a->out("line").asLine2D()->dir[1] ==
          doctest::Approx(b->out("line").asLine2D()->dir[1]).epsilon(1e-6));
  }
}
