#include <cmath>
#include <fstream>
#include <limits>

#include "helpers.h"
#include "lyflow/calibration.h"
#include "lyflow/pose.h"

namespace {

using namespace lyflow;
using Json = nlohmann::json;
constexpr double pi = poses::kPi;

Data pose2(const char* id = "part", double x = 1, double y = 2, double angle = 0,
           const char* frame = "image", const char* unit = "px") {
  auto pose = poses::make2D(id, frame, unit, {x, y}, angle, {4, 2});
  pose.data["classId"] = "零件-A";
  pose.data["attributes"] = {{"serial", "000123"}, {"checked", true}, {"nullable", nullptr}};
  return Data::record(std::move(pose));
}

Data pose3(const char* id, std::array<double, 3> position, poses::Quaternion q = {0, 0, 0, 1},
           const char* frame = "world") {
  return Data::record(poses::make3D(id, frame, position, q, {0.1, 0.2, 0.3}));
}

const Json& data(const test::OpCall& call, const char* port) {
  const auto found = call.outputs.find(port);
  REQUIRE(found != call.outputs.end());
  const auto* record = found->second.asRecord();
  REQUIRE(record != nullptr);
  return record->data;
}

const Json& first(const test::OpCall& call, const char* port = "poses") {
  const auto& d = data(call, port);
  REQUIRE(d.at("items").size() > 0);
  return d.at("items")[0];
}

Transform quarterTurn(float tx = 0, float ty = 0) {
  Transform transform;
  transform.m[0] = 0;
  transform.m[1] = -1;
  transform.m[4] = 1;
  transform.m[5] = 0;
  transform.m[3] = tx;
  transform.m[7] = ty;
  return transform;
}

} // namespace

TEST_CASE("pose constructors keep explicit dimensions units identities and dynamic values") {
  test::OpCall c;
  c.inputs = {{"x", Data::record(values::number(12, "px"))},
              {"angle", Data::record(values::number(pi / 2, "rad"))}};
  REQUIRE(
      c.run("pose.construct_2d", {{"id", Value::text("零件一")}, {"size", Value::vec({5, 6})}}).ok);
  CHECK(data(c, "pose")["id"] == "零件一");
  CHECK(data(c, "pose")["position"][0] == 12);
  CHECK(data(c, "pose")["angle"].get<double>() == doctest::Approx(pi / 2));
  CHECK(data(c, "pose")["pickable"] == false);
  c.inputs["x"] = Data::record(values::number(12, "m"));
  CHECK_FALSE(c.run("pose.construct_2d").ok);
  c.inputs.clear();
  CHECK_FALSE(c.run("pose.construct_2d", {{"size", Value::vec({0, 1})}}).ok);
  CHECK_FALSE(c.run("pose.construct_3d", {{"quaternion", Value::vec({0, 0, 0, 0})}}).ok);
  for (double factor : {1e-300, 1e300, -1e-300, -1e300}) {
    REQUIRE(c.run("pose.construct_3d", {{"quaternion", Value::vec({0, 0, factor, factor})}}).ok);
    const auto q = data(c, "pose")["rotation"].get<poses::Quaternion>();
    CHECK(q[2] == doctest::Approx(std::sqrt(0.5)));
    CHECK(q[3] == doctest::Approx(std::sqrt(0.5)));
  }
  c.inputs = {{"rx", Data::record(values::number(pi / 2, "rad"))},
              {"ry", Data::record(values::number(pi / 2, "rad"))}};
  REQUIRE(c.run("pose.construct_3d", {{"rotationMode", Value::text("euler_xyz")}}).ok);
  const auto rotation = poses::rotationMatrix(data(c, "pose")["rotation"].get<poses::Quaternion>());
  CHECK(rotation[2] == doctest::Approx(1));
  CHECK(rotation[3] == doctest::Approx(1));
  CHECK(rotation[7] == doctest::Approx(1));
  CHECK_FALSE(c.run("pose.construct_3d").ok);
}

