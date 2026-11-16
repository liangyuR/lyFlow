#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <vector>

#include "lyflow/kernel/profile_features.h"

namespace features = lyflow::kernel::profile_features;
namespace robust = lyflow::kernel::profile_robust;

namespace {
robust::Samples samples(const std::vector<robust::Point>& points) {
  return {points.data(), points.size(), false};
}
features::LineOptions options() {
  features::LineOptions out;
  out.fit.distThresh = 0.0005F;
  out.endpoints = features::Endpoints::InlierEnds;
  out.roi = {{-1, -1}, {1, 1}};
  return out;
}
std::vector<robust::Point> line(int count, double degrees = 0) {
  std::vector<robust::Point> out;
  const double angle = degrees * 3.14159265358979323846 / 180;
  for (int i = 0; i < count; ++i)
    out.push_back({static_cast<float>(i * 0.001 * std::cos(angle)),
                   static_cast<float>(0.5 + i * 0.001 * std::sin(angle)), 0});
  return out;
}
} // namespace

TEST_CASE(
    "Profile features: toward selection and inner endpoint follow projection in either order") {
  for (const bool reverse : {false, true}) {
    for (const bool left : {false, true}) {
      CAPTURE(reverse);
      CAPTURE(left);
      auto points = line(100);
      if (reverse)
        std::reverse(points.begin(), points.end());
      auto config = options();
      config.segmentPoints = 10;
      config.toward = {left ? -0.01 : 0.11, 0.5};
      features::LineFeature out;
      REQUIRE(features::fitLine(samples(points), config, out));
      CHECK(out.segmentApplied);
      CHECK_FALSE(out.segmentSkipped);
      REQUIRE(out.inliers.size() == 10);
      CHECK(out.inliers.front() == (reverse == left ? 90 : 0));
      CHECK(out.inliers.back() == (reverse == left ? 99 : 9));
      REQUIRE(out.innerSlot >= 0);
      CHECK(points[static_cast<std::size_t>(out.innerSlot)].x ==
            doctest::Approx(left ? 0 : 0.099).epsilon(1e-5));
      CHECK(out.start[0] <= out.end[0]);
      CHECK(out.quality.inlierRatio == 0.1);
    }
  }
}

TEST_CASE("Profile features: skipped segment and mask gaps retain original sample identities") {
  auto points = line(12);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  points[1] = {nan, nan, nan};
  points[8] = {nan, nan, nan};
  auto config = options();
  config.segmentPoints = 20;
  config.toward = {1, 0.5};
  features::LineFeature out;
  REQUIRE(features::fitLine(samples(points), config, out));
  CHECK_FALSE(out.segmentApplied);
  CHECK(out.segmentSkipped);
  CHECK(out.initialInlierCount == 10);
  CHECK(out.inliers == std::vector<int>{0, 2, 3, 4, 5, 6, 7, 9, 10, 11});
  CHECK(out.innerSlot == 11);
  CHECK(out.quality.pointCount == 12);
  CHECK(out.quality.inlierCount == 10);
  CHECK(out.quality.inlierRatio == doctest::Approx(10.0 / 12));
  CHECK(std::isnan(points[1].x));
  CHECK(std::isnan(points[8].x));
}

TEST_CASE(
    "Profile features: band preserves free fit in tolerance and pins out-of-band directions") {
  auto config = options();
  config.hasReference = true;
  config.referenceDirection = {1, 0};
  config.toleranceDeg = 10;
  auto points = line(40, 2);
  features::LineFeature free, bounded;
  REQUIRE(features::fitLine(samples(points), config, free));
  config.directionMode = features::DirectionMode::Band;
  REQUIRE(features::fitLine(samples(points), config, bounded));
  CHECK(bounded.coefficients == free.coefficients);
  CHECK(bounded.inliers == free.inliers);
  CHECK_FALSE(bounded.directionPinned);
  points = line(40, 30);
  REQUIRE(features::fitLine(samples(points), config, bounded));
  CHECK(bounded.directionPinned);
  CHECK(bounded.direction[0] == 1);
  CHECK(bounded.direction[1] == 0);
  config.directionMode = features::DirectionMode::Fixed;
  config.nominalDeg = 3;
  config.fit.distThresh = 0.005F;
  REQUIRE(features::fitLine(samples(points), config, bounded));
  CHECK(std::atan2(bounded.direction[1], bounded.direction[0]) * 180 / 3.14159265358979323846 ==
        doctest::Approx(3).epsilon(1e-5));
}

