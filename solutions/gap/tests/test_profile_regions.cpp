#include <doctest/doctest.h>

#include <cstring>
#include <limits>

#include "lyflow/profile.h"
#include "lyflow_profile/merge.h"
#include "lyflow_profile/selection.h"
#include "ops/profile_regions.h"

using namespace lyflow;
namespace scene = lyflow::packs::gap::profile_scene;
namespace {
using Json = nlohmann::json;
struct TestContext : ExecContext {
  std::filesystem::path base;
  bool cancelled() const override { return false; }
  void progress(float, std::string_view) override {}
  void log(LogLevel, std::string) override {}
  const std::filesystem::path& baseDir() const override { return base; }
  int threadBudget() const override { return 1; }
};
const Json& baseline() {
  static const Json value = Json::parse(
      R"json({"cases":[{"name":"fixed","op":"gap.overall_roi","params":{"roi":[-11.2345,-4.3456,10.7891,9.5432],"mode":"fixed","usingCamera":"Both"},"primary":[[-0.004,0.17,0],[-0.003,0.172,0],[-0.002,0.171,0],[-0.001,0.169,0]],"secondary":[[0.001,0.173,0],[0.002,0.174,0],[0.003,0.175,0],[0.004,0.176,0]],"left":[[-0.006,0.169],[-0.004,0.171]],"right":[[0.004,0.169],[0.006,0.171]],"height":null,"bits":[3157790952,3146671497,1009829060,1008491284],"status":null},{"name":"auto-both","op":"gap.overall_roi","params":{"roi":[-11.2345,-4.3456,10.7891,9.5432],"mode":"auto_center","usingCamera":"Both"},"primary":[[-0.004,0.17,0],[-0.003,0.172,0],[-0.002,0.171,0],[-0.001,0.169,0]],"secondary":[[0.001,0.173,0],[0.002,0.174,0],[0.003,0.175,0],[0.004,0.176,0]],"left":[[-0.006,0.169],[-0.004,0.171]],"right":[[0.004,0.169],[0.006,0.171]],"height":null,"bits":[3157551830,1042909032,1010068182,1043841094],"status":null},{"name":"auto-left","op":"gap.overall_roi","params":{"roi":[-11.2345,-4.3456,10.7891,9.5432],"mode":"auto_center","usingCamera":"Left"},"primary":[[-0.004,0.17,0],[-0.003,0.172,0],[-0.002,0.171,0],[-0.001,0.169,0]],"secondary":[[0.001,0.173,0],[0.002,0.174,0],[0.003,0.175,0],[0.004,0.176,0]],"left":[[-0.006,0.169],[-0.004,0.171]],"right":[[0.004,0.169],[0.006,0.171]],"height":null,"bits":[3160236185,1042774815,1007383827,1043706877],"status":null},{"name":"auto-right","op":"gap.overall_roi","params":{"roi":[-11.2345,-4.3456,10.7891,9.5432],"mode":"auto_center","usingCamera":"Right"},"primary":[[-0.004,0.17,0],[-0.003,0.172,0],[-0.002,0.171,0],[-0.001,0.169,0]],"secondary":[[0.001,0.173,0],[0.002,0.174,0],[0.003,0.175,0],[0.004,0.176,0]],"left":[[-0.006,0.169],[-0.004,0.171]],"right":[[0.004,0.169],[0.006,0.171]],"height":null,"bits":[3154867475,1043043249,1012752537,1043975310],"status":null},{"name":"auto-single","op":"gap.overall_roi","params":{"roi":[-11.2345,-4.3456,10.7891,9.5432],"mode":"auto_center","usingCamera":"Both"},"primary":[[-0.004,0.17,0],[-0.003,0.172,0],[-0.002,0.171,0],[-0.001,0.169,0]],"secondary":[],"left":[[-0.006,0.169],[-0.004,0.171]],"right":[[0.004,0.169],[0.006,0.171]],"height":null,"bits":[3160236185,1042774815,1007383827,1043706877],"status":null},{"name":"auto-right-empty","op":"gap.overall_roi","params":{"roi":[-11.2345,-4.3456,10.7891,9.5432],"mode":"auto_center","usingCamera":"Right"},"primary":[[-0.004,0.17,0],[-0.003,0.172,0],[-0.002,0.171,0],[-0.001,0.169,0]],"secondary":[],"left":[[-0.006,0.169],[-0.004,0.171]],"right":[[0.004,0.169],[0.006,0.171]],"height":null,"bits":[3157790952,3146671497,1009829060,1008491284],"status":null},{"name":"roll-applied","op":"gap.roll_anchored_crop","params":{"halfWidth":4,"halfHeight":4,"minPointsKept":1},"primary":[[-0.004,0.17,0],[-0.003,0.172,0],[-0.002,0.171,0],[-0.001,0.169,0]],"secondary":[[0.001,0.173,0],[0.002,0.174,0],[0.003,0.175,0],[0.004,0.176,0]],"left":[[-0.006,0.169],[-0.004,0.171]],"right":[[0.004,0.169],[0.006,0.171]],"height":null,"bits":[3145929327,1042938855,998445679,1043475726],"status":{"afterPrimary":3,"afterSecondary":1,"applied":true,"beforePrimary":4,"beforeSecondary":4,"boxMm":[-4.0,166.00000178813934,4.0,174.00000178813934],"minPointsKept":1,"status":"applied"}},{"name":"roll-reverted","op":"gap.roll_anchored_crop","params":{"halfWidth":4,"halfHeight":4,"minPointsKept":20},"primary":[[-0.004,0.17,0],[-0.003,0.172,0],[-0.002,0.171,0],[-0.001,0.169,0]],"secondary":[[0.001,0.173,0],[0.002,0.174,0],[0.003,0.175,0],[0.004,0.176,0]],"left":[[-0.006,0.169],[-0.004,0.171]],"right":[[0.004,0.169],[0.006,0.171]],"height":null,"bits":[3145929327,1043140182,998445679,1043609944],"status":{"afterPrimary":4,"afterSecondary":4,"applied":false,"beforePrimary":4,"beforeSecondary":4,"boxMm":[-4.0,166.00000178813934,4.0,174.00000178813934],"minPointsKept":20,"status":"reverted:min_points"}},{"name":"roll-single-both","op":"gap.roll_anchored_crop","params":{"halfWidth":4,"halfHeight":4,"minPointsKept":1},"primary":[[-0.004,0.17,0],[-0.003,0.172,0],[-0.002,0.171,0],[-0.001,0.169,0]],"secondary":[],"left":[[-0.006,0.169],[-0.004,0.171]],"right":[[0.004,0.169],[0.006,0.171]],"height":null,"bits":[3145929327,1042938855,998445679,1043475726],"status":{"afterPrimary":3,"afterSecondary":0,"applied":true,"beforePrimary":4,"beforeSecondary":0,"boxMm":[-4.0,166.00000178813934,4.0,174.00000178813934],"minPointsKept":1,"status":"applied"}},{"name":"roll-single-right-reverted","op":"gap.roll_anchored_crop","params":{"halfWidth":4,"halfHeight":4,"minPointsKept":1,"usingCamera":"Right"},"primary":[[-0.004,0.17,0],[-0.003,0.172,0],[-0.002,0.171,0],[-0.001,0.169,0]],"secondary":[],"left":[[-0.006,0.169],[-0.004,0.171]],"right":[[0.004,0.169],[0.006,0.171]],"height":null,"bits":[3145929327,1043140182,3129152111,1043341509],"status":{"afterPrimary":4,"afterSecondary":0,"applied":false,"beforePrimary":4,"beforeSecondary":0,"boxMm":[-4.0,166.00000178813934,4.0,174.00000178813934],"minPointsKept":1,"status":"reverted:min_points"}},{"name":"roll-disabled","op":"gap.roll_anchored_crop","params":{"enabled":false},"primary":[[-0.004,0.17,0],[-0.003,0.172,0],[-0.002,0.171,0],[-0.001,0.169,0]],"secondary":[[0.001,0.173,0],[0.002,0.174,0],[0.003,0.175,0],[0.004,0.176,0]],"left":[[-0.006,0.169],[-0.004,0.171]],"right":[[0.004,0.169],[0.006,0.171]],"height":null,"bits":[3145929327,1043140182,998445679,1043609944],"status":{"afterPrimary":4,"afterSecondary":4,"applied":false,"beforePrimary":4,"beforeSecondary":4,"minPointsKept":50,"status":"disabled"}},{"name":"roll-bad-config","op":"gap.roll_anchored_crop","params":{"halfWidth":0},"primary":[[-0.004,0.17,0],[-0.003,0.172,0],[-0.002,0.171,0],[-0.001,0.169,0]],"secondary":[[0.001,0.173,0],[0.002,0.174,0],[0.003,0.175,0],[0.004,0.176,0]],"left":[[-0.006,0.169],[-0.004,0.171]],"right":[[0.004,0.169],[0.006,0.171]],"height":null,"bits":[3145929327,1043140182,998445679,1043609944],"status":{"afterPrimary":4,"afterSecondary":4,"applied":false,"beforePrimary":4,"beforeSecondary":4,"minPointsKept":50,"status":"rejected:bad_config"}},{"name":"roll-degenerate","op":"gap.roll_anchored_crop","params":{},"primary":[[-0.004,0.17,0],[-0.003,0.172,0],[-0.002,0.171,0],[-0.001,0.169,0]],"secondary":[[0.001,0.173,0],[0.002,0.174,0],[0.003,0.175,0],[0.004,0.176,0]],"left":[[-0.004,0.17],[-0.004,0.17]],"right":[[0.004,0.169],[0.006,0.171]],"height":null,"bits":[3145929327,1043140182,998445679,1043609944],"status":{"afterPrimary":4,"afterSecondary":4,"applied":false,"beforePrimary":4,"beforeSecondary":4,"minPointsKept":50,"status":"rejected:degenerate_roll_box"}},{"name":"roll-height","op":"gap.roll_anchored_crop","params":{"maxRollBoxHeight":1},"primary":[[-0.004,0.17,0],[-0.003,0.172,0],[-0.002,0.171,0],[-0.001,0.169,0]],"secondary":[[0.001,0.173,0],[0.002,0.174,0],[0.003,0.175,0],[0.004,0.176,0]],"left":[[-0.006,0.169],[-0.004,0.171]],"right":[[0.004,0.169],[0.006,0.171]],"height":null,"bits":[3145929327,1043140182,998445679,1043609944],"status":{"afterPrimary":4,"afterSecondary":4,"applied":false,"beforePrimary":4,"beforeSecondary":4,"minPointsKept":50,"status":"rejected:roll_box_height"}},{"name":"roll-span","op":"gap.roll_anchored_crop","params":{},"primary":[[-0.004,0.17,0],[-0.003,0.172,0],[-0.002,0.171,0],[-0.001,0.169,0]],"secondary":[[0.001,0.173,0],[0.002,0.174,0],[0.003,0.175,0],[0.004,0.176,0]],"left":[[-0.006,0.169],[-0.004,0.171]],"right":[[0.05,0.169],[0.052,0.171]],"height":null,"bits":[3145929327,1043140182,998445679,1043609944],"status":{"afterPrimary":4,"afterSecondary":4,"applied":false,"beforePrimary":4,"beforeSecondary":4,"minPointsKept":50,"status":"rejected:span"}}]})json");
  return value;
}
std::array<std::uint32_t, 4> bits(const Data& data) {
  const auto& box = *data.asBox2D();
  std::array<std::uint32_t, 4> result{};
  const float values[4]{box.min[0], box.min[1], box.max[0], box.max[1]};
  std::memcpy(result.data(), values, sizeof(values));
  return result;
}
struct Row {
  Data sensor;
  Data measurement;
};
Row row(const Json& values, const std::string& id) {
  PointCloud cloud;
  std::vector<std::uint32_t> indices;
  std::vector<std::uint8_t> valid;
  for (std::size_t i = 0; i < values.size(); ++i) {
    cloud.push(values[i][0].get<float>(), values[i][2].get<float>(), values[i][1].get<float>());
    cloud.intensity.push_back(static_cast<float>(10 + i));
    cloud.normals.insert(cloud.normals.end(), {0, 1, 0});
    cloud.rgb.insert(cloud.rgb.end(), {10, 20, 30});
    indices.push_back(static_cast<std::uint32_t>(i));
    valid.push_back(1);
  }
  Profile sensor, measurement;
  std::string why;
  REQUIRE(makeProfile(std::make_shared<const PointCloud>(std::move(cloud)),
                      {1, "sensor-frame", "m", "XZ", "right", "acquisition", id, values.size()},
                      std::move(valid), std::move(indices), sensor, why));
  REQUIRE(remapProfileAxes(sensor, "XZY", {1, 1, 1}, "measurement-frame", measurement, why));
  return {Data::profile(std::move(sensor)), Data::profile(std::move(measurement))};
}
Data box(const Json& values) {
  Box2D result;
  result.min[0] = values[0][0].get<float>();
  result.min[1] = values[0][1].get<float>();
  result.max[0] = values[1][0].get<float>();
  result.max[1] = values[1][1].get<float>();
  return Data::box2d(result);
}
Record context(const Data& profile) {
  return profile_contract::geometryContext(profile.asProfile()->descriptor);
}
Data scan(const Row& primary, const Row& secondary, bool narrowMerged = true) {
  profile_contract::MergeResult merged;
  std::string why;
  REQUIRE(profile_contract::mergeProfiles(secondary.measurement, primary.measurement,
                                          "intersection", merged, why));
  if (narrowMerged && primary.measurement.asProfile()->pointCount() > 1) {
    TestContext ctx;
    profile_contract::SelectionOptions options;
    options.emptyPolicy = "allow";
    profile_contract::SelectionResult result;
    Indices indices{
        merged.profile.asProfile()->cloud->id,
        {static_cast<std::int32_t>(secondary.measurement.asProfile()->pointCount() + 1)}};
    REQUIRE(
        profile_contract::selectProfileIndices(merged.profile, indices, options, result, ctx).ok);
    merged.profile = result.profile;
  }
  Json origins = Json::object();
  for (const auto& entry :
       {std::make_pair("primary", &primary.sensor), std::make_pair("secondary", &secondary.sensor)})
    origins[entry.first] = {{"kind", "injected_profile"},
                            {"sourceId", entry.second->asProfile()->descriptor.sourceId},
                            {"details", Json::object()}};
  scene::ScanMetadata metadata{
      "sensor-frame",
      "measurement-frame",
      "raw",
      {{"taken", "both"}, {"evaluated", false}, {"exceeded", false}, {"onDisagree", "off"}},
      nullptr,
      origins,
      {{"method", "read"},
       {"status", "complete"},
       {"details", {{"mergedPrefiltered", narrowMerged}}}}};
  Data output;
  REQUIRE_MESSAGE(scene::makeScanPair(primary.sensor, secondary.sensor, primary.measurement,
                                      secondary.measurement, merged.profile, metadata, output, why),
                  why);
  return output;
}
struct Stop : TestContext {
  bool cancelled() const override { return true; }
};
}

