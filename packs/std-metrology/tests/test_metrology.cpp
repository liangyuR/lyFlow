#include <doctest/doctest.h>

#include "helpers.h"
#include "lyflow/calibration.h"

using namespace lyflow;

TEST_CASE(
    "Portable calibrated distance: moved operator retains ID version units and frame contract") {
  test::OpCall call;
  const auto* descriptor = lyflow::ensureRegistry().find("image.calibrated_distance");
  REQUIRE(descriptor);
  CHECK(descriptor->version == "1.1.0");
  REQUIRE(descriptor->params.size() == 1);
  CHECK(descriptor->params[0].name == "imageFrame");
  Record plane{calibration::kPlaneType,
               {{"H", {2., 0., 0., 0., 2., 0., 0., 0., 1.}}, {"unit", "mm"}}};
  Point2D a, b;
  a.p[0] = 1;
  a.p[1] = 2;
  a.unit = Unit2D::Pixel;
  b.p[0] = 4;
  b.p[1] = 6;
  b.unit = Unit2D::Pixel;
  call.inputs = {{"a", Data::point2d(a)}, {"b", Data::point2d(b)}, {"calib", Data::record(plane)}};
  REQUIRE(call.run("image.calibrated_distance").ok);
  REQUIRE(call.out("distance").asMeasurement());
  CHECK(call.out("distance").asMeasurement()->value == doctest::Approx(0.01));
  CHECK(call.out("distance").asMeasurement()->unit == "m");
  Record canonical;
  std::string why;
  REQUIRE(calibration::upgradeLegacyPlane(plane, "mm", "camera-A", "table", canonical, why));
  call.inputs["calib"] = Data::record(canonical);
  CHECK_FALSE(call.run("image.calibrated_distance").ok);
  CHECK(call.outputs.empty());
  CHECK(call.status.portName == "calib");
  CHECK_FALSE(call.run("image.calibrated_distance", {{"imageFrame", Value::text("camera-B")}}).ok);
  REQUIRE(call.run("image.calibrated_distance", {{"imageFrame", Value::text("camera-A")}}).ok);
  CHECK(call.out("distance").asMeasurement()->value == doctest::Approx(0.01));
  CHECK(canonical.data["imageFrame"] == "camera-A");
  a.unit = Unit2D::Meter;
  call.inputs["a"] = Data::point2d(a);
  CHECK_FALSE(call.run("image.calibrated_distance", {{"imageFrame", Value::text("camera-A")}}).ok);
  CHECK(call.status.code == "bad_input");
  CHECK(call.status.portName == "a");
  CHECK(call.outputs.empty());
}
