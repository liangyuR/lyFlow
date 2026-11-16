#include <doctest/doctest.h>
#include <cmath>
#include <limits>

#include "helpers.h"
#include "lyflow/profile.h"
#include "lyflow_profile/features.h"
#include "lyflow_profile/provenance.h"
#include "ops/profile_feature_bundle.h"
#include "ops/profile_regions.h"
#include "ops/profile_scan.h"

namespace lyflow::packs::gap {
void registerResultBundle(Registry&);
}
using namespace lyflow;
namespace scene = lyflow::packs::gap::profile_scene;
namespace {
using Json = nlohmann::json;
Data measure(double value, const char* unit = "mm", bool ok = true) {
  Measurement result;
  result.value = value;
  result.unit = unit;
  result.ok = ok;
  return Data::measurement(result);
}
Data decision(const char* reason, const char* choice = "b") {
  return Data::record(Record{"FallbackChoice", {{"choice", choice}, {"reason", reason}}});
}
Data source(float delta = 0) {
  PointCloud cloud;
  std::vector<std::uint32_t> indices;
  std::vector<std::uint8_t> validity;
  for (int i = 0; i < 100; ++i) {
    const float x = .0002F * i;
    cloud.push(x, 0, .17F + .02F * x + delta);
    cloud.intensity.push_back(static_cast<float>(i));
    indices.push_back(static_cast<std::uint32_t>(2 * i + 1));
    validity.push_back(i == 5 ? 0 : 1);
  }
  Profile profile;
  std::string why;
  REQUIRE(makeProfile(std::make_shared<const PointCloud>(std::move(cloud)),
                      {1, "sensor", "m", "XZ", "right", "acquisition", "physical-camera", 205},
                      std::move(validity), std::move(indices), profile, why));
  return Data::profile(std::move(profile));
}
Data bind(const Data& profile, const Data& geometry) {
  return Data::record(
      Record{"profile.Geometry.v2", profile_contract::geometryContextV2(profile, geometry)});
}
struct Fixture {
  Registry registry;
  Data scan, rois, feature;
  profile_contract::RobustLineResult fit;
  Fixture(float delta = 0, bool crop = false) {
    scene::registerProfileBundles(registry);
    scene::registerProfileFeatureBundles(registry);
    packs::gap::registerResultBundle(registry);
    scene::ReadScanOptions options;
    options.measurementFrame = "measurement";
    test::StubContext ctx;
    REQUIRE(scene::buildReadScan(source(delta), source(delta), options, scan, ctx).ok);
    scene::ScanView view;
    std::string why;
    REQUIRE(scene::readScanPair(scan, view, why));
    if (crop) {
      const auto left = Data::box2d(Box2D{{.004F, .169F}, {.005F, .171F}});
      const auto right = Data::box2d(Box2D{{.008F, .169F}, {.009F, .171F}});
      scene::RollCropOptions cropOptions;
      cropOptions.halfWidth = 2;
      cropOptions.halfHeight = 2;
      cropOptions.minPointsKept = 1;
      scene::RollCropResult result;
      REQUIRE(scene::rollAnchoredCrop(scan, left, *bind(*view.merged, left).asRecord(), right,
                                      *bind(*view.merged, right).asRecord(), cropOptions, result,
                                      ctx)
                  .ok);
      REQUIRE(result.status.data.at("applied") == true);
      scan = result.scan;
      REQUIRE(scene::readScanPair(scan, view, why));
    }
    scene::RoiFields fields;
    for (std::size_t i = 0; i < 4; ++i) {
      fields.boxes[i] = Data::box2d(Box2D{{.001F * static_cast<float>(i), .16F},
                                          {.012F + .001F * static_cast<float>(i), .18F}});
      fields.contexts[i] = bind(*view.merged, fields.boxes[i]);
    }
    fields.info = {
        scene::kRoiInfoType,
        {{"schemaVersion", 2},
         {"source", "model"},
         {"datumSide", "left"},
         {"alignment", nullptr},
         {"overallMm", nullptr},
         {"cropStatus", nullptr},
         {"processing",
          {{"method", "assemble"}, {"status", "complete"}, {"details", Json::object()}}}}};
    REQUIRE(scene::makeRoiSet(fields, rois, why));
    const auto box = Data::box2d(Box2D{{-.001F, .16F}, {.021F, .18F}});
    Point2D end;
    end.p[0] = .021F;
    end.p[1] = .17F;
    const auto toward = Data::point2d(end);
    ParamMap parameters;
    for (const auto& parameter : profile_contract::robustLineParams())
      parameters[parameter.name] = parameter.def;
    parameters["segmentPoints"] = Value::integer(0);
    REQUIRE(profile_contract::fitRobustLine({*view.merged,
                                             box,
                                             bind(*view.merged, box),
                                             toward,
                                             bind(*view.merged, toward),
                                             {},
                                             {},
                                             Data::record(*view.mergedProvenance)},
                                            ParamView(parameters, ctx.baseDir()), fit, ctx)
                .ok);
    REQUIRE(scene::makeLineFeature({fit.profile, fit.line, fit.lineContext, fit.innerEnd,
                                    fit.pointContext, fit.inliers, fit.weightedInliers, fit.quality,
                                    fit.provenance},
                                   feature, why));
  }
};
class Cancel : public ExecContext {
public:
  bool cancelled() const override { return true; }
  void progress(float, std::string_view) override {}
  void log(LogLevel, std::string) override {}
  const std::filesystem::path& baseDir() const override { return base; }
  int threadBudget() const override { return 1; }

private:
  std::filesystem::path base;
};
}

