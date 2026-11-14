#include <doctest/doctest.h>

#include <cmath>
#include <limits>

#include "helpers.h"
#include "lyflow/value.h"

using namespace lyflow;

namespace {

Data point(float x, float y, Unit2D unit = Unit2D::Meter) {
  Point2D p;
  p.p[0] = x;
  p.p[1] = y;
  p.unit = unit;
  return Data::point2d(p);
}
Data line(float x, float y, float dx, float dy) {
  Line2D l;
  l.point[0] = x;
  l.point[1] = y;
  l.dir[0] = dx;
  l.dir[1] = dy;
  return Data::line2d(l);
}
Data segment(float x, float y, float xx, float yy) {
  Line2D l;
  l.start[0] = x;
  l.start[1] = y;
  l.end[0] = xx;
  l.end[1] = yy;
  l.hasSegment = true;
  return Data::line2d(l);
}
Data circle(float x, float y, float r) {
  Circle2D c;
  c.center[0] = x;
  c.center[1] = y;
  c.radius = r;
  return Data::circle2d(c);
}

const nlohmann::json& intersections(test::OpCall& call, Data a, Data b) {
  call.inputs = {{"a", std::move(a)}, {"b", std::move(b)}};
  const auto s = call.run("geometry.intersect_2d");
  REQUIRE_MESSAGE(s.ok, s.message);
  return call.outputs.at("intersections").asRecord()->data;
}

double distance(Data a, Data b) {
  test::OpCall c;
  c.inputs = {{"a", std::move(a)}, {"b", std::move(b)}};
  const Status s = c.run("geometry.distance_2d");
  REQUIRE_MESSAGE(s.ok, s.message);
  const Measurement* m = c.outputs.at("distance").asMeasurement();
  REQUIRE(m != nullptr);
  CHECK(m->ok);
  CHECK(m->unit == "m");
  return m->value;
}

} // namespace

TEST_CASE("geometry constructors normalize geometry without changing plane equation") {
  test::OpCall c;
  REQUIRE(
      c.run("geometry.point_2d", {{"position", Value::vec({3, 4})}, {"unit", Value::text("px")}})
          .ok);
  CHECK(c.outputs.at("point").asPoint2D()->unit == Unit2D::Pixel);
  CHECK(c.outputs.at("point").asPoint2D()->p[1] == 4);
  REQUIRE(c.run("geometry.line_2d", {{"direction", Value::vec({3, 4})}}).ok);
  CHECK(c.outputs.at("line").asLine2D()->dir[0] == doctest::Approx(0.6));
  REQUIRE(c.run("geometry.line_2d", {{"mode", Value::text("segment")},
                                     {"start", Value::vec({2, 3})},
                                     {"end", Value::vec({2, 7})}})
              .ok);
  const Line2D* l = c.outputs.at("line").asLine2D();
  REQUIRE(l->hasSegment);
  CHECK(l->point[0] == 2);
  CHECK(l->start[1] == 3);
  CHECK(l->end[1] == 7);
  CHECK(l->dir[1] == 1);
  REQUIRE(
      c.run("geometry.circle_2d", {{"center", Value::vec({1, 2})}, {"radius", Value::number(3)}})
          .ok);
  CHECK(c.outputs.at("circle").asCircle2D()->radius == 3);
  REQUIRE(
      c.run("geometry.plane", {{"normal", Value::vec({0, 0, 2})}, {"offset", Value::number(-6)}})
          .ok);
  const Plane* plane = c.outputs.at("plane").asPlane();
  CHECK(plane->normal[2] == 1);
  CHECK(plane->d == -3);
}

TEST_CASE("geometry rejects invalid constants and unrepresentable values") {
  test::OpCall c;
  CHECK_FALSE(c.run("geometry.line_2d", {{"direction", Value::vec({0, 0})}}).ok);
  CHECK_FALSE(c.run("geometry.line_2d", {{"mode", Value::text("segment")},
                                         {"start", Value::vec({1, 1})},
                                         {"end", Value::vec({1, 1})}})
                  .ok);
  CHECK_FALSE(c.run("geometry.point_2d", {{"position", Value::vec({1e100, 0})}}).ok);
  for (double r : {0.0, -1.0, 1e100, 1e-100, std::numeric_limits<double>::infinity()})
    CHECK_FALSE(c.run("geometry.circle_2d", {{"radius", Value::number(r)}}).ok);
  CHECK_FALSE(c.run("geometry.plane", {{"normal", Value::vec({0, 0, 0})}}).ok);
  CHECK_FALSE(c.run("geometry.plane", {{"offset", Value::number(1e100)}}).ok);
}

