#include <doctest/doctest.h>
#include "helpers.h"
#include "ops/profile_bundle_ops.h"

#include <limits>

#include "lyflow/profile.h"
#include "lyflow_profile/provenance.h"
#include "ops/profile_scene.h"
#include "ops/profile_guard.h"

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
Data select(const Data& data, std::vector<std::int32_t> keep) {
  const auto& input = *data.asProfile();
  Profile result;
  result.descriptor = input.descriptor;
  result.cloud = std::make_shared<const PointCloud>(input.cloud->select(keep));
  for (const auto index : keep) {
    result.validity.push_back(input.validity[index]);
    result.sourceIndices.push_back(input.sourceIndices[index]);
  }
  return Data::profile(std::move(result));
}
std::pair<Data, Record> merge(const Data& secondary, const Data& primary) {
  PointCloud cloud;
  Profile result;
  std::uint64_t offset = 0;
  for (const auto* data : {&secondary, &primary}) {
    const auto& profile = *data->asProfile();
    const auto& input = *profile.cloud;
    cloud.xyz.insert(cloud.xyz.end(), input.xyz.begin(), input.xyz.end());
    cloud.intensity.insert(cloud.intensity.end(), input.intensity.begin(), input.intensity.end());
    cloud.normals.insert(cloud.normals.end(), input.normals.begin(), input.normals.end());
    cloud.rgb.insert(cloud.rgb.end(), input.rgb.begin(), input.rgb.end());
    result.validity.insert(result.validity.end(), profile.validity.begin(), profile.validity.end());
    for (const auto index : profile.sourceIndices)
      result.sourceIndices.push_back(static_cast<std::uint32_t>(offset + index));
    offset += profile.descriptor.sourceCount;
  }
  result.cloud = std::make_shared<const PointCloud>(std::move(cloud));
  Record provenance;
  std::string why;
  REQUIRE(profile_contract::makeCompositeProvenance(secondary, primary, nullptr, nullptr,
                                                    "secondary", "primary", "require_equal", result,
                                                    provenance, why));
  return {Data::profile(std::move(result)), std::move(provenance)};
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
  Record provenance;
  scene::ScanMetadata info;
};
Fixture fixture() {
  Fixture result;
  result.sensorPrimary = sensor("declared-file-A");
  result.sensorSecondary = sensor("declared-file-B");
  result.primary = measurement(result.sensorPrimary);
  result.secondary = measurement(result.sensorSecondary);
  auto combined = merge(result.secondary, result.primary);
  result.merged = combined.first;
  result.provenance = combined.second;
  result.info = metadata(result.sensorPrimary, result.sensorSecondary);
  return result;
}
Fixture guardedFixture(bool duplicate) {
  auto result = fixture();
  Data raw[2];
  for (int side = 0; side < 2; ++side) {
    auto profile = *sensor(side ? "declared-file-B" : "declared-file-A").asProfile();
    auto cloud = *profile.cloud;
    cloud.xyz = {1, 0, 3 + .0015F * side, 1.001F, 0, 3 + .0015F * side};
    profile.cloud = std::make_shared<const PointCloud>(std::move(cloud));
    profile.validity = {1, 1};
    std::string why;
    REQUIRE(validateProfile(profile, why));
    raw[side] = Data::profile(std::move(profile));
  }
  scene::CameraGuardOptions options;
  options.onDisagree = duplicate ? "keepPrimary" : "record";
  options.sampleStepMm = .2;
  options.halfWindowMm = 1;
  options.minSamples = 1;
  scene::GuardResult guarded;
  test::StubContext context;
  REQUIRE(scene::guardProfiles(raw[0], raw[1], options, guarded, context).ok);
  REQUIRE(guarded.quality.data.at("evaluated") == true);
  REQUIRE(guarded.quality.data.at("exceeded") == true);
  result.sensorPrimary = guarded.primary;
  result.sensorSecondary = guarded.secondary;
  result.primary = measurement(result.sensorPrimary);
  result.secondary = measurement(result.sensorSecondary);
  auto combined = merge(result.secondary, result.primary);
  result.merged = combined.first;
  result.provenance = combined.second;
  result.info = metadata(result.sensorPrimary, result.sensorSecondary);
  result.info.guard = guarded.quality.data;
  return result;
}
bool make(const Fixture& input, Data& output, std::string& why) {
  return scene::makeScanPair(input.sensorPrimary, input.sensorSecondary, input.primary,
                             input.secondary, input.merged, input.provenance, input.info, output,
                             why);
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
    fields.contexts[i] = Data::record(Record{
        "profile.Geometry.v2", profile_contract::geometryContextV2(measurement, fields.boxes[i])});
  }
  fields.info = {
      scene::kRoiInfoType,
      {{"schemaVersion", 2},
       {"datumSide", "left"},
       {"source", "model"},
       {"alignment", nullptr},
       {"overallMm", nullptr},
       {"cropStatus", nullptr},
       {"processing", {{"method", "model"}, {"status", "complete"}, {"details", Json::object()}}}}};
  return fields;
}
} // namespace

