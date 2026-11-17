#include <doctest/doctest.h>

#include <limits>
#include <vector>

#include "lyflow_profile/context.h"

using namespace lyflow;

namespace {
ProfileDescriptor space() {
  return {1, "measurement", "m", "XY", "right", "acquisition", "scan-A", 8};
}
std::vector<Data> shapes() {
  Point2D point;
  point.p[0] = 0.01F;
  point.p[1] = 0.02F;
  Line2D line;
  line.point[0] = 0.01F;
  line.point[1] = 0.02F;
  line.dir[0] = 1;
  line.dir[1] = 0;
  Circle2D circle;
  circle.center[0] = 0.01F;
  circle.center[1] = 0.02F;
  circle.radius = 0.005F;
  Box2D box;
  box.min[0] = 0;
  box.min[1] = 0;
  box.max[0] = 0.1F;
  box.max[1] = 0.1F;
  return {Data::point2d(point), Data::line2d(line), Data::circle2d(circle), Data::box2d(box)};
}
}

TEST_CASE("Geometry context carries only the physical space of four metric shapes") {
  const auto record = profile_contract::geometryContext(space());
  CHECK(record.type == "profile.Geometry.v3");
  CHECK(record.data == nlohmann::json{{"schemaVersion", 3},
                                      {"frame", "measurement"},
                                      {"plane", "XY"},
                                      {"handedness", "right"}});
  for (const auto& shape : shapes()) {
    ProfileDescriptor parsed;
    std::string why;
    REQUIRE(profile_contract::geometrySpace(shape, &record, parsed, why));
    CHECK(profile_contract::sameSpace(parsed, space()));
    CHECK(profile_contract::geometryInSpace(shape, &record, space(), why));
  }
  for (const char* field : {"frame", "plane", "handedness"}) {
    auto other = space();
    if (std::string(field) == "frame")
      other.frame = "other-frame";
    if (std::string(field) == "plane")
      other.plane = "XZ";
    if (std::string(field) == "handedness")
      other.handedness = "reflected";
    std::string why;
    CHECK_FALSE(profile_contract::geometryInSpace(shapes()[0], &record, other, why));
  }
}

TEST_CASE("Geometry context rejects malformed records and invalid metric content") {
  const auto original = profile_contract::geometryContext(space());
  const auto point = shapes()[0];
  ProfileDescriptor parsed;
  std::string why;
  for (const char* mutation : {"type", "version", "extra", "missing", "frame"}) {
    auto record = original;
    const std::string which = mutation;
    if (which == "type")
      record.type = "profile.Geometry.v2";
    if (which == "version")
      record.data["schemaVersion"] = 2;
    if (which == "extra")
      record.data["unit"] = "m";
    if (which == "missing")
      record.data.erase("handedness");
    if (which == "frame")
      record.data["frame"] = 1;
    CHECK_FALSE(profile_contract::geometrySpace(point, &record, parsed, why));
  }
  CHECK_FALSE(profile_contract::geometrySpace(point, nullptr, parsed, why));
  auto pixel = *point.asPoint2D();
  pixel.unit = Unit2D::Pixel;
  CHECK_FALSE(profile_contract::geometrySpace(Data::point2d(pixel), &original, parsed, why));
  auto line = *shapes()[1].asLine2D();
  line.hasSegment = true;
  line.start[0] = line.end[0] = 0.01F;
  line.start[1] = line.end[1] = 0.02F;
  CHECK_FALSE(profile_contract::validGeometry(Data::line2d(line), why));
  line.hasSegment = false;
  line.dir[0] = line.dir[1] = 0;
  CHECK_FALSE(profile_contract::validGeometry(Data::line2d(line), why));
  auto circle = *shapes()[2].asCircle2D();
  circle.radius = std::numeric_limits<float>::infinity();
  CHECK_FALSE(profile_contract::validGeometry(Data::circle2d(circle), why));
  auto box = *shapes()[3].asBox2D();
  box.min[0] = 1;
  CHECK_FALSE(profile_contract::validGeometry(Data::box2d(box), why));
}
