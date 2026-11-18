#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <limits>

#include "lyflow/kernel/profile_analysis.h"

using namespace lyflow;
namespace analysis = lyflow::kernel::profile_analysis;

namespace {
struct Samples {
  std::vector<float> xyz, intensity, normals;
  std::vector<std::uint8_t> rgb;
  analysis::CloudView view() const { return {xyz, intensity, normals, rgb}; }
  void add(float x, float y, float z) { xyz.insert(xyz.end(), {x, y, z}); }
};
}

TEST_CASE(
    "Profile analysis kernel: TLS preserves chosen axes closed intervals and delivered precision") {
  for (const auto axes : {std::array<int, 2>{0, 1}, {0, 2}, {1, 2}}) {
    Samples cloud;
    for (float x : {4.f, 1.f, 3.f, 0.f, 2.f}) {
      std::array<float, 3> point{91, 91, 91};
      point[axes[0]] = x;
      point[axes[1]] = 2 * x + 3;
      cloud.add(point[0], point[1], point[2]);
    }
    analysis::LineFit fit;
    REQUIRE(analysis::fitLine(cloud.view(), axes[0], axes[1], 1, 3, fit));
    CHECK(fit.count == 3);
    CHECK(fit.minimum == 1);
    CHECK(fit.maximum == 3);
    CHECK(fit.center[0] == doctest::Approx(2));
    CHECK(fit.center[1] == doctest::Approx(7));
    CHECK(fit.direction[0] > 0);
    CHECK(fit.direction[1] / fit.direction[0] == doctest::Approx(2));
    CHECK(fit.rms < 1e-6);
    const auto line = analysis::nativeLine(fit);
    CHECK(line.unit == Unit2D::Meter);
    CHECK(line.point[0] == fit.center[0]);
    CHECK(line.dir[1] == fit.direction[1]);
  }
  Samples large;
  for (float x : {0.f, 8.f, 24.f})
    large.add(x, 0, 1e8f + x);
  analysis::LineFit fit;
  REQUIRE(analysis::fitLine(large.view(), 0, 2, -INFINITY, INFINITY, fit));
  double squared = 0;
  for (std::size_t i = 0; i < large.xyz.size() / 3; ++i) {
    const double x = large.xyz[i * 3] - fit.center[0];
    const double y = large.xyz[i * 3 + 2] - fit.center[1];
    const double cross = x * fit.direction[1] - y * fit.direction[0];
    squared +=
        cross * cross / (fit.direction[0] * fit.direction[0] + fit.direction[1] * fit.direction[1]);
  }
  CHECK(fit.rms == doctest::Approx(std::sqrt(squared / 3)).epsilon(1e-12));
  CHECK(fit.rms > 1);
}

TEST_CASE(
    "Profile analysis kernel: full-cloud validation rejects unused coordinates and optional channels") {
  Samples cloud;
  for (int i = 0; i < 4; ++i)
    cloud.add(static_cast<float>(i), 0, static_cast<float>(i * 2));
  std::string why;
  REQUIRE(analysis::validateCloud(cloud.view(), 3, why));
  cloud.xyz[1] = std::numeric_limits<float>::quiet_NaN();
  CHECK_FALSE(analysis::validateCloud(cloud.view(), 3, why));
  CHECK(why == "点云坐标必须全部有限；请先显式移除无效点");
  analysis::LineFit fit;
  fit.count = 99;
  CHECK_FALSE(analysis::fitLine(cloud.view(), 0, 2, 0, 3, fit));
  CHECK(fit.count == 99);
  cloud.xyz[1] = 0;
  cloud.intensity.assign(4, 1);
  cloud.intensity[2] = std::numeric_limits<float>::infinity();
  CHECK_FALSE(analysis::validateCloud(cloud.view(), 3, why));
  CHECK(why == "点云强度必须有限");
  cloud.intensity.clear();
  cloud.normals.assign(12, 0);
  cloud.normals[5] = std::numeric_limits<float>::quiet_NaN();
  CHECK_FALSE(analysis::validateCloud(cloud.view(), 3, why));
  CHECK(why == "点云法线分量必须有限");
  cloud.normals.clear();
  cloud.rgb.assign(11, 1);
  CHECK_FALSE(analysis::validateCloud(cloud.view(), 3, why));
  cloud.rgb.clear();
  cloud.xyz.push_back(1);
  CHECK_FALSE(analysis::validateCloud(cloud.view(), 3, why));
}