TEST_CASE("geometry distance covers all point line circle combinations symmetrically") {
  const Data a[] = {point(0, 3), line(0, 0, 2, 0), circle(0, 0, 1)};
  const Data b[] = {point(4, 3), line(0, 5, 1, 0), circle(5, 0, 1)};
  const double expected[3][3] = {{4, 2, std::sqrt(34.0) - 1}, {3, 5, 0}, {4, 4, 3}};
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      CAPTURE(i);
      CAPTURE(j);
      CHECK(distance(a[i], b[j]) == doctest::Approx(expected[i][j]));
      CHECK(distance(b[j], a[i]) == doctest::Approx(expected[i][j]));
    }
  }
}

TEST_CASE("geometry segment distances respect finite endpoints and circle boundaries") {
  CHECK(distance(point(4, 2), segment(0, 0, 2, 0)) == doctest::Approx(std::sqrt(8.0)));
  CHECK(distance(segment(0, 0, 1, 0), segment(2, -1, 2, 1)) == doctest::Approx(1));
  CHECK(distance(segment(0, 0, 3, 0), segment(2, -1, 2, 1)) == doctest::Approx(0));
  CHECK(distance(segment(0, 0, 1, 0), line(2, -1, 0, 1)) == doctest::Approx(1));
  CHECK(distance(segment(0, 0, 3, 0), segment(1, 0, 2, 0)) == doctest::Approx(0));
  CHECK(distance(segment(-1, 0, 1, 0), circle(0, 0, 3)) == doctest::Approx(2));
  CHECK(distance(circle(0, 0, 3), circle(0, 0, 1)) == doctest::Approx(2));
  CHECK(distance(circle(0, 0, 3), circle(2, 0, 1)) == doctest::Approx(0));
  CHECK(distance(line(0, 0, 1, 0), line(0, 1, 1, 1e-13f)) == doctest::Approx(0));
  CHECK(distance(line(0, 1, 1, 1e-13f), line(0, 0, 1, 0)) == doctest::Approx(0));
}

TEST_CASE("geometry intersections distinguish finite empty and continuous overlap") {
  test::OpCall c;
  auto r = intersections(c, line(0, 0, 1, 0), line(2, -1, 0, 1));
  CHECK(r["relation"] == "finite");
  CHECK(r["points"] == nlohmann::json::array({{2.0, 0.0}}));
  CHECK(intersections(c, line(0, 0, 1, 0), line(0, 1, 1, 0))["relation"] == "empty");
  CHECK(intersections(c, line(0, 0, 1, 0), line(0, 1, -1, 0))["relation"] == "empty");
  CHECK(intersections(c, line(0, 0, 1, 0), line(2, 0, -1, 0))["relation"] == "coincident");
  CHECK(intersections(c, segment(0, 0, 2, 0), segment(1, 0, 3, 0))["relation"] == "coincident");
  CHECK(intersections(c, segment(0, 0, 2, 0), segment(2, 0, 3, 0))["points"].size() == 1);
  CHECK(intersections(c, segment(0, 0, 1, 0), segment(2, 0, 3, 0))["relation"] == "empty");
  CHECK(intersections(c, segment(0, 0, 1, 0), line(2, -1, 0, 1))["relation"] == "empty");
  r = intersections(c, line(0, 0, 1, 0), line(0, 1, 1, 0.001f));
  REQUIRE(r["points"].size() == 1);
  CHECK(r["points"][0][0].get<double>() == doctest::Approx(-1000).epsilon(1e-6));
}

