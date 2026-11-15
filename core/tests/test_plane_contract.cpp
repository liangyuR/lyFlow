#include <doctest/doctest.h>

#include "lyflow/calibration.h"

using namespace lyflow;

TEST_CASE("PlaneCalib: explicit legacy upgrade is unit-safe, normalized and idempotent") {
  Record old;
  old.type = calibration::kPlaneType;
  old.data = {{"H", {2, 0, 0, 0, 2, 0, 0, 0, 4}}};
  Record upgraded;
  std::string why;
  CHECK_FALSE(calibration::upgradeLegacyPlane(old, "", "image", "table", upgraded, why));
  REQUIRE(calibration::upgradeLegacyPlane(old, "mm", "image", "table", upgraded, why));
  CHECK_FALSE(old.data.contains("unit"));
  calibration::Plane plane;
  REQUIRE(calibration::parsePlane(upgraded, plane, why));
  double x = 0, y = 0;
  REQUIRE(plane.map(20, 40, x, y));
  CHECK(x == doctest::Approx(10));
  CHECK(y == doctest::Approx(20));
  CHECK(x * plane.metersPerUnit == doctest::Approx(0.01));
  CHECK(plane.imageFrame == "image");
  CHECK(plane.planeFrame == "table");
  Record again;
  REQUIRE(calibration::upgradeLegacyPlane(upgraded, "m", "image", "table", again, why));
  CHECK(again.data == upgraded.data);
  CHECK_FALSE(calibration::upgradeLegacyPlane(upgraded, "mm", "another", "table", again, why));
  old.data["unit"] = "cm";
  CHECK_FALSE(calibration::upgradeLegacyPlane(old, "mm", "image", "table", again, why));
  upgraded.data["schemaVersion"] = 2;
  CHECK_FALSE(calibration::parsePlane(upgraded, plane, why));
}
