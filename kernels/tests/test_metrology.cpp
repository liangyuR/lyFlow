#include <doctest/doctest.h>

#include <cmath>
#include <limits>

#include "lyflow/calibration.h"
#include "lyflow/kernel/metrology.h"

using namespace lyflow;

namespace {
Point2D pixel(float x, float y) {
  Point2D point;
  point.p[0] = x;
  point.p[1] = y;
  point.unit = Unit2D::Pixel;
  return point;
}

Record planeCalibration(const char* unit, double scale) {
  return {calibration::kPlaneType,
          {{"H", {scale, 0., 0., 0., scale, 0., 0., 0., 1.}}, {"unit", unit}}};
}
} // namespace

TEST_CASE("Metrology kernel: calibrated distance preserves legacy units and explicit v1 frames") {
  auto a = pixel(1, 2), b = pixel(4, 6);
  auto millimeters = planeCalibration("mm", 2);
  auto meters = planeCalibration("m", 0.002);
  const auto original = millimeters.data;
  Measurement output;
  std::string why, port;
  REQUIRE(kernel::metrology::calibratedDistance(&millimeters, &a, &b, "", output, why, port));
  CHECK(output.value == doctest::Approx(0.01));
  CHECK(output.ok);
  CHECK(output.unit == "m");
  CHECK_FALSE(output.hasLimits);
  CHECK(output.verdict.empty());
  meters.data["imageFrame"] = "legacy-unversioned-metadata";
  REQUIRE(kernel::metrology::calibratedDistance(&meters, &a, &b, "ignored-legacy-frame", output,
                                                why, port));
  CHECK(output.value == doctest::Approx(0.01));
  Record canonical;
  REQUIRE(calibration::upgradeLegacyPlane(millimeters, "mm", "camera-A", "table", canonical, why));
  REQUIRE(kernel::metrology::calibratedDistance(&canonical, &a, &b, "camera-A", output, why, port));
  CHECK(output.value == doctest::Approx(0.01));
  CHECK(millimeters.data == original);
  CHECK(a.p[0] == 1);
  CHECK(b.p[1] == 6);
  CHECK(why.empty());
  CHECK(port.empty());
  canonical.data["H"] = {2e-40, 0, 0, 0, 2e-40, 0, 0, 0, 1e-40};
  REQUIRE(kernel::metrology::calibratedDistance(&canonical, &a, &b, "camera-A", output, why, port));
  CHECK(output.value == doctest::Approx(0.01));
}