TEST_CASE(
    "Gap native overall windows match legacy DLL fixed median and single camera cases bitwise") {
  TestContext ctx;
  for (const auto& test : baseline()["cases"]) {
    if (test["op"] != "gap.overall_roi")
      continue;
    CAPTURE(test["name"]);
    auto p = row(test["primary"], "P"), s = row(test["secondary"], "S");
    scene::OverallRoiOptions options;
    for (std::size_t i = 0; i < 4; ++i)
      options.roiMm[i] = test["params"]["roi"][i].get<float>();
    options.mode = test["params"]["mode"];
    options.usingCamera = test["params"]["usingCamera"];
    scene::GeometryResult result;
    REQUIRE(scene::overallRoi(p.measurement, s.measurement, options, result, ctx).ok);
    CHECK(Json(bits(result.box)) == test["bits"]);
    CHECK(result.context.data == context(p.measurement).data);
  }
}

TEST_CASE("Gap native roll windows and all status fields match nine actual legacy DLL branches") {
  TestContext ctx;
  for (const auto& test : baseline()["cases"]) {
    if (test["op"] != "gap.roll_anchored_crop")
      continue;
    CAPTURE(test["name"]);
    const auto p = row(test["primary"], "P"), s = row(test["secondary"], "S");
    const auto input = scan(p, s), left = box(test["left"]), right = box(test["right"]);
    const auto leftContext = context(p.measurement), rightContext = context(s.measurement);
    const auto& params = test["params"];
    scene::RollCropOptions options{
        params.value("enabled", true),     params.value("halfWidth", 35.),
        params.value("halfHeight", 20.),   params.value("maxRollBoxHeight", 8.),
        params.value("minPointsKept", 50), params.value("usingCamera", std::string("Both"))};
    scene::RollCropResult result;
    REQUIRE_MESSAGE(
        scene::rollAnchoredCrop(input, left, leftContext, right, rightContext, options, result, ctx)
            .ok,
        test["name"].get<std::string>());
    CHECK(Json(bits(result.window)) == test["bits"]);
    for (const auto& expected : test["status"].items())
      CHECK(result.status.data[expected.key()] == expected.value());
    CHECK(result.status.data["mergedRebuilt"] == false);
    scene::ScanView view;
    std::string why;
    REQUIRE(scene::readScanPair(result.scan, view, why));
    CHECK(view.primary->asProfile()->pointCount() ==
          test["status"]["afterPrimary"].get<std::size_t>());
    CHECK(view.secondary->asProfile()->pointCount() ==
          test["status"]["afterSecondary"].get<std::size_t>());
    CHECK(view.sensorPrimary->profilePtr() == p.sensor.profilePtr());
    CHECK(view.merged->asProfile()->pointCount() == 1);
    if (test["status"]["applied"] == false)
      CHECK(result.scan.asBundle() == input.asBundle());
    else {
      CHECK(view.info->data["stage"] == "processed");
      CHECK(view.info->data["processing"]["details"]["mergedRebuilt"] == false);
      CHECK(view.merged->asProfile()->sourceIndices[0] ==
            s.measurement.asProfile()->descriptor.sourceCount + 1);
    }
  }
}

