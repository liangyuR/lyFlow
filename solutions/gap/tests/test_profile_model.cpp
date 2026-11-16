#include <doctest/doctest.h>

#include <cmath>
#include <limits>

#include "gap_ml/RoiFeatures.hpp"
#include "lyflow/profile.h"
#include "lyflow_profile/provenance.h"
#include "ops/profile_model.h"

using namespace lyflow;
namespace model = ::gap::ml;
namespace native = lyflow::packs::gap::profile_model;
namespace scene = lyflow::packs::gap::profile_scene;

namespace {
using Json = nlohmann::json;
struct Context : ExecContext {
  mutable int polls = 0;
  int stopAt = std::numeric_limits<int>::max();
  std::filesystem::path base;
  std::vector<std::string> logs;
  bool cancelled() const override { return ++polls >= stopAt; }
  void progress(float, std::string_view) override {}
  void log(LogLevel, std::string text) override { logs.push_back(std::move(text)); }
  const std::filesystem::path& baseDir() const override { return base; }
  int threadBudget() const override { return 1; }
};
Data sensor(const std::string& source, const std::string& unit = "m", bool rgb = true) {
  PointCloud cloud;
  std::vector<std::uint8_t> validity(1280, 1);
  std::vector<std::uint32_t> indices;
  for (int i = 0; i < 1280; ++i) {
    cloud.push((i - 640) * .125F, 0, 100 + (i % 17) * .125F);
    cloud.intensity.push_back(9000 + static_cast<float>(i));
    if (rgb) {
      cloud.rgb.push_back(static_cast<std::uint8_t>(i % 251));
      cloud.rgb.push_back(220);
      cloud.rgb.push_back(240);
    }
    indices.push_back(i);
  }
  validity[5] = validity[7] = 0;
  cloud.xyz[15] = cloud.xyz[16] = cloud.xyz[17] = std::numeric_limits<float>::quiet_NaN();
  cloud.xyz[21] = cloud.xyz[23] = 9999;
  Profile result;
  std::string why;
  REQUIRE(makeProfile(std::make_shared<const PointCloud>(std::move(cloud)),
                      {1, "sensor-frame", "mm", "XZ", "right", "acquisition", source, 1280},
                      std::move(validity), std::move(indices), result, why));
  if (unit == "m") {
    Profile metric;
    REQUIRE(convertProfileUnit(result, "m", metric, why));
    result = std::move(metric);
  }
  return Data::profile(std::move(result));
}
Data measurement(const Data& sensor) {
  Profile axes, metric;
  std::string why;
  REQUIRE(remapProfileAxes(*sensor.asProfile(), "XZY", {1, 1, 1}, "measurement-frame", axes, why));
  REQUIRE(convertProfileUnit(axes, "m", metric, why));
  return Data::profile(std::move(metric));
}
Data scan(const Data& a, const Data& b, bool processed = false) {
  Data primary = measurement(a), secondary = measurement(b);
  if (processed)
    for (auto* data : {&primary, &secondary}) {
      auto p = *data->asProfile();
      p.cloud = std::make_shared<const PointCloud>(p.cloud->select({0, 1, 2}));
      p.validity = {1, 1, 1};
      p.sourceIndices = {0, 1, 2};
      *data = Data::profile(std::move(p));
    }
  PointCloud cloud;
  Profile merged;
  for (const auto* data : {&secondary, &primary}) {
    const auto& p = *data->asProfile();
    const auto& c = *p.cloud;
    const auto offset = merged.descriptor.sourceCount;
    cloud.xyz.insert(cloud.xyz.end(), c.xyz.begin(), c.xyz.end());
    cloud.intensity.insert(cloud.intensity.end(), c.intensity.begin(), c.intensity.end());
    cloud.rgb.insert(cloud.rgb.end(), c.rgb.begin(), c.rgb.end());
    merged.validity.insert(merged.validity.end(), p.validity.begin(), p.validity.end());
    for (const auto index : p.sourceIndices)
      merged.sourceIndices.push_back(static_cast<std::uint32_t>(offset + index));
    merged.descriptor.sourceCount += p.descriptor.sourceCount;
  }
  merged.cloud = std::make_shared<const PointCloud>(std::move(cloud));
  Record provenance;
  std::string why;
  REQUIRE(profile_contract::makeCompositeProvenance(secondary, primary, nullptr, nullptr,
                                                    "secondary", "primary", "require_equal", merged,
                                                    provenance, why));
  Json origins = Json::object();
  for (const auto& item : {std::make_pair("primary", &a), std::make_pair("secondary", &b)})
    origins[item.first] = {{"kind", "injected_profile"},
                           {"sourceId", item.second->asProfile()->descriptor.sourceId},
                           {"details", Json::object()}};
  scene::ScanMetadata metadata{
      "sensor-frame",
      "measurement-frame",
      processed ? "processed" : "raw",
      {{"taken", "both"}, {"evaluated", false}, {"exceeded", false}, {"onDisagree", "off"}},
      nullptr,
      origins,
      {{"method", "read"}, {"status", "complete"}, {"details", Json::object()}}};
  Data result;
  REQUIRE_MESSAGE(scene::makeScanPair(a, b, primary, secondary, Data::profile(std::move(merged)),
                                      provenance, metadata, result, why),
                  why);
  return result;
}
Tensor labelTensor() {
  Tensor labels;
  labels.shape = {2, 1280};
  labels.data.assign(2560, 0);
  constexpr std::array<int, 4> classes{2, 3, 5, 6};
  for (int row = 0; row < 2; ++row)
    for (int segment = 0; segment < 4; ++segment)
      for (int i = 100 + segment * 200; i < 120 + segment * 200; ++i)
        labels.data[row * 1280 + i] = static_cast<float>(classes[segment]);
  return labels;
}
Data labelData(const Data& a, const Data& b) {
  Data labels;
  Context ctx;
  REQUIRE(native::makeLabelRows(labelTensor(), 8, a, b, labels, ctx).ok);
  return labels;
}
} // namespace

