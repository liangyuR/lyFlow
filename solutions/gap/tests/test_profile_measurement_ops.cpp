#include <doctest/doctest.h>

#include <cmath>
#include <limits>

#include "helpers.h"
#include "lyflow/profile.h"
#include "lyflow_profile/context.h"
#include "ops/profile_measurement_ops.h"
#include "ops/profile_feature_bundle.h"
#include "ops/profile_scan.h"

using namespace lyflow;
namespace scene = lyflow::packs::gap::profile_scene;
namespace {
using Json = nlohmann::json;
Data sensor(const std::string& id) {
  PointCloud cloud;
  std::vector<std::uint8_t> validity;
  std::vector<std::uint32_t> sourceIndices;
  for (int i = 0; i < 200; ++i) {
    const float x = -.025F + .00025F * i;
    cloud.push(x, 0, .17F + .02F * x);
    cloud.intensity.push_back(100 + i);
    cloud.rgb.insert(cloud.rgb.end(), {120, 70, 20});
    validity.push_back(i == 5 ? 0 : 1);
    sourceIndices.push_back(static_cast<std::uint32_t>(i));
  }
  Profile result;
  std::string why;
  REQUIRE(makeProfile(std::make_shared<const PointCloud>(std::move(cloud)),
                      {1, "sensor", "m", "XZ", "right", "acquisition", id, 200},
                      std::move(validity), std::move(sourceIndices), result, why));
  return Data::profile(std::move(result));
}
struct Fixture {
  Registry registry;
  Data scan, rois;
  scene::RoiFields fields;
  Fixture() {
    scene::registerProfileBundles(registry);
    scene::registerProfileFeatureBundles(registry);
    scene::registerProfileMeasurementOps(registry);
    test::StubContext ctx;
    scene::ReadScanOptions options;
    options.measurementFrame = "measurement";
    REQUIRE(scene::buildReadScan(sensor("camera-A"), sensor("camera-B"), options, scan, ctx).ok);
    scene::ScanView view;
    std::string why;
    REQUIRE(scene::readScanPair(scan, view, why));
    const float bounds[4][4] = {{-.02F, .165F, -.002F, .175F},
                                {.002F, .165F, .02F, .175F},
                                {-.003F, .169F, -.001F, .171F},
                                {.001F, .169F, .003F, .171F}};
    for (std::size_t i = 0; i < 4; ++i) {
      Box2D box;
      std::copy_n(bounds[i], 2, box.min);
      std::copy_n(bounds[i] + 2, 2, box.max);
      fields.boxes[i] = Data::box2d(box);
      fields.contexts[i] =
          Data::record(Record{"profile.Geometry.v2",
                              profile_contract::geometryContextV2(*view.merged, fields.boxes[i])});
    }
    fields.info = {
        scene::kRoiInfoType,
        {{"schemaVersion", 2},
         {"source", "config"},
         {"datumSide", "left"},
         {"alignment", nullptr},
         {"overallMm", nullptr},
         {"cropStatus", nullptr},
         {"processing",
          {{"method", "template"}, {"status", "complete"}, {"details", Json::object()}}}}};
    REQUIRE(scene::makeRoiSet(fields, rois, why));
  }
  void complete(const std::string& id, const test::OpCall& call) {
    const auto* op = registry.find(id);
    REQUIRE(op != nullptr);
    for (const auto& port : op->outputs) {
      INFO(port.name);
      const auto found = call.outputs.find(port.name);
      REQUIRE(found != call.outputs.end());
      CHECK_FALSE(found->second.empty());
      CHECK(found->second.kind() == kindFromTypeName(port.type));
    }
  }
};
}
TEST_CASE(
    "Native scene role line and line-end reference reuse public fitting with complete outputs") {
  Fixture fixture;
  const auto before = fixture.scan.asBundle()->field("merged")->profilePtr();
  test::OpCall fit(fixture.registry);
  fit.inputs = {{"scan", fixture.scan}, {"rois", fixture.rois}};
  REQUIRE(fit.run("gap.role_line", {{"segmentPoints", Value::integer(0)}}).ok);
  fixture.complete("gap.role_line", fit);
  CHECK(fit.out("profile").asProfile()->pointCount() < before->pointCount());
  CHECK(fit.out("inliers").asIndices()->sourceCloudId == fit.out("profile").asProfile()->cloud->id);
  CHECK(fit.out("profile").asProfile()->cloud->hasRgb());
  CHECK(fit.out("weightedInliers").asIndices()->values.size() >=
        fit.out("inliers").asIndices()->values.size());
  ProfileDescriptor space;
  std::string why;
  REQUIRE(profile_contract::geometryMatchesPhysical(fit.out("line"),
                                                    fit.out("lineContext").asRecord(), space, why));
  CHECK(space.frame == "measurement");
  test::OpCall point(fixture.registry);
  point.inputs = fit.inputs;
  REQUIRE(point
              .run("gap.ref_point",
                   {{"role", Value::text("datum")}, {"segmentPoints", Value::integer(0)}})
              .ok);
  fixture.complete("gap.ref_point", point);
  CHECK(point.out("point").valueJson() == fit.out("innerEnd").valueJson());
  CHECK(point.out("line").valueJson() == fit.out("line").valueJson());
  CHECK(point.out("selection").asRecord()->data["lineFitted"] == true);
  CHECK(fixture.scan.asBundle()->field("merged")->profilePtr() == before);
}