TEST_CASE(
    "pose collections retain stable order reject duplicate identities and preserve empty results") {
  test::OpCall c;
  c.inputs = {{"a", pose2("a")}, {"b", pose2("b")}};
  REQUIRE(c.run("pose.concat").ok);
  const Data combined = c.outputs.at("poses");
  CHECK(data(c, "poses")["items"][0]["id"] == "a");
  CHECK(data(c, "poses")["items"][1]["id"] == "b");
  c.inputs = {{"poses", combined}};
  REQUIRE(c.run("pose.get", {{"index", Value::integer(1)}}).ok);
  CHECK(data(c, "pose")["id"] == "b");
  CHECK_FALSE(c.run("pose.get", {{"index", Value::integer(-1)}}).ok);
  CHECK(c.status.code == "index_out_of_range");
  c.inputs = {{"a", pose2("a")}, {"b", pose2("a")}};
  CHECK_FALSE(c.run("pose.concat").ok);
  c.inputs["b"] = pose2("b", 0, 0, 0, "camera-2");
  CHECK_FALSE(c.run("pose.concat").ok);
  c.inputs = {{"poses", combined}};
  REQUIRE(
      c.run("pose.filter", {{"mode", Value::text("classId")}, {"text", Value::text("不存在")}}).ok);
  CHECK(data(c, "matched")["items"].empty());
  CHECK(data(c, "rejected")["items"].size() == 2);
  const Data empty = c.outputs.at("matched");
  c.inputs = {{"poses", empty}};
  CHECK_FALSE(c.run("pose.update", {{"patch", Value::text("{\"confidence\":2}")}}).ok);
  CHECK_FALSE(c.run("pose.get").ok);
  CHECK_FALSE(
      c.run("pose.filter", {{"mode", Value::text("distance")}, {"minimum", Value::number(-1)}}).ok);
}

TEST_CASE("pose filtering and updates preserve source data and optional state semantics") {
  test::OpCall c;
  auto a = pose2("a").asRecord()->data;
  auto b = pose2("b", 2, 2).asRecord()->data;
  a["confidence"] = 0.7;
  b["confidence"] = 0.9;
  const Data input = Data::record(poses::make2DList({a, b}));
  c.inputs = {{"poses", input}};
  REQUIRE(c.run("pose.filter", {{"minimum", Value::number(0.8)}}).ok);
  CHECK(first(c, "matched")["id"] == "b");
  CHECK(first(c, "rejected")["id"] == "a");
  REQUIRE(
      c.run("pose.filter", {{"mode", Value::text("distance")}, {"minimum", Value::number(2)}}).ok);
  CHECK(first(c, "matched")["id"] == "a");
  CHECK(data(c, "rejected")["items"].size() == 1);
  REQUIRE(
      c.run("pose.update", {{"patch", Value::text("{\"name\":\"新名称\",\"pickable\":true}")}}).ok);
  CHECK(first(c)["name"] == "新名称");
  CHECK(first(c)["pickable"] == true);
  CHECK_FALSE(input.asRecord()->data["items"][0].contains("name"));
  CHECK_FALSE(c.run("pose.update", {{"patch", Value::text("{\"frame\":\"bad\"}")}}).ok);
  CHECK_FALSE(c.run("pose.update", {{"patch", Value::text("{\"size\":[-1,1]}")}}).ok);
  REQUIRE(c.run("pose.filter", {{"mode", Value::text("pickable")}}).ok);
  CHECK(data(c, "matched")["items"].empty());
}

TEST_CASE(
    "pose transformations distinguish source target frames rotation order and local offsets") {
  test::OpCall c;
  const Data original = pose2("a", 1, 2, 0, "image", "px");
  c.inputs = {{"poses", original}, {"transform", Data::transform(quarterTurn(10, 20))}};
  REQUIRE(c.run("pose.transform_2d", {{"targetFrame", Value::text("camera-2")}}).ok);
  CHECK(first(c)["position"][0] == 8);
  CHECK(first(c)["position"][1] == 21);
  CHECK(first(c)["angle"].get<double>() == doctest::Approx(pi / 2));
  CHECK(first(c)["attributes"] == original.asRecord()->data["attributes"]);
  CHECK(first(c)["frame"] == "camera-2");
  CHECK_FALSE(c.run("pose.transform_2d", {{"sourceFrame", Value::text("wrong")}}).ok);
  CHECK_FALSE(c.run("pose.transform_2d", {{"unit", Value::text("m")}}).ok);
  Transform scaled;
  scaled.m[0] = scaled.m[5] = 2;
  c.inputs["transform"] = Data::transform(scaled);
  REQUIRE(c.run("pose.transform_2d").ok);
  CHECK(first(c)["size"][0] == 8);
  c.inputs = {{"poses", pose3("a", {1, 2, 3}, {0, 0, std::sqrt(0.5), std::sqrt(0.5)})}};
  REQUIRE(c.run("pose.offset_3d", {{"offset", Value::vec({1, 0, 0})}}).ok);
  CHECK(first(c)["position"][0].get<double>() == doctest::Approx(1));
  CHECK(first(c)["position"][1].get<double>() == doctest::Approx(3));
  REQUIRE(c.run("pose.offset_3d",
                {{"offset", Value::vec({1, 0, 0})}, {"reference", Value::text("world")}})
              .ok);
  CHECK(first(c)["position"][0] == 2);
  c.inputs["transform"] = Data::transform(quarterTurn(10, 20));
  REQUIRE(c.run("pose.transform_3d").ok);
  CHECK(first(c)["position"][0] == 8);
  CHECK(first(c)["position"][1] == 21);
  const auto q = first(c)["rotation"].get<poses::Quaternion>();
  CHECK(std::abs(q[2]) == doctest::Approx(1));
  c.inputs["transform"] = Data::transform(scaled);
  CHECK_FALSE(c.run("pose.transform_3d").ok);
  scaled = Transform{};
  scaled.m[0] = -1;
  c.inputs["transform"] = Data::transform(scaled);
  CHECK_FALSE(c.run("pose.transform_3d").ok);
}

