#include "profile_migration.h"
#include "profile_locate_template.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "lyflow_profile/selection.h"

namespace lyflow::packs::gap::profile_template {
namespace {
using Json = nlohmann::json;
Status failure(const std::string& text, const char* code = "bad_input", const char* port = "scan") {
  return Status::Error(Phase::Execute, code, text, {}, port);
}
Status cancelled() { return failure("模板定位已取消", "cancelled"); }
Param text(const std::string& name, const std::string& label, const std::string& value) {
  Param result;
  result.name = name;
  result.label = label;
  result.type = ParamType::String;
  result.def = Value::text(value);
  return result;
}
Param choice(const std::string& name, const std::string& label, const std::string& value,
             std::vector<EnumOption> options) {
  auto result = text(name, label, value);
  result.type = ParamType::Enum;
  result.options = std::move(options);
  return result;
}
Param roi(const std::string& name, const std::string& label, std::vector<double> values) {
  auto result = text(name, label, "");
  result.type = ParamType::Vec4f;
  result.def = Value::vec(std::move(values));
  result.unit = "mm";
  result.semantic = "roi";
  result.componentLabels = {"X Min", "Y Min", "X Max", "Y Max"};
  return result;
}
Param named(const std::vector<Param>& parameters, const std::string& name) {
  const auto found = std::find_if(parameters.begin(), parameters.end(),
                                  [&](const Param& parameter) { return parameter.name == name; });
  if (found == parameters.end())
    throw std::logic_error("模板定位参数工厂缺字段：" + name);
  return *found;
}
std::vector<std::size_t> enabledSlots(const LocateOptions& options) {
  std::vector<std::size_t> slots;
  for (std::size_t i = 0; i < 4; ++i)
    if (i == 0 || options.slots[i].enabled)
      slots.push_back(i);
  return slots;
}
Json boxMm(const Box2D& box) {
  return {static_cast<double>(box.min[0]) * 1000, static_cast<double>(box.min[1]) * 1000,
          static_cast<double>(box.max[0]) * 1000, static_cast<double>(box.max[1]) * 1000};
}
}
LocateOptions locateOptions(const ParamView& p) {
  LocateOptions out;
  out.overall = {p.vec4("overallRoi"), p.choice("overallMode"), p.choice("overallCamera")};
  const auto minimum = p.integer("minScore");
  out.minScore = minimum >= 0 && minimum <= 100 ? static_cast<int>(minimum) : -1;
  const auto defaults = alignmentParameters();
  ParamMap base;
  for (const auto& parameter : defaults)
    base[parameter.name] = p.has(parameter.name) ? p.raw().at(parameter.name) : parameter.def;
  for (std::size_t i = 0; i < 4; ++i) {
    auto& slot = out.slots[i];
    const auto prefix = "template" + std::to_string(i + 1);
    slot.enabled = i == 0 || p.flag(prefix + "Enabled");
    if (!slot.enabled)
      continue;
    slot.load.dir = p.path("templateDir");
    slot.load.left = p.text(prefix + "Left");
    slot.load.right = p.text(prefix + "Right");
    slot.load.layout = p.choice("templateLayout");
    slot.load.unit = p.choice("templateUnit");
    slot.load.fileFrame = p.text("templateFileFrame");
    slot.load.templateFrame = p.text("templateFrame");
    slot.load.handedness = p.choice("templateHandedness");
    auto parameters = base;
    parameters["templateId"] = p.raw().at(prefix + "Id");
    parameters["roisFrom"] = p.raw().at(prefix + "RoisFrom");
    parameters["roiFlushBase"] = p.raw().at(prefix + "DatumRoi");
    parameters["roiFlushRef"] = p.raw().at(prefix + "TargetRoi");
    parameters["roiGapLeft"] = p.raw().at(prefix + "SeamLeftRoi");
    parameters["roiGapRight"] = p.raw().at(prefix + "SeamRightRoi");
    slot.alignment = alignmentOptions(ParamView(parameters, p.baseDir()));
  }
  return out;
}
std::vector<Param> locateParameters() {
  const auto alignment = alignmentParameters(), loading = loadParameters();
  auto dir = named(loading, "dir");
  dir.name = "templateDir";
  dir.label = "模板目录";
  auto minimum = named(alignment, "minScore");
  minimum.min = 0;
  minimum.max = 100;
  std::vector<Param> out{dir, minimum};
  auto overall = roi("overallRoi", "整体区域", {-1000, -1000, 1000, 1000});
  overall.advanced = true;
  overall.group = "整体框";
  out.push_back(overall);
  auto mode = choice("overallMode", "整体区域方式", "fixed",
                     {{"fixed", "固定框", ""},
                      {"auto_center", "自动居中", "保留配置宽高，跟随有效观测的逐坐标中位中心。"}});
  mode.advanced = true;
  mode.group = "整体框";
  out.push_back(mode);
  auto camera = choice("overallCamera", "居中参考相机", "Both",
                       {{"Both", "双路", ""}, {"Left", "primary", ""}, {"Right", "secondary", ""}});
  camera.advanced = true;
  camera.group = "整体框";
  camera.visibleWhen.param = "overallMode";
  camera.visibleWhen.eq = Value::text("auto_center");
  out.push_back(camera);
  const std::array<std::pair<const char*, const char*>, 5> loadNames{
      {{"layout", "templateLayout"},
       {"unit", "templateUnit"},
       {"fileFrame", "templateFileFrame"},
       {"templateFrame", "templateFrame"},
       {"handedness", "templateHandedness"}}};
  for (std::size_t i = 0; i < 5; ++i) {
    auto p = named(loading, loadNames[i].first);
    p.name = loadNames[i].second;
    p.advanced = true;
    p.group = "模板来源";
    out.push_back(std::move(p));
  }
  constexpr std::array<const char*, 4> roles{"Datum", "Target", "SeamLeft", "SeamRight"};
  constexpr std::array<const char*, 4> labels{"基准区域", "参考区域", "缝隙左侧区域",
                                              "缝隙右侧区域"};
  for (int k = 1; k <= 4; ++k) {
    const auto prefix = "template" + std::to_string(k), group = "模板槽 " + std::to_string(k);
    auto slotted = [&](Param p) {
      p.group = group;
      p.advanced = k > 1;
      if (k > 1 && p.name != prefix + "Enabled") {
        p.visibleWhen.param = prefix + "Enabled";
        p.visibleWhen.eq = Value::boolean(true);
      }
      return p;
    };
    if (k > 1) {
      auto enabled = text(prefix + "Enabled", "启用", "");
      enabled.type = ParamType::Bool;
      enabled.def = Value::boolean(false);
      out.push_back(slotted(enabled));
    }
    out.push_back(slotted(text(prefix + "Id", "模板标识", "f" + std::to_string(k))));
    out.push_back(
        slotted(text(prefix + "Left", "左模板文件",
                     k == 1 ? "left_template.pcd" : "f" + std::to_string(k) + "_left.pcd")));
    out.push_back(
        slotted(text(prefix + "Right", "右模板文件",
                     k == 1 ? "right_template.pcd" : "f" + std::to_string(k) + "_right.pcd")));
    for (std::size_t i = 0; i < 4; ++i) {
      auto p = roi(prefix + roles[i] + "Roi", labels[i], {0, 0, 0, 0});
      p.roiBackdrop.dirParam = "templateDir";
      p.roiBackdrop.fileParams = {prefix + "Left", prefix + "Right"};
      p.roiBackdrop.label = "模板 " + std::to_string(k);
      p.roiBackdrop.labelParam = prefix + "Id";
      out.push_back(slotted(p));
    }
    auto from = named(alignment, "roisFrom");
    from.name = prefix + "RoisFrom";
    from = slotted(from);
    from.advanced = true;
    out.push_back(std::move(from));
  }
  for (const auto* name : {"initialPoseMode", "maxMatchingDist", "maxFitnessDist", "maxIterations",
                           "normalKnn", "bidirection", "globalCoarse", "successGuide", "segRoi",
                           "trustTranslation", "trustRotation", "degenerateRatio"}) {
    auto p = named(alignment, name);
    p.group = "ICP";
    p.advanced = true;
    out.push_back(std::move(p));
  }
  return out;
}
std::vector<Issue> validateLocateOptions(const LocateOptions& options) {
  std::vector<Issue> issues;
  if (options.minScore < 0 || options.minScore > 100)
    issues.push_back(Issue::error("bad_param", "最低ICP分必须位于0到100", "minScore"));
  int firstSide = -1;
  std::string firstSlot;
  for (const auto i : enabledSlots(options)) {
    const auto prefix = "template" + std::to_string(i + 1);
    const auto& slot = options.slots[i];
    const auto label = "模板 " + std::to_string(i + 1) + " · " + slot.alignment.templateId + "：";
    constexpr std::array<std::size_t, 4> mapping{0, 2, 1, 3};
    constexpr std::array<const char*, 4> roles{"Datum", "Target", "SeamLeft", "SeamRight"};
    bool degenerate = false;
    for (std::size_t j = 0; j < 4; ++j) {
      const auto& b = slot.alignment.roisMm[mapping[j]];
      bool finite = true;
      for (const auto value : b)
        finite = finite && std::isfinite(value);
      if (!finite || !(b[2] > b[0]) || !(b[3] > b[1])) {
        issues.push_back(Issue::error("bad_param", label + roles[j] + "区域必须为有限非退化边界",
                                      prefix + roles[j] + "Roi"));
        degenerate = true;
      }
    }
    if (degenerate)
      continue;
    const auto& datum = slot.alignment.roisMm[0];
    const auto& target = slot.alignment.roisMm[2];
    const auto& left = slot.alignment.roisMm[1];
    const auto& right = slot.alignment.roisMm[3];
    if (!(left[2] <= right[0])) {
      issues.push_back(
          Issue::error("bad_param", label + "缝框左右颠倒或重叠", prefix + "SeamLeftRoi"));
      continue;
    }
    const double seamX = .5 * (.5 * (static_cast<double>(left[0]) + left[2]) +
                               .5 * (static_cast<double>(right[0]) + right[2]));
    const bool datumRight = .5 * (static_cast<double>(datum[0]) + datum[2]) > seamX,
               targetRight = .5 * (static_cast<double>(target[0]) + target[2]) > seamX;
    if (datumRight == targetRight) {
      issues.push_back(
          Issue::error("bad_param", label + "基准与参考区域必须位于缝隙两侧", prefix + "DatumRoi"));
      continue;
    }
    if (firstSide < 0) {
      firstSide = datumRight ? 1 : 0;
      firstSlot = label;
    } else if (firstSide != (datumRight ? 1 : 0))
      issues.push_back(Issue::error("bad_param", label + "基准侧与" + firstSlot + "不一致",
                                    prefix + "DatumRoi"));
  }
  return issues;
}
std::string locateFingerprint(const LocateOptions& options) {
  if (options.slots[0].load.dir.empty())
    return {};
  std::string key;
  for (const auto i : enabledSlots(options))
    key += templateFingerprint(options.slots[i].load) + ";";
  return key;
}
Status locate(const Data& input, const LocateOptions& options, LocateResult& result,
              ExecContext& context) {
  if (context.cancelled())
    return cancelled();
  profile_scene::ScanView scan;
  std::string why;
  if (!profile_scene::readScanPair(input, scan, why))
    return failure(why);
  profile_scene::GeometryResult overall;
  auto status =
      profile_scene::overallRoi(*scan.primary, *scan.secondary, options.overall, overall, context);
  if (!status.ok)
    return status;
  profile_contract::SelectionOptions selection;
  selection.mode = "compact";
  selection.bounds = "open";
  selection.emptyPolicy = "allow";
  std::array<profile_contract::SelectionResult, 3> crops;
  const Data* profiles[3] = {scan.primary, scan.secondary, scan.merged};
  for (std::size_t i = 0; i < 3; ++i) {
    status = profile_contract::cropProfile(*profiles[i], overall.box, &overall.context, selection,
                                           crops[i], context);
    if (!status.ok)
      return status;
  }
  const auto& previous = scan.info->data;
  Json crop{
      {"schemaVersion", 1},
      {"kind", "gap.OverallCrop.v1"},
      {"status", "applied"},
      {"applied", true},
      {"method", "overall"},
      {"bounds", "open"},
      {"boxMm", boxMm(*overall.box.asBox2D())},
      {"before",
       {scan.primary->asProfile()->pointCount(), scan.secondary->asProfile()->pointCount(),
        scan.merged->asProfile()->pointCount()}},
      {"after",
       {crops[0].profile.asProfile()->pointCount(), crops[1].profile.asProfile()->pointCount(),
        crops[2].profile.asProfile()->pointCount()}}};
  profile_scene::ScanMetadata metadata{previous["sensorFrame"].get<std::string>(),
                                       previous["measurementFrame"].get<std::string>(),
                                       "processed",
                                       previous["guard"],
                                       crop,
                                       previous["origins"],
                                       {{"method", "template"},
                                        {"status", "complete"},
                                        {"details", {{"overall", overall.status.data}}}}};
  LocateResult candidate;
  if (!profile_scene::makeScanPair(*scan.sensorPrimary, *scan.sensorSecondary, crops[0].profile,
                                   crops[1].profile, crops[2].profile, metadata, candidate.scan,
                                   why))
    return failure(why);
  std::vector<Data> candidates;
  std::int64_t order = 0;
  for (const auto i : enabledSlots(options)) {
    if (context.cancelled())
      return cancelled();
    const auto& slot = options.slots[i];
    Data templates, alignment;
    context.progress(static_cast<float>(i) / 4, "模板 " + slot.alignment.templateId);
    const auto annotate = [&](Status error) {
      if (error.code != "cancelled") {
        error.message = "模板 " + std::to_string(i + 1) + " · " + slot.alignment.templateId + "：" +
                        error.message;
        error.paramPath = "template" + std::to_string(i + 1) + "Id";
        error.portName.clear();
      }
      return error;
    };
    status = loadTemplatePair(slot.load, templates, context);
    if (!status.ok)
      return annotate(status);
    auto settings = slot.alignment;
    settings.order = order++;
    settings.minScore = options.minScore;
    status = align(candidate.scan, templates, settings, alignment, context);
    if (!status.ok)
      return annotate(status);
    candidates.push_back(std::move(alignment));
  }
  status = select(candidates, options.minScore, candidate.alignment, context);
  if (!status.ok)
    return status;
  profile_scene::RoiFields fields;
  Data seam, seamContext;
  status = businessRois(candidate.alignment, "auto", fields, seam, seamContext, context);
  if (!status.ok)
    return status;
  fields.info.data["overallMm"] = boxMm(*overall.box.asBox2D());
  fields.info.data["cropStatus"] = crop;
  if (!profile_scene::makeRoiSet(fields, candidate.rois, why))
    return failure(why, "invalid_result", "rois");
  if (context.cancelled())
    return cancelled();
  result = std::move(candidate);
  return Status::Ok();
}
void registerProfileLocateTemplate(Registry& registry) {
  OperatorDesc op;
  op.id = "gap.locate_template";
  op.version = "4.0.0";
  declareGapGraphUpgrade(op, 2);
  op.migrations.push_back({3, nullptr, &profile_contract::keepInputs});
  op.label = "模板区域定位";
  op.category = "间隙/场景";
  op.keywords = {"template localization", "2D ICP", "模板定位", "多模板"};
  op.doc =
      "按整体框分别裁剪双路与已有合并轮廓，再逐启用模板配准并按原候选规则选择。保留传感器原槽，输出场景、角色框和原生配准结果。";
  op.inputs = {{"scan", "Bundle<gap.ScanPair.v3>", "扫描场景", "原生轮廓及已有merged。", true}};
  op.outputs = {{"scan", "Bundle<gap.ScanPair.v3>", "定位场景", "实际整体框裁剪后的轮廓。", true},
                {"rois", "Bundle<gap.RoiSet.v3>", "角色区域", "量测空间四角色框。", true},
                {"alignment", "Bundle<gap.Alignment.v3>", "选中配准",
                 "实际应用矩阵后的模板与候选记录。", true}};
  op.params = locateParameters();
  op.capabilities = {true, false, true};
  op.externalKey =
      +[](const ParamView& params) { return locateFingerprint(locateOptions(params)); };
  op.validate = +[](const ParamView& params, const std::set<std::string>&) {
    return validateLocateOptions(locateOptions(params));
  };
  op.compute = [](const Inputs& inputs, const ParamView& params, Outputs& outputs,
                  ExecContext& ctx) {
    LocateResult result;
    const auto status = locate(inputs.get("scan"), locateOptions(params), result, ctx);
    if (status.ok) {
      outputs.set("scan", std::move(result.scan));
      outputs.set("rois", std::move(result.rois));
      outputs.set("alignment", std::move(result.alignment));
    }
    return status;
  };
  registry.addOperator(std::move(op));
}
} // namespace lyflow::packs::gap::profile_template