TEST_CASE(
    "geometry circle intersections handle tangency containment ordering and segment clipping") {
  test::OpCall c;
  auto r = intersections(c, line(0, 0, 3, 0), circle(0, 0, 2));
  CHECK(r["points"] == nlohmann::json::array({{-2.0, 0.0}, {2.0, 0.0}}));
  CHECK(intersections(c, line(0, 2, 1, 0), circle(0, 0, 2))["points"].size() == 1);
  CHECK(intersections(c, line(0, 3, 1, 0), circle(0, 0, 2))["relation"] == "empty");
  CHECK(intersections(c, segment(0, 0, 3, 0), circle(0, 0, 2))["points"].size() == 1);
  CHECK(intersections(c, segment(-1, 0, 1, 0), circle(0, 0, 2))["relation"] == "empty");
  CHECK(intersections(c, circle(0, 0, 2), circle(0, 0, 2))["relation"] == "coincident");
  CHECK(intersections(c, circle(0, 0, 2), circle(0, 0, 1))["relation"] == "empty");
  CHECK(intersections(c, circle(0, 0, 2), circle(3, 0, 1))["points"].size() == 1);
  CHECK(intersections(c, circle(0, 0, 2), circle(1, 0, 1))["points"].size() == 1);
  r = intersections(c, circle(0, 0, 2), circle(2, 0, 2));
  REQUIRE(r["points"].size() == 2);
  CHECK(r["points"][0][0].get<double>() == doctest::Approx(1));
  CHECK(r["points"][0][1].get<double>() == doctest::Approx(-std::sqrt(3.0)));
}

TEST_CASE("geometry point membership midpoint and pixel unit survive roundtrip") {
  test::OpCall c;
  CHECK(intersections(c, point(2, 0), circle(0, 0, 2))["points"].size() == 1);
  CHECK(intersections(c, point(2, 0), segment(0, 0, 1, 0))["relation"] == "empty");
  CHECK(intersections(c, point(2, 0), line(0, 0, 1, 0))["points"].size() == 1);
  CHECK(intersections(c, point(2, 0), point(2, 0))["points"].size() == 1);
  c.inputs = {{"a", point(2, 4, Unit2D::Pixel)}, {"b", point(4, 8, Unit2D::Pixel)}};
  REQUIRE(c.run("geometry.midpoint_2d").ok);
  const Point2D* p = c.outputs.at("point").asPoint2D();
  CHECK(p->p[0] == 3);
  CHECK(p->p[1] == 6);
  CHECK(p->unit == Unit2D::Pixel);
  intersections(c, point(2, 0, Unit2D::Pixel), point(2, 0, Unit2D::Pixel));
  const Data record = c.outputs.at("intersections");
  c.inputs = {{"intersections", record}};
  REQUIRE(c.run("geometry.point_at").ok);
  CHECK(c.outputs.at("point").asPoint2D()->unit == Unit2D::Pixel);
  CHECK(c.outputs.at("point").asPoint2D()->p[0] == 2);
}

TEST_CASE("geometry near-parallel intersections agree with distance and point extraction") {
  for (float slope : {1e-13f, -1e-13f}) {
    const double expected = -1.0 / static_cast<double>(slope);
    const Data horizontal = line(0, 0, 1, 0);
    const Data tilted = line(0, 1, 1, slope);
    for (bool reverse : {false, true}) {
      test::OpCall c;
      const auto result =
          intersections(c, reverse ? tilted : horizontal, reverse ? horizontal : tilted);
      REQUIRE(result["relation"] == "finite");
      REQUIRE(result["points"].size() == 1);
      CHECK(result["points"][0][0].get<double>() == doctest::Approx(expected));
      CHECK(result["points"][0][1].get<double>() == doctest::Approx(0));
      const Data payload = c.outputs.at("intersections");
      c.inputs = {{"intersections", payload}};
      REQUIRE(c.run("geometry.point_at").ok);
      const auto* p = c.outputs.at("point").asPoint2D();
      REQUIRE(p != nullptr);
      CHECK(p->p[0] == doctest::Approx(expected).epsilon(1e-6));
      CHECK(p->p[1] == doctest::Approx(0));
      CHECK(distance(horizontal, tilted) == 0);
    }
  }
}