TEST_CASE("pose point frames prioritize third point and reject nonorthogonal degeneracy") {
  test::OpCall c;
  c.inputs = {
      {"a", pose3("a", {0, 0, 0})}, {"b", pose3("b", {1, 0, 0})}, {"c", pose3("c", {0, 0, 1})}};
  REQUIRE(c.run("pose.frame_from_points", {{"origin", Value::text("first")}}).ok);
  auto r = poses::rotationMatrix(first(c)["rotation"].get<poses::Quaternion>());
  CHECK(r[0] == doctest::Approx(1));
  CHECK(r[5] == doctest::Approx(-1));
  CHECK(r[7] == doctest::Approx(1));
  CHECK(first(c)["position"] == Json::array({0, 0, 0}));
  c.inputs.erase("c");
  c.inputs["b"] = pose3("b", {1, 0, 1});
  REQUIRE(c.run("pose.frame_from_points").ok);
  r = poses::rotationMatrix(first(c)["rotation"].get<poses::Quaternion>());
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) {
      double product = 0;
      for (int k = 0; k < 3; ++k)
        product += r[k * 3 + i] * r[k * 3 + j];
      CHECK(product == doctest::Approx(i == j ? 1 : 0));
    }
  c.inputs["b"] = pose3("b", {0, 0, 1});
  CHECK_FALSE(c.run("pose.frame_from_points").ok);
  c.inputs["b"] = pose3("b", {1, 0, 0});
  c.inputs["c"] = pose3("c", {2, 0, 0});
  CHECK_FALSE(c.run("pose.frame_from_points").ok);
  c.inputs["c"] = pose3("c", {0, 1, 0}, {0, 0, 0, 1}, "wrong");
  CHECK_FALSE(c.run("pose.frame_from_points").ok);
}

TEST_CASE("pose angular comparison and decomposition use explicit units and symmetry") {
  test::OpCall c;
  c.inputs = {{"a", pose2("a", 0, 0, pi / 180)}, {"b", pose2("b", 3, 4, 179 * pi / 180)}};
  REQUIRE(c.run("pose.compare_2d", {{"symmetryPeriod", Value::number(pi)},
                                    {"positionTolerance", Value::number(5)},
                                    {"angleTolerance", Value::number(2.001 * pi / 180)}})
              .ok);
  CHECK(data(c, "matched")["value"] == true);
  CHECK(data(c, "distance")["value"] == 5);
  CHECK(data(c, "angle")["value"].get<double>() == doctest::Approx(-2 * pi / 180));
  c.inputs = {{"poses", pose2("a", 0, 0, 170 * pi / 180)}};
  REQUIRE(c.run("pose.flip_axis_2d").ok);
  CHECK(first(c)["angle"].get<double>() == doctest::Approx(-10 * pi / 180));
  c.inputs["poses"] = c.outputs.at("poses");
  REQUIRE(c.run("pose.decompose_2d").ok);
  CHECK(data(c, "angle")["unit"] == "rad");
  CHECK(data(c, "width")["unit"] == "px");
  CHECK(data(c, "width")["value"] == 4);
  CHECK_FALSE(c.run("pose.decompose_2d", {{"index", Value::integer(1)}}).ok);
}

