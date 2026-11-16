#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <limits>

#include "helpers.h"
#include "lyflow_profile/merge.h"
#include "lyflow_profile/selection.h"
#include "ops/profile_model_ops.h"

using namespace lyflow;
namespace native = lyflow::packs::gap::profile_model;
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
using Json = nlohmann::json;
const Registry& registry() {
  static const Registry result = [] {
    Registry value;
    scene::registerProfileBundles(value);
    native::registerProfileModelOps(value);
    return value;
  }();
  return result;
}
Data sensor(const std::string& id) {
  PointCloud cloud;
  std::vector<std::uint8_t> validity(1280, 1);
  std::vector<std::uint32_t> indices;
  for (int i = 0; i < 1280; ++i) {
    cloud.push((i - 640) * .000125F, 0, .1F + (i % 17) * .000125F);
    cloud.intensity.push_back(9000.F + i);
    cloud.normals.insert(cloud.normals.end(), {0, 1, 0});
    cloud.rgb.insert(cloud.rgb.end(), {static_cast<std::uint8_t>(i % 251), 220, 240});
    indices.push_back(i);
  }
  validity[5] = validity[7] = 0;
  cloud.xyz[15] = cloud.xyz[16] = cloud.xyz[17] = std::numeric_limits<float>::quiet_NaN();
  Profile result;
  std::string why;
  REQUIRE(makeProfile(std::make_shared<const PointCloud>(std::move(cloud)),
                      {1, "sensor-frame", "m", "XZ", "right", "acquisition", id, 1280},
                      std::move(validity), std::move(indices), result, why));
  return Data::profile(std::move(result));
}
Data measured(const Data& source) {
  Profile result;
  std::string why;
  REQUIRE(
      remapProfileAxes(*source.asProfile(), "XZY", {1, 1, 1}, "measurement-frame", result, why));
  return Data::profile(std::move(result));
}
Data scan(const Data& primary, const Data& secondary) {
  const Data p = measured(primary), s = measured(secondary);
  profile_contract::SelectionResult finite[2];
  LocalContext ctx;
  std::string why;
  int role = 0;
  for (const auto* data : {&p, &s}) {
    Indices indices{data->asProfile()->cloud->id, {}};
    for (std::size_t i = 0; i < 1280; ++i)
      if (data->asProfile()->validity[i])
        indices.values.push_back(static_cast<std::int32_t>(i));
    profile_contract::SelectionOptions options;
    options.emptyPolicy = "allow";
    REQUIRE(profile_contract::selectProfileIndices(*data, indices, nullptr, options, finite[role++],
                                                   ctx)
                .ok);
  }
  profile_contract::MergeResult merged;
  REQUIRE(profile_contract::mergeProfiles(finite[1].profile, finite[0].profile,
                                          &finite[1].provenance, &finite[0].provenance,
                                          {"secondary", "primary", "intersection"}, merged, why));
  Json origins = Json::object();
  for (const auto& entry :
       {std::make_pair("primary", &primary), std::make_pair("secondary", &secondary)})
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
      {{"method", "read"}, {"status", "complete"}, {"details", {{"mergedPrefiltered", true}}}}};
  Data result;
  REQUIRE_MESSAGE(scene::makeScanPair(primary, secondary, p, s, merged.profile, merged.provenance,
                                      metadata, result, why),
                  why);
  return result;
}
Tensor labelTensor(bool missing = false) {
  Tensor labels;
  labels.shape = {2, 1280};
  labels.data.assign(2560, 0);
  if (!missing)
    for (int row = 0; row < 2; ++row)
      for (int segment = 0; segment < 4; ++segment)
        for (int i = 100 + segment * 200; i < 120 + segment * 200; ++i)
          labels.data[row * 1280 + i] = static_cast<float>(std::array<int, 4>{2, 3, 5, 6}[segment]);
  return labels;
}
Tensor logits(bool missing = false) {
  const auto labels = labelTensor(missing);
  Tensor result;
  result.shape = {2, 8, 1280};
  result.data.assign(20480, 0);
  for (int row = 0; row < 2; ++row)
    for (int i = 0; i < 1280; ++i)
      result.data[row * 10240 + static_cast<int>(labels.data[row * 1280 + i]) * 1280 + i] = 1;
  return result;
}
Data labels(const Data& primary, const Data& secondary, bool missing = false) {
  LocalContext ctx;
  Data result;
  REQUIRE(native::makeLabelRows(labelTensor(missing), 8, primary, secondary, result, ctx).ok);
  return result;
}
struct Stop : LocalContext {
  bool cancelled() const override { return true; }
};
#ifdef LYFLOW_GAP_MODEL
std::string varint(std::uint64_t value) {
  std::string result;
  while (value > 127) {
    result.push_back(static_cast<char>((value & 127) | 128));
    value >>= 7;
  }
  result.push_back(static_cast<char>(value));
  return result;
}
std::string integerField(unsigned number, std::uint64_t value) {
  return varint(number << 3) + varint(value);
}
std::string bytesField(unsigned number, const std::string& value) {
  return varint((number << 3) | 2) + varint(value.size()) + value;
}
std::string info(const std::string& name, std::initializer_list<int> dimensions) {
  std::string shape;
  for (auto d : dimensions)
    shape += bytesField(1, integerField(1, d));
  return bytesField(1, name) +
         bytesField(2, bytesField(1, integerField(1, 1) + bytesField(2, shape)));
}
class ModelFiles {
public:
  std::filesystem::path root;
  ModelFiles() {
    static std::atomic<unsigned> count{0};
    root = std::filesystem::temp_directory_path() / "lyflow-model-native-ops" /
           (std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
            std::to_string(count++));
    std::filesystem::create_directories(root);
  }
  ~ModelFiles() {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }
  std::filesystem::path write(bool missing = false) {
    const auto scores = logits(missing);
    std::string tensor;
    for (auto d : scores.shape)
      tensor += integerField(1, d);
    tensor += integerField(2, 1) + bytesField(8, "constant") +
              bytesField(9, std::string(reinterpret_cast<const char*>(scores.data.data()),
                                        scores.data.size() * sizeof(float)));
    const auto node =
        bytesField(1, "constant") + bytesField(2, "output") + bytesField(4, "Identity");
    const auto graph = bytesField(1, node) + bytesField(2, "lyflow_synthetic_model_ops") +
                       bytesField(5, tensor) + bytesField(11, info("input", {2, 6, 1280})) +
                       bytesField(12, info("output", {2, 8, 1280}));
    const auto model = integerField(1, 8) + bytesField(2, "lyflow-synthetic-native-contract") +
                       bytesField(7, graph) + bytesField(8, integerField(2, 13));
    const auto path = root / (missing ? "missing-segments.onnx" : "segmented.onnx");
    std::ofstream output(path, std::ios::binary);
    output.write(model.data(), model.size());
    output.close();
    REQUIRE(output.good());
    return path;
  }
};
#endif
}