TEST_CASE(
    "Profile features: fixed-direction fallback preserves upper median and repeated weights") {
  std::vector<robust::Point> points;
  for (int i = 0; i < 6; ++i)
    points.push_back({i * 0.01F, i * 0.001F, 0});
  auto config = options();
  config.directionMode = features::DirectionMode::Fixed;
  config.hasReference = true;
  config.referenceDirection = {1, 0};
  config.fit.distThresh = 1e-8F;
  features::LineFeature out;
  REQUIRE(features::fitLine(samples(points), config, out));
  CHECK(out.coefficients[1] == points[3].y);
  CHECK(out.inliers == std::vector<int>{2, 3, 3, 4});
  CHECK(out.quality.inlierCount == 4);
  CHECK(std::set<int>(out.inliers.begin(), out.inliers.end()).size() == 3);
  const double a = static_cast<double>(points[2].y - points[3].y) * 1000;
  const double b = static_cast<double>(points[4].y - points[3].y) * 1000;
  CHECK(out.quality.rmsResidualMm == std::sqrt((a * a + b * b) / 4));
}

TEST_CASE("Profile features: ROI intersections retain strict crossing and corner-touch behavior") {
  auto config = options();
  config.endpoints = features::Endpoints::RoiIntersection;
  config.roi = {{0, 0}, {1, 1}};
  config.toward = {1, 0.5};
  auto points = line(20);
  features::LineFeature out;
  REQUIRE(features::fitLine(samples(points), config, out));
  CHECK(out.hasSegment);
  CHECK(out.start == std::array<float, 2>{0, 0.5F});
  CHECK(out.end == std::array<float, 2>{1, 0.5F});
  for (auto& point : points)
    point.y = 0;
  REQUIRE(features::fitLine(samples(points), config, out));
  CHECK_FALSE(out.hasSegment);
  CHECK(out.roiIntersectionMissing);
  for (auto& point : points)
    point.y = point.x;
  REQUIRE(features::fitLine(samples(points), config, out));
  CHECK_FALSE(out.hasSegment);
}

TEST_CASE("Profile features: requested axis retries and final endpoints use the chosen feature") {
  std::vector<robust::Point> points;
  for (int i = 0; i < 41; ++i)
    points.push_back({i * 0.001F, 0, 0});
  for (int i = 0; i < 16; ++i)
    points.push_back({0.06F, (i + 1) * 0.001F, 0});
  auto config = options();
  config.type = features::LineType::Vertical;
  config.toward = {0.06, 1};
  features::LineFeature out;
  REQUIRE(features::fitLine(samples(points), config, out));
  REQUIRE(out.inliers.size() == 16);
  CHECK(out.inliers.front() == 41);
  CHECK(out.inliers.back() == 56);
  CHECK(out.innerSlot == 56);
  CHECK(out.start[1] == points[41].y);
  CHECK(out.end[1] == points[56].y);
}

