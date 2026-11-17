#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <fstream>

#include "helpers.h"
#include "exec/plan.h"
#include "lyflow_profile/merge.h"
#include "ops/profile_scan.h"

using namespace lyflow;
namespace scene = lyflow::packs::gap::profile_scene;
namespace {
struct LocalContext : ExecContext {
  std::filesystem::path base;
  bool cancelled() const override { return false; }
  void progress(float, std::string_view) override {}
  void log(LogLevel, std::string) override {}
  const std::filesystem::path& baseDir() const override { return base; }
  int threadBudget() const override { return 1; }
};
namespace fs = std::filesystem;
class Files {
public:
  fs::path root;
  Files() {
    static std::atomic<unsigned> sequence{0};
    root = fs::temp_directory_path() / fs::u8path(u8"lyflow 原生轮廓") /
           (std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
            std::to_string(sequence.fetch_add(1)));
    fs::create_directories(root);
  }
  ~Files() {
    std::error_code error;
    fs::remove_all(root, error);
  }
  fs::path write(const std::string& name, bool profile = false, bool millimeters = false) {
    const auto path = root / fs::u8path(name);
    std::ofstream file(path);
    file << "VERSION .7\nFIELDS x y z intensity normal_x normal_y normal_z rgb\n"
            "SIZE 4 4 4 4 4 4 4 4\nTYPE F F F F F F F U\nCOUNT 1 1 1 1 1 1 1 1\n"
            "WIDTH 4\nHEIGHT 1\nPOINTS 4\nDATA ascii\n";
    const auto height = millimeters ? "1000" : "1";
    const auto y = profile ? height : "0";
    const auto z = profile ? "0" : height;
    file << "0 " << y << ' ' << z << " 7 0 1 0 660510\n";
    file << (millimeters ? "1" : ".001") << ' ' << y << ' ' << z << " 8 0 1 0 660510\n";
    file << "nan nan nan 9 0 1 0 660510\n";
    file << (millimeters ? "10000" : "10") << ' ' << y << ' ' << z << " 10 0 1 0 660510\n";
    file.close();
    REQUIRE(file.good());
    return path;
  }
};
scene::ProfilePairOptions options(Files& files) {
  scene::ProfilePairOptions result;
  result.source = "files";
  result.primaryFile = files.write(u8"首 相机.pcd");
  result.secondaryFile = files.write(u8"次 相机.pcd");
  result.layout = "sensor";
  result.unit = "m";
  result.fileFrame = "file-sensor-frame";
  result.sensorFrame = "sensor-frame";
  result.handedness = "right";
  return result;
}
struct Stop : LocalContext {
  mutable int polls = 0;
  int stopAt = std::numeric_limits<int>::max();
  bool cancelled() const override { return ++polls >= stopAt; }
};
const Registry& registry() {
  static const Registry value = [] {
    Registry result;
    scene::registerProfileBundles(result);
    scene::registerProfileScanOps(result);
    return result;
  }();
  return value;
}
Record copiedGuard(const Data& primary, const Data& secondary) {
  return {"gap.CameraGuard.v3",
          {{"schemaVersion", 3},
           {"onDisagree", "keepPrimary"},
           {"maxDeltaMm", .5},
           {"taken", "primary"},
           {"evaluated", true},
           {"exceeded", true},
           {"deltaMedianMm", 1.0},
           {"signedMedianMm", 1.0},
           {"inputPrimarySourceId", primary.asProfile()->descriptor.sourceId},
           {"inputSecondarySourceId", secondary.asProfile()->descriptor.sourceId},
           {"primarySourceId", primary.asProfile()->descriptor.sourceId},
           {"secondarySourceId", primary.asProfile()->descriptor.sourceId}}};
}
} // namespace

TEST_CASE(
    "Gap PCD native pair loading preserves NaN slots original indices and every present channel") {
  Files files;
  auto config = options(files);
  LocalContext context;
  scene::LoadedProfilePair result;
  REQUIRE(scene::loadProfilePair(config, result, context).ok);
  const auto* profile = result.primary.asProfile();
  REQUIRE(profile);
  CHECK(profile->descriptor.sourceCount == 4);
  CHECK(profile->sourceIndices == std::vector<std::uint32_t>{0, 1, 2, 3});
  CHECK(profile->validity == std::vector<std::uint8_t>{1, 1, 0, 1});
  CHECK(std::isnan(profile->cloud->xyz[6]));
  CHECK(profile->cloud->intensity == std::vector<float>{7, 8, 9, 10});
  CHECK(profile->cloud->normals.size() == 12);
  CHECK(profile->cloud->rgb ==
        std::vector<std::uint8_t>{10, 20, 30, 10, 20, 30, 10, 20, 30, 10, 20, 30});
  CHECK(profile->descriptor.sourceId == "file:" + fs::canonical(config.primaryFile).u8string());
  CHECK(result.sourceInfo.data["origins"]["primary"]["kind"] == "file");
  CHECK(result.sourceInfo.data["origins"]["primary"]["details"]["fileFrame"] ==
        "file-sensor-frame");
  CHECK(result.sourceInfo.data.dump().size() < 4096);
  config.primarySourceId = "declared-capture-A";
  REQUIRE(scene::loadProfilePair(config, result, context).ok);
  CHECK(result.primary.asProfile()->descriptor.sourceId == "declared-capture-A");
}