TEST_CASE(
    "Native model adapter registry exposes all tensor labels and ROI operations without hidden ML dependency") {
  for (const auto* id : {"gap.profile_tensor", "gap.labels_from_logits", "gap.roi_from_labels",
                         "gap.labels_to_cloud"}) {
    REQUIRE(registry().find(id));
    CHECK(registry().find(id)->version == "2.0.0");
  }
#ifdef LYFLOW_GAP_MODEL
  CHECK(registry().find("gap.locate_model") != nullptr);
  REQUIRE(registry().find("gap.infer_profile"));
  CHECK(registry().find("gap.infer_profile")->version == "1.0.0");
#else
  CHECK(registry().find("gap.locate_model") == nullptr);
  CHECK(registry().find("gap.infer_profile") == nullptr);
#endif
  const auto a = sensor("A"), b = sensor("B");
  test::OpCall call(registry());
  call.inputs = {{"primary", a}, {"secondary", b}};
  REQUIRE(call.run("gap.profile_tensor").ok);
  const auto tensor = call.out("tensor");
  CHECK(tensor.asTensor()->shape == std::vector<std::int64_t>{2, 6, 1280});
  call.inputs = {
      {"primary", a}, {"secondary", b}, {"tensor", Data::tensor(logits())}, {"modelInput", tensor}};
  REQUIRE(call.run("gap.labels_from_logits").ok);
  native::LabelRowsView view;
  std::string why;
  REQUIRE(native::readLabelRows(call.out("labels"), view, why));
  CHECK(view.labels->shape == std::vector<std::int64_t>{2, 1280});
  CHECK(view.info->data["association"] == "declared");
  CHECK(view.info->data.dump().size() < 4096);
  auto incorrect = *tensor.asTensor();
  incorrect.data[0] += 1;
  call.inputs["modelInput"] = Data::tensor(std::move(incorrect));
  CHECK_FALSE(call.run("gap.labels_from_logits").ok);
  CHECK(call.outputs.empty());
}

