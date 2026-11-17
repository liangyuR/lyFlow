#include <doctest/doctest.h>

#include <cmath>
#include <limits>

#include "lyflow/kernel/profile.h"

namespace kp = lyflow::kernel::profile;

TEST_CASE("Profile kernel: median preserves rejected centers, truncated ends and even widths") {
  const std::vector<float> input{9, 1, 5, 3};
  const std::vector<float> expected{5, 5, 3, 4};
  CHECK(kp::movingMedian(input, 3) == expected);
  CHECK(kp::movingMedian(input, 2) == expected);
  CHECK(kp::movingMedian(input, 1) == input);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const auto holes = kp::movingMedian({0, nan, 4}, 5);
  REQUIRE(holes.size() == 3);
  CHECK(holes[0] == 2);
  CHECK(std::isnan(holes[1]));
  CHECK(holes[2] == 2);
}

TEST_CASE("Profile kernel: overlapping faces split in acquisition order before indexed sorting") {
  const kp::PlanarSamples samples{{0, 1, 2, 2, 1, 0}, {0, 0, 0, 4, 4, 4}};
  const auto pieces = kp::splitPieces(samples, 1, 1, 3);
  REQUIRE(pieces.size() == 2);
  CHECK(pieces[0].i0 == 0);
  CHECK(pieces[0].i1 == 2);
  CHECK(pieces[1].i0 == 3);
  CHECK(pieces[1].i1 == 5);
  const auto second = kp::pieceView(samples, pieces[1]);
  CHECK(second.indices() == std::vector<int>{5, 4, 3});
  const auto merged = kp::mergedView(samples, pieces);
  CHECK(merged.indices() == std::vector<int>{0, 5, 1, 4, 2, 3});
  CHECK(merged.lowerBound(1) == 2);
  CHECK(merged.upperBound(1) == 4);
  CHECK(samples.x == std::vector<float>{0, 1, 2, 2, 1, 0});
  CHECK(kp::splitPieces(samples, 1, 1, 4).empty());
  CHECK(kp::splitPieces(samples, .99, 1, 2).empty());
}

TEST_CASE("Profile kernel: TLS directions and millimeter covariance threshold remain unchanged") {
  const kp::PlanarSamples diagonal{{-2, -1, 0, 1, 2}, {-3, -1, 1, 3, 5}};
  const auto view = kp::pieceView(diagonal, {0, 4});
  kp::Line line;
  REQUIRE(kp::fitLine(view, 0, 4, line));
  CHECK(line.px == 0);
  CHECK(line.pz == 1);
  CHECK(line.dx == doctest::Approx(1 / std::sqrt(5.)));
  CHECK(line.dz == doctest::Approx(2 / std::sqrt(5.)));
  CHECK(line.rms == doctest::Approx(0));
  CHECK(line.n == 5);
  const kp::PlanarSamples vertical{{3, 3, 3}, {-2, 0, 2}};
  REQUIRE(kp::fitLine(kp::pieceView(vertical, {0, 2}), 0, 2, line));
  CHECK(line.dx == 0);
  CHECK(line.dz == 1);
  const kp::PlanarSamples tiny{{-1e-7f, 0, 1e-7f}, {-1e-7f, 0, 1e-7f}};
  REQUIRE(kp::fitLine(kp::pieceView(tiny, {0, 2}), 0, 2, line));
  CHECK(line.dx == 1);
  CHECK(line.dz == 0);
  const kp::PlanarSamples coincident{{2, 2, 2}, {4, 4, 4}};
  REQUIRE(kp::fitLine(kp::pieceView(coincident, {0, 2}), 0, 2, line));
  CHECK(line.dx == 1);
  CHECK(line.dz == 0);
  CHECK(line.rms == 0);
  const auto original = line;
  CHECK_FALSE(kp::fitLine(view, 1, 1, line));
  CHECK(line.rms == original.rms);
}