TEST_CASE(
    "Gap native scene bundles preserve sensor slots actual sources and mapped measurement channels") {
  const auto input = fixture();
  const auto before = profile_contract::context(input.sensorPrimary);
  Data output;
  std::string why;
  REQUIRE_MESSAGE(make(input, output, why), why);
  scene::ScanView view;
  REQUIRE(scene::readScanPair(output, view, why));
  CHECK(output.asBundle()->fields.size() == 7);
  CHECK(view.sensorPrimary->profilePtr() == input.sensorPrimary.profilePtr());
  CHECK(view.primary->asProfile()->descriptor.plane == "XY");
  CHECK(view.primary->asProfile()->descriptor.handedness == "reflected");
  CHECK(view.primary->asProfile()->cloud->xyz[1] == 3);
  CHECK(view.primary->asProfile()->cloud->normals == std::vector<float>{0, 0, 1, 1, 0, 0});
  CHECK(view.sensorPrimary->asProfile()->cloud->rgb == input.primary.asProfile()->cloud->rgb);
  CHECK(view.merged->asProfile()->sourceIndices == std::vector<std::uint32_t>{2, 5, 10, 13});
  CHECK(view.info->data["roles"]["primary"]["actualSourceId"] == "declared-file-A");
  CHECK(view.info->data["roles"]["secondary"]["modelRow"] == 1);
  CHECK(view.info->data["processing"]["status"] == "complete");
  CHECK(view.info->data["crop"].is_null());
  CHECK(view.info->data.dump().size() < 8192);
  CHECK(view.pick("secondary") == view.secondary);
  CHECK(view.pick("unknown") == nullptr);
  CHECK(profile_contract::context(input.sensorPrimary) == before);
  Registry registry;
  scene::registerProfileBundles(registry);
  REQUIRE(registry.findBundle(scene::kScanPairKind));
  CHECK(registry.findBundle(scene::kScanPairKind)->fields.size() == 7);
  CHECK(registry.findBundle(scene::kRoiSetKind)->fields.size() == 9);
  auto assembled = input;
  assembled.info.processing = {
      {"method", "assemble"}, {"status", "complete"}, {"details", {{"relation", "declared"}}}};
  Data declaration;
  REQUIRE(make(assembled, declaration, why));
  REQUIRE(scene::readScanPair(declaration, view, why));
  CHECK(view.info->data["processing"]["method"] == "assemble");
  CHECK(view.info->data["processing"]["details"]["relation"] == "declared");
}

TEST_CASE(
    "Gap native scene validates real millimeter conversion and rejects invented mappings atomically") {
  auto input = fixture();
  input.sensorPrimary = sensor("declared-file-A", "mm");
  input.primary = measurement(input.sensorPrimary);
  auto combined = merge(input.secondary, input.primary);
  input.merged = combined.first;
  input.provenance = combined.second;
  Data output;
  std::string why;
  REQUIRE_MESSAGE(make(input, output, why), why);
  CHECK(input.primary.asProfile()->cloud->xyz[1] == doctest::Approx(.003));
  const auto original = output.asBundle();
  auto changed = *input.primary.asProfile();
  auto cloud = *changed.cloud;
  cloud.xyz[1] = .004F;
  changed.cloud = std::make_shared<const PointCloud>(std::move(cloud));
  input.primary = Data::profile(changed);
  CHECK_FALSE(make(input, output, why));
  CHECK(output.asBundle() == original);
  input = fixture();
  changed = *input.primary.asProfile();
  cloud = *changed.cloud;
  cloud.rgb[0] = 0;
  changed.cloud = std::make_shared<const PointCloud>(std::move(cloud));
  input.primary = Data::profile(changed);
  CHECK_FALSE(make(input, output, why));
  CHECK(output.asBundle() == original);
  input = fixture();
  changed = *input.primary.asProfile();
  changed.descriptor.sourceId = "fabricated-other-camera";
  input.primary = Data::profile(changed);
  CHECK_FALSE(make(input, output, why));
  input = fixture();
  changed = *input.sensorSecondary.asProfile();
  changed.descriptor.handedness = "reflected";
  input.sensorSecondary = Data::profile(changed);
  input.secondary = measurement(input.sensorSecondary);
  CHECK_FALSE(make(input, output, why));
  CHECK(why.find("相同坐标帧") != std::string::npos);
  CHECK(output.asBundle() == original);
}

