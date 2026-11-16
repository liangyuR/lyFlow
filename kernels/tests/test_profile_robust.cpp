#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <vector>

#include "lyflow/kernel/profile_robust.h"

namespace robust = lyflow::kernel::profile_robust;

namespace {
robust::Samples samples(const std::vector<robust::Point>& points) {
  return {points.data(), points.size(), false};
}

std::vector<robust::Point> noisyArc(double radius, double fromDeg, double spanDeg, int count,
                                    double noise, unsigned seed) {
  std::mt19937 generator(seed);
  std::normal_distribution<double> jitter(0.0, noise);
  std::vector<robust::Point> points;
  for (int i = 0; i < count; ++i) {
    const double angle = (fromDeg + spanDeg * i / (count - 1)) * 3.14159265358979323846 / 180.0;
    points.push_back({static_cast<float>(radius * std::sin(angle) + jitter(generator)),
                      static_cast<float>(radius - radius * std::cos(angle) + jitter(generator)),
                      0});
  }
  return points;
}
} // namespace

TEST_CASE("Robust Profile kernel: competing traces select the upper supported surface") {
  std::vector<robust::Point> points;
  for (int i = 0; i < 41; ++i)
    points.push_back({i * 0.001F, 0, 0});
  const float nan = std::numeric_limits<float>::quiet_NaN();
  points.push_back({nan, nan, nan});
  for (int i = 0; i < 21; ++i)
    points.push_back({i * 0.002F, 0.0003F, 0});
  points.push_back({0.015F, 0.02F, 0});
  const auto original = points;
  robust::LineFit fit;
  REQUIRE(robust::fitLine(samples(points), fit));
  CHECK(fit.coefficients[1] == doctest::Approx(0.0003).epsilon(1e-5));
  REQUIRE(fit.inliers.size() == 21);
  CHECK(fit.inliers.front() == 42);
  CHECK(fit.inliers.back() == 62);
  CHECK(std::is_sorted(fit.inliers.begin(), fit.inliers.end()));
  for (std::size_t i = 0; i < points.size(); ++i) {
    if (i == 41) {
      CHECK(std::isnan(points[i].x));
    } else {
      CHECK(points[i].x == original[i].x);
      CHECK(points[i].y == original[i].y);
      CHECK(points[i].z == original[i].z);
    }
  }
  robust::LineFit repeated;
  REQUIRE(robust::fitLine(samples(points), repeated));
  CHECK(repeated.coefficients == fit.coefficients);
  CHECK(repeated.inliers == fit.inliers);
}

TEST_CASE("Robust Profile kernel: axis retries retain original slots after rejection") {
  std::vector<robust::Point> points;
  for (int i = 0; i < 41; ++i)
    points.push_back({i * 0.001F, 0, 0});
  for (int i = 0; i < 16; ++i)
    points.push_back({0.06F, (i + 1) * 0.001F, 0});
  robust::LineFit fit;
  REQUIRE(robust::fitAxisLine(samples(points), true, fit));
  CHECK(std::fabs(fit.coefficients[4]) > std::fabs(fit.coefficients[3]));
  REQUIRE(fit.inliers.size() == 16);
  CHECK(fit.inliers.front() == 41);
  CHECK(fit.inliers.back() == 56);
  REQUIRE(robust::fitAxisLine(samples(points), false, fit));
  CHECK(std::fabs(fit.coefficients[3]) > std::fabs(fit.coefficients[4]));
  CHECK(fit.inliers.size() == 41);
}

TEST_CASE("Robust Profile kernel: free circle RANSAC is deterministic without optimization") {
  const auto points = noisyArc(0.004, 10, 80, 60, 0.000015, 2);
  robust::Circle2DFitOptions options;
  options.minRadius = 0.0005;
  options.maxRadius = 0.006;
  options.optimizeCoefficients = false;
  robust::CircleFit first, repeated;
  REQUIRE(robust::fitCircle(samples(points), first, options));
  REQUIRE(robust::fitCircle(samples(points), repeated, options));
  CHECK(first.coefficients == repeated.coefficients);
  CHECK(first.inliers == repeated.inliers);
  CHECK(first.report.radius == robust::Circle2DRadiusOutcome::Free);
  CHECK(first.report.unclampedRadius == first.coefficients[2]);
  CHECK(first.inliers.size() >= 35);
}

TEST_CASE("Robust Profile kernel: radius limits retain valid RANSAC arcs after refinement") {
  robust::Circle2DFitOptions options;
  options.minRadius = 0.003;
  options.maxRadius = 0.004;
  robust::CircleFit fit;
  const auto upper = noisyArc(0.005, 20, 60, 70, 0.000012, 7);
  REQUIRE(robust::fitCircle(samples(upper), fit, options));
  CHECK(fit.report.radius == robust::Circle2DRadiusOutcome::ClampedMax);
  CHECK(fit.report.unclampedRadius > options.maxRadius);
  CHECK(fit.coefficients[2] == static_cast<float>(options.maxRadius));
  CHECK(fit.inliers.size() >= 10);
  options.minRadius = 0.0016;
  options.maxRadius = 0.003;
  const auto lower = noisyArc(0.0012, 20, 60, 70, 0.000012, 8);
  REQUIRE(robust::fitCircle(samples(lower), fit, options));
  CHECK(fit.report.radius == robust::Circle2DRadiusOutcome::ClampedMin);
  CHECK(fit.coefficients[2] == static_cast<float>(options.minRadius));
}