TEST_CASE("Native model features retain 1280 slots masks RGB R and exact m/mm conversion") {
  const auto mm = sensor("physical-A", "mm"), meters = sensor("physical-A");
  const auto before = profile_contract::context(mm);
  Tensor fromMm, fromM;
  Context ctx;
  REQUIRE(native::buildTensor(mm, mm, fromMm, ctx).ok);
  REQUIRE(native::buildTensor(meters, meters, fromM, ctx).ok);
  CHECK(fromMm.shape == std::vector<std::int64_t>{2, 6, 1280});
  CHECK(fromMm.data == fromM.data);
  model::ProfileRow expected;
  const auto& cloud = *meters.asProfile()->cloud;
  expected.valid = meters.asProfile()->validity;
  for (std::size_t i = 0; i < 1280; ++i) {
    expected.x_mm.push_back(static_cast<double>(cloud.xyz[i * 3]) * 1000);
    expected.z_mm.push_back(static_cast<double>(cloud.xyz[i * 3 + 2]) * 1000);
    expected.intensity.push_back(cloud.rgb[3 * i]);
  }
  const auto channels = model::buildChannels(expected);
  CHECK(std::equal(channels.begin(), channels.end(), fromM.data.begin()));
  CHECK(std::equal(channels.begin(), channels.end(), fromM.data.begin() + 6 * 1280));
  CHECK(fromMm.data[4 * 1280 + 10] == doctest::Approx(10. / 255.));
  for (int channel = 0; channel < 6; ++channel) {
    CHECK(fromMm.data[channel * 1280 + 5] == 0);
    CHECK(fromMm.data[channel * 1280 + 7] == 0);
  }
  CHECK(profile_contract::context(mm) == before);
  const auto noRgb = sensor("physical-B", "m", false);
  REQUIRE(native::buildTensor(noRgb, noRgb, fromM, ctx).ok);
  CHECK(fromM.data[4 * 1280 + 10] == 0);
  CHECK(noRgb.asProfile()->cloud->intensity[10] == 9010);
  CHECK_FALSE(ctx.logs.empty());
}

TEST_CASE("Native labels keep arrays in Tensor bind actual rows and preserve minimum argmax ties") {
  const auto a = sensor("same-actual-source");
  Context ctx;
  Tensor input;
  REQUIRE(native::buildTensor(a, a, input, ctx).ok);
  Tensor logits;
  logits.shape = {2, 8, 1280};
  logits.data.assign(2 * 8 * 1280, -1);
  logits.data[2 * 1280 + 10] = logits.data[3 * 1280 + 10] = 5;
  Data labels;
  REQUIRE(native::labelsFromLogits(logits, a, a, labels, ctx, &input).ok);
  native::LabelRowsView view;
  std::string why;
  REQUIRE(native::readLabelRows(labels, view, why));
  CHECK(view.labels->data.size() == 2560);
  CHECK(view.labels->data[10] == 2);
  CHECK(view.labels->data[11] == 0);
  CHECK(view.info->data["sensorContexts"][0] == view.info->data["sensorContexts"][1]);
  CHECK(view.info->data["logicalRows"] == Json::array({"primary", "secondary"}));
  CHECK(view.info->data["association"] == "declared");
  CHECK(view.info->data.dump().size() < 4096);
  CHECK(native::matchesSensors(labels, a, a, why));
  auto changed = *a.asProfile();
  auto cloud = *changed.cloud;
  cloud.rgb[0] += 1;
  changed.cloud = std::make_shared<const PointCloud>(std::move(cloud));
  CHECK_FALSE(native::matchesSensors(labels, Data::profile(changed), a, why));
  auto bundle = *labels.asBundle();
  auto altered = *view.labels;
  altered.data[10] = 1;
  bundle.set("labels", Data::tensor(std::move(altered)));
  native::LabelRowsView sentinel;
  sentinel.classCount = 99;
  CHECK_FALSE(native::readLabelRows(Data::bundle(std::move(bundle)), sentinel, why));
  CHECK(sentinel.classCount == 99);
  input.data[0] += 1;
  Data untouched = Data::record({"sentinel", {}});
  CHECK_FALSE(native::labelsFromLogits(logits, a, a, untouched, ctx, &input).ok);
  CHECK(untouched.asRecord()->type == "sentinel");
}