TEST_CASE(
    "Gap PCD directory selection retains case sensitive extension and sorted prefix behavior") {
  Files files;
  auto config = options(files);
  config.source = "dir";
  config.dir = files.root;
  config.primaryPrefix = "L0_";
  config.secondaryPrefix = "R1_";
  files.write("L0_00.PCD");
  const auto first = files.write("L0_01.pcd");
  files.write("L0_02.pcd");
  files.write("L0_00.txt");
  files.write("R1_02.pcd");
  const auto second = files.write("R1_01.pcd");
  scene::LoadedProfilePair result;
  LocalContext context;
  REQUIRE(scene::loadProfilePair(config, result, context).ok);
  CHECK(result.primary.asProfile()->descriptor.sourceId ==
        "file:" + fs::canonical(first).u8string());
  CHECK(result.secondary.asProfile()->descriptor.sourceId ==
        "file:" + fs::canonical(second).u8string());
  config.secondaryPrefix = "missing";
  const auto before = result.primary.profilePtr();
  CHECK_FALSE(scene::loadProfilePair(config, result, context).ok);
  CHECK(result.primary.profilePtr() == before);
}

TEST_CASE(
    "Gap profile layout loading uses shared signed axis mapping and explicit millimeter conversion") {
  Files files;
  auto config = options(files);
  config.layout = "profile";
  config.unit = "mm";
  config.fileFrame = "archived-profile-frame";
  config.primaryFile = files.write("archived-A.pcd", true, true);
  config.secondaryFile = files.write("archived-B.pcd", true, true);
  scene::LoadedProfilePair loaded;
  LocalContext context;
  REQUIRE(scene::loadProfilePair(config, loaded, context).ok);
  const auto* sensor = loaded.primary.asProfile();
  CHECK(sensor->descriptor.unit == "mm");
  CHECK(sensor->descriptor.plane == "XZ");
  CHECK(sensor->descriptor.handedness == "reflected");
  CHECK(sensor->cloud->xyz[2] == 1000);
  CHECK(sensor->cloud->normals[2] == 1);
  scene::ReadScanOptions read;
  read.measurementFrame = "measurement-frame";
  read.origins = loaded.sourceInfo.data["origins"];
  Data scan;
  REQUIRE_MESSAGE(scene::buildReadScan(loaded.primary, loaded.secondary, read, scan, context).ok,
                  "explicit profile layout and unit mapping");
  scene::ScanView view;
  std::string why;
  REQUIRE(scene::readScanPair(scan, view, why));
  CHECK(view.primary->asProfile()->cloud->xyz[4] == doctest::Approx(1));
  CHECK(view.primary->asProfile()->cloud->xyz[3] == doctest::Approx(.001));
  CHECK(view.primary->asProfile()->descriptor.handedness == "right");
  CHECK(view.primary->asProfile()->cloud->normals[1] == 1);
  CHECK(view.sensorPrimary->asProfile()->pointCount() == 4);
}

