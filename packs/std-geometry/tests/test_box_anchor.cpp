#include <doctest/doctest.h>
#include <cmath>
#include <limits>
#include "helpers.h"
namespace lyflow::packs::std_geometry {
void registerBoxAnchor(Registry&);
}
using namespace lyflow;
TEST_CASE("Box anchors preserve units and finite centers without float addition overflow") {
  Registry registry;
  packs::std_geometry::registerBoxAnchor(registry);
  test::OpCall call(registry);
  for (const auto unit : {Unit2D::Meter, Unit2D::Pixel}) {
    Box2D box;
    box.unit = unit;
    box.min[0] = 1;
    box.max[0] = std::nextafter(1.0F, 2.0F);
    box.min[1] = box.max[1] = std::numeric_limits<float>::max();
    call.inputs = {{"box", Data::box2d(box)}};
    REQUIRE(call.run("geometry.box_anchor").ok);
    CHECK(call.out("point").asPoint2D()->p[0] == box.min[0]);
    CHECK(call.out("point").asPoint2D()->unit == unit);
    REQUIRE(call.run("geometry.box_anchor", {{"anchor", Value::text("center")}}).ok);
    CHECK(call.out("point").asPoint2D()->p[0] == 1.0F);
    CHECK(call.out("point").asPoint2D()->p[1] == std::numeric_limits<float>::max());
    REQUIRE(call.run("geometry.box_anchor", {{"anchor", Value::text("maximum")}}).ok);
    CHECK(call.out("point").asPoint2D()->p[0] == box.max[0]);
    box.min[1] = std::numeric_limits<float>::infinity();
    call.inputs["box"] = Data::box2d(box);
    CHECK_FALSE(call.run("geometry.box_anchor").ok);
    CHECK(call.outputs.empty());
    box.min[1] = 2;
    box.max[1] = 1;
    call.inputs["box"] = Data::box2d(box);
    CHECK_FALSE(call.run("geometry.box_anchor").ok);
    CHECK(call.outputs.empty());
  }
}

TEST_CASE("Box anchor cancellation publishes no geometry") {
  struct Cancelled final : ExecContext {
    std::filesystem::path base;
    bool cancelled() const override { return true; }
    void progress(float, std::string_view) override {}
    void log(LogLevel, std::string) override {}
    const std::filesystem::path& baseDir() const override { return base; }
    int threadBudget() const override { return 1; }
  } ctx;
  Registry registry;
  packs::std_geometry::registerBoxAnchor(registry);
  const auto* op = registry.find("geometry.box_anchor");
  REQUIRE(op);
  Box2D box;
  box.max[0] = box.max[1] = 1;
  std::unordered_map<std::string, Data> inputs{{"box", Data::box2d(box)}}, result;
  ParamMap params;
  params["anchor"] = Value::text("center");
  Outputs outputs(result);
  CHECK_FALSE(op->compute(Inputs(inputs), ParamView(params, ctx.base), outputs, ctx).ok);
  CHECK(result.empty());
}