TEST_CASE("Native label model validation and cancellation do not publish partial payloads") {
  const auto a = sensor("A"), b = sensor("B");
  Context ctx;
  Data output = Data::record({"sentinel", {}});
  for (int classes : {0, 257})
    CHECK_FALSE(native::makeLabelRows(labelTensor(), classes, a, b, output, ctx).ok);
  for (float value : {-1.F, 8.F, 1.5F, std::numeric_limits<float>::quiet_NaN()}) {
    auto labels = labelTensor();
    labels.data[0] = value;
    CHECK_FALSE(native::makeLabelRows(labels, 8, a, b, output, ctx).ok);
  }
  auto source = *a.asProfile();
  source.descriptor.sourceCount = 1281;
  Tensor tensor;
  CHECK_FALSE(native::buildTensor(Data::profile(source), b, tensor, ctx).ok);
  source = *a.asProfile();
  source.descriptor.sampleOrder = "unknown";
  CHECK_FALSE(native::buildTensor(Data::profile(source), b, tensor, ctx).ok);
  source = *a.asProfile();
  source.descriptor.plane = "XY";
  CHECK_FALSE(native::buildTensor(Data::profile(source), b, tensor, ctx).ok);
  source = *a.asProfile();
  source.sourceIndices[10] = 11;
  CHECK_FALSE(native::buildTensor(Data::profile(source), b, tensor, ctx).ok);
  Tensor logits;
  logits.shape = {2, 0, 1280};
  CHECK_FALSE(native::labelsFromLogits(logits, a, b, output, ctx).ok);
  CHECK(output.asRecord()->type == "sentinel");
  Context stopped;
  stopped.stopAt = 1;
  tensor.shape = {1};
  tensor.data = {77};
  CHECK_FALSE(native::buildTensor(a, b, tensor, stopped).ok);
  CHECK(tensor.data == std::vector<float>{77});
  CHECK_FALSE(native::makeLabelRows(labelTensor(), 8, a, b, output, stopped).ok);
  CHECK(output.asRecord()->type == "sentinel");
}

