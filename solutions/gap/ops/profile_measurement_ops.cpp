#include "profile_migration.h"
#include "profile_measurement_ops.h"

#include <algorithm>
#include <string>

#include "lyflow_profile/features.h"
#include "lyflow_profile/selection.h"
#include "profile_bundle_ops.h"
#include "profile_circles.h"
#include "profile_feature_bundle.h"
#include "profile_regions.h"

namespace lyflow::packs::gap::profile_scene {
namespace {
namespace contract = profile_contract;
using Json = nlohmann::json;
Status bad(const std::string& message, const char* port = "scan") {
  return Status::Error(Phase::Execute, "bad_input", message, {}, port);
}
Status cancelled() { return Status::Error(Phase::Execute, "cancelled", "场景特征计算已取消"); }
Status read(const Inputs& inputs, ScanView& scan, RoiView& rois) {
  std::string why;
  if (!readScanPair(inputs.get("scan"), scan, why))
    return bad(why);
  if (!readRoiSet(inputs.get("rois"), rois, why))
    return bad(why, "rois");
  return Status::Ok();
}
Param choice(const char* name, const char* initial, std::initializer_list<const char*> values) {
  Param p;
  p.name = name;
  p.label = name;
  p.type = ParamType::Enum;
  p.def = Value::text(initial);
  for (const auto* value : values)
    p.options.push_back({value, value});
  return p;
}
std::vector<Param> lineParameters(bool direction = true) {
  auto result = contract::robustLineParams();
  for (auto& p : result) {
    if (p.name == "distanceThreshold") {
      p.name = "distThresh";
      p.unit = "mm";
      p.def = Value::number(.1);
    }
    p.advanced = p.name != "distThresh" && p.name != "segmentPoints";
  }
  if (!direction)
    result.erase(std::remove_if(result.begin(), result.end(),
                                [](const Param& p) {
                                  return p.name == "dirMode" || p.name == "dirNominalDeg" ||
                                         p.name == "dirTolDeg";
                                }),
                 result.end());
  return result;
}
ParamMap lineValues(const ParamView& params) {
  ParamMap values;
  for (const auto& p : contract::robustLineParams()) {
    if (p.name == "distanceThreshold")
      values[p.name] = Value::number(params.number("distThresh") / 1000.0);
    else
      values[p.name] = params.has(p.name) ? params.raw().at(p.name) : p.def;
  }
  return values;
}
const Record* provenance(const ScanView& scan, const std::string& role) {
  return role == "primary"     ? &scan.primaryProvenance
         : role == "secondary" ? &scan.secondaryProvenance
         : role == "merged"    ? scan.mergedProvenance
                               : nullptr;
}
Status crop(const Data& input, const Record* origin, const Data& box, const Data& context,
            contract::SelectionResult& result, ExecContext& ctx) {
  contract::SelectionOptions options;
  options.bounds = "open";
  options.mode = "compact";
  options.emptyPolicy = "error";
  return contract::cropProfile(input, box, *context.asRecord(), origin, options, result, ctx);
}
Status fitRole(const ScanView& scan, const RoiView& rois, const ParamView& params,
               const Data& refLine, const Data& refContext, contract::RobustLineResult& result,
               ExecContext& ctx) {
  const auto* box = rois.role(params.choice("role"));
  const auto* context = rois.context(params.choice("role"));
  const auto* profile = scan.pick(params.choice("cloud"));
  contract::SelectionResult selected;
  if (const auto status =
          crop(*profile, provenance(scan, params.choice("cloud")), *box, *context, selected, ctx);
      !status.ok)
    return status;
  Data toward;
  Record towardContext;
  if (const auto status = seamTowardGeometry(rois, toward, towardContext, ctx); !status.ok)
    return status;
  contract::RobustLineInputs input{selected.profile,
                                   *box,
                                   *context,
                                   toward,
                                   Data::record(std::move(towardContext)),
                                   refLine,
                                   refContext,
                                   Data::record(std::move(selected.provenance))};
  const auto values = lineValues(params);
  return contract::fitRobustLine(input, ParamView(values, params.baseDir()), result, ctx);
}
void publishFit(const contract::RobustLineResult& fit, Outputs& out, bool point = false) {
  out.set("profile", fit.profile);
  out.set("line", fit.line);
  out.set("lineContext", fit.lineContext);
  out.set(point ? "point" : "innerEnd", fit.innerEnd);
  out.set("pointContext", fit.pointContext);
  out.set("quality", fit.quality);
  if (!point) {
    out.set("inliers", fit.inliers);
    out.set("weightedInliers", fit.weightedInliers);
  }
  out.set("provenance", fit.provenance);
}
bool lineFeatureOf(const contract::RobustLineResult& fit, Data& feature, std::string& why) {
  return makeLineFeature({fit.profile, fit.line, fit.lineContext, fit.innerEnd, fit.pointContext,
                          fit.inliers, fit.weightedInliers, fit.quality, fit.provenance},
                         feature, why);
}
Status publishReference(const std::string& method, const ReferenceFeatureFields& partial,
                        Outputs& out, ExecContext& ctx) {
  ReferenceFeatureFields fields;
  Data feature;
  std::string why;
  if (!completeReferenceFeature(method, partial, fields, why) ||
      !makeReferenceFeature(fields, feature, why))
    return bad(why, "feature");
  if (ctx.cancelled())
    return cancelled();
  out.set("profile", fields.profile);
  out.set("line", fields.line);
  out.set("lineContext", fields.lineContext);
  out.set("point", fields.point);
  out.set("pointContext", fields.pointContext);
  out.set("selection", fields.selection);
  out.set("quality", fields.quality);
  out.set("provenance", fields.provenance);
  out.set("feature", std::move(feature));
  return Status::Ok();
}
Status roleLine(const Inputs& in, const ParamView& params, Outputs& out, ExecContext& ctx) {
  ScanView scan;
  RoiView rois;
  if (const auto status = read(in, scan, rois); !status.ok)
    return status;
  contract::RobustLineResult fit;
  if (const auto status =
          fitRole(scan, rois, params, in.get("refLine"), in.get("refLineContext"), fit, ctx);
      !status.ok)
    return status;
  Data feature;
  std::string why;
  if (!lineFeatureOf(fit, feature, why))
    return bad(why, "feature");
  if (ctx.cancelled())
    return cancelled();
  publishFit(fit, out);
  out.set("feature", std::move(feature));
  return Status::Ok();
}
Status refPoint(const Inputs& in, const ParamView& params, Outputs& out, ExecContext& ctx) {
  ScanView scan;
  RoiView rois;
  if (const auto status = read(in, scan, rois); !status.ok)
    return status;
  const auto method = params.choice("method");
  if (method == "line_end") {
    contract::RobustLineResult fit;
    if (const auto status = fitRole(scan, rois, params, {}, {}, fit, ctx); !status.ok)
      return status;
    return publishReference(method,
                            {fit.profile, fit.line, fit.lineContext, fit.innerEnd, fit.pointContext,
                             fit.indexMap, fit.quality, fit.provenance},
                            out, ctx);
  }
  const auto* boxData = rois.role(params.choice("role"));
  const auto* boxContext = rois.context(params.choice("role"));
  const auto* input = scan.pick(params.choice("cloud"));
  contract::NearestInputs nearest;
  nearest.profile = *input;
  nearest.provenance = Data::record(*provenance(scan, params.choice("cloud")));
  contract::NearestReference kind;
  if (method == "selected_point") {
    Point2D anchor;
    std::copy_n(boxData->asBox2D()->min, 2, anchor.p);
    nearest.reference = Data::point2d(anchor);
    ProfileDescriptor descriptor;
    Json sources;
    std::string why;
    if (!contract::geometryMatchesPhysical(*boxData, boxContext->asRecord(), descriptor, why,
                                           &sources))
      return bad(why, "rois");
    nearest.referenceContext = Data::record(Record{
        "profile.Geometry.v2", contract::geometryContextV2FromSources(nearest.reference, sources)});
    kind = contract::NearestReference::Point;
  } else {
    contract::SelectionResult selected;
    if (const auto status = crop(*input, provenance(scan, params.choice("cloud")), *boxData,
                                 *boxContext, selected, ctx);
        !status.ok)
      return status;
    nearest.profile = selected.profile;
    nearest.provenance = Data::record(std::move(selected.provenance));
    nearest.reference = in.get("baseLine");
    nearest.referenceContext = in.get("baseLineContext");
    kind = contract::NearestReference::Line;
  }
  contract::NearestResult point;
  if (const auto status = contract::nearestSample(nearest, kind, point, ctx); !status.ok)
    return status;
  return publishReference(method,
                          {point.profile,
                           {},
                           {},
                           point.point,
                           point.pointContext,
                           point.selection,
                           {},
                           point.provenance},
                          out, ctx);
}
Status datumDirection(const Inputs& in, const ParamView& params, Outputs& out, ExecContext& ctx) {
  ScanView scan;
  RoiView rois;
  if (const auto status = read(in, scan, rois); !status.ok)
    return status;
  const auto role = params.choice("role");
  const auto* height = rois.role(role);
  const auto* heightContext = rois.context(role);
  const bool datumRight = rois.info->data["datumSide"] == "right";
  const bool right = role == "target" ? !datumRight : datumRight;
  const std::size_t side = right ? 3 : 2;
  GeometryResult window;
  if (const auto status = datumWindow(*rois.boxes[side], *rois.contexts[side]->asRecord(), height,
                                      heightContext->asRecord(),
                                      {right ? "right" : "left", params.number("startMm"),
                                       params.number("lengthMm"), params.number("heightMm")},
                                      window, ctx);
      !status.ok)
    return status;
  contract::SelectionResult selected;
  if (const auto status = crop(*scan.merged, scan.mergedProvenance, window.box,
                               Data::record(window.context), selected, ctx);
      !status.ok)
    return status;
  auto values = lineValues(params);
  values["segmentPoints"] = Value::integer(0);
  values["endpoints"] = Value::text("inlier_ends");
  contract::RobustLineInputs input{selected.profile,
                                   window.box,
                                   Data::record(window.context),
                                   *rois.boxes[side],
                                   *rois.contexts[side],
                                   {},
                                   {},
                                   Data::record(std::move(selected.provenance))};
  contract::RobustLineResult fit;
  if (const auto status =
          contract::fitRobustLine(input, ParamView(values, params.baseDir()), fit, ctx);
      !status.ok)
    return status;
  Data feature;
  std::string why;
  if (!lineFeatureOf(fit, feature, why))
    return bad(why, "feature");
  if (ctx.cancelled())
    return cancelled();
  publishFit(fit, out);
  out.set("feature", std::move(feature));
  out.set("window", window.box);
  out.set("windowContext", Data::record(std::move(window.context)));
  return Status::Ok();
}
Status seamCircles(const Inputs& in, const ParamView& params, Outputs& out, ExecContext& ctx) {
  ScanView scan;
  RoiView rois;
  if (const auto status = read(in, scan, rois); !status.ok)
    return status;
  CirclePairInputs input{*scan.merged,
                         *scan.primary,
                         *scan.secondary,
                         Data::record(*scan.mergedProvenance),
                         Data::record(scan.primaryProvenance),
                         Data::record(scan.secondaryProvenance),
                         {*rois.boxes[2], *rois.boxes[3]},
                         {*rois.contexts[2], *rois.contexts[3]},
                         in.get("refLine"),
                         in.get("refLineContext"),
                         in.get("refLineRight"),
                         in.get("refLineRightContext")};
  CirclePairResult result;
  if (const auto status = fitProfileGapCircles(input, params, result, ctx); !status.ok)
    return status;
  Data feature;
  std::string why;
  if (!makeCirclePairFeature({result.profiles, result.circles, result.contexts, result.provenances,
                              result.inliers, result.quality},
                             feature, why))
    return bad(why, "feature");
  if (ctx.cancelled())
    return cancelled();
  for (std::size_t i = 0; i < 2; ++i) {
    const std::string side = i ? "right" : "left";
    out.set(side, result.circles[i]);
    out.set(side + "Context", result.contexts[i]);
    out.set(side + "Profile", result.profiles[i]);
    out.set(side + "Provenance", result.provenances[i]);
    out.set(side + "Inliers", result.inliers[i]);
  }
  out.set("quality", Data::record(std::move(result.quality)));
  out.set("feature", std::move(feature));
  return Status::Ok();
}
std::vector<Port> lineOutputs(bool point) {
  std::vector<Port> ports{
      {"feature", point ? "Bundle<gap.ReferenceFeature.v2>" : "Bundle<gap.LineFeature.v2>",
       "原子特征", "几何、上下文、观测和质量随同一fallback选择。", true},
      {"line", "Line2D", "拟合线", "仅参考点非拟合方法输出声明占位线。", true},
      {"lineContext", "Record", "线上下文", "与实际几何内容绑定。", true},
      {point ? "point" : "innerEnd", "Point2D", "近端点", "原生观测样本。", true},
      {"pointContext", "Record", "点上下文", "与点成对。", true},
      {"profile", "Profile", "观测轮廓", "保留原槽和通道。", true},
      {"provenance", "Record", "观测来源", "当前轮廓的完整来源。", true},
      {"quality", "Record", "质量", "显式区分拟合与选点。", true}};
  if (point)
    ports.push_back(
        {"selection", "Record", "提取记录", "真实拟合或选点过程，明确区分方法。", true});
  if (!point) {
    ports.push_back({"inliers", "Indices", "唯一内点", "当前观测轮廓的真实槽。", true});
    ports.push_back({"weightedInliers", "Indices", "内点权重", "保留多重内点权重。", true});
  }
  return ports;
}

std::vector<Issue> validateLine(const ParamView& params, const std::set<std::string>& connected) {
  std::vector<Issue> issues;
  const bool line = connected.count("refLine") != 0;
  const bool context = connected.count("refLineContext") != 0;
  if (line != context || (params.choice("dirMode") != "free" && !line))
    issues.push_back(Issue::error("missing_input", "方向约束必须提供参考线及其几何上下文",
                                  "dirMode", !line ? "refLine" : "refLineContext"));
  return issues;
}
std::vector<Issue> validatePoint(const ParamView& params, const std::set<std::string>& connected) {
  std::vector<Issue> issues;
  const bool line = connected.count("baseLine") != 0;
  const bool context = connected.count("baseLineContext") != 0;
  if (line != context || (params.choice("method") == "nearest_point" && !line))
    issues.push_back(Issue::error("missing_input", "最近点方法必须提供基准线及其几何上下文",
                                  "method", !line ? "baseLine" : "baseLineContext"));
  return issues;
}
}
void registerProfileMeasurementOps(Registry& registry) {
  for (const auto& kind : {std::string("role_line"), std::string("ref_point"),
                           std::string("datum_direction"), std::string("seam_circles")}) {
    OperatorDesc op;
    op.id = "gap." + kind;
    op.version = "2.0.0";
    declareGapGraphUpgrade(op);
    op.label = kind == "role_line"         ? "角色区域直线拟合"
               : kind == "ref_point"       ? "参考点提取"
               : kind == "datum_direction" ? "方向基准线提取"
                                           : "缝隙双侧圆拟合";
    op.category = "间隙/积木";
    op.keywords = {"profile", "datum", "reference", "circle", "角色", "基准", "参考", "圆弧"};
    op.doc = "复用公共轮廓特征与严格场景来源；不通过其他算子的注册表运行计算。";
    op.inputs = {{"scan", "Bundle<gap.ScanPair.v2>", "轮廓对", "实际输入及原始传感器槽。", true},
                 {"rois", "Bundle<gap.RoiSet.v2>", "角色区域", "绑定量测几何及全部来源。", true}};
    op.capabilities = {false, false, true};
    if (kind == "seam_circles") {
      op.params = circlePairParams();
      op.validate = &validateCirclePairParams;
      for (const auto* name : {"refLine", "refLineRight"}) {
        op.inputs.push_back({name, "Line2D", name, "可选圆心约束参考线。", false});
        op.inputs.push_back(
            {std::string(name) + "Context", "Record", "参考来源", "与线成对。", false});
      }
      for (const auto* name : {"left", "right"}) {
        op.outputs.push_back({name, "Circle2D", name, "实际选定圆。", true});
        op.outputs.push_back(
            {std::string(name) + "Context", "Record", "圆来源", "完整上下文。", true});
        op.outputs.push_back(
            {std::string(name) + "Profile", "Profile", "观测轮廓", "真实原槽。", true});
        op.outputs.push_back(
            {std::string(name) + "Provenance", "Record", "轮廓来源", "完整绑定。", true});
        op.outputs.push_back(
            {std::string(name) + "Inliers", "Indices", "观测内点", "实际轮廓索引。", true});
      }
      op.outputs.push_back({"quality", "Record", "质量", "双侧实际候选选择。", true});
      op.outputs.push_back({"feature", "Bundle<gap.CirclePair.v2>", "原子双圆特征",
                            "双侧几何、来源与质量同时选择。", true});
      op.compute = &seamCircles;
    } else {
      op.params = lineParameters(kind != "ref_point");
      op.params.insert(op.params.begin(), choice("role", kind == "ref_point" ? "target" : "datum",
                                                 {"datum", "target"}));
      op.outputs = lineOutputs(kind == "ref_point");
      if (kind == "datum_direction") {
        op.params.erase(std::remove_if(op.params.begin(), op.params.end(),
                                       [](const Param& p) {
                                         return p.name != "role" && p.name != "distThresh" &&
                                                p.name != "minInliers";
                                       }),
                        op.params.end());
        for (auto& p : op.params) {
          if (p.name == "distThresh")
            p.def = Value::number(.35);
          if (p.name == "minInliers")
            p.def = Value::integer(60);
          if (p.name == "segmentPoints")
            p.def = Value::integer(0);
          if (p.name == "endpoints")
            p.def = Value::text("inlier_ends");
        }
        for (const auto& field : {std::make_pair("startMm", .6), std::make_pair("lengthMm", 13.4),
                                  std::make_pair("heightMm", 2.5)}) {
          Param p;
          p.name = field.first;
          p.label = field.first;
          p.type = ParamType::Float;
          p.unit = "mm";
          p.def = Value::number(field.second);
          op.params.push_back(std::move(p));
        }
        op.outputs.push_back({"window", "Box2D", "方向窗口", "真实长面范围。", true});
        op.outputs.push_back({"windowContext", "Record", "窗口来源", "双锚来源。", true});
        op.compute = &datumDirection;
      } else {
        op.params.push_back(choice("cloud", "merged", {"merged", "primary", "secondary"}));
        if (kind == "ref_point") {
          op.params.insert(
              op.params.begin(),
              choice("method", "line_end", {"line_end", "selected_point", "nearest_point"}));
          op.inputs.push_back({"baseLine", "Line2D", "基准线", "nearest_point时必需。", false});
          op.inputs.push_back({"baseLineContext", "Record", "基准线来源", "与基准线成对。", false});
          op.compute = &refPoint;
          op.validate = &validatePoint;
        } else {
          op.inputs.push_back({"refLine", "Line2D", "参考线", "方向约束时必需。", false});
          op.inputs.push_back({"refLineContext", "Record", "参考线来源", "与线成对。", false});
          op.compute = &roleLine;
          op.validate = &validateLine;
        }
      }
    }
    registry.addOperator(std::move(op));
  }
}
}
