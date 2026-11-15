#pragma once

#include <algorithm>
#include <cmath>
#include <stdexcept>

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
} // namespace lyflow::profile_contract
