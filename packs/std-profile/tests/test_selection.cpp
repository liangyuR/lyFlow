#include <doctest/doctest.h>

#include <cmath>
#include <limits>

#include "helpers.h"
#include "lyflow_profile/context.h"

using namespace lyflow;

namespace {
Data fixture(const char* frame = "measurement", const char* plane = "XZ",
             const char* handedness = "right") {
  PointCloud cloud;
  cloud.push(0, 9, 0);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  cloud.push(nan, nan, nan);
  cloud.push(2, 9, 1);
  cloud.push(4, 9, 3);
  cloud.intensity = {10, 11, 12, 13};
  cloud.normals = {0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0};
  cloud.rgb = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
  Profile profile;
  std::string why;
  REQUIRE(makeProfile(std::make_shared<const PointCloud>(cloud),
                      {1, frame, "mm", plane, handedness, "acquisition", "scan-A", 20},
                      {1, 0, 0, 1}, {2, 4, 8, 13}, profile, why));
  return Data::profile(std::move(profile));
}
Data point(float x, float y) {
  Point2D value;
  value.p[0] = x;
  value.p[1] = y;
  return Data::point2d(value);
}
Data declared(const Data& profile, const Data& geometry) {
  return Data::record(Record{"profile.Geometry.v2",
                             profile_contract::geometryContextV2(profile, geometry, "declared")});
}
struct PollContext : ExecContext {
  mutable int calls = 0;
  int stopAt = std::numeric_limits<int>::max();
  std::filesystem::path base;
  bool cancelled() const override { return ++calls >= stopAt; }
  void progress(float, std::string_view) override {}
  void log(LogLevel, std::string) override {}
  const std::filesystem::path& baseDir() const override { return base; }
  int threadBudget() const override { return 1; }
};
}

TEST_CASE("Profile geometry declarations require explicit matching frame and plane") {
  const auto source = fixture(), anchor = point(.004F, .003F);
  test::OpCall call;
  call.inputs = {{"profile", source}, {"geometry", anchor}};
  CHECK_FALSE(call.run("profile.bind_geometry").ok);
  CHECK(call.outputs.empty());
  CHECK_FALSE(call.run("profile.bind_geometry",
                       {{"frame", Value::text("measurement")}, {"plane", Value::text("XY")}})
                  .ok);
  CHECK(call.outputs.empty());
  REQUIRE(call.run("profile.bind_geometry",
                   {{"frame", Value::text("measurement")}, {"plane", Value::text("XZ")}})
              .ok);
  CHECK(call.out("context").asRecord()->data["relation"] == "declared");
  CHECK(profile_contract::geometryHashV2(call.out("geometry")) ==
        profile_contract::geometryHashV2(anchor));
  ProfileDescriptor descriptor;
  std::string why;
  CHECK(profile_contract::geometryMatchesPhysical(anchor, call.out("context").asRecord(),
                                                  descriptor, why));
  auto pixel = *anchor.asPoint2D();
  pixel.unit = Unit2D::Pixel;
  call.inputs["geometry"] = Data::point2d(pixel);
  CHECK_FALSE(call.run("profile.bind_geometry",
                       {{"frame", Value::text("measurement")}, {"plane", Value::text("XZ")}})
                  .ok);
  CHECK(call.outputs.empty());
}

TEST_CASE("Profile nearest samples retain original slot and physical source index") {
  const auto source = fixture(), anchor = point(.0038F, .0029F);
  const auto before = profile_contract::context(source);
  test::OpCall call;
  call.inputs = {
      {"profile", source}, {"anchor", anchor}, {"anchorContext", declared(source, anchor)}};
  REQUIRE(call.run("profile.nearest_sample").ok);
  CHECK(call.out("point").asPoint2D()->unit == Unit2D::Meter);
  CHECK(call.out("point").asPoint2D()->p[0] == doctest::Approx(.004));
  CHECK(call.out("point").asPoint2D()->p[1] == doctest::Approx(.003));
  CHECK(call.out("selection").asRecord()->data["slotIndex"] == 3);
  CHECK(call.out("selection").asRecord()->data["sourceIndex"] == 13);
  CHECK(call.out("profile").profilePtr() == source.profilePtr());
  CHECK(profile_contract::context(source) == before);
  CHECK(call.out("selection").asRecord()->data.dump().size() < 1024);
  const auto midpoint = point(.002F, .0015F);
  call.inputs["anchor"] = midpoint;
  call.inputs["anchorContext"] = declared(source, midpoint);
  REQUIRE(call.run("profile.nearest_sample").ok);
  CHECK(call.out("selection").asRecord()->data["slotIndex"] == 0);
  call.inputs["anchorContext"] = declared(source, anchor);
  auto changed = *anchor.asPoint2D();
  changed.p[0] += .01F;
  call.inputs["anchor"] = Data::point2d(changed);
  CHECK_FALSE(call.run("profile.nearest_sample").ok);
  CHECK(call.outputs.empty());
  for (const auto& other : {fixture("other"), fixture("measurement", "XY"),
                            fixture("measurement", "XZ", "reflected")}) {
    call.inputs["anchor"] = anchor;
    call.inputs["anchorContext"] = declared(other, anchor);
    CHECK_FALSE(call.run("profile.nearest_sample").ok);
    CHECK(call.outputs.empty());
  }
}

