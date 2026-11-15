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
