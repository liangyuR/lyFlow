#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <limits>

#include "helpers.h"

using namespace lyflow;

namespace {
const nlohmann::json& data(const test::OpCall& call, const char* port) {
  const auto* record = call.outputs.at(port).asRecord();
  REQUIRE(record != nullptr);
  return record->data;
}
}

TEST_CASE("P1 single-profile TLS uses delivered float32 line quality") {
  test::OpCall call;
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
  const double ax = nativeLine.point[0], ay = nativeLine.point[1];
  const double dx = nativeLine.dir[0], dy = nativeLine.dir[1];
  double squared = 0;
  for (std::size_t i = 0; i < large.pointCount(); ++i) {
    const double x = large.xyz[i * 3] - ax, y = large.xyz[i * 3 + 2] - ay;
    const double cross = x * dy - y * dx;
    squared += cross * cross / (dx * dx + dy * dy);
  }
  CHECK(data(call, "statistics")["rms"].get<double>() ==
        doctest::Approx(std::sqrt(squared / large.pointCount())).epsilon(1e-12));
  CHECK(data(call, "statistics")["rms"].get<double>() > 1);
}

TEST_CASE("P1 profile projector composes explicit extrinsics without mutating inputs") {
  PointCloud input;
  for (const auto& point : {std::array<float, 3>{0, 0, 0},
                            {1, 0, 0},
                            {0, 2, 0},
                            {0, 0, 3},
                            {1, 2, 3},
                            {-1, 0.5f, 0.7f}}) {
    input.push(point[0], point[1], point[2]);
    input.intensity.push_back(static_cast<float>(input.pointCount()));
    input.normals.insert(input.normals.end(), {0, 0, 1});
    input.rgb.insert(input.rgb.end(), {1, 2, 3});
  }
  Transform master, slave, common;
  master.m[3] = 1;
  slave.m[11] = 2;
  const double c = std::cos(0.5), s = std::sin(0.5);
  common.m[0] = static_cast<float>(c);
  common.m[2] = static_cast<float>(s);
  common.m[8] = static_cast<float>(-s);
  common.m[10] = static_cast<float>(c);
  test::OpCall call;
  call.inputs = {{"master", Data::cloud(input)},
                 {"slave", Data::cloud(input)},
                 {"masterToCommon", Data::transform(master)},
                 {"slaveToCommon", Data::transform(slave)},
                 {"commonToOutput", Data::transform(common)}};
  REQUIRE(call.run("profile.project_pair", {{"outputFrame", Value::text("measurement")}}).ok);
  const auto* actual = call.outputs.at("slave").asCloud();
  for (std::size_t i = 0; i < input.pointCount(); ++i) {
    CHECK(actual->xyz[i * 3] ==
          doctest::Approx(c * input.xyz[i * 3] + s * (input.xyz[i * 3 + 2] + 2)));
    CHECK(actual->xyz[i * 3 + 1] == doctest::Approx(input.xyz[i * 3 + 1]));
    CHECK(actual->xyz[i * 3 + 2] ==
          doctest::Approx(-s * input.xyz[i * 3] + c * (input.xyz[i * 3 + 2] + 2)));
  }
  CHECK(actual->rgb == input.rgb);
  CHECK(actual->intensity == input.intensity);
  CHECK(call.inputs.at("slave").asCloud()->xyz == input.xyz);
  call.inputs.erase("commonToOutput");
  CHECK_FALSE(call.run("profile.project_pair", {{"outputFrame", Value::text("measurement")}}).ok);
}
