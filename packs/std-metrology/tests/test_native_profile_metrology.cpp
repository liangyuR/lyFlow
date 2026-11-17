#include <doctest/doctest.h>

#include <limits>

#include "helpers.h"
#include "lyflow/profile.h"
#include "lyflow_profile/context.h"

using namespace lyflow;
namespace {
Data profile(bool right, const char* frame = "measurement", const char* handedness = "right") {
  PointCloud cloud;
  for (int i = 0; i < 4; ++i)
    cloud.push(right ? 10.f + 2.f * i : -20.f + 2.f * i, 0, right ? 3.f : 0.f);
  Profile input;
  std::string why;
  REQUIRE(makeProfile(std::make_shared<const PointCloud>(cloud),
                      {1, frame, "mm", "XZ", handedness, "acquisition", right ? "R" : "L", 4},
                      {1, 1, 1, 1}, {0, 1, 2, 3}, input, why));
  return Data::profile(input);
}
std::pair<Data, Data> sampledLineFixture(const Data& source) {
  const auto* input = source.asProfile();
  REQUIRE(input);
  Profile metric;
  std::string why;
  REQUIRE(convertProfileUnit(*input, "m", metric, why));
  REQUIRE(metric.pointCount() == 4);
  const auto& xyz = metric.cloud->xyz;
  Line2D line;
  line.unit = Unit2D::Meter;
  line.point[0] = (xyz[0] + xyz[9]) * 0.5f;
  line.point[1] = xyz[2];
  line.dir[0] = 1;
  line.dir[1] = 0;
  const auto geometry = Data::line2d(line);
  Record statistics{"profile.LineFit.v2",
                    {{"schemaVersion", 2},
                     {"frame", input->descriptor.frame},
                     {"plane", input->descriptor.plane},
                     {"handedness", input->descriptor.handedness},
                     {"count", 4},
                     {"rms", 0.0},
                     {"abscissaRange", {xyz[0], xyz[9]}}}};
  return {geometry, Data::record(std::move(statistics))};
}
std::unordered_map<std::string, Data> sampledFixtures(const Data& left, const Data& right) {
  const auto l = sampledLineFixture(left), r = sampledLineFixture(right);
  return {{"left", l.first},
          {"leftStatistics", l.second},
          {"right", r.first},
          {"rightStatistics", r.second}};
}
}

TEST_CASE(
    "Native profile metrology consumes explicit sampled line fixtures with legacy gap flush equivalence") {
  const auto left = profile(false), right = profile(true);
  test::OpCall call;
  call.inputs = sampledFixtures(left, right);
  REQUIRE_MESSAGE(call.run("metrology.profile_gap_flush").ok, call.status.message);
  const auto gap = *call.out("gap").asMeasurement(), flush = *call.out("flush").asMeasurement();
  CHECK(gap.value == doctest::Approx(0.024).epsilon(1e-6));
  CHECK(flush.value == doctest::Approx(0.003));
  CHECK(gap.unit == "m");
  CHECK_FALSE(gap.hasLimits);
  CHECK(flush.verdict.empty());
  const auto quality = call.out("quality").asRecord();
  CHECK(quality->type == "metrology.ProfileGapFlush.v2");
  PointCloud cloud;
  for (const auto& input : {left, right}) {
    Profile metric;
    std::string why;
    REQUIRE(convertProfileUnit(*input.asProfile(), "m", metric, why));
    cloud.xyz.insert(cloud.xyz.end(), metric.cloud->xyz.begin(), metric.cloud->xyz.end());
  }
  call.inputs = {{"cloud", Data::cloud(cloud)}};
  REQUIRE(call.run("profile.gap_flush").ok);
  CHECK(call.out("gap").asMeasurement()->value == doctest::Approx(gap.value).epsilon(1e-12));
  CHECK(call.out("flush").asMeasurement()->value == doctest::Approx(flush.value).epsilon(1e-12));
}

TEST_CASE(
    "Native profile metrology rejects mismatched declarations statistics and unsupported extrapolation") {
  test::OpCall call;
  const auto good = sampledFixtures(profile(false), profile(true));
  call.inputs = good;
  CHECK_FALSE(call.run("metrology.profile_gap_flush", {{"maxExtrapolation", Value::number(0)}}).ok);
  auto line = *good.at("right").asLine2D();
  line.unit = Unit2D::Pixel;
  call.inputs = good;
  call.inputs["right"] = Data::line2d(line);
  CHECK_FALSE(call.run("metrology.profile_gap_flush").ok);
  for (const auto* key : {"schemaVersion", "handedness", "count"}) {
    auto statistics = *good.at("rightStatistics").asRecord();
    statistics.data[key] = std::string(key) == "handedness" ? nlohmann::json(1) : "invalid";
    call.inputs = good;
    call.inputs["rightStatistics"] = Data::record(statistics);
    CHECK_FALSE(call.run("metrology.profile_gap_flush").ok);
  }
  call.inputs = sampledFixtures(profile(false), profile(true, "other-frame"));
  CHECK_FALSE(call.run("metrology.profile_gap_flush").ok);
  call.inputs = sampledFixtures(profile(false), profile(true, "measurement", "reflected"));
  CHECK_FALSE(call.run("metrology.profile_gap_flush").ok);
  auto malformed = *good.at("rightStatistics").asRecord();
  malformed.data["rms"] = -1;
  call.inputs = good;
  call.inputs["rightStatistics"] = Data::record(malformed);
  CHECK_FALSE(call.run("metrology.profile_gap_flush").ok);
}

