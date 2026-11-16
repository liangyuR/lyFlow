#include "profile_feature_bundle.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

#include "lyflow/profile.h"
#include "lyflow_profile/provenance.h"
#include "profile_scene.h"

namespace lyflow::packs::gap::profile_scene {
namespace {
using Json = nlohmann::json;
struct Field {
  const char* name;
  const char* type;
};
const std::vector<Field> lineFields{
    {"profile", "Profile"},         {"line", "Line2D"},         {"lineContext", "Record"},
    {"innerEnd", "Point2D"},        {"pointContext", "Record"}, {"inliers", "Indices"},
    {"weightedInliers", "Indices"}, {"quality", "Record"},      {"provenance", "Record"}};
const std::vector<Field> referenceFields{{"profile", "Profile"},     {"line", "Line2D"},
                                         {"lineContext", "Record"},  {"point", "Point2D"},
                                         {"pointContext", "Record"}, {"selection", "Record"},
                                         {"quality", "Record"},      {"provenance", "Record"}};
const std::vector<Field> circleFields{
    {"left", "Circle2D"},         {"leftContext", "Record"},   {"leftProfile", "Profile"},
    {"leftProvenance", "Record"}, {"leftInliers", "Indices"},  {"right", "Circle2D"},
    {"rightContext", "Record"},   {"rightProfile", "Profile"}, {"rightProvenance", "Record"},
    {"rightInliers", "Indices"},  {"quality", "Record"}};
const std::vector<Field> locationFields{{"sensorPrimary", "Profile"},
                                        {"sensorSecondary", "Profile"},
                                        {"primary", "Profile"},
                                        {"secondary", "Profile"},
                                        {"merged", "Profile"},
                                        {"mergedProvenance", "Record"},
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
bool fail(std::string& why, const char* message) {
  why = message;
  return false;
}
bool has(const Data& value) { return value.kind() != Data::Kind::None; }
bool contains(const Json& list, const Json& value) {
  return std::find(list.begin(), list.end(), value) != list.end();
}
bool join(Json& into, const Json& from, std::string& why) {
  for (const auto& value : from)
    if (!contains(into, value))
      into.push_back(value);
  return into.size() <= 64 ? true : fail(why, "原子特征来源超过64项预算");
}
bool integer(const Json& value, std::size_t expected) {
  return (value.is_number_unsigned() ||
          (value.is_number_integer() && value.get<std::int64_t>() >= 0)) &&
         value.get<std::uint64_t>() == expected;
}
bool number(const Json& value, double expected) {
  return value.is_number() && value.get<double>() == expected;
}
bool bounded(const Json& value, unsigned depth = 0) {
  if (depth > 16 || (value.is_number_float() && !std::isfinite(value.get<double>())) ||
      (value.is_string() && value.get_ref<const std::string&>().size() > 4096) ||
      (value.is_structured() && value.size() > 128))
    return false;
  if (value.is_structured())
    for (const auto& child : value)
      if (!bounded(child, depth + 1))
        return false;
  return true;
}
bool smallRecord(const Record* record, std::string& why) {
  return record && record->data.is_object() && bounded(record->data) &&
                 record->data.dump().size() <= 65536
             ? true
             : fail(why, "特征质量/选择记录必须有限且小于64KiB，不能承载大型数组");
}
bool exact(const Data& data, const char* kind, const std::vector<Field>& expected,
           std::string& why) {
  const auto* bundle = data.asBundle();
  if (!bundle || bundle->kind != kind || bundle->fields.size() != expected.size())
    return fail(why, "原子特征Bundle类型或字段数不匹配");
  std::set<std::string> seen;
  for (const auto& value : bundle->fields) {
    const auto found = std::find_if(expected.begin(), expected.end(),
                                    [&](const Field& field) { return field.name == value.first; });
    if (!seen.insert(value.first).second || found == expected.end() ||
        value.second.typeName() != std::string(found->type))
      return fail(why, "原子特征Bundle字段重复、未知、缺失或类型不符");
  }
  return true;
}
bool profileSources(const Data& profile, const Data& provenance, Json& sources, std::string& why) {
  const auto* record = provenance.asRecord();
  return record && profile_contract::profileSources(profile, record, sources, why)
             ? true
             : fail(why, "特征需要严格绑定当前原生Profile的完整provenance");
}
bool geometry(const Data& profile, const Data& value, const Data& context,
              const Json& expectedSources, Json& sources, std::string& why) {
  const auto* record = context.asRecord();
  ProfileDescriptor descriptor;
  if (!profile.asProfile() || !record || record->type != "profile.Geometry.v2" ||
      !profile_contract::geometryMatchesPhysical(value, record, descriptor, why, &sources) ||
      !profile_contract::sameSpace(profile.asProfile()->descriptor, descriptor))
    return fail(why, "特征几何必须绑定实际内容与轮廓的同一物理空间");
  for (const auto& expected : expectedSources)
    if (!contains(sources, expected))
      return fail(why, "特征几何丢失当前Profile或其完整来源");
  return true;
}
bool indexValues(const Data& profile, const Data& indices, bool unique, std::string& why) {
  const auto* p = profile.asProfile();
  const auto* value = indices.asIndices();
  if (!p || !value || value->sourceCloudId != p->cloud->id || value->values.empty())
    return fail(why, "特征索引必须非空且绑定实际Profile.cloud.id");
  std::int32_t previous = -1;
  for (auto slot : value->values) {
    if (slot < 0 || static_cast<std::size_t>(slot) >= p->pointCount() || !p->validity[slot] ||
        (unique && slot <= previous))
      return fail(why, "特征索引越界、指向无效槽或未按唯一有序契约排列");
    previous = slot;
  }
  return true;
}
bool observedPoint(const Data& profile, const Data& point, const std::vector<std::int32_t>* slots,
                   std::string& why) {
  if (!point.asPoint2D())
    return fail(why, "特征端点必须为实际有效米制采样点");
  Profile metric;
  if (!convertProfileUnit(*profile.asProfile(), "m", metric, why))
    return false;
  const int a = metric.descriptor.plane == "YZ" ? 1 : 0;
  const int b = metric.descriptor.plane == "XY" ? 1 : 2;
  const auto matches = [&](std::size_t slot) {
    return metric.validity[slot] && metric.cloud->xyz[3 * slot + a] == point.asPoint2D()->p[0] &&
           metric.cloud->xyz[3 * slot + b] == point.asPoint2D()->p[1];
  };
  if (slots) {
    for (auto slot : *slots)
      if (matches(static_cast<std::size_t>(slot)))
        return true;
  } else {
    for (std::size_t slot = 0; slot < metric.pointCount(); ++slot)
      if (matches(slot))
        return true;
  }
  return fail(why, "特征点不是对应原生Profile中的有效观测槽");
}
bool lineQuality(const Data& profile, const Data& line, const Data& quality, std::string& why) {
  const auto* record = quality.asRecord();
  if (!smallRecord(record, why) || record->type != "profile.LineFitRobust.v1")
    return fail(why, "拟合特征需要公共鲁棒直线质量记录");
  const auto& q = record->data;
  const auto* p = profile.asProfile();
  const auto valid =
      static_cast<std::size_t>(std::count(p->validity.begin(), p->validity.end(), 1));
  if (!integer(q.at("schemaVersion"), 1) || q.at("context") != profile_contract::context(profile) ||
      q.at("geometryHash") != profile_contract::geometryHashV2(line) || q.at("unit") != "m" ||
      !integer(q.at("inputSlotCount"), p->pointCount()) || !integer(q.at("validSlotCount"), valid))
    return fail(why, "直线质量与实际Profile、几何内容或观测计数不匹配");
  const auto& unique = q.at("uniqueInlierCount");
  const auto& weighted = q.at("weightedInlierCount");
  if ((!unique.is_number_unsigned() && !unique.is_number_integer()) ||
      (!weighted.is_number_unsigned() && !weighted.is_number_integer()) ||
      (unique.is_number_integer() && unique.get<std::int64_t>() <= 0) ||
      (weighted.is_number_integer() && weighted.get<std::int64_t>() <= 0) ||
      unique.get<std::uint64_t>() == 0 || unique.get<std::uint64_t>() > valid ||
      weighted.get<std::uint64_t>() < unique.get<std::uint64_t>() ||
      !number(q.at("uniqueInlierRatio"),
              static_cast<double>(unique.get<std::uint64_t>()) / valid) ||
      q.at("method") != "ransac_dual_trace_segment_direction" ||
      q.at("residualAggregation") != "final_inlier_multiset" ||
      !q.at("directionPinned").is_boolean() || !q.at("segmentApplied").is_boolean())
    return fail(why, "直线质量的真实观测/权重计数或方法声明非法");
  for (const auto* field : {"weightedRmsResidual", "maxResidual"})
    if (!q.at(field).is_number() || q.at(field).get<double>() < 0)
      return fail(why, "直线质量残差必须是有限非负米制数值");
  return true;
}
bool lineValid(const LineFeatureFields& f, std::string& why) {
  Json expected, lineSources, pointSources;
  if (!profileSources(f.profile, f.provenance, expected, why) || !f.line.asLine2D() ||
      !geometry(f.profile, f.line, f.lineContext, expected, lineSources, why) ||
      !geometry(f.profile, f.innerEnd, f.pointContext, expected, pointSources, why) ||
      !indexValues(f.profile, f.inliers, true, why) ||
      !indexValues(f.profile, f.weightedInliers, false, why) ||
      !lineQuality(f.profile, f.line, f.quality, why))
    return false;
  auto unique = f.weightedInliers.asIndices()->values;
  std::sort(unique.begin(), unique.end());
  unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
  if (unique != f.inliers.asIndices()->values ||
      !integer(f.quality.asRecord()->data.at("uniqueInlierCount"), unique.size()) ||
      !integer(f.quality.asRecord()->data.at("weightedInlierCount"),
               f.weightedInliers.asIndices()->values.size()))
    return fail(why, "唯一内点、最终权重及质量计数不匹配");
  return observedPoint(f.profile, f.innerEnd, &unique, why);
}
bool selectionMap(const Data& profile, const Json& data, const Json& quality, std::string& why) {
  if (!integer(data.at("schemaVersion"), 1) ||
      data.at("context") != profile_contract::context(profile) ||
      data.at("sourceCloudId") != std::to_string(profile.asProfile()->cloud->id) ||
      data.at("indexSpace") != "profile_slots" ||
      data.at("sourceIndexMapping") != "profile.sourceIndices" ||
      data.at("indexCount") != quality.at("uniqueInlierCount") ||
      data.at("weightedInlierCount") != quality.at("weightedInlierCount"))
    return fail(why, "拟合选择元数据与实际Profile或质量计数不匹配");
  return true;
}
bool referenceValid(const ReferenceFeatureFields& f, std::string& why) {
  Json expected, pointSources;
  if (!profileSources(f.profile, f.provenance, expected, why) || !f.line.asLine2D() ||
      !geometry(f.profile, f.point, f.pointContext, expected, pointSources, why) ||
      !observedPoint(f.profile, f.point, nullptr, why) ||
      !smallRecord(f.selection.asRecord(), why) || !smallRecord(f.quality.asRecord(), why))
    return false;
  const auto* quality = f.quality.asRecord();
  const auto* selection = f.selection.asRecord();
  if (quality->type == "profile.LineFitRobust.v1") {
    Json lineSources;
    if (!geometry(f.profile, f.line, f.lineContext, expected, lineSources, why) ||
        !lineQuality(f.profile, f.line, f.quality, why) ||
        selection->type != "gap.ReferenceSelection.v2" ||
        !integer(selection->data.at("schemaVersion"), 2) ||
        selection->data.at("method") != "line_end" || selection->data.at("lineFitted") != true ||
        selection->data.at("pointContext") != f.pointContext.asRecord()->data ||
        !selectionMap(f.profile, selection->data.at("fitSelection"), quality->data, why))
      return fail(why, "拟合参考点需要同源直线、点与选择记录");
    return true;
  }
  const auto* unavailable = f.lineContext.asRecord();
  if (quality->type != "gap.ReferencePoint.v2" || !integer(quality->data.at("schemaVersion"), 2) ||
      quality->data.at("lineFitted") != false ||
      (quality->data.at("model") != "selected_point" &&
       quality->data.at("model") != "nearest_point") ||
      !integer(quality->data.at("pointCount"), f.profile.asProfile()->pointCount()) ||
      quality->data.at("selection") != selection->data || !unavailable ||
      unavailable->type != "gap.GeometryUnavailable.v1" || !smallRecord(unavailable, why) ||
      !integer(unavailable->data.at("schemaVersion"), 1) ||
      unavailable->data.at("available") != false ||
      unavailable->data.at("reason") != "method_has_no_fitted_line" ||
      unavailable->data.at("method") != quality->data.at("model") ||
      unavailable->data.at("sources") != pointSources)
    return fail(why, "非拟合参考点必须明确标记直线几何不可用");
  const auto* placeholder = f.line.asLine2D();
  if (placeholder->unit != Unit2D::Meter || placeholder->hasSegment ||
      placeholder->point[0] != f.point.asPoint2D()->p[0] ||
      placeholder->point[1] != f.point.asPoint2D()->p[1] || placeholder->dir[0] != 1 ||
      placeholder->dir[1] != 0)
    return fail(why, "非拟合显示线必须为真实选中点上的标准不可测占位载荷");
  if (selection->type != "profile.SampleSelection.v1" ||
      !integer(selection->data.at("schemaVersion"), 1) ||
      selection->data.at("context") != profile_contract::context(f.profile) ||
      selection->data.at("sourceCloudId") != std::to_string(f.profile.asProfile()->cloud->id) ||
      selection->data.at("indexSpace") != "profile_slots")
    return fail(why, "选点记录与当前Profile不匹配");
  const auto slot = selection->data.at("slotIndex");
  if ((!slot.is_number_unsigned() && !slot.is_number_integer()) ||
      (slot.is_number_integer() && slot.get<std::int64_t>() < 0) ||
      slot.get<std::uint64_t>() >= f.profile.asProfile()->pointCount())
    return fail(why, "选点记录槽号越界");
  const auto index = slot.get<std::size_t>();
  if (!integer(selection->data.at("sourceIndex"), f.profile.asProfile()->sourceIndices[index]) ||
      !selection->data.at("distance").is_number() ||
      selection->data.at("distance").get<double>() < 0 || selection->data.at("unit") != "m")
    return fail(why, "选点记录物理源索引或距离非法");
  if (index > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
    return fail(why, "选点记录超出原生槽索引范围");
  Json resolved;
  if (!profile_contract::resolveSourceIndex(f.profile, f.provenance.asRecord(),
                                            f.profile.asProfile()->sourceIndices[index], resolved,
                                            why))
    return false;
  if ((selection->data.contains("source") && selection->data.at("source") != resolved) ||
      (!selection->data.contains("source") &&
       f.provenance.asRecord()->data.at("kind") == "derived_composite"))
    return fail(why, "选点记录必须保留准确的复合物理来源");
  const std::vector<std::int32_t> selected{static_cast<std::int32_t>(index)};
  return observedPoint(f.profile, f.point, &selected, why);
}
bool circleValid(const CirclePairFeatureFields& f, std::string& why) {
  if (!smallRecord(&f.quality, why) || f.quality.type != "gap.CircleFitPair.v2" ||
      !integer(f.quality.data.at("schemaVersion"), 2))
    return fail(why, "双圆特征需要原生双侧圆质量记录");
  ProfileDescriptor first;
  for (std::size_t side = 0; side < 2; ++side) {
    Json expected, sources;
    if (!profileSources(f.profiles[side], f.provenances[side], expected, why) ||
        !f.circles[side].asCircle2D() ||
        !geometry(f.profiles[side], f.circles[side], f.contexts[side], expected, sources, why) ||
        !indexValues(f.profiles[side], f.inliers[side], true, why))
      return false;
    const auto& descriptor = f.profiles[side].asProfile()->descriptor;
    if (side && !profile_contract::sameSpace(first, descriptor))
      return fail(why, "双圆特征两侧必须位于同一物理空间");
    first = descriptor;
    const auto* name = side ? "right" : "left";
    const auto& q = f.quality.data.at(name);
    const auto* circle = f.circles[side].asCircle2D();
    if (q.at("logicalRole") != name ||
        q.at("profileContext") != profile_contract::context(f.profiles[side]) ||
        !integer(q.at("pointCount"), f.profiles[side].asProfile()->pointCount()) ||
        !integer(q.at("inlierCount"), f.inliers[side].asIndices()->values.size()) ||
        !number(q.at("centerXMm"), static_cast<double>(circle->center[0]) * 1000) ||
        !number(q.at("centerYMm"), static_cast<double>(circle->center[1]) * 1000) ||
        !number(q.at("radiusMm"), static_cast<double>(circle->radius) * 1000))
      return fail(why, "双圆质量与实际圆、原生Profile或内点数不匹配");
  }
  return true;
}
bool locationValid(const Data& scanData, const Data& roiData, std::string& why) {
  ScanView scan;
  RoiView rois;
  if (!readScanPair(scanData, scan, why) || !readRoiSet(roiData, rois, why))
    return false;
  const Data* profiles[]{scan.primary, scan.secondary, scan.merged};
  const Record* provenance[]{&scan.primaryProvenance, &scan.secondaryProvenance,
                             scan.mergedProvenance};
  Json known = Json::array();
  for (int i = 0; i < 3; ++i) {
    Json sources;
    if (!profile_contract::profileSources(*profiles[i], provenance[i], sources, why) ||
        !join(known, sources, why))
      return false;
  }
  for (const auto* sensor : {scan.sensorPrimary, scan.sensorSecondary}) {
    Profile axes, metric;
    if (!remapProfileAxes(*sensor->asProfile(), "XZY", {1, 1, 1},
                          scan.primary->asProfile()->descriptor.frame, axes, why) ||
        !convertProfileUnit(axes, "m", metric, why) ||
        !join(known, Json::array({profile_contract::context(Data::profile(std::move(metric)))}),
              why))
      return false;
  }
  Json unioned = known;
  for (std::size_t role = 0; role < 4; ++role) {
    const auto* record = rois.contexts[role]->asRecord();
    ProfileDescriptor descriptor;
    Json sources;
    if (!record || record->type != "profile.Geometry.v2" ||
        !profile_contract::geometryMatchesPhysical(*rois.boxes[role], record, descriptor, why,
                                                   &sources) ||
        !profile_contract::sameSpace(scan.primary->asProfile()->descriptor, descriptor))
      return fail(why, "定位角色框必须与实际scan处于同一物理空间");
    if (std::none_of(sources.begin(), sources.end(),
                     [&](const Json& source) { return contains(known, source); }))
      return fail(why, "定位角色框没有绑定本次scan的完整量测来源上下文");
    if (!join(unioned, sources, why))
      return false;
  }
  return true;
}
}

bool makeLocation(const Data& scan, const Data& rois, Data& result, std::string& why) {
  try {
    if (!locationValid(scan, rois, why))
      return false;
    Bundle bundle(kLocationKind);
    for (const auto& field : scan.asBundle()->fields)
      bundle.set(field.first == "info" ? "scanInfo" : field.first, field.second);
    for (const auto& field : rois.asBundle()->fields)
      bundle.set(field.first == "info" ? "roiInfo" : field.first, field.second);
    result = Data::bundle(std::move(bundle));
    why.clear();
    return true;
  } catch (const std::exception& error) {
    why = error.what();
    return false;
  }
}
bool readLocation(const Data& data, LocationFields& result, std::string& why) {
  try {
    if (!exact(data, kLocationKind, locationFields, why))
      return false;
    Bundle scan(kScanPairKind), rois(kRoiSetKind);
    for (std::size_t i = 0; i < locationFields.size(); ++i) {
      const auto* name = locationFields[i].name;
      const auto target =
          std::string(name) == "scanInfo" || std::string(name) == "roiInfo" ? "info" : name;
      (i < 7 ? scan : rois).set(target, *data.asBundle()->field(name));
    }
    LocationFields candidate{Data::bundle(std::move(scan)), Data::bundle(std::move(rois))};
    if (!locationValid(candidate.scan, candidate.rois, why))
      return false;
    result = std::move(candidate);
    why.clear();
    return true;
  } catch (const std::exception& error) {
    why = error.what();
    return false;
  }
}
bool makeLineFeature(const LineFeatureFields& f, Data& result, std::string& why) {
  try {
    if (!lineValid(f, why))
      return false;
    Bundle bundle(kLineFeatureKind);
    const Data* values[]{&f.profile,         &f.line,         &f.lineContext,
                         &f.innerEnd,        &f.pointContext, &f.inliers,
                         &f.weightedInliers, &f.quality,      &f.provenance};
    for (std::size_t i = 0; i < lineFields.size(); ++i)
      bundle.set(lineFields[i].name, *values[i]);
    result = Data::bundle(std::move(bundle));
    why.clear();
    return true;
  } catch (const std::exception& error) {
    why = error.what();
    return false;
  }
}
bool readLineFeature(const Data& data, LineFeatureFields& result, std::string& why) {
  try {
    if (!exact(data, kLineFeatureKind, lineFields, why))
      return false;
    const auto value = [&](const char* name) { return *data.asBundle()->field(name); };
    LineFeatureFields candidate{
        value("profile"),         value("line"),         value("lineContext"),
        value("innerEnd"),        value("pointContext"), value("inliers"),
        value("weightedInliers"), value("quality"),      value("provenance")};
    if (!lineValid(candidate, why))
      return false;
    result = std::move(candidate);
    why.clear();
    return true;
  } catch (const std::exception& error) {
    why = error.what();
    return false;
  }
}
bool makeReferenceFeature(const ReferenceFeatureFields& f, Data& result, std::string& why) {
  try {
    if (!referenceValid(f, why))
      return false;
    Bundle bundle(kReferenceFeatureKind);
    const Data* values[]{&f.profile,      &f.line,      &f.lineContext, &f.point,
                         &f.pointContext, &f.selection, &f.quality,     &f.provenance};
    for (std::size_t i = 0; i < referenceFields.size(); ++i)
      bundle.set(referenceFields[i].name, *values[i]);
    result = Data::bundle(std::move(bundle));
    why.clear();
    return true;
  } catch (const std::exception& error) {
    why = error.what();
    return false;
  }
}
bool readReferenceFeature(const Data& data, ReferenceFeatureFields& result, std::string& why) {
  try {
    if (!exact(data, kReferenceFeatureKind, referenceFields, why))
      return false;
    const auto value = [&](const char* name) { return *data.asBundle()->field(name); };
    ReferenceFeatureFields candidate{value("profile"), value("line"),         value("lineContext"),
                                     value("point"),   value("pointContext"), value("selection"),
                                     value("quality"), value("provenance")};
    if (!referenceValid(candidate, why))
      return false;
    result = std::move(candidate);
    why.clear();
    return true;
  } catch (const std::exception& error) {
    why = error.what();
    return false;
  }
}
bool completeReferenceFeature(const std::string& method, const ReferenceFeatureFields& partial,
                              ReferenceFeatureFields& result, std::string& why) {
  try {
    ReferenceFeatureFields candidate = partial;
    if (method == "line_end") {
      const auto* map = partial.selection.asRecord();
      if (!map || map->type != "profile.Selection.v1" || !partial.pointContext.asRecord())
        return fail(why, "line_end需要公共拟合indexMap与点上下文");
      candidate.selection =
          Data::record(Record{"gap.ReferenceSelection.v2",
                              {{"schemaVersion", 2},
                               {"method", method},
                               {"lineFitted", true},
                               {"pointContext", partial.pointContext.asRecord()->data},
                               {"fitSelection", map->data}}});
    } else if (method == "selected_point" || method == "nearest_point") {
      if (has(partial.line) || has(partial.lineContext) || has(partial.quality) ||
          !partial.profile.asProfile() || !partial.point.asPoint2D() ||
          !partial.pointContext.asRecord() || !partial.selection.asRecord())
        return fail(why, "非拟合参考点禁止覆盖显示线、不可用上下文或质量记录");
      Line2D placeholder;
      std::copy_n(partial.point.asPoint2D()->p, 2, placeholder.point);
      candidate.line = Data::line2d(placeholder);
      candidate.lineContext =
          Data::record(Record{"gap.GeometryUnavailable.v1",
                              {{"schemaVersion", 1},
                               {"available", false},
                               {"reason", "method_has_no_fitted_line"},
                               {"method", method},
                               {"sources", partial.pointContext.asRecord()->data.at("sources")}}});
      candidate.quality =
          Data::record(Record{"gap.ReferencePoint.v2",
                              {{"schemaVersion", 2},
                               {"model", method},
                               {"lineFitted", false},
                               {"pointCount", partial.profile.asProfile()->pointCount()},
                               {"selection", partial.selection.asRecord()->data}}});
    } else {
      return fail(why, "参考点特征方法非法");
    }
    if (!referenceValid(candidate, why))
      return false;
    result = std::move(candidate);
    why.clear();
    return true;
  } catch (const std::exception& error) {
    why = error.what();
    return false;
  }
}
bool makeCirclePairFeature(const CirclePairFeatureFields& f, Data& result, std::string& why) {
  try {
    if (!circleValid(f, why))
      return false;
    Bundle bundle(kCirclePairKind);
    for (std::size_t side = 0; side < 2; ++side) {
      const std::string prefix = side ? "right" : "left";
      bundle.set(prefix, f.circles[side])
          .set(prefix + "Context", f.contexts[side])
          .set(prefix + "Profile", f.profiles[side])
          .set(prefix + "Provenance", f.provenances[side])
          .set(prefix + "Inliers", f.inliers[side]);
    }
    bundle.set("quality", Data::record(f.quality));
    result = Data::bundle(std::move(bundle));
    why.clear();
    return true;
  } catch (const std::exception& error) {
    why = error.what();
    return false;
  }
}
bool readCirclePairFeature(const Data& data, CirclePairFeatureFields& result, std::string& why) {
  try {
    if (!exact(data, kCirclePairKind, circleFields, why))
      return false;
    CirclePairFeatureFields candidate;
    for (std::size_t side = 0; side < 2; ++side) {
      const std::string prefix = side ? "right" : "left";
      candidate.circles[side] = *data.asBundle()->field(prefix);
      candidate.contexts[side] = *data.asBundle()->field(prefix + "Context");
      candidate.profiles[side] = *data.asBundle()->field(prefix + "Profile");
      candidate.provenances[side] = *data.asBundle()->field(prefix + "Provenance");
      candidate.inliers[side] = *data.asBundle()->field(prefix + "Inliers");
    }
    candidate.quality = *data.asBundle()->field("quality")->asRecord();
    if (!circleValid(candidate, why))
      return false;
    result = std::move(candidate);
    why.clear();
    return true;
  } catch (const std::exception& error) {
    why = error.what();
    return false;
  }
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
    const auto* record = inputs.get(name).asRecord();
    std::string why;
    if (!smallRecord(record, why) || record->type != "FallbackChoice" ||
        !record->data.contains("choice") || !record->data["choice"].is_string() ||
        (record->data["choice"] != "a" && record->data["choice"] != "b"))
      return bad("回退路径证据必须为明确a/b的FallbackChoice", name);
    const auto choice = record->data["choice"].get<std::string>();
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
  for (const auto& field : fields)
    result.push_back({field.name, field.type, field.name, "原子特征的实际载荷与内容绑定。", true});
  return result;
}
bool validateFeature(int kind, const Data& data, std::string& why) {
  if (kind == 0) {
    LineFeatureFields result;
    return readLineFeature(data, result, why);
  }
  if (kind == 1) {
    ReferenceFeatureFields result;
    return readReferenceFeature(data, result, why);
  }
  if (kind == 2) {
    CirclePairFeatureFields result;
    return readCirclePairFeature(data, result, why);
  }
  LocationFields result;
  return readLocation(data, result, why);
}
}
void registerProfileFeatureBundles(Registry& registry) {
  const char* kinds[]{kLineFeatureKind, kReferenceFeatureKind, kCirclePairKind, kLocationKind};
  const char* names[]{"line_feature", "reference_feature", "circle_pair", "location"};
  const std::vector<Field>* fields[]{&lineFields, &referenceFields, &circleFields, &locationFields};
  for (int kind = 0; kind < 4; ++kind) {
    BundleDesc bundle;
    bundle.kind = kinds[kind];
    bundle.label = names[kind];
    bundle.doc = "单个原子结果绑定数值、几何、原生观测和来源；fallback必须选择整个Bundle。";
    for (const auto& field : *fields[kind])
      bundle.fields.push_back({field.name, field.type, "同一特征的实际数据与内容绑定。"});
    registry.addBundle(std::move(bundle));
    for (const bool make : {true, false}) {
      OperatorDesc op;
      op.id = "gap." + std::string(make ? "make_" : "split_") + names[kind];
      op.version = "1.0.0";
      op.label = std::string(make ? "组装" : "拆分") + names[kind];
      op.category = "间隙/原子特征";
      op.keywords = {"atomic feature", "bundle", "fallback", "原子特征", "整体回退"};
      op.doc = "严格校验原生轮廓、索引、几何内容、质量与来源；失败和取消不发布部分结果。";
      op.capabilities = {true, true, true};
      if (kind == 3) {
        const std::vector<Port> locationPorts{
            {"scan", "Bundle<gap.ScanPair.v2>", "轮廓对", "严格来源绑定。", true},
            {"rois", "Bundle<gap.RoiSet.v2>", "角色框", "须含本次scan的真实完整来源。", true}};
        const std::vector<Port> atomic{{"location", "Bundle<gap.Location.v2>", "原子定位",
                                        "扁平16字段，不嵌套Bundle。", true}};
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
          for (const auto* field : {"line", "lineContext", "quality"})
            if (connected.count(field) != static_cast<std::size_t>(fitted))
              result.push_back(Issue::error("bad_input",
                                            fitted ? "line_end需要实际拟合线、上下文和质量"
                                                   : "非拟合参考点禁止覆盖占位线、上下文或质量",
                                            "method", field));
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
              return bad(why, "location");
            if (ctx.cancelled())
              return cancelled();
            out.set("location", std::move(result));
          } else {
            LocationFields location;
            if (!readLocation(in.get("location"), location, why))
              return bad(why, "location");
            if (ctx.cancelled())
              return cancelled();
            out.set("scan", std::move(location.scan));
            out.set("rois", std::move(location.rois));
          }
          return Status::Ok();
        }
        if (make) {
          if (kind == 0) {
            const LineFeatureFields feature{
                in.get("profile"),         in.get("line"),         in.get("lineContext"),
                in.get("innerEnd"),        in.get("pointContext"), in.get("inliers"),
                in.get("weightedInliers"), in.get("quality"),      in.get("provenance")};
            if (!makeLineFeature(feature, result, why))
              return bad(why);
          } else if (kind == 1) {
            const ReferenceFeatureFields partial{in.get("profile"),      in.get("line"),
                                                 in.get("lineContext"),  in.get("point"),
                                                 in.get("pointContext"), in.get("selection"),
                                                 in.get("quality"),      in.get("provenance")};
            ReferenceFeatureFields feature;
            if (!completeReferenceFeature(p.choice("method"), partial, feature, why) ||
                !makeReferenceFeature(feature, result, why))
              return bad(why);
          } else {
            const auto* quality = in.get("quality").asRecord();
            if (!quality)
              return bad("双圆特征缺少质量记录");
            CirclePairFeatureFields feature;
            feature.quality = *quality;
            for (std::size_t side = 0; side < 2; ++side) {
              const std::string prefix = side ? "right" : "left";
              feature.circles[side] = in.get(prefix);
              feature.contexts[side] = in.get(prefix + "Context");
              feature.profiles[side] = in.get(prefix + "Profile");
              feature.provenances[side] = in.get(prefix + "Provenance");
              feature.inliers[side] = in.get(prefix + "Inliers");
            }
            if (!makeCirclePairFeature(feature, result, why))
              return bad(why);
          }
          if (ctx.cancelled())
            return cancelled();
          out.set("feature", std::move(result));
        } else {
          const auto& feature = in.get("feature");
          if (!validateFeature(kind, feature, why))
            return bad(why);
          if (ctx.cancelled())
            return cancelled();
          for (const auto& field : *fields[kind])
            out.set(field.name, *feature.asBundle()->field(field.name));
        }
        return Status::Ok();
      };
      registry.addOperator(std::move(op));
    }
  }
}
}