TEST_CASE("geometry parallel predicates preserve raw directions and segment endpoints") {
  struct Case {
    Data a, b;
    const char* relation;
    double expectedDistance;
  };
  const std::vector<Case> cases = {
      {segment(0, 0, 1, 3), segment(0, 0, 5, 15), "coincident", 0},
      {segment(0, 0, 1, 3), segment(5, 15, 0, 0), "coincident", 0},
      {segment(0, 0, 5, 15), segment(1, 3, 2, 6), "coincident", 0},
      {segment(0, 0, 1, 3), segment(2, 6, 5, 15), "empty", std::sqrt(10.0)},
      {segment(1, 3, 0, 0), segment(5, 15, 2, 6), "empty", std::sqrt(10.0)},
      {segment(0, 0, 1, 3), segment(1, 3, 6, 18), "finite", 0},
      {segment(1, 3, 0, 0), segment(6, 18, 1, 3), "finite", 0},
      {segment(0, 0, 1, 3), segment(0, 1, 5, 16), "empty", 1 / std::sqrt(10.0)},
      {line(0, 0, 1, 3), line(2, 6, 5, 15), "coincident", 0},
      {line(0, 0, 1, 3), line(2, 6, -5, -15), "coincident", 0},
      {line(0, 0, 1, 3), line(0, 1, 5, 15), "empty", 1 / std::sqrt(10.0)},
      {line(0, 0, 1, 3), line(0, 1, -5, -15), "empty", 1 / std::sqrt(10.0)},
  };
  for (std::size_t i = 0; i < cases.size(); ++i) {
    CAPTURE(i);
    const auto& item = cases[i];
    for (bool swap : {false, true}) {
      CAPTURE(swap);
      test::OpCall call;
      const Data a = swap ? item.b : item.a, b = swap ? item.a : item.b;
      const auto result = intersections(call, a, b);
      CHECK(result["relation"] == item.relation);
      CHECK(result["points"].size() == (std::string(item.relation) == "finite" ? 1 : 0));
      CHECK(distance(a, b) == doctest::Approx(item.expectedDistance).epsilon(1e-12));
    }
  }
  test::OpCall call;
  const Data a = segment(1, 0, 1e20f, 1e20f), b = segment(0, 0, 1e20f, 1e20f);
  const auto result = intersections(call, a, b);
  REQUIRE(result["relation"] == "finite");
  REQUIRE(result["points"].size() == 1);
  CHECK(result["points"][0][0].get<double>() == static_cast<double>(1e20f));
  CHECK(result["points"][0][1].get<double>() == static_cast<double>(1e20f));
  CHECK(distance(a, b) == doctest::Approx(0));
}

TEST_CASE("geometry rejects invalid input types units nonfinite and degenerate geometry") {
  test::OpCall c;
  for (const std::string op :
       {"geometry.distance_2d", "geometry.intersect_2d", "geometry.midpoint_2d"}) {
    c.inputs = {{"a", point(0, 0)}, {"b", point(0, 0, Unit2D::Pixel)}};
    CHECK_FALSE(c.run(op).ok);
    c.inputs = {{"a", Data::record({"test", {}})}, {"b", point(0, 0)}};
    CHECK_FALSE(c.run(op).ok);
    c.inputs = {{"a", point(std::numeric_limits<float>::quiet_NaN(), 0)}, {"b", point(0, 0)}};
    CHECK_FALSE(c.run(op).ok);
  }
  for (const Data invalid : {line(0, 0, 0, 0), segment(0, 0, 0, 0), circle(0, 0, -1)}) {
    c.inputs = {{"a", invalid}, {"b", point(0, 0)}};
    CHECK_FALSE(c.run("geometry.distance_2d").ok);
  }
  c.inputs = {{"a", point(0, 0)}, {"b", point(0, 0)}};
  CHECK_FALSE(c.run("geometry.intersect_2d", {{"tolerance", Value::number(0)}}).ok);
}