TEST_CASE(
    "Gap processed scene permits filtered subsets with original measurement parent bindings") {
  auto input = fixture();
  const auto rawMerged = input.merged;
  input.primary = select(input.primary, {0});
  input.secondary = select(input.secondary, {0});
  input.merged = select(input.merged, {0, 2});
  Record updated;
  std::string why;
  REQUIRE(
      profile_contract::rebindProvenance(rawMerged, input.merged, &input.provenance, updated, why));
  input.provenance = updated;
  input.info.stage = "processed";
  input.info.crop = {{"status", "applied"}, {"before", 4}, {"after", 2}};
  input.info.processing = {{"method", "template"},
                           {"status", "complete"},
                           {"details", {{"templateId", "explicit-candidate"}}}};
  Data output;
  REQUIRE_MESSAGE(make(input, output, why), why);
  scene::ScanView view;
  REQUIRE(scene::readScanPair(output, view, why));
  CHECK(view.sensorPrimary->asProfile()->pointCount() == 2);
  CHECK(view.primary->asProfile()->pointCount() == 1);
  CHECK(view.info->data["roles"]["primary"]["sensorStage"] == "raw");
  CHECK(view.info->data["roles"]["primary"]["measurementStage"] == "processed");
  CHECK(view.mergedProvenance->data["recipe"]["segments"][0]["sourceContext"] !=
        profile_contract::context(input.secondary));
  input.info.stage = "raw";
  CHECK_FALSE(make(input, output, why));
  input.info.stage = "processed";
  input.primary = select(input.primary, {});
  CHECK_FALSE(make(input, output, why));
}

TEST_CASE(
    "Gap guard duplication preserves actual source identity and both logical segment weights") {
  auto input = guardedFixture(true);
  Data output;
  std::string why;
  REQUIRE_MESSAGE(make(input, output, why), why);
  scene::ScanView view;
  REQUIRE(scene::readScanPair(output, view, why));
  CHECK(view.info->data["roles"]["secondary"]["actualSourceId"] == "declared-file-A");
  CHECK(view.info->data["roles"]["secondary"]["logicalRole"] == "secondary");
  CHECK(view.merged->asProfile()->pointCount() == 4);
  CHECK(view.merged->asProfile()->descriptor.sourceCount == 16);
  input.info.origins["secondary"]["sourceId"] = "declared-file-B";
  CHECK_FALSE(make(input, output, why));
  input = fixture();
  input.info.guard = {{"taken", "primary"},
                      {"evaluated", true},
                      {"exceeded", true},
                      {"deltaMedianMm", 1.5},
                      {"signedMedianMm", 1.5}};
  CHECK_FALSE(make(input, output, why));
}

TEST_CASE(
    "Gap scene rejects stale metadata unknown bundle fields sensor provenance and merged reorder") {
  auto input = fixture();
  Data output;
  std::string why;
  REQUIRE(make(input, output, why));
  auto bundle = *output.asBundle();
  auto info = *bundle.field("info")->asRecord();
  info.data["roles"]["primary"]["actualSourceId"] = "forged";
  bundle.set("info", Data::record(info));
  scene::ScanView sentinel;
  sentinel.primary = &input.primary;
  CHECK_FALSE(scene::readScanPair(Data::bundle(bundle), sentinel, why));
  CHECK(sentinel.primary == &input.primary);
  bundle = *output.asBundle();
  bundle.fields.push_back({"primary", input.primary});
  CHECK_FALSE(scene::readScanPair(Data::bundle(bundle), sentinel, why));
  bundle = *output.asBundle();
  bundle.fields[0].first = "unknown";
  CHECK_FALSE(scene::readScanPair(Data::bundle(bundle), sentinel, why));
  input.info.guard["deltaMedianMm"] = 0;
  CHECK_FALSE(make(input, output, why));
  input = fixture();
  auto profile = *input.merged.asProfile();
  profile.descriptor.sampleOrder = "unknown";
  input.merged = select(Data::profile(profile), {2, 0});
  input.provenance = profile_contract::provenance_detail::record(input.merged, "derived_composite",
                                                                 input.provenance.data["recipe"]);
  CHECK_FALSE(make(input, output, why));
  input = fixture();
  auto recipe = input.provenance.data["recipe"];
  recipe["segments"][0]["sourceContext"] = profile_contract::context(input.sensorSecondary);
  input.provenance =
      profile_contract::provenance_detail::record(input.merged, "derived_composite", recipe);
  CHECK_FALSE(make(input, output, why));
}