TEST_CASE(
    "Native model ROI callbacks preserve formal source contexts and missing segment diagnostic backdrop") {
  const auto a = sensor("A"), b = sensor("B"), input = scan(a, b);
  test::OpCall call(registry());
  const auto backdrop = Data::cloud(*measured(a).asProfile()->cloud);
  call.inputs = {{"scan", input}, {"labels", labels(a, b)}, {"backdrop", backdrop}};
  REQUIRE(call.run("gap.roi_from_labels", {{"refine", Value::boolean(false)}}).ok);
  CHECK(call.out("backdrop").cloudPtr() == backdrop.cloudPtr());
  scene::RoiView view;
  std::string why;
  REQUIRE(scene::readRoiSet(call.out("rois"), view, why));
  CHECK(call.out("gapLeft").valueJson() == view.boxes[2]->valueJson());
  CHECK(call.out("flushRef").valueJson() == view.boxes[1]->valueJson());
  const auto* context = call.out("gapLeftContext").asRecord();
  for (const auto& source : context->data["sources"])
    CHECK(source["descriptor"]["plane"] == "XY");
  call.inputs["labels"] = labels(a, b, true);
  CHECK_FALSE(call.run("gap.roi_from_labels").ok);
  CHECK(call.status.code == "model_roi_failed");
  CHECK(call.outputs.count("rois") == 0);
  CHECK(call.outputs.count("gapLeft") == 0);
  CHECK(call.out("backdrop").cloudPtr() == backdrop.cloudPtr());
  CHECK_FALSE(call.out("refinements").asRecord()->data["missingSegments"].empty());
  native::ModelRegionResult typed;
  typed.rois = Data::record({"sentinel", {}});
  LocalContext ctx;
  CHECK_FALSE(native::extractModelRegions(labels(a, b, true), input,
                                          ::gap::ml::MaskRefineOptions::contractExact(), typed, ctx)
                  .ok);
  CHECK(typed.rois.asRecord()->type == "sentinel");
}

TEST_CASE(
    "Native label visualization keeps legacy eight colors and rejects stale or compacted model slots") {
  const auto a = sensor("A"), b = sensor("B"), input = scan(a, b), labelData = labels(a, b);
  LocalContext ctx;
  Data output;
  const auto before = profile_contract::context(a);
  REQUIRE(native::labelsToCloud(input, labelData, "primary", output, ctx).ok);
  REQUIRE(output.asCloud());
  CHECK(output.asProfile() == nullptr);
  CHECK(output.asCloud()->pointCount() == 1280);
  CHECK(output.asCloud()->rgb[300] == 60);
  CHECK(output.asCloud()->rgb[301] == 170);
  CHECK(output.asCloud()->rgb[302] == 255);
  CHECK(output.asCloud()->intensity[100] == 2);
  CHECK(output.asCloud()->intensity[300] == 3);
  CHECK(output.asCloud()->normals.size() == 3840);
  CHECK(profile_contract::context(a) == before);
  const auto previous = output.cloudPtr();
  CHECK_FALSE(
      native::labelsToCloud(input, labels(sensor("different"), b), "primary", output, ctx).ok);
  CHECK(output.cloudPtr() == previous);
  CHECK_FALSE(native::labelsToCloud(input, labelData, "unknown", output, ctx).ok);
  Stop stop;
  CHECK_FALSE(native::labelsToCloud(input, labelData, "primary", output, stop).ok);
  CHECK(output.cloudPtr() == previous);
  test::OpCall call(registry());
  call.inputs = {{"scan", input}, {"labels", labelData}};
  REQUIRE(call.run("gap.labels_to_cloud").ok);
  CHECK(call.out("cloud").asCloud()->rgb == previous->rgb);
}