TEST_CASE("Profile kernel: algebraic circle keeps bulge mapping and determinant rejection") {
  const kp::PlanarSamples arc{{4, 1, -2, 1}, {2, 5, 2, -1}};
  const auto view = kp::pieceView(arc, {0, 3});
  double cx = 99, cz = 98, radius = 97;
  REQUIRE(kp::fitCircle(view, 0, 3, 1, cx, cz, radius));
  CHECK(cx == doctest::Approx(1));
  CHECK(cz == doctest::Approx(2));
  CHECK(radius == doctest::Approx(3));
  REQUIRE(kp::fitCircle(view, 0, 3, -1, cx, cz, radius));
  CHECK(cx == doctest::Approx(1));
  CHECK(cz == doctest::Approx(2));
  CHECK(radius == doctest::Approx(3));
  const kp::PlanarSamples collinear{{0, 1, 2}, {1, 2, 3}};
  CHECK_FALSE(kp::fitCircle(kp::pieceView(collinear, {0, 2}), 0, 2, 1, cx, cz, radius));
  const kp::PlanarSamples tiny{{-1e-5f, 0, 1e-5f}, {0, 1e-5f, 0}};
  CHECK_FALSE(kp::fitCircle(kp::pieceView(tiny, {0, 2}), 0, 2, 1, cx, cz, radius));
  CHECK(cx == doctest::Approx(1));
  CHECK(cz == doctest::Approx(2));
  CHECK(radius == doctest::Approx(3));
}

TEST_CASE("Profile kernel: sliding faces retain strict RMS and inclusive span thresholds") {
  kp::PlanarSamples samples;
  for (int i = 0; i <= 100; ++i) {
    samples.x.push_back(static_cast<float>(i) / 10);
    samples.z.push_back(2);
  }
  kp::FaceParams params;
  params.minLenMm = 10;
  const std::vector<kp::Piece> pieces{{0, 100}};
  const auto faces = kp::splitFaces(samples, pieces, params);
  REQUIRE(faces.size() == 1);
  CHECK(faces[0].lenMm == 10);
  CHECK(faces[0].rmsMm == 0);
  CHECK(faces[0].n == 101);
  params.maxRmsMm = 0;
  CHECK(kp::splitFaces(samples, pieces, params).empty());
  params.maxRmsMm = .05;
  params.minLenMm = 10.01;
  CHECK(kp::splitFaces(samples, pieces, params).empty());
}

TEST_CASE("Profile kernel: prominence extrema and canonical normal preserve orientation") {
  const auto extrema = kp::zigzag({0, 2, 0, 3, 0}, 1);
  REQUIRE(extrema.size() == 4);
  CHECK(extrema[0].k == 0);
  CHECK_FALSE(extrema[0].isMax);
  CHECK(extrema[1].k == 1);
  CHECK(extrema[1].isMax);
  CHECK(extrema[2].k == 2);
  CHECK_FALSE(extrema[2].isMax);
  CHECK(extrema[3].k == 3);
  CHECK(extrema[3].isMax);
  CHECK(kp::normalizeDeg(-180) == 180);
  CHECK(kp::normalizeDeg(540) == 180);
}

TEST_CASE("Profile kernel: cancellation and invalid views cannot overwrite fit outputs") {
  const kp::PlanarSamples samples{{0, 1, 2, 3}, {0, 1, 0, 1}};
  const auto view = kp::pieceView(samples, {0, 3});
  kp::Line output;
  output.px = 42;
  int checks = 0;
  CHECK_THROWS_AS(kp::fitLine(view, 0, 3, output, [&] { return ++checks == 3; }), kp::Cancelled);
  CHECK(output.px == 42);
  double cx = 42, cz = 43, radius = 44;
  checks = 0;
  CHECK_THROWS_AS(kp::fitCircle(view, 0, 3, 1, cx, cz, radius, [&] { return ++checks == 3; }),
                  kp::Cancelled);
  CHECK(cx == 42);
  CHECK(cz == 43);
  CHECK(radius == 44);
  CHECK_THROWS_AS(kp::movingMedian({1, 2, 3}, 3, [] { return true; }), kp::Cancelled);
  CHECK_THROWS_AS(kp::splitPieces(samples, 1, 1, 3, [] { return true; }), kp::Cancelled);
  CHECK_THROWS_AS(kp::splitFaces(samples, {{0, 3}}, {}, [] { return true; }), kp::Cancelled);
  CHECK_THROWS_AS(kp::zigzag({0, 2, 0}, 1, [] { return true; }), kp::Cancelled);
  CHECK_THROWS_AS(kp::pieceView(samples, {-1, 3}), std::invalid_argument);
  CHECK_THROWS_AS(kp::View(samples, {4}), std::invalid_argument);
  CHECK_THROWS_AS(kp::splitPieces({{0}, {}}, 1, 1, 3), std::invalid_argument);
  CHECK_FALSE(kp::fitLine(view, -1, 2, output));
  CHECK_FALSE(kp::fitCircle(view, 0, 4, 1, cx, cz, radius));
}
