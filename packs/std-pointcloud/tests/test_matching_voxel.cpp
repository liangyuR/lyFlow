#include <doctest/doctest.h>

#include <kiss_matcher/points/downsampling.hpp>
#include <tbb/task_arena.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <random>
#include <vector>

namespace {
using Points = std::vector<Eigen::Vector3f>;
using Cell = std::array<std::int64_t, 3>;

bool cellOf(const Eigen::Vector3f& point, double leaf, Cell& cell) {
  const Eigen::Vector3d scaled = point.cast<double>() / leaf;
  for (int axis = 0; axis < 3; ++axis) {
    if (!std::isfinite(scaled[axis]) || scaled[axis] < -1048576 || scaled[axis] >= 1048576)
      return false;
    cell[axis] = static_cast<std::int64_t>(std::floor(scaled[axis]));
  }
  return true;
}

void checkVoxels(const Points& points, double leaf) {
  struct Aggregate {
    Eigen::Vector3d sum = Eigen::Vector3d::Zero();
    std::size_t count = 0;
  };
  std::map<Cell, Aggregate> expected;
  for (const auto& point : points) {
    Cell cell{};
    if (cellOf(point, leaf, cell)) {
      expected[cell].sum += point.cast<double>();
      ++expected[cell].count;
    }
  }
  Points baseline;
  for (int threads : {1, 16}) {
    CAPTURE(threads);
    tbb::task_arena arena(threads);
    const auto result =
        arena.execute([&] { return kiss_matcher::VoxelgridSampling(points, leaf); });
    REQUIRE(result.size() == expected.size());
    std::map<Cell, bool> observed;
    for (const auto& point : result) {
      Cell cell{};
      REQUIRE(cellOf(point, leaf, cell));
      REQUIRE(expected.count(cell) == 1);
      REQUIRE(observed.emplace(cell, true).second);
      const auto& group = expected.at(cell);
      const Eigen::Vector3f mean = (group.sum / static_cast<double>(group.count)).cast<float>();
      for (int axis = 0; axis < 3; ++axis)
        CHECK(std::abs(point[axis] - mean[axis]) <=
              2 * std::numeric_limits<float>::epsilon() * std::max(1e-5f, std::abs(mean[axis])));
    }
    if (threads == 1) {
      baseline = result;
    } else {
      REQUIRE(result.size() == baseline.size());
      for (std::size_t i = 0; i < result.size(); ++i)
        CHECK((result[i].array() == baseline[i].array()).all());
    }
  }
}
} // namespace

TEST_CASE("KISS voxel sampling keeps complete cells across parallel range boundaries") {
  Points points;
  for (int i = 0; i < 4097; ++i)
    points.emplace_back(.001f + static_cast<float>(i % 11) * .00001f, .002f, .003f);
  checkVoxels(points, .01);
  for (std::uint32_t seed : {1U, 17U, 99U}) {
    std::mt19937 random(seed);
    std::shuffle(points.begin(), points.end(), random);
    checkVoxels(points, .01);
  }
  for (int i = 0; i < 2201; ++i)
    points.emplace_back(.021f, -.008f + static_cast<float>(i % 7) * .00001f, .023f);
  for (int i = 0; i < 513; ++i)
    points.emplace_back(-.019f, .032f, -.007f);
  points.emplace_back(.045f, .056f, .067f);
  for (std::uint32_t seed : {2U, 23U, 101U}) {
    std::mt19937 random(seed);
    std::shuffle(points.begin(), points.end(), random);
    checkVoxels(points, .01);
  }
}

TEST_CASE("KISS voxel sampling excludes nonfinite and out of grid coordinates") {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  Points invalid{{nan, 0, 0}, {0, inf, 0}, {0, 0, -inf}, {1e20f, 0, 0}, {-1e20f, 0, 0}};
  checkVoxels(invalid, .01);
  invalid.emplace_back(.001f, .002f, .003f);
  invalid.emplace_back(.001f, .002f, .003f);
  checkVoxels(invalid, .01);
  checkVoxels({}, .01);
  for (double leaf : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                      std::numeric_limits<double>::quiet_NaN()})
    CHECK_THROWS_AS(kiss_matcher::VoxelgridSampling(invalid, leaf), std::invalid_argument);
}