TEST_CASE(
    "Native reference selection keeps full-profile min-corner and cropped infinite-line policies") {
  Fixture fixture;
  auto box = *fixture.fields.boxes[1].asBox2D();
  box.min[0] = .2F;
  box.max[0] = .3F;
  fixture.fields.boxes[1] = Data::box2d(box);
  const auto& measurement = *fixture.scan.asBundle()->field("merged");
  fixture.fields.contexts[1] = Data::record(
      Record{"profile.Geometry.v2",
             profile_contract::geometryContextV2(measurement, fixture.fields.boxes[1])});
  std::string why;
  REQUIRE(scene::makeRoiSet(fixture.fields, fixture.rois, why));
  test::OpCall point(fixture.registry);
  point.inputs = {{"scan", fixture.scan}, {"rois", fixture.rois}};
  REQUIRE(point.run("gap.ref_point", {{"method", Value::text("selected_point")}}).ok);
  fixture.complete("gap.ref_point", point);
  CHECK(point.out("point").asPoint2D()->p[0] < .2F);
  CHECK(point.out("profile").profilePtr() == measurement.profilePtr());
  CHECK(point.out("quality").asRecord()->data["lineFitted"] == false);
  CHECK(point.out("lineContext").asRecord()->type == "gap.GeometryUnavailable.v1");
  CHECK(point.out("lineContext").asRecord()->data["available"] == false);
  ProfileDescriptor unavailableSpace;
  CHECK_FALSE(profile_contract::geometryMatchesPhysical(
      point.out("line"), point.out("lineContext").asRecord(), unavailableSpace, why));
  CHECK_FALSE(point.run("gap.ref_point", {{"method", Value::text("nearest_point")}}).ok);
  CHECK(point.outputs.empty());
  fixture.fields.boxes[1] = Data::box2d(Box2D{{.002F, .165F}, {.02F, .175F}});
  fixture.fields.contexts[1] = Data::record(
      Record{"profile.Geometry.v2",
             profile_contract::geometryContextV2(measurement, fixture.fields.boxes[1])});
  REQUIRE(scene::makeRoiSet(fixture.fields, fixture.rois, why));
  Line2D line;
  line.point[1] = .17035F;
  const auto base = Data::line2d(line);
  point.inputs = {{"scan", fixture.scan},
                  {"rois", fixture.rois},
                  {"baseLine", base},
                  {"baseLineContext",
                   Data::record(Record{"profile.Geometry.v2",
                                       profile_contract::geometryContextV2(measurement, base)})}};
  REQUIRE(point.run("gap.ref_point", {{"method", Value::text("nearest_point")}}).ok);
  fixture.complete("gap.ref_point", point);
  CHECK(point.out("profile").asProfile()->pointCount() < measurement.asProfile()->pointCount());
  CHECK(point.out("point").asPoint2D()->p[0] == doctest::Approx(.0175).epsilon(.02));
  CHECK(point.out("quality").asRecord()->data["model"] == "nearest_point");
  point.inputs.erase("baseLineContext");
  CHECK_FALSE(point.run("gap.ref_point", {{"method", Value::text("nearest_point")}}).ok);
  CHECK(point.outputs.empty());
  CHECK_FALSE(point
                  .validate("gap.ref_point", {{"method", Value::text("nearest_point")}},
                            {"scan", "rois", "baseLine"})
                  .empty());
}

TEST_CASE("Native datum direction uses the long outer window and preserves ROI reference sources") {
  Fixture fixture;
  test::OpCall direction(fixture.registry);
  direction.inputs = {{"scan", fixture.scan}, {"rois", fixture.rois}};
  REQUIRE(direction.run("gap.datum_direction").ok);
  fixture.complete("gap.datum_direction", direction);
  CHECK(direction.out("window").asBox2D()->max[0] < fixture.fields.boxes[2].asBox2D()->min[0]);
  CHECK(direction.out("quality").asRecord()->data["segmentApplied"] == false);
  CHECK(direction.out("profile").asProfile()->cloud->hasIntensity());
  const auto& referenceSources = direction.out("provenance").asRecord()->data["referenceSources"];
  CHECK_FALSE(referenceSources.empty());
  test::OpCall fit(fixture.registry);
  fit.inputs = {{"scan", fixture.scan},
                {"rois", fixture.rois},
                {"refLine", direction.out("line")},
                {"refLineContext", direction.out("lineContext")}};
  REQUIRE(fit.run("gap.role_line",
                  {{"dirMode", Value::text("fixed")}, {"segmentPoints", Value::integer(0)}})
              .ok);
  fixture.complete("gap.role_line", fit);
  CHECK_FALSE(fit.validate("gap.role_line", {{"dirMode", Value::text("fixed")}},
                           {"scan", "rois", "refLine"})
                  .empty());
  fit.inputs["refLineContext"] = fixture.fields.contexts[0];
  CHECK_FALSE(fit.run("gap.role_line", {{"dirMode", Value::text("fixed")}}).ok);
  CHECK(fit.outputs.empty());
}
