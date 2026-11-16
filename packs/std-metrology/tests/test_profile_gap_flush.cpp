#include <doctest/doctest.h>

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
  PointCloud large;
  for (float x : {0.0f, 8.0f, 24.0f})
    large.push(x, 0, 1e8f + x);
  for (float x : {40.0f, 48.0f, 64.0f})
    large.push(x, 0, 1e8f + x);
  call.inputs = {{"cloud", Data::cloud(large)}};
  CHECK_FALSE(call.run("profile.gap_flush", {{"leftRange", Value::vec({0, 25})},
                                             {"rightRange", Value::vec({39, 65})},
                                             {"referenceX", Value::number(30)},
                                             {"maxExtrapolation", Value::number(30)}})
                  .ok);
}
