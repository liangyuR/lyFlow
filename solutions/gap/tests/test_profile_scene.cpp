#include <doctest/doctest.h>
#include "helpers.h"
#include "ops/profile_bundle_ops.h"

#include <limits>

#include "lyflow/profile.h"
#include "lyflow_profile/merge.h"
#include "ops/profile_scene.h"

using namespace lyflow;
namespace scene = lyflow::packs::gap::profile_scene;

namespace {
using Json = nlohmann::json;
Data sensor(const std::string& source, const std::string& unit = "m") {
  PointCloud cloud;
  cloud.push(1, 0, 3);
  const auto nan = std::numeric_limits<float>::quiet_NaN();
  cloud.push(nan, nan, nan);
  cloud.intensity = {70, 80};
  cloud.normals = {0, 1, 0, 1, 0, 0};
  cloud.rgb = {9, 10, 11, 12, 13, 14};
  Profile profile;
  std::string why;
  REQUIRE(makeProfile(std::make_shared<const PointCloud>(std::move(cloud)),
                      {1, "sensor-frame", unit, "XZ", "right", "acquisition", source, 8}, {1, 0},
                      {2, 5}, profile, why));
  return Data::profile(std::move(profile));
}
Data measurement(const Data& sensor) {
  Profile axes, metric;
  std::string why;
  REQUIRE(remapProfileAxes(*sensor.asProfile(), "XZY", {1, 1, 1}, "measurement-frame", axes, why));
  REQUIRE(convertProfileUnit(axes, "m", metric, why));
  return Data::profile(std::move(metric));
}
Data merge(const Data& secondary, const Data& primary) {
  profile_contract::MergeResult result;
  std::string why;
  REQUIRE(profile_contract::mergeProfiles(secondary, primary, "require_equal", result, why));
  return result.profile;
}
scene::ScanMetadata metadata(const Data& primary, const Data& secondary) {
  Json origins = Json::object();
  for (const auto& entry :
       {std::make_pair("primary", &primary), std::make_pair("secondary", &secondary)})
    origins[entry.first] = {{"kind", "injected_profile"},
                            {"sourceId", entry.second->asProfile()->descriptor.sourceId},
                            {"details", {{"declaredBy", "test-host"}}}};
  return {"sensor-frame",
          "measurement-frame",
          "raw",
          {{"taken", "both"}, {"evaluated", false}, {"exceeded", false}, {"onDisagree", "off"}},
          nullptr,
          origins,
          {{"method", "read"}, {"status", "complete"}, {"details", Json::object()}}};
}
struct Fixture {
  Data sensorPrimary, sensorSecondary, primary, secondary, merged;
  scene::ScanMetadata info;
};
Fixture fixture() {
  Fixture result;
  result.sensorPrimary = sensor("declared-file-A");
  result.sensorSecondary = sensor("declared-file-B");
  result.primary = measurement(result.sensorPrimary);
  result.secondary = measurement(result.sensorSecondary);
  result.merged = merge(result.secondary, result.primary);
  result.info = metadata(result.sensorPrimary, result.sensorSecondary);
  return result;
}
bool make(const Fixture& input, Data& output, std::string& why) {
  return scene::makeScanPair(input.sensorPrimary, input.sensorSecondary, input.primary,
                             input.secondary, input.merged, input.info, output, why);
}
Data space(const Data& profile) {
  return Data::record(profile_contract::geometryContext(profile.asProfile()->descriptor));
}
scene::RoiFields roiFields(const Data& measurement) {
  scene::RoiFields fields;
  for (std::size_t i = 0; i < 4; ++i) {
    Box2D box;
    box.min[0] = static_cast<float>(i);
    box.min[1] = 0;
    box.max[0] = static_cast<float>(i + 1);
    box.max[1] = 4;
    fields.boxes[i] = Data::box2d(box);
    fields.contexts[i] = space(measurement);
  }
  fields.info = {
      scene::kRoiInfoType,
      {{"schemaVersion", 3},
       {"datumSide", "left"},
       {"source", "model"},
       {"alignment", nullptr},
       {"overallMm", nullptr},
       {"cropStatus", nullptr},
       {"processing", {{"method", "model"}, {"status", "complete"}, {"details", Json::object()}}}}};
  return fields;
}
} // namespace