TEST_CASE(
    "Profile nearest sample retains finite ordering when float32 squared distances overflow") {
  PointCloud cloud;
  cloud.push(3e38F, 0, 0);
  cloud.push(1e38F, 0, 0);
  Profile profile;
  std::string why;
  REQUIRE(makeProfile(std::make_shared<const PointCloud>(cloud),
                      {1, "large", "m", "XY", "right", "acquisition", "large-scan", 9}, {1, 1},
                      {3, 7}, profile, why));
  const auto source = Data::profile(std::move(profile)), anchor = point(-3e38F, 0);
  test::OpCall call;
  call.inputs = {
      {"profile", source}, {"anchor", anchor}, {"anchorContext", declared(source, anchor)}};
  REQUIRE(call.run("profile.nearest_sample").ok);
  CHECK(call.out("selection").asRecord()->data["slotIndex"] == 1);
  CHECK(call.out("selection").asRecord()->data["sourceIndex"] == 7);
  CHECK(std::isfinite(call.out("selection").asRecord()->data["distance"].get<double>()));
}

TEST_CASE("Profile nearest to a line uses the infinite line and skips masked samples") {
  const auto source = fixture();
  Line2D line;
  line.dir[0] = 1;
  line.dir[1] = 0;
  line.hasSegment = true;
  line.start[0] = 100;
  line.end[0] = 101;
  const auto geometry = Data::line2d(line);
  test::OpCall call;
  call.inputs = {
      {"profile", source},
      {"line", geometry},
      {"lineContext", Data::record(Record{"profile.Geometry.v1",
                                          profile_contract::geometryContext(source, geometry)})}};
  REQUIRE(call.run("profile.nearest_to_line").ok);
  CHECK(call.out("selection").asRecord()->data["slotIndex"] == 0);
  CHECK(call.out("selection").asRecord()->data["sourceIndex"] == 2);
  CHECK(call.out("selection").asRecord()->data["distance"] == 0);
  auto masked = *source.asProfile();
  masked.validity[0] = 0;
  const auto onlyLast = Data::profile(std::move(masked));
  call.inputs = {
      {"profile", onlyLast}, {"line", geometry}, {"lineContext", declared(onlyLast, geometry)}};
  REQUIRE(call.run("profile.nearest_to_line").ok);
  CHECK(call.out("selection").asRecord()->data["slotIndex"] == 3);
}

TEST_CASE("Profile validity filtering preserves channels and exposes native input slot indices") {
  const auto source = fixture();
  const auto before = profile_contract::context(source);
  test::OpCall call;
  call.inputs = {{"profile", source}};
  REQUIRE(call.run("profile.filter_validity").ok);
  CHECK(call.out("profile").profilePtr() == source.profilePtr());
  CHECK(call.out("selection").asRecord()->data["invalidCount"] == 2);
  CHECK(call.out("selection").asRecord()->data["removedCount"] == 0);
  REQUIRE(call.run("profile.filter_validity", {{"mode", Value::text("compact")}}).ok);
  const auto* compact = call.out("profile").asProfile();
  REQUIRE(compact);
  CHECK(compact->pointCount() == 2);
  CHECK(compact->descriptor.unit == "mm");
  CHECK(compact->descriptor.sourceCount == 20);
  CHECK(compact->sourceIndices == std::vector<std::uint32_t>{2, 13});
  CHECK(compact->validity == std::vector<std::uint8_t>{1, 1});
  CHECK(compact->cloud->intensity == std::vector<float>{10, 13});
  CHECK(compact->cloud->rgb == std::vector<std::uint8_t>{1, 2, 3, 10, 11, 12});
  CHECK(compact->cloud->normals == std::vector<float>{0, 1, 0, 0, 1, 0});
  CHECK(call.out("valid").asIndices()->sourceCloudId == source.asProfile()->cloud->id);
  CHECK(call.out("valid").asIndices()->values == std::vector<std::int32_t>{0, 3});
  CHECK(call.out("invalid").asIndices()->values == std::vector<std::int32_t>{1, 2});
  CHECK(call.out("selection").asRecord()->data["removedCount"] == 2);
  CHECK(call.out("selection").asRecord()->data.dump().size() < 2048);
  CHECK(profile_contract::context(source) == before);
}

TEST_CASE(
    "Profile selection reports no result for all invalid input and never publishes on cancellation") {
  auto invalid = *fixture().asProfile();
  invalid.validity.assign(invalid.pointCount(), 0);
  const auto source = Data::profile(std::move(invalid)), anchor = point(0, 0);
  test::OpCall call;
  call.inputs = {
      {"profile", source}, {"anchor", anchor}, {"anchorContext", declared(source, anchor)}};
  CHECK_FALSE(call.run("profile.nearest_sample").ok);
  CHECK(call.status.code == "no_result");
  CHECK(call.outputs.empty());
  call.inputs = {{"profile", source}};
  REQUIRE(call.run("profile.filter_validity", {{"mode", Value::text("compact")}}).ok);
  CHECK(call.out("profile").asProfile()->pointCount() == 0);
  for (const char* id :
       {"profile.bind_geometry", "profile.nearest_sample", "profile.filter_validity"}) {
    const auto* op = call.registry->find(id);
    REQUIRE(op);
    ParamMap params;
    for (const auto& p : op->params)
      params[p.name] = p.def;
    params["frame"] = Value::text("measurement");
    params["plane"] = Value::text("XZ");
    const auto validSource = fixture();
    std::unordered_map<std::string, Data> in{{"profile", validSource},
                                             {"geometry", anchor},
                                             {"anchor", anchor},
                                             {"anchorContext", declared(validSource, anchor)}};
    Inputs inputs(in);
    std::unordered_map<std::string, Data> values;
    Outputs outputs(values);
    PollContext completed;
    REQUIRE(op->compute(inputs, ParamView(params, completed.base), outputs, completed).ok);
    REQUIRE_FALSE(values.empty());
    values.clear();
    PollContext cancelled;
    cancelled.stopAt = completed.calls;
    CHECK_FALSE(op->compute(inputs, ParamView(params, cancelled.base), outputs, cancelled).ok);
    CHECK(values.empty());
  }
}