TEST_CASE(
    "pose complete JSON text roundtrip preserves metadata rejects malformed and wrong versions") {
  test::OpCall c;
  for (const Data& input :
       {pose2(), pose3("id", {0.12345678901234566, -3, 4}, {0.1, 0.2, 0.3, 0.4})}) {
    c.inputs = {{"poses", input}};
    REQUIRE(c.run("pose.to_text").ok);
    const Data serialized = c.outputs.at("text");
    c.inputs = {{"text", serialized}};
    REQUIRE(c.run("pose.from_text").ok);
    CHECK(c.outputs.at("poses").asRecord()->type == input.asRecord()->type);
    CHECK(c.outputs.at("poses").asRecord()->data == input.asRecord()->data);
  }
  for (const char* text : {"{}", "{\"type\":\"vision.Pose3D.v2\",\"data\":{}}", "[", "null"}) {
    c.inputs = {{"text", Data::record(values::string(text))}};
    CHECK_FALSE(c.run("pose.from_text").ok);
  }
  auto corrupt = pose3("id", {1, 2, 3}).asRecord()->data;
  corrupt["rotation"] = {0, 0, 0, 2};
  c.inputs = {{"poses", Data::record(Record{poses::kPose3DType, corrupt})}};
  CHECK_FALSE(c.run("pose.to_text").ok);
  poses::Quaternion invalid{0, 0, std::numeric_limits<double>::quiet_NaN(), 1};
  CHECK_FALSE(poses::normalizeQuaternion(invalid));
  const auto rotation = poses::rotationMatrix({0, 0, 0.7071068, 0.7071068});
  CHECK(std::abs(rotation[0] * rotation[0] + rotation[3] * rotation[3] - 1) < 1e-12);
}

TEST_CASE(
    "pose known plane mapping retains identity and requires explicit physical sizes and frames") {
  test::OpCall c;
  Record calib{calibration::kPlaneType, {{"H", {0, -2, 10, 3, 0, 20, 0, 0, 1}}, {"unit", "mm"}}};
  Transform world = quarterTurn(1, 2);
  c.inputs = {{"poses", pose2("part", 4, 5, 0)},
              {"calib", Data::record(calib)},
              {"planeToWorld", Data::transform(world)}};
  REQUIRE(c.run("pose.to_world_plane", {{"size3D", Value::vec({0.1, 0.2, 0.03})}}).ok);
  CHECK(first(c)["id"] == "part");
  CHECK(first(c)["position"][0].get<double>() == doctest::Approx(0.968));
  CHECK(first(c)["position"][1].get<double>() == doctest::Approx(2));
  CHECK(first(c)["position"][2] == 0);
  CHECK(first(c)["size"] == Json::array({0.1, 0.2, 0.03}));
  CHECK(first(c)["unit"] == "m");
  CHECK(first(c)["frame"] == "world");
  const auto r = poses::rotationMatrix(first(c)["rotation"].get<poses::Quaternion>());
  CHECK(r[0] == doctest::Approx(-1));
  const Data projected = c.outputs.at("poses");
  c.inputs = {{"poses", projected}};
  REQUIRE(c.run("pose.to_transform").ok);
  CHECK(c.outputs.at("transform").asTransform()->m[3] == doctest::Approx(0.968));
  c.inputs["poses"] = pose2();
  CHECK_FALSE(c.run("pose.to_transform").ok);
}

TEST_CASE(
    "geometry similarity transform updates endpoints directions and circles without mutating inputs") {
  test::OpCall c;
  Point2D p;
  p.p[0] = 1;
  p.p[1] = 2;
  c.inputs = {{"geometry", Data::point2d(p)}, {"transform", Data::transform(quarterTurn(10, 20))}};
  REQUIRE(c.run("geometry.transform_2d").ok);
  CHECK(c.outputs.at("geometry").asPoint2D()->p[0] == 8);
  CHECK(c.outputs.at("geometry").asPoint2D()->p[1] == 21);
  CHECK(c.inputs.at("geometry").asPoint2D()->p[0] == 1);
  Line2D line;
  line.hasSegment = true;
  line.start[0] = 1;
  line.end[0] = 3;
  c.inputs["geometry"] = Data::line2d(line);
  REQUIRE(c.run("geometry.transform_2d").ok);
  const auto* result = c.outputs.at("geometry").asLine2D();
  CHECK(result->dir[0] == doctest::Approx(0));
  CHECK(result->dir[1] == doctest::Approx(1));
  CHECK(result->start[1] == 21);
  CHECK(result->end[1] == 23);
  Circle2D circle;
  circle.radius = 2;
  c.inputs["geometry"] = Data::circle2d(circle);
  Transform scaled;
  scaled.m[0] = scaled.m[5] = 3;
  c.inputs["transform"] = Data::transform(scaled);
  REQUIRE(c.run("geometry.transform_2d").ok);
  CHECK(c.outputs.at("geometry").asCircle2D()->radius == 6);
  scaled.m[5] = 2;
  c.inputs["transform"] = Data::transform(scaled);
  CHECK_FALSE(c.run("geometry.transform_2d").ok);
}