TEST_CASE("Robust Profile kernel: soft radius prior and fixed radius remain separate methods") {
  const auto points = noisyArc(0.004, 30, 50, 50, 0.000015, 11);
  robust::Circle2DFitOptions options;
  options.minRadius = 0.001;
  options.maxRadius = 0.008;
  robust::CircleFit free, prior, fixed;
  REQUIRE(robust::fitCircle(samples(points), free, options));
  options.radiusPrior = 0.004;
  options.radiusPriorSigma = 0.0003;
  REQUIRE(robust::fitCircle(samples(points), prior, options));
  CHECK(prior.report.radius == robust::Circle2DRadiusOutcome::Prior);
  CHECK(std::fabs(prior.coefficients[2] - 0.004) < std::fabs(free.coefficients[2] - 0.004));
  options.radiusPriorSigma = 0;
  REQUIRE(robust::fitCircleFixedRadius(samples(points), 0.004, fixed, options));
  CHECK(fixed.coefficients[2] == static_cast<float>(0.004));
  CHECK(fixed.inliers.size() >= 20);
}

TEST_CASE("Robust Profile kernel: failed and malformed input never partially updates outputs") {
  robust::LineFit line;
  line.coefficients.fill(99);
  line.inliers = {7};
  const auto sentinel = line;
  CHECK_FALSE(robust::fitLine({}, line));
  CHECK(line.coefficients == sentinel.coefficients);
  CHECK(line.inliers == sentinel.inliers);
  CHECK_THROWS_AS(robust::fitLine({nullptr, 3, false}, line), std::invalid_argument);
  CHECK_THROWS_AS(
      robust::fitLine(
          {nullptr, static_cast<std::size_t>(std::numeric_limits<int>::max()) + 1, false}, line),
      std::invalid_argument);
  robust::Line2DFitOptions options;
  options.distThresh = 0;
  CHECK_THROWS_AS(robust::fitLine({}, line, options), std::invalid_argument);
  CHECK(line.coefficients == sentinel.coefficients);
  CHECK(line.inliers == sentinel.inliers);
  robust::CircleFit circle;
  circle.coefficients = {1, 2, 3};
  circle.inliers = {9};
  CHECK_FALSE(robust::fitCircle({}, circle));
  CHECK(circle.coefficients == std::array<float, 3>{1, 2, 3});
  CHECK(circle.inliers == std::vector<int>{9});
  CHECK_THROWS_AS(robust::fitCircleFixedRadius({}, std::numeric_limits<double>::infinity(), circle),
                  std::invalid_argument);
}

TEST_CASE("Robust Profile kernel: singular fixed-radius centers cannot publish NaN results") {
  const std::vector<robust::Point> points{{1, 0, 0}, {0, 1, 0}, {-1, 0, 0}, {0, -1, 0}, {0, 0, 0}};
  robust::Circle2DFitOptions options;
  options.distThresh = 2;
  options.minRadius = 0.9;
  options.maxRadius = 1.1;
  options.optimizeCoefficients = false;
  robust::CircleFit out;
  out.coefficients = {1, 2, 3};
  out.inliers = {9};
  CHECK_FALSE(robust::fitCircleFixedRadius(samples(points), 1, out, options));
  CHECK(out.coefficients == std::array<float, 3>{1, 2, 3});
  CHECK(out.inliers == std::vector<int>{9});
}

TEST_CASE("Robust Profile kernel: cancellation after RANSAC and during refinement is atomic") {
  std::vector<robust::Point> points;
  for (int i = 0; i < 100; ++i)
    points.push_back({i * 0.001F, i * 0.002F, 0});
  robust::LineFit line;
  line.coefficients.fill(99);
  line.inliers = {7};
  int calls = 0;
  CHECK_THROWS_AS(robust::fitLine(samples(points), line, {}, [&] { return ++calls >= 3; }),
                  robust::Cancelled);
  CHECK(line.coefficients[0] == 99);
  CHECK(line.inliers == std::vector<int>{7});
  const auto arc = noisyArc(0.004, 30, 50, 50, 0.000015, 11);
  robust::Circle2DFitOptions options;
  options.radiusPrior = 0.004;
  options.radiusPriorSigma = 0.0003;
  robust::CircleFit circle;
  circle.coefficients = {1, 2, 3};
  circle.inliers = {9};
  calls = 0;
  CHECK_THROWS_AS(robust::fitCircle(samples(arc), circle, options, [&] { return ++calls >= 4; }),
                  robust::Cancelled);
  CHECK(circle.coefficients == std::array<float, 3>{1, 2, 3});
  CHECK(circle.inliers == std::vector<int>{9});
}