TEST_CASE(
    "native result v3 converts explicit measurement units and never invents missing point counts timing or consistency") {
  Fixture fixture;
  test::OpCall call(fixture.registry);
  call.inputs = {{"gap", measure(.002, "m")},
                 {"flush", measure(std::numeric_limits<double>::quiet_NaN(), "mm", false)}};
  REQUIRE(call.run("gap.result_bundle").ok);
  const auto& result = call.out("bundle").asRecord()->data;
  CHECK(result.at("schemaVersion") == 3);
  CHECK(result.at("gap").at("value_mm") == 2.0);
  CHECK(result.at("flush").at("status") == "failure");
  CHECK(result.at("flush").at("value_mm").is_null());
  CHECK(result.at("input_point_count").is_null());
  CHECK(result.at("left_point_count").is_null());
  CHECK(result.at("point_count_status") == "unavailable");
  CHECK(result.at("runtime_us").is_null());
  CHECK(result.at("consistency_retry_attempted").is_null());
  CHECK(result.at("consistency_gate_failed").is_null());
  call.inputs = {{"gap", measure(3, "px")}};
  REQUIRE(call.run("gap.result_bundle").ok);
  CHECK(call.out("bundle").asRecord()->data.at("gap").at("status") == "failure");
  CHECK(call.out("bundle").asRecord()->data.at("gap").at("code") == "unsupported_measurement_unit");
  CHECK(call.out("bundle").asRecord()->data.at("gap").at("value_mm").is_null());
  CHECK(call.out("bundle").asRecord()->data.at("flush").at("status") == "inactive");
  const auto* desc = fixture.registry.find("gap.result_bundle");
  REQUIRE(desc);
  CHECK(desc->version == "3.0.0");
  REQUIRE(desc->migrations.size() == 2);
  CHECK(desc->migrations[0].fromMajor == 1);
  CHECK(desc->migrations[1].fromMajor == 2);
  CHECK_THROWS_WITH_AS(desc->migrations[0].apply(Json::object()),
                       doctest::Contains("graph_upgrade_required"), std::invalid_argument);
}

TEST_CASE(
    "native result preserves legacy quality CSV fields and maps public SI residuals with distinct weighting") {
  Fixture fixture;
  test::OpCall call(fixture.registry);
  const Record legacy{"gap.FitQuality.v2",
                      {{"model", "line"},
                       {"pointCount", 10},
                       {"inlierCount", 8},
                       {"inlierRatio", .8},
                       {"rmsResidualMm", .02},
                       {"maxResidualMm", .1},
                       {"linePointXMm", 12.},
                       {"lineDirX", .8}}};
  call.inputs = {
      {"gap", measure(1)}, {"fitBase", Data::record(legacy)}, {"fitRef", fixture.fit.quality}};
  REQUIRE_MESSAGE(call.run("gap.result_bundle").ok, call.status.message);
  const auto& result = call.out("bundle").asRecord()->data;
  REQUIRE(result.at("fits").size() == 2);
  const auto& old = result.at("fits")[0];
  CHECK(old.at("point_count") == 10);
  CHECK(old.at("inlier_count") == 8);
  CHECK(old.at("rms_residual_mm") == .02);
  CHECK(old.at("line_point_x_mm") == 12.);
  CHECK(old.at("line_point_y_mm").is_null());
  const auto& modern = result.at("fits")[1];
  CHECK(modern.at("model") == "line");
  CHECK(modern.at("rms_residual_mm") ==
        fixture.fit.quality.asRecord()->data.at("weightedRmsResidual").get<double>() * 1000);
  CHECK(modern.at("max_residual_mm") ==
        fixture.fit.quality.asRecord()->data.at("maxResidual").get<double>() * 1000);
  CHECK(modern.at("inlier_count") == fixture.fit.weightedInliers.asIndices()->values.size());
  CHECK(modern.at("unique_inlier_count") == fixture.fit.inliers.asIndices()->values.size());
  CHECK(modern.at("inlier_count_semantics") == "final_inlier_multiset");
  CHECK(modern.at("weighted_inlier_count") ==
        fixture.fit.weightedInliers.asIndices()->values.size());
  CHECK(modern.at("line_dir_x").is_null());
  CHECK(result.at("native_quality").at("fitRef").at("data") ==
        fixture.fit.quality.asRecord()->data);
}

