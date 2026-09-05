// 数据模型：select 的通道搬运、bounds、Data 的类型标签。
// select 是所有滤波算子的唯一出口，它漏搬一个通道就等于每个滤波算子都漏搬。
#include <doctest/doctest.h>

#include <cmath>
#include <limits>

#include "lyflow/data.h"
#include "lyflow/json_writer.h"

using namespace lyflow;

namespace {

PointCloud makeCloud(std::size_t n, bool intensity, bool normals, bool rgb) {
  PointCloud c;
  for (std::size_t i = 0; i < n; ++i) {
    const float f = static_cast<float>(i);
    c.push(f, f * 2.0f, f * 3.0f);
    if (intensity) c.intensity.push_back(f * 0.5f);
    if (normals) {
      c.normals.push_back(1.0f);
      c.normals.push_back(0.0f);
      c.normals.push_back(0.0f);
    }
    if (rgb) {
      c.rgb.push_back(static_cast<std::uint8_t>(i));
      c.rgb.push_back(static_cast<std::uint8_t>(i + 1));
      c.rgb.push_back(static_cast<std::uint8_t>(i + 2));
    }
  }
  return c;
}

}  // namespace

TEST_CASE("select 保留全部通道") {
  const PointCloud c = makeCloud(10, true, true, true);
  const PointCloud s = c.select({2, 5, 7});

  CHECK(s.pointCount() == 3);
  REQUIRE(s.hasIntensity());
  REQUIRE(s.hasNormals());
  REQUIRE(s.hasRgb());
  CHECK(s.channelsConsistent());

  CHECK(s.xyz[0] == doctest::Approx(2.0f));
  CHECK(s.xyz[1] == doctest::Approx(4.0f));
  CHECK(s.xyz[2] == doctest::Approx(6.0f));
  CHECK(s.intensity[1] == doctest::Approx(2.5f));
  CHECK(s.rgb[6] == 7);

  // 新点云拿到新 id —— 旧的 Indices 不该还能声称指向它
  CHECK(s.id != c.id);
}

TEST_CASE("select 只有部分通道时不会凭空造出通道") {
  const PointCloud c = makeCloud(5, true, false, false);
  const PointCloud s = c.select({0, 1});
  CHECK(s.hasIntensity());
  CHECK_FALSE(s.hasNormals());
  CHECK_FALSE(s.hasRgb());
  CHECK(s.channelsConsistent());
}

TEST_CASE("select 越界下标静默跳过，不越界读") {
  const PointCloud c = makeCloud(3, true, false, false);
  const PointCloud s = c.select({-1, 0, 99, 2});
  CHECK(s.pointCount() == 2);
  CHECK(s.channelsConsistent());
}

TEST_CASE("selectInverse 是 select 的补集") {
  const PointCloud c = makeCloud(6, true, true, true);
  const PointCloud rest = c.selectInverse({1, 3});
  CHECK(rest.pointCount() == 4);
  CHECK(rest.xyz[0] == doctest::Approx(0.0f));
  CHECK(rest.xyz[3] == doctest::Approx(2.0f));
  CHECK(rest.channelsConsistent());
}

TEST_CASE("bounds 跳过非有限点") {
  PointCloud c = makeCloud(3, false, false, false);
  c.push(std::nanf(""), 0.0f, 0.0f);
  const Bounds b = c.bounds();
  REQUIRE(b.valid);
  CHECK(b.min[0] == doctest::Approx(0.0f));
  CHECK(b.max[0] == doctest::Approx(2.0f));
  CHECK(std::isfinite(b.max[1]));
}

TEST_CASE("空点云的 bounds 不 valid") {
  const PointCloud c;
  CHECK_FALSE(c.bounds().valid);
}

TEST_CASE("Data 的类型标签与 manifest 端口类型一一对应") {
  CHECK(kindFromTypeName("PointCloud") == Data::Kind::PointCloud);
  CHECK(kindFromTypeName("Indices") == Data::Kind::Indices);
  CHECK(kindFromTypeName("Transform") == Data::Kind::Transform);
  CHECK(kindFromTypeName("Plane") == Data::Kind::Plane);
  // Any 不约束具体载荷，所以映射成 None 而不是某个具体 Kind
  CHECK(kindFromTypeName("Any") == Data::Kind::None);
  CHECK(kindFromTypeName("PointCloudXYZI") == Data::Kind::None);  // D10：这个类型已删除

  const Data d = Data::cloud(makeCloud(4, true, false, false));
  CHECK(std::string(d.typeName()) == "PointCloud");
  CHECK(d.elementCount() == 4);
  // byteSize 把所有通道算进去：4 点 × (3 + 1) 个 float
  CHECK(d.byteSize() == 4 * 4 * sizeof(float));
  CHECK(d.asIndices() == nullptr);
  CHECK(d.asCloud() != nullptr);
}

TEST_CASE("Indices 记住来源点云 id") {
  const PointCloud c = makeCloud(3, false, false, false);
  Indices idx;
  idx.sourceCloudId = c.id;
  idx.values = {0, 2};
  const Data d = Data::indices(std::move(idx));
  REQUIRE(d.asIndices() != nullptr);
  CHECK(d.asIndices()->sourceCloudId == c.id);
  CHECK(d.elementCount() == 2);
}

// ----------- 下面这组来自一次代码审查抓到的缺陷，每条对着一个具体的失败场景

TEST_CASE("jsonNumber 对非有限值给出合法 JSON") {
  // 裸的 nan / inf 是无效 JSON，前端会整份 parse 失败，而症状离现场很远
  CHECK(jsonNumber(std::numeric_limits<double>::quiet_NaN()) == "null");
  CHECK(jsonNumber(std::numeric_limits<double>::infinity()) == "null");
  CHECK(jsonNumber(-std::numeric_limits<double>::infinity()) == "null");
  CHECK(jsonNumber(2.0) == "2.0");
  CHECK(jsonNumber(0.5) == "0.5");
}