TEST_CASE(
    "Gap typed read scan keeps raw rows and applies compaction radius only to merged source slots") {
  Files files;
  auto config = options(files);
  scene::LoadedProfilePair loaded;
  LocalContext context;
  REQUIRE(scene::loadProfilePair(config, loaded, context).ok);
  scene::ReadScanOptions read;
  read.measurementFrame = "measurement-frame";
  read.origins = loaded.sourceInfo.data["origins"];
  Data scan;
  REQUIRE(scene::buildReadScan(loaded.primary, loaded.secondary, read, scan, context).ok);
  scene::ScanView view;
  std::string why;
  REQUIRE(scene::readScanPair(scan, view, why));
  CHECK(view.primary->asProfile()->pointCount() == 4);
  CHECK(view.secondary->asProfile()->pointCount() == 4);
  CHECK(view.merged->asProfile()->sourceIndices == std::vector<std::uint32_t>{0, 1, 3, 4, 5, 7});
  CHECK(view.merged->asProfile()->descriptor.sourceCount == 8);
  CHECK(view.info->data["processing"]["details"]["radius"]["status"] == "skipped");
  CHECK(view.info->data["processing"]["details"]["radius"]["radiusMm"].is_null());
  read.removeOutliers = true;
  read.outlierRadiusMm = 2;
  read.outlierNeighbors = 2;
  REQUIRE(scene::buildReadScan(loaded.primary, loaded.secondary, read, scan, context).ok);
  REQUIRE(scene::readScanPair(scan, view, why));
  CHECK(view.merged->asProfile()->sourceIndices == std::vector<std::uint32_t>{0, 1, 4, 5});
  CHECK(view.primary->asProfile()->sourceIndices == std::vector<std::uint32_t>{0, 1, 2, 3});
  CHECK(view.info->data["processing"]["details"]["radius"]["before"] == 6);
  CHECK(view.info->data["processing"]["details"]["radius"]["after"] == 4);
  auto allMissing = *loaded.primary.asProfile();
  auto noChannels = *allMissing.cloud;
  const auto nan = std::numeric_limits<float>::quiet_NaN();
  std::fill(noChannels.xyz.begin(), noChannels.xyz.end(), nan);
  noChannels.intensity.clear();
  noChannels.normals.clear();
  noChannels.rgb.clear();
  allMissing.cloud = std::make_shared<const PointCloud>(std::move(noChannels));
  allMissing.validity.assign(4, 0);
  read.removeOutliers = false;
  REQUIRE(
      scene::buildReadScan(Data::profile(allMissing), loaded.secondary, read, scan, context).ok);
  REQUIRE(scene::readScanPair(scan, view, why));
  CHECK(view.merged->asProfile()->cloud->hasRgb());
  CHECK(view.merged->asProfile()->pointCount() == 3);
  CHECK(view.primary->asProfile()->pointCount() == 4);
  auto mixed = *loaded.secondary.asProfile();
  auto mixedCloud = *mixed.cloud;
  mixedCloud.rgb.clear();
  mixed.cloud = std::make_shared<const PointCloud>(std::move(mixedCloud));
  REQUIRE(scene::buildReadScan(loaded.primary, Data::profile(mixed), read, scan, context).ok);
  REQUIRE(scene::readScanPair(scan, view, why));
  CHECK_FALSE(view.merged->asProfile()->cloud->hasRgb());
  Profile empty = *view.secondary->asProfile();
  empty.cloud = std::make_shared<const PointCloud>(empty.cloud->select({}));
  empty.validity.clear();
  empty.sourceIndices.clear();
  Profile cropped = *view.merged->asProfile();
  cropped.cloud = std::make_shared<const PointCloud>(cropped.cloud->select({3, 4, 5}));
  cropped.validity = {1, 1, 1};
  cropped.sourceIndices = {4, 5, 7};
  const auto cropData = Data::profile(std::move(cropped));
  scene::ScanMetadata processed{
      view.info->data["sensorFrame"].get<std::string>(),
      view.info->data["measurementFrame"].get<std::string>(),
      "processed",
      view.info->data["guard"],
      {{"status", "applied"}},
      view.info->data["origins"],
      {{"method", "template"}, {"status", "complete"}, {"details", nlohmann::json::object()}}};
  Data processedScan;
  REQUIRE(scene::makeScanPair(*view.sensorPrimary, *view.sensorSecondary, *view.primary,
                              Data::profile(std::move(empty)), cropData, processed, processedScan,
                              why));
  REQUIRE(scene::readScanPair(processedScan, view, why));
  CHECK_FALSE(view.merged->asProfile()->cloud->hasRgb());
}