TEST_CASE("Gap native scene bundles preserve sensor slots and mapped measurement channels") {
  const auto input = fixture();
  Data output;
  std::string why;
  REQUIRE_MESSAGE(make(input, output, why), why);
  scene::ScanView view;
  REQUIRE(scene::readScanPair(output, view, why));
  CHECK(output.asBundle()->fields.size() == 6);
  CHECK(view.sensorPrimary->profilePtr() == input.sensorPrimary.profilePtr());
  CHECK(view.primary->asProfile()->descriptor.plane == "XY");
  CHECK(view.primary->asProfile()->descriptor.handedness == "reflected");
  CHECK(view.primary->asProfile()->cloud->xyz[1] == 3);
  CHECK(view.primary->asProfile()->cloud->normals == std::vector<float>{0, 0, 1, 1, 0, 0});
  CHECK(view.sensorPrimary->asProfile()->cloud->rgb == input.primary.asProfile()->cloud->rgb);
  CHECK(view.merged->asProfile()->sourceIndices == std::vector<std::uint32_t>{2, 5, 10, 13});
  CHECK(view.info->type == scene::kScanInfoType);
  CHECK(view.info->data["processing"]["status"] == "complete");
  CHECK(view.info->data["crop"].is_null());
  CHECK(view.pick("secondary") == view.secondary);
  CHECK(view.pick("unknown") == nullptr);
  Registry registry;
  scene::registerProfileBundles(registry);
  REQUIRE(registry.findBundle(scene::kScanPairKind));
  CHECK(registry.findBundle(scene::kScanPairKind)->fields.size() == 6);
  CHECK(registry.findBundle(scene::kRoiSetKind)->fields.size() == 9);
}

TEST_CASE("Gap native scene converts millimeter sensors and rejects mismatched spaces") {
  auto input = fixture();
  input.sensorPrimary = sensor("declared-file-A", "mm");
  input.primary = measurement(input.sensorPrimary);
  input.merged = merge(input.secondary, input.primary);
  Data output;
  std::string why;
  REQUIRE_MESSAGE(make(input, output, why), why);
  CHECK(input.primary.asProfile()->cloud->xyz[1] == doctest::Approx(.003));
  const auto original = output.asBundle();
  input = fixture();
  auto changed = *input.sensorSecondary.asProfile();
  changed.descriptor.handedness = "reflected";
  input.sensorSecondary = Data::profile(changed);
  input.secondary = measurement(input.sensorSecondary);
  CHECK_FALSE(make(input, output, why));
  CHECK(why.find("同一米制XY空间") != std::string::npos);
  CHECK(output.asBundle() == original);
}

TEST_CASE("Gap role boxes keep four boxes in one measurement space") {
  const auto input = fixture();
  auto fields = roiFields(input.primary);
  Data output;
  std::string why;
  REQUIRE_MESSAGE(scene::makeRoiSet(fields, output, why), why);
  scene::RoiView view;
  REQUIRE(scene::readRoiSet(output, view, why));
  CHECK(output.asBundle()->fields.size() == 9);
  CHECK(view.role("seamLeft") == view.boxes[2]);
  CHECK(view.context("seamRight") == view.contexts[3]);
  CHECK(view.role("unknown") == nullptr);
  CHECK(view.info->data["overallMm"].is_null());
  CHECK(view.info->data["alignment"].is_null());
  const auto original = output.asBundle();
  fields.contexts[0] = space(input.sensorPrimary);
  CHECK_FALSE(scene::makeRoiSet(fields, output, why));
  CHECK(output.asBundle() == original);
  fields = roiFields(input.primary);
  fields.info.data["source"] = "template";
  fields.info.data["processing"]["method"] = "template";
  fields.info.data["roisFrom"] = "config";
  REQUIRE(scene::makeRoiSet(fields, output, why));
  REQUIRE(scene::readRoiSet(output, view, why));
  CHECK(view.info->data["roisFrom"] == "config");
}