TEST_CASE(
    "native result feature evidence and per-stage fallback keep actual ROI source and original sensor counts") {
  Fixture fixture(0, true);
  test::OpCall call(fixture.registry);
  call.inputs = {{"gap", measure(.5)},
                 {"scan", fixture.scan},
                 {"rois", fixture.rois},
                 {"baseFeature", fixture.feature},
                 {"fitBase", fixture.fit.quality},
                 {"fallbackLine", decision("line_fit_failed")},
                 {"fallbackReference", decision("reference_unavailable")},
                 {"fallbackCircles", decision("circle_retry")},
                 {"fallback", decision("location_primary_failed")}};
  REQUIRE_MESSAGE(call.run("gap.result_bundle").ok, call.status.message);
  const auto& result = call.out("bundle").asRecord()->data;
  CHECK(result.at("roi_source") == "model");
  CHECK(result.at("fallback_decisions").size() == 4);
  CHECK(result.at("fallback_decisions").at("line").at("reason") == "line_fit_failed");
  CHECK(result.at("fallback_decisions").at("circles").at("reason") == "circle_retry");
  CHECK(result.at("fallback_reason") == "location_primary_failed");
  CHECK(result.at("fits")[0].at("line_point_x_mm") ==
        static_cast<double>(fixture.fit.line.asLine2D()->point[0]) * 1000);
  CHECK(result.at("fits")[0].at("line_dir_x") == fixture.fit.line.asLine2D()->dir[0]);
  CHECK(result.at("source_evidence").at("baseFeature").at("profile") ==
        profile_contract::context(fixture.fit.profile));
  CHECK(result.at("source_evidence").at("rois").at("status") == "bound_to_scan");
  CHECK(result.at("point_counts").at("sensor_input_primary") == 100);
  CHECK(result.at("point_counts").at("input_primary").get<std::size_t>() < 100);
  CHECK(result.at("input_point_count").get<std::size_t>() < 200);
  call.inputs.erase("fallback");
  REQUIRE(call.run("gap.result_bundle").ok);
  CHECK(call.out("bundle").asRecord()->data.at("fallback_reason").is_null());
  CHECK(call.out("bundle").asRecord()->data.at("roi_source") == "model");
}

TEST_CASE(
    "native result rejects wrong types stale features inconsistent quality and ROI contexts atomically") {
  Fixture fixture, other(.001F);
  test::OpCall call(fixture.registry);
  for (const auto& invalid : std::vector<std::pair<const char*, Data>>{
           {"fitBase", Data::record(Record{"wrong", Json::object()})},
           {"fits", Data::record(Record{"GapFitQualityPair", Json::object()})},
           {"fallbackLine", Data::record(Record{"wrong", Json::object()})},
           {"scan", Data::record(Record{"wrong", Json::object()})},
           {"baseFeature", other.feature}}) {
    CAPTURE(invalid.first);
    call.inputs = {{"gap", measure(.5)}, {"scan", fixture.scan}, {"rois", fixture.rois}};
    call.inputs[invalid.first] = invalid.second;
    CHECK_FALSE(call.run("gap.result_bundle").ok);
    CHECK(call.outputs.empty());
  }
  call.inputs = {{"gap", measure(.5)},
                 {"scan", fixture.scan},
                 {"baseFeature", fixture.feature},
                 {"fitBase", other.fit.quality}};
  CHECK_FALSE(call.run("gap.result_bundle").ok);
  CHECK(call.outputs.empty());
  const auto box = Data::box2d(Box2D{{0, .16F}, {.02F, .18F}});
  call.inputs = {{"gap", measure(.5)}, {"rois", fixture.rois}, {"roiOverall", box}};
  CHECK_FALSE(call.run("gap.result_bundle").ok);
  call.inputs["roiOverallContext"] = bind(fixture.fit.profile, box);
  REQUIRE(call.run("gap.result_bundle").ok);
  Profile foreign = *fixture.fit.profile.asProfile();
  foreign.descriptor.frame = "foreign";
  call.inputs["roiOverallContext"] = bind(Data::profile(foreign), box);
  CHECK_FALSE(call.run("gap.result_bundle").ok);
  Cancel cancel;
  ParamMap values;
  for (const auto& parameter : fixture.registry.find("gap.result_bundle")->params)
    values[parameter.name] = parameter.def;
  const std::unordered_map<std::string, Data> inputs{{"gap", measure(1)}};
  std::unordered_map<std::string, Data> outputs{{"sentinel", measure(1)}};
  Outputs pending(outputs);
  CHECK(fixture.registry.find("gap.result_bundle")
            ->compute(Inputs(inputs), ParamView(values, cancel.baseDir()), pending, cancel)
            .code == "cancelled");
  CHECK(outputs.size() == 1);
}