TEST_CASE("Profile features: failure and invalid reference preserve caller-owned output") {
  auto points = line(20);
  auto config = options();
  features::LineFeature out;
  out.innerSlot = 99;
  out.inliers = {7};
  features::LineFailure failure;
  config.minInliers = 100;
  CHECK_FALSE(features::fitLine(samples(points), config, out, &failure));
  CHECK(failure.stage == features::LineFailureStage::MinInliers);
  CHECK(failure.inlierCount == 20);
  CHECK(out.innerSlot == 99);
  CHECK(out.inliers == std::vector<int>{7});
  CHECK_FALSE(features::fitLine({}, config, out, &failure));
  CHECK(failure.stage == features::LineFailureStage::InsufficientSamples);
  const auto valid = config;
  config.type = static_cast<features::LineType>(-1);
  CHECK_THROWS_AS(features::fitLine(samples(points), config, out), std::invalid_argument);
  config = valid;
  config.directionMode = static_cast<features::DirectionMode>(-1);
  CHECK_THROWS_AS(features::fitLine(samples(points), config, out), std::invalid_argument);
  config = valid;
  config.endpoints = static_cast<features::Endpoints>(-1);
  CHECK_THROWS_AS(features::fitLine(samples(points), config, out), std::invalid_argument);
  config = valid;
  config.directionMode = features::DirectionMode::Fixed;
  CHECK_THROWS_AS(features::fitLine(samples(points), config, out), std::invalid_argument);
  config.hasReference = true;
  config.referenceDirection = {0, 0};
  CHECK_THROWS_AS(features::fitLine(samples(points), config, out), std::invalid_argument);
  config.referenceDirection = {std::numeric_limits<float>::infinity(), 0};
  CHECK_THROWS_AS(features::fitLine(samples(points), config, out), std::invalid_argument);
  CHECK(out.innerSlot == 99);
  CHECK(out.inliers == std::vector<int>{7});
  config = options();
  config.directionMode = features::DirectionMode::Fixed;
  config.hasReference = true;
  config.referenceDirection = {1, -2};
  const std::vector<robust::Point> extreme{{3e38F, 3e38F, 0}, {2.9e38F, 3e38F, 0}};
  CHECK_FALSE(features::fitLine(samples(extreme), config, out, &failure));
  CHECK(failure.stage == features::LineFailureStage::FixedFit);
  CHECK(out.innerSlot == 99);
  CHECK(out.inliers == std::vector<int>{7});
}

TEST_CASE("Profile features: cancellation at entry and after initial fit is atomic") {
  const auto points = line(100);
  const auto config = options();
  features::LineFeature out;
  out.innerSlot = 99;
  out.inliers = {7};
  CHECK_THROWS_AS(features::fitLine(samples(points), config, out, nullptr, [] { return true; }),
                  robust::Cancelled);
  int calls = 0;
  CHECK_THROWS_AS(
      features::fitLine(samples(points), config, out, nullptr, [&] { return ++calls >= 5; }),
      robust::Cancelled);
  CHECK(out.innerSlot == 99);
  CHECK(out.inliers == std::vector<int>{7});
}

TEST_CASE(
    "Profile features: direction keeps float hypot while residuals promote direction before hypot") {
  std::vector<robust::Point> points;
  for (int i = 0; i < 100; ++i)
    points.push_back({i * 0.001F, 0.5F + i * 0.0001F, 0});
  auto config = options();
  config.segmentPoints = 10;
  config.toward = {0.11, 0.5};
  features::LineFeature out;
  REQUIRE(features::fitLine(samples(points), config, out));
  const auto& c = out.coefficients;
  const double dx = c[3];
  const double dy = c[4];
  const double norm = std::hypot(dx, dy);
  const double floatNorm = std::hypot(c[3], c[4]);
  REQUIRE(norm != floatNorm);
  CHECK(out.quality.lineDirection[0] == c[3] / floatNorm);
  CHECK(out.quality.lineDirection[1] == c[4] / floatNorm);
  double squareSum = 0;
  double maximum = 0;
  for (const auto index : out.inliers) {
    const auto& p = points[static_cast<std::size_t>(index)];
    const double residual = std::fabs((p.x - c[0]) * dy - (p.y - c[1]) * dx) / norm * 1000.0;
    squareSum += residual * residual;
    maximum = std::max(maximum, residual);
  }
  CHECK(out.quality.rmsResidualMm == std::sqrt(squareSum / out.inliers.size()));
  CHECK(out.quality.maxResidualMm == maximum);
}
