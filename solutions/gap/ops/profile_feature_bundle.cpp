#include "profile_feature_bundle.h"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "lyflow_profile/context.h"
#include "profile_scene.h"

namespace lyflow::packs::gap::profile_scene {
namespace {
struct Field {
  const char* name;
  const char* type;
};
const std::vector<Field> lineFields{{"profile", "Profile"},         {"line", "Line2D"},
                                    {"lineContext", "Record"},      {"innerEnd", "Point2D"},
                                    {"pointContext", "Record"},     {"inliers", "Indices"},
                                    {"weightedInliers", "Indices"}, {"quality", "Record"}};
const std::vector<Field> referenceFields{{"profile", "Profile"},     {"line", "Line2D"},
                                         {"lineContext", "Record"},  {"point", "Point2D"},
                                         {"pointContext", "Record"}, {"selection", "Record"},
                                         {"quality", "Record"}};
const std::vector<Field> circleFields{
    {"left", "Circle2D"},        {"leftContext", "Record"},   {"leftProfile", "Profile"},
    {"leftInliers", "Indices"},  {"right", "Circle2D"},       {"rightContext", "Record"},
    {"rightProfile", "Profile"}, {"rightInliers", "Indices"}, {"quality", "Record"}};
const std::vector<Field> locationFields{{"sensorPrimary", "Profile"},
                                        {"sensorSecondary", "Profile"},
                                        {"primary", "Profile"},
                                        {"secondary", "Profile"},
                                        {"merged", "Profile"},
                                        {"scanInfo", "Record"},
                                        {"datum", "Box2D"},
                                        {"target", "Box2D"},
                                        {"seamLeft", "Box2D"},
                                        {"seamRight", "Box2D"},
                                        {"datumContext", "Record"},
                                        {"targetContext", "Record"},
                                        {"seamLeftContext", "Record"},
                                        {"seamRightContext", "Record"},
                                        {"roiInfo", "Record"}};
constexpr std::size_t kScanFieldCount = 6;
bool fail(std::string& why, const char* message) {
  why = message;
  return false;
}
bool has(const Data& value) { return value.kind() != Data::Kind::None; }
bool inSpace(const Data& profile, const Data& geometry, const Data& context, std::string& why) {
  return profile_contract::geometryInSpace(geometry, context.asRecord(),
                                           profile.asProfile()->descriptor, why);
}
bool boundTo(const Data& profile, const Data& indices, std::string& why) {
  return indices.asIndices()->sourceCloudId == profile.asProfile()->cloud->id
             ? true
             : fail(why, "特征索引必须绑定实际Profile.cloud.id");
}
bool lineValid(const LineFeatureFields& f, std::string& why) {
  if (!inSpace(f.profile, f.line, f.lineContext, why) ||
      !inSpace(f.profile, f.innerEnd, f.pointContext, why) || !boundTo(f.profile, f.inliers, why) ||
      !boundTo(f.profile, f.weightedInliers, why))
    return false;
  return f.quality.asRecord()->type == "profile.LineFitRobust.v2"
             ? true
             : fail(why, "拟合特征需要profile.LineFitRobust.v2质量记录");
}
bool referenceValid(const ReferenceFeatureFields& f, std::string& why) {
  if (!inSpace(f.profile, f.point, f.pointContext, why))
    return false;
  return f.lineContext.asRecord()->type != "profile.Geometry.v3" ||
         inSpace(f.profile, f.line, f.lineContext, why);
}
bool locationValid(const Data& scan, const Data& rois, std::string& why) {
  ScanView scanView;
  RoiView roiView;
  if (!readScanPair(scan, scanView, why) || !readRoiSet(rois, roiView, why))
    return false;
  return profile_contract::geometryInSpace(*roiView.boxes[0], roiView.contexts[0]->asRecord(),
                                           scanView.primary->asProfile()->descriptor, why)
             ? true
             : fail(why, "定位角色框必须与实际scan处于同一物理空间");
}
bool circleValid(const CirclePairFeatureFields& f, std::string& why) {
  if (f.quality.type != "gap.CircleFitPair.v3")
    return fail(why, "双圆特征需要gap.CircleFitPair.v3质量记录");
  for (std::size_t side = 0; side < 2; ++side)
    if (!inSpace(f.profiles[side], f.circles[side], f.contexts[side], why) ||
        !boundTo(f.profiles[side], f.inliers[side], why))
      return false;
  return profile_contract::sameSpace(f.profiles[0].asProfile()->descriptor,
                                     f.profiles[1].asProfile()->descriptor)
             ? true
             : fail(why, "双圆特征两侧必须位于同一物理空间");
}
Data field(const Data& data, const std::string& name) { return *data.asBundle()->field(name); }
} // namespace

bool makeLocation(const Data& scan, const Data& rois, Data& result, std::string& why) {
  if (!locationValid(scan, rois, why))
    return false;
  Bundle bundle(kLocationKind);
  for (const auto& entry : scan.asBundle()->fields)
    bundle.set(entry.first == "info" ? "scanInfo" : entry.first, entry.second);
  for (const auto& entry : rois.asBundle()->fields)
    bundle.set(entry.first == "info" ? "roiInfo" : entry.first, entry.second);
  result = Data::bundle(std::move(bundle));
  why.clear();
  return true;
}
bool readLocation(const Data& data, LocationFields& result, std::string& why) {
  Bundle scan(kScanPairKind), rois(kRoiSetKind);
  for (std::size_t i = 0; i < locationFields.size(); ++i) {
    const std::string name = locationFields[i].name;
    const auto target = name == "scanInfo" || name == "roiInfo" ? "info" : name;
    (i < kScanFieldCount ? scan : rois).set(target, field(data, name));
  }
  result = {Data::bundle(std::move(scan)), Data::bundle(std::move(rois))};
  if (!locationValid(result.scan, result.rois, why))
    return false;
  why.clear();
  return true;
}
bool makeLineFeature(const LineFeatureFields& f, Data& result, std::string& why) {
  if (!lineValid(f, why))
    return false;
  Bundle bundle(kLineFeatureKind);
  const Data* values[]{&f.profile,      &f.line,    &f.lineContext,     &f.innerEnd,
                       &f.pointContext, &f.inliers, &f.weightedInliers, &f.quality};
  for (std::size_t i = 0; i < lineFields.size(); ++i)
    bundle.set(lineFields[i].name, *values[i]);
  result = Data::bundle(std::move(bundle));
  why.clear();
  return true;
}
bool readLineFeature(const Data& data, LineFeatureFields& result, std::string& why) {
  result = {field(data, "profile"),         field(data, "line"),         field(data, "lineContext"),
            field(data, "innerEnd"),        field(data, "pointContext"), field(data, "inliers"),
            field(data, "weightedInliers"), field(data, "quality")};
  if (!lineValid(result, why))
    return false;
  why.clear();
  return true;
}
bool makeReferenceFeature(const ReferenceFeatureFields& f, Data& result, std::string& why) {
  if (!referenceValid(f, why))
    return false;
  Bundle bundle(kReferenceFeatureKind);
  const Data* values[]{&f.profile,      &f.line,      &f.lineContext, &f.point,
                       &f.pointContext, &f.selection, &f.quality};
  for (std::size_t i = 0; i < referenceFields.size(); ++i)
    bundle.set(referenceFields[i].name, *values[i]);
  result = Data::bundle(std::move(bundle));
  why.clear();
  return true;
}
bool readReferenceFeature(const Data& data, ReferenceFeatureFields& result, std::string& why) {
  result = {field(data, "profile"), field(data, "line"),         field(data, "lineContext"),
            field(data, "point"),   field(data, "pointContext"), field(data, "selection"),
            field(data, "quality")};
  if (!referenceValid(result, why))
    return false;
  why.clear();
  return true;
}
bool completeReferenceFeature(const std::string& method, const ReferenceFeatureFields& partial,
                              ReferenceFeatureFields& result, std::string& why) {
  ReferenceFeatureFields candidate = partial;
  if (method == "line_end") {
    const auto* map = partial.selection.asRecord();
    const auto* quality = partial.quality.asRecord();
    if (!map || map->type != "profile.Selection.v2" || !quality ||
        quality->type != "profile.LineFitRobust.v2")
      return fail(why, "line_end需要公共拟合indexMap与profile.LineFitRobust.v2质量");
    candidate.selection = Data::record(
        Record{"gap.ReferenceSelection.v3",
               {{"schemaVersion", 3}, {"method", method}, {"fitSelection", map->data}}});
  } else if (method == "selected_point" || method == "nearest_point") {
    if (has(partial.line) || has(partial.lineContext) || has(partial.quality))
      return fail(why, "非拟合参考点禁止覆盖显示线、不可用上下文或质量记录");
    Line2D placeholder;
    std::copy_n(partial.point.asPoint2D()->p, 2, placeholder.point);
    candidate.line = Data::line2d(placeholder);
    candidate.lineContext = Data::record(Record{"gap.GeometryUnavailable.v2",
                                                {{"schemaVersion", 2},
                                                 {"available", false},
                                                 {"reason", "method_has_no_fitted_line"},
                                                 {"method", method}}});
    candidate.quality =
        Data::record(Record{"gap.ReferencePoint.v3",
                            {{"schemaVersion", 3},
                             {"model", method},
                             {"lineFitted", false},
                             {"pointCount", partial.profile.asProfile()->pointCount()},
                             {"selection", partial.selection.asRecord()->data}}});
  } else {
    return fail(why, "参考点特征方法非法");
  }
  result = std::move(candidate);
  why.clear();
  return true;
}
bool makeCirclePairFeature(const CirclePairFeatureFields& f, Data& result, std::string& why) {
  if (!circleValid(f, why))
    return false;
  Bundle bundle(kCirclePairKind);
  for (std::size_t side = 0; side < 2; ++side) {
    const std::string prefix = side ? "right" : "left";
    bundle.set(prefix, f.circles[side])
        .set(prefix + "Context", f.contexts[side])
        .set(prefix + "Profile", f.profiles[side])
        .set(prefix + "Inliers", f.inliers[side]);
  }
  bundle.set("quality", Data::record(f.quality));
  result = Data::bundle(std::move(bundle));
  why.clear();
  return true;
}
bool readCirclePairFeature(const Data& data, CirclePairFeatureFields& result, std::string& why) {
  for (std::size_t side = 0; side < 2; ++side) {
    const std::string prefix = side ? "right" : "left";
    result.circles[side] = field(data, prefix);
    result.contexts[side] = field(data, prefix + "Context");
    result.profiles[side] = field(data, prefix + "Profile");
    result.inliers[side] = field(data, prefix + "Inliers");
  }
  result.quality = *field(data, "quality").asRecord();
  if (!circleValid(result, why))
    return false;
  why.clear();
  return true;
}

namespace {
Status bad(const std::string& why, const char* port = "feature") {
  return Status::Error(Phase::Execute, "bad_input", why, {}, port);
}
Status cancelled() { return Status::Error(Phase::Execute, "cancelled", "原子特征组装已取消"); }
constexpr const char* locationChoicePorts[] = {"scanChoice",   "roiChoice",      "datumChoice",
                                               "targetChoice", "seamLeftChoice", "seamRightChoice"};
Status locationChoices(const Inputs& inputs) {
  bool any = false;
  for (const auto* name : locationChoicePorts)
    any = any || inputs.has(name);
  if (!any)
    return Status::Ok();
  if (!inputs.has("scanChoice") || !inputs.has("roiChoice"))
    return bad("回退路径证据需要同时提供轮廓与区域选择", "scanChoice");
  std::string selected;
  for (const auto* name : locationChoicePorts) {
    if (!inputs.has(name))
      continue;
    const auto& data = inputs.get(name).asRecord()->data;
    const auto found = data.find("choice");
    if (found == data.end() || (*found != "a" && *found != "b"))
      return bad("回退路径证据必须为明确a/b的FallbackChoice", name);
    const auto choice = found->get<std::string>();
    if (selected.empty())
      selected = choice;
    else if (selected != choice)
      return Status::Error(Phase::Execute, "location_path_mismatch",
                           "角色框和轮廓选中了不同定位路径，拒绝混合来源", {}, name);
  }
  return Status::Ok();
}
std::vector<Port> ports(const std::vector<Field>& fields) {
  std::vector<Port> result;
  for (const auto& entry : fields)
    result.push_back({entry.name, entry.type, entry.name, "原子特征的实际载荷。", true});
  return result;
}
TopologyEdit retireProvenance(const nlohmann::json&, const std::vector<MigrationInput>& inputs) {
  return profile_contract::retireInputs(inputs,
                                        {"provenance", "leftProvenance", "rightProvenance"});
}
Status compose(int kind, const Inputs& in, const ParamView& p, Data& result) {
  std::string why;
  if (kind == 0) {
    const LineFeatureFields feature{
        in.get("profile"),      in.get("line"),    in.get("lineContext"),     in.get("innerEnd"),
        in.get("pointContext"), in.get("inliers"), in.get("weightedInliers"), in.get("quality")};
    return makeLineFeature(feature, result, why) ? Status::Ok() : bad(why);
  }
  if (kind == 1) {
    const ReferenceFeatureFields partial{
        in.get("profile"),      in.get("line"),      in.get("lineContext"), in.get("point"),
        in.get("pointContext"), in.get("selection"), in.get("quality")};
    ReferenceFeatureFields feature;
    return completeReferenceFeature(p.choice("method"), partial, feature, why) &&
                   makeReferenceFeature(feature, result, why)
               ? Status::Ok()
               : bad(why);
  }
  CirclePairFeatureFields feature;
  feature.quality = *in.get("quality").asRecord();
  for (std::size_t side = 0; side < 2; ++side) {
    const std::string prefix = side ? "right" : "left";
    feature.circles[side] = in.get(prefix);
    feature.contexts[side] = in.get(prefix + "Context");
    feature.profiles[side] = in.get(prefix + "Profile");
    feature.inliers[side] = in.get(prefix + "Inliers");
  }
  return makeCirclePairFeature(feature, result, why) ? Status::Ok() : bad(why);
}
} // namespace
void registerProfileFeatureBundles(Registry& registry) {
  const char* kinds[]{kLineFeatureKind, kReferenceFeatureKind, kCirclePairKind, kLocationKind};
  const char* names[]{"line_feature", "reference_feature", "circle_pair", "location"};
  const std::vector<Field>* fields[]{&lineFields, &referenceFields, &circleFields, &locationFields};
  for (int kind = 0; kind < 4; ++kind) {
    BundleDesc bundle;
    bundle.kind = kinds[kind];
    bundle.label = names[kind];
    bundle.doc = "单个原子结果绑定数值、几何与原生观测；fallback必须选择整个Bundle。";
    for (const auto& entry : *fields[kind])
      bundle.fields.push_back({entry.name, entry.type, "同一特征的实际数据。"});
    registry.addBundle(std::move(bundle));
    for (const bool make : {true, false}) {
      OperatorDesc op;
      op.id = "gap." + std::string(make ? "make_" : "split_") + names[kind];
      op.version = "2.0.0";
      op.migrations = {{1, nullptr, &retireProvenance}};
      op.label = std::string(make ? "组装" : "拆分") + names[kind];
      op.category = "间隙/原子特征";
      op.keywords = {"atomic feature", "bundle", "fallback", "原子特征", "整体回退"};
      op.doc =
          "组装时要求几何与原生轮廓处于同一坐标空间、索引绑定该轮廓；失败和取消不发布部分结果。";
      op.capabilities = {true, true, true};
      if (kind == 3) {
        const std::vector<Port> locationPorts{
            {"scan", "Bundle<gap.ScanPair.v3>", "轮廓对", "实际量测轮廓对。", true},
            {"rois", "Bundle<gap.RoiSet.v3>", "角色框", "须与scan处于同一坐标空间。", true}};
        const std::vector<Port> atomic{{"location", "Bundle<gap.Location.v3>", "原子定位",
                                        "扁平15字段，不嵌套Bundle。", true}};
        op.inputs = make ? locationPorts : atomic;
        op.outputs = make ? atomic : locationPorts;
        if (make) {
          for (const auto* name : locationChoicePorts) {
            Port input{name, "Record", name, "可选回退路径证据；轮廓与区域必须同支。", false};
            input.contract = {{"recordType", "FallbackChoice"}};
            op.inputs.push_back(std::move(input));
          }
          op.validate = [](const ParamView&, const std::set<std::string>& connected) {
            bool any = false;
            for (const auto* name : locationChoicePorts)
              any = any || connected.count(name) != 0;
            if (any && (!connected.count("scanChoice") || !connected.count("roiChoice")))
              return std::vector<Issue>{
                  Issue::error("missing_input", "回退路径证据需要同时连接scanChoice与roiChoice", {},
                               "scanChoice")};
            return std::vector<Issue>{};
          };
        }
      } else {
        const auto fieldPorts = ports(*fields[kind]);
        const std::vector<Port> atomic{{"feature", bundleTypeName(kinds[kind]), "原子特征",
                                        "数值、上下文与质量整体选择。", true}};
        op.inputs = make ? fieldPorts : atomic;
        op.outputs = make ? atomic : fieldPorts;
      }
      if (make && kind == 1) {
        for (auto& port : op.inputs)
          if (port.name == "line" || port.name == "lineContext" || port.name == "quality")
            port.required = false;
        Param method;
        method.name = "method";
        method.label = "参考提取方法";
        method.type = ParamType::Enum;
        method.def = Value::text("line_end");
        method.options = {{"line_end", "拟合线近端点"},
                          {"selected_point", "框锚点最近样本"},
                          {"nearest_point", "直线最近样本"}};
        op.params = {method};
        op.validate = [](const ParamView& p, const std::set<std::string>& connected) {
          std::vector<Issue> result;
          const bool fitted = p.choice("method") == "line_end";
          for (const auto* name : {"line", "lineContext", "quality"})
            if (connected.count(name) != static_cast<std::size_t>(fitted))
              result.push_back(Issue::error("bad_input",
                                            fitted ? "line_end需要实际拟合线、上下文和质量"
                                                   : "非拟合参考点禁止覆盖占位线、上下文或质量",
                                            "method", name));
          return result;
        };
      }
      op.compute = [kind, make, fields](const Inputs& in, const ParamView& p, Outputs& out,
                                        ExecContext& ctx) {
        if (ctx.cancelled())
          return cancelled();
        std::string why;
        Data result;
        if (kind == 3) {
          if (make) {
            if (const auto status = locationChoices(in); !status.ok)
              return status;
            if (!makeLocation(in.get("scan"), in.get("rois"), result, why))
              return bad(why, "rois");
            if (ctx.cancelled())
              return cancelled();
            out.set("location", std::move(result));
          } else {
            LocationFields location;
            readLocation(in.get("location"), location, why);
            out.set("scan", std::move(location.scan));
            out.set("rois", std::move(location.rois));
          }
          return Status::Ok();
        }
        if (make) {
          if (const auto status = compose(kind, in, p, result); !status.ok)
            return status;
          if (ctx.cancelled())
            return cancelled();
          out.set("feature", std::move(result));
        } else {
          const auto& feature = in.get("feature");
          for (const auto& entry : *fields[kind])
            out.set(entry.name, field(feature, entry.name));
        }
        return Status::Ok();
      };
      registry.addOperator(std::move(op));
    }
  }
}
} // namespace lyflow::packs::gap::profile_scene