TEST_CASE(
    "Gap region helpers enforce mask physical space no fake empty bounds and cancellation atomically") {
  const auto& test = baseline()["cases"][0];
  auto p = row(test["primary"], "P"), s = row(test["secondary"], "S");
  auto masked = *p.measurement.asProfile();
  masked.validity.assign(masked.pointCount(), 0);
  TestContext ctx;
  scene::GeometryResult result;
  scene::OverallRoiOptions options{{-11.2345F, -4.3456F, 10.7891F, 9.5432F}, "auto_center", "Both"};
  REQUIRE(scene::overallRoi(Data::profile(masked), s.measurement, options, result, ctx).ok);
  CHECK(Json(bits(result.box)) == baseline()["cases"][3]["bits"]);
  const auto original = result.box.valueJson();
  auto other = *s.measurement.asProfile();
  other.descriptor.frame = "unrelated-frame";
  CHECK_FALSE(scene::overallRoi(p.measurement, Data::profile(other), options, result, ctx).ok);
  CHECK(result.box.valueJson() == original);
  Stop stop;
  CHECK_FALSE(scene::overallRoi(p.measurement, s.measurement, options, result, stop).ok);
  CHECK(result.box.valueJson() == original);
  const auto left = box(test["left"]), right = box(test["right"]);
  const auto a = context(p.measurement), b = context(s.measurement);
  const auto input = scan(p, s);
  scene::RollCropResult roll;
  REQUIRE(
      scene::rollAnchoredCrop(input, left, a, right, b, {false, 4, 4, 8, 1, "Both"}, roll, ctx).ok);
  const auto* unchanged = roll.scan.asBundle();
  CHECK_FALSE(scene::rollAnchoredCrop(input, left, a, right, b, {}, roll, stop).ok);
  CHECK(roll.scan.asBundle() == unchanged);
  const auto empty = row(Json::array(), "empty");
  const auto emptyScan = scan(empty, empty, false);
  auto status =
      scene::rollAnchoredCrop(emptyScan, left, context(empty.measurement), right,
                              context(empty.measurement), {false, 4, 4, 8, 1, "Both"}, roll, ctx);
  CHECK_FALSE(status.ok);
  CHECK(status.code == "no_result");
  CHECK(roll.scan.asBundle() == unchanged);
}
