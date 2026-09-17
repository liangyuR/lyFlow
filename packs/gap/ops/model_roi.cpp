// 模型 ROI 路径（计划第二部分 §7–§9）：ONNX 分割 → 四框 → 跟随零件的裁剪窗。
// 推理在测量之前，输入必须是**原始 1280 槽**的传感器帧云（NaN 槽保留）。
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>

#include "algo/crop2d.h"
#include "gap_detection/GapUtils.hpp"
#include "gap_ml/RoiBoxes.hpp"
#include "gap_ml/RoiFeatures.hpp"
#include "gap_ops.h"
#include "lyflow/json_writer.h"

namespace lyflow::packs::gap {
namespace {

namespace fs = std::filesystem;
namespace ml = ::gap::ml;

/// 八类固定色表。下标即类 id：0/1/4/7 是背景与未用类，2/3/5/6 是契约里的四段
/// （left_surface / left_roll / right_roll / right_surface）。
constexpr std::uint8_t kClassColors[8][3] = {
    {60, 60, 60},     // 0 背景
    {120, 120, 120},  // 1 未用
    {60, 170, 255},   // 2 left_surface
    {255, 190, 40},   // 3 left_roll
    {120, 120, 120},  // 4 未用
    {255, 90, 60},    // 5 right_roll
    {90, 220, 120},   // 6 right_surface
    {200, 90, 220},   // 7 未用
};

/// Record 里的一行标签。缺字段或长度不对返回 false。
bool readLabelRow(const lyflow::Record& record, const char* key, std::vector<int>* out) {
  if (record.type != "GapLabels" || !record.data.contains(key)) return false;
  const auto& row = record.data[key];
  if (!row.is_array() || row.size() != ml::kProfileSlots) return false;
  out->clear();
  out->reserve(row.size());
  for (const auto& v : row) out->push_back(v.get<int>());
  return true;
}

// --------------------------------------------------- profile_tensor / labels_from_logits

/// 两片原始剖面 -> 模型输入张量 [2, 6, 1280]。通道构造是领域约定（T6）。
Status profileTensor(const Inputs& inputs, const ParamView&, Outputs& outputs, ExecContext& ctx) {
  const char* ports[2] = {"primary", "secondary"};
  ml::ProfileRow rows[2];
  for (int i = 0; i < 2; ++i) {
    const lyflow::PointCloud& cloud = *inputs.get(ports[i]).asCloud();
    if (!profileRowOf(cloud, &rows[i])) {
      return Status::Error(Phase::Execute, "bad_input",
                           std::string("模型要的是原始 ") + std::to_string(ml::kProfileSlots) +
                               " 槽剖面，这一片是 " + std::to_string(cloud.pointCount()) +
                               " 个点（load 时把 dropNonFinite 关掉）",
                           {}, ports[i]);
    }
  }

  lyflow::Tensor tensor;
  tensor.shape = {2, static_cast<std::int64_t>(ml::kNumChannels),
                  static_cast<std::int64_t>(ml::kProfileSlots)};
  tensor.data.reserve(tensor.elementCount());
  try {
    for (const auto& row : rows) {
      const std::vector<float> channels = ml::buildChannels(row);
      tensor.data.insert(tensor.data.end(), channels.begin(), channels.end());
    }
  } catch (const std::exception& e) {
    return Status::Error(Phase::Execute, "bad_input", std::string("通道构造失败: ") + e.what(), {},
                         "primary");
  }
  ctx.log(LogLevel::Info, "profile tensor " + tensor.shapeString());
  outputs.set("tensor", Data::tensor(std::move(tensor)));
  return Status::Ok();
}

/// 逐槽 argmax。严格的 > 让并列时留下最小的类 id，与 np.argmax 一致。
Status labelsFromLogits(const Inputs& inputs, const ParamView&, Outputs& outputs,
                        ExecContext& ctx) {
  const lyflow::Tensor* tensor = inputs.get("tensor").asTensor();
  if (tensor == nullptr || tensor->shape.size() != 3 || tensor->shape[0] != 2 ||
      tensor->shape[2] != static_cast<std::int64_t>(ml::kProfileSlots) || !tensor->consistent()) {
    return Status::Error(Phase::Execute, "bad_input",
                         "要的是 [2, 类别数, " + std::to_string(ml::kProfileSlots) +
                             "] 的 logits 张量",
                         {}, "tensor");
  }
  const std::int64_t classes = tensor->shape[1];
  const auto slots = static_cast<std::int64_t>(ml::kProfileSlots);
  const float* logits = tensor->data.data();

  std::array<std::vector<int>, 2> labels;
  for (std::int64_t batch = 0; batch < 2; ++batch) {
    auto& row = labels[static_cast<std::size_t>(batch)];
    row.resize(static_cast<std::size_t>(slots));
    const float* base = logits + batch * classes * slots;
    for (std::int64_t slot = 0; slot < slots; ++slot) {
      int best = 0;
      float bestValue = base[slot];
      for (std::int64_t c = 1; c < classes; ++c) {
        const float value = base[c * slots + slot];
        if (value > bestValue) {
          bestValue = value;
          best = static_cast<int>(c);
        }
      }
      row[static_cast<std::size_t>(slot)] = best;
    }
  }

  lyflow::Record record;
  record.type = "GapLabels";
  record.data["row0"] = labels[0];
  record.data["row1"] = labels[1];
  outputs.set("labels", Data::record(std::move(record)));

  std::array<int, 8> histogram{};
  for (const auto& row : labels) {
    for (int label : row) {
      if (label >= 0 && label < 8) histogram[static_cast<std::size_t>(label)] += 1;
    }
  }
  ctx.log(LogLevel::Info, "labels: left_surface=" + std::to_string(histogram[2]) +
                              " left_roll=" + std::to_string(histogram[3]) +
                              " right_roll=" + std::to_string(histogram[5]) +
                              " right_surface=" + std::to_string(histogram[6]));
  return Status::Ok();
}

// ------------------------------------------------------------------ roi_from_labels

Status roiFromLabels(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                     ExecContext& ctx) {
  // 底图原样透传（同一个 shared_ptr，零拷贝），没接就出一片空云 ——
  // 声明过的输出端口必须填。先写，错误路径上那四个框看不成也还有底图。
  const std::shared_ptr<const lyflow::PointCloud> backdrop = inputs.get("backdrop").cloudPtr();
  outputs.set("backdrop", backdrop ? Data::cloud(backdrop) : Data::cloud(lyflow::PointCloud{}));

  const lyflow::Record* record = inputs.get("labels").asRecord();
  if (record == nullptr || record->type != "GapLabels") {
    return Status::Error(Phase::Execute, "bad_input", "输入不是 GapLabels", {}, "labels");
  }
  std::vector<int> labels[2];
  if (!readLabelRow(*record, "row0", &labels[0]) || !readLabelRow(*record, "row1", &labels[1])) {
    return Status::Error(Phase::Execute, "bad_input", "标签行缺失或长度不是 1280", {}, "labels");
  }

  const char* ports[2] = {"primary", "secondary"};
  ml::ProfileRow rows[2];
  for (int i = 0; i < 2; ++i) {
    const lyflow::PointCloud& cloud = *inputs.get(ports[i]).asCloud();
    if (!profileRowOf(cloud, &rows[i])) {
      return Status::Error(Phase::Execute, "bad_input",
                           std::string("要的是原始 ") + std::to_string(ml::kProfileSlots) +
                               " 槽剖面，这一片是 " + std::to_string(cloud.pointCount()) + " 个点",
                           {}, ports[i]);
    }
  }

  ml::MaskRefineOptions options;
  options.enabled = params.flag("refine");
  options.split_step_mm = params.number("splitStepMm");
  options.split_slot_gap = static_cast<std::size_t>(std::max<std::int64_t>(0, params.integer("splitSlotGap")));
  options.link_mm = params.number("linkMm");
  options.min_component_slots =
      static_cast<std::size_t>(std::max<std::int64_t>(0, params.integer("minComponentSlots")));
  options.anchor_gap_mm = params.number("anchorGapMm");

  const ml::SegmentRow row0{&rows[0].x_mm, &rows[0].z_mm, &rows[0].valid, &labels[0]};
  const ml::SegmentRow row1{&rows[1].x_mm, &rows[1].z_mm, &rows[1].valid, &labels[1]};
  ml::RoiBoxesResult result;
  try {
    result = ml::boxesFromRefinedLabels(row0, row1, options);
  } catch (const std::exception& e) {
    return Status::Error(Phase::Execute, "model_roi_failed", std::string("推框失败: ") + e.what());
  }

  lyflow::Record refinements;
  refinements.type = "GapRefinements";
  refinements.data["text"] = ml::describeRefinements(result.refinements);
  refinements.data["items"] = nlohmann::json::array();
  for (const auto& r : result.refinements) {
    refinements.data["items"].push_back({{"segment", r.segment},
                                         {"row", r.row},
                                         {"componentCount", r.component_count},
                                         {"keptSlots", r.kept_slots},
                                         {"droppedSlots", r.dropped_slots},
                                         {"anchorDistanceMm", r.anchor_distance_mm},
                                         {"anchorGateMissed", r.anchor_gate_missed},
                                         {"pairingDegraded", r.pairing_degraded}});
  }
  refinements.data["missingSegments"] = result.missing_segments;
  outputs.set("refinements", Data::record(std::move(refinements)));

  if (!result.missing_segments.empty()) {
    std::string joined;
    for (const auto& s : result.missing_segments) {
      if (!joined.empty()) joined += ", ";
      joined += s;
    }
    return Status::Error(Phase::Execute, "model_roi_failed", "模型没标出这些段: " + joined);
  }

  // base_side 互换与模板路径同一个 apply_business_rois（GapDetection.cpp:234），
  // 只是这里的四框直接来自模型而不是模板。
  std::array<std::array<double, 4>, 4> boxes{
      result.rois.flush_base.values, result.rois.gap_left.values, result.rois.flush_ref.values,
      result.rois.gap_right.values};
  const char* outPorts[4] = {"flushBase", "gapLeft", "flushRef", "gapRight"};
  if (params.choice("baseSide") == "right") {
    std::swap(boxes[0], boxes[2]);
    outPorts[0] = "flushRef";
    outPorts[2] = "flushBase";
  }
  for (int i = 0; i < 4; ++i) {
    const auto& b = boxes[static_cast<std::size_t>(i)];
    outputs.set(outPorts[i], Data::box2d(boxFromMm(b[0], b[1], b[2], b[3])));
  }
  const std::string text = ml::describeRefinements(result.refinements);
  if (!text.empty()) ctx.log(LogLevel::Info, "refine: " + text);
  return Status::Ok();
}

// ------------------------------------------------------------------ labels_to_cloud

Status labelsToCloud(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                     ExecContext& ctx) {
  const lyflow::Record* record = inputs.get("labels").asRecord();
  if (record == nullptr || record->type != "GapLabels") {
    return Status::Error(Phase::Execute, "bad_input", "输入不是 GapLabels", {}, "labels");
  }
  std::vector<int> labels;
  const char* key = params.choice("row") == "secondary" ? "row1" : "row0";
  if (!readLabelRow(*record, key, &labels)) {
    return Status::Error(Phase::Execute, "bad_input", "标签行缺失或长度不是 1280", {}, "labels");
  }
  lyflow::PointCloud out = *inputs.get("cloud").asCloud();
  if (out.pointCount() != labels.size()) {
    return Status::Error(Phase::Execute, "bad_input",
                         "槽数 " + std::to_string(out.pointCount()) + " 不等于标签数 " +
                             std::to_string(labels.size()) +
                             "：这一步要的是原始 1280 槽（换轴之后、gap.drop_non_finite 之前）",
                         {}, "cloud");
  }
  out.rgb.assign(out.pointCount() * 3, 0);
  // intensity 也写一份类 id：LyFlow 的 3D 视图没有 rgb 着色模式，
  // 不给强度通道的话这个「只为了看」的算子在视图里什么也看不出来。
  out.intensity.assign(out.pointCount(), 0.0f);
  for (std::size_t i = 0; i < labels.size(); ++i) {
    const std::size_t cls = labels[i] >= 0 && labels[i] < 8 ? static_cast<std::size_t>(labels[i]) : 0;
    out.rgb[i * 3] = kClassColors[cls][0];
    out.rgb[i * 3 + 1] = kClassColors[cls][1];
    out.rgb[i * 3 + 2] = kClassColors[cls][2];
    out.intensity[i] = static_cast<float>(cls);
  }
  ctx.log(LogLevel::Info, "着色 " + std::to_string(out.pointCount()) + " 个槽（8 类固定色表）");
  outputs.set("cloud", Data::cloud(std::move(out)));
  return Status::Ok();
}

// ------------------------------------------------------------------ drop_non_finite

Status dropNonFinite(const Inputs& inputs, const ParamView&, Outputs& outputs, ExecContext&) {
  const lyflow::PointCloud& in = *inputs.get("cloud").asCloud();
  std::vector<std::int32_t> keep;
  keep.reserve(in.pointCount());
  for (std::size_t i = 0; i < in.pointCount(); ++i) {
    if (std::isfinite(in.xyz[i * 3]) && std::isfinite(in.xyz[i * 3 + 1]) &&
        std::isfinite(in.xyz[i * 3 + 2])) {
      keep.push_back(static_cast<std::int32_t>(i));
    }
  }
  outputs.set("cloud", Data::cloud(in.select(keep)));
  return Status::Ok();
}

// ------------------------------------------------------------------ roll_anchored_crop

/// 四边严格开区间，与 filter.crop_box2d(open) 同一条判据（判据本身在 lyflow_std_algo）。
std::vector<std::int32_t> insideStrict(const lyflow::PointCloud& in, const lyflow::Box2D& box) {
  const Eigen::Vector2f lo(box.min[0], box.min[1]);
  const Eigen::Vector2f hi(box.max[0], box.max[1]);
  std::vector<std::int32_t> keep;
  keep.reserve(in.pointCount());
  for (std::size_t i = 0; i < in.pointCount(); ++i) {
    if (lyflow::std_pc::insideBox2D(in.xyz[i * 3], in.xyz[i * 3 + 1], lo, hi,
                                    lyflow::std_pc::Bounds2D::Open)) {
      keep.push_back(static_cast<std::int32_t>(i));
    }
  }
  return keep;
}

::detection::domain::Roi roiOf(const lyflow::Box2D& box) {
  ::detection::domain::Roi roi;
  roi.values = {mToMm(box.min[0]), mToMm(box.min[1]), mToMm(box.max[0]), mToMm(box.max[1])};
  return roi;
}

/// 没有窗时的诊断框：两片云真正剩下的包围盒（复刻 Alignment.cpp:402 的 honest diagnostics）。
lyflow::Box2D boundsOf(const lyflow::PointCloud& a, const lyflow::PointCloud& b) {
  float lo[2] = {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity()};
  float hi[2] = {-lo[0], -lo[1]};
  for (const lyflow::PointCloud* c : {&a, &b}) {
    for (std::size_t i = 0; i < c->pointCount(); ++i) {
      for (int k = 0; k < 2; ++k) {
        lo[k] = std::min(lo[k], c->xyz[i * 3 + static_cast<std::size_t>(k)]);
        hi[k] = std::max(hi[k], c->xyz[i * 3 + static_cast<std::size_t>(k)]);
      }
    }
  }
  lyflow::Box2D box;
  if (lo[0] > hi[0]) return box;
  box.min[0] = lo[0];
  box.min[1] = lo[1];
  box.max[0] = hi[0];
  box.max[1] = hi[1];
  return box;
}

Status rollAnchoredCrop(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                        ExecContext& ctx) {
  const lyflow::PointCloud& primary = *inputs.get("primary").asCloud();
  const lyflow::PointCloud& secondary = *inputs.get("secondary").asCloud();

  ::detection::domain::RollAnchoredCropConfiguration config;
  config.enabled = params.flag("enabled");
  config.half_width_mm = params.number("halfWidth");
  config.half_height_mm = params.number("halfHeight");
  config.max_roll_box_height_mm = params.number("maxRollBoxHeight");
  config.min_points_kept =
      static_cast<std::size_t>(std::max<std::int64_t>(0, params.integer("minPointsKept")));

  const auto crop = ::detection::utils::computeRollAnchoredCropMm(
      roiOf(*inputs.get("gapLeft").asBox2D()), roiOf(*inputs.get("gapRight").asBox2D()), config);

  std::string status;
  lyflow::Box2D window;
  std::size_t keptPrimary = primary.pointCount();
  std::size_t keptSecondary = secondary.pointCount();
  bool applied = false;

  if (crop.box_mm) {
    const auto& b = *crop.box_mm;
    const lyflow::Box2D candidate = boxFromMm(b[0], b[1], b[2], b[3]);
    const std::vector<std::int32_t> keepP = insideStrict(primary, candidate);
    const std::vector<std::int32_t> keepS = insideStrict(secondary, candidate);
    // 点数保护：一个由错标 roll 推出的窗绝不能把测量饿死（Alignment.cpp:376）。
    // seg_mode: ROI 下 allow_single_camera 为真，所以 Both 看的是两片之和。
    const std::string camera = params.choice("usingCamera");
    const std::size_t minimum = config.min_points_kept;
    bool insufficient = false;
    if (camera == "Left") {
      insufficient = keepP.size() < minimum;
    } else if (camera == "Right") {
      insufficient = keepS.size() < minimum;
    } else {
      insufficient = keepP.size() + keepS.size() < minimum;
    }
    if (insufficient) {
      status = "reverted:min_points";
      keptPrimary = keepP.size();
      keptSecondary = keepS.size();
    } else {
      applied = true;
      status = "applied";
      window = candidate;
      outputs.set("primary", Data::cloud(primary.select(keepP)));
      outputs.set("secondary", Data::cloud(secondary.select(keepS)));
      keptPrimary = keepP.size();
      keptSecondary = keepS.size();
    }
  } else {
    status = std::string(config.enabled ? "rejected:" : "") + crop.skip_reason;
  }

  if (!applied) {
    // 原样透传，窗退成两片云真正的包围盒 —— 无界框画在视图里没有意义。
    outputs.set("primary", Data::cloud(primary));
    outputs.set("secondary", Data::cloud(secondary));
    window = boundsOf(primary, secondary);
    keptPrimary = primary.pointCount();
    keptSecondary = secondary.pointCount();
  }
  outputs.set("window", Data::box2d(window));

  lyflow::Record record;
  record.type = "GapRollCrop";
  record.data["status"] = status;
  record.data["applied"] = applied;
  record.data["beforePrimary"] = primary.pointCount();
  record.data["beforeSecondary"] = secondary.pointCount();
  record.data["afterPrimary"] = keptPrimary;
  record.data["afterSecondary"] = keptSecondary;
  record.data["minPointsKept"] = config.min_points_kept;
  if (crop.box_mm) {
    record.data["boxMm"] = *crop.box_mm;
  }
  outputs.set("status", Data::record(std::move(record)));
  ctx.log(LogLevel::Info, "roll crop " + status + ": " + std::to_string(primary.pointCount()) +
                              "+" + std::to_string(secondary.pointCount()) + " → " +
                              std::to_string(keptPrimary) + "+" + std::to_string(keptSecondary));
  return Status::Ok();
}

// ---------------------------------------------------------------------- 参数小工具

Param boolParam(const char* name, const char* label, bool def, const char* doc) {
  Param p;
  p.name = name;
  p.type = ParamType::Bool;
  p.label = label;
  p.doc = doc;
  p.def = Value::boolean(def);
  return p;
}

Param mmParam(const char* name, const char* label, double def, const char* doc) {
  Param p;
  p.name = name;
  p.type = ParamType::Float;
  p.label = label;
  p.doc = doc;
  p.def = Value::number(def);
  p.unit = "mm";
  p.min = 0.0;
  return p;
}

Param intParam(const char* name, const char* label, std::int64_t def, const char* doc) {
  Param p;
  p.name = name;
  p.type = ParamType::Int;
  p.label = label;
  p.doc = doc;
  p.def = Value::integer(def);
  p.min = 0.0;
  return p;
}

}  // namespace

void registerProfileTensor(Registry& r) {
  OperatorDesc op;
  op.id = "gap.profile_tensor";
  op.version = "1.0.0";
  op.label = "剖面张量";
  op.category = "间隙/模型";
  op.keywords = {"onnx", "tensor", "channels", "剖面", "张量"};
  op.doc =
      "把两片**原始 1280 槽**剖面拼成模型输入张量 [2, 6, 1280]。"
      "六个通道是 z_norm / x_diff / normal_angle / curvature / intensity_norm / valid，"
      "逐条对着 ml_handoff 的 build_channels 契约。\n"
      "输入必须保留 NaN 槽（load 时把 dropNonFinite 关掉），点数不是 1280 直接报错。"
      "推理本身是通用的，接 ml.onnx_run（T6）。";
  op.preconditions = {
      "输入必须是**原始 1280 槽**、保留了空槽的传感器帧剖面（load 时 dropNonFinite 关掉），"
      "槽数不是 1280 直接报错。",
      "六个通道的口径与训练侧 ml_handoff 的 build_channels 逐条对应；训练那边改了通道定"
      "义，这里也要跟着改，否则模型吃到的是另一份特征。",
  };
  // 空槽 bug 的那条不变量（ADR-0024）：槽数必须正好 1280，值一到端口上就查。
  // **刻意不写 finite** —— 这个算子要的恰恰是**保留了 NaN 空槽**的原始剖面，
  // 声明 finite:true 会把它唯一正确的输入判成违反。
  const nlohmann::json slots = {{"elementCount", {{"eq", ml::kProfileSlots}}}};
  op.inputs = {
      withContract(
          Port{"primary", "PointCloud", "Primary", "Master 剖面，传感器 XZ 帧、1280 槽。", true},
          slots),
      withContract(
          Port{"secondary", "PointCloud", "Secondary", "Slave 剖面，传感器 XZ 帧、1280 槽。", true},
          slots),
  };
  op.outputs = {Port{"tensor", "Tensor", "Tensor", "[2, 6, 1280] 的 float32 张量。", true}};
  op.capabilities = {false, false, true};
  op.compute = &profileTensor;
  r.addOperator(std::move(op));
}

void registerLabelsFromLogits(Registry& r) {
  OperatorDesc op;
  op.id = "gap.labels_from_logits";
  op.version = "1.0.0";
  op.label = "由 Logits 得到标签";
  op.category = "间隙/模型";
  op.keywords = {"argmax", "labels", "logits", "分割", "类别"};
  op.doc =
      "逐槽 argmax：[2, 类别数, 1280] 的 logits -> 两行各 1280 个类 id。"
      "并列时留最小的类 id（与 np.argmax 一致）。";
  op.preconditions = {
      "假定 logits 形状是 [2, 类别数, 1280]，行 0 是 primary、行 1 是 secondary；行序错"
      "了四个框会整体对调。",
      "只做 argmax，不看置信度：模型对整帧都没把握时照样给出满满一行标签。",
  };
  // [2, 类别数, 1280]：批与槽是定死的，类别数随模型走，所以中间那一维是 -1。
  op.inputs = {withContract(
      Port{"tensor", "Tensor", "Logits", "ml.onnx_run 的输出。", true},
      {{"shape", {2, -1, static_cast<std::int64_t>(ml::kProfileSlots)}}})};
  op.outputs = {withExample(
      Port{"labels", "Record", "Labels", "GapLabels：两行各 1280 个类 id。", true},
      examples::labels())};
  op.capabilities = {false, false, true};
  op.compute = &labelsFromLogits;
  r.addOperator(std::move(op));
}

void registerRoiFromLabels(Registry& r) {
  OperatorDesc op;
  op.id = "gap.roi_from_labels";
  op.version = "1.0.0";
  op.label = "由标签得到 ROI";
  op.category = "间隙/模型";
  op.keywords = {"roi", "boxes", "refine", "模型框"};
  op.doc =
      "逐槽标签 → 四个业务 ROI。框在这里就换成测量帧的米（x→x, z→y, /1000），"
      "下游的裁剪与叠画零改动（H3）。有 missing_segments 就报 model_roi_failed。\n"
      "可选的 backdrop 接一片**测量帧**的同帧云（通常是 gap.labels_to_cloud 的输出），"
      "原样透传到同名输出：这样选中本节点时四个框就叠在自己的剖面底图上，"
      "而不是靠底图规则去上游借一片传感器帧的云（那一片在 2D 剖面里退化成一条线）。";
  op.preconditions = {
      "假定模型给出的四个语义段都在；缺段就报 model_roi_failed，不会自动退回模板路径。",
      "labels 的槽号与两片输入剖面一一对应，所以两片剖面必须是没删过点的原始 1280 槽。",
      "backdrop 只是叠画底图，原样透传，不参与推框。",
  };
  // 槽号与标签一一对应 —— 上游删过点就全错位了，所以两片剖面都查槽数（ADR-0024）。
  const nlohmann::json slots = {{"elementCount", {{"eq", ml::kProfileSlots}}}};
  op.inputs = {
      withContract(Port{"primary", "PointCloud", "Primary", "原始 1280 槽的 Master 剖面。", true},
                   slots),
      withContract(Port{"secondary", "PointCloud", "Secondary", "原始 1280 槽的 Slave 剖面。", true},
                   slots),
      withContract(Port{"labels", "Record", "Labels", "gap.onnx_segment 的输出。", true},
                   {{"recordType", "GapLabels"}}),
      Port{"backdrop", "PointCloud", "Backdrop",
           "可选：叠画用的同帧底图云（测量帧）。原样透传，不参与推框。", false},
  };
  op.outputs = {
      Port{"flushBase", "Box2D", "Flush Base", "段差基准面 ROI。", true},
      Port{"gapLeft", "Box2D", "Gap Left", "间隙左侧 ROI（模型的 left_roll）。", true},
      Port{"flushRef", "Box2D", "Flush Ref", "段差参考面 ROI。", true},
      Port{"gapRight", "Box2D", "Gap Right", "间隙右侧 ROI（模型的 right_roll）。", true},
      withExample(Port{"refinements", "Record", "Refinements", "掩膜精修的诊断。", true},
                  examples::refinements()),
      Port{"backdrop", "PointCloud", "Backdrop", "backdrop 输入的原样透传；没接就是空云。", true},
  };

  Param baseSide;
  baseSide.name = "baseSide";
  baseSide.type = ParamType::Enum;
  baseSide.label = "Base Side";
  baseSide.doc = "基准面在左还是右。right 时 flush_base 与 flush_ref 互换（与模板路径同一条）。";
  baseSide.def = Value::text("left");
  baseSide.options = {EnumOption{"left", "Left", ""}, EnumOption{"right", "Right", ""}};

  auto onlyWhenRefine = [](Param p) {
    p.visibleWhen.param = "refine";
    p.visibleWhen.eq = Value::boolean(true);
    return p;
  };
  op.params = {
      baseSide,
      boolParam("refine", "Refine Masks", true,
                "掩膜精修 refine-v1。关掉就是 boxesFromLabels 的逐位契约行为。"),
      onlyWhenRefine(mmParam("splitStepMm", "Split Step", 1.0, "相邻槽的欧氏步长超过它就断开。")),
      onlyWhenRefine(intParam("splitSlotGap", "Split Slot Gap", 16, "槽号间隔超过它就断开。")),
      onlyWhenRefine(mmParam("linkMm", "Link Distance", 3.0, "包围盒相距小于它的连通块重新合并。")),
      onlyWhenRefine(intParam("minComponentSlots", "Min Component Slots", 4,
                              "小于这么多槽的连通块直接丢掉。")),
      onlyWhenRefine(mmParam("anchorGapMm", "Anchor Gap", 5.0, "面段离 roll 锚框多近才算数。")),
  };
  op.capabilities = {false, true, true};
  op.compute = &roiFromLabels;
  r.addOperator(std::move(op));
}

void registerLabelsToCloud(Registry& r) {
  OperatorDesc op;
  op.id = "gap.labels_to_cloud";
  op.version = "1.0.0";
  op.label = "标签映射到点云";
  op.category = "间隙/模型";
  op.keywords = {"labels", "color", "着色", "分割结果"};
  op.doc =
      "按类别给点上色，只为了在 3D 视图里看分割结果。八类固定色表写进 rgb，"
      "同时把类 id 写进 intensity —— 视图没有 rgb 着色模式，靠强度才看得见。\n"
      "接**测量帧**的云（gap.to_measurement_frame 之后、gap.drop_non_finite 之前）："
      "换轴只换轴不删点，槽位与标签仍一一对应，而 2D 剖面俯视 XY 才看得出形状；"
      "删过点的云槽位会错位，所以槽数不是 1280 直接报 bad_input。";
  op.preconditions = {
      "只为了看分割结果，不参与测量：它改的是 rgb 与 intensity，几何一个点都不动。",
      "要接**测量帧**、且**没删过点**的 1280 槽云（换轴之后、剔非有限点之前）；槽数不是"
      " 1280 报 bad_input。",
  };
  op.inputs = {
      withContract(Port{"cloud", "PointCloud", "Cloud",
                        "与标签同序的 1280 槽剖面，通常是 gap.to_measurement_frame 的输出。", true},
                   {{"elementCount", {{"eq", ml::kProfileSlots}}}}),
      withContract(Port{"labels", "Record", "Labels", "gap.onnx_segment 的输出。", true},
                   {{"recordType", "GapLabels"}}),
  };
  op.outputs = {Port{"cloud", "PointCloud", "Cloud",
                     "按类着色的剖面。可以接进 gap.roi_from_labels.backdrop 当底图。", true}};

  Param row;
  row.name = "row";
  row.type = ParamType::Enum;
  row.label = "Row";
  row.doc = "这片云是哪一行：primary 是 row0，secondary 是 row1。";
  row.def = Value::text("primary");
  row.options = {EnumOption{"primary", "Primary (row 0)", ""},
                 EnumOption{"secondary", "Secondary (row 1)", ""}};
  op.params = {row};
  op.capabilities = {false, true, true};
  op.compute = &labelsToCloud;
  r.addOperator(std::move(op));
}

void registerDropNonFinite(Registry& r) {
  OperatorDesc op;
  op.id = "gap.drop_non_finite";
  op.version = "1.0.0";
  op.label = "剔除非有限值";
  op.category = "间隙/预处理";
  // 关键词不能出现小写的 n-a-n：core 的 manifest 序列化用例是子串匹配（test_executor.cpp:419）
  op.keywords = {"NaN", "finite", "剔除", "无效槽"};
  op.doc =
      "剔除非有限点，对应 NonFinitePointPolicy::kRemove。"
      "模型路径里 load 必须保留 NaN 槽，所以这一步单独拿出来。";
  op.preconditions = {
      "删点会打乱槽号：凡是按槽号与标签或张量对齐的算子（gap.profile_tensor、"
      "gap.labels_to_cloud、gap.roi_from_labels）都要接在它之前。",
      "只看坐标是否有限，不做任何离群点剔除。",
  };
  op.inputs = {Port{"cloud", "PointCloud", "Cloud", "可能带 NaN 槽的点云。", true}};
  op.outputs = {Port{"cloud", "PointCloud", "Cloud", "只剩有限点。", true}};
  op.capabilities = {false, true, true};
  op.compute = &dropNonFinite;
  r.addOperator(std::move(op));
}

void registerRollAnchoredCrop(Registry& r) {
  OperatorDesc op;
  op.id = "gap.roll_anchored_crop";
  op.version = "1.0.0";
  op.label = "随动裁剪";
  op.category = "间隙/模型";
  op.keywords = {"crop", "roll", "window", "跟随零件"};
  op.doc =
      "跟随零件的整体裁剪窗：中心取两个 roll 框中心的中点，半宽半高来自参数。"
      "五种失效保护（disabled / bad_config / degenerate_roll_box / roll_box_height / span）"
      "与裁后点数不足时回退无界框，全部复刻（H4）。";
  op.preconditions = {
      "假定两个 roll 框来自同一帧的模型推理且都可信；单框高度超过 maxRollBoxHeight 就整"
      "帧拒绝裁剪，两片云原样透传。",
      "裁后点数不足 minPointsKept 时丢掉窗、回退无界框 —— 下游拿到的是没裁过的云，不"
      "是空云。",
      "窗恒为轴对齐，半宽半高是固定参数，不随零件姿态旋转。",
  };
  op.inputs = {
      Port{"primary", "PointCloud", "Primary", "测量帧的 Master 云（已剔非有限点）。", true},
      Port{"secondary", "PointCloud", "Secondary", "测量帧的 Slave 云。", true},
      Port{"gapLeft", "Box2D", "Gap Left", "模型的 left_roll 框。", true},
      Port{"gapRight", "Box2D", "Gap Right", "模型的 right_roll 框。", true},
  };
  op.outputs = {
      Port{"primary", "PointCloud", "Primary", "裁过（或原样透传）的 Master 云。", true},
      Port{"secondary", "PointCloud", "Secondary", "裁过（或原样透传）的 Slave 云。", true},
      Port{"window", "Box2D", "Window", "真正生效的窗；没生效时是剩余点的包围盒。", true},
      withExample(Port{"status", "Record", "Status", "GapRollCrop：状态与前后点数。", true},
                  examples::rollCrop()),
  };

  Param camera;
  camera.name = "usingCamera";
  camera.type = ParamType::Enum;
  camera.label = "Using Camera";
  camera.doc = "点数保护看哪几片。Both 看两片之和（seg_mode: ROI 下的原行为）。";
  camera.def = Value::text("Both");
  camera.options = {EnumOption{"Both", "Both", "两片之和。"},
                    EnumOption{"Left", "Left", "只看 primary。"},
                    EnumOption{"Right", "Right", "只看 secondary。"}};

  op.params = {
      boolParam("enabled", "Enabled", true, "关掉就是 skip_reason: disabled，两片云原样透传。"),
      mmParam("halfWidth", "Half Width", 35.0, "窗的半宽。"),
      mmParam("halfHeight", "Half Height", 20.0, "窗的半高。"),
      mmParam("maxRollBoxHeight", "Max Roll Box Height", 8.0,
              "单个 roll 框的最大合理高度。这是信任判据不是调优参数，超了整帧拒绝。"),
      intParam("minPointsKept", "Min Points Kept", 50, "裁后少于这么多点就丢掉窗、回退无界框。"),
      camera,
  };
  op.capabilities = {false, true, true};
  op.compute = &rollAnchoredCrop;
  r.addOperator(std::move(op));
}

}  // namespace lyflow::packs::gap
