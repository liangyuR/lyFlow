#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>

#include "lyflow/profile.h"
#include "lyflow_profile/selection.h"
#include "ops/profile_locate_template.h"
#include "ops/profile_scan.h"

using namespace lyflow;
namespace native = lyflow::packs::gap::profile_template;
namespace scene = lyflow::packs::gap::profile_scene;
namespace {
using Json = nlohmann::json;
struct Context : ExecContext {
  bool stop = false, stopAtIcp = false;
  std::filesystem::path base;
  bool cancelled() const override { return stop; }
  void progress(float, std::string_view) override {}
  void log(LogLevel, std::string message) override {
    if (stopAtIcp && message.rfind("ICP ", 0) == 0)
      stop = true;
  }
  const std::filesystem::path& baseDir() const override { return base; }
  int threadBudget() const override { return 1; }
};
Data sensor(const std::string& source) {
  PointCloud cloud;
  std::vector<std::uint8_t> validity;
  std::vector<std::uint32_t> indices;
  for (int i = 0; i < 160; ++i) {
    cloud.push((i - 80) * .00025F, 0, .16F + .001F * std::sin(i * .07F));
    cloud.intensity.push_back(8000 + i);
    validity.push_back(i == 7 ? 0 : 1);
    indices.push_back(i);
  }
  cloud.xyz[21] = cloud.xyz[22] = cloud.xyz[23] = std::numeric_limits<float>::quiet_NaN();
  Profile profile;
  std::string why;
  REQUIRE(makeProfile(std::make_shared<const PointCloud>(std::move(cloud)),
                      {1, "sensor-frame", "m", "XZ", "right", "acquisition", source, 160},
                      std::move(validity), std::move(indices), profile, why));
  return Data::profile(std::move(profile));
}
Data scan(bool denoised) {
  Context context;
  scene::ReadScanOptions options;
  options.measurementFrame = "measurement-frame";
  Data input;
  REQUIRE(
      scene::buildReadScan(sensor("physical-A"), sensor("physical-B"), options, input, context).ok);
  if (!denoised)
    return input;
  scene::ScanView view;
  std::string why;
  REQUIRE(scene::readScanPair(input, view, why));
  Indices indices;
  indices.sourceCloudId = view.merged->asProfile()->cloud->id;
  for (std::size_t i = 0; i < view.merged->asProfile()->pointCount(); ++i)
    if (view.merged->asProfile()->sourceIndices[i] != 20 &&
        view.merged->asProfile()->sourceIndices[i] != 190)
      indices.values.push_back(static_cast<std::int32_t>(i));
  profile_contract::SelectionResult selected;
  REQUIRE(profile_contract::selectProfileIndices(*view.merged, indices, view.mergedProvenance, {},
                                                 selected, context)
              .ok);
  const auto& old = view.info->data;
  scene::ScanMetadata metadata{old["sensorFrame"],
                               old["measurementFrame"],
                               "processed",
                               old["guard"],
                               old["crop"],
                               old["origins"],
                               old["processing"],
                               view.primaryProvenance,
                               view.secondaryProvenance};
  Data output;
  REQUIRE(scene::makeScanPair(*view.sensorPrimary, *view.sensorSecondary, *view.primary,
                              *view.secondary, selected.profile, selected.provenance, metadata,
                              output, why));
  return output;
}
struct Fixture {
  std::filesystem::path root;
  Fixture() {
    root = std::filesystem::temp_directory_path() / std::filesystem::u8path(u8"lyflow 高层模板") /
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(root);
    for (const auto* name :
         {"left_template.pcd", "right_template.pcd", "f4_left.pcd", "f4_right.pcd"})
      write(name);
  }
  void write(const char* name) {
    std::ofstream stream(root / name);
    stream
        << "VERSION 0.7\nFIELDS x y z\nSIZE 4 4 4\nTYPE F F F\nCOUNT 1 1 1\nWIDTH 160\nHEIGHT 1\nPOINTS 160\nDATA ascii\n"
        << std::setprecision(9);
    for (int i = 0; i < 160; ++i) {
      if (i == 7)
        stream << "nan nan nan\n";
      else
        stream << (i - 80) * .00025F << ' ' << .16F + .001F * std::sin(i * .07F) << " 0\n";
    }
  }
  ~Fixture() {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }
  native::LocateOptions options() const {
    native::LocateOptions out;
    out.minScore = 20;
    out.overall.roiMm = {-15, 150, 15, 170};
    for (std::size_t i = 0; i < 4; ++i) {
      auto& slot = out.slots[i];
      slot.enabled = i == 0 || i == 3;
      slot.load.dir = root;
      slot.load.left = i == 0 ? "left_template.pcd" : "f4_left.pcd";
      slot.load.right = i == 0 ? "right_template.pcd" : "f4_right.pcd";
      slot.alignment.templateId = i == 0 ? "z-primary" : "a-later";
      slot.alignment.maxIterations = 50;
      slot.alignment.normalKnn = 8;
      slot.alignment.roisMm = {
          {{{-14, 158, -8, 162}}, {{-5, 158, -3, 162}}, {{8, 157, 14, 161}}, {{3, 157, 5, 161}}}};
    }
    out.slots[0].alignment.roisFrom = "config";
    out.slots[1].load.dir = root / "missing-dir";
    out.slots[1].load.left = "disabled-missing.pcd";
    out.slots[1].alignment.roisMm = {};
    out.slots[2].alignment.roisMm[0][0] = std::numeric_limits<float>::quiet_NaN();
    return out;
  }
};
ParamMap defaults() {
  ParamMap values;
  for (const auto& p : native::locateParameters())
    values[p.name] = p.def;
  return values;
}
}
TEST_CASE(
    "Locate template crops existing denoised merged and preserves actual scan lineage and chosen slot") {
  Fixture fixture;
  const auto input = scan(true);
  const auto settings = fixture.options();
  CHECK(native::validateLocateOptions(settings).empty());
  scene::ScanView original;
  std::string why;
  REQUIRE(scene::readScanPair(input, original, why));
  native::LocateResult result;
  Context context;
  const auto status = native::locate(input, settings, result, context);
  REQUIRE_MESSAGE(status.ok, status.message);
  scene::ScanView output;
  REQUIRE_MESSAGE(scene::readScanPair(result.scan, output, why), why);
  CHECK(output.info->data["stage"] == "processed");
  CHECK(output.info->data["processing"]["method"] == "template");
  CHECK(output.info->data["processing"]["status"] == "complete");
  CHECK(output.info->data["guard"] == original.info->data["guard"]);
  CHECK(output.info->data["origins"] == original.info->data["origins"]);
  CHECK(output.sensorPrimary->asProfile()->cloud.get() ==
        original.sensorPrimary->asProfile()->cloud.get());
  CHECK(output.sensorSecondary->asProfile()->cloud.get() ==
        original.sensorSecondary->asProfile()->cloud.get());
  CHECK(output.merged->asProfile()->pointCount() < original.merged->asProfile()->pointCount());
  for (const auto index : output.merged->asProfile()->sourceIndices) {
    CHECK(std::find(original.merged->asProfile()->sourceIndices.begin(),
                    original.merged->asProfile()->sourceIndices.end(),
                    index) != original.merged->asProfile()->sourceIndices.end());
    CHECK(index != 20);
    CHECK(index != 190);
  }
  for (const auto* p : {output.primary, output.secondary, output.merged})
    for (std::size_t i = 0; i < p->asProfile()->pointCount(); ++i) {
      CHECK(p->asProfile()->cloud->xyz[3 * i] > -.015F);
      CHECK(p->asProfile()->cloud->xyz[3 * i] < .015F);
    }
  CHECK(output.merged->asProfile()->descriptor.sourceCount == 320);
  native::AlignmentView alignment;
  REQUIRE(native::readAlignment(result.alignment, alignment, why));
  CHECK(alignment.info->data["templateId"] == "z-primary");
  CHECK(alignment.info->data["order"] == 0);
  CHECK(alignment.info->data["roisFrom"] == "config");
  CHECK(profile_contract::context(*alignment.target) == profile_contract::context(*output.merged));
  scene::RoiView rois;
  REQUIRE(scene::readRoiSet(result.rois, rois, why));
  CHECK(rois.info->data["source"] == "config");
  CHECK(rois.info->data["cropStatus"] == output.info->data["crop"]);
  CHECK(rois.info->data["overallMm"][0].get<double>() == doctest::Approx(-15));
  for (const auto& record : rois.contexts)
    for (const auto& source : record->asRecord()->data["sources"])
      CHECK(source["descriptor"]["frame"] == "measurement-frame");
  CHECK(profile_contract::context(*original.sensorPrimary) ==
        profile_contract::context(*input.asBundle()->field("sensorPrimary")));
}
TEST_CASE(
    "Locate template slot contract validates enabled geometry and binds independent defaults without Registry") {
  Fixture fixture;
  auto settings = fixture.options();
  auto invalid = settings;
  invalid.slots[1].enabled = true;
  const auto errors = native::validateLocateOptions(invalid);
  REQUIRE_FALSE(errors.empty());
  CHECK(errors.front().status.paramPath == "template2DatumRoi");
  invalid = settings;
  std::swap(invalid.slots[3].alignment.roisMm[0], invalid.slots[3].alignment.roisMm[2]);
  const auto sides = native::validateLocateOptions(invalid);
  REQUIRE_FALSE(sides.empty());
  CHECK(sides.back().status.paramPath == "template4DatumRoi");
  auto values = defaults();
  values["templateDir"] = Value::text(fixture.root.u8string());
  values["minScore"] = Value::integer(20);
  values["template4Enabled"] = Value::boolean(true);
  for (const auto i : {0, 3}) {
    const auto prefix = "template" + std::to_string(i + 1);
    const auto& slot = settings.slots[i];
    values[prefix + "Id"] = Value::text(slot.alignment.templateId);
    values[prefix + "RoisFrom"] = Value::text(slot.alignment.roisFrom);
    constexpr std::array<std::size_t, 4> mapping{0, 2, 1, 3};
    const char* roles[4] = {"Datum", "Target", "SeamLeft", "SeamRight"};
    for (std::size_t role = 0; role < 4; ++role) {
      std::vector<double> bounds;
      for (const auto v : slot.alignment.roisMm[mapping[role]])
        bounds.push_back(v);
      values[prefix + roles[role] + "Roi"] = Value::vec(bounds);
    }
  }
  std::filesystem::path base;
  const auto parsed = native::locateOptions(ParamView(values, base));
  CHECK(native::validateLocateOptions(parsed).empty());
  CHECK(parsed.slots[0].enabled);
  CHECK_FALSE(parsed.slots[1].enabled);
  CHECK(parsed.slots[3].enabled);
  CHECK(parsed.slots[0].load.layout == "profile");
  CHECK(parsed.slots[0].load.handedness == "reflected");
  CHECK(parsed.slots[0].alignment.roisMm == settings.slots[0].alignment.roisMm);
  CHECK(parsed.slots[0].alignment.roisFrom == "config");
  const auto key = native::locateFingerprint(parsed);
  CHECK(std::count(key.begin(), key.end(), ';') == 2);
  {
    std::ofstream disabled(fixture.root / "f2_left.pcd");
    disabled << "ignored";
  }
  CHECK(native::locateFingerprint(parsed) == key);
  {
    std::ofstream enabled(fixture.root / "f4_left.pcd", std::ios::app);
    enabled << "\n";
  }
  CHECK(native::locateFingerprint(parsed) != key);
  Registry registry;
  native::registerProfileLocateTemplate(registry);
  const auto* op = registry.find("gap.locate_template");
  REQUIRE(op);
  CHECK(op->version == "3.0.0");
  CHECK(op->inputs[0].type == "Bundle<gap.ScanPair.v2>");
  CHECK(op->outputs[2].type == "Bundle<gap.Alignment.v2>");
  CHECK(op->params.size() == values.size());
}
TEST_CASE("Locate template auto center and failure cancellation keep outputs atomic") {
  Fixture fixture;
  const auto input = scan(false);
  auto settings = fixture.options();
  settings.overall = {{-15, -10, 15, 10}, "auto_center", "Both"};
  Context context;
  native::LocateResult result;
  const auto status = native::locate(input, settings, result, context);
  REQUIRE_MESSAGE(status.ok, status.message);
  scene::ScanView view;
  std::string why;
  REQUIRE(scene::readScanPair(result.scan, view, why));
  CHECK(view.info->data["processing"]["details"]["overall"]["centerApplied"] == true);
  const auto before = result.rois.asBundle()->field("info")->asRecord()->data;
  Context stopped;
  stopped.stopAtIcp = true;
  const auto cancellation = native::locate(input, settings, result, stopped);
  CHECK_FALSE(cancellation.ok);
  CHECK(cancellation.code == "cancelled");
  CHECK(result.rois.asBundle()->field("info")->asRecord()->data == before);
  settings.slots[3].load.right = "missing.pcd";
  CHECK_FALSE(native::locate(input, settings, result, context).ok);
  CHECK(result.rois.asBundle()->field("info")->asRecord()->data == before);
  settings = fixture.options();
  settings.overall.roiMm = {100, 100, 110, 110};
  const auto empty = native::locate(input, settings, result, context);
  CHECK_FALSE(empty.ok);
  CHECK(result.rois.asBundle()->field("info")->asRecord()->data == before);
}