TEST_CASE(
    "Gap native scan callbacks prefer complete Profile inputs and preserve trusted guard origins") {
  Files files;
  auto config = options(files);
  scene::LoadedProfilePair loaded;
  LocalContext context;
  REQUIRE(scene::loadProfilePair(config, loaded, context).ok);
  test::OpCall call(registry());
  call.inputs = {{"primary", loaded.primary}, {"secondary", loaded.secondary}};
  REQUIRE(call.run("gap.read_scan", {{"source", Value::text("inputs")},
                                     {"measurementFrame", Value::text("measurement-frame")},
                                     {"removeOutliers", Value::boolean(true)}})
              .ok);
  scene::ScanView filtered;
  std::string filteredWhy;
  REQUIRE(scene::readScanPair(call.out("scan"), filtered, filteredWhy));
  CHECK(filtered.info->data["processing"]["details"]["radius"]["radiusMm"] == .4);
  CHECK(filtered.info->data["processing"]["details"]["radius"]["minNeighbors"] == 5);
  CHECK(scene::ReadScanOptions{}.outlierRadiusMm == .4);
  CHECK(scene::ReadScanOptions{}.outlierNeighbors == 5);
  REQUIRE(call.run("gap.read_scan", {{"source", Value::text("files")},
                                     {"primaryFile", Value::text("missing-A.pcd")},
                                     {"secondaryFile", Value::text("missing-B.pcd")},
                                     {"measurementFrame", Value::text("measurement-frame")}})
              .ok);
  CHECK(call.out("sensorPrimary").profilePtr() == loaded.primary.profilePtr());
  CHECK(call.validate("gap.read_scan", {}, {"primary"}).size() == 1);
  CHECK(call.validate("gap.read_scan", {{"source", Value::text("inputs")}}, {}).empty());
  call.inputs.clear();
  CHECK_FALSE(call.run("gap.read_scan", {{"source", Value::text("inputs")}}).ok);
  CHECK(call.status.code == "missing_input");
  exec::RawGraph raw;
  Diagnostics diagnostics;
  auto graph = test::makeGraph(
      {{"read", "gap.read_scan", {{"source", "files"}, {"measurementFrame", "measurement-frame"}}}},
      {});
  graph["nodes"][0]["opVersion"] = "3.0.0";
  REQUIRE(exec::parseGraph(graph.dump(), raw, diagnostics));
  exec::BuildOptions build;
  build.runId = "profile-scan-input-priority";
  build.injectedPorts["read"] = {"primary", "secondary"};
  build.providedDigest["read"] = "actual-profile-inputs";
  exec::Plan plan;
  REQUIRE(exec::buildPlan(registry(), raw, build, plan, diagnostics));
  REQUIRE(plan.nodes.size() == 1);
  CHECK(plan.nodes[0].valid);
  for (const auto& diagnostic : diagnostics.items())
    CHECK((diagnostic.severity != Severity::Error));
  build.injectedPorts.clear();
  build.providedDigest.clear();
  diagnostics = {};
  REQUIRE(exec::buildPlan(registry(), raw, build, plan, diagnostics));
  REQUIRE(plan.nodes.size() == 1);
  CHECK_FALSE(plan.nodes[0].valid);
  call.inputs = {{"primary", loaded.primary},
                 {"secondary", loaded.primary},
                 {"sourceInfo", Data::record(loaded.sourceInfo)},
                 {"guardQuality", Data::record(copiedGuard(loaded.primary, loaded.secondary))}};
  REQUIRE(call.run("gap.read_scan", {{"source", Value::text("inputs")},
                                     {"measurementFrame", Value::text("measurement-frame")}})
              .ok);
  scene::ScanView view;
  std::string why;
  REQUIRE(scene::readScanPair(call.out("scan"), view, why));
  CHECK(view.info->data["origins"]["primary"] == view.info->data["origins"]["secondary"]);
  CHECK(view.info->data["guard"]["taken"] == "primary");
  CHECK(view.merged->asProfile()->pointCount() == 6);
}

TEST_CASE(
    "Gap native load and read failures cancellation and typed merge keep caller outputs atomic") {
  Files files;
  auto config = options(files);
  scene::LoadedProfilePair loaded;
  LocalContext context;
  REQUIRE(scene::loadProfilePair(config, loaded, context).ok);
  const auto original = loaded.primary.profilePtr();
  auto broken = config;
  broken.secondaryFile = files.root / "missing.pcd";
  CHECK_FALSE(scene::loadProfilePair(broken, loaded, context).ok);
  CHECK(loaded.primary.profilePtr() == original);
  broken = config;
  broken.unit.clear();
  CHECK_FALSE(scene::loadProfilePair(broken, loaded, context).ok);
  Stop stop;
  stop.stopAt = 1;
  CHECK_FALSE(scene::loadProfilePair(config, loaded, stop).ok);
  CHECK(loaded.primary.profilePtr() == original);
  scene::ReadScanOptions read;
  read.measurementFrame = "measurement-frame";
  Data scan;
  REQUIRE(scene::buildReadScan(loaded.primary, loaded.secondary, read, scan, context).ok);
  const auto* before = scan.asBundle();
  stop.polls = 0;
  CHECK_FALSE(scene::buildReadScan(loaded.primary, loaded.secondary, read, scan, stop).ok);
  CHECK(scan.asBundle() == before);
  read.removeOutliers = true;
  read.outlierRadiusMm = 0;
  CHECK_FALSE(scene::buildReadScan(loaded.primary, loaded.secondary, read, scan, context).ok);
  CHECK(scan.asBundle() == before);
  profile_contract::MergeResult merged;
  std::string why;
  REQUIRE(profile_contract::mergeProfiles(loaded.primary, loaded.secondary, "require_equal", merged,
                                          why));
  const auto mergeBefore = merged.profile.profilePtr();
  CHECK_FALSE(profile_contract::mergeProfiles(loaded.primary, loaded.secondary, "require_equal",
                                              merged, why, [] { return true; }));
  CHECK(merged.profile.profilePtr() == mergeBefore);
}
