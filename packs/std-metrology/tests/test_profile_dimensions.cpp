#include <doctest/doctest.h>

#include <cmath>
#include <limits>

#include "helpers.h"
#include "lyflow/profile.h"
#include "lyflow_profile/context.h"

namespace lyflow::packs::std_metrology {
void registerProfileDimensions(Registry&);
}
using namespace lyflow;
namespace {
const Registry& dimensionsRegistry() {
  static const Registry registry = [] {
    Registry value;
    packs::std_metrology::registerProfileDimensions(value);
    return value;
  }();
  return registry;
}
Data source(const char* id, const char* unit = "m", const char* frame = "measurement",
            const char* plane = "XY", const char* handedness = "right") {
  PointCloud cloud;
  cloud.push(0, 0, 0);
  cloud.push(1, 0, 1);
  Profile result;
  std::string why;
  REQUIRE(makeProfile(std::make_shared<const PointCloud>(cloud),
                      {1, frame, unit, plane, handedness, "acquisition", id, 8}, {1, 1}, {1, 4},
                      result, why));
  return Data::profile(std::move(result));
}
Data point(float x, float y) {
  Point2D result;
  result.p[0] = x;
  result.p[1] = y;
  return Data::point2d(result);
}
Data line(bool segment = true) {
  Line2D result;
  result.dir[0] = 2;
  result.hasSegment = segment;
  result.start[0] = -0.001F;
  result.end[0] = 0.001F;
  return Data::line2d(result);
}
Data circle(float x, float y, float radius) {
  Circle2D result;
  result.center[0] = x;
  result.center[1] = y;
  result.radius = radius;
  return Data::circle2d(result);
}
Data contextV1(const Data& geometry, const Data& profile) {
  return Data::record(
      Record{"profile.Geometry.v1", profile_contract::geometryContext(profile, geometry)});
}
Data contextV2(const Data& geometry, const Data& a, const Data& b) {
  const auto sources = nlohmann::json::array(
      {profile_contract::context(a), profile_contract::context(b), profile_contract::context(a)});
  return Data::record(Record{"profile.Geometry.v2", profile_contract::geometryContextV2FromSources(
                                                        geometry, sources, "declared")});
}
void attachFlush(test::OpCall& call, const Data& a, const Data& b) {
  const auto base = line(), reference = point(0.004F, -0.003F);
  call.inputs = {{"baseLine", base},
                 {"baseLineContext", contextV1(base, a)},
                 {"refPoint", reference},
                 {"refPointContext", contextV2(reference, b, a)}};
}
void attachGap(test::OpCall& call, const Data& a, const Data& b, bool crossed = false) {
  const auto left = circle(0, 0, crossed ? 0.003F : 0.001F);
  const auto right =
      circle(crossed ? 0.004F : 0.01F, crossed ? 0.F : 0.004F, crossed ? 0.003F : 0.002F);
  auto base = *line().asLine2D();
  base.start[0] = 0;
  base.end[0] = 0.02F;
  const auto datum = Data::line2d(base);
  call.inputs = {{"left", left},      {"leftContext", contextV2(left, a, a)},
                 {"right", right},    {"rightContext", contextV2(right, b, a)},
                 {"baseLine", datum}, {"baseLineContext", contextV1(datum, a)}};
}
void attachOffset(test::OpCall& call, const Data& a, const Data& b, bool same = false) {
  const auto start = point(0.001F, 0.002F), end = same ? start : point(0.004F, -0.002F);
  call.inputs = {{"a", start},
                 {"aContext", contextV1(start, a)},
                 {"b", end},
                 {"bContext", contextV2(end, b, a)}};
}
void checkContext(const Data& geometry, const Data& context) {
  const auto* record = context.asRecord();
  REQUIRE(record);
  CHECK(record->type == "profile.Geometry.v2");
  CHECK(record->data["sources"].size() == 2);
  ProfileDescriptor descriptor;
  std::string why;
  REQUIRE_MESSAGE(profile_contract::geometryMatchesPhysical(geometry, record, descriptor, why),
                  why);
  CHECK(descriptor.frame == "measurement");
  CHECK(descriptor.plane == "XY");
  CHECK(descriptor.handedness == "right");
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

TEST_CASE(
    "Profile dimension operators merge v1 and v2 sources while flush keeps abs gain offset order") {
  const auto a = source("A", "mm"), b = source("B");
  for (const auto* unit : {"m", "mm"}) {
    test::OpCall call(dimensionsRegistry());
    attachFlush(call, a, b);
    const auto beforeBase = call.inputs["baseLine"].valueJson();
    REQUIRE_MESSAGE(call.run("metrology.flush_point_line", {{"signed", Value::boolean(false)},
                                                            {"gain", Value::number(-2)},
                                                            {"offsetMm", Value::number(0.5)},
                                                            {"outputUnit", Value::text(unit)}})
                        .ok,
                    call.status.message);
    const auto* measured = call.out("value").asMeasurement();
    REQUIRE(measured);
    CHECK(measured->ok);
    CHECK(measured->unit == unit);
    CHECK(measured->value ==
          doctest::Approx(std::string(unit) == "m" ? -0.0055 : -5.5).epsilon(1e-6));
    CHECK_FALSE(measured->hasLimits);
    CHECK(measured->verdict.empty());
    CHECK(call.out("foot").asPoint2D()->p[0] == 0.004F);
    CHECK(call.out("foot").asPoint2D()->p[1] == 0);
    CHECK(call.out("baseLine").asLine2D()->end[0] == 0.004F);
    CHECK(call.inputs["baseLine"].valueJson() == beforeBase);
    checkContext(call.out("foot"), call.out("footContext"));
    checkContext(call.out("segment"), call.out("segmentContext"));
    checkContext(call.out("baseLine"), call.out("baseLineContext"));
    const auto& quality = call.out("quality").asRecord()->data;
    CHECK(quality["sources"].size() == 2);
    CHECK(quality["valueMm"].get<double>() == doctest::Approx(-5.5).epsilon(1e-6));
  }
}

TEST_CASE(
    "Profile circle gap operators keep tangent and center definitions distinct with declared units") {
  const auto a = source("A", "mm"), b = source("B");
  test::OpCall call(dimensionsRegistry());
  attachGap(call, a, b);
  REQUIRE(call.run("metrology.circle_gap", {{"definition", Value::text("datum_tangents")},
                                            {"offsetMm", Value::number(0.25)},
                                            {"outputUnit", Value::text("mm")}})
              .ok);
  CHECK(call.out("value").asMeasurement()->ok);
  CHECK(call.out("value").asMeasurement()->value == doctest::Approx(7.25).epsilon(1e-6));
  checkContext(call.out("segment"), call.out("segmentContext"));
  REQUIRE(call.run("metrology.circle_gap", {{"definition", Value::text("center_line")},
                                            {"offsetMm", Value::number(0.25)},
                                            {"outputUnit", Value::text("m")}})
              .ok);
  CHECK(call.out("value").asMeasurement()->value ==
        doctest::Approx((std::sqrt(116.) - 3 + 0.25) * 0.001).epsilon(1e-6));
  CHECK(call.out("quality").asRecord()->data["sources"].size() == 2);
  call.inputs.erase("baseLine");
  call.inputs.erase("baseLineContext");
  CHECK(call.validate("metrology.circle_gap", {{"definition", Value::text("datum_tangents")}},
                      {"left", "right", "leftContext", "rightContext"})
            .size() == 2);
  CHECK_FALSE(call.run("metrology.circle_gap", {{"definition", Value::text("datum_tangents")}}).ok);
  CHECK(call.outputs.empty());
}

TEST_CASE(
    "Profile crossed circle gaps reject tangent geometry and preserve invalid NaN center measurements") {
  test::OpCall call(dimensionsRegistry());
  attachGap(call, source("A"), source("B", "mm"), true);
  CHECK_FALSE(call.run("metrology.circle_gap", {{"definition", Value::text("datum_tangents")}}).ok);
  CHECK(call.status.code == "invalid_geometry");
  CHECK(call.outputs.empty());
  REQUIRE_MESSAGE(call.run("metrology.circle_gap", {{"definition", Value::text("center_line")}}).ok,
                  call.status.message);
  const auto* value = call.out("value").asMeasurement();
  REQUIRE(value);
  CHECK_FALSE(value->ok);
  CHECK(std::isnan(value->value));
  CHECK(value->unit == "m");
  CHECK_FALSE(value->message.empty());
  CHECK(value->verdict.empty());
  CHECK_FALSE(value->hasLimits);
  CHECK(call.out("quality").asRecord()->data["crossed"] == true);
  CHECK(call.out("quality").asRecord()->data["valid"] == false);
  CHECK(call.out("quality").asRecord()->data["valueMm"].is_null());
  checkContext(call.out("segment"), call.out("segmentContext"));
}

TEST_CASE(
    "Profile point offset operator preserves gain abs offset order and accepts identical points") {
  const auto a = source("A", "mm"), b = source("B");
  test::OpCall call(dimensionsRegistry());
  attachOffset(call, a, b);
  for (const auto* unit : {"m", "mm"}) {
    REQUIRE(call.run("metrology.point_offset", {{"gainDx", Value::number(-2)},
                                                {"gainDy", Value::number(3)},
                                                {"absDx", Value::boolean(true)},
                                                {"absDy", Value::boolean(true)},
                                                {"dxOffsetMm", Value::number(-0.5)},
                                                {"dyOffsetMm", Value::number(0.25)},
                                                {"outputUnit", Value::text(unit)}})
                .ok);
    const double scale = std::string(unit) == "m" ? 0.001 : 1;
    CHECK(call.out("dx").asMeasurement()->value == doctest::Approx(5.5 * scale).epsilon(1e-6));
    CHECK(call.out("dy").asMeasurement()->value == doctest::Approx(12.25 * scale).epsilon(1e-6));
    CHECK(call.out("distance").asMeasurement()->value == doctest::Approx(5 * scale).epsilon(1e-6));
    CHECK(call.out("distance").asMeasurement()->unit == unit);
    checkContext(call.out("segment"), call.out("segmentContext"));
  }
  attachOffset(call, a, b, true);
  REQUIRE_MESSAGE(call.run("metrology.point_offset").ok, call.status.message);
  for (const auto* port : {"dx", "dy", "distance"}) {
    const auto* value = call.out(port).asMeasurement();
    REQUIRE(value);
    CHECK(value->ok);
    CHECK(value->value == 0);
  }
  const auto* segment = call.out("segment").asLine2D();
  REQUIRE(segment);
  CHECK(segment->hasSegment);
  CHECK(segment->start[0] == segment->end[0]);
  CHECK(segment->start[1] == segment->end[1]);
  checkContext(call.out("segment"), call.out("segmentContext"));
}

TEST_CASE(
    "Profile dimension operators reject mismatched physical spaces and altered geometry before publishing") {
  const auto a = source("A"), b = source("B", "mm");
  for (const auto* mismatch : {"frame", "plane", "handedness"}) {
    const auto different =
        source("B", "mm", std::string(mismatch) == "frame" ? "other" : "measurement",
               std::string(mismatch) == "plane" ? "XZ" : "XY",
               std::string(mismatch) == "handedness" ? "reflected" : "right");
    test::OpCall call(dimensionsRegistry());
    attachFlush(call, a, a);
    call.inputs["refPointContext"] = contextV2(call.inputs["refPoint"], different, different);
    CHECK_FALSE(call.run("metrology.flush_point_line").ok);
    CHECK(call.outputs.empty());
    attachOffset(call, a, a);
    call.inputs["bContext"] = contextV2(call.inputs["b"], different, different);
    CHECK_FALSE(call.run("metrology.point_offset").ok);
    CHECK(call.outputs.empty());
    attachGap(call, a, a);
    call.inputs["rightContext"] = contextV2(call.inputs["right"], different, different);
    CHECK_FALSE(call.run("metrology.circle_gap").ok);
    CHECK(call.outputs.empty());
  }
  test::OpCall call(dimensionsRegistry());
  attachOffset(call, a, b);
  call.inputs["b"] = point(0.9F, 0.8F);
  CHECK_FALSE(call.run("metrology.point_offset").ok);
  CHECK(call.outputs.empty());
  attachGap(call, a, b);
  auto broken = *call.inputs["leftContext"].asRecord();
  broken.data["sources"][0]["profileHash"] = "bad";
  call.inputs["leftContext"] = Data::record(broken);
  CHECK_FALSE(call.run("metrology.circle_gap").ok);
  CHECK(call.outputs.empty());
  attachFlush(call, a, b);
  CHECK_FALSE(call.run("metrology.flush_point_line",
                       {{"gain", Value::number(std::numeric_limits<double>::infinity())}})
                  .ok);
  CHECK(call.outputs.empty());
}

TEST_CASE("Profile dimension operators cancel atomically at their final publication boundary") {
  const auto a = source("A"), b = source("B", "mm");
  for (const auto* id :
       {"metrology.flush_point_line", "metrology.circle_gap", "metrology.point_offset"}) {
    const auto* op = dimensionsRegistry().find(id);
    REQUIRE(op);
    test::OpCall call(dimensionsRegistry());
    if (std::string(id) == "metrology.flush_point_line")
      attachFlush(call, a, b);
    if (std::string(id) == "metrology.circle_gap")
      attachGap(call, a, b);
    if (std::string(id) == "metrology.point_offset")
      attachOffset(call, a, b);
    ParamMap params;
    for (const auto& parameter : op->params)
      params[parameter.name] = parameter.def;
    std::unordered_map<std::string, Data> values;
    Inputs inputs(call.inputs);
    Outputs outputs(values);
    PollContext completed;
    const ParamView view(params, completed.base);
    REQUIRE(op->compute(inputs, view, outputs, completed).ok);
    REQUIRE_FALSE(values.empty());
    PollContext cancelled;
    cancelled.stopAt = completed.calls;
    values.clear();
    CHECK(op->compute(inputs, view, outputs, cancelled).ok);
    CHECK(values.empty());
  }
}