TEST_CASE("Gap native bundle nodes round trip immutable profiles and scene info") {
  Registry registry;
  scene::registerProfileBundles(registry);
  scene::registerProfileBundleOps(registry);
  const auto input = fixture();
  Data scan;
  std::string why;
  REQUIRE(make(input, scan, why));
  test::OpCall split(registry);
  split.inputs = {{"scan", scan}};
  REQUIRE(split.run("gap.split_scan_pair").ok);
  CHECK(split.outputs.size() == 6);
  CHECK(split.out("primary").profilePtr() == input.primary.profilePtr());
  CHECK(split.out("sensorSecondary").profilePtr() == input.sensorSecondary.profilePtr());
  auto fields = split.outputs;
  test::OpCall assemble(registry);
  assemble.inputs = fields;
  REQUIRE(assemble.run("gap.make_scan_pair").ok);
  scene::ScanView view;
  REQUIRE(scene::readScanPair(assemble.out("scan"), view, why));
  CHECK(view.merged->profilePtr() == input.merged.profilePtr());
  CHECK(view.info->data == split.out("info").asRecord()->data);
  assemble.inputs["guardQuality"] = Data::record(Record{"gap.CameraGuard.v3", Json::object()});
  CHECK_FALSE(assemble.run("gap.make_scan_pair").ok);
  CHECK(assemble.outputs.empty());
  assemble.inputs = fields;
  assemble.inputs.erase("info");
  REQUIRE(assemble.run("gap.make_scan_pair").ok);
  REQUIRE(scene::readScanPair(assemble.out("scan"), view, why));
  CHECK(view.info->data["processing"]["method"] == "assemble");
  CHECK(view.info->data["origins"]["primary"]["sourceId"] ==
        input.sensorPrimary.asProfile()->descriptor.sourceId);
}

TEST_CASE("Gap native ROI nodes derive the seam center and reject foreign spaces") {
  Registry registry;
  scene::registerProfileBundles(registry);
  scene::registerProfileBundleOps(registry);
  const auto input = fixture();
  auto fields = roiFields(input.primary);
  Data rois;
  std::string why;
  REQUIRE(scene::makeRoiSet(fields, rois, why));
  test::OpCall split(registry);
  split.inputs = {{"rois", rois}};
  REQUIRE(split.run("gap.split_roi_set").ok);
  CHECK(split.outputs.size() == 11);
  CHECK(split.out("seam").asBox2D()->min[0] == 3);
  CHECK(split.out("seam").asBox2D()->max[0] == 3);
  CHECK(split.out("seam").asBox2D()->min[1] == 2);
  CHECK(split.out("seamContext").asRecord()->data == fields.contexts[2].asRecord()->data);
  auto parts = split.outputs;
  parts.erase("seam");
  parts.erase("seamContext");
  test::OpCall assemble(registry);
  assemble.inputs = parts;
  REQUIRE(assemble.run("gap.make_roi_set").ok);
  scene::RoiView view;
  REQUIRE(scene::readRoiSet(assemble.out("rois"), view, why));
  CHECK(view.info->data == fields.info.data);
  assemble.inputs["overall"] = fields.boxes[2];
  assemble.inputs["overallContext"] = fields.contexts[2];
  CHECK_FALSE(assemble.run("gap.make_roi_set").ok);
  REQUIRE(assemble.run("gap.make_roi_set", {{"metadataMode", Value::text("update_metadata")}}).ok);
  REQUIRE(scene::readRoiSet(assemble.out("rois"), view, why));
  CHECK(view.info->data["source"] == fields.info.data["source"]);
  CHECK(view.info->data["datumSide"] == fields.info.data["datumSide"]);
  CHECK(view.info->data["processing"]["details"]["metadataUpdate"]["overallUpdated"] == true);
  CHECK(view.info->data["overallMm"][0] ==
        static_cast<double>(fields.boxes[2].asBox2D()->min[0]) * 1000);
  assemble.inputs["overallContext"] = space(input.sensorPrimary);
  CHECK_FALSE(
      assemble.run("gap.make_roi_set", {{"metadataMode", Value::text("update_metadata")}}).ok);
  CHECK(assemble.outputs.empty());
  parts.erase("info");
  assemble.inputs = parts;
  REQUIRE(assemble.run("gap.make_roi_set").ok);
  REQUIRE(scene::readRoiSet(assemble.out("rois"), view, why));
  CHECK(view.info->data["processing"]["method"] == "assemble");
  CHECK(view.info->data["source"] == "config");
  assemble.inputs.erase("targetContext");
  CHECK_FALSE(assemble.run("gap.make_roi_set").ok);
  CHECK(assemble.outputs.empty());
}
