#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <string>

#include "lyflow/value.h"

namespace lyflow::poses {

inline constexpr const char* kPose2DType = "vision.Pose2D.v1";
inline constexpr const char* kPose3DType = "vision.Pose3D.v1";
inline constexpr const char* kPoses2DType = "vision.Poses2D.v1";
inline constexpr const char* kPoses3DType = "vision.Poses3D.v1";
inline constexpr double kPi = 3.1415926535897932384626433832795;
using Json = nlohmann::json;
using Quaternion = std::array<double, 4>;
using Matrix3 = std::array<double, 9>;

inline double wrapAngle(double angle) {
  const double wrapped = std::remainder(angle, 2 * kPi);
  return wrapped >= kPi ? wrapped - 2 * kPi : wrapped;
}

inline bool numericArray(const Json& value, std::size_t count, bool positive = false) {
  if (!value.is_array() || value.size() != count)
    return false;
  for (const auto& component : value)
    if (!values::safeNumber(component) || (positive && component.get<double>() <= 0))
      return false;
  return true;
}

inline bool normalizeQuaternion(Quaternion& q) {
  for (double component : q)
    if (!std::isfinite(component))
      return false;
  const double scale = std::max({std::abs(q[0]), std::abs(q[1]), std::abs(q[2]), std::abs(q[3])});
  if (!std::isfinite(scale) || scale == 0)
    return false;
  double sum = 0;
  for (double& component : q) {
    component /= scale;
    sum += component * component;
  }
  const double length = std::sqrt(sum);
  for (double& component : q)
    component /= length;
  bool flip = q[3] < 0;
  if (q[3] == 0)
    for (int i = 0; i < 3; ++i)
      if (q[i] != 0) {
        flip = q[i] < 0;
        break;
      }
  if (flip)
    for (double& component : q)
      component = -component;
  return true;
}

inline Matrix3 rotationMatrix(Quaternion q) {
  normalizeQuaternion(q);
  const double x = q[0], y = q[1], z = q[2], w = q[3];
  return {1 - 2 * (y * y + z * z), 2 * (x * y - z * w),     2 * (x * z + y * w),
          2 * (x * y + z * w),     1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
          2 * (x * z - y * w),     2 * (y * z + x * w),     1 - 2 * (x * x + y * y)};
}

inline Quaternion multiply(const Quaternion& a, const Quaternion& b) {
  Quaternion q{a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
               a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
               a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
               a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
  normalizeQuaternion(q);
  return q;
}

inline Quaternion quaternion(const Matrix3& m) {
  Quaternion q{};
  const double trace = m[0] + m[4] + m[8];
  if (trace > 0) {
    const double s = 2 * std::sqrt(1 + trace);
    q = {(m[7] - m[5]) / s, (m[2] - m[6]) / s, (m[3] - m[1]) / s, s / 4};
  } else {
    const int i = m[0] >= m[4] && m[0] >= m[8] ? 0 : (m[4] >= m[8] ? 1 : 2);
    const int j = (i + 1) % 3, k = (i + 2) % 3;
    const double s = 2 * std::sqrt(std::max(0.0, 1 + m[i * 3 + i] - m[j * 3 + j] - m[k * 3 + k]));
    q[i] = s / 4;
    q[j] = (m[j * 3 + i] + m[i * 3 + j]) / s;
    q[k] = (m[k * 3 + i] + m[i * 3 + k]) / s;
    q[3] = (m[k * 3 + j] - m[j * 3 + k]) / s;
  }
  normalizeQuaternion(q);
  return q;
}

inline bool textField(const Json& object, const char* key, bool required = true) {
  if (!object.contains(key))
    return !required;
  if (!object[key].is_string() || (required && object[key].get_ref<const std::string&>().empty()))
    return false;
  try {
    object[key].dump();
    return true;
  } catch (...) {
    return false;
  }
}

inline bool validateItem(const Json& data, int dimension, std::string& why) {
  if (!data.is_object() || !textField(data, "id") || !textField(data, "frame") ||
      !textField(data, "unit") || !data.contains("position") || !data.contains("size") ||
      !numericArray(data["position"], dimension) || !numericArray(data["size"], dimension, true)) {
    why = "位姿需要非空 id/frame、unit、有限 position 和正尺寸 size";
    return false;
  }
  const std::string unit = data["unit"].get<std::string>();
  if ((dimension == 2 && unit != "m" && unit != "px") || (dimension == 3 && unit != "m")) {
    why = "二维位姿仅允许 px/m，三维位姿仅允许 m";
    return false;
  }
  if (dimension == 2) {
    if (!data.contains("angle") || !values::safeNumber(data["angle"])) {
      why = "二维位姿 angle 必须是有限弧度";
      return false;
    }
  } else {
    if (!data.contains("rotation") || !numericArray(data["rotation"], 4)) {
      why = "三维位姿需要 xyzw 四元数";
      return false;
    }
    const auto q = data["rotation"].get<Quaternion>();
    const double norm = std::hypot(std::hypot(q[0], q[1]), std::hypot(q[2], q[3]));
    if (!std::isfinite(norm) || std::abs(norm - 1) > 1e-6) {
      why = "三维位姿四元数必须归一化";
      return false;
    }
  }
  for (const char* field : {"classId", "classLabel", "name"})
    if (!textField(data, field, false)) {
      why = std::string(field) + " 必须为 UTF-8 字符串";
      return false;
    }
  if (data.contains("confidence") &&
      (!values::safeNumber(data["confidence"]) || data["confidence"].get<double>() < 0 ||
       data["confidence"].get<double>() > 1)) {
    why = "confidence 必须位于 [0,1]";
    return false;
  }
  if ((data.contains("pickable") && !data["pickable"].is_boolean()) ||
      (data.contains("attributes") && !data["attributes"].is_object())) {
    why = "pickable/attributes 类型错误";
    return false;
  }
  const std::set<std::string> fields{"id",         "frame",    "unit",      "position",   "size",
                                     "angle",      "rotation", "classId",   "classLabel", "name",
                                     "confidence", "pickable", "attributes"};
  for (auto it = data.begin(); it != data.end(); ++it)
    if (!fields.count(it.key()) || (dimension == 2 && it.key() == "rotation") ||
        (dimension == 3 && it.key() == "angle")) {
      why = "未知或维度不符的位姿字段 " + it.key();
      return false;
    }
  return true;
}

inline bool validate(const Record& record, std::string& why) {
  const bool list = record.type == kPoses2DType || record.type == kPoses3DType;
  const int dimension = record.type == kPose2DType || record.type == kPoses2DType ? 2 : 3;
  if (record.type != kPose2DType && record.type != kPose3DType && !list) {
    why = "未知位姿 Record 类型 " + record.type;
    return false;
  }
  if (!values::boundedJson(record.data, why, true))
    return false;
  if (!list)
    return validateItem(record.data, dimension, why);
  const auto& data = record.data;
  if (!data.is_object() || data.size() != 3 || !textField(data, "frame") ||
      !textField(data, "unit") || !data.contains("items") || !data["items"].is_array() ||
      data["items"].size() > values::kMaxItems) {
    why = "位姿集合需要且仅允许 frame、unit、最多4096项的 items";
    return false;
  }
  if ((dimension == 2 && data["unit"] != "px" && data["unit"] != "m") ||
      (dimension == 3 && data["unit"] != "m")) {
    why = "位姿集合单位非法";
    return false;
  }
  std::set<std::string> ids;
  for (const auto& item : data["items"]) {
    if (!validateItem(item, dimension, why))
      return false;
    if (item["frame"] != data["frame"] || item["unit"] != data["unit"] ||
        !ids.insert(item["id"].get<std::string>()).second) {
      why = "集合元素必须同帧同单位且 id 唯一";
      return false;
    }
  }
  return true;
}

inline Record make2D(std::string id, std::string frame, std::string unit,
                     std::array<double, 2> position, double angle, std::array<double, 2> size) {
  return {kPose2DType,
          {{"id", std::move(id)},
           {"frame", std::move(frame)},
           {"unit", std::move(unit)},
           {"position", position},
           {"angle", wrapAngle(angle)},
           {"size", size}}};
}

inline Record make3D(std::string id, std::string frame, std::array<double, 3> position,
                     Quaternion rotation, std::array<double, 3> size) {
  normalizeQuaternion(rotation);
  return {kPose3DType,
          {{"id", std::move(id)},
           {"frame", std::move(frame)},
           {"unit", "m"},
           {"position", position},
           {"rotation", rotation},
           {"size", size}}};
}

inline Record make2DList(Json items, std::string frame = "image", std::string unit = "px") {
  return {kPoses2DType,
          {{"frame", std::move(frame)}, {"unit", std::move(unit)}, {"items", std::move(items)}}};
}

inline Record make3DList(Json items, std::string frame = "world") {
  return {kPoses3DType, {{"frame", std::move(frame)}, {"unit", "m"}, {"items", std::move(items)}}};
}

inline bool readList(const Data& data, int dimension, Record& result, std::string& why) {
  const Record* record = data.asRecord();
  if (!record || (dimension == 2 && record->type != kPose2DType && record->type != kPoses2DType) ||
      (dimension == 3 && record->type != kPose3DType && record->type != kPoses3DType)) {
    why = "需要相应维度的位姿或位姿集合";
    return false;
  }
  if (!validate(*record, why))
    return false;
  if (record->type == kPose2DType || record->type == kPose3DType) {
    result = dimension == 2 ? make2DList(Json::array({record->data}), record->data["frame"],
                                         record->data["unit"])
                            : make3DList(Json::array({record->data}), record->data["frame"]);
  } else {
    result = *record;
  }
  return true;
}

inline bool read2DList(const Data& data, Record& result, std::string& why) {
  return readList(data, 2, result, why);
}

inline bool read3DList(const Data& data, Record& result, std::string& why) {
  return readList(data, 3, result, why);
}

} // namespace lyflow::poses