TEST_CASE("Native model boxes reuse the exact ROI algorithm and bind full measurement parents") {
  const auto a = sensor("A", "mm"), b = sensor("B");
  const auto labels = labelData(a, b);
  Context ctx;
  native::BoxComputation output;
  const auto options = model::MaskRefineOptions::contractExact();
  const auto bundle = scan(a, b, true);
  REQUIRE_MESSAGE(native::boxesFromLabels(labels, bundle, options, output, ctx).ok,
                  "box computation");
  REQUIRE(output.complete);
  std::array<model::ProfileRow, 2> expectedRows;
  std::array<std::vector<int>, 2> expectedLabels;
  const auto nativeLabels = labelTensor();
  int row = 0;
  for (const auto* data : {&a, &b}) {
    Profile metric;
    std::string why;
    REQUIRE(convertProfileUnit(*data->asProfile(), "m", metric, why));
    expectedRows[row].valid = metric.validity;
    for (std::size_t slot = 0; slot < 1280; ++slot) {
      expectedRows[row].x_mm.push_back(static_cast<double>(metric.cloud->xyz[3 * slot]) * 1000);
      expectedRows[row].z_mm.push_back(static_cast<double>(metric.cloud->xyz[3 * slot + 2]) * 1000);
      expectedLabels[row].push_back(static_cast<int>(nativeLabels.data[row * 1280 + slot]));
    }
    ++row;
  }
  const auto exact = model::boxesFromLabels(
      {&expectedRows[0].x_mm, &expectedRows[0].z_mm, &expectedRows[0].valid, &expectedLabels[0]},
      {&expectedRows[1].x_mm, &expectedRows[1].z_mm, &expectedRows[1].valid, &expectedLabels[1]});
  const std::array<std::array<double, 4>, 4> exactBounds{
      exact.rois.flush_base.values, exact.rois.flush_ref.values, exact.rois.gap_left.values,
      exact.rois.gap_right.values};
  for (int index = 0; index < 4; ++index)
    for (int axis = 0; axis < 2; ++axis) {
      const auto* box = output.fields.boxes[index].asBox2D();
      REQUIRE(box);
      CHECK(box->min[axis] == static_cast<float>(exactBounds[index][axis]) / 1000.F);
      CHECK(box->max[axis] == static_cast<float>(exactBounds[index][axis + 2]) / 1000.F);
    }
  Data rois;
  std::string why;
  REQUIRE_MESSAGE(scene::makeRoiSet(output.fields, rois, why), why);
  const auto fullA = profile_contract::context(measurement(a));
  const auto fullB = profile_contract::context(measurement(b));
  for (int i = 0; i < 4; ++i) {
    ProfileDescriptor descriptor;
    REQUIRE(profile_contract::geometryMatchesPhysical(
        output.fields.boxes[i], output.fields.contexts[i].asRecord(), descriptor, why));
    CHECK(descriptor.frame == "measurement-frame");
    CHECK(descriptor.plane == "XY");
    const auto& sources = output.fields.contexts[i].asRecord()->data["sources"];
    CHECK(sources == Json::array({fullA, fullB}));
  }
  CHECK(output.fields.boxes[0].asBox2D()->min[0] < output.fields.boxes[2].asBox2D()->min[0]);
  CHECK(output.fields.boxes[2].asBox2D()->min[0] < output.fields.boxes[3].asBox2D()->min[0]);
  CHECK(output.fields.boxes[3].asBox2D()->min[0] < output.fields.boxes[1].asBox2D()->min[0]);
  CHECK(output.seam.asBox2D() != nullptr);
  CHECK(output.refinements.data["missingSegments"].empty());
  CHECK(output.fields.info.data["roisFrom"] == "model");
  CHECK(output.fields.info.data["overallMm"].is_null());
}

TEST_CASE(
    "Native model missing segments remain candidates while stale mapping and cancel are atomic") {
  const auto a = sensor("A"), b = sensor("B");
  const auto bundle = scan(a, b);
  Context ctx;
  auto empty = labelTensor();
  empty.data.assign(2560, 0);
  Data labels;
  REQUIRE(native::makeLabelRows(empty, 8, a, b, labels, ctx).ok);
  native::BoxComputation candidate;
  REQUIRE(native::boxesFromLabels(labels, bundle, {}, candidate, ctx).ok);
  CHECK_FALSE(candidate.complete);
  CHECK(candidate.missingSegments.size() == 4);
  CHECK(candidate.fields.info.type.empty());
  for (const auto& box : candidate.fields.boxes)
    CHECK(box.empty());
  auto partial = labelTensor();
  for (auto& value : partial.data)
    if (value == 2)
      value = 0;
  partial.data[7] = partial.data[1280 + 7] = 2;
  REQUIRE(native::makeLabelRows(partial, 8, a, b, labels, ctx).ok);
  REQUIRE(native::boxesFromLabels(labels, bundle, {}, candidate, ctx).ok);
  CHECK_FALSE(candidate.complete);
  CHECK(candidate.missingSegments == std::vector<std::string>{"left_surface"});
  CHECK(candidate.fields.boxes[0].empty());
  REQUIRE(candidate.fields.boxes[1].asBox2D());
  Data formal;
  std::string why;
  CHECK_FALSE(scene::makeRoiSet(candidate.fields, formal, why));
  CHECK(formal.empty());
  candidate.missingSegments = {"sentinel"};
  CHECK_FALSE(
      native::boxesFromLabels(labelData(sensor("different"), b), bundle, {}, candidate, ctx).ok);
  CHECK(candidate.missingSegments == std::vector<std::string>{"sentinel"});
  Context completed;
  const auto valid = labelData(a, b);
  native::BoxComputation baseline;
  REQUIRE(native::boxesFromLabels(valid, bundle, {}, baseline, completed).ok);
  Context stopped;
  stopped.stopAt = completed.polls;
  CHECK_FALSE(native::boxesFromLabels(valid, bundle, {}, candidate, stopped).ok);
  CHECK(candidate.missingSegments == std::vector<std::string>{"sentinel"});
}
