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

// ------------------------------------------- 2D 量测域的六种载荷（ADR-0013）

TEST_CASE("六种 2D 载荷的类型名往返") {
  for (const char* name : {"Box2D", "Line2D", "Circle2D", "Point2D", "Measurement", "Record"}) {
    const Data::Kind k = kindFromTypeName(name);
    CHECK(k != Data::Kind::None);
    CHECK(std::string(typeNameFromKind(k)) == name);
  }
}

TEST_CASE("Box2D / Point2D 的 valueJson") {
  Box2D b;
  b.min[0] = -0.5f;
  b.min[1] = 0.25f;
  b.max[0] = 1.5f;
  b.max[1] = 2.0f;
  const Data d = Data::box2d(b);
  CHECK(std::string(d.typeName()) == "Box2D");
  CHECK(d.elementCount() == 1);
  const nlohmann::json j = nlohmann::json::parse(d.valueJson());
  CHECK(j["kind"] == "Box2D");
  CHECK(j["min"][0].get<double>() == doctest::Approx(-0.5));
  CHECK(j["max"][1].get<double>() == doctest::Approx(2.0));

  Point2D p;
  p.p[0] = 3.0f;
  p.p[1] = -4.0f;
  const nlohmann::json jp = nlohmann::json::parse(Data::point2d(p).valueJson());
  CHECK(jp["p"][0].get<double>() == doctest::Approx(3.0));
  CHECK(jp["p"][1].get<double>() == doctest::Approx(-4.0));
}

TEST_CASE("Line2D 只有带端点时才写 start/end") {
  Line2D l;
  l.point[0] = 1.0f;
  l.dir[1] = 1.0f;
  const nlohmann::json bare = nlohmann::json::parse(Data::line2d(l).valueJson());
  CHECK(bare["hasSegment"] == false);
  CHECK_FALSE(bare.contains("start"));

  l.hasSegment = true;
  l.start[0] = -1.0f;
  l.end[0] = 2.0f;
  const nlohmann::json seg = nlohmann::json::parse(Data::line2d(l).valueJson());
  CHECK(seg["hasSegment"] == true);
  CHECK(seg["start"][0].get<double>() == doctest::Approx(-1.0));
  CHECK(seg["end"][0].get<double>() == doctest::Approx(2.0));
}

TEST_CASE("Circle2D 的 valueJson") {
  Circle2D c;
  c.center[0] = 0.1f;
  c.center[1] = -0.2f;
  c.radius = 0.0075f;
  const nlohmann::json j = nlohmann::json::parse(Data::circle2d(c).valueJson());
  CHECK(j["kind"] == "Circle2D");
  CHECK(j["radius"].get<double>() == doctest::Approx(0.0075));
}

TEST_CASE("Measurement 的非有限值写成 null 而不是把整份 JSON 弄坏") {
  Measurement m;
  m.value = std::numeric_limits<double>::quiet_NaN();
  m.ok = false;
  m.message = "gap_left 圆拟合失败";
  const nlohmann::json bad = nlohmann::json::parse(Data::measurement(m).valueJson());
  CHECK(bad["value"].is_null());
  CHECK(bad["ok"] == false);
  CHECK(bad["message"] == "gap_left 圆拟合失败");
  CHECK(bad["unit"] == "mm");
  CHECK_FALSE(bad.contains("nominal"));  // 没设上下限就不写，免得前端以为判过了

  m.value = 6.4138;
  m.ok = true;
  m.message.clear();
  m.verdict = "ok";
  m.hasLimits = true;
  m.nominal = 6.4;
  m.upper = 7.0;
  m.lower = 5.8;
  const nlohmann::json good = nlohmann::json::parse(Data::measurement(m).valueJson());
  CHECK(good["value"].get<double>() == doctest::Approx(6.4138));
  CHECK(good["verdict"] == "ok");
  CHECK(good["upper"].get<double>() == doctest::Approx(7.0));
  CHECK_FALSE(good.contains("message"));
}

TEST_CASE("Record 原样带着算子包定义的 JSON") {
  Record r;
  r.type = "GapAlignment";
  r.data = nlohmann::json{{"templateId", "f1"}, {"score", 92.5}, {"bidirectional", false}};
  const Data d = Data::record(r);
  CHECK(std::string(d.typeName()) == "Record");
  const nlohmann::json j = nlohmann::json::parse(d.valueJson());
  CHECK(j["type"] == "GapAlignment");
  CHECK(j["data"]["templateId"] == "f1");
  CHECK(j["data"]["score"].get<double>() == doctest::Approx(92.5));
}

TEST_CASE("Tensor 只把形状与统计量写进 valueJson") {
  Tensor t;
  t.shape = {2, 3};
  t.data = {1.f, 2.f, 3.f, 4.f, 5.f, 6.f};
  CHECK(t.elementCount() == 6);
  CHECK(t.consistent());
  CHECK(t.shapeString() == "[2,3]");

  const Data d = Data::tensor(t);
  CHECK(std::string(d.typeName()) == "Tensor");
  CHECK(d.elementCount() == 6);
  const nlohmann::json j = nlohmann::json::parse(d.valueJson());
  CHECK(j["kind"] == "Tensor");
  CHECK(j["shape"] == nlohmann::json::array({2, 3}));
  CHECK(j["count"].get<int>() == 6);
  CHECK(j["min"].get<double>() == doctest::Approx(1.0));
  CHECK(j["max"].get<double>() == doctest::Approx(6.0));
  CHECK(j["mean"].get<double>() == doctest::Approx(3.5));
  CHECK(kindFromTypeName("Tensor") == Data::Kind::Tensor);
}

TEST_CASE("Tensor 全是非有限值时统计量写成 null 而不是坏 JSON") {
  Tensor t;
  t.shape = {2};
  t.data = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()};
  const nlohmann::json j = nlohmann::json::parse(Data::tensor(t).valueJson());
  CHECK(j["min"].is_null());
  CHECK(j["mean"].is_null());
  CHECK(j["count"].get<int>() == 2);
}

TEST_CASE("点云与 Indices 不走 valueJson") {
  CHECK(Data::cloud(makeCloud(3, false, false, false)).valueJson().empty());
  CHECK(Data::indices(Indices{}).valueJson().empty());
  CHECK(Data().valueJson().empty());
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
