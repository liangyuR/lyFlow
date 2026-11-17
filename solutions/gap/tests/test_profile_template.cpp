#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>

#include <Eigen/Core>

#include "lyflow/profile.h"
#include "lyflow_profile/context.h"
#include "ops/profile_scan.h"
#include "ops/profile_template.h"

using namespace lyflow;
namespace native = lyflow::packs::gap::profile_template;
namespace scene = lyflow::packs::gap::profile_scene;
namespace {
using Json = nlohmann::json;
struct Context : ExecContext {
  mutable int polls = 0;
  int stopAt = std::numeric_limits<int>::max();
  bool stopOnLog = false;
  std::filesystem::path base;
  std::vector<std::string> logs;
  bool cancelled() const override { return ++polls >= stopAt; }
  void progress(float, std::string_view) override {}
  void log(LogLevel, std::string message) override {
    logs.push_back(std::move(message));
    if (stopOnLog)
      stopAt = polls + 1;
  }
  const std::filesystem::path& baseDir() const override { return base; }
  int threadBudget() const override { return 1; }
};
Data profile(const std::string& source, bool sensor = false, float dx = 0, float dy = 0) {
  PointCloud cloud;
  std::vector<std::uint8_t> validity;
  std::vector<std::uint32_t> indices;
  for (int i = 0; i < 160; ++i) {
    const float x = (i - 80) * .00025F + dx, y = .16F + .001F * std::sin(i * .07F) + dy;
    cloud.push(x, sensor ? 0 : y, sensor ? y : 0);
    cloud.intensity.push_back(8000 + i);
    cloud.normals.insert(cloud.normals.end(), sensor ? std::initializer_list<float>{3, 0, 4}
                                                     : std::initializer_list<float>{3, 4, 0});
    cloud.rgb.insert(cloud.rgb.end(), {10, 30, 250});
    validity.push_back(i == 7 || i == 9 ? 0 : 1);
    indices.push_back(static_cast<std::uint32_t>(i + 10));
  }
  cloud.xyz[21] = cloud.xyz[22] = cloud.xyz[23] = std::numeric_limits<float>::quiet_NaN();
  cloud.xyz[27] = cloud.xyz[28] = cloud.xyz[29] = 10000;
  Profile output;
  std::string why;
  REQUIRE_MESSAGE(
      makeProfile(std::make_shared<const PointCloud>(std::move(cloud)),
                  {1, sensor ? "sensor-frame" : "template-frame", "m", sensor ? "XZ" : "XY",
                   sensor ? "right" : "reflected", "acquisition", source, 180},
                  std::move(validity), std::move(indices), output, why),
      why);
  return Data::profile(std::move(output));
}
Json origins(const Data& left, const Data& right) {
  return {{"left",
           {{"kind", "injected_profile"},
            {"sourceId", left.asProfile()->descriptor.sourceId},
            {"details", {{"declaredBy", "test-host"}}}}},
          {"right",
           {{"kind", "injected_profile"},
            {"sourceId", right.asProfile()->descriptor.sourceId},
            {"details", {{"declaredBy", "test-host"}}}}}};
}
Data templates() {
  const auto left = profile("template-physical-left"), right = profile("template-physical-right");
  return native::makeTemplatePair(left, right, origins(left, right));
}
Data scan(float dx = .0015F, float dy = -.0007F) {
  Context context;
  scene::ReadScanOptions options;
  options.measurementFrame = "measurement-frame";
  Data result;
  const auto status =
      scene::buildReadScan(profile("sensor-A", true, dx, dy), profile("sensor-B", true, dx, dy),
                           options, result, context);
  REQUIRE_MESSAGE(status.ok, status.message);
  return result;
}
native::AlignmentOptions options() {
  native::AlignmentOptions result;
  result.minScore = 20;
  result.normalKnn = 8;
  result.maxIterations = 50;
  result.roisMm = {
      {{{-18, 158, -8, 162}}, {{-5, 158, -3, 162}}, {{8, 157, 18, 161}}, {{3, 157, 5, 161}}}};
  return result;
}
Data aligned(const Data& input, const Data& pair, native::AlignmentOptions settings = options()) {
  Data result;
  Context context;
  const auto status = native::align(input, pair, settings, result, context);
  REQUIRE_MESSAGE(status.ok, status.message);
  return result;
}
struct Temp {
  std::filesystem::path root;
  Temp() {
    root = std::filesystem::temp_directory_path() / std::filesystem::u8path(u8"lyflow 模板契约") /
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(root);
  }
  ~Temp() {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }
};
void writePcd(const std::filesystem::path& path, bool sensor, bool mm) {
  std::ofstream stream(path);
  stream
      << "VERSION 0.7\nFIELDS x y z intensity normal_x normal_y normal_z rgb\nSIZE 4 4 4 4 4 4 4 4\nTYPE F F F F F F F U\nCOUNT 1 1 1 1 1 1 1 1\nWIDTH 3\nHEIGHT 1\nPOINTS 3\nDATA ascii\n";
  const auto scale = mm ? 1000 : 1;
  stream << .001 * scale << ' ' << (sensor ? 0 : .16 * scale) << ' ' << (sensor ? .16 * scale : 0)
         << " 9000 3 " << (sensor ? 0 : 4) << ' ' << (sensor ? 4 : 0) << " 660510\n";
  stream << "nan nan nan 9100 0 0 0 1976370\n";
  stream << .002 * scale << ' ' << (sensor ? 0 : .161 * scale) << ' ' << (sensor ? .161 * scale : 0)
         << " 9200 0 0 0 2634300\n";
}
Json canonicalMatrix(float angle, float tx, float ty) {
  const float c = std::cos(angle), s = std::sin(angle);
  return {c, -s, tx, s, c, ty, 0, 0, 1};
}
Record roiRecord() {
  return {"GapAlignment",
          {{"left", {{"transform", canonicalMatrix(.3F, .001F, .002F)}}},
           {"right", {{"transform", canonicalMatrix(-.2F, -.003F, .001F)}}},
           {"rois",
            {{"flushBase", {8.125, 157.375, 18.625, 161.875}},
             {"gapLeft", {-5.375, 158.125, -3.125, 162.625}},
             {"flushRef", {-18.125, 158.375, -8.625, 162.875}},
             {"gapRight", {3.125, 157.875, 5.375, 161.625}}}}}};
}
}
TEST_CASE("Template PCD loading preserves masked slots all channels units and file layout") {
  Temp temporary;
  Context context;
  for (const bool sensor : {false, true})
    for (const bool mm : {false, true}) {
      writePcd(temporary.root / "left.pcd", sensor, mm);
      writePcd(temporary.root / "right.pcd", sensor, mm);
      native::TemplateLoadOptions opts;
      opts.dir = temporary.root;
      opts.left = "left.pcd";
      opts.right = "right.pcd";
      opts.layout = sensor ? "sensor" : "profile";
      opts.unit = mm ? "mm" : "m";
      opts.handedness = sensor ? "right" : "reflected";
      Data result;
      const auto status = native::loadTemplatePair(opts, result, context);
      REQUIRE_MESSAGE(status.ok, status.message);
      native::TemplateView view;
      std::string why;
      REQUIRE_MESSAGE(native::readTemplatePair(result, view, why), why);
      Bundle mixed = *result.asBundle();
      auto reflected = *view.right->asProfile();
      reflected.descriptor.handedness =
          reflected.descriptor.handedness == "right" ? "reflected" : "right";
      mixed.set("right", Data::profile(reflected));
      native::TemplateView rejected;
      CHECK_FALSE(native::readTemplatePair(Data::bundle(std::move(mixed)), rejected, why));
      const auto& p = *view.left->asProfile();
      CHECK(p.descriptor.unit == "m");
      CHECK(p.descriptor.plane == "XY");
      CHECK(p.descriptor.handedness == "reflected");
      CHECK(p.pointCount() == 3);
      CHECK(p.descriptor.sourceCount == 3);
      CHECK(p.sourceIndices == std::vector<std::uint32_t>{0, 1, 2});
      CHECK(p.validity == std::vector<std::uint8_t>{1, 0, 1});
      CHECK(p.cloud->xyz[0] == doctest::Approx(.001));
      CHECK(p.cloud->xyz[1] == doctest::Approx(.16));
      CHECK(p.cloud->normals == std::vector<float>{3, 4, 0, 0, 0, 0, 0, 0, 0});
      CHECK(p.cloud->intensity == std::vector<float>{9000, 9100, 9200});
      CHECK(p.cloud->rgb == std::vector<std::uint8_t>{10, 20, 30, 30, 40, 50, 40, 50, 60});
      CHECK(view.info->data["origins"]["left"]["details"]["fileLayout"] == opts.layout);
      CHECK_FALSE(native::templateFingerprint(opts).empty());
      const auto* before = view.left->asProfile();
      Context stopped;
      stopped.stopAt = 1;
      CHECK_FALSE(native::loadTemplatePair(opts, result, stopped).ok);
      CHECK(result.asBundle()->field("left")->asProfile() == before);
      opts.right = "missing.pcd";
      CHECK_FALSE(native::loadTemplatePair(opts, result, context).ok);
      CHECK(result.asBundle()->field("left")->asProfile() == before);
    }
}
TEST_CASE("Template alignment preserves original algorithm metadata and applies real poses") {
  const auto input = scan(), pair = templates();
  const auto output = aligned(input, pair);
  native::AlignmentView view;
  std::string why;
  REQUIRE_MESSAGE(native::readAlignment(output, view, why), why);
  CHECK(view.info->data["mirrored"] == true);
  CHECK(view.info->data["globalCoarse"] == true);
  CHECK(view.info->data["left"]["success"] == true);
  CHECK(view.info->data["left"]["score"].get<double>() >= 20);
  const auto& before = *pair.asBundle()->field("left")->asProfile();
  const auto& after = *view.alignedLeft->asProfile();
  CHECK(before.descriptor.frame == "template-frame");
  CHECK(after.descriptor.frame == "measurement-frame");
  CHECK(after.descriptor.sourceCount == 180);
  CHECK(after.sourceIndices == before.sourceIndices);
  CHECK(after.validity == before.validity);
  CHECK(after.cloud->intensity == before.cloud->intensity);
  CHECK(after.cloud->rgb == before.cloud->rgb);
  const auto& m = view.info->data["left"]["transform"];
  Eigen::Matrix3f pose;
  for (int i = 0; i < 9; ++i)
    pose(i / 3, i % 3) = m[i].get<float>();
  CHECK(std::abs(pose(0, 2)) > 1e-4F);
  for (const auto i : {0, 17, 159}) {
    const auto expected =
        pose * Eigen::Vector3f(before.cloud->xyz[i * 3], before.cloud->xyz[i * 3 + 1], 1);
    CHECK(after.cloud->xyz[i * 3] == expected.x());
    CHECK(after.cloud->xyz[i * 3 + 1] == expected.y());
    const Eigen::Vector2f normal = pose.block<2, 2>(0, 0) * Eigen::Vector2f(3, 4);
    CHECK(after.cloud->normals[i * 3] == normal.x());
    CHECK(after.cloud->normals[i * 3 + 1] == normal.y());
  }
  CHECK(std::memcmp(after.cloud->xyz.data() + 21, before.cloud->xyz.data() + 21,
                    3 * sizeof(float)) == 0);
  CHECK(after.cloud->xyz[27] == 10000);
  auto opts = options();
  opts.successGuide = true;
  opts.globalCoarse = false;
  opts.segRoi = false;
  opts.bidirection = true;
  opts.initialPoseMode = "pca";
  const auto guided = aligned(input, pair, opts);
  REQUIRE(native::readAlignment(guided, view, why));
  CHECK(view.info->data["globalCoarse"] == false);
  CHECK(view.info->data["mirrored"] == false);
  Context stopped;
  stopped.stopOnLog = true;
  Data sentinel = Data::record({"sentinel", {}});
  const auto status = native::align(input, pair, options(), sentinel, stopped);
  CHECK_FALSE(status.ok);
  CHECK(status.code == "cancelled");
  CHECK(sentinel.asRecord()->type == "sentinel");
}
TEST_CASE("Template selection uses original score ranking and ties") {
  const auto input = scan(), pair = templates();
  auto a = options(), b = options();
  a.templateId = "z";
  a.order = 2;
  b.templateId = "a";
  b.order = 1;
  auto va = aligned(input, pair, a), vb = aligned(input, pair, b);
  Context context;
  Data selected;
  REQUIRE(native::select({va, vb}, 20, selected, context).ok);
  CHECK(selected.asBundle()->field("info")->asRecord()->data["templateId"] == "a");
  a.order = 1;
  va = aligned(input, pair, a);
  REQUIRE(native::select({va, vb}, 20, selected, context).ok);
  CHECK(selected.asBundle()->field("info")->asRecord()->data["templateId"] == "a");
  auto failOptions = options();
  failOptions.minScore = 100;
  failOptions.maxMatchingDist = .0001;
  failOptions.maxFitnessDist = .00001;
  failOptions.globalCoarse = false;
  failOptions.initialPoseMode = "centroid";
  native::TemplateView templateView;
  std::string bindingWhy;
  REQUIRE(native::readTemplatePair(pair, templateView, bindingWhy));
  std::array<Data, 2> unmatched;
  for (std::size_t i = 0; i < 2; ++i) {
    auto p = *(i == 0 ? templateView.left : templateView.right)->asProfile();
    auto c = *p.cloud;
    c.xyz[120] += .05F;
    p.cloud = std::make_shared<const PointCloud>(std::move(c));
    unmatched[i] = Data::profile(std::move(p));
  }
  const auto unmatchedPair =
      native::makeTemplatePair(unmatched[0], unmatched[1], origins(unmatched[0], unmatched[1]));
  const auto failed = aligned(scan(.0015F, .001F), unmatchedPair, failOptions);
  native::AlignmentView failedView;
  std::string why;
  REQUIRE(native::readAlignment(failed, failedView, why));
  Data sentinel = Data::record({"sentinel", {}});
  const auto low = native::select({failed}, 100, sentinel, context);
  CHECK_FALSE(low.ok);
  CHECK(low.code == "icp_score_low");
  CHECK(sentinel.asRecord()->type == "sentinel");
  Context stopped;
  stopped.stopAt = 1;
  CHECK_FALSE(native::select({va, vb}, 20, selected, stopped).ok);
}
TEST_CASE(
    "Business ROI transform preserves legacy float center-only boxes and double seam arithmetic") {
  const auto record = roiRecord();
  std::array<Box2D, 4> boxes;
  Box2D seam;
  std::string resolved;
  const auto status = native::transformBusinessRois(record, "auto", boxes, seam, resolved);
  REQUIRE_MESSAGE(status.ok, status.message);
  CHECK(resolved == "right");
  for (std::size_t i = 0; i < 4; ++i) {
    const char* names[4] = {"flushBase", "gapLeft", "flushRef", "gapRight"};
    const char* sides[4] = {"right", "left", "left", "right"};
    const auto& roi = record.data["rois"][names[i]];
    const auto& values = record.data[sides[i]]["transform"];
    Eigen::Matrix3f pose;
    for (int j = 0; j < 9; ++j)
      pose(j / 3, j % 3) = values[j].get<float>();
    const float x0 = static_cast<float>(roi[0].get<double>()) / 1000.F,
                y0 = static_cast<float>(roi[1].get<double>()) / 1000.F,
                x1 = static_cast<float>(roi[2].get<double>()) / 1000.F,
                y1 = static_cast<float>(roi[3].get<double>()) / 1000.F;
    const Eigen::Vector3f c = pose * Eigen::Vector3f(.5F * (x0 + x1), .5F * (y0 + y1), 1);
    CHECK(boxes[i].min[0] == c.x() - .5F * (x1 - x0));
    CHECK(boxes[i].min[1] == c.y() - .5F * (y1 - y0));
    CHECK(boxes[i].max[0] == c.x() + .5F * (x1 - x0));
    CHECK(boxes[i].max[1] == c.y() + .5F * (y1 - y0));
  }
  const double lx = .5 * (static_cast<double>(boxes[1].min[0]) + boxes[1].max[0]),
               rx = .5 * (static_cast<double>(boxes[3].min[0]) + boxes[3].max[0]);
  CHECK(seam.min[0] == static_cast<float>(.5 * (lx + rx)));
  CHECK(seam.min[0] == seam.max[0]);
  const auto autoBase = boxes[0].min[0];
  REQUIRE(native::transformBusinessRois(record, "left", boxes, seam, resolved).ok);
  CHECK(resolved == "left");
  CHECK(boxes[0].min[0] != autoBase);
  auto precise = record;
  precise.data["rois"]["flushBase"] = {1.00000001, 0.0, 1.00000003, 1.0};
  precise.data["rois"]["gapLeft"] = {1.0, 0.0, 1.0, 1.0};
  precise.data["rois"]["gapRight"] = {1.0, 0.0, 1.0, 1.0};
  REQUIRE(native::transformBusinessRois(precise, "auto", boxes, seam, resolved).ok);
  CHECK(resolved == "right");
  auto bad = record;
  bad.data["rois"]["flushBase"][0] = std::numeric_limits<double>::infinity();
  const auto before = Data::box2d(boxes[0]).valueJson();
  CHECK_FALSE(native::transformBusinessRois(bad, "auto", boxes, seam, resolved).ok);
  CHECK(Data::box2d(boxes[0]).valueJson() == before);
}
TEST_CASE(
    "Native business ROI boxes stay in the scan measurement space and retain ROI origin semantics") {
  const auto input = scan(), pair = templates();
  auto opts = options();
  opts.roisFrom = "config";
  const auto alignment = aligned(input, pair, opts);
  Context context;
  scene::RoiFields fields;
  Data seam, seamContext;
  const auto status = native::businessRois(alignment, "auto", fields, seam, seamContext, context);
  REQUIRE_MESSAGE(status.ok, status.message);
  Data output;
  std::string why;
  REQUIRE_MESSAGE(scene::makeRoiSet(fields, output, why), why);
  CHECK(fields.info.data["source"] == "config");
  CHECK(fields.info.data["roisFrom"] == "config");
  CHECK(fields.info.data["datumSide"] == "left");
  const auto& expected = input.asBundle()->field("merged")->asProfile()->descriptor;
  for (std::size_t i = 0; i < 4; ++i) {
    CHECK(profile_contract::geometryInSpace(fields.boxes[i], fields.contexts[i].asRecord(),
                                            expected, why));
    CHECK(expected.frame == "measurement-frame");
  }
  CHECK(profile_contract::geometryInSpace(seam, seamContext.asRecord(), expected, why));
  const auto before = fields.info.data;
  Context stopped;
  stopped.stopAt = 1;
  CHECK_FALSE(native::businessRois(alignment, "right", fields, seam, seamContext, stopped).ok);
  CHECK(fields.info.data == before);
  Registry registry;
  scene::registerProfileBundles(registry);
  native::registerProfileTemplateOps(registry);
  CHECK(registry.operators().size() == 4);
  for (const auto& op : registry.operators())
    CHECK(op.version == "3.0.0");
  CHECK(registry.find("gap.align_template")->inputs[0].type == "Bundle<gap.ScanPair.v3>");
  CHECK(registry.find("gap.business_rois")->outputs.size() == 11);
}