TEST_CASE(
    "Profile analysis kernel: degenerate neighborhoods and cancellation do not publish partial fits") {
  for (const auto points :
       {std::vector<float>{0, 0, 0, 0, 0, 0, 0, 0, 0}, {1, 0, 0, -1, 0, 0, 0, 0, 1, 0, 0, -1}}) {
    Samples cloud;
    cloud.xyz = points;
    analysis::LineFit fit;
    fit.count = 42;
    CHECK_FALSE(analysis::fitLine(cloud.view(), 0, 2, -INFINITY, INFINITY, fit));
    CHECK(fit.count == 42);
  }
  Samples cloud;
  for (int i = 0; i < 4; ++i)
    cloud.add(static_cast<float>(i), 0, 1);
  analysis::LineFit fit;
  fit.count = 42;
  CHECK_FALSE(analysis::fitLine(cloud.view(), 0, 2, 0, 1, fit));
  CHECK_FALSE(analysis::fitLine(cloud.view(), 0, 0, 0, 3, fit));
  int polls = 0;
  CHECK_FALSE(analysis::fitLine(cloud.view(), 0, 2, 0, 3, fit, [&] { return ++polls >= 2; }));
  CHECK(polls == 2);
  CHECK(fit.count == 42);
}

TEST_CASE(
    "Profile analysis kernel: rigid composition preserves channels order normalized normals and input") {
  Samples cloud;
  cloud.add(1, 2, 3);
  cloud.add(-1, 4, 2);
  cloud.intensity = {5, 6};
  cloud.rgb = {1, 2, 3, 4, 5, 6};
  cloud.normals = {0, 0, 2, 0, 0, 3};
  const auto original = cloud.xyz;
  Transform sensor, common;
  sensor.m[3] = 3;
  common.m[0] = 0;
  common.m[2] = 1;
  common.m[8] = -1;
  common.m[10] = 0;
  analysis::Matrix4 a, b;
  REQUIRE(analysis::rigidMatrix(sensor, a));
  REQUIRE(analysis::rigidMatrix(common, b));
  const auto composed = analysis::compose(b, a);
  analysis::CloudChannels output;
  std::vector<float> progress;
  REQUIRE(analysis::transformCloud(cloud.view(), composed, output, {},
                                   [&](float value) { progress.push_back(value); }));
  CHECK(output.xyz == std::vector<float>{3, 2, -4, 2, 4, -2});
  CHECK(output.intensity == cloud.intensity);
  CHECK(output.rgb == cloud.rgb);
  CHECK(output.normals == std::vector<float>{1, 0, 0, 1, 0, 0});
  CHECK(cloud.xyz == original);
  CHECK(progress == std::vector<float>{0});
  const auto saved = output.xyz;
  CHECK_FALSE(analysis::transformCloud(cloud.view(), composed, output, [] { return true; }));
  CHECK(output.xyz == saved);
  cloud.normals.assign(6, 0);
  CHECK_FALSE(analysis::transformCloud(cloud.view(), composed, output));
  CHECK(output.xyz == saved);
  common.m[0] = -1;
  CHECK_FALSE(analysis::rigidMatrix(common, b));
  common = Transform{};
  common.m[15] = 2;
  CHECK_FALSE(analysis::rigidMatrix(common, b));
  cloud.normals.clear();
  cloud.xyz[0] = std::numeric_limits<float>::max();
  auto overflow = analysis::identityMatrix();
  overflow[3] = static_cast<double>(std::numeric_limits<float>::max());
  CHECK_FALSE(analysis::transformCloud(cloud.view(), overflow, output));
  CHECK(output.xyz == saved);
}
