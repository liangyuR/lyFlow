#include <doctest/doctest.h>

#include "helpers.h"

using namespace lyflow;

TEST_CASE(
    "DTS legacy geometry: explicit provenance repairs millimeter payload without scaling direction") {
  Line2D line;
  line.point[0] = 1000;
  line.point[1] = 2000;
  line.dir[0] = 0;
  line.dir[1] = 1;
  line.hasSegment = true;
  line.start[0] = 1000;
  line.end[0] = 3000;
  test::OpCall call;
  call.inputs["geometry"] = Data::line2d(line);
  CHECK_FALSE(call.run("dts.geometry_to_metric").ok);
  REQUIRE(call.run("dts.geometry_to_metric", {{"sourceContract", Value::text("dts.geometry.v1")},
                                              {"sourceOp", Value::text("dts.pick_metal")}})
              .ok);
  const auto* converted = call.out("geometry").asLine2D();
  REQUIRE(converted);
  CHECK(converted->point[0] == doctest::Approx(1));
  CHECK(converted->point[1] == doctest::Approx(2));
  CHECK(converted->end[0] == doctest::Approx(3));
  CHECK(converted->dir[1] == 1);
  CHECK(call.inputs["geometry"].asLine2D()->point[0] == 1000);
  Point2D point;
  point.p[0] = 40;
  point.unit = Unit2D::Pixel;
  call.inputs["geometry"] = Data::point2d(point);
  CHECK_FALSE(
      call.run("dts.geometry_to_metric", {{"sourceContract", Value::text("dts.geometry.v1")},
                                          {"sourceOp", Value::text("dts.seal_root")}})
          .ok);
  CHECK(call.outputs.empty());
}

TEST_CASE("DTS geometry adapter v2 retains explicit legacy scaling and metric identity") {
  test::OpCall call;
  const auto* op = ensureRegistry().find("dts.geometry_to_metric");
  REQUIRE(op);
  CHECK(op->version == "2.0.0");
  REQUIRE(op->migrations.size() == 1);
  const auto migrated = op->migrations[0].apply(
      {{"sourceContract", "dts.geometry.v1"}, {"sourceOp", "dts.seal_root"}});
  CHECK(migrated["sourceContract"] == "dts.geometry.v1");
  CHECK_THROWS_AS(op->migrations[0].apply({{"sourceContract", "dts.geometry.v2"}}),
                  std::invalid_argument);
  Point2D legacy;
  legacy.p[0] = 1.234567f;
  legacy.p[1] = -8.765432f;
  call.inputs["geometry"] = Data::point2d(legacy);
  REQUIRE(call.run("dts.geometry_to_metric", {{"sourceContract", Value::text("dts.geometry.v1")},
                                              {"sourceOp", Value::text("dts.seal_root")}})
              .ok);
  const Data metric = call.out("geometry");
  CHECK(metric.asPoint2D()->p[0] == static_cast<float>(static_cast<double>(legacy.p[0]) * .001));
  call.inputs["geometry"] = metric;
  REQUIRE(call.run("dts.geometry_to_metric", {{"sourceContract", Value::text("dts.geometry.v2")},
                                              {"sourceOp", Value::text("dts.seal_root")}})
              .ok);
  CHECK(call.out("geometry").asPoint2D()->p[0] == metric.asPoint2D()->p[0]);
  CHECK(call.out("geometry").asPoint2D()->p[1] == metric.asPoint2D()->p[1]);
}
