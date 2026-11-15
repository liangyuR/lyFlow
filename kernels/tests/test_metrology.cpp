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
