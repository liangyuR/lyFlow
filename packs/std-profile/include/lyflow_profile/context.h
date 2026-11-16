#pragma once

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

#include "lyflow/hash.h"
#include "lyflow/profile.h"

namespace lyflow::profile_contract {
inline bool fingerprint(const nlohmann::json& value) {
  if (!value.is_string())
    return false;
  const auto& text = value.get_ref<const std::string&>();
  return text.size() == 32 && std::all_of(text.begin(), text.end(), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

inline bool parse(const nlohmann::json& value, ProfileDescriptor& descriptor, std::string& why) {
  if (!value.is_object() || value.size() != 3 || !value.contains("schemaVersion") ||
      !value["schemaVersion"].is_number_integer() || value["schemaVersion"] != 1 ||
      !value.contains("descriptor") || !value.contains("profileHash") ||
      !fingerprint(value["profileHash"])) {
    why = "轮廓上下文必须含版本1描述与32位内容指纹";
    return false;
  }
  if (!parseProfileDescriptor(value["descriptor"], descriptor, why))
    return false;
  why.clear();
  return true;
}

inline nlohmann::json context(const Data& data) {
  const auto* profile = data.asProfile();
  std::string why;
  if (!profile || !validateProfile(*profile, why))
    throw std::invalid_argument(why.empty() ? "需要有效Profile" : why);
  const auto descriptor = profileDescriptorJson(profile->descriptor);
  HashBuilder hash;
  hash.add("profile-context/v1");
  hash.add(descriptor.dump());
  const auto& cloud = *profile->cloud;
  hash.addBytes(cloud.xyz.data(), cloud.xyz.size() * sizeof(float));
  hash.addBytes(cloud.intensity.data(), cloud.intensity.size() * sizeof(float));
  hash.addBytes(cloud.normals.data(), cloud.normals.size() * sizeof(float));
  hash.addBytes(cloud.rgb.data(), cloud.rgb.size());
  hash.addBytes(profile->validity.data(), profile->validity.size());
  hash.addBytes(profile->sourceIndices.data(),
                profile->sourceIndices.size() * sizeof(std::uint32_t));
  return {{"schemaVersion", 1}, {"descriptor", descriptor}, {"profileHash", hash.hex()}};
}

inline bool matches(const Data& data, const nlohmann::json& value, std::string& why) {
  ProfileDescriptor descriptor;
  if (!parse(value, descriptor, why))
    return false;
  const auto* profile = data.asProfile();
  if (!profile || !validateProfile(*profile, why)) {
    if (why.empty())
      why = "需要有效Profile";
    return false;
  }
  if (value != context(data)) {
    why = "轮廓声明或内容与配套上下文不匹配";
    return false;
  }
  why.clear();
  return true;
}

inline bool validLine(const Data& data, std::string& why) {
  const auto* line = data.asLine2D();
  if (!line || (line->unit != Unit2D::Meter && line->unit != Unit2D::Pixel)) {
    why = "需要单位明确的Line2D";
    return false;
  }
  for (int i = 0; i < 2; ++i)
    if (!std::isfinite(line->point[i]) || !std::isfinite(line->dir[i]) ||
        (line->hasSegment && (!std::isfinite(line->start[i]) || !std::isfinite(line->end[i])))) {
      why = "直线坐标与方向必须有限";
      return false;
    }
  if (std::hypot(static_cast<double>(line->dir[0]), static_cast<double>(line->dir[1])) == 0 ||
      (line->hasSegment && line->start[0] == line->end[0] && line->start[1] == line->end[1])) {
    why = "直线方向或线段退化";
    return false;
  }
  why.clear();
  return true;
}

inline std::string geometryHash(const Data& data) {
  std::string why;
  if (const auto* point = data.asPoint2D()) {
    if ((point->unit != Unit2D::Meter && point->unit != Unit2D::Pixel) ||
        !std::isfinite(point->p[0]) || !std::isfinite(point->p[1]))
      throw std::invalid_argument("需要坐标有限且单位明确的Point2D");
  } else if (!validLine(data, why)) {
    throw std::invalid_argument(why);
  }
  HashBuilder hash;
  hash.add(data.typeName());
  hash.add(data.valueJson());
  return hash.hex();
}

inline nlohmann::json geometryContext(const Data& profile, const Data& geometry) {
  const auto* point = geometry.asPoint2D();
  const auto* line = geometry.asLine2D();
  if ((!point || point->unit != Unit2D::Meter) && (!line || line->unit != Unit2D::Meter))
    throw std::invalid_argument("几何上下文只接受米制Point2D或Line2D");
  return {{"schemaVersion", 1},
          {"context", context(profile)},
          {"geometryUnit", "m"},
          {"geometryHash", geometryHash(geometry)}};
}

inline bool geometryMatches(const Data& geometry, const Record* record,
                            ProfileDescriptor& descriptor, std::string& why) {
  if (!record || record->type != "profile.Geometry.v1" || !record->data.is_object()) {
    why = "需要profile.Geometry.v1几何上下文";
    return false;
  }
  const auto& value = record->data;
  if (value.size() != 4 || !value.contains("schemaVersion") ||
      !value["schemaVersion"].is_number_integer() || value["schemaVersion"] != 1 ||
      !value.contains("geometryUnit") || value["geometryUnit"] != "m" ||
      !value.contains("geometryHash") || !fingerprint(value["geometryHash"]) ||
      !value.contains("context")) {
    why = "几何上下文版本、米制单位或指纹非法";
    return false;
  }
  const auto* point = geometry.asPoint2D();
  const auto* line = geometry.asLine2D();
  if ((!point || point->unit != Unit2D::Meter) && (!line || line->unit != Unit2D::Meter)) {
    why = "几何上下文只接受米制Point2D或Line2D";
    return false;
  }
  if (!parse(value["context"], descriptor, why))
    return false;
  try {
    if (value["geometryHash"] != geometryHash(geometry)) {
      why = "几何内容与配套上下文指纹不匹配";
      return false;
    }
  } catch (const std::invalid_argument& error) {
    why = error.what();
    return false;
  }
  why.clear();
  return true;
}
inline bool sameSpace(const ProfileDescriptor& a, const ProfileDescriptor& b) {
  return a.frame == b.frame && a.plane == b.plane && a.handedness == b.handedness;
}

inline bool validGeometryV2(const Data& geometry, std::string& why) {
  const auto metric = [](Unit2D unit) { return unit == Unit2D::Meter; };
  bool valid = false;
  if (const auto* point = geometry.asPoint2D()) {
    valid = metric(point->unit) && std::isfinite(point->p[0]) && std::isfinite(point->p[1]);
  } else if (const auto* line = geometry.asLine2D()) {
    valid = metric(line->unit) && std::isfinite(line->point[0]) && std::isfinite(line->point[1]) &&
            std::isfinite(line->dir[0]) && std::isfinite(line->dir[1]) &&
            std::hypot(static_cast<double>(line->dir[0]), line->dir[1]) > 0;
    if (line->hasSegment)
      valid = valid && std::isfinite(line->start[0]) && std::isfinite(line->start[1]) &&
              std::isfinite(line->end[0]) && std::isfinite(line->end[1]);
  } else if (const auto* circle = geometry.asCircle2D()) {
    valid = metric(circle->unit) && std::isfinite(circle->center[0]) &&
            std::isfinite(circle->center[1]) && std::isfinite(circle->radius) && circle->radius > 0;
  } else if (const auto* box = geometry.asBox2D()) {
    valid = metric(box->unit) && std::isfinite(box->min[0]) && std::isfinite(box->min[1]) &&
            std::isfinite(box->max[0]) && std::isfinite(box->max[1]) &&
            box->min[0] <= box->max[0] && box->min[1] <= box->max[1];
  }
  why = valid ? "" : "需要有效米制Point2D、Line2D、Circle2D或Box2D";
  return valid;
}

inline std::string geometryHashV2(const Data& geometry) {
  std::string why;
  if (!validGeometryV2(geometry, why))
    throw std::invalid_argument(why);
  HashBuilder hash;
  hash.add(geometry.typeName());
  hash.add(geometry.valueJson());
  return hash.hex();
}

inline bool geometrySources(const nlohmann::json& sources, ProfileDescriptor& descriptor,
                            std::string& why) {
  if (!sources.is_array() || sources.empty() || sources.size() > 64) {
    why = "几何来源需要1到64项完整轮廓上下文";
    return false;
  }
  ProfileDescriptor first;
  for (std::size_t i = 0; i < sources.size(); ++i) {
    ProfileDescriptor current;
    if (!parse(sources[i], current, why))
      return false;
    if (i == 0)
      first = current;
    else if (!sameSpace(first, current)) {
      why = "几何的所有来源必须处于相同坐标帧、轴平面和手性";
      return false;
    }
    for (std::size_t previous = 0; previous < i; ++previous)
      if (sources[previous] == sources[i]) {
        why = "几何来源上下文重复";
        return false;
      }
  }
  descriptor = std::move(first);
  why.clear();
  return true;
}

inline nlohmann::json geometryContextV2FromSources(const Data& geometry,
                                                   const nlohmann::json& sources,
                                                   const std::string& relation = "derived") {
  if (!sources.is_array() || sources.empty() || sources.size() > 256 ||
      (relation != "derived" && relation != "declared"))
    throw std::invalid_argument("几何来源或生成关系非法");
  nlohmann::json unique = nlohmann::json::array();
  for (const auto& source : sources) {
    ProfileDescriptor descriptor;
    std::string why;
    if (!parse(source, descriptor, why))
      throw std::invalid_argument(why);
    if (std::find(unique.begin(), unique.end(), source) == unique.end())
      unique.push_back(source);
  }
  ProfileDescriptor descriptor;
  std::string why;
  if (!geometrySources(unique, descriptor, why))
    throw std::invalid_argument(why);
  return {{"schemaVersion", 2},           {"geometryType", geometry.typeName()},
          {"geometryUnit", "m"},          {"geometryHash", geometryHashV2(geometry)},
          {"sources", std::move(unique)}, {"relation", relation}};
}

inline nlohmann::json geometryContextV2(const Data& profile, const Data& geometry,
                                        const std::string& relation = "derived") {
  return geometryContextV2FromSources(geometry, nlohmann::json::array({context(profile)}),
                                      relation);
}

inline bool geometryMatchesPhysical(const Data& geometry, const Record* record,
                                    ProfileDescriptor& descriptor, std::string& why,
                                    nlohmann::json* sources = nullptr) {
  ProfileDescriptor parsed;
  if (record && record->type == "profile.Geometry.v1") {
    if (!geometryMatches(geometry, record, parsed, why))
      return false;
    if (sources)
      *sources = nlohmann::json::array({record->data["context"]});
    descriptor = std::move(parsed);
    return true;
  }
  if (!record || record->type != "profile.Geometry.v2" || !record->data.is_object()) {
    why = "需要profile.Geometry.v1或v2几何上下文";
    return false;
  }
  const auto& value = record->data;
  if (value.size() != 6 || !value.contains("schemaVersion") ||
      !value["schemaVersion"].is_number_integer() || value["schemaVersion"] != 2 ||
      !value.contains("geometryType") || value["geometryType"] != geometry.typeName() ||
      !value.contains("geometryUnit") || value["geometryUnit"] != "m" ||
      !value.contains("geometryHash") || !fingerprint(value["geometryHash"]) ||
      !value.contains("sources") || !value.contains("relation") ||
      (value["relation"] != "derived" && value["relation"] != "declared")) {
    why = "几何上下文版本、类型、单位、来源或生成关系非法";
    return false;
  }
  if (!geometrySources(value["sources"], parsed, why))
    return false;
  try {
    if (value["geometryHash"] != geometryHashV2(geometry)) {
      why = "几何内容与配套上下文指纹不匹配";
      return false;
    }
  } catch (const std::invalid_argument& error) {
    why = error.what();
    return false;
  }
  if (sources)
    *sources = value["sources"];
  descriptor = std::move(parsed);
  why.clear();
  return true;
}
} // namespace lyflow::profile_contract
