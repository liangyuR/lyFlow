#include "profile_migration.h"
#include "profile_inspection.h"

#include <algorithm>
#include <limits>

#include "lyflow/profile.h"
#include "lyflow_profile/provenance.h"

namespace lyflow::packs::gap::profile_scene {
namespace {
using Json = nlohmann::json;
Status bad(const std::string& message, const char* port = "primary",
           const char* code = "bad_input") {
  return Status::Error(Phase::Execute, code, message, {}, port);
}
Status cancelled() { return bad("场景轮廓测量已取消", "primary", "cancelled"); }
bool present(const Data& value) { return value.kind() != Data::Kind::None; }
bool join(Json& into, const Json& from, std::string& why) {
  for (const auto& source : from)
    if (std::find(into.begin(), into.end(), source) == into.end())
      into.push_back(source);
  if (into.size() > 64) {
    why = "场景几何来源超过64项预算";
    return false;
  }
  return true;
}
Status parametersValid(const ParamView& parameters, const std::vector<Param>& descriptions) {
  for (const auto& parameter : descriptions) {
    if (parameter.type != ParamType::Int)
      continue;
    const double number = parameters.number(parameter.name);
    if (number > std::numeric_limits<int>::max() || number < std::numeric_limits<int>::min())
      return Status::Error(Phase::Execute, "bad_param", "场景计数必须在int范围内", parameter.name);
  }
  if (parameters.has("lineDistThresh") && !(parameters.number("lineDistThresh") > 0))
    return Status::Error(Phase::Execute, "bad_param", "拟合门限必须为正", "lineDistThresh");
  for (const auto* name : {"levelDepth", "gapDepth", "grooveDepthMin"})
    if (parameters.has(name) && parameters.number(name) < 0)
      return Status::Error(Phase::Execute, "bad_param", "深度必须非负", name);
  for (const auto* prefix : {"left", "right", "base", "ref"}) {
    const auto nearName = std::string(prefix) + "Near";
    const auto farName = std::string(prefix) + "Far";
    if (parameters.has(nearName) && (parameters.number(nearName) < 0 ||
                                     parameters.number(farName) <= parameters.number(nearName)))
      return Status::Error(Phase::Execute, "bad_param", "窗口需要0≤near<far", farName);
  }
  return Status::Ok();
}
Status physical(const Data& geometry, const Data& context, const ProfileDescriptor* expected,
                Json& sources, ProfileDescriptor& descriptor, const char* port) {
  Json parents;
  std::string why;
  const auto* record = context.asRecord();
  if (!record || record->type != "profile.Geometry.v2" ||
      !profile_contract::geometryMatchesPhysical(geometry, record, descriptor, why, &parents))
    return bad(why.empty() ? "场景几何需要严格v2内容绑定上下文" : why, port);
  if (expected && !profile_contract::sameSpace(*expected, descriptor))
    return bad("场景几何的坐标帧、分析平面或手性不一致", port);
  if (!join(sources, parents, why))
    return bad(why, port);
  return Status::Ok();
}
Data contextFor(const Data& geometry, const Json& sources) {
  return Data::record(Record{"profile.Geometry.v2",
                             profile_contract::geometryContextV2FromSources(geometry, sources)});
}
Data unavailable(const Json& sources, const char* geometry) {
  return Data::record(Record{"gap.GeometryUnavailable.v1",
                             {{"schemaVersion", 1},
                              {"available", false},
                              {"code", "reference_fit_failed"},
                              {"message", "对面翼面拟合失败，显示载荷不能参与几何测量"},
                              {"geometryPort", geometry},
                              {"sources", sources}}});
}
Status workCloud(const Profile& input, PointCloud& output, ExecContext& ctx) {
  Profile metric;
  std::string why;
  if (!convertProfileUnit(input, "m", metric, why, [&] { return ctx.cancelled(); }))
    return ctx.cancelled() ? cancelled() : bad(why);
  const int a = input.descriptor.plane == "YZ" ? 1 : 0;
  const int b = input.descriptor.plane == "XY" ? 1 : 2;
  PointCloud cloud;
  cloud.reserve(metric.pointCount());
  for (std::size_t i = 0; i < metric.pointCount(); ++i) {
    if ((i & 1023U) == 0 && ctx.cancelled())
      return cancelled();
    if (metric.validity[i])
      cloud.push(metric.cloud->xyz[3 * i + a], metric.cloud->xyz[3 * i + b], 0);
  }
  output = std::move(cloud);
  return Status::Ok();
}
Status measureJoint(const JointInputs& input, const ParamView& parameters, JointResult& out,
                    ExecContext& ctx, bool notch) {
  if (ctx.cancelled())
    return cancelled();
  if (auto status =
          parametersValid(parameters, notch ? notchInspectionParams() : grooveInspectionParams());
      !status.ok)
    return status;
  const Data* profiles[]{&input.primary, &input.secondary};
  const Data* records[]{&input.primaryProvenance, &input.secondaryProvenance};
  const char* names[]{"primary", "secondary"};
  Json sources = Json::array();
  Json actualSources = Json::object();
  std::unordered_map<std::string, Data> cloudInputs;
  std::string why;
  for (int i = 0; i < 2; ++i) {
    const auto* profile = profiles[i]->asProfile();
    if (i &&
        !profile_contract::sameSpace(input.primary.asProfile()->descriptor, profile->descriptor))
      return bad("两路轮廓必须处于相同物理空间", names[i]);
    if (profile->pointCount() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
      return bad("轮廓超过拟合索引预算", names[i], "budget_exceeded");
    Json parents;
    const auto* provenance = records[i]->asRecord();
    if (!profile_contract::profileSources(*profiles[i], provenance, parents, why) ||
        !join(sources, parents, why))
      return bad(why.empty() ? "轮廓来源必须为绑定当前Profile的Record" : why, names[i]);
    Record bound;
    if (!profile_contract::rebindProvenance(*profiles[i], *profiles[i], provenance, bound, why))
      return bad(why, names[i]);
    actualSources[names[i]] = Json::array();
    for (const auto& segment : bound.data.at("recipe").at("segments")) {
      const auto& identity = segment.at("sourceContext").at("descriptor").at("sourceId");
      auto& identities = actualSources[names[i]];
      if (std::find(identities.begin(), identities.end(), identity) == identities.end())
        identities.push_back(identity);
    }
    PointCloud cloud;
    if (auto status = workCloud(*profile, cloud, ctx); !status.ok)
      return status;
    cloudInputs.emplace(names[i], Data::cloud(std::move(cloud)));
  }
  std::unordered_map<std::string, Data> values;
  Outputs pending(values);
  const auto status =
      notch ? inspection_detail::notchClouds(Inputs(cloudInputs), parameters, pending, ctx)
            : inspection_detail::grooveClouds(Inputs(cloudInputs), parameters, pending, ctx);
  if (!status.ok)
    return status;
  if (ctx.cancelled())
    return cancelled();
  JointResult result;
  result.gap = values.at("gap");
  result.flush = values.at("flush");
  result.baseLine = values.at("baseLine");
  result.refLine = values.at("refLine");
  result.gapSegment = values.at("gapSegment");
  result.flushSegment = values.at("flushSegment");
  result.feature = values.at(notch ? "anchor" : "groove");
  result.quality = *values.at("quality").asRecord();
  result.qualityBase = *values.at("qualityBase").asRecord();
  result.qualityRef = *values.at("qualityRef").asRecord();
  const bool referenceAvailable = !notch || result.quality.data.at("flushCameras").get<int>() > 0;
  try {
    result.baseLineContext = contextFor(result.baseLine, sources);
    result.gapSegmentContext = contextFor(result.gapSegment, sources);
    result.pointContext = contextFor(result.feature, sources);
    result.refLineContext =
        referenceAvailable ? contextFor(result.refLine, sources) : unavailable(sources, "refLine");
    result.flushSegmentContext = referenceAvailable ? contextFor(result.flushSegment, sources)
                                                    : unavailable(sources, "flushSegment");
  } catch (const std::invalid_argument& error) {
    return bad(error.what(), "primary", "invalid_geometry");
  }
  result.quality.type = notch ? "gap.NotchQuality.v2" : "gap.GrooveQuality.v2";
  result.quality.data["schemaVersion"] = 2;
  result.quality.data["refGeometryAvailable"] = referenceAvailable;
  result.quality.data["inputContexts"] = {profile_contract::context(input.primary),
                                          profile_contract::context(input.secondary)};
  result.quality.data["sources"] = sources;
  result.quality.data["actualSourceIds"] = actualSources;
  if (notch) {
    const auto selected = parameters.choice("camera");
    const auto& cameras = result.quality.data.at("cameras");
    const char* shown = selected != "secondary" && cameras.at("primary").at("valid").get<bool>()
                            ? "primary"
                            : "secondary";
    result.quality.data["displayRole"] = shown;
    result.quality.data["displaySourceIds"] = actualSources.at(shown);
  } else {
    result.quality.data["displayRole"] = "averaged_valid_cameras";
  }
  result.qualityBase.type = "gap.FitQuality.v2";
  result.qualityRef.type = "gap.FitQuality.v2";
  result.qualityBase.data["sources"] = sources;
  result.qualityRef.data["sources"] = sources;
  result.qualityRef.data["geometryAvailable"] = referenceAvailable;
  if (ctx.cancelled())
    return cancelled();
  out = std::move(result);
  return Status::Ok();
}
void publishJoint(const JointResult& result, Outputs& outputs, bool notch) {
  outputs.set("gap", result.gap);
  outputs.set("flush", result.flush);
  outputs.set("baseLine", result.baseLine);
  outputs.set("refLine", result.refLine);
  outputs.set("gapSegment", result.gapSegment);
  outputs.set("flushSegment", result.flushSegment);
  outputs.set(notch ? "anchor" : "groove", result.feature);
  outputs.set("baseLineContext", result.baseLineContext);
  outputs.set("refLineContext", result.refLineContext);
  outputs.set("gapSegmentContext", result.gapSegmentContext);
  outputs.set("flushSegmentContext", result.flushSegmentContext);
  outputs.set("pointContext", result.pointContext);
  outputs.set("quality", Data::record(result.quality));
  outputs.set("qualityBase", Data::record(result.qualityBase));
  outputs.set("qualityRef", Data::record(result.qualityRef));
}
}
Status measureProfileGroove(const JointInputs& input, const ParamView& parameters, JointResult& out,
                            ExecContext& ctx) {
  return measureJoint(input, parameters, out, ctx, false);
}
Status measureProfileNotch(const JointInputs& input, const ParamView& parameters, JointResult& out,
                           ExecContext& ctx) {
  return measureJoint(input, parameters, out, ctx, true);
}
Status measureProfileCorner(const CornerInputs& input, const ParamView& parameters,
                            CornerResult& out, ExecContext& ctx) {
  if (ctx.cancelled())
    return cancelled();
  if (auto status = parametersValid(parameters, cornerInspectionParams()); !status.ok)
    return status;
  Json sources = Json::array();
  ProfileDescriptor space, current;
  if (auto status = physical(input.lineLeft, input.lineLeftContext, nullptr, sources, space,
                             "lineLeftContext");
      !status.ok)
    return status;
  if (auto status = physical(input.lineRight, input.lineRightContext, &space, sources, current,
                             "lineRightContext");
      !status.ok)
    return status;
  if (auto status =
          physical(input.origin, input.originContext, &space, sources, current, "originContext");
      !status.ok)
    return status;
  std::unordered_map<std::string, Data> valuesIn{{"lineLeft", input.lineLeft},
                                                 {"lineRight", input.lineRight}};
  if (present(input.baseLine) != present(input.baseLineContext))
    return bad("基准线与上下文须成对提供", "baseLineContext");
  if (present(input.baseLine)) {
    if (auto status = physical(input.baseLine, input.baseLineContext, &space, sources, current,
                               "baseLineContext");
        !status.ok)
      return status;
    valuesIn.emplace("baseLine", input.baseLine);
  }
  if (parameters.number("minAngle") < 0 || parameters.number("maxAngle") > 180 ||
      parameters.number("minAngle") > parameters.number("maxAngle"))
    return Status::Error(Phase::Execute, "bad_param", "夹角范围必须满足0≤minAngle≤maxAngle≤180",
                         "minAngle");
  std::unordered_map<std::string, Data> values;
  Outputs pending(values);
  if (auto status = inspection_detail::cornerGeometry(Inputs(valuesIn), parameters, pending, ctx,
                                                      input.origin.asPoint2D());
      !status.ok)
    return status;
  CornerResult result{values.at("vertex"),
                      {},
                      values.at("gap"),
                      values.at("flush"),
                      values.at("angle"),
                      values.at("segment"),
                      {},
                      *values.at("quality").asRecord()};
  try {
    result.vertexContext = contextFor(result.vertex, sources);
    result.segmentContext = contextFor(result.segment, sources);
  } catch (const std::invalid_argument& error) {
    return bad(error.what(), "lineLeft", "invalid_geometry");
  }
  result.quality.type = "gap.CornerQuality.v2";
  result.quality.data["schemaVersion"] = 2;
  result.quality.data["sources"] = sources;
  result.quality.data["originMode"] = "explicit_metric_point";
  if (ctx.cancelled())
    return cancelled();
  out = std::move(result);
  return Status::Ok();
}
void registerProfileInspection(Registry& registry) {
  OperatorDesc corner;
  corner.id = "gap.corner_vertex";
  corner.version = "2.0.0";
  declareGapGraphUpgrade(corner);
  corner.label = "原生轮廓夹角接缝测量";
  corner.category = "间隙/场景";
  corner.keywords = {"corner joint", "vertex", "夹角接缝", "虚拟顶点"};
  corner.doc = "两条翼面线的交点相对显式米制origin的投影位移；所有几何必须有同空间严格v2上下文。";
  corner.inputs = {{"lineLeft", "Line2D", "左翼面", "米制几何。", true},
                   {"lineLeftContext", "Record", "左线上下文", "profile.Geometry.v2。", true},
                   {"lineRight", "Line2D", "右翼面", "米制几何。", true},
                   {"lineRightContext", "Record", "右线上下文", "profile.Geometry.v2。", true},
                   {"baseLine", "Line2D", "基准方向", "缺省采用左线方向。", false},
                   {"baseLineContext", "Record", "基准上下文", "须与baseLine成对。", false},
                   {"origin", "Point2D", "参照原点", "已在测量空间中的米制原点。", true},
                   {"originContext", "Record", "原点上下文", "profile.Geometry.v2。", true}};
  corner.outputs = {{"vertex", "Point2D", "虚拟顶点", "米制交点。", true},
                    {"vertexContext", "Record", "顶点上下文", "profile.Geometry.v2。", true},
                    {"gap", "Measurement", "间隙", "毫米。", true},
                    {"flush", "Measurement", "段差", "毫米。", true},
                    {"angle", "Measurement", "夹角", "度。", true},
                    {"segment", "Line2D", "位移段", "米制origin到vertex。", true},
                    {"segmentContext", "Record", "位移段上下文", "profile.Geometry.v2。", true},
                    {"quality", "Record", "质量", "gap.CornerQuality.v2。", true}};
  corner.params = cornerInspectionParams();
  corner.capabilities = {false, true, true};
  corner.compute = [](const Inputs& in, const ParamView& params, Outputs& outputs,
                      ExecContext& ctx) {
    CornerInputs input{in.get("lineLeft"),  in.get("lineLeftContext"),
                       in.get("lineRight"), in.get("lineRightContext"),
                       in.get("baseLine"),  in.get("baseLineContext"),
                       in.get("origin"),    in.get("originContext")};
    CornerResult result;
    if (auto status = measureProfileCorner(input, params, result, ctx); !status.ok)
      return status;
    if (ctx.cancelled())
      return cancelled();
    outputs.set("vertex", result.vertex);
    outputs.set("vertexContext", result.vertexContext);
    outputs.set("gap", result.gap);
    outputs.set("flush", result.flush);
    outputs.set("angle", result.angle);
    outputs.set("segment", result.segment);
    outputs.set("segmentContext", result.segmentContext);
    outputs.set("quality", Data::record(result.quality));
    return Status::Ok();
  };
  registry.addOperator(std::move(corner));
  for (bool notch : {false, true}) {
    OperatorDesc op;
    op.id = notch ? "gap.notch_width" : "gap.groove_joint";
    op.version = "2.0.0";
    declareGapGraphUpgrade(op);
    op.label = notch ? "原生轮廓V形缝测量" : "原生轮廓槽形对接缝测量";
    op.category = "间隙/场景";
    op.keywords = {"profile", notch ? "notch width" : "groove joint", "轮廓", "接缝测量"};
    op.doc =
        "按Profile的m/mm和分析平面转换工作视图，只使用有效槽；保留逐相机拟合和场景窗口策略。"
        "全部几何带严格v2来源。参考翼面不可用时保留显示载荷，但上下文为gap.GeometryUnavailable.v1，不能用于几何测量。";
    op.inputs = {{"primary", "Profile", "主路轮廓", "显式单位、空间与有效槽。", true},
                 {"secondary", "Profile", "辅路轮廓", "与主路处于相同物理空间。", true},
                 {"primaryProvenance", "Record", "主路来源", "可选严格绑定来源。", false},
                 {"secondaryProvenance", "Record", "辅路来源", "可选严格绑定来源。", false}};
    op.outputs = {{"gap", "Measurement", "间隙", "毫米。", true},
                  {"flush", "Measurement", "段差", "毫米；参考拟合失败时ok=false。", true},
                  {"baseLine", "Line2D", "基准线", "米制。", true},
                  {"refLine", "Line2D", "参考线", "上下文明确实际可用性。", true},
                  {"gapSegment", "Line2D", "间隙段", "米制。", true},
                  {"flushSegment", "Line2D", "段差段", "上下文明确实际可用性。", true},
                  {notch ? "anchor" : "groove", "Point2D", "场景锚点", "米制。", true},
                  {"baseLineContext", "Record", "基准上下文", "profile.Geometry.v2。", true},
                  {"refLineContext", "Record", "参考上下文",
                   "可用时profile.Geometry.v2，否则gap.GeometryUnavailable.v1。", true},
                  {"gapSegmentContext", "Record", "间隙段上下文", "profile.Geometry.v2。", true},
                  {"flushSegmentContext", "Record", "段差段上下文",
                   "可用时profile.Geometry.v2，否则gap.GeometryUnavailable.v1。", true},
                  {"pointContext", "Record", "锚点上下文", "profile.Geometry.v2。", true},
                  {"quality", "Record", "场景质量", "逐相机结果与真实来源。", true},
                  {"qualityBase", "Record", "基准拟合质量", "保留原残差与计数。", true},
                  {"qualityRef", "Record", "参考拟合质量", "包含geometryAvailable。", true}};
    op.params = notch ? notchInspectionParams() : grooveInspectionParams();
    op.capabilities = {false, true, true};
    op.compute = [notch](const Inputs& in, const ParamView& params, Outputs& outputs,
                         ExecContext& ctx) {
      JointInputs input{in.get("primary"), in.get("secondary"), in.get("primaryProvenance"),
                        in.get("secondaryProvenance")};
      JointResult result;
      if (auto status = measureJoint(input, params, result, ctx, notch); !status.ok)
        return status;
      if (ctx.cancelled())
        return cancelled();
      publishJoint(result, outputs, notch);
      return Status::Ok();
    };
    registry.addOperator(std::move(op));
  }
}
}
