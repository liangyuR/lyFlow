#include "profile_scene.h"

#include <cmath>
#include <cstring>
#include <set>
#include <unordered_map>

#include "lyflow/profile.h"
#include "lyflow_profile/provenance.h"
#include "profile_guard.h"

namespace lyflow::packs::gap::profile_scene {
namespace {
using Json = nlohmann::json;
constexpr std::array<const char*, 4> roles{"datum", "target", "seamLeft", "seamRight"};
constexpr std::array<const char*, 4> contextNames{"datumContext", "targetContext",
                                                  "seamLeftContext", "seamRightContext"};
bool fail(std::string& why, const char* text) {
  why = text;
  return false;
}
bool keys(const Json& data, std::initializer_list<const char*> names) {
  if (!data.is_object() || data.size() != names.size())
    return false;
  for (const auto* name : names)
    if (!data.contains(name))
      return false;
  return true;
}
bool name(const std::string& value) {
  return !value.empty() && value.size() <= 1024 && value.find('\0') == std::string::npos;
}
bool bounded(const Json& value, unsigned depth = 0) {
  if (depth > 12)
    return false;
  if (value.is_string())
    return value.get_ref<const std::string&>().size() <= 4096;
  if (value.is_number_float())
    return std::isfinite(value.get<double>());
  if (value.is_array() && value.size() > 16)
    return false;
  if (value.is_object() && value.size() > 64)
    return false;
  if (value.is_structured())
    for (const auto& item : value)
      if (!bounded(item, depth + 1))
        return false;
  return true;
}
bool small(const Json& value) { return bounded(value) && value.dump().size() <= 64 * 1024; }
bool processing(const Json& value) {
  return keys(value, {"method", "status", "details"}) &&
         (value["method"] == "read" || value["method"] == "template" ||
          value["method"] == "model" || value["method"] == "assemble") &&
         (value["status"] == "complete" || value["status"] == "skipped") &&
         value["details"].is_object();
}
bool exactBundle(const Bundle* bundle, const char* kind, std::initializer_list<const char*> names,
                 std::string& why) {
  if (!bundle || bundle->kind != kind || bundle->fields.size() != names.size())
    return fail(why, "场景Bundle类型或字段数量不匹配");
  std::set<std::string> found;
  for (const auto& field : bundle->fields) {
    if (!found.insert(field.first).second)
      return fail(why, "场景Bundle包含重复字段");
    bool known = false;
    for (const auto* candidate : names)
      known = known || field.first == candidate;
    if (!known)
      return fail(why, "场景Bundle包含未知字段");
  }
  return true;
}
bool profile(const Data& data, const Profile*& result, std::string& why) {
  result = data.asProfile();
  return result && validateProfile(*result, why) ? true : fail(why, "场景字段需要有效原生Profile");
}
bool subset(const Profile& input, const Profile& output, bool preserveAll, std::string& why) {
  if (profileDescriptorJson(input.descriptor) != profileDescriptorJson(output.descriptor))
    return fail(why, "场景处理不得重写源身份、sourceCount或坐标声明");
  if (preserveAll &&
      (input.sourceIndices != output.sourceIndices || input.validity != output.validity))
    return fail(why, "raw量测轮廓必须保留传感器原始槽与有效性");
  std::unordered_map<std::uint32_t, std::size_t> slots;
  for (std::size_t i = 0; i < input.pointCount(); ++i)
    slots.emplace(input.sourceIndices[i], i);
  for (std::size_t i = 0; i < output.pointCount(); ++i) {
    const auto found = slots.find(output.sourceIndices[i]);
    if (found == slots.end())
      return fail(why, "处理轮廓引入不存在的传感器源槽");
    const auto j = found->second;
    const auto& a = *input.cloud;
    const auto& b = *output.cloud;
    if (output.validity[i] > input.validity[j] ||
        !profile_contract::provenance_detail::slotEqual(a.xyz, j, b.xyz, i, 3) ||
        !profile_contract::provenance_detail::slotEqual(a.intensity, j, b.intensity, i, 1) ||
        !profile_contract::provenance_detail::slotEqual(a.normals, j, b.normals, i, 3) ||
        !profile_contract::provenance_detail::slotEqual(a.rgb, j, b.rgb, i, 3))
      return fail(why, "量测轮廓与实际换轴后的槽坐标、通道或有效性不匹配");
  }
  return true;
}
bool mapped(const Data& sensor, const Data& measurement, const ScanMetadata& metadata,
            Profile& full, std::string& why) {
  const Profile *input = nullptr, *output = nullptr;
  if (!profile(sensor, input, why) || !profile(measurement, output, why))
    return false;
  if (input->descriptor.frame != metadata.sensorFrame || input->descriptor.plane != "XZ" ||
      output->descriptor.frame != metadata.measurementFrame || output->descriptor.plane != "XY" ||
      output->descriptor.unit != "m" ||
      input->descriptor.sourceId.rfind("derived:composite/", 0) == 0)
    return fail(why, "传感器XZ与米制量测XY必须显式声明真实帧和单路来源");
  Profile axes;
  if (!remapProfileAxes(*input, "XZY", {1, 1, 1}, metadata.measurementFrame, axes, why) ||
      !convertProfileUnit(axes, "m", full, why))
    return false;
  return subset(full, *output, metadata.stage == "raw", why);
}
bool metadataValid(const ScanMetadata& metadata, const Data& sensorPrimary,
                   const Data& sensorSecondary, std::string& why) {
  if (!name(metadata.sensorFrame) || !name(metadata.measurementFrame) ||
      metadata.sensorFrame == metadata.measurementFrame ||
      (metadata.stage != "raw" && metadata.stage != "processed") ||
      !processing(metadata.processing) || !metadata.origins.is_object() ||
      !keys(metadata.origins, {"primary", "secondary"}) ||
      (!metadata.crop.is_null() && !metadata.crop.is_object()) || !metadata.guard.is_object())
    return fail(why, "场景需要明确帧、处理stage、来源声明、guard与处理状态");
  const Json disabledGuard{
      {"taken", "both"}, {"evaluated", false}, {"exceeded", false}, {"onDisagree", "off"}};
  if (metadata.guard != disabledGuard &&
      !guardMatches(sensorPrimary, sensorSecondary, Record{"gap.CameraGuard.v2", metadata.guard},
                    why))
    return false;
  for (const auto& entry :
       {std::make_pair("primary", &sensorPrimary), std::make_pair("secondary", &sensorSecondary)}) {
    const auto& origin = metadata.origins[entry.first];
    const auto* input = entry.second->asProfile();
    if (!input || !keys(origin, {"kind", "sourceId", "details"}) ||
        (origin["kind"] != "file" && origin["kind"] != "injected_profile" &&
         origin["kind"] != "injected_cloud_declared" && origin["kind"] != "profile_declaration") ||
        origin["sourceId"] != input->descriptor.sourceId || !origin["details"].is_object())
      return fail(why, "每个逻辑角色必须保留所选Profile的实际软件来源声明");
  }
  if (metadata.guard["taken"] != "both" &&
      profile_contract::context(sensorPrimary) != profile_contract::context(sensorSecondary))
    return fail(why, "guard单路复制必须保留同一可信原Profile，不能改名成另一相机");
  const Json metadataJson{{"guard", metadata.guard},
                          {"crop", metadata.crop},
                          {"origins", metadata.origins},
                          {"processing", metadata.processing}};
  if (!small(metadataJson))
    return fail(why, "场景元数据必须有限且小于64KiB，不承载大型采样数组");
  return true;
}
bool mergedValid(const Data& primary, const Data& secondary, const Data& merged,
                 const Profile& fullPrimary, const Profile& fullSecondary, const Record& provenance,
                 std::string& why) {
  Json sources;
  if (!profile_contract::profileSources(merged, &provenance, sources, why))
    return false;
  const auto& value = provenance.data;
  if (value["kind"] != "derived_composite" || value["recipe"]["segments"].size() != 2)
    return fail(why, "场景合并轮廓必须明确保留secondary后primary两段");
  const auto* output = merged.asProfile();
  if (!output || output->descriptor.unit != "m" || output->descriptor.plane != "XY")
    return fail(why, "场景合并轮廓必须处于米制量测XY空间");
  std::size_t segmentIndex = 0;
  const Profile* boundParents[2]{};
  std::unordered_map<std::uint32_t, std::size_t> mergedOrder;
  std::size_t order = 0;
  for (const auto& entry : {std::make_tuple("secondary", &secondary, &fullSecondary),
                            std::make_tuple("primary", &primary, &fullPrimary)}) {
    const auto& segment = value["recipe"]["segments"][segmentIndex];
    const auto& current = *std::get<1>(entry);
    const auto* currentProfile = current.asProfile();
    const auto fullData = Data::profile(*std::get<2>(entry));
    if (segment["logicalRole"] != std::get<0>(entry) ||
        (segment["sourceContext"] != profile_contract::context(current) &&
         segment["sourceContext"] != profile_contract::context(fullData)))
      return fail(why, "复合分段必须绑定实际所选量测Profile，不得伪用逻辑角色或传感器上下文");
    boundParents[segmentIndex++] = segment["sourceContext"] == profile_contract::context(fullData)
                                       ? std::get<2>(entry)
                                       : currentProfile;
    std::unordered_map<std::uint32_t, std::size_t> slots;
    for (std::size_t i = 0; i < currentProfile->pointCount(); ++i)
      slots.emplace(currentProfile->sourceIndices[i], i);
    const auto offset = segment["offset"].get<std::uint64_t>();
    const auto count = segment["count"].get<std::uint64_t>();
    for (const auto index : currentProfile->sourceIndices)
      mergedOrder.emplace(static_cast<std::uint32_t>(offset + index), order++);
    for (std::size_t i = 0; i < output->pointCount(); ++i) {
      const auto index = output->sourceIndices[i];
      if (index < offset || index >= offset + count)
        continue;
      const auto found = slots.find(static_cast<std::uint32_t>(index - offset));
      if (found == slots.end())
        return fail(why, "合并轮廓包含当前量测分路已不持有的源槽");
      const auto j = found->second;
      const auto& a = *currentProfile->cloud;
      const auto& b = *output->cloud;
      if (output->validity[i] > currentProfile->validity[j] ||
          !profile_contract::provenance_detail::slotEqual(a.xyz, j, b.xyz, i, 3) ||
          (b.hasIntensity() &&
           !profile_contract::provenance_detail::slotEqual(a.intensity, j, b.intensity, i, 1)) ||
          (b.hasNormals() &&
           !profile_contract::provenance_detail::slotEqual(a.normals, j, b.normals, i, 3)) ||
          (b.hasRgb() && !profile_contract::provenance_detail::slotEqual(a.rgb, j, b.rgb, i, 3)))
        return fail(why, "合并载荷与实际量测分路的原槽坐标、通道或有效性不一致");
    }
  }
  std::size_t previous = 0;
  bool first = true;
  for (const auto index : output->sourceIndices) {
    const auto found = mergedOrder.find(index);
    if (found == mergedOrder.end() || (!first && found->second <= previous))
      return fail(why, "合并结果必须保留secondary后primary及各输入槽序的子序列");
    previous = found->second;
    first = false;
  }
  const auto& a = *boundParents[0]->cloud;
  const auto& b = *boundParents[1]->cloud;
  const auto& c = *output->cloud;
  const auto active = [](const Profile& profile) {
    return std::find(profile.validity.begin(), profile.validity.end(), std::uint8_t(1)) !=
           profile.validity.end();
  };
  const bool hasA = active(*boundParents[0]), hasB = active(*boundParents[1]);
  const auto retained = [&](bool av, bool bv) {
    return (av || bv) && (!hasA || av) && (!hasB || bv);
  };
  if (output->pointCount() && (c.hasIntensity() != retained(a.hasIntensity(), b.hasIntensity()) ||
                               c.hasNormals() != retained(a.hasNormals(), b.hasNormals()) ||
                               c.hasRgb() != retained(a.hasRgb(), b.hasRgb())))
    return fail(why, "合并轮廓通道不符合实际分路与显式通道策略");
  if (value["recipe"]["channelPolicy"] == "require_equal" && hasA && hasB &&
      (a.hasIntensity() != b.hasIntensity() || a.hasNormals() != b.hasNormals() ||
       a.hasRgb() != b.hasRgb()))
    return fail(why, "require_equal不得在场景处理中静默丢弃不共有通道");
  return true;
}
bool scanInfo(const Data& sensorPrimary, const Data& sensorSecondary, const Data& primary,
              const Data& secondary, const Data& merged, const Record& provenance,
              const ScanMetadata& metadata, Record& result, std::string& why) {
  const Profile *sp = nullptr, *ss = nullptr, *p = nullptr, *s = nullptr;
  if (!profile(sensorPrimary, sp, why) || !profile(sensorSecondary, ss, why) ||
      !profile(primary, p, why) || !profile(secondary, s, why) ||
      !metadataValid(metadata, sensorPrimary, sensorSecondary, why))
    return false;
  Profile fullPrimary, fullSecondary;
  if (!mapped(sensorPrimary, primary, metadata, fullPrimary, why) ||
      !mapped(sensorSecondary, secondary, metadata, fullSecondary, why))
    return false;
  if (!profile_contract::sameSpace(p->descriptor, s->descriptor))
    return fail(why, "两路量测轮廓必须具有相同坐标帧、分析平面和手性");
  if (!mergedValid(primary, secondary, merged, fullPrimary, fullSecondary, provenance, why))
    return false;
  if (metadata.guard["taken"] != "both" &&
      profile_contract::context(primary) != profile_contract::context(secondary))
    return fail(why, "guard单路复制必须同时保留实际量测原槽和处理stage");
  Json roleInfo = Json::object();
  Record roleProvenance[2];
  const Record* provided[2]{&metadata.primaryProvenance, &metadata.secondaryProvenance};
  const Data* current[2]{&primary, &secondary};
  const Profile* full[2]{&fullPrimary, &fullSecondary};
  for (int i = 0; i < 2; ++i) {
    Json sources;
    if (provided[i]->type.empty()) {
      if (!profile_contract::rebindProvenance(Data::profile(*full[i]), *current[i], nullptr,
                                              roleProvenance[i], why))
        return false;
    } else {
      if (!profile_contract::profileSources(*current[i], provided[i], sources, why) ||
          provided[i]->data["kind"] != "single_source")
        return fail(why, "角色轮廓来源必须严格绑定当前单路实际Profile");
      roleProvenance[i] = *provided[i];
    }
  }
  for (const auto& entry : {std::make_tuple("primary", &sensorPrimary, &primary, 0),
                            std::make_tuple("secondary", &sensorSecondary, &secondary, 1)}) {
    const auto& sensor = *std::get<1>(entry);
    roleInfo[std::get<0>(entry)] = {
        {"logicalRole", std::get<0>(entry)},
        {"modelRow", std::get<3>(entry)},
        {"actualSourceId", sensor.asProfile()->descriptor.sourceId},
        {"sensorStage", "raw"},
        {"measurementStage", metadata.stage},
        {"sensorContext", profile_contract::context(sensor)},
        {"measurementContext", profile_contract::context(*std::get<2>(entry))},
        {"provenance", roleProvenance[std::get<3>(entry)].data}};
  }
  result = {kScanInfoType,
            {{"schemaVersion", 2},
             {"sensorFrame", metadata.sensorFrame},
             {"measurementFrame", metadata.measurementFrame},
             {"stage", metadata.stage},
             {"roles", std::move(roleInfo)},
             {"guard", metadata.guard},
             {"crop", metadata.crop},
             {"origins", metadata.origins},
             {"processing", metadata.processing}}};
  if (!small(result.data))
    return fail(why, "完整场景信息超过64KiB预算");
  return true;
}
bool roiValid(const RoiFields& fields, std::string& why) {
  const auto& info = fields.info.data;
  bool correctKeys = keys(info, {"schemaVersion", "datumSide", "source", "alignment", "overallMm",
                                 "cropStatus", "processing"});
  if (info.is_object() && info.contains("roisFrom")) {
    auto without = info;
    without.erase("roisFrom");
    correctKeys = keys(without, {"schemaVersion", "datumSide", "source", "alignment", "overallMm",
                                 "cropStatus", "processing"}) &&
                  (info["roisFrom"] == "template" || info["roisFrom"] == "config" ||
                   info["roisFrom"] == "model");
  }
  if (fields.info.type != kRoiInfoType || !correctKeys)
    return fail(why, "角色框需要完整gap.RoiInfo.v2元数据");
  if (!info["schemaVersion"].is_number_integer() || info["schemaVersion"] != 2 ||
      (info["datumSide"] != "left" && info["datumSide"] != "right") ||
      (info["source"] != "template" && info["source"] != "model" && info["source"] != "config") ||
      (!info["alignment"].is_null() && !info["alignment"].is_object()) ||
      (!info["cropStatus"].is_null() && !info["cropStatus"].is_object()) ||
      !processing(info["processing"]) || !small(info))
    return fail(why, "角色框来源、方向或处理状态非法，不能伪造空数值");
  if (!info["overallMm"].is_null()) {
    if (!info["overallMm"].is_array() || info["overallMm"].size() != 4)
      return fail(why, "整体框必须为真实毫米边界或null");
    for (const auto& value : info["overallMm"])
      if (!value.is_number() || !std::isfinite(value.get<double>()))
        return fail(why, "整体框必须含有限边界");
    if (info["overallMm"][0].get<double>() > info["overallMm"][2].get<double>() ||
        info["overallMm"][1].get<double>() > info["overallMm"][3].get<double>())
      return fail(why, "整体框边界倒置");
  }
  ProfileDescriptor first;
  for (std::size_t i = 0; i < fields.boxes.size(); ++i) {
    ProfileDescriptor descriptor;
    const auto* context = fields.contexts[i].asRecord();
    if (!fields.boxes[i].asBox2D() || !context || context->type != "profile.Geometry.v2" ||
        !profile_contract::geometryMatchesPhysical(fields.boxes[i], context, descriptor, why) ||
        descriptor.plane != "XY" || (i && !profile_contract::sameSpace(first, descriptor)))
      return fail(why, "四个角色框必须严格绑定同一量测XY空间的版本2几何来源");
    if (!i)
      first = std::move(descriptor);
  }
  return true;
}
} // namespace

const Data* ScanView::pick(const std::string& role) const {
  return role == "primary"     ? primary
         : role == "secondary" ? secondary
         : role == "merged"    ? merged
                               : nullptr;
}
const Data* RoiView::role(const std::string& name) const {
  for (std::size_t i = 0; i < roles.size(); ++i)
    if (name == roles[i])
      return boxes[i];
  return nullptr;
}
const Data* RoiView::context(const std::string& name) const {
  for (std::size_t i = 0; i < roles.size(); ++i)
    if (name == roles[i])
      return contexts[i];
  return nullptr;
}
bool makeScanPair(const Data& sensorPrimary, const Data& sensorSecondary, const Data& primary,
                  const Data& secondary, const Data& merged, const Record& mergedProvenance,
                  const ScanMetadata& metadata, Data& result, std::string& why) {
  try {
    Record info;
    if (!scanInfo(sensorPrimary, sensorSecondary, primary, secondary, merged, mergedProvenance,
                  metadata, info, why))
      return false;
    Bundle bundle(kScanPairKind);
    bundle.set("sensorPrimary", sensorPrimary)
        .set("sensorSecondary", sensorSecondary)
        .set("primary", primary)
        .set("secondary", secondary)
        .set("merged", merged)
        .set("mergedProvenance", Data::record(mergedProvenance))
        .set("info", Data::record(std::move(info)));
    result = Data::bundle(std::move(bundle));
    why.clear();
    return true;
  } catch (const Json::exception&) {
    return fail(why, "场景来源或元数据字段类型/编码非法");
  }
}
bool readScanPair(const Data& data, ScanView& result, std::string& why) {
  try {
    const auto* bundle = data.asBundle();
    if (!exactBundle(bundle, kScanPairKind,
                     {"sensorPrimary", "sensorSecondary", "primary", "secondary", "merged",
                      "mergedProvenance", "info"},
                     why))
      return false;
    ScanView candidate{
        bundle->field("sensorPrimary"),   bundle->field("sensorSecondary"),
        bundle->field("primary"),         bundle->field("secondary"),
        bundle->field("merged"),          bundle->field("mergedProvenance")->asRecord(),
        bundle->field("info")->asRecord()};
    if (!candidate.info || candidate.info->type != kScanInfoType || !candidate.mergedProvenance ||
        !keys(candidate.info->data, {"schemaVersion", "sensorFrame", "measurementFrame", "stage",
                                     "roles", "guard", "crop", "origins", "processing"}) ||
        !candidate.info->data["schemaVersion"].is_number_integer() ||
        candidate.info->data["schemaVersion"] != 2)
      return fail(why, "场景Bundle缺少严格绑定的来源与处理元数据");
    const auto& info = candidate.info->data;
    ScanMetadata metadata{info["sensorFrame"].get<std::string>(),
                          info["measurementFrame"].get<std::string>(),
                          info["stage"].get<std::string>(),
                          info["guard"],
                          info["crop"],
                          info["origins"],
                          info["processing"]};
    if (!info["roles"].is_object() || !info["roles"].contains("primary") ||
        !info["roles"].contains("secondary") || !info["roles"]["primary"].is_object() ||
        !info["roles"]["secondary"].is_object() ||
        !info["roles"]["primary"].contains("provenance") ||
        !info["roles"]["secondary"].contains("provenance"))
      return fail(why, "角色来源缺少原生轮廓的完整provenance绑定");
    metadata.primaryProvenance = {"profile.CompositeSources.v1",
                                  info["roles"]["primary"]["provenance"]};
    metadata.secondaryProvenance = {"profile.CompositeSources.v1",
                                    info["roles"]["secondary"]["provenance"]};
    Record expected;
    if (!scanInfo(*candidate.sensorPrimary, *candidate.sensorSecondary, *candidate.primary,
                  *candidate.secondary, *candidate.merged, *candidate.mergedProvenance, metadata,
                  expected, why))
      return false;
    if (expected.data != info)
      return fail(why, "场景逻辑角色、stage或完整上下文与实际载荷不匹配");
    candidate.primaryProvenance = metadata.primaryProvenance;
    candidate.secondaryProvenance = metadata.secondaryProvenance;
    result = candidate;
    why.clear();
    return true;
  } catch (const Json::exception&) {
    return fail(why, "场景Bundle字段类型/编码非法");
  }
}
bool makeRoiSet(const RoiFields& fields, Data& result, std::string& why) {
  try {
    if (!roiValid(fields, why))
      return false;
    Bundle bundle(kRoiSetKind);
    for (std::size_t i = 0; i < roles.size(); ++i)
      bundle.set(roles[i], fields.boxes[i]).set(contextNames[i], fields.contexts[i]);
    bundle.set("info", Data::record(fields.info));
    result = Data::bundle(std::move(bundle));
    why.clear();
    return true;
  } catch (const Json::exception&) {
    return fail(why, "角色框元数据字段类型/编码非法");
  }
}
bool readRoiSet(const Data& data, RoiView& result, std::string& why) {
  try {
    const auto* bundle = data.asBundle();
    if (!exactBundle(bundle, kRoiSetKind,
                     {"datum", "target", "seamLeft", "seamRight", "datumContext", "targetContext",
                      "seamLeftContext", "seamRightContext", "info"},
                     why))
      return false;
    RoiFields fields;
    RoiView candidate;
    for (std::size_t i = 0; i < roles.size(); ++i) {
      candidate.boxes[i] = bundle->field(roles[i]);
      candidate.contexts[i] = bundle->field(contextNames[i]);
      fields.boxes[i] = *candidate.boxes[i];
      fields.contexts[i] = *candidate.contexts[i];
    }
    candidate.info = bundle->field("info")->asRecord();
    if (!candidate.info)
      return fail(why, "角色框缺少业务来源记录");
    fields.info = *candidate.info;
    if (!roiValid(fields, why))
      return false;
    result = candidate;
    why.clear();
    return true;
  } catch (const Json::exception&) {
    return fail(why, "角色框Bundle字段类型/编码非法");
  }
}
void registerProfileBundles(Registry& registry) {
  registry.addBundle({kScanPairKind,
                      "原生轮廓对",
                      "保留传感器原槽、量测轮廓与明确复合来源的版本2场景。",
                      "",
                      {{"sensorPrimary", "Profile", "模型row0所选真实传感器来源，XZ原槽。"},
                       {"sensorSecondary", "Profile", "模型row1所选真实传感器来源，XZ原槽。"},
                       {"primary", "Profile", "primary角色的量测XY轮廓。"},
                       {"secondary", "Profile", "secondary角色的量测XY轮廓。"},
                       {"merged", "Profile", "secondary后primary的原索引区间，允许去噪选槽。"},
                       {"mergedProvenance", "Record", "profile.CompositeSources.v1完整来源绑定。"},
                       {"info", "Record", "实际软件来源声明、角色、stage、guard与处理状态。"}}});
  registry.addBundle({kRoiSetKind,
                      "带来源角色框",
                      "四角色米制框及各自完整版本2几何上下文。",
                      "",
                      {{"datum", "Box2D", "基准面。"},
                       {"target", "Box2D", "参考面。"},
                       {"seamLeft", "Box2D", "左侧缝。"},
                       {"seamRight", "Box2D", "右侧缝。"},
                       {"datumContext", "Record", "datum的profile.Geometry.v2。"},
                       {"targetContext", "Record", "target的profile.Geometry.v2。"},
                       {"seamLeftContext", "Record", "seamLeft的profile.Geometry.v2。"},
                       {"seamRightContext", "Record", "seamRight的profile.Geometry.v2。"},
                       {"info", "Record", "角色方向、实际定位来源与处理状态。"}}});
  registry.addBundle({kLabelRowsKind,
                      "带来源模型标签",
                      "两路1280原槽的原生标签张量及明确的传感器来源声明。",
                      "",
                      {{"labels", "Tensor", "float32[2,1280]整数类别，不压缩原槽。"},
                       {"info", "Record", "gap.LabelRowsInfo.v2来源、内容绑定和类别预算。"}}});
}
} // namespace lyflow::packs::gap::profile_scene