TEST_CASE("Gap role boxes retain all four geometry bindings and honest optional metadata") {
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
  auto box = *fields.boxes[0].asBox2D();
  box.max[0] = 5;
  fields.boxes[0] = Data::box2d(box);
  CHECK_FALSE(scene::makeRoiSet(fields, output, why));
  CHECK(output.asBundle() == original);
  fields = roiFields(input.primary);
  fields.contexts[0] = Data::record(
      Record{"profile.Geometry.v2",
             profile_contract::geometryContextV2(input.sensorPrimary, fields.boxes[0])});
  CHECK_FALSE(scene::makeRoiSet(fields, output, why));
  fields = roiFields(input.primary);
  fields.info.data["source"] = "";
  CHECK_FALSE(scene::makeRoiSet(fields, output, why));
  fields = roiFields(input.primary);
  fields.info.data["processing"]["status"] = "ready";
  CHECK_FALSE(scene::makeRoiSet(fields, output, why));
  fields = roiFields(input.primary);
  fields.info.data["source"] = "template";
  fields.info.data["processing"]["method"] = "template";
  fields.info.data["roisFrom"] = "config";
  REQUIRE(scene::makeRoiSet(fields, output, why));
  REQUIRE(scene::readRoiSet(output, view, why));
  CHECK(view.info->data["roisFrom"] == "config");
}

TEST_CASE("Gap native bundle nodes round trip immutable profiles and complete provenance") {
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
  CHECK(split.outputs.size() == 9);
  CHECK(split.out("primary").profilePtr() == input.primary.profilePtr());
  CHECK(split.out("sensorSecondary").profilePtr() == input.sensorSecondary.profilePtr());
  Json sources;
  REQUIRE(profile_contract::profileSources(
      split.out("primary"), split.out("primaryProvenance").asRecord(), sources, why));
  CHECK(sources[0] == profile_contract::context(input.primary));
  auto fields = split.outputs;
  fields.erase("primaryProvenance");
  fields.erase("secondaryProvenance");
  test::OpCall assemble(registry);
  assemble.inputs = fields;
  REQUIRE(assemble.run("gap.make_scan_pair").ok);
  scene::ScanView view;
  REQUIRE(scene::readScanPair(assemble.out("scan"), view, why));
  CHECK(view.merged->profilePtr() == input.merged.profilePtr());
  CHECK(view.info->data == split.out("info").asRecord()->data);
  CHECK(view.mergedProvenance->data == input.provenance.data);
  auto stale = *fields.at("info").asRecord();
  stale.data["roles"]["primary"]["actualSourceId"] = "different-physical-source";
  assemble.inputs["info"] = Data::record(stale);
  CHECK_FALSE(assemble.run("gap.make_scan_pair").ok);
  CHECK(assemble.outputs.empty());
  CHECK(profile_contract::context(input.primary) ==
        profile_contract::context(fields.at("primary")));
  assemble.inputs = fields;
  assemble.inputs.erase("info");
  REQUIRE(assemble.run("gap.make_scan_pair").ok);
  REQUIRE(scene::readScanPair(assemble.out("scan"), view, why));
  CHECK(view.info->data["processing"]["method"] == "assemble");
  CHECK(view.info->data["roles"]["primary"]["actualSourceId"] ==
        input.sensorPrimary.asProfile()->descriptor.sourceId);
  assemble.inputs["sourceInfo"] = Data::record(
      Record{"gap.ProfilePairSource.v2", {{"schemaVersion", "2"}, {"origins", Json::object()}}});
  CHECK_FALSE(assemble.run("gap.make_scan_pair").ok);
  CHECK(assemble.outputs.empty());
  assemble.inputs.erase("sourceInfo");
  assemble.inputs["guardQuality"] = Data::record(Record{"gap.CameraGuard.v2", Json::object()});
  CHECK_FALSE(assemble.run("gap.make_scan_pair").ok);
  CHECK(assemble.outputs.empty());
}

