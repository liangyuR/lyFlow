#include <doctest/doctest.h>

#include "lyflow/kernel/geometry.h"
#include "lyflow/kernel/rigid.h"

#include <cmath>
#include <limits>

using namespace lyflow;
using namespace lyflow::kernel;

TEST_CASE("Public geometry kernel preserves relation definitions, units and atomic failures") {
  Point2D point;
  point.p[0] = 3;
  point.p[1] = 4;
  Line2D line;
  line.hasSegment = true;
  line.end[0] = 2;
  GeometryIssue issue;
  double value = -1;
  REQUIRE(distance2D(point, line, {}, value, issue));
  CHECK(value == doctest::Approx(std::sqrt(17.0)));
  Point2D projected;
  REQUIRE(projectPoint2D(point, line, true, projected, issue));
  CHECK(projected.p[0] == 2);
  REQUIRE(projectPoint2D(point, line, false, projected, issue));
  CHECK(projected.p[0] == 3);
  Circle2D circle;
  circle.center[1] = 0.5f;
  circle.radius = 1;
  REQUIRE(distance2D(line, circle, {DistanceMode::SignedClearance}, value, issue));
  CHECK(value == doctest::Approx(-0.5));
  REQUIRE(distance2D(line, circle, {DistanceMode::Minimum}, value, issue));
  CHECK(value == 0);
  Intersections2D intersections;
  REQUIRE(intersect2D(line, line, 1e-6, intersections, issue));
  CHECK(intersections.coincident);
  CHECK(intersections.points.empty());
  const auto before = projected;
  auto wrongUnit = point;
  wrongUnit.unit = Unit2D::Pixel;
  CHECK_FALSE(projectPoint2D(wrongUnit, line, true, projected, issue));
  CHECK(issue.operand == 1);
  CHECK(projected.p[0] == before.p[0]);
  CHECK(projected.p[1] == before.p[1]);
  CHECK(projected.unit == before.unit);
  const auto oldValue = value;
  CHECK_FALSE(distance2D(point, line, {static_cast<DistanceMode>(99)}, value, issue));
  CHECK(issue.parameter);
  CHECK(value == oldValue);
  Transform affineTransform;
  affineTransform.m[0] = 2;
  affineTransform.m[5] = 2;
  affineTransform.m[3] = 10;
  Geometry2D mapped;
  REQUIRE(transformGeometry2D(circle, affineTransform, Unit2D::Meter, mapped, issue));
  CHECK(std::get<Circle2D>(mapped).radius == 2);
  CHECK(std::get<Circle2D>(mapped).center[0] == 10);
  for (Geometry2D geometry : {Geometry2D{point}, Geometry2D{line}, Geometry2D{circle}}) {
    Point2D sentinel;
    sentinel.p[0] = 17;
    sentinel.p[1] = -9;
    sentinel.unit = Unit2D::Pixel;
    Geometry2D output = sentinel;
    CHECK_FALSE(
        transformGeometry2D(geometry, affineTransform, static_cast<Unit2D>(17), output, issue));
    CHECK(issue.parameter);
    CHECK(issue.operand == -1);
    std::visit([](auto& value) { value.unit = static_cast<Unit2D>(17); }, geometry);
    for (const auto expected : {Unit2D::Meter, static_cast<Unit2D>(17)}) {
      CHECK_FALSE(transformGeometry2D(geometry, affineTransform, expected, output, issue));
      CHECK(issue.parameter);
      REQUIRE(std::holds_alternative<Point2D>(output));
      CHECK(std::get<Point2D>(output).p[0] == sentinel.p[0]);
      CHECK(std::get<Point2D>(output).p[1] == sentinel.p[1]);
      CHECK(std::get<Point2D>(output).unit == sentinel.unit);
    }
  }
  Plane plane;
  plane.d = -3;
  affineTransform.m[11] = 2;
  Plane mappedPlane;
  REQUIRE(kernel::transformPlane(plane, affineTransform, mappedPlane, issue));
  CHECK(mappedPlane.d == doctest::Approx(-5));
  CHECK(mappedPlane.normal[2] == 1);
  line.end[0] = 0;
  std::string why;
  CHECK_FALSE(validateGeometry2D(line, why));
  CHECK(why.find("重合") != std::string::npos);
  CHECK(point.p[0] == 3);
}

TEST_CASE("Public rigid kernel keeps legacy Euler order, proper rotations and Record ownership") {
  const auto transform = legacyEulerDegrees({1, 2, 3}, {90, 90, 0});
  CHECK(transform.m[1] == doctest::Approx(1));
  CHECK(transform.m[6] == doctest::Approx(-1));
  CHECK(transform.m[8] == doctest::Approx(-1));
  CHECK(transform.m[3] == 1);
  CHECK(transform.m[7] == 2);
  CHECK(transform.m[11] == 3);
  CHECK(transform.m[15] == 1);
  M4 matrix{};
  poses::Quaternion quaternion;
  std::string why;
  REQUIRE(rigid(&transform, matrix, quaternion, why));
  const auto mapped = kernel::apply(matrix, {1, 0, 0});
  CHECK(mapped[0] == doctest::Approx(1));
  CHECK(mapped[1] == doctest::Approx(2));
  CHECK(mapped[2] == doctest::Approx(2));
  auto reflected = transform;
  for (int i = 0; i < 3; ++i)
    reflected.m[4 * i] = -reflected.m[4 * i];
  CHECK_FALSE(rigid(&reflected, matrix, quaternion, why));
  CHECK_FALSE(kernel::finite(basis(-1)));
  CHECK_FALSE(std::isfinite(turn(3, 0)[0]));
  Record record = poses::make3DList(
      nlohmann::json::array(
          {poses::make3D("part", "source", {1, 0, 0}, {0, 0, 0, 1}, {1, 2, 3}).data}),
      "source");
  const auto original = record;
  CHECK_FALSE(transformPoses3D(record, nullptr, "target", why));
  CHECK(record.data == original.data);
  REQUIRE(transformPoses3D(record, &transform, "target", why));
  CHECK(record.data["items"][0]["id"] == "part");
  CHECK(record.data["items"][0]["frame"] == "target");
  CHECK(record.data["items"][0]["position"][2].get<double>() == doctest::Approx(2));
  CHECK(original.data["frame"] == "source");
  const auto committed = record;
  bool invalidResult = false;
  CHECK_FALSE(transformPoses3D(record, &transform, "", why, &invalidResult));
  CHECK(invalidResult);
  CHECK(record.data == committed.data);
}