TEST_CASE(
    "real executor emits the complete native result record with feature lineage and explicit unavailable timing") {
  Fixture fixture;
  auto& global = ensureRegistry();
  const std::string id = "test.m4c.profile_result_bundle";
  if (!global.find(id)) {
    for (const auto& bundle : fixture.registry.bundles())
      global.addBundle(bundle);
    auto op = *fixture.registry.find("gap.result_bundle");
    op.id = id;
    op.version = "1.0.0";
    op.migrations.clear();
    global.addOperator(std::move(op));
  }
  const auto graph = test::makeGraph({{"value", "util.reroute"},
                                      {"feature", "util.reroute"},
                                      {"scan", "util.reroute"},
                                      {"rois", "util.reroute"},
                                      {"result", id}},
                                     {{"value.out", "result.gap"},
                                      {"feature.out", "result.baseFeature"},
                                      {"scan.out", "result.scan"},
                                      {"rois.out", "result.rois"}});
  test::Session session(graph, {}, {}, false, 1, true,
                        {{"value", "out", measure(.003, "m")},
                         {"feature", "out", fixture.feature},
                         {"scan", "out", fixture.scan},
                         {"rois", "out", fixture.rois}});
  const auto& log = session.wait();
  REQUIRE_MESSAGE(log.runStatus() == "ok", Json(log.events).dump());
  CHECK(log.finalState("result") == "done");
  Data value;
  REQUIRE(exec::ResultStore::instance().get(session.runId(), "result", "bundle", value));
  REQUIRE(value.asRecord());
  CHECK(value.asRecord()->type == "GapResultBundle");
  CHECK(value.asRecord()->data.at("gap").at("value_mm") == 3.0);
  CHECK(value.asRecord()->data.at("schemaVersion") == 3);
  CHECK(value.asRecord()->data.at("point_count_status") == "available");
  CHECK(value.asRecord()->data.at("timing_status") == "unavailable");
}

TEST_CASE(
    "native result preserves legacy weighted CSV inlier counts and adds actual unique observations") {
  Fixture fixture;
  PointCloud cloud;
  for (int i = 0; i < 6; ++i)
    cloud.push(i * .01F, i * .001F, .017F);
  Profile source;
  std::string why;
  REQUIRE(makeProfile(std::make_shared<const PointCloud>(std::move(cloud)),
                      {1, "measurement", "m", "XY", "right", "acquisition", "weighted", 6},
                      {1, 1, 1, 1, 1, 1}, {0, 1, 2, 3, 4, 5}, source, why));
  const auto input = Data::profile(source);
  const auto box = Data::box2d(Box2D{{-.01F, -.02F}, {.05F, .02F}});
  Point2D toward;
  toward.p[0] = .03F;
  toward.p[1] = .004F;
  const auto point = Data::point2d(toward), ref = Data::line2d(Line2D{});
  ParamMap parameters;
  for (const auto& parameter : profile_contract::robustLineParams())
    parameters[parameter.name] = parameter.def;
  parameters["dirMode"] = Value::text("fixed");
  parameters["distanceThreshold"] = Value::number(1e-8);
  parameters["segmentPoints"] = Value::integer(0);
  parameters["endpoints"] = Value::text("inlier_ends");
  test::StubContext ctx;
  profile_contract::RobustLineResult fit;
  REQUIRE(profile_contract::fitRobustLine(
              {input, box, bind(input, box), point, bind(input, point), ref, bind(input, ref), {}},
              ParamView(parameters, ctx.baseDir()), fit, ctx)
              .ok);
  REQUIRE(fit.inliers.asIndices()->values.size() == 3);
  REQUIRE(fit.weightedInliers.asIndices()->values.size() == 4);
  Data feature;
  REQUIRE(scene::makeLineFeature({fit.profile, fit.line, fit.lineContext, fit.innerEnd,
                                  fit.pointContext, fit.inliers, fit.weightedInliers, fit.quality,
                                  fit.provenance},
                                 feature, why));
  test::OpCall call(fixture.registry);
  call.inputs = {{"gap", measure(1)}, {"baseFeature", feature}};
  REQUIRE_MESSAGE(call.run("gap.result_bundle").ok, call.status.message);
  const auto& output = call.out("bundle").asRecord()->data.at("fits")[0];
  CHECK(output.at("inlier_count") == 4);
  CHECK(output.at("inlier_ratio") == 4.0 / 6);
  CHECK(output.at("unique_inlier_count") == 3);
  CHECK(output.at("unique_inlier_ratio") == .5);
  CHECK(output.at("weighted_inlier_count") == 4);
}

