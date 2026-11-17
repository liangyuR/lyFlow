#include "profile_migration.h"
#include "profile_bundle_ops.h"

#include <cmath>
#include <utility>

#include "lyflow_profile/context.h"
#include "profile_guard.h"

namespace lyflow::packs::gap::profile_scene {
namespace {
using Json = nlohmann::json;
constexpr const char* scanFields[] = {"sensorPrimary", "sensorSecondary",  "primary", "secondary",
                                      "merged",        "mergedProvenance", "info"};
constexpr const char* roles[] = {"datum", "target", "seamLeft", "seamRight"};
constexpr const char* contexts[] = {"datumContext", "targetContext", "seamLeftContext",
                                    "seamRightContext"};
Status bad(const std::string& message, const char* port) {
  return Status::Error(Phase::Execute, "bad_input", message, {}, port);
}
Status cancelled() { return Status::Error(Phase::Execute, "cancelled", "场景数据组装已取消"); }
Status assembleScan(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                    ExecContext& ctx) {
  if (ctx.cancelled())
    return cancelled();
  Data result;
  std::string why;
  if (inputs.has("info")) {
    for (const auto* name :
         {"sourceInfo", "guardQuality", "cropStatus", "primaryProvenance", "secondaryProvenance"})
      if (inputs.has(name))
        return bad("完整info往返不可同时覆盖独立来源输入", name);
    Bundle bundle(kScanPairKind);
    for (const auto* field : scanFields)
      bundle.set(field, inputs.get(field));
    result = Data::bundle(std::move(bundle));
    ScanView view;
    if (!readScanPair(result, view, why))
      return bad(why, "info");
  } else {
    const auto& p = inputs.get("sensorPrimary");
    const auto& s = inputs.get("sensorSecondary");
    const auto* primary = p.asProfile();
    const auto* secondary = s.asProfile();
    const auto* measured = inputs.get("primary").asProfile();
    const auto* mergedOrigin = inputs.get("mergedProvenance").asRecord();
    ScanMetadata metadata;
    metadata.sensorFrame = primary->descriptor.frame;
    metadata.measurementFrame = measured->descriptor.frame;
    metadata.stage = params.choice("stage");
    metadata.guard = {
        {"taken", "both"}, {"evaluated", false}, {"exceeded", false}, {"onDisagree", "off"}};
    if (inputs.has("guardQuality")) {
      const auto* guard = inputs.get("guardQuality").asRecord();
      if (!guardMatches(p, s, *guard, why))
        return bad(why, "guardQuality");
      metadata.guard = guard->data;
    }
    if (inputs.has("cropStatus")) {
      const auto* crop = inputs.get("cropStatus").asRecord();
      if (crop->type != "gap.RollCrop.v2")
        return bad("裁剪状态必须为gap.RollCrop.v2", "cropStatus");
      metadata.crop = crop->data;
    }
    metadata.origins = Json::object();
    for (const auto& entry :
         {std::make_pair("primary", primary), std::make_pair("secondary", secondary)})
      metadata.origins[entry.first] = {{"kind", "profile_declaration"},
                                       {"sourceId", entry.second->descriptor.sourceId},
                                       {"details", {{"declaredBy", "gap.make_scan_pair"}}}};
    if (inputs.has("sourceInfo")) {
      const auto* origin = inputs.get("sourceInfo").asRecord();
      if (origin->type != "gap.ProfilePairSource.v2" || !origin->data.is_object() ||
          origin->data.size() != 2 || !origin->data.contains("schemaVersion") ||
          !origin->data["schemaVersion"].is_number_integer() ||
          origin->data["schemaVersion"] != 2 || !origin->data.contains("origins"))
        return bad("需要原始配对来源gap.ProfilePairSource.v2", "sourceInfo");
      metadata.origins = origin->data["origins"];
      const auto taken = metadata.guard.value("taken", std::string());
      if (taken == "primary" || taken == "secondary") {
        if (!metadata.origins.is_object() || !metadata.origins.contains(taken))
          return bad("guard选中来源缺少原始来源记录", "sourceInfo");
        const auto trusted = metadata.origins[taken];
        metadata.origins = {{"primary", trusted}, {"secondary", trusted}};
      }
    }
    for (const auto& entry :
         {std::make_pair("primaryProvenance", &metadata.primaryProvenance),
          std::make_pair("secondaryProvenance", &metadata.secondaryProvenance)}) {
      if (!inputs.has(entry.first))
        continue;
      *entry.second = *inputs.get(entry.first).asRecord();
    }
    metadata.processing = {
        {"method", "assemble"},
        {"status", "complete"},
        {"details",
         {{"declaredStage", metadata.stage}, {"originEvidence", "native_profile_declarations"}}}};
    if (!makeScanPair(p, s, inputs.get("primary"), inputs.get("secondary"), inputs.get("merged"),
                      *mergedOrigin, metadata, result, why))
      return bad(why, "scan");
  }
  if (ctx.cancelled())
    return cancelled();
  outputs.set("scan", std::move(result));
  return Status::Ok();
}
Status splitScan(const Inputs& inputs, const ParamView&, Outputs& outputs, ExecContext& ctx) {
  if (ctx.cancelled())
    return cancelled();
  ScanView view;
  std::string why;
  const auto& scan = inputs.get("scan");
  if (!readScanPair(scan, view, why))
    return bad(why, "scan");
  if (ctx.cancelled())
    return cancelled();
  for (const auto* field : scanFields)
    outputs.set(field, *scan.asBundle()->field(field));
  outputs.set("primaryProvenance", Data::record(std::move(view.primaryProvenance)));
  outputs.set("secondaryProvenance", Data::record(std::move(view.secondaryProvenance)));
  return Status::Ok();
}
Status assembleRois(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                    ExecContext& ctx) {
  if (ctx.cancelled())
    return cancelled();
  RoiFields fields;
  for (std::size_t i = 0; i < 4; ++i) {
    fields.boxes[i] = inputs.get(roles[i]);
    fields.contexts[i] = inputs.get(contexts[i]);
  }
  if (inputs.has("info")) {
    const auto* info = inputs.get("info").asRecord();
    if (params.choice("metadataMode") != "update_metadata" &&
        (inputs.has("overall") || inputs.has("overallContext") || inputs.has("cropStatus")))
      return bad("完整业务info往返不可同时覆盖独立窗口或裁剪状态", "info");
    fields.info = *info;
    Data validated;
    std::string why;
    if (!makeRoiSet(fields, validated, why))
      return bad(why, "info");
  } else {
    if (params.choice("metadataMode") == "update_metadata")
      return bad("update_metadata必须提供原完整info", "info");
    fields.info = {kRoiInfoType,
                   {{"schemaVersion", 2},
                    {"source", params.choice("source")},
                    {"datumSide", params.choice("datumSide")},
                    {"alignment", nullptr},
                    {"overallMm", nullptr},
                    {"cropStatus", nullptr},
                    {"processing",
                     {{"method", "assemble"},
                      {"status", "complete"},
                      {"details", {{"originEvidence", "explicit_role_declarations"}}}}}}};
  }
  if (inputs.has("overall") != inputs.has("overallContext"))
    return bad("整体窗口与其上下文必须成对提供", "overallContext");
  if (inputs.has("overall")) {
    const auto* overall = inputs.get("overall").asBox2D();
    const auto* context = inputs.get("overallContext").asRecord();
    ProfileDescriptor space, expected;
    std::string why;
    if (context->type != "profile.Geometry.v2" ||
        !profile_contract::geometryMatchesPhysical(inputs.get("overall"), context, space, why) ||
        !profile_contract::geometryMatchesPhysical(fields.boxes[0], fields.contexts[0].asRecord(),
                                                   expected, why) ||
        !profile_contract::sameSpace(space, expected))
      return bad(why.empty() ? "整体窗口需要同量测空间的有效几何绑定" : why, "overallContext");
    fields.info.data["overallMm"] = Json::array(
        {static_cast<double>(overall->min[0]) * 1000, static_cast<double>(overall->min[1]) * 1000,
         static_cast<double>(overall->max[0]) * 1000, static_cast<double>(overall->max[1]) * 1000});
  }
  if (inputs.has("cropStatus")) {
    const auto* crop = inputs.get("cropStatus").asRecord();
    if (crop->type != "gap.RollCrop.v2")
      return bad("裁剪状态必须为gap.RollCrop.v2", "cropStatus");
    fields.info.data["cropStatus"] = crop->data;
  }
  if (inputs.has("info") && params.choice("metadataMode") == "update_metadata")
    fields.info.data["processing"]["details"]["metadataUpdate"] = {
        {"declaredBy", "gap.make_roi_set"},
        {"overallUpdated", inputs.has("overall")},
        {"cropUpdated", inputs.has("cropStatus")}};
  Data result;
  std::string why;
  if (!makeRoiSet(fields, result, why))
    return bad(why, "info");
  if (ctx.cancelled())
    return cancelled();
  outputs.set("rois", std::move(result));
  return Status::Ok();
}
Status splitRois(const Inputs& inputs, const ParamView&, Outputs& outputs, ExecContext& ctx) {
  if (ctx.cancelled())
    return cancelled();
  RoiView view;
  std::string why;
  if (!readRoiSet(inputs.get("rois"), view, why))
    return bad(why, "rois");
  Data seam;
  Record seamContext;
  if (const auto status = seamTowardGeometry(view, seam, seamContext, ctx); !status.ok)
    return status;
  for (std::size_t i = 0; i < 4; ++i) {
    outputs.set(roles[i], *view.boxes[i]);
    outputs.set(contexts[i], *view.contexts[i]);
  }
  outputs.set("info", Data::record(*view.info));
  outputs.set("seam", std::move(seam));
  outputs.set("seamContext", Data::record(std::move(seamContext)));
  return Status::Ok();
}
std::vector<Port> scanPorts() {
  std::vector<Port> ports;
  for (const auto* field : scanFields)
    ports.push_back({field,
                     std::string(field) == "mergedProvenance" || std::string(field) == "info"
                         ? "Record"
                         : "Profile",
                     field, "完整来源绑定随原生轮廓一起传递。", true});
  return ports;
}
std::vector<Port> roiPorts() {
  std::vector<Port> ports;
  for (std::size_t i = 0; i < 4; ++i) {
    ports.push_back({roles[i], "Box2D", roles[i], "量测空间的米制角色框。", true});
    ports.push_back({contexts[i], "Record", contexts[i], "绑定实际框内容的版本2上下文。", true});
  }
  ports.push_back({"info", "Record", "业务来源", "完整gap.RoiInfo.v2，保留真实处理状态。", true});
  return ports;
}
}
Status seamTowardGeometry(const RoiView& rois, Data& box, Record& context, ExecContext& ctx) {
  if (ctx.cancelled())
    return cancelled();
  Json sources = Json::array();
  ProfileDescriptor first;
  std::string why;
  for (std::size_t i = 2; i < 4; ++i) {
    const auto* bound = rois.contexts[i]->asRecord();
    ProfileDescriptor space;
    Json parents;
    if (!bound || bound->type != "profile.Geometry.v2" ||
        !profile_contract::geometryMatchesPhysical(*rois.boxes[i], bound, space, why, &parents))
      return bad(why.empty() ? "缝框需要版本2几何上下文" : why, contexts[i]);
    if (space.plane != "XY" || (i == 3 && !profile_contract::sameSpace(first, space)))
      return bad("两侧缝框必须位于同一量测XY空间", contexts[i]);
    if (i == 2)
      first = std::move(space);
    for (const auto& parent : parents)
      if (std::find(sources.begin(), sources.end(), parent) == sources.end())
        sources.push_back(parent);
  }
  if (sources.size() > 64)
    return bad("缝中心来源超过64项预算", "seamContext");
  const auto& left = *rois.boxes[2]->asBox2D();
  const auto& right = *rois.boxes[3]->asBox2D();
  Box2D toward;
  for (int axis = 0; axis < 2; ++axis) {
    const double a = .5 * (static_cast<double>(left.min[axis]) + left.max[axis]);
    const double b = .5 * (static_cast<double>(right.min[axis]) + right.max[axis]);
    toward.min[axis] = toward.max[axis] = static_cast<float>(.5 * (a + b));
    if (!std::isfinite(toward.min[axis]))
      return bad("缝中心超过有限几何范围", "seam");
  }
  Data candidate = Data::box2d(toward);
  Record bound{"profile.Geometry.v2",
               profile_contract::geometryContextV2FromSources(candidate, sources)};
  if (ctx.cancelled())
    return cancelled();
  box = std::move(candidate);
  context = std::move(bound);
  return Status::Ok();
}
void registerProfileBundleOps(Registry& registry) {
  for (const bool scan : {true, false}) {
    for (const bool assemble : {true, false}) {
      OperatorDesc op;
      op.id = scan ? (assemble ? "gap.make_scan_pair" : "gap.split_scan_pair")
                   : (assemble ? "gap.make_roi_set" : "gap.split_roi_set");
      op.version = "2.0.0";
      declareGapGraphUpgrade(op);
      op.label = scan ? (assemble ? "原生轮廓对组装" : "原生轮廓对解包")
                      : (assemble ? "角色区域集组装" : "角色区域集解包");
      op.category = "间隙/组合";
      op.keywords = {"profile", "bundle", "scan pair", "ROI", "原生轮廓", "角色区域", "来源"};
      op.doc = "原生载荷与严格来源配套组装或解包；不推测外部轮廓的原槽、单位或坐标系。";
      op.capabilities = {false, true, true};
      auto fields = scan ? scanPorts() : roiPorts();
      const Port bundle{scan ? "scan" : "rois",
                        scan ? "Bundle<gap.ScanPair.v2>" : "Bundle<gap.RoiSet.v2>",
                        scan ? "轮廓对" : "角色框", "完整版本2场景。", true};
      if (assemble) {
        op.inputs = std::move(fields);
        for (auto& input : op.inputs)
          if (input.name == "info")
            input.required = false;
        if (scan) {
          Param stage;
          stage.name = "stage";
          stage.type = ParamType::Enum;
          stage.label = "处理阶段";
          stage.def = Value::text("raw");
          stage.options = {{"raw", "原始传感器量测映射"}, {"processed", "显式处理后子集"}};
          op.params = {stage};
          for (const auto* field : {"sourceInfo", "guardQuality", "cropStatus", "primaryProvenance",
                                    "secondaryProvenance"})
            op.inputs.push_back(
                {field, "Record", field, "可选完整来源声明，与info往返互斥。", false});
        } else {
          Param source;
          source.name = "source";
          source.label = "区域来源";
          source.type = ParamType::Enum;
          source.def = Value::text("config");
          source.options = {{"config", "显式配置"}, {"model", "模型关联声明"}};
          Param side;
          side.name = "datumSide";
          side.label = "基准件侧";
          side.type = ParamType::Enum;
          side.def = Value::text("left");
          side.options = {{"left", "左侧"}, {"right", "右侧"}};
          Param mode;
          mode.name = "metadataMode";
          mode.label = "元数据处理";
          mode.type = ParamType::Enum;
          mode.def = Value::text("strict_roundtrip");
          mode.options = {{"strict_roundtrip", "完整往返"},
                          {"update_metadata", "显式更新窗口记录"}};
          op.params = {source, side, mode};
          op.inputs.push_back({"overall", "Box2D", "整体窗口", "可选实际窗口。", false});
          op.inputs.push_back(
              {"overallContext", "Record", "窗口来源", "与整体窗口成对提供。", false});
          op.inputs.push_back({"cropStatus", "Record", "裁剪状态", "实际版本2裁剪结果。", false});
        }
        op.outputs = {bundle};
        op.compute = scan ? &assembleScan : &assembleRois;
      } else {
        op.inputs = {bundle};
        op.outputs = std::move(fields);
        if (scan) {
          op.outputs.push_back(
              {"primaryProvenance", "Record", "主角色来源", "保留全部裁剪来源。", true});
          op.outputs.push_back(
              {"secondaryProvenance", "Record", "次角色来源", "保留全部裁剪来源。", true});
        } else {
          op.outputs.push_back({"seam", "Box2D", "缝中心", "旧数值约定的零尺寸方向框。", true});
          op.outputs.push_back(
              {"seamContext", "Record", "缝中心来源", "两侧真实来源的并集。", true});
        }
        op.compute = scan ? &splitScan : &splitRois;
      }
      registry.addOperator(std::move(op));
    }
  }
}
}