TEST_CASE("Metrology kernel: rejected calibration points and mapping preserve the prior result") {
  auto a = pixel(1, 2), b = pixel(4, 6);
  auto plane = planeCalibration("mm", 2);
  Measurement output;
  output.value = 42;
  output.ok = true;
  output.unit = "sentinel";
  output.message = "keep";
  std::string why, port;
  const auto unchanged = [&] {
    CHECK(output.value == 42);
    CHECK(output.ok);
    CHECK(output.unit == "sentinel");
    CHECK(output.message == "keep");
  };
  CHECK_FALSE(
      kernel::metrology::calibratedDistance(nullptr, nullptr, nullptr, "", output, why, port));
  CHECK(port == "calib");
  CHECK(why == "缺少平面标定");
  unchanged();
  auto wrongType = plane;
  wrongType.type = "other.Record.v1";
  CHECK_FALSE(
      kernel::metrology::calibratedDistance(&wrongType, nullptr, nullptr, "", output, why, port));
  CHECK(port == "calib");
  unchanged();
  Record canonical;
  REQUIRE(calibration::upgradeLegacyPlane(plane, "mm", "camera-A", "table", canonical, why));
  for (const auto* frame : {"", "camera-B"}) {
    CHECK_FALSE(kernel::metrology::calibratedDistance(&canonical, nullptr, nullptr, frame, output,
                                                      why, port));
    CHECK(port == "calib");
    CHECK(why == "版本化标定的图像帧必须匹配量测点的明确声明");
    unchanged();
  }
  CHECK_FALSE(kernel::metrology::calibratedDistance(&plane, nullptr, &b, "", output, why, port));
  CHECK(port == "a");
  unchanged();
  a.unit = Unit2D::Meter;
  CHECK_FALSE(kernel::metrology::calibratedDistance(&plane, &a, &b, "", output, why, port));
  CHECK(why == "物理量测输入必须是原图像素Point2D");
  unchanged();
  a.unit = Unit2D::Pixel;
  a.p[0] = std::numeric_limits<float>::quiet_NaN();
  CHECK_FALSE(kernel::metrology::calibratedDistance(&plane, &a, &b, "", output, why, port));
  CHECK(why == "量测点位于单应性无效区域");
  unchanged();
  a.p[0] = 1;
  plane.data["H"] = {1., 0., 0., 0., 1., 0., 1., 0., -1.};
  CHECK_FALSE(kernel::metrology::calibratedDistance(&plane, &a, &b, "", output, why, port));
  CHECK(port == "a");
  unchanged();
  for (const auto& value : {nlohmann::json("cm"), nlohmann::json(nullptr)}) {
    plane.data["unit"] = value;
    CHECK_FALSE(kernel::metrology::calibratedDistance(&plane, &a, &b, "", output, why, port));
    CHECK(port == "calib");
    unchanged();
  }
  canonical.data["schemaVersion"] = 2;
  CHECK_FALSE(
      kernel::metrology::calibratedDistance(&canonical, &a, &b, "camera-A", output, why, port));
  CHECK(port == "calib");
  unchanged();
}

TEST_CASE("Metrology quality: residual norms retain observation weights and invalid evidence") {
  kernel::metrology::ResidualAccumulator quality;
  CHECK(std::isnan(quality.weightedRmse()));
  CHECK(quality.maxError() == 0);
  quality.add(3, 2);
  quality.add(4, 1);
  CHECK(quality.weightedRmse() == doctest::Approx(std::sqrt(34.0 / 3.0)));
  CHECK(quality.maxError() == 4);
  kernel::metrology::ResidualAccumulator scaledWeights;
  scaledWeights.add(3, 20);
  scaledWeights.add(4, 10);
  CHECK(scaledWeights.weightedRmse() == doctest::Approx(quality.weightedRmse()));
  CHECK(scaledWeights.maxError() == quality.maxError());
  quality.add(std::numeric_limits<double>::infinity(), 1);
  CHECK(std::isinf(quality.weightedRmse()));
  CHECK(std::isinf(quality.maxError()));
  quality.add(std::numeric_limits<double>::quiet_NaN(), 1);
  CHECK(std::isnan(quality.weightedRmse()));
}

TEST_CASE("Metrology sampled gap flush uses observed edges and the common signed ordinate") {
  using namespace kernel::metrology;
  SampledLine left{{0, 1}, {1, 2}, -0.02, -0.013, 0.001};
  SampledLine right{{0, 1.003}, {1, -1}, 0.01, 0.017, 0};
  SampledGapFlush result;
  std::string why;
  REQUIRE(sampledGapFlush(left, right, {0, 0.001, 0.05}, result, why));
  CHECK(result.gap == doctest::Approx(0.023));
  CHECK(result.flush == doctest::Approx(0.003));
  REQUIRE(sampledGapFlush(left, right, {0.001, 0.001, 0.05}, result, why));
  CHECK(result.flush == doctest::Approx(0));
  std::swap(left.center[1], right.center[1]);
  REQUIRE(sampledGapFlush(left, right, {0, 0.001, 0.05}, result, why));
  CHECK(result.flush == doctest::Approx(-0.003));
  CHECK(result.gap == doctest::Approx(0.023));
  CHECK(why.empty());
}

