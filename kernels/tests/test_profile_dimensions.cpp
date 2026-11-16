#include <doctest/doctest.h>

#include <cmath>
#include <limits>

#include "lyflow/kernel/profile_dimensions.h"

using namespace lyflow;
namespace dimensions = lyflow::kernel::profile_dimensions;
namespace {
Point2D point(float x, float y) {
  Point2D result;
  result.p[0] = x;
  result.p[1] = y;
  return result;
}
Line2D line(float x, float y, float dx, float dy) {
  Line2D result;
  result.point[0] = x;
  result.point[1] = y;
  result.dir[0] = dx;
  result.dir[1] = dy;
  return result;
}
Circle2D circle(float x, float y, float radius) {
  Circle2D result;
  result.center[0] = x;
  result.center[1] = y;
  result.radius = radius;
  return result;
}
}

TEST_CASE(
    "Profile dimensions kernel preserves signed flush direction and extends the datum segment") {
  auto base = line(0, 0, 2, 0);
  base.hasSegment = true;
  base.start[0] = -.001f;
  base.end[0] = .001f;
  const auto reference = point(.004f, -.003f);
  dimensions::FlushResult result;
  dimensions::FlushOptions options;
  std::string why;
  REQUIRE(dimensions::flushPointLine(base, reference, options, result, why));
  CHECK(result.valueMm == doctest::Approx(3));
  CHECK(result.foot.p[0] == reference.p[0]);
  CHECK(result.foot.p[1] == 0);
  CHECK(result.baseLine.start[0] == base.start[0]);
  CHECK(result.baseLine.end[0] == reference.p[0]);
  CHECK(result.segment.start[0] == reference.p[0]);
  CHECK(result.segment.end[1] == reference.p[1]);
  CHECK(base.end[0] == .001f);
  const double value = result.valueMm;
  base.dir[0] = -base.dir[0];
  REQUIRE(dimensions::flushPointLine(base, reference, options, result, why));
  CHECK(result.valueMm == value);
  base = line(0, 0, 0, -2);
  REQUIRE(dimensions::flushPointLine(base, point(.004f, -.003f), options, result, why));
  CHECK(result.valueMm == doctest::Approx(4));
  options.signedDistance = false;
  options.gain = -2;
  options.offsetMm = .5;
  REQUIRE(dimensions::flushPointLine(base, point(-.004f, -.003f), options, result, why));
  CHECK(result.valueMm == doctest::Approx(-7.5));
}

TEST_CASE(
    "Profile dimensions kernel has an explicit normal policy independent of datum orientation") {
  auto base = line(.001f, -.003f, .6f, .8f);
  const auto reference = point(.007f, -.001f);
  dimensions::FlushOptions options;
  options.sign = dimensions::FlushSign::PositiveNormal;
  options.positiveNormal = {.8f, -.6f};
  dimensions::FlushResult first, reversed;
  std::string why;
  REQUIRE(dimensions::flushPointLine(base, reference, options, first, why));
  base.dir[0] = -base.dir[0];
  base.dir[1] = -base.dir[1];
  REQUIRE(dimensions::flushPointLine(base, reference, options, reversed, why));
  CHECK(reversed.valueMm == first.valueMm);
  CHECK(reversed.foot.p[0] == first.foot.p[0]);
  CHECK(reversed.foot.p[1] == first.foot.p[1]);
  options.positiveNormal = {-.8f, .6f};
  REQUIRE(dimensions::flushPointLine(base, reference, options, reversed, why));
  CHECK(reversed.valueMm == -first.valueMm);
  options.positiveNormal = {.6f, .8f};
  CHECK_FALSE(dimensions::flushPointLine(base, reference, options, reversed, why));
  CHECK(reversed.valueMm == -first.valueMm);
}

TEST_CASE(
    "Profile dimensions kernel keeps center-line and datum-tangent gap definitions distinct") {
  const auto left = circle(0, 0, .001f), right = circle(.01f, .004f, .002f);
  dimensions::CircleGapResult center, tangent;
  dimensions::CircleGapOptions options;
  std::string why;
  REQUIRE(dimensions::circleGap(left, right, nullptr, options, center, why));
  CHECK(center.valid);
  CHECK_FALSE(center.crossed);
  CHECK(center.valueMm == doctest::Approx(std::sqrt(116.) - 3).epsilon(1e-6));
  auto base = line(0, 0, 1, 0);
  base.hasSegment = true;
  base.end[0] = .02f;
  options.definition = dimensions::CircleGapDefinition::DatumTangents;
  options.offsetMm = .25;
  REQUIRE(dimensions::circleGap(left, right, &base, options, tangent, why));
  CHECK(tangent.valueMm == doctest::Approx(7.25));
  CHECK(tangent.segment.start[1] == 0);
  CHECK(tangent.segment.end[1] == 0);
  std::swap(base.start[0], base.end[0]);
  dimensions::CircleGapResult reversed;
  REQUIRE(dimensions::circleGap(left, right, &base, options, reversed, why));
  CHECK(reversed.valueMm == tangent.valueMm);
  CHECK(reversed.segment.start[0] == tangent.segment.start[0]);
  CHECK(reversed.segment.end[0] == tangent.segment.end[0]);
}