TEST_CASE("geometry plane transforms use inverse transpose and reject singular affine matrices") {
  test::OpCall c;
  Plane p;
  p.normal[0] = 1;
  p.normal[2] = 0;
  p.d = -1;
  Transform transform;
  transform.m[1] = 1;
  c.inputs = {{"plane", Data::plane(p)}, {"transform", Data::transform(transform)}};
  REQUIRE(c.run("geometry.transform_plane").ok);
  const auto* result = c.outputs.at("plane").asPlane();
  CHECK(result->normal[0] == doctest::Approx(std::sqrt(0.5)));
  CHECK(result->normal[1] == doctest::Approx(-std::sqrt(0.5)));
  CHECK(result->d == doctest::Approx(-std::sqrt(0.5)));
  transform = Transform{};
  transform.m[0] = 2;
  transform.m[3] = 3;
  c.inputs["transform"] = Data::transform(transform);
  REQUIRE(c.run("geometry.transform_plane").ok);
  CHECK(c.outputs.at("plane").asPlane()->d == doctest::Approx(-5));
  transform.m[0] = 0;
  c.inputs["transform"] = Data::transform(transform);
  CHECK_FALSE(c.run("geometry.transform_plane").ok);
  transform.m[15] = 2;
  c.inputs["transform"] = Data::transform(transform);
  CHECK_FALSE(c.run("geometry.transform_plane").ok);
  transform = Transform{};
  transform.m[0] = 1e-13f;
  c.inputs["transform"] = Data::transform(transform);
  CHECK_FALSE(c.run("geometry.transform_plane").ok);
}

TEST_CASE("geometry plane offsets retain small terms through large affine cancellation") {
  const float large = 1e20f;
  for (int axis = 0; axis < 3; ++axis) {
    const int second = (axis + 1) % 3, third = (axis + 2) % 3;
    for (const float sign : {-1.f, 1.f}) {
      Plane plane;
      std::fill_n(plane.normal, 3, 0.f);
      plane.normal[axis] = sign;
      plane.d = sign * large;
      Transform matrix;
      matrix.m[axis * 4 + second] = 1;
      matrix.m[axis * 4 + third] = 1;
      matrix.m[axis * 4 + 3] = large;
      matrix.m[second * 4 + 3] = 2;
      matrix.m[third * 4 + 3] = 3;
      test::OpCall c;
      c.inputs = {{"plane", Data::plane(plane)}, {"transform", Data::transform(matrix)}};
      REQUIRE(c.run("geometry.transform_plane").ok);
      const auto* result = c.outputs.at("plane").asPlane();
      CHECK(result->normal[axis] == doctest::Approx(sign / std::sqrt(3.0)));
      CHECK(result->normal[second] == doctest::Approx(-sign / std::sqrt(3.0)));
      CHECK(result->normal[third] == doctest::Approx(-sign / std::sqrt(3.0)));
      CHECK(result->d == doctest::Approx(sign * 5 / std::sqrt(3.0)));
    }
  }
}

TEST_CASE("geometry point transforms retain affine translation cancellation") {
  const float large = 1e20f;
  Point2D point;
  point.p[0] = -large;
  point.p[1] = 3;
  point.unit = Unit2D::Meter;
  Transform matrix;
  matrix.m[0] = matrix.m[5] = 1;
  matrix.m[1] = -1;
  matrix.m[4] = 1;
  matrix.m[3] = large;
  matrix.m[7] = large;
  test::OpCall c;
  c.inputs = {{"geometry", Data::point2d(point)}, {"transform", Data::transform(matrix)}};
  REQUIRE(c.run("geometry.transform_2d").ok);
  const auto* result = c.outputs.at("geometry").asPoint2D();
  CHECK(result->p[0] == -3);
  CHECK(result->p[1] == 3);
}

TEST_CASE("pose scheduling composes frame transform then local offset without changing order") {
  std::ifstream example(std::filesystem::path(LYFLOW_EXAMPLES_DIR) / "p1-pose-flow.lyflow.json");
  REQUIRE(example.good());
  const auto graph = Json::parse(example);
  test::Session session(graph);
  const auto& log = session.wait();
  REQUIRE(log.runStatus() == "ok");
  Data result;
  REQUIRE(exec::ResultStore::instance().get(session.runId(), "restored", "poses", result));
  const auto& item = result.asRecord()->data["items"][0];
  CHECK(item["position"][0].get<double>() == doctest::Approx(10));
  CHECK(item["position"][1].get<double>() == doctest::Approx(2));
  CHECK(item["frame"] == "world");
}