TEST_CASE("geometry point_at validates the whole record before indexing") {
  const nlohmann::json valid = {{"unit", "m"}, {"relation", "finite"}, {"points", {{1, 2}}}};
  std::vector<nlohmann::json> invalid;
  for (const char* field : {"unit", "relation", "points"}) {
    auto item = valid;
    item.erase(field);
    invalid.push_back(item);
  }
  auto item = valid;
  item["points"] = {{1, 2}, {nullptr, 2}};
  invalid.push_back(item);
  item = valid;
  item["relation"] = "coincident";
  invalid.push_back(item);
  item = valid;
  item["unit"] = "mm";
  invalid.push_back(item);
  item = valid;
  item["points"] = {{1, 2}, {3, 4}, {5, 6}};
  invalid.push_back(item);
  test::OpCall c;
  for (const auto& data : invalid) {
    c.inputs = {{"intersections", Data::record({"geometry.Intersections2D.v1", data})}};
    CHECK_FALSE(c.run("geometry.point_at").ok);
  }
  c.inputs = {{"intersections", Data::record({"geometry.Intersections2D.v1", valid})}};
  CHECK_FALSE(c.run("geometry.point_at", {{"index", Value::integer(-1)}}).ok);
  CHECK_FALSE(c.run("geometry.point_at", {{"index", Value::integer(1)}}).ok);
  c.inputs = {{"intersections",
               Data::record(
                   {"geometry.Intersections2D.v1",
                    {{"unit", "m"}, {"relation", "empty"}, {"points", nlohmann::json::array()}}})}};
  const Status s = c.run("geometry.point_at");
  CHECK_FALSE(s.ok);
  CHECK(s.code == "index_out_of_range");
}

TEST_CASE("geometry mixed native types run through real scheduling and record contracts") {
  auto doc = test::makeGraph({{"point", "geometry.point_2d", {{"position", {0, 3}}}},
                              {"circle", "geometry.circle_2d", {{"radius", 1}}},
                              {"distance", "geometry.distance_2d"},
                              {"line", "geometry.line_2d"},
                              {"intersection", "geometry.intersect_2d"},
                              {"select", "geometry.point_at"}},
                             {{"point.point", "distance.a"},
                              {"circle.circle", "distance.b"},
                              {"line.line", "intersection.a"},
                              {"circle.circle", "intersection.b"},
                              {"intersection.intersections", "select.intersections"}});
  const auto log = test::runGraph(doc);
  CHECK(log.runStatus() == "ok");
  CHECK(log.finalState("distance") == "done");
  CHECK(log.finalState("select") == "done");
  CHECK(log.seqIsDense());
}

TEST_CASE("geometry dynamic scalar overrides require explicit coordinate and angle units") {
  test::OpCall c;
  c.inputs = {{"x", Data::record(values::number(2, "px"))},
              {"y", Data::record(values::number(3, "px"))}};
  REQUIRE(c.run("geometry.point_2d", {{"unit", Value::text("px")}}).ok);
  CHECK(c.outputs.at("point").asPoint2D()->p[0] == 2);
  CHECK(c.outputs.at("point").asPoint2D()->p[1] == 3);
  CHECK_FALSE(c.run("geometry.point_2d").ok);
  c.inputs = {{"x", Data::record(values::number(1, "m"))},
              {"angle", Data::record(values::number(90, "deg"))}};
  REQUIRE(c.run("geometry.line_2d").ok);
  CHECK(c.outputs.at("line").asLine2D()->point[0] == 1);
  CHECK(c.outputs.at("line").asLine2D()->dir[1] == doctest::Approx(1));
  c.inputs["angle"] = Data::record(values::number(3.14159265358979323846 / 2, "rad"));
  REQUIRE(c.run("geometry.line_2d").ok);
  CHECK(c.outputs.at("line").asLine2D()->dir[1] == doctest::Approx(1));
  CHECK_FALSE(c.run("geometry.line_2d", {{"mode", Value::text("segment")}}).ok);
  c.inputs["angle"] = Data::record(values::number(90, ""));
  CHECK_FALSE(c.run("geometry.line_2d").ok);
  c.inputs = {{"radius", Data::record(values::number(3, "m"))}};
  REQUIRE(c.run("geometry.circle_2d").ok);
  CHECK(c.outputs.at("circle").asCircle2D()->radius == 3);
  c.inputs["radius"] = Data::record(values::number(-1, "m"));
  CHECK_FALSE(c.run("geometry.circle_2d").ok);
  c.inputs["radius"] = Data::record(values::number(3, "mm"));
  CHECK_FALSE(c.run("geometry.circle_2d").ok);
  c.inputs = {{"x", Data::record(values::number(1.00000001, "m"))}};
  const Status collapsed =
      c.run("geometry.line_2d", {{"mode", Value::text("segment")}, {"end", Value::vec({1, 0})}});
  CHECK_FALSE(collapsed.ok);
  CHECK(collapsed.code == "bad_param");
  CHECK(c.outputs.empty());
  c.inputs = {{"x", Data::record(values::number(100000000.1, "m"))}};
  REQUIRE(c.run("geometry.line_2d",
                {{"mode", Value::text("segment")}, {"end", Value::vec({100000008, 1})}})
              .ok);
  const Line2D* rounded = c.outputs.at("line").asLine2D();
  REQUIRE(rounded != nullptr);
  CHECK(rounded->dir[0] == doctest::Approx(8 / std::sqrt(65.0)).epsilon(1e-7));
  CHECK(rounded->dir[1] == doctest::Approx(1 / std::sqrt(65.0)).epsilon(1e-7));
  const Data produced = c.outputs.at("line");
  c.inputs = {{"a", produced}, {"b", point(100000000, 0)}};
  REQUIRE(c.run("geometry.distance_2d").ok);
  CHECK(c.outputs.at("distance").asMeasurement()->value == doctest::Approx(0));
}

