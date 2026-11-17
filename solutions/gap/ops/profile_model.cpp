#include "profile_model.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "gap_ml/RoiFeatures.hpp"
#include "lyflow/profile.h"
#include "lyflow_profile/context.h"

namespace lyflow::packs::gap::profile_model {
namespace {
using Json = nlohmann::json;
namespace model = ::gap::ml;
constexpr std::size_t slots = model::kProfileSlots;
Status bad(const std::string& message, const char* port = "labels",
           const char* code = "bad_input") {
  return Status::Error(Phase::Execute, code, message, {}, port);
}
Status cancelled() { return bad("模型轮廓处理已取消", "", "cancelled"); }
bool fail(std::string& why, const char* message) {
  why = message;
  return false;
}
bool rawDescriptor(const ProfileDescriptor& d) {
  return d.plane == "XZ" && d.sampleOrder == "acquisition" && d.sourceCount == slots &&
         d.sourceId.rfind("derived:composite/", 0) != 0;
}
bool sensor(const Data& data, std::string& why) {
  const auto* p = data.asProfile();
  if (!p || p->pointCount() != slots || !rawDescriptor(p->descriptor))
    return fail(why, "模型输入必须为完整1280原槽、acquisition、XZ的单源Profile");
  for (std::size_t i = 0; i < slots; ++i)
    if (p->sourceIndices[i] != i)
      return fail(why, "模型输入源索引必须与1280个原始槽逐项相同");
  return true;
}
bool sensors(const Data& a, const Data& b, std::string& why) {
  return sensor(a, why) && sensor(b, why) &&
         (profile_contract::sameSpace(a.asProfile()->descriptor, b.asProfile()->descriptor) ||
          fail(why, "两个逻辑模型row必须处于相同传感器坐标空间"));
}
bool labelsValid(const Tensor& labels, int classes, std::string& why) {
  if (classes < 1 || classes > 256 || labels.shape != std::vector<std::int64_t>{2, slots} ||
      !labels.consistent())
    return fail(why, "标签需要Tensor[2,1280]及1到256个类别");
  for (const auto value : labels.data)
    if (!std::isfinite(value) || value < 0 || value >= classes || std::floor(value) != value)
      return fail(why, "每个标签必须为类别范围内的有限整数");
  return true;
}
Status rows(const Data& a, const Data& b, std::array<model::ProfileRow, 2>& result,
            ExecContext& ctx) {
  std::string why;
  if (!sensor(a, why))
    return bad(why, "sensorPrimary");
  if (!sensor(b, why))
    return bad(why, "sensorSecondary");
  if (!sensors(a, b, why))
    return bad(why, "sensorSecondary");
  std::size_t rowIndex = 0;
  for (const auto* data : {&a, &b}) {
    Profile metric;
    if (!convertProfileUnit(*data->asProfile(), "m", metric, why,
                            [&ctx] { return ctx.cancelled(); }))
      return ctx.cancelled() ? cancelled() : bad(why);
    auto& row = result[rowIndex++];
    row.x_mm.reserve(slots);
    row.z_mm.reserve(slots);
    row.intensity.reserve(slots);
    row.valid = metric.validity;
    for (std::size_t i = 0; i < slots; ++i) {
      if ((i & 255U) == 0 && ctx.cancelled())
        return cancelled();
      row.x_mm.push_back(static_cast<double>(metric.cloud->xyz[3 * i]) * 1000.0);
      row.z_mm.push_back(static_cast<double>(metric.cloud->xyz[3 * i + 2]) * 1000.0);
      row.intensity.push_back(metric.cloud->hasRgb() ? metric.cloud->rgb[3 * i] : 0);
    }
  }
  return Status::Ok();
}
Status pack(const Tensor& labels, int classes, const Data& a, const Data& b, Data& result,
            ExecContext& ctx) {
  std::string why;
  if (ctx.cancelled())
    return cancelled();
  if (!sensors(a, b, why) || !labelsValid(labels, classes, why))
    return bad(why);
  Bundle bundle(kLabelRowsKind);
  bundle.set("labels", Data::tensor(labels))
      .set("info", Data::record({kLabelInfoType, {{"schemaVersion", 3}, {"classCount", classes}}}));
  result = Data::bundle(std::move(bundle));
  return Status::Ok();
}
Json finiteOrNull(double value) { return std::isfinite(value) ? Json(value) : Json(); }
} // namespace

Status buildTensor(const Data& a, const Data& b, Tensor& result, ExecContext& ctx) {
  if (ctx.cancelled())
    return cancelled();
  std::array<model::ProfileRow, 2> source;
  const auto status = rows(a, b, source, ctx);
  if (!status.ok)
    return status;
  Tensor candidate;
  candidate.shape = {2, model::kNumChannels, slots};
  candidate.data.reserve(candidate.elementCount());
  try {
    for (const auto& row : source) {
      if (ctx.cancelled())
        return cancelled();
      const auto channels = model::buildChannels(row);
      candidate.data.insert(candidate.data.end(), channels.begin(), channels.end());
    }
  } catch (const std::exception& error) {
    return bad(std::string("通道构造失败: ") + error.what(), "sensorPrimary");
  }
  if (ctx.cancelled())
    return cancelled();
  for (const auto* data : {&a, &b})
    if (!data->asProfile()->cloud->hasRgb())
      ctx.log(LogLevel::Warn,
              "模型输入缺少RGB：按原约定将rgb.R强度设为0，不读取或修改intensity通道");
  ctx.log(LogLevel::Info, "profile tensor " + candidate.shapeString());
  if (ctx.cancelled())
    return cancelled();
  result = std::move(candidate);
  return Status::Ok();
}
Status makeLabelRows(const Tensor& labels, int classes, const Data& a, const Data& b, Data& result,
                     ExecContext& ctx) {
  return pack(labels, classes, a, b, result, ctx);
}
Status labelsFromLogits(const Tensor& logits, const Data& a, const Data& b, Data& result,
                        ExecContext& ctx) {
  if (ctx.cancelled())
    return cancelled();
  if (logits.shape.size() != 3 || logits.shape[0] != 2 || logits.shape[1] < 1 ||
      logits.shape[1] > 256 || logits.shape[2] != slots || !logits.consistent())
    return bad("模型输出需要Tensor[2,1..256,1280]", "tensor");
  const auto classes = static_cast<int>(logits.shape[1]);
  Tensor labels;
  labels.shape = {2, slots};
  labels.data.resize(2 * slots);
  for (std::size_t row = 0; row < 2; ++row)
    for (std::size_t slot = 0; slot < slots; ++slot) {
      if ((slot & 255U) == 0 && ctx.cancelled())
        return cancelled();
      const auto base = row * classes * slots;
      int best = 0;
      float value = logits.data[base + slot];
      for (int cls = 1; cls < classes; ++cls)
        if (logits.data[base + cls * slots + slot] > value) {
          best = cls;
          value = logits.data[base + cls * slots + slot];
        }
      labels.data[row * slots + slot] = static_cast<float>(best);
    }
  return pack(labels, classes, a, b, result, ctx);
}
bool readLabelRows(const Data& data, LabelRowsView& result, std::string& why) {
  const auto* bundle = data.asBundle();
  if (!bundle || bundle->kind != kLabelRowsKind)
    return fail(why, "需要gap.LabelRows.v3");
  const auto* record = bundle->field("info")->asRecord();
  result = {bundle->field("labels")->asTensor(), record, record->data.at("classCount").get<int>()};
  why.clear();
  return true;
}

Status boxesFromLabels(const Data& labelData, const Data& scanData,
                       const model::MaskRefineOptions& options, BoxComputation& result,
                       ExecContext& ctx) {
  if (ctx.cancelled())
    return cancelled();
  profile_scene::ScanView scan;
  std::string why;
  if (!profile_scene::readScanPair(scanData, scan, why))
    return bad(why, "scan");
  std::array<model::ProfileRow, 2> raw;
  auto status = rows(*scan.sensorPrimary, *scan.sensorSecondary, raw, ctx);
  if (!status.ok)
    return status;
  LabelRowsView labels;
  if (!readLabelRows(labelData, labels, why))
    return bad(why);
  std::array<std::vector<int>, 2> classes;
  for (std::size_t row = 0; row < 2; ++row)
    for (std::size_t i = 0; i < slots; ++i)
      classes[row].push_back(static_cast<int>(labels.labels->data[row * slots + i]));
  model::RoiBoxesResult boxes;
  try {
    boxes = model::boxesFromRefinedLabels({&raw[0].x_mm, &raw[0].z_mm, &raw[0].valid, &classes[0]},
                                          {&raw[1].x_mm, &raw[1].z_mm, &raw[1].valid, &classes[1]},
                                          options);
  } catch (const std::exception& error) {
    return bad(std::string("推框失败: ") + error.what(), "labels", "model_roi_failed");
  }
  if (ctx.cancelled())
    return cancelled();
  const auto space =
      Data::record(profile_contract::geometryContext(scan.primary->asProfile()->descriptor));
  BoxComputation candidate;
  candidate.missingSegments = boxes.missing_segments;
  candidate.complete = candidate.missingSegments.empty();
  candidate.refinements = {"GapRefinements",
                           {{"text", model::describeRefinements(boxes.refinements)},
                            {"items", Json::array()},
                            {"missingSegments", boxes.missing_segments}}};
  for (const auto& item : boxes.refinements)
    candidate.refinements.data["items"].push_back(
        {{"segment", item.segment},
         {"row", item.row},
         {"componentCount", item.component_count},
         {"keptSlots", item.kept_slots},
         {"droppedSlots", item.dropped_slots},
         {"anchorDistanceMm", finiteOrNull(item.anchor_distance_mm)},
         {"anchorGateMissed", item.anchor_gate_missed},
         {"pairDistanceMm", finiteOrNull(item.pair_distance_mm)},
         {"pairingDegraded", item.pairing_degraded}});
  const std::array<std::array<double, 4>, 4> bounds{
      boxes.rois.flush_base.values, boxes.rois.flush_ref.values, boxes.rois.gap_left.values,
      boxes.rois.gap_right.values};
  constexpr std::array<const char*, 4> names{"left_surface", "right_surface", "left_roll",
                                             "right_roll"};
  for (std::size_t index = 0; index < 4; ++index) {
    if (std::find(boxes.missing_segments.begin(), boxes.missing_segments.end(), names[index]) !=
        boxes.missing_segments.end())
      continue;
    Box2D box;
    for (int axis = 0; axis < 2; ++axis) {
      box.min[axis] = static_cast<float>(bounds[index][axis]) / 1000.0F;
      box.max[axis] = static_cast<float>(bounds[index][axis + 2]) / 1000.0F;
    }
    candidate.fields.boxes[index] = Data::box2d(box);
    if (!profile_contract::validGeometry(candidate.fields.boxes[index], why))
      return bad(why, "labels", "model_roi_failed");
    candidate.fields.contexts[index] = space;
  }
  if (candidate.complete) {
    const auto& left = *candidate.fields.boxes[2].asBox2D();
    const auto& right = *candidate.fields.boxes[3].asBox2D();
    const auto& datum = *candidate.fields.boxes[0].asBox2D();
    Box2D seam;
    for (int axis = 0; axis < 2; ++axis) {
      const auto lc = .5 * (static_cast<double>(left.min[axis]) + left.max[axis]);
      const auto rc = .5 * (static_cast<double>(right.min[axis]) + right.max[axis]);
      seam.min[axis] = seam.max[axis] = static_cast<float>(.5 * (lc + rc));
    }
    candidate.seam = Data::box2d(seam);
    candidate.seamContext = space;
    const double datumX =
        .5 * (static_cast<double>(datum.min[0]) * 1000 + static_cast<double>(datum.max[0]) * 1000);
    const double seamX =
        .25 * (static_cast<double>(left.min[0]) * 1000 + static_cast<double>(left.max[0]) * 1000 +
               static_cast<double>(right.min[0]) * 1000 + static_cast<double>(right.max[0]) * 1000);
    const bool onRight = datumX > seamX;
    candidate.fields.info = {profile_scene::kRoiInfoType,
                             {{"schemaVersion", 3},
                              {"datumSide", onRight ? "right" : "left"},
                              {"source", "model"},
                              {"roisFrom", "model"},
                              {"alignment", nullptr},
                              {"overallMm", nullptr},
                              {"cropStatus", nullptr},
                              {"processing",
                               {{"method", "model"},
                                {"status", "complete"},
                                {"details", {{"intensitySource", "rgb.R-or-zero"}}}}}}};
  }
  if (ctx.cancelled())
    return cancelled();
  result = std::move(candidate);
  return Status::Ok();
}
} // namespace lyflow::packs::gap::profile_model
