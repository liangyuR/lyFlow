#include "profile_migration.h"
#include "profile_model_ops.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "lyflow_profile/provenance.h"
#include "lyflow_profile/selection.h"
#ifdef LYFLOW_GAP_MODEL
#include "lyflow_ml/runtime.h"
#endif

namespace lyflow::packs::gap::profile_model {
namespace {
Status bad(const std::string& message, const char* port = "", const char* code = "bad_input") {
  return Status::Error(Phase::Execute, code, message, {}, port);
}
Status cancelled() { return bad("模型节点执行已取消", "", "cancelled"); }
constexpr std::uint8_t colors[8][3]{{60, 60, 60},   {120, 120, 120}, {60, 170, 255},
                                    {255, 190, 40}, {120, 120, 120}, {255, 90, 60},
                                    {90, 220, 120}, {200, 90, 220}};
Param text(const char* name, const char* initial = "") {
  Param p;
  p.name = name;
  p.label = name;
  p.type = ParamType::String;
  p.def = Value::text(initial);
  return p;
}
Param mm(const char* name, double initial) {
  auto p = text(name);
  p.type = ParamType::Float;
  p.def = Value::number(initial);
  p.unit = "mm";
  p.min = 0;
  return p;
}
Param integer(const char* name, std::int64_t initial) {
  auto p = text(name);
  p.type = ParamType::Int;
  p.def = Value::integer(initial);
  p.min = 0;
  return p;
}
Param flag(const char* name, bool initial) {
  auto p = text(name);
  p.type = ParamType::Bool;
  p.def = Value::boolean(initial);
  return p;
}
#ifdef LYFLOW_GAP_MODEL
Param camera(const char* name) {
  auto p = text(name, "Both");
  p.type = ParamType::Enum;
  p.options = {{"Both", "Both"}, {"Left", "Left"}, {"Right", "Right"}};
  return p;
}
#endif
OperatorDesc description(const char* id, const char* label) {
  OperatorDesc op;
  op.id = id;
  op.version = "2.0.0";
  declareGapGraphUpgrade(op);
  op.label = label;
  op.category = "间隙/模型";
  op.capabilities.cancellable = true;
  op.capabilities.deterministic = true;
  return op;
}
std::vector<Param> refineParams() {
  std::vector<Param> result{flag("refine", true),
                            mm("splitStepMm", 1),
                            integer("splitSlotGap", 16),
                            mm("linkMm", 3),
                            integer("minComponentSlots", 4),
                            mm("anchorGapMm", 5)};
  for (std::size_t i = 1; i < result.size(); ++i) {
    result[i].visibleWhen.param = "refine";
    result[i].visibleWhen.eq = Value::boolean(true);
  }
  return result;
}
::gap::ml::MaskRefineOptions refineOptions(const ParamView& p) {
  ::gap::ml::MaskRefineOptions result;
  result.enabled = p.flag("refine");
  result.split_step_mm = p.number("splitStepMm");
  result.split_slot_gap =
      static_cast<std::size_t>(std::max<std::int64_t>(0, p.integer("splitSlotGap")));
  result.link_mm = p.number("linkMm");
  result.min_component_slots =
      static_cast<std::size_t>(std::max<std::int64_t>(0, p.integer("minComponentSlots")));
  result.anchor_gap_mm = p.number("anchorGapMm");
  return result;
}
void publishRegions(const ModelRegionResult& result, Outputs& out) {
  constexpr std::array<const char*, 4> names{"flushBase", "flushRef", "gapLeft", "gapRight"};
  constexpr std::array<const char*, 4> contexts{"flushBaseContext", "flushRefContext",
                                                "gapLeftContext", "gapRightContext"};
  out.set("rois", result.rois);
  for (std::size_t i = 0; i < 4; ++i) {
    out.set(names[i], result.boxes.fields.boxes[i]);
    out.set(contexts[i], result.boxes.fields.contexts[i]);
  }
  out.set("seam", result.boxes.seam);
  out.set("seamContext", result.boxes.seamContext);
}
} // namespace

Status extractModelRegions(const Data& labels, const Data& scan,
                           const ::gap::ml::MaskRefineOptions& options, ModelRegionResult& result,
                           ExecContext& context) {
  BoxComputation boxes;
  if (auto status = boxesFromLabels(labels, scan, options, boxes, context); !status.ok)
    return status;
  if (!boxes.complete) {
    std::string missing;
    for (const auto& name : boxes.missingSegments) {
      if (!missing.empty())
        missing += ", ";
      missing += name;
    }
    result.boxes = std::move(boxes);
    return bad("模型没标出这些段: " + missing, "labels", "model_roi_failed");
  }
  ModelRegionResult output;
  output.boxes = std::move(boxes);
  std::string why;
  if (!profile_scene::makeRoiSet(output.boxes.fields, output.rois, why))
    return bad(why, "labels", "model_roi_failed");
  if (context.cancelled())
    return cancelled();
  if (!output.boxes.refinements.data["text"].get<std::string>().empty())
    context.log(LogLevel::Info,
                "refine: " + output.boxes.refinements.data["text"].get<std::string>());
  if (context.cancelled())
    return cancelled();
  result = std::move(output);
  return Status::Ok();
}
Status labelsToCloud(const Data& scanData, const Data& labelData, const std::string& row,
                     Data& result, ExecContext& context) {
  if (context.cancelled())
    return cancelled();
  if (row != "primary" && row != "secondary")
    return bad("标签可视化row必须明确primary或secondary", "row", "bad_param");
  profile_scene::ScanView scan;
  std::string why;
  if (!profile_scene::readScanPair(scanData, scan, why))
    return bad(why, "scan");
  if (!matchesSensors(labelData, *scan.sensorPrimary, *scan.sensorSecondary, why))
    return bad(why, "labels");
  LabelRowsView labels;
  if (!readLabelRows(labelData, labels, why))
    return bad(why, "labels");
  const auto& profile = *(row == "primary" ? scan.primary : scan.secondary)->asProfile();
  if (profile.pointCount() != 1280 || profile.descriptor.sourceCount != 1280 ||
      profile.descriptor.sampleOrder != "acquisition")
    return bad("着色需要完整1280个原量测槽，不能使用裁剪/压缩后的轮廓", "scan");
  for (std::size_t i = 0; i < 1280; ++i)
    if (profile.sourceIndices[i] != i)
      return bad("着色源索引必须与原始1280槽一致", "scan");
  PointCloud output = *profile.cloud;
  PointCloud identity;
  output.id = identity.id;
  output.rgb.assign(3840, 0);
  output.intensity.assign(1280, 0);
  const std::size_t offset = row == "secondary" ? 1280 : 0;
  for (std::size_t i = 0; i < 1280; ++i) {
    if ((i & 255U) == 0 && context.cancelled())
      return cancelled();
    const auto value = static_cast<int>(labels.labels->data[offset + i]);
    const auto cls = value >= 0 && value < 8 ? static_cast<std::size_t>(value) : 0;
    for (std::size_t channel = 0; channel < 3; ++channel)
      output.rgb[3 * i + channel] = colors[cls][channel];
    output.intensity[i] = static_cast<float>(cls);
  }
  if (context.cancelled())
    return cancelled();
  context.log(LogLevel::Info, "着色 1280 个槽（8类固定色表，仅诊断PointCloud）");
  if (context.cancelled())
    return cancelled();
  result = Data::cloud(std::move(output));
  return Status::Ok();
}

#ifdef LYFLOW_GAP_MODEL
Status inferProfileTensor(const Tensor& input, const std::filesystem::path& modelPath,
                          Tensor& result, ExecContext& context) {
  if (context.cancelled())
    return cancelled();
  if (input.shape != std::vector<std::int64_t>{2, 6, 1280} || !input.consistent() ||
      !std::all_of(input.data.begin(), input.data.end(),
                   [](float value) { return std::isfinite(value); }))
    return bad("剖面推理需要有限float32特征Tensor[2,6,1280]", "input");
  if (modelPath.empty())
    return Status::Error(Phase::Execute, "bad_param", "必须选择剖面ONNX模型", "modelPath");
  ::lyflow::ml::RuntimeOptions options;
  options.modelPath = modelPath;
  options.intraOpThreads = 1;
  options.modelContract = "generic";
  options.batchMode = "model";
  Tensor candidate;
  if (auto status = ::lyflow::ml::inferTensor(input, options, candidate, context); !status.ok) {
    if (status.paramPath.empty() || status.paramPath == "modelPath")
      status.paramPath = "modelPath";
    return status;
  }
  if (candidate.shape.size() != 3 || candidate.shape[0] != 2 || candidate.shape[1] < 1 ||
      candidate.shape[1] > 256 || candidate.shape[2] != 1280 || !candidate.consistent())
    return Status::Error(Phase::Execute, "bad_model_output",
                         "剖面模型输出必须为float32 Tensor[2,1..256,1280]", "modelPath", "output");
  if (context.cancelled())
    return cancelled();
  result = std::move(candidate);
  return Status::Ok();
}
Status locateModel(const Data& input, const LocateModelOptions& options, LocateModelResult& result,
                   ExecContext& context) {
  if (context.cancelled())
    return cancelled();
  profile_scene::ScanView scan;
  std::string why;
  if (!profile_scene::readScanPair(input, scan, why))
    return bad(why, "scan");
  if (scan.info->data["stage"] != "raw")
    return bad("模型定位需要raw完整原槽场景，不从processed场景恢复旧输入", "scan");
  Tensor modelInput, logits;
  if (auto status = buildTensor(*scan.sensorPrimary, *scan.sensorSecondary, modelInput, context);
      !status.ok)
    return status;
  if (auto status = inferProfileTensor(modelInput, options.modelPath, logits, context);
      !status.ok) {
    return status;
  }
  Data labels;
  if (auto status = labelsFromLogits(logits, *scan.sensorPrimary, *scan.sensorSecondary, labels,
                                     context, &modelInput);
      !status.ok)
    return status;
  ModelRegionResult regions;
  if (auto status = extractModelRegions(labels, input, options.refine, regions, context);
      !status.ok) {
    if (!regions.boxes.refinements.type.empty())
      result.refinements = regions.boxes.refinements;
    return status;
  }
  const auto* leftContext = regions.boxes.fields.contexts[2].asRecord();
  const auto* rightContext = regions.boxes.fields.contexts[3].asRecord();
  profile_contract::SelectionResult finite[2];
  const Data* measured[2]{scan.primary, scan.secondary};
  const Record* provenance[2]{&scan.primaryProvenance, &scan.secondaryProvenance};
  profile_contract::SelectionOptions selection;
  selection.emptyPolicy = "allow";
  for (int side = 0; side < 2; ++side) {
    const auto& profile = *measured[side]->asProfile();
    Indices keep{profile.cloud->id, {}};
    for (std::size_t i = 0; i < profile.pointCount(); ++i) {
      if ((i & 255U) == 0 && context.cancelled())
        return cancelled();
      if (profile.validity[i])
        keep.values.push_back(static_cast<std::int32_t>(i));
    }
    if (auto status = profile_contract::selectProfileIndices(
            *measured[side], keep, provenance[side], selection, finite[side], context);
        !status.ok)
      return status;
  }
  const auto& rawInfo = scan.info->data;
  profile_scene::ScanMetadata finiteMetadata{rawInfo["sensorFrame"],
                                             rawInfo["measurementFrame"],
                                             "processed",
                                             rawInfo["guard"],
                                             nullptr,
                                             rawInfo["origins"],
                                             rawInfo["processing"],
                                             finite[0].provenance,
                                             finite[1].provenance};
  finiteMetadata.processing["method"] = "model";
  finiteMetadata.processing["details"]["operation"] = "pre_roll_validity_compact";
  Data finiteScan;
  if (!profile_scene::makeScanPair(*scan.sensorPrimary, *scan.sensorSecondary, finite[0].profile,
                                   finite[1].profile, *scan.merged, *scan.mergedProvenance,
                                   finiteMetadata, finiteScan, why))
    return bad(why, "scan");
  profile_scene::RollCropResult crop;
  if (auto status = profile_scene::rollAnchoredCrop(finiteScan, regions.boxes.fields.boxes[2],
                                                    *leftContext, regions.boxes.fields.boxes[3],
                                                    *rightContext, options.crop, crop, context);
      !status.ok)
    return status;
  regions.boxes.fields.info.data["overallMm"] = {
      static_cast<double>(crop.window.asBox2D()->min[0]) * 1000.,
      static_cast<double>(crop.window.asBox2D()->min[1]) * 1000.,
      static_cast<double>(crop.window.asBox2D()->max[0]) * 1000.,
      static_cast<double>(crop.window.asBox2D()->max[1]) * 1000.};
  regions.boxes.fields.info.data["cropStatus"] = crop.status.data;
  LocateModelResult output;
  output.labels = labels;
  output.refinements = regions.boxes.refinements;
  if (!profile_scene::makeRoiSet(regions.boxes.fields, output.rois, why))
    return bad(why, "labels", "model_roi_failed");
  profile_scene::ScanView cropped;
  if (!profile_scene::readScanPair(crop.scan, cropped, why))
    return bad(why, "scan");
  const auto& info = cropped.info->data;
  auto processing = info["processing"];
  processing["method"] = "model";
  processing["status"] = "complete";
  const auto* labelInfo = labels.asBundle()->field("info")->asRecord();
  processing["details"]["labelsHash"] = labelInfo->data["labelsHash"];
  processing["details"]["inputHash"] = labelInfo->data["inputHash"];
  processing["details"]["logitsHash"] = labelInfo->data["logitsHash"];
  processing["details"]["inferenceAssociation"] = "direct_infer_call";
  processing["details"]["modelFingerprint"] = ::lyflow::ml::modelFingerprint(options.modelPath);
  processing["details"]["mergedRebuilt"] = false;
  profile_scene::ScanMetadata metadata{
      info["sensorFrame"],   info["measurementFrame"],  "processed",
      info["guard"],         crop.status.data,          info["origins"],
      std::move(processing), cropped.primaryProvenance, cropped.secondaryProvenance};
  if (!profile_scene::makeScanPair(*cropped.sensorPrimary, *cropped.sensorSecondary,
                                   *cropped.primary, *cropped.secondary, *cropped.merged,
                                   *cropped.mergedProvenance, metadata, output.scan, why))
    return bad(why, "scan");
  if (context.cancelled())
    return cancelled();
  result = std::move(output);
  return Status::Ok();
}
#endif

void registerProfileModelOps(Registry& registry) {
  auto tensor = description("gap.profile_tensor", "原生剖面特征张量构建");
  tensor.inputs = {
      {"primary", "Profile", "首传感器原槽", "1280槽、XZ、acquisition、原源索引。", true},
      {"secondary", "Profile", "次传感器原槽", "RGB.R为旧约定强度，保NaN槽。", true}};
  tensor.outputs = {{"tensor", "Tensor", "特征张量", "原生float32[2,6,1280]。", true}};
  tensor.compute = [](const Inputs& in, const ParamView&, Outputs& out, ExecContext& ctx) {
    Tensor result;
    auto status = buildTensor(in.get("primary"), in.get("secondary"), result, ctx);
    if (!status.ok)
      return status;
    out.set("tensor", Data::tensor(std::move(result)));
    return Status::Ok();
  };
  registry.addOperator(std::move(tensor));
  auto decode = description("gap.labels_from_logits", "原生逐槽标签解码");
  decode.inputs = {{"tensor", "Tensor", "logits", "[2,类别数,1280]，并列保较小类别。", true},
                   {"primary", "Profile", "首传感器原槽", "标签来源声明。", true},
                   {"secondary", "Profile", "次传感器原槽", "标签来源声明。", true},
                   {"modelInput", "Tensor", "外部输入关联声明",
                    "可选；核对特征内容，不证明外部推理实际使用它。", false}};
  decode.outputs = {{"labels", "Bundle<gap.LabelRows.v2>", "原生标签行",
                     "大数组保留Tensor，info仅绑定指纹与完整来源。", true}};
  decode.doc =
      "外部logits与输入的关联为显式软件声明；仅locate_model直调路径使用实际同次infer输入。";
  decode.compute = [](const Inputs& in, const ParamView&, Outputs& out, ExecContext& ctx) {
    const auto* logits = in.get("tensor").asTensor();
    const auto* model = in.get("modelInput").asTensor();
    if (!logits || (in.has("modelInput") && !model))
      return bad("logits与关联输入必须为原生Tensor", "tensor");
    Data result;
    auto status =
        labelsFromLogits(*logits, in.get("primary"), in.get("secondary"), result, ctx, model);
    if (!status.ok)
      return status;
    out.set("labels", std::move(result));
    return Status::Ok();
  };
  registry.addOperator(std::move(decode));
  auto regions = description("gap.roi_from_labels", "原生标签区域提取");
  regions.inputs = {
      {"scan", "Bundle<gap.ScanPair.v2>", "场景", "模型标签sensor来源与实际量测XY映射。", true},
      {"labels", "Bundle<gap.LabelRows.v2>", "标签行", "原生Tensor[2,1280]与完整来源绑定。", true},
      {"backdrop", "PointCloud", "诊断底图", "可选，只读透传不参与推框。", false}};
  regions.outputs = {{"rois", "Bundle<gap.RoiSet.v2>", "角色框", "四框及测量空间上下文。", true},
                     {"flushBase", "Box2D", "基准面", "模型left_surface。", true},
                     {"flushBaseContext", "Record", "基准上下文", "严格v2。", true},
                     {"flushRef", "Box2D", "参考面", "模型right_surface。", true},
                     {"flushRefContext", "Record", "参考上下文", "严格v2。", true},
                     {"gapLeft", "Box2D", "左缝框", "模型left_roll。", true},
                     {"gapLeftContext", "Record", "左上下文", "严格v2。", true},
                     {"gapRight", "Box2D", "右缝框", "模型right_roll。", true},
                     {"gapRightContext", "Record", "右上下文", "严格v2。", true},
                     {"seam", "Box2D", "缝中心锚", "零尺寸方向锚。", true},
                     {"seamContext", "Record", "缝上下文", "严格v2。", true},
                     {"refinements", "Record", "细化诊断", "缺段时仍可查看。", true},
                     {"backdrop", "PointCloud", "只读底图", "没接时为空云。", true}};
  regions.params = refineParams();
  regions.compute = [](const Inputs& in, const ParamView& params, Outputs& out, ExecContext& ctx) {
    const auto backdrop = in.get("backdrop").cloudPtr();
    if (in.has("backdrop") && !backdrop)
      return bad("底图必须为PointCloud", "backdrop");
    if (ctx.cancelled())
      return cancelled();
    out.set("backdrop", backdrop ? Data::cloud(backdrop) : Data::cloud(PointCloud{}));
    ModelRegionResult result;
    auto status =
        extractModelRegions(in.get("labels"), in.get("scan"), refineOptions(params), result, ctx);
    if (!result.boxes.refinements.type.empty())
      out.set("refinements", Data::record(result.boxes.refinements));
    if (!status.ok)
      return status;
    publishRegions(result, out);
    return Status::Ok();
  };
  registry.addOperator(std::move(regions));
  auto colored = description("gap.labels_to_cloud", "原生标签诊断着色");
  colored.inputs = {
      {"scan", "Bundle<gap.ScanPair.v2>", "完整场景", "当前量测行仍需1280原槽。", true},
      {"labels", "Bundle<gap.LabelRows.v2>", "标签行", "与实际sensor两路绑定。", true}};
  colored.outputs = {
      {"cloud", "PointCloud", "诊断点云", "RGB8色与intensity类编号；不作为raw Profile。", true}};
  auto row = text("row", "primary");
  row.type = ParamType::Enum;
  row.options = {{"primary", "Primary(row0)"}, {"secondary", "Secondary(row1)"}};
  colored.params = {row};
  colored.compute = [](const Inputs& in, const ParamView& params, Outputs& out, ExecContext& ctx) {
    Data result;
    auto status =
        labelsToCloud(in.get("scan"), in.get("labels"), params.choice("row"), result, ctx);
    if (!status.ok)
      return status;
    out.set("cloud", std::move(result));
    return Status::Ok();
  };
  registry.addOperator(std::move(colored));
#ifdef LYFLOW_GAP_MODEL
  auto infer = description("gap.infer_profile", "原生剖面模型推理");
  infer.version = "1.0.0";
  infer.migrations.clear();
  infer.inputs = {{"input", "Tensor", "特征张量", "有限float32[2,6,1280]。", true}};
  infer.outputs = {{"output", "Tensor", "剖面logits", "float32[2,类别1..256,1280]。", true}};
  auto inferPath = text("modelPath");
  inferPath.type = ParamType::Path;
  inferPath.mode = "open";
  inferPath.filters = {{"ONNX", {"onnx"}}};
  infer.params = {inferPath};
  infer.doc =
      "直接复用共享CPU推理会话，固定模型原生批次与单线程归约；不声明外部物理来源或推理关联认证。";
  infer.externalKey = [](const ParamView& params) {
    return ::lyflow::ml::modelFingerprint(params.path("modelPath"));
  };
  infer.compute = [](const Inputs& in, const ParamView& params, Outputs& out, ExecContext& ctx) {
    const auto* input = in.get("input").asTensor();
    if (!input)
      return bad("剖面推理输入必须为原生Tensor", "input");
    Tensor result;
    const auto status = inferProfileTensor(*input, params.path("modelPath"), result, ctx);
    if (!status.ok)
      return status;
    out.set("output", Data::tensor(std::move(result)));
    return Status::Ok();
  };
  registry.addOperator(std::move(infer));
  auto locate = description("gap.locate_model", "原生分割模型定位");
  locate.inputs = {
      {"scan", "Bundle<gap.ScanPair.v2>", "原始场景", "只接受raw完整1280原槽。", true}};
  locate.outputs = {
      {"rois", "Bundle<gap.RoiSet.v2>", "角色框", "完整模型定位。", true},
      {"scan", "Bundle<gap.ScanPair.v2>", "定位场景", "保留raw sensor与裁剪后的量测轮廓。", true},
      {"labels", "Bundle<gap.LabelRows.v2>", "实际推理标签", "绑定实际同次输入与sensor槽。", true},
      {"refinements", "Record", "细化诊断", "完整缺段与细化状态。", true}};
  auto path = text("modelPath");
  path.type = ParamType::Path;
  path.mode = "open";
  path.filters = {{"ONNX", {"onnx"}}};
  locate.params = {path};
  auto params = refineParams();
  locate.params.insert(locate.params.end(), params.begin(), params.end());
  for (auto p :
       {flag("cropEnabled", true), mm("cropHalfWidth", 35), mm("cropHalfHeight", 20),
        mm("cropMaxRollBoxHeight", 8), integer("cropMinPointsKept", 50), camera("cropUsingCamera")})
    locate.params.push_back(std::move(p));
  locate.externalKey = [](const ParamView& params) {
    return ::lyflow::ml::modelFingerprint(params.path("modelPath"));
  };
  locate.compute = [](const Inputs& in, const ParamView& params, Outputs& out, ExecContext& ctx) {
    LocateModelOptions options;
    options.modelPath = params.path("modelPath");
    options.refine = refineOptions(params);
    options.crop = {params.flag("cropEnabled"),          params.number("cropHalfWidth"),
                    params.number("cropHalfHeight"),     params.number("cropMaxRollBoxHeight"),
                    params.integer("cropMinPointsKept"), params.choice("cropUsingCamera")};
    LocateModelResult result;
    auto status = locateModel(in.get("scan"), options, result, ctx);
    if (!result.refinements.type.empty())
      out.set("refinements", Data::record(result.refinements));
    if (!status.ok)
      return status;
    out.set("rois", std::move(result.rois));
    out.set("scan", std::move(result.scan));
    out.set("labels", std::move(result.labels));
    return Status::Ok();
  };
  registry.addOperator(std::move(locate));
#endif
}
} // namespace lyflow::packs::gap::profile_model