TEST_CASE(
    "Gap native ROI nodes bind seam center to both actual parents and reject stale bindings") {
  Registry registry;
  scene::registerProfileBundles(registry);
  scene::registerProfileBundleOps(registry);
  const auto input = fixture();
  auto fields = roiFields(input.primary);
  fields.contexts[3] =
      Data::record(Record{"profile.Geometry.v2",
                          profile_contract::geometryContextV2(input.secondary, fields.boxes[3])});
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
  CHECK(split.out("seamContext").asRecord()->data["sources"].size() == 2);
  ProfileDescriptor space;
  REQUIRE(profile_contract::geometryMatchesPhysical(
      split.out("seam"), split.out("seamContext").asRecord(), space, why));
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
  assemble.inputs["overallContext"] = fields.contexts[3];
  CHECK_FALSE(
      assemble.run("gap.make_roi_set", {{"metadataMode", Value::text("update_metadata")}}).ok);
  CHECK(assemble.outputs.empty());
  assemble.inputs = parts;
  auto changed = *parts.at("target").asBox2D();
  changed.max[0] += 1;
  assemble.inputs["target"] = Data::box2d(changed);
  CHECK_FALSE(assemble.run("gap.make_roi_set").ok);
  CHECK(assemble.outputs.empty());
  parts.erase("info");
  assemble.inputs = parts;
  assemble.inputs["target"] = fields.boxes[1];
  REQUIRE(assemble.run("gap.make_roi_set").ok);
  REQUIRE(scene::readRoiSet(assemble.out("rois"), view, why));
  CHECK(view.info->data["processing"]["method"] == "assemble");
  CHECK(view.info->data["source"] == "config");
  assemble.inputs.erase("targetContext");
  CHECK_FALSE(assemble.run("gap.make_roi_set").ok);
  CHECK(assemble.outputs.empty());
}

TEST_CASE("Gap guard metadata roundtrip binds actual contexts and rejects schema downgrade") {
  const auto input = guardedFixture(false);
  Data output;
  std::string why;
  REQUIRE(make(input, output, why));
  scene::ScanView view;
  REQUIRE(scene::readScanPair(output, view, why));
  auto original = *output.asBundle();
  const auto actualInfo = *original.field("info")->asRecord();
  CHECK(actualInfo.data.at("guard") == input.info.guard);
  std::vector<Json> invalid;
  auto stale = input.info.guard;
  stale["primaryContext"] = profile_contract::context(input.sensorSecondary);
  invalid.push_back(stale);
  stale = input.info.guard;
  stale.erase("schemaVersion");
  invalid.push_back(stale);
  stale = input.info.guard;
  stale["maxDeltaMm"] = 100;
  invalid.push_back(stale);
  invalid.push_back({{"taken", "both"},
                     {"evaluated", true},
                     {"exceeded", false},
                     {"onDisagree", "record"},
                     {"deltaMedianMm", .1},
                     {"signedMedianMm", .1}});
  invalid.push_back(
      {{"taken", "both"}, {"evaluated", false}, {"exceeded", false}, {"onDisagree", "record"}});
  Registry registry;
  scene::registerProfileBundles(registry);
  scene::registerProfileBundleOps(registry);
  for (const auto& guard : invalid) {
    CAPTURE(guard);
    auto bundle = original;
    auto info = actualInfo;
    info.data["guard"] = guard;
    bundle.set("info", Data::record(info));
    scene::ScanView sentinel;
    sentinel.primary = &input.primary;
    CHECK_FALSE(scene::readScanPair(Data::bundle(bundle), sentinel, why));
    CHECK(sentinel.primary == &input.primary);
    test::OpCall assemble(registry);
    for (const auto& field : bundle.fields)
      assemble.inputs[field.first] = field.second;
    CHECK_FALSE(assemble.run("gap.make_scan_pair").ok);
    CHECK(assemble.outputs.empty());
  }
}