TEST_CASE(
    "Profile dimensions kernel returns invalid NaN for crossed center gap and rejects crossed tangents") {
  const auto left = circle(0, 0, .003f), right = circle(.004f, 0, .003f);
  dimensions::CircleGapResult result;
  dimensions::CircleGapOptions options;
  std::string why;
  REQUIRE(dimensions::circleGap(left, right, nullptr, options, result, why));
  CHECK_FALSE(result.valid);
  CHECK(result.crossed);
  CHECK(std::isnan(result.valueMm));
  auto base = line(0, 0, 1, 0);
  base.hasSegment = true;
  base.end[0] = .02f;
  options.definition = dimensions::CircleGapDefinition::DatumTangents;
  result.valueMm = 42;
  CHECK_FALSE(dimensions::circleGap(left, right, &base, options, result, why));
  CHECK(result.valueMm == 42);
  CHECK(why == "definition A：两条切线交叉了（gap < 0）");
  base.end[0] = 0;
  CHECK_FALSE(dimensions::circleGap(left, right, &base, options, result, why));
  CHECK(result.valueMm == 42);
  options.definition = dimensions::CircleGapDefinition::CenterLine;
  CHECK_FALSE(dimensions::circleGap(left, left, nullptr, options, result, why));
  CHECK(result.valueMm == 42);
}

TEST_CASE(
    "Profile dimensions kernel preserves gain then absolute value then offset and uncompensated distance") {
  const auto a = point(.001f, .002f), b = point(.004f, -.002f);
  dimensions::PointOffsetOptions options;
  options.gainDx = -2;
  options.gainDy = 3;
  options.absDx = options.absDy = true;
  options.dxOffsetMm = -.5;
  options.dyOffsetMm = .25;
  dimensions::PointOffsetResult result;
  std::string why;
  REQUIRE(dimensions::pointOffset(a, b, options, result, why));
  CHECK(result.dxMm == doctest::Approx(5.5));
  CHECK(result.dyMm == doctest::Approx(12.25));
  CHECK(result.distanceMm == doctest::Approx(5));
  CHECK(result.segment.start[0] == a.p[0]);
  CHECK(result.segment.end[1] == b.p[1]);
  options = {};
  REQUIRE(dimensions::pointOffset(a, a, options, result, why));
  CHECK(result.dxMm == 0);
  CHECK(result.dyMm == 0);
  CHECK(result.distanceMm == 0);
  CHECK(result.segment.hasSegment);
  CHECK(result.segment.dir[0] == 1);
  CHECK(result.segment.start[0] == result.segment.end[0]);
}

TEST_CASE(
    "Profile dimensions kernel rejects ambiguous units nonfinite values and overflow atomically") {
  auto a = point(0, 0), b = point(.004f, .003f);
  auto base = line(0, 0, 1, 0);
  dimensions::PointOffsetOptions offset;
  dimensions::PointOffsetResult result;
  result.dxMm = 42;
  std::string why;
  a.unit = Unit2D::Pixel;
  CHECK_FALSE(dimensions::pointOffset(a, b, offset, result, why));
  CHECK(result.dxMm == 42);
  a.unit = Unit2D::Meter;
  offset.gainDx = std::numeric_limits<double>::infinity();
  CHECK_FALSE(dimensions::pointOffset(a, b, offset, result, why));
  CHECK(result.dxMm == 42);
  offset = {};
  a.p[0] = -std::numeric_limits<float>::max();
  b.p[0] = std::numeric_limits<float>::max();
  CHECK_FALSE(dimensions::pointOffset(a, b, offset, result, why));
  CHECK(result.dxMm == 42);
  dimensions::FlushResult flush;
  flush.valueMm = 42;
  base.dir[0] = 0;
  CHECK_FALSE(dimensions::flushPointLine(base, point(0, 0), {}, flush, why));
  CHECK(flush.valueMm == 42);
  base = line(0, 0, 1, 0);
  auto reference = point(std::numeric_limits<float>::quiet_NaN(), 0);
  CHECK_FALSE(dimensions::flushPointLine(base, reference, {}, flush, why));
  CHECK(flush.valueMm == 42);
  dimensions::CircleGapResult gap;
  gap.valueMm = 42;
  CHECK_FALSE(
      dimensions::circleGap(circle(0, 0, -1), circle(.01f, 0, .001f), nullptr, {}, gap, why));
  CHECK(gap.valueMm == 42);
}
