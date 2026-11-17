#include "profile_scene.h"

#include "lyflow/profile.h"
#include "lyflow_profile/context.h"

namespace lyflow::packs::gap::profile_scene {
namespace {
constexpr std::array<const char*, 4> roles{"datum", "target", "seamLeft", "seamRight"};
constexpr std::array<const char*, 4> contextNames{"datumContext", "targetContext",
                                                  "seamLeftContext", "seamRightContext"};
bool fail(std::string& why, const char* text) {
  why = text;
  return false;
}
bool roiValid(const std::array<const Data*, 4>& boxes, const std::array<const Data*, 4>& contexts,
              std::string& why) {
  ProfileDescriptor first;
  for (std::size_t i = 0; i < boxes.size(); ++i) {
    ProfileDescriptor space;
    if (!boxes[i]->asBox2D() ||
        !profile_contract::geometrySpace(*boxes[i], contexts[i]->asRecord(), space, why))
      return false;
    if (space.plane != "XY" || (i && !profile_contract::sameSpace(first, space)))
      return fail(why, "四个角色框必须位于同一量测XY空间");
    if (!i)
      first = std::move(space);
  }
  return true;
}
bool scanValid(const Profile& sp, const Profile& ss, const Profile& p, const Profile& s,
               const Profile& m, std::string& why) {
  if (sp.descriptor.plane != "XZ" || ss.descriptor.plane != "XZ" || p.descriptor.plane != "XY" ||
      p.descriptor.unit != "m" || s.descriptor.unit != "m" || m.descriptor.unit != "m" ||
      !profile_contract::sameSpace(p.descriptor, s.descriptor) ||
      !profile_contract::sameSpace(p.descriptor, m.descriptor))
    return fail(why, "传感器轮廓须为XZ，两路量测与合并轮廓须处于同一米制XY空间");
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
                  const Data& secondary, const Data& merged, const ScanMetadata& metadata,
                  Data& result, std::string& why) {
  const auto* sp = sensorPrimary.asProfile();
  const auto* ss = sensorSecondary.asProfile();
  const auto* p = primary.asProfile();
  const auto* s = secondary.asProfile();
  const auto* m = merged.asProfile();
  if (!sp || !ss || !p || !s || !m)
    return fail(why, "场景字段需要原生Profile");
  if (!scanValid(*sp, *ss, *p, *s, *m, why))
    return false;
  Bundle bundle(kScanPairKind);
  bundle.set("sensorPrimary", sensorPrimary)
      .set("sensorSecondary", sensorSecondary)
      .set("primary", primary)
      .set("secondary", secondary)
      .set("merged", merged)
      .set("info", Data::record({kScanInfoType,
                                 {{"schemaVersion", 3},
                                  {"sensorFrame", metadata.sensorFrame},
                                  {"measurementFrame", metadata.measurementFrame},
                                  {"stage", metadata.stage},
                                  {"guard", metadata.guard},
                                  {"crop", metadata.crop},
                                  {"origins", metadata.origins},
                                  {"processing", metadata.processing}}}));
  result = Data::bundle(std::move(bundle));
  why.clear();
  return true;
}
bool readScanPair(const Data& data, ScanView& result, std::string& why) {
  const auto* bundle = data.asBundle();
  if (!bundle || bundle->kind != kScanPairKind)
    return fail(why, "需要gap.ScanPair.v3场景");
  result = {bundle->field("sensorPrimary"), bundle->field("sensorSecondary"),
            bundle->field("primary"),       bundle->field("secondary"),
            bundle->field("merged"),        bundle->field("info")->asRecord()};
  if (!scanValid(*result.sensorPrimary->asProfile(), *result.sensorSecondary->asProfile(),
                 *result.primary->asProfile(), *result.secondary->asProfile(),
                 *result.merged->asProfile(), why))
    return false;
  why.clear();
  return true;
}
bool makeRoiSet(const RoiFields& fields, Data& result, std::string& why) {
  std::array<const Data*, 4> boxes, contexts;
  for (std::size_t i = 0; i < roles.size(); ++i) {
    boxes[i] = &fields.boxes[i];
    contexts[i] = &fields.contexts[i];
  }
  if (!roiValid(boxes, contexts, why))
    return false;
  Bundle bundle(kRoiSetKind);
  for (std::size_t i = 0; i < roles.size(); ++i)
    bundle.set(roles[i], fields.boxes[i]).set(contextNames[i], fields.contexts[i]);
  bundle.set("info", Data::record(fields.info));
  result = Data::bundle(std::move(bundle));
  why.clear();
  return true;
}
bool readRoiSet(const Data& data, RoiView& result, std::string& why) {
  const auto* bundle = data.asBundle();
  if (!bundle || bundle->kind != kRoiSetKind)
    return fail(why, "需要gap.RoiSet.v3角色框");
  for (std::size_t i = 0; i < roles.size(); ++i) {
    result.boxes[i] = bundle->field(roles[i]);
    result.contexts[i] = bundle->field(contextNames[i]);
  }
  result.info = bundle->field("info")->asRecord();
  if (!roiValid(result.boxes, result.contexts, why))
    return false;
  why.clear();
  return true;
}
void registerProfileBundles(Registry& registry) {
  registry.addBundle(
      {kScanPairKind,
       "原生轮廓对",
       "保留传感器原槽、量测轮廓与合并轮廓的场景。",
       "",
       {{"sensorPrimary", "Profile", "模型row0所选真实传感器来源，XZ原槽。"},
        {"sensorSecondary", "Profile", "模型row1所选真实传感器来源，XZ原槽。"},
        {"primary", "Profile", "primary角色的量测XY轮廓。"},
        {"secondary", "Profile", "secondary角色的量测XY轮廓。"},
        {"merged", "Profile", "secondary后primary的原索引区间，允许去噪选槽。"},
        {"info", "Record", "gap.ScanInfo.v3：帧、stage、guard、来源声明与处理状态。"}}});
  registry.addBundle({kRoiSetKind,
                      "角色框集",
                      "四角色米制框及各自profile.Geometry.v3几何上下文。",
                      "",
                      {{"datum", "Box2D", "基准面。"},
                       {"target", "Box2D", "参考面。"},
                       {"seamLeft", "Box2D", "左侧缝。"},
                       {"seamRight", "Box2D", "右侧缝。"},
                       {"datumContext", "Record", "datum的profile.Geometry.v3。"},
                       {"targetContext", "Record", "target的profile.Geometry.v3。"},
                       {"seamLeftContext", "Record", "seamLeft的profile.Geometry.v3。"},
                       {"seamRightContext", "Record", "seamRight的profile.Geometry.v3。"},
                       {"info", "Record", "角色方向、实际定位来源与处理状态。"}}});
  registry.addBundle({kLabelRowsKind,
                      "模型标签",
                      "两路1280原槽的原生标签张量。",
                      "",
                      {{"labels", "Tensor", "float32[2,1280]整数类别，不压缩原槽。"},
                       {"info", "Record", "gap.LabelRowsInfo.v3类别数。"}}});
}
} // namespace lyflow::packs::gap::profile_scene