TEST_CASE(
    "Metrology sampled gap flush rejects quality vertical and extrapolation while retaining prior output") {
  using namespace kernel::metrology;
  SampledLine left{{0, 0}, {1, 0}, -2, -1, 0};
  SampledLine right{{0, 1}, {1, 0}, 1, 2, 0};
  SampledGapFlush result{42, -42};
  std::string why;
  const auto saved = [&] {
    CHECK(result.gap == 42);
    CHECK(result.flush == -42);
  };
  CHECK_FALSE(sampledGapFlush(left, right, {0, 0.001, 0}, result, why));
  CHECK(why == "拟合质量、竖直线或外推距离不满足门限");
  saved();
  left.rms = 0.002;
  CHECK_FALSE(sampledGapFlush(left, right, {0, 0.001, 1}, result, why));
  saved();
  left.rms = 0;
  left.direction = {0, 1};
  CHECK_FALSE(sampledGapFlush(left, right, {0, 0.001, 1}, result, why));
  saved();
  left.direction = {1e-8, 1};
  REQUIRE(sampledGapFlush(left, right, {0, 0.001, 1}, result, why));
  CHECK(result.flush == 1);
  result = {42, -42};
  left.direction[0] = 0.999e-8;
  CHECK_FALSE(sampledGapFlush(left, right, {0, 0.001, 1}, result, why));
  saved();
  left.direction = {1, 0};
  for (const auto bad : {SampledGapFlushOptions{0, 0, 1},
                         {0, 0.001, -1},
                         {std::numeric_limits<double>::quiet_NaN(), 0.001, 1}}) {
    CHECK_FALSE(sampledGapFlush(left, right, bad, result, why));
    saved();
  }
  left.center[0] = std::numeric_limits<double>::infinity();
  CHECK_FALSE(sampledGapFlush(left, right, {0, 0.001, 1}, result, why));
  saved();
  left.center = {0, std::numeric_limits<double>::max()};
  right.center = {0, -std::numeric_limits<double>::max()};
  CHECK_FALSE(sampledGapFlush(left, right, {0, 0.001, 1}, result, why));
  CHECK(why == "剖面计算超出范围");
  saved();
}

TEST_CASE(
    "Metrology signed point line normalizes generic directions and keeps explicit sign independent of line reversal") {
  using namespace kernel::metrology;
  SignedPointLine result;
  std::string why;
  REQUIRE(signedPointLine({3, 4}, {1, 1}, {2, 0}, {0, 1}, DirectionPolicy::Normalize, result, why));
  CHECK(result.distance == 3);
  CHECK(result.foot == std::array<double, 2>{3, 1});
  const auto forward = result;
  REQUIRE(
      signedPointLine({3, 4}, {1, 1}, {-2, 0}, {0, 1}, DirectionPolicy::Normalize, result, why));
  CHECK(result.distance == forward.distance);
  CHECK(result.foot == forward.foot);
  REQUIRE(
      signedPointLine({3, 4}, {1, 1}, {2, 0}, {0, -1}, DirectionPolicy::Normalize, result, why));
  CHECK(result.distance == -forward.distance);
  CHECK(result.foot == forward.foot);
  REQUIRE(signedPointLine({3, 4}, {1, 0}, {0, 2}, {1, 0}, DirectionPolicy::Normalize, result, why));
  CHECK(result.distance == 2);
  CHECK(result.foot == std::array<double, 2>{1, 4});
  const double maximum = std::numeric_limits<double>::max();
  REQUIRE(signedPointLine({3, 4}, {1, 1}, {2, 0}, {maximum, maximum}, DirectionPolicy::Normalize,
                          result, why));
  CHECK(result.distance == 3);
}

