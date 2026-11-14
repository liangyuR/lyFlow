#pragma once

#include <Eigen/Dense>
#include <Eigen/Geometry>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <string>

#include "lyflow/cloud_groups.h"
#include "lyflow/operator.h"
#include "lyflow/registry.h"

namespace lyflow::ops::p1 {

using Vec3 = Eigen::Vector3d;
using Mat3 = Eigen::Matrix3d;
using Mat4 = Eigen::Matrix4d;

inline Status error(const std::string& message, const std::string& port = "cloud",
                    const std::string& code = "bad_input") {
  const bool parameter = code == "bad_param" || code == "index_out_of_range";
  return Status::Error(Phase::Execute, code, message, parameter ? port : std::string{},
                       parameter ? std::string{} : port);
}

inline OperatorDesc descriptor(std::string id, std::string label, std::string english) {
  OperatorDesc op;
  op.id = std::move(id);
  op.label = std::move(label);
  op.version = "1.0.0";
  op.category = "点云/三维分析";
  op.keywords = {english, op.label};
  op.capabilities = {true, false, true};
  return op;
}

inline Param number(std::string name, std::string label, double def, double min = 0,
                    double max = 1e6, std::string unit = {}) {
  Param p;
  p.name = std::move(name);
  p.label = std::move(label);
  p.type = ParamType::Float;
  p.def = Value::number(def);
  p.min = min;
  p.max = max;
  p.unit = std::move(unit);
  return p;
}

inline Param integer(std::string name, std::string label, std::int64_t def, std::int64_t min,
                     std::int64_t max) {
  Param p;
  p.name = std::move(name);
  p.label = std::move(label);
  p.type = ParamType::Int;
  p.def = Value::integer(def);
  p.min = static_cast<double>(min);
  p.max = static_cast<double>(max);
  return p;
}

inline Param text(std::string name, std::string label, std::string def) {
  Param p;
  p.name = std::move(name);
  p.label = std::move(label);
  p.type = ParamType::String;
  p.def = Value::text(std::move(def));
  return p;
}

inline Param choice(std::string name, std::string label, std::string def,
                    std::initializer_list<std::pair<const char*, const char*>> choices) {
  Param p = text(std::move(name), std::move(label), std::move(def));
  p.type = ParamType::Enum;
  for (const auto& item : choices)
    p.options.push_back({item.first, item.second});
  return p;
}

inline Param vector(std::string name, std::string label, std::vector<double> def) {
  Param p;
  p.name = std::move(name);
  p.label = std::move(label);
  p.type = ParamType::Vec3f;
  p.def = Value::vec(std::move(def));
  return p;
}

inline Vec3 point(const PointCloud& cloud, std::size_t i) {
  return Vec3(cloud.xyz[3 * i], cloud.xyz[3 * i + 1], cloud.xyz[3 * i + 2]);
}

inline Status validate(const PointCloud& cloud, std::size_t minimum = 0,
                       const std::string& port = "cloud") {
  if (!cloud.channelsConsistent() || cloud.pointCount() < minimum ||
      cloud.pointCount() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
    return error("点云通道不一致、点数不足或超过原生索引范围", port);
  for (float value : cloud.xyz)
    if (!std::isfinite(value))
      return error("点云坐标必须全部有限；请先显式移除无效点", port);
  for (float value : cloud.intensity)
    if (!std::isfinite(value))
      return error("点云强度必须有限", port);
  for (float value : cloud.normals)
    if (!std::isfinite(value))
      return error("点云法线分量必须有限", port);
  return Status::Ok();
}

inline bool frameValid(const std::string& frame) { return !frame.empty() && frame.size() <= 1024; }

inline std::vector<double> array(const Vec3& vector) {
  return {vector.x(), vector.y(), vector.z()};
}

inline nlohmann::json matrixJson(const Mat4& matrix) {
  nlohmann::json data = nlohmann::json::array();
  for (int row = 0; row < 4; ++row)
    for (int column = 0; column < 4; ++column)
      data.push_back(matrix(row, column));
  return data;
}

inline bool rigid(const Transform& transform, Mat4& result) {
  for (int row = 0; row < 4; ++row)
    for (int column = 0; column < 4; ++column)
      result(row, column) = transform.m[row * 4 + column];
  const Mat3 rotation = result.topLeftCorner<3, 3>();
  return result.allFinite() && (result.row(3) - Eigen::RowVector4d(0, 0, 0, 1)).norm() <= 1e-6 &&
         (rotation.transpose() * rotation - Mat3::Identity()).norm() <= 1e-5 &&
         std::abs(rotation.determinant() - 1) <= 1e-5;
}

inline bool transform(const Mat4& matrix, Transform& result) {
  if (!matrix.allFinite() || matrix.cwiseAbs().maxCoeff() > std::numeric_limits<float>::max())
    return false;
  for (int row = 0; row < 4; ++row)
    for (int column = 0; column < 4; ++column)
      result.m[row * 4 + column] = static_cast<float>(matrix(row, column));
  return true;
}

inline bool transformed(const PointCloud& input, const Mat4& matrix, PointCloud& output,
                        ExecContext& ctx) {
  output.xyz.resize(input.xyz.size());
  output.intensity = input.intensity;
  output.rgb = input.rgb;
  output.normals.resize(input.normals.size());
  Ticker ticker(ctx, input.pointCount());
  for (std::size_t i = 0; i < input.pointCount(); ++i) {
    if (ticker.tick(i))
      return false;
    const Vec3 p = matrix.topLeftCorner<3, 3>() * point(input, i) + matrix.topRightCorner<3, 1>();
    if (!p.allFinite() || p.cwiseAbs().maxCoeff() > std::numeric_limits<float>::max())
      return false;
    for (int j = 0; j < 3; ++j)
      output.xyz[i * 3 + j] = static_cast<float>(p[j]);
    if (input.hasNormals()) {
      const Vec3 normal(input.normals[i * 3], input.normals[i * 3 + 1], input.normals[i * 3 + 2]);
      Vec3 rotated = matrix.topLeftCorner<3, 3>() * normal;
      if (!rotated.allFinite() || !(rotated.norm() > 0))
        return false;
      rotated.normalize();
      for (int j = 0; j < 3; ++j)
        output.normals[i * 3 + j] = static_cast<float>(rotated[j]);
    }
  }
  return true;
}

inline Measurement measurement(double value, std::string unit = "m") {
  Measurement m;
  m.ok = true;
  m.value = value;
  m.unit = std::move(unit);
  return m;
}

} // namespace lyflow::ops::p1