TEST_CASE(
    "native result distinguishes actual overall crop from roll crop and rejects unbound declarations") {
  Fixture fixture;
  scene::ScanView scan;
  scene::RoiView rois;
  std::string why;
  REQUIRE(scene::readScanPair(fixture.scan, scan, why));
  REQUIRE(scene::readRoiSet(fixture.rois, rois, why));
  const Json counts = {scan.primary->asProfile()->pointCount(),
                       scan.secondary->asProfile()->pointCount(),
                       scan.merged->asProfile()->pointCount()};
  const Json crop{{"schemaVersion", 1},
                  {"kind", "gap.OverallCrop.v1"},
                  {"status", "applied"},
                  {"applied", true},
                  {"method", "overall"},
                  {"bounds", "open"},
                  {"boxMm", {-10, -1000, 1000, 1000}},
                  {"before", counts},
                  {"after", counts}};
  const auto& previous = scan.info->data;
  scene::ScanMetadata metadata{
      previous.at("sensorFrame").get<std::string>(),
      previous.at("measurementFrame").get<std::string>(),
      "processed",
      previous.at("guard"),
      crop,
      previous.at("origins"),
      {{"method", "template"}, {"status", "complete"}, {"details", Json::object()}},
      scan.primaryProvenance,
      scan.secondaryProvenance};
  Data processed;
  REQUIRE(scene::makeScanPair(*scan.sensorPrimary, *scan.sensorSecondary, *scan.primary,
                              *scan.secondary, *scan.merged, *scan.mergedProvenance, metadata,
                              processed, why));
  scene::RoiFields fields;
  for (std::size_t i = 0; i < 4; ++i) {
    fields.boxes[i] = *rois.boxes[i];
    fields.contexts[i] = *rois.contexts[i];
  }
  fields.info = *rois.info;
  fields.info.data["cropStatus"] = crop;
  fields.info.data["overallMm"] = crop.at("boxMm");
  fields.info.data["processing"] = metadata.processing;
  Data actualRois;
  REQUIRE(scene::makeRoiSet(fields, actualRois, why));
  test::OpCall call(fixture.registry);
  call.inputs = {{"gap", measure(1)}, {"scan", processed}, {"rois", actualRois}};
  REQUIRE_MESSAGE(call.run("gap.result_bundle").ok, call.status.message);
  const auto output = call.out("bundle").asRecord()->data;
  CHECK(output.at("crop_status") == "applied");
  CHECK(output.at("point_counts").at("roll_crop_applied") == 0);
  CHECK(output.at("point_counts").at("roll_crop_reverted") == 0);
  call.inputs.erase("scan");
  CHECK_FALSE(call.run("gap.result_bundle").ok);
  call.inputs["scan"] = fixture.scan;
  CHECK_FALSE(call.run("gap.result_bundle").ok);
  call.inputs["scan"] = processed;
  fields.info.data["cropStatus"]["method"] = "unknown";
  REQUIRE(scene::makeRoiSet(fields, actualRois, why));
  call.inputs["rois"] = actualRois;
  CHECK_FALSE(call.run("gap.result_bundle").ok);

  Fixture rolled(0, true);
  REQUIRE(scene::readScanPair(rolled.scan, scan, why));
  call.inputs = {
      {"gap", measure(1)},
      {"scan", rolled.scan},
      {"rois", rolled.rois},
      {"cropStatus", Data::record(Record{"gap.RollCrop.v2", scan.info->data.at("crop")})}};
  REQUIRE_MESSAGE(call.run("gap.result_bundle").ok, call.status.message);
  CHECK(call.out("bundle").asRecord()->data.at("point_counts").at("roll_crop_applied") == 1);
  CHECK(call.out("bundle").asRecord()->data.at("point_counts").at("roll_crop_reverted") == 0);
}