TEST_CASE("geometry distance distinguishes minimum signed clearance and line anchor conventions") {
  test::OpCall c;
  c.inputs = {{"a", circle(0, 0, 3)}, {"b", circle(0, 0, 1)}};
  REQUIRE(c.run("geometry.distance_2d", {{"mode", Value::text("signed_clearance")}}).ok);
  CHECK(c.outputs.at("distance").asMeasurement()->value == doctest::Approx(-4));
  REQUIRE(c.run("geometry.distance_2d").ok);
  CHECK(c.outputs.at("distance").asMeasurement()->value == doctest::Approx(2));
  c.inputs = {{"a", circle(0, 0, 3)}, {"b", line(0, 1, 1, 0)}};
  REQUIRE(c.run("geometry.distance_2d", {{"mode", Value::text("signed_clearance")}}).ok);
  CHECK(c.outputs.at("distance").asMeasurement()->value == doctest::Approx(-2));
  REQUIRE(c.run("geometry.distance_2d", {{"useFirstCenter", Value::boolean(true)}}).ok);
  CHECK(c.outputs.at("distance").asMeasurement()->value == doctest::Approx(1));
  c.inputs = {{"a", line(0, 0, 1, 0)}, {"b", line(2, -1, 0, 1)}};
  REQUIRE(c.run("geometry.distance_2d", {{"mode", Value::text("first_anchor")}}).ok);
  CHECK(c.outputs.at("distance").asMeasurement()->value == doctest::Approx(2));
  REQUIRE(c.run("geometry.distance_2d").ok);
  CHECK(c.outputs.at("distance").asMeasurement()->value == doctest::Approx(0));
  CHECK_FALSE(c.run("geometry.distance_2d", {{"mode", Value::text("signed_clearance")}}).ok);
}

TEST_CASE("geometry point projection covers perpendicular foot segment clamping and extension") {
  test::OpCall c;
  c.inputs = {{"point", point(4, 3)}, {"line", line(0, 0, 2, 0)}};
  REQUIRE(c.run("geometry.project_point_2d").ok);
  CHECK(c.outputs.at("point").asPoint2D()->p[0] == 4);
  CHECK(c.outputs.at("point").asPoint2D()->p[1] == 0);
  c.inputs["line"] = segment(0, 0, 2, 0);
  REQUIRE(c.run("geometry.project_point_2d").ok);
  CHECK(c.outputs.at("point").asPoint2D()->p[0] == 2);
  REQUIRE(c.run("geometry.project_point_2d", {{"clampToSegment", Value::boolean(false)}}).ok);
  CHECK(c.outputs.at("point").asPoint2D()->p[0] == 4);
  c.inputs["point"] = point(4, 3, Unit2D::Pixel);
  CHECK_FALSE(c.run("geometry.project_point_2d").ok);
}