#ifdef LYFLOW_GAP_MODEL
TEST_CASE(
    "Native model location runs real ONNX inference and retains finite pre roll behavior for every crop outcome") {
  ModelFiles files;
  const auto model = files.write(), missing = files.write(true);
  const auto a = sensor("A"), b = sensor("B"), input = scan(a, b);
  LocalContext ctx;
  Tensor features, inferred;
  REQUIRE(native::buildTensor(a, b, features, ctx).ok);
  REQUIRE(native::inferProfileTensor(features, model, inferred, ctx).ok);
  CHECK(inferred.shape == std::vector<std::int64_t>{2, 8, 1280});
  CHECK(inferred.data == logits().data);
  const auto previousLogits = inferred.data;
  auto invalidShape = features;
  invalidShape.shape = {1, 6, 1280};
  invalidShape.data.resize(7680);
  CHECK_FALSE(native::inferProfileTensor(invalidShape, model, inferred, ctx).ok);
  CHECK(inferred.data == previousLogits);
  auto invalidFeatures = features;
  invalidFeatures.data[0] = std::numeric_limits<float>::quiet_NaN();
  CHECK_FALSE(native::inferProfileTensor(invalidFeatures, model, inferred, ctx).ok);
  CHECK(inferred.data == previousLogits);
  test::OpCall fine(registry());
  fine.inputs = {{"input", Data::tensor(features)}};
  REQUIRE(fine.run("gap.infer_profile", {{"modelPath", Value::text(model.u8string())}}).ok);
  const auto actualOutput = fine.out("output");
  fine.inputs = {{"tensor", actualOutput},
                 {"primary", a},
                 {"secondary", b},
                 {"modelInput", Data::tensor(features)}};
  REQUIRE(fine.run("gap.labels_from_logits").ok);
  native::LabelRowsView fineLabels;
  std::string fineWhy;
  REQUIRE(native::readLabelRows(fine.out("labels"), fineLabels, fineWhy));
  CHECK(fineLabels.labels->data == labelTensor().data);
  CHECK(fineLabels.info->data["association"] == "declared");
  native::LocateModelOptions options;
  options.modelPath = model;
  options.refine = ::gap::ml::MaskRefineOptions::contractExact();
  options.crop.enabled = false;
  native::LocateModelResult result;
  REQUIRE_MESSAGE(native::locateModel(input, options, result, ctx).ok,
                  "real constant synthetic ONNX model");
  scene::ScanView view;
  std::string why;
  REQUIRE(scene::readScanPair(result.scan, view, why));
  CHECK(view.info->data["stage"] == "processed");
  CHECK(view.primary->asProfile()->pointCount() == 1278);
  CHECK(view.secondary->asProfile()->pointCount() == 1278);
  CHECK(view.sensorPrimary->asProfile()->pointCount() == 1280);
  CHECK(view.info->data["crop"]["status"] == "disabled");
  CHECK(view.info->data["processing"]["details"]["inferenceAssociation"] == "direct_infer_call");
  CHECK(profile_contract::fingerprint(view.info->data["processing"]["details"]["inputHash"]));
  native::LabelRowsView decoded;
  REQUIRE(native::readLabelRows(result.labels, decoded, why));
  CHECK(decoded.labels->shape == std::vector<std::int64_t>{2, 1280});
  options.crop.enabled = true;
  options.crop.minPointsKept = 100000;
  REQUIRE(native::locateModel(input, options, result, ctx).ok);
  REQUIRE(scene::readScanPair(result.scan, view, why));
  CHECK(view.primary->asProfile()->pointCount() == 1278);
  CHECK(view.info->data["crop"]["status"] == "reverted:min_points");
  options.crop.minPointsKept = 1;
  REQUIRE(native::locateModel(input, options, result, ctx).ok);
  REQUIRE(scene::readScanPair(result.scan, view, why));
  CHECK(view.info->data["crop"]["status"] == "applied");
  CHECK(view.primary->asProfile()->pointCount() < 1278);
  CHECK(view.info->data["processing"]["details"]["mergedRebuilt"] == false);
  const auto* previousScan = result.scan.asBundle();
  const auto* previousRois = result.rois.asBundle();
  options.modelPath = missing;
  CHECK_FALSE(native::locateModel(input, options, result, ctx).ok);
  CHECK(result.scan.asBundle() == previousScan);
  CHECK(result.rois.asBundle() == previousRois);
  CHECK_FALSE(result.refinements.data["missingSegments"].empty());
  options.modelPath = files.root / "absent.onnx";
  auto processed = result.scan;
  auto status = native::locateModel(processed, options, result, ctx);
  CHECK_FALSE(status.ok);
  CHECK(status.code == "bad_input");
  CHECK(result.scan.asBundle() == previousScan);
  test::OpCall call(registry());
  call.inputs = {{"scan", input}};
  REQUIRE(call.run("gap.locate_model", {{"modelPath", Value::text(model.u8string())},
                                        {"refine", Value::boolean(false)},
                                        {"cropEnabled", Value::boolean(false)}})
              .ok);
  CHECK(call.out("scan").asBundle() != nullptr);
  CHECK(call.out("rois").asBundle() != nullptr);
}
#endif