TEST_CASE(
    "Native signed point line uses paired SI geometry contexts normalizes directions and returns meter foot") {
  Point2D point;
  point.p[0] = 0.002f;
  point.p[1] = 0.003f;
  Line2D line;
  line.dir[0] = 2;
  const auto pointData = Data::point2d(point), lineData = Data::line2d(line);
  const auto pointContext =
      Data::record(profile_contract::geometryContext(profile(false).asProfile()->descriptor));
  const auto lineContext =
      Data::record(profile_contract::geometryContext(profile(true).asProfile()->descriptor));
  test::OpCall call;
  call.inputs = {{"point", pointData},
                 {"line", lineData},
                 {"pointContext", pointContext},
                 {"lineContext", lineContext}};
  REQUIRE_MESSAGE(call.run("metrology.signed_point_line").ok, call.status.message);
  const auto* measurement = call.out("distance").asMeasurement();
  const auto* foot = call.out("foot").asPoint2D();
  REQUIRE(measurement);
  REQUIRE(foot);
  CHECK(measurement->value == doctest::Approx(0.003));
  CHECK(measurement->unit == "m");
  CHECK(measurement->ok);
  CHECK_FALSE(measurement->hasLimits);
  CHECK(foot->unit == Unit2D::Meter);
  CHECK(foot->p[0] == point.p[0]);
  CHECK(foot->p[1] == 0);
  REQUIRE(call.run("metrology.signed_point_line", {{"positiveNormal", Value::vec({0, -1})}}).ok);
  CHECK(call.out("distance").asMeasurement()->value == doctest::Approx(-0.003));
  CHECK(call.out("foot").asPoint2D()->p[1] == 0);
}

TEST_CASE(
    "Native signed point line rejects mixed frames units references and unrepresentable foot") {
  Point2D point;
  point.p[1] = 3;
  Line2D line;
  const auto pointData = Data::point2d(point), lineData = Data::line2d(line);
  const auto pointSource = profile(false), lineSource = profile(true);
  const auto context = [](const Data& source) {
    return Data::record(profile_contract::geometryContext(source.asProfile()->descriptor));
  };
  const std::unordered_map<std::string, Data> good{{"point", pointData},
                                                   {"line", lineData},
                                                   {"pointContext", context(pointSource)},
                                                   {"lineContext", context(lineSource)}};
  test::OpCall call;
  call.inputs = good;
  CHECK_FALSE(call.run("metrology.signed_point_line", {{"positiveNormal", Value::vec({1, 0})}}).ok);
  CHECK_FALSE(call.run("metrology.signed_point_line", {{"positiveNormal", Value::vec({0, 0})}}).ok);
  call.inputs = good;
  call.inputs["lineContext"] = context(profile(true, "other-frame"));
  CHECK_FALSE(call.run("metrology.signed_point_line").ok);
  call.inputs["lineContext"] = context(profile(true, "measurement", "reflected"));
  CHECK_FALSE(call.run("metrology.signed_point_line").ok);
  auto declared = *good.at("pointContext").asRecord();
  declared.data["geometryUnit"] = "mm";
  call.inputs = good;
  call.inputs["pointContext"] = Data::record(declared);
  CHECK_FALSE(call.run("metrology.signed_point_line").ok);
  point.unit = Unit2D::Pixel;
  call.inputs = good;
  call.inputs["point"] = Data::point2d(point);
  CHECK_FALSE(call.run("metrology.signed_point_line").ok);
  point.unit = Unit2D::Meter;
  point.p[0] = std::numeric_limits<float>::max();
  point.p[1] = std::numeric_limits<float>::max();
  line.point[0] = -point.p[0];
  line.point[1] = point.p[1];
  line.dir[0] = 1;
  line.dir[1] = 1;
  const auto largePoint = Data::point2d(point), largeLine = Data::line2d(line);
  call.inputs = {{"point", largePoint},
                 {"line", largeLine},
                 {"pointContext", context(pointSource)},
                 {"lineContext", context(lineSource)}};
  CHECK_FALSE(call.run("metrology.signed_point_line").ok);
  CHECK(call.status.message == "垂足超出Point2D的float32范围");
  CHECK(call.outputs.empty());
}