TEST_CASE(
    "Metrology declared unit direction retains legacy DTS float normal multiplication and all business signs") {
  using namespace kernel::metrology;
  for (float directionSign : {1.f, -1.f}) {
    const double dx = static_cast<double>(0.6f * directionSign);
    const double dy = static_cast<double>(0.8f * directionSign);
    const double nx = dx >= 0 ? -dy : dy, ny = dx >= 0 ? dx : -dx;
    const std::array<double, 2> point{static_cast<double>(21.7f), static_cast<double>(7.2f)};
    const std::array<double, 2> anchor{static_cast<double>(3.1f), static_cast<double>(2.8f)};
    const double oldSigned = (point[0] - anchor[0]) * nx + (point[1] - anchor[1]) * ny;
    const std::array<double, 2> oldFoot{point[0] - oldSigned * nx, point[1] - oldSigned * ny};
    for (double bulge : {1., -1.}) {
      for (double deeper : {1., -1.}) {
        const double toward = bulge == 1 ? oldSigned : -oldSigned;
        const double flush = deeper == 1 ? -toward : toward;
        const double sign = bulge * -deeper;
        SignedPointLine actual;
        std::string why;
        REQUIRE(signedPointLine(point, anchor, {dx, dy}, {sign * nx, sign * ny},
                                DirectionPolicy::DeclaredUnit, actual, why));
        CHECK(actual.distance == flush);
        CHECK(actual.foot == oldFoot);
      }
    }
    SignedPointLine declared, normalized;
    std::string why;
    REQUIRE(signedPointLine(point, anchor, {dx, dy}, {nx, ny}, DirectionPolicy::DeclaredUnit,
                            declared, why));
    REQUIRE(signedPointLine(point, anchor, {dx, dy}, {nx, ny}, DirectionPolicy::Normalize,
                            normalized, why));
    CHECK(declared.distance != normalized.distance);
    CHECK(declared.foot != normalized.foot);
  }
}

TEST_CASE(
    "Metrology signed point line rejects invalid directions references nonfinite values and overflow atomically") {
  using namespace kernel::metrology;
  SignedPointLine result{42, {-42, 19}};
  std::string why;
  const auto unchanged = [&] {
    CHECK(result.distance == 42);
    CHECK(result.foot == std::array<double, 2>{-42, 19});
  };
  CHECK_FALSE(
      signedPointLine({0, 1}, {0, 0}, {0, 0}, {0, 1}, DirectionPolicy::Normalize, result, why));
  unchanged();
  CHECK_FALSE(
      signedPointLine({0, 1}, {0, 0}, {1, 0}, {0, 0}, DirectionPolicy::Normalize, result, why));
  unchanged();
  CHECK_FALSE(
      signedPointLine({0, 1}, {0, 0}, {1, 0}, {1, 0}, DirectionPolicy::Normalize, result, why));
  unchanged();
  CHECK_FALSE(
      signedPointLine({0, 1}, {0, 0}, {2, 0}, {0, 1}, DirectionPolicy::DeclaredUnit, result, why));
  unchanged();
  const double nan = std::numeric_limits<double>::quiet_NaN();
  CHECK_FALSE(
      signedPointLine({nan, 1}, {0, 0}, {1, 0}, {0, 1}, DirectionPolicy::Normalize, result, why));
  unchanged();
  CHECK_FALSE(
      signedPointLine({0, 1}, {nan, 0}, {1, 0}, {0, 1}, DirectionPolicy::Normalize, result, why));
  unchanged();
  CHECK_FALSE(
      signedPointLine({0, 1}, {0, 0}, {1, nan}, {0, 1}, DirectionPolicy::Normalize, result, why));
  unchanged();
  CHECK_FALSE(
      signedPointLine({0, 1}, {0, 0}, {1, 0}, {0, nan}, DirectionPolicy::Normalize, result, why));
  unchanged();
  const double maximum = std::numeric_limits<double>::max();
  CHECK_FALSE(signedPointLine({maximum, maximum}, {-maximum, maximum}, {1, 1}, {0, 1},
                              DirectionPolicy::Normalize, result, why));
  unchanged();
  CHECK_FALSE(signedPointLine({0, 1}, {0, 0}, {maximum, maximum}, {0, 1},
                              DirectionPolicy::Normalize, result, why));
  unchanged();
  CHECK_FALSE(signedPointLine({0, 1}, {0, 0}, {1, 0}, {0, 1}, static_cast<DirectionPolicy>(99),
                              result, why));
  unchanged();
}
