#pragma once

#include <cstring>
#include <limits>
#include <set>
#include <tuple>
#include <unordered_map>

#include "lyflow_profile/context.h"

namespace lyflow::profile_contract {
namespace provenance_detail {
using Json = nlohmann::json;
inline bool fail(std::string& why, const char* message) {
  why = message;
  return false;
}
inline bool keys(const Json& value, std::initializer_list<const char*> names) {
  if (!value.is_object() || value.size() != names.size())
    return false;
  for (const auto* name : names)
    if (!value.contains(name))
      return false;
  return true;
}
inline bool name(const Json& value) {
  return value.is_string() && !value.get_ref<const std::string&>().empty() &&
         value.get_ref<const std::string&>().size() <= 1024 &&
         value.get_ref<const std::string&>().find('\0') == std::string::npos;
}
inline std::string recipeHash(const std::string& kind, const Json& recipe) {
  HashBuilder hash;
  hash.add("profile-composite-source/v1");
  hash.add(kind);
  hash.add(recipe.dump());
  return hash.hex();
}
inline Json leafRecipe(const Data& data) {
  const auto& d = data.asProfile()->descriptor;
  return {{"schemaVersion", 1},
          {"unit", d.unit},
          {"frame", d.frame},
          {"plane", d.plane},
          {"handedness", d.handedness},
          {"channelPolicy", "preserve"},
          {"segments", Json::array({{{"logicalRole", "source"},
                                     {"offset", 0},
                                     {"count", d.sourceCount},
                                     {"sourceContext", context(data)}}})}};
}
inline std::string bindingHash(const Json& profileContext, const Json& recipeHash,
                               const Json& references) {
  HashBuilder hash;
  hash.add("profile-provenance-binding/v1");
  hash.add(profileContext.dump());
  hash.add(recipeHash.dump());
  hash.add(references.dump());
  return hash.hex();
}
inline Record record(const Data& data, const std::string& kind, const Json& recipe,
                     const Json& references = Json::array()) {
  const auto current = context(data);
  const auto identity = recipeHash(kind, recipe);
  return {"profile.CompositeSources.v1",
          {{"schemaVersion", 1},
           {"kind", kind},
           {"profileContext", current},
           {"recipeHash", identity},
           {"recipe", recipe},
           {"referenceSources", references},
           {"bindingHash", bindingHash(current, identity, references)}}};
}
inline bool validate(const Data& data, const Record& record, Json& sources, std::string& why) {
  const auto* profile = data.asProfile();
  if (!profile || !validateProfile(*profile, why))
    return fail(why, "来源绑定需要有效Profile");
  const auto& v = record.data;
  if (record.type != "profile.CompositeSources.v1" ||
      !keys(v, {"schemaVersion", "kind", "profileContext", "recipeHash", "recipe",
                "referenceSources", "bindingHash"}) ||
      !v["schemaVersion"].is_number_integer() || v["schemaVersion"] != 1 ||
      (v["kind"] != "single_source" && v["kind"] != "derived_composite") ||
      !fingerprint(v["recipeHash"]) || !fingerprint(v["bindingHash"]) ||
      !v["referenceSources"].is_array() || v["referenceSources"].size() > 64)
    return fail(why, "需要完整版本1复合来源声明");
  const auto& recipe = v["recipe"];
  if (!keys(recipe, {"schemaVersion", "unit", "frame", "plane", "handedness", "channelPolicy",
                     "segments"}) ||
      !recipe["schemaVersion"].is_number_integer() || recipe["schemaVersion"] != 1 ||
      !recipe["segments"].is_array() || recipe["segments"].empty() ||
      recipe["segments"].size() > 64)
    return fail(why, "复合来源配方需要1到64个有界分段");
  const auto& descriptor = profile->descriptor;
  if (recipe["unit"] != descriptor.unit || recipe["frame"] != descriptor.frame ||
      recipe["plane"] != descriptor.plane || recipe["handedness"] != descriptor.handedness ||
      !matches(data, v["profileContext"], why))
    return fail(why, "复合来源与当前轮廓内容或空间不匹配");
  const bool composite = v["kind"] == "derived_composite";
  if ((composite && recipe["channelPolicy"] != "require_equal" &&
       recipe["channelPolicy"] != "intersection") ||
      (!composite && (recipe["channelPolicy"] != "preserve" || recipe["segments"].size() != 1)))
    return fail(why, "来源配方通道策略或单源分段非法");
  if (v["recipeHash"] != recipeHash(v["kind"].get<std::string>(), recipe))
    return fail(why, "复合来源配方指纹不匹配");
  if (v["bindingHash"] != bindingHash(v["profileContext"], v["recipeHash"], v["referenceSources"]))
    return fail(why, "当前载荷与参考来源绑定指纹不匹配");
  if (composite && descriptor.sourceId != "derived:composite/" + v["recipeHash"].get<std::string>())
    return fail(why, "派生轮廓身份与来源配方不匹配");
  Json candidate = Json::array({context(data)});
  std::uint64_t offset = 0;
  std::set<std::string> roles;
  for (const auto& segment : recipe["segments"]) {
    if (!keys(segment, {"logicalRole", "offset", "count", "sourceContext"}) ||
        !name(segment["logicalRole"]) || !segment["offset"].is_number_integer() ||
        !segment["count"].is_number_integer() || segment["offset"] < 0 || segment["count"] < 0 ||
        segment["offset"] > std::numeric_limits<std::uint32_t>::max() ||
        segment["count"] > std::numeric_limits<std::uint32_t>::max())
      return fail(why, "来源分段字段、角色或范围非法");
    if (!roles.insert(segment["logicalRole"].get<std::string>()).second)
      return fail(why, "来源分段逻辑角色重复");
    ProfileDescriptor parent;
    if (!parse(segment["sourceContext"], parent, why))
      return false;
    const auto count = segment["count"].get<std::uint64_t>();
    if (segment["offset"] != offset || count != parent.sourceCount ||
        !sameSpace(descriptor, parent) || descriptor.unit != parent.unit ||
        offset + count > std::numeric_limits<std::uint32_t>::max())
      return fail(why, "来源分段必须连续、同空间同单位并保留原sourceCount");
    if (parent.sourceId.rfind("derived:composite/", 0) == 0)
      return fail(why, "复合来源必须展平到真实父来源声明");
    if (!composite &&
        (segment["logicalRole"] != "source" || parent.sourceId != descriptor.sourceId))
      return fail(why, "单源声明不得重写来源身份或角色");
    offset += count;
    const auto& source = segment["sourceContext"];
    if (std::find(candidate.begin(), candidate.end(), source) == candidate.end())
      candidate.push_back(source);
    if (candidate.size() > 64)
      return fail(why, "当前轮廓与父来源合计超过64个几何来源预算");
  }
  if (offset != descriptor.sourceCount)
    return fail(why, "来源分段未完整覆盖当前轮廓sourceCount");
  Json references = Json::array();
  for (const auto& reference : v["referenceSources"]) {
    ProfileDescriptor parent;
    if (!parse(reference, parent, why))
      return false;
    if (!sameSpace(descriptor, parent) ||
        std::find(references.begin(), references.end(), reference) != references.end())
      return fail(why, "参考来源必须同空间且完整上下文不得重复");
    references.push_back(reference);
    if (std::find(candidate.begin(), candidate.end(), reference) == candidate.end())
      candidate.push_back(reference);
    if (candidate.size() > 64)
      return fail(why, "当前轮廓、采样父来源与参考来源合计超过64项");
  }
  if (v.dump().size() > 1024U * 1024U)
    return fail(why, "复合来源声明超过1MiB预算");
  sources = std::move(candidate);
  why.clear();
  return true;
}
template <class T>
inline bool slotEqual(const std::vector<T>& a, std::size_t ai, const std::vector<T>& b,
                      std::size_t bi, std::size_t width) {
  return a.empty() == b.empty() &&
         (a.empty() ||
          std::memcmp(a.data() + ai * width, b.data() + bi * width, width * sizeof(T)) == 0);
}
} // namespace provenance_detail

inline bool profileSources(const Data& data, const Record* provenance, nlohmann::json& sources,
                           std::string& why) {
  try {
    const auto* profile = data.asProfile();
    if (!profile || !validateProfile(*profile, why))
      return provenance_detail::fail(why, "需要有效Profile来源");
    nlohmann::json candidate;
    if (provenance) {
      if (!provenance_detail::validate(data, *provenance, candidate, why))
        return false;
    } else {
      candidate = nlohmann::json::array({context(data)});
    }
    sources = std::move(candidate);
    why.clear();
    return true;
  } catch (const nlohmann::json::exception&) {
    return provenance_detail::fail(why, "轮廓来源字段类型或编码非法");
  }
}

inline bool rebindProvenance(const Data& input, const Data& output, const Record* provenance,
                             Record& result, std::string& why) {
  nlohmann::json sources;
  if (!profileSources(input, provenance, sources, why))
    return false;
  const auto* before = input.asProfile();
  const auto* after = output.asProfile();
  if (!after || !validateProfile(*after, why) ||
      profileDescriptorJson(before->descriptor) != profileDescriptorJson(after->descriptor))
    return provenance_detail::fail(why, "来源重绑定只允许同声明轮廓的保槽或选槽操作");
  std::unordered_map<std::uint32_t, std::size_t> slots;
  for (std::size_t i = 0; i < before->pointCount(); ++i)
    slots.emplace(before->sourceIndices[i], i);
  for (std::size_t i = 0; i < after->pointCount(); ++i) {
    const auto found = slots.find(after->sourceIndices[i]);
    if (found == slots.end())
      return provenance_detail::fail(why, "输出引入了输入不存在的源槽");
    const auto j = found->second;
    const auto& a = *before->cloud;
    const auto& b = *after->cloud;
    if (after->validity[i] > before->validity[j] ||
        !provenance_detail::slotEqual(a.xyz, j, b.xyz, i, 3) ||
        !provenance_detail::slotEqual(a.intensity, j, b.intensity, i, 1) ||
        !provenance_detail::slotEqual(a.normals, j, b.normals, i, 3) ||
        !provenance_detail::slotEqual(a.rgb, j, b.rgb, i, 3))
      return provenance_detail::fail(why, "来源重绑定不能修改坐标、通道或提升无效槽");
  }
  try {
    const auto recipe =
        provenance ? provenance->data.at("recipe") : provenance_detail::leafRecipe(input);
    const auto kind = provenance ? provenance->data.at("kind").get<std::string>() : "single_source";
    const auto references =
        provenance ? provenance->data.at("referenceSources") : nlohmann::json::array();
    auto candidate = provenance_detail::record(output, kind, recipe, references);
    if (!profileSources(output, &candidate, sources, why))
      return false;
    result = std::move(candidate);
    why.clear();
    return true;
  } catch (const nlohmann::json::exception&) {
    return provenance_detail::fail(why, "来源重绑定字段非法");
  }
}

inline bool extendProvenanceSources(const Data& data, const Record& provenance,
                                    const nlohmann::json& additionalSources, Record& result,
                                    std::string& why) {
  nlohmann::json sources;
  if (!profileSources(data, &provenance, sources, why))
    return false;
  if (!additionalSources.is_array() || additionalSources.size() > 256)
    return provenance_detail::fail(why, "待添加参考来源必须为有界上下文数组");
  auto references = provenance.data.at("referenceSources");
  for (const auto& reference : additionalSources) {
    ProfileDescriptor parent;
    if (!parse(reference, parent, why))
      return false;
    if (!sameSpace(data.asProfile()->descriptor, parent))
      return provenance_detail::fail(why, "参考来源与当前轮廓不同空间");
    if (std::find(sources.begin(), sources.end(), reference) == sources.end()) {
      references.push_back(reference);
      sources.push_back(reference);
    }
    if (sources.size() > 64)
      return provenance_detail::fail(why, "添加参考来源后超过64项总预算");
  }
  auto candidate = provenance_detail::record(data, provenance.data.at("kind").get<std::string>(),
                                             provenance.data.at("recipe"), references);
  if (!profileSources(data, &candidate, sources, why))
    return false;
  result = std::move(candidate);
  why.clear();
  return true;
}

inline bool makeCompositeProvenance(const Data& a, const Data& b, const Record* aProvenance,
                                    const Record* bProvenance, const std::string& roleA,
                                    const std::string& roleB, const std::string& channelPolicy,
                                    Profile& merged, Record& result, std::string& why) {
  using namespace provenance_detail;
  Json sources;
  if (!profileSources(a, aProvenance, sources, why) ||
      !profileSources(b, bProvenance, sources, why))
    return false;
  const auto& ad = a.asProfile()->descriptor;
  const auto& bd = b.asProfile()->descriptor;
  if (!sameSpace(ad, bd) || ad.unit != bd.unit || !name(Json(roleA)) || !name(Json(roleB)) ||
      roleA == roleB || roleA.find('/') != std::string::npos ||
      roleB.find('/') != std::string::npos ||
      (channelPolicy != "require_equal" && channelPolicy != "intersection") ||
      ad.sourceCount + bd.sourceCount > std::numeric_limits<std::uint32_t>::max())
    return fail(why, "合并要求同空间同单位、不同明确角色、合法通道策略与有限索引空间");
  Json segments = Json::array();
  for (const auto& entry : {std::make_tuple(&a, aProvenance, roleA, std::uint64_t(0)),
                            std::make_tuple(&b, bProvenance, roleB, ad.sourceCount)}) {
    const auto& data = *std::get<0>(entry);
    const auto* record = std::get<1>(entry);
    if (!record && data.asProfile()->descriptor.sourceId.rfind("derived:composite/", 0) == 0)
      return fail(why, "派生复合输入缺少可展平的配套来源声明");
    const auto recipe = record ? record->data.at("recipe") : leafRecipe(data);
    for (auto segment : recipe.at("segments")) {
      const auto role =
          record && record->data.at("kind") == "derived_composite"
              ? std::get<2>(entry) + "/" + segment.at("logicalRole").get<std::string>()
              : std::get<2>(entry);
      if (role.size() > 1024 || segments.size() >= 64)
        return fail(why, "展平后的复合来源分段或角色超过预算");
      segment["logicalRole"] = role;
      segment["offset"] = segment.at("offset").get<std::uint64_t>() + std::get<3>(entry);
      segments.push_back(std::move(segment));
    }
  }
  Json recipe{{"schemaVersion", 1},
              {"unit", ad.unit},
              {"frame", ad.frame},
              {"plane", ad.plane},
              {"handedness", ad.handedness},
              {"channelPolicy", channelPolicy},
              {"segments", std::move(segments)}};
  Profile candidate = merged;
  candidate.descriptor = ad;
  candidate.descriptor.sampleOrder =
      ad.sampleOrder == "acquisition" && bd.sampleOrder == "acquisition" ? "acquisition"
                                                                         : "unknown";
  candidate.descriptor.sourceCount = ad.sourceCount + bd.sourceCount;
  candidate.descriptor.sourceId = "derived:composite/" + recipeHash("derived_composite", recipe);
  if (!validateProfile(candidate, why) ||
      candidate.pointCount() != a.asProfile()->pointCount() + b.asProfile()->pointCount())
    return fail(why, "合并载荷或槽数非法");
  const auto& ac = *a.asProfile()->cloud;
  const auto& bc = *b.asProfile()->cloud;
  const auto retained = [&](bool av, bool bv) {
    return (av || bv) && (!a.asProfile()->pointCount() || av) &&
           (!b.asProfile()->pointCount() || bv);
  };
  const auto& cc = *candidate.cloud;
  if ((channelPolicy == "require_equal" && a.asProfile()->pointCount() &&
       b.asProfile()->pointCount() &&
       (ac.hasIntensity() != bc.hasIntensity() || ac.hasNormals() != bc.hasNormals() ||
        ac.hasRgb() != bc.hasRgb())) ||
      cc.hasIntensity() != retained(ac.hasIntensity(), bc.hasIntensity()) ||
      cc.hasNormals() != retained(ac.hasNormals(), bc.hasNormals()) ||
      cc.hasRgb() != retained(ac.hasRgb(), bc.hasRgb()))
    return fail(why, "合并通道不符合明确的通道策略");
  std::size_t slot = 0;
  for (const auto& entry : {std::make_pair(a.asProfile(), std::uint64_t(0)),
                            std::make_pair(b.asProfile(), ad.sourceCount)})
    for (std::size_t i = 0; i < entry.first->pointCount(); ++i, ++slot)
      if (candidate.sourceIndices[slot] != entry.second + entry.first->sourceIndices[i] ||
          candidate.validity[slot] != entry.first->validity[i] ||
          !slotEqual(entry.first->cloud->xyz, i, cc.xyz, slot, 3) ||
          (cc.hasIntensity() &&
           !slotEqual(entry.first->cloud->intensity, i, cc.intensity, slot, 1)) ||
          (cc.hasNormals() && !slotEqual(entry.first->cloud->normals, i, cc.normals, slot, 3)) ||
          (cc.hasRgb() && !slotEqual(entry.first->cloud->rgb, i, cc.rgb, slot, 3)))
        return fail(why, "合并必须保持a后b、原始通道、有效性和源槽映射");
  const auto data = Data::profile(candidate);
  Json references = Json::array();
  for (const auto* parent : {aProvenance, bProvenance})
    if (parent)
      for (const auto& reference : parent->data.at("referenceSources"))
        if (std::find(references.begin(), references.end(), reference) == references.end())
          references.push_back(reference);
  auto record = provenance_detail::record(data, "derived_composite", recipe, references);
  if (!profileSources(data, &record, sources, why))
    return false;
  merged = std::move(candidate);
  result = std::move(record);
  why.clear();
  return true;
}

inline bool resolveSourceIndex(const Data& data, const Record* provenance,
                               std::uint32_t sourceIndex, nlohmann::json& result,
                               std::string& why) {
  nlohmann::json sources;
  if (!profileSources(data, provenance, sources, why))
    return false;
  const auto& profile = *data.asProfile();
  if (std::find(profile.sourceIndices.begin(), profile.sourceIndices.end(), sourceIndex) ==
      profile.sourceIndices.end())
    return provenance_detail::fail(why, "源索引不属于当前轮廓实际槽");
  auto candidate = nlohmann::json{{"logicalRole", "source"},
                                  {"sourceId", profile.descriptor.sourceId},
                                  {"sourceIndex", sourceIndex}};
  if (provenance)
    for (const auto& segment : provenance->data["recipe"]["segments"]) {
      const auto offset = segment["offset"].get<std::uint64_t>();
      const auto count = segment["count"].get<std::uint64_t>();
      if (sourceIndex >= offset && sourceIndex < offset + count) {
        candidate = {{"logicalRole", segment["logicalRole"]},
                     {"sourceId", segment["sourceContext"]["descriptor"]["sourceId"]},
                     {"sourceIndex", sourceIndex - offset}};
        break;
      }
    }
  result = std::move(candidate);
  why.clear();
  return true;
}
} // namespace lyflow::profile_contract
