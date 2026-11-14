#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "lyflow/operator.h"
#include "lyflow/pose.h"
#include "lyflow/registry.h"

namespace lyflow::packs::std_geometry::pose_detail {

using V3 = std::array<double, 3>;
using M4 = std::array<double, 16>;
using Json = nlohmann::json;

inline Status bad(const std::string& text, const char* port = "poses") {
  return Status::Error(Phase::Execute, "bad_input", text, {}, port);
}
inline Status badParam(const std::string& text, const char* name) {
  return Status::Error(Phase::Execute, "bad_param", text, name);
}
inline Param text(const char* name, const char* label, const char* def = "") {
  Param p;
  p.name = name;
  p.label = label;
  p.type = ParamType::String;
  p.def = Value::text(def);
  return p;
}
inline Param number(const char* name, const char* label, double def = 0, const char* unit = "") {
  Param p;
  p.name = name;
  p.label = label;
  p.type = ParamType::Float;
  p.def = Value::number(def);
  p.unit = unit;
  return p;
}
inline Param integer(const char* name, const char* label, std::int64_t def = 0) {
  Param p = number(name, label);
  p.type = ParamType::Int;
  p.def = Value::integer(def);
  return p;
}
inline Param flag(const char* name, const char* label, bool def = false) {
  Param p = text(name, label);
  p.type = ParamType::Bool;
  p.def = Value::boolean(def);
  return p;
}
inline Param vec(const char* name, const char* label, std::vector<double> values) {
  Param p = text(name, label);
  p.type = values.size() == 2   ? ParamType::Vec2f
           : values.size() == 3 ? ParamType::Vec3f
                                : ParamType::Vec4f;
  p.def = Value::vec(std::move(values));
  return p;
}
inline Param choice(const char* name, const char* label, const char* def,
                    std::initializer_list<const char*> values) {
  Param p = text(name, label, def);
  p.type = ParamType::Enum;
  for (const char* value : values)
    p.options.push_back({value, value, ""});
  return p;
}
inline OperatorDesc descriptor(const char* id, const char* label, const char* keywords) {
  OperatorDesc op;
  op.id = id;
  op.version = "1.0.0";
  op.label = label;
  op.category = "几何/工件位姿";
  op.keywords = {keywords, label, "pose", "工件", "位姿", "坐标系"};
  op.capabilities = {true, false, true};
  return op;
}
inline Port record(const char* name, bool required = true) {
  return {name, "Record", name, "版本化 Record，具体载荷与单位见算子说明。", required};
}
inline Port scalarPort(const char* name, bool required = false) {
  return withContract(
      Port{name, "Record", name, "std.Value.v1；数值单位必须匹配算子声明。", required},
      {{"recordType", values::kValueType}});
}
inline bool finite(V3 v) {
  return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}
inline V3 sub(V3 a, V3 b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
inline V3 cross(V3 a, V3 b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
inline double dot(V3 a, V3 b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline bool normalize(V3& v) {
  const double length = std::hypot(v[0], v[1], v[2]);
  if (!std::isfinite(length) || length == 0)
    return false;
  for (double& c : v)
    c /= length;
  return true;
}
inline double sum3(double a, double b, double c) {
  double sum = 0, error = 0;
  for (double value : {a, b, c}) {
    const double next = sum + value;
    error += std::abs(sum) >= std::abs(value) ? (sum - next) + value : (value - next) + sum;
    sum = next;
  }
  return sum + error;
}
inline double productSum(std::initializer_list<std::pair<double, double>> terms) {
  double sum = 0, error = 0;
  const auto add = [&](double value) {
    const double next = sum + value;
    error += std::abs(sum) >= std::abs(value) ? (sum - next) + value : (value - next) + sum;
    sum = next;
  };
  for (const auto& [a, b] : terms) {
    const double product = a * b;
    add(product);
    add(std::fma(a, b, -product));
  }
  return sum + error;
}
inline V3 apply(const M4& m, V3 p, bool direction = false) {
  V3 result{};
  for (int i = 0; i < 3; ++i)
    result[i] = productSum({{m[i * 4], p[0]},
                            {m[i * 4 + 1], p[1]},
                            {m[i * 4 + 2], p[2]},
                            {direction ? 0 : m[i * 4 + 3], 1}});
  return result;
}
inline bool affine(const Transform* transform, M4& m, std::string& why) {
  if (!transform) {
    why = "需要原生 Transform";
    return false;
  }
  for (int i = 0; i < 16; ++i) {
    m[i] = transform->m[i];
    if (!std::isfinite(m[i])) {
      why = "变换矩阵必须有限";
      return false;
    }
  }
  if (m[12] != 0 || m[13] != 0 || m[14] != 0 || m[15] != 1) {
    why = "变换末行必须是 [0,0,0,1]";
    return false;
  }
  return true;
}
inline bool rigid(const Transform* transform, M4& m, poses::Quaternion& q, std::string& why) {
  if (!affine(transform, m, why))
    return false;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) {
      double product = 0;
      for (int k = 0; k < 3; ++k)
        product += m[k * 4 + i] * m[k * 4 + j];
      if (std::abs(product - (i == j ? 1 : 0)) > 1e-5) {
        why = "位姿仅接受刚体变换，旋转不得含尺度或剪切";
        return false;
      }
    }
  const V3 x{m[0], m[4], m[8]}, y{m[1], m[5], m[9]}, z{m[2], m[6], m[10]};
  if (dot(cross(x, y), z) < 0) {
    why = "旋转必须为右手正交矩阵，不接受镜像";
    return false;
  }
  q = poses::quaternion({m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]});
  return true;
}
inline bool similarity2D(const Transform* transform, M4& m, double& scale, bool proper,
                         std::string& why) {
  if (!affine(transform, m, why))
    return false;
  const double a = std::hypot(m[0], m[4]), b = std::hypot(m[1], m[5]);
  if (a == 0 || b == 0 || std::abs(a - b) > 1e-6 * std::max(a, b) ||
      std::abs((m[0] / a) * (m[1] / b) + (m[4] / a) * (m[5] / b)) > 1e-6 || m[2] != 0 ||
      m[6] != 0 || m[8] != 0 || m[9] != 0 || m[10] != 1 || m[11] != 0 ||
      (proper && m[0] * m[5] - m[1] * m[4] < 0)) {
    why = "二维变换必须为非退化相似变换，保留 z 平面；工件位姿不接受镜像";
    return false;
  }
  scale = a;
  return true;
}
inline bool nativeFloat(double v) {
  return std::isfinite(v) && std::abs(v) <= std::numeric_limits<float>::max();
}
inline Status read(const Data& data, Record& out) {
  const auto* record = data.asRecord();
  std::string why;
  if (!record || !poses::validate(*record, why))
    return bad(why.empty() ? "需要位姿 Record" : why);
  const bool two = record->type == poses::kPose2DType || record->type == poses::kPoses2DType;
  if (!poses::readList(data, two ? 2 : 3, out, why))
    return bad(why);
  return Status::Ok();
}
inline Status put(Outputs& out, const char* port, Record record) {
  std::string why;
  if (!poses::validate(record, why))
    return bad("结果不满足位姿契约：" + why);
  out.set(port, Data::record(std::move(record)));
  return Status::Ok();
}
inline bool two(const Record& r) {
  return r.type == poses::kPoses2DType || r.type == poses::kPose2DType;
}
inline Status scalar(const Inputs& in, const char* port, double fallback, const std::string& unit,
                     double& out) {
  out = fallback;
  if (in.has(port)) {
    std::string actual, why;
    if (!values::readNumber(in.get(port), out, actual, why) || actual != unit)
      return bad(why.empty() ? "动态输入单位不匹配：" + unit : why, port);
  }
  return std::isfinite(out) ? Status::Ok() : bad("数值必须有限", port);
}
inline bool parseText(const std::string& text, Json& result, std::string& why) {
  if (text.size() > values::kMaxStringBytes) {
    why = "JSON 文本超过 64 KiB";
    return false;
  }
  bool quoted = false, escaped = false;
  int depth = 0;
  for (char c : text) {
    if (quoted) {
      if (escaped)
        escaped = false;
      else if (c == '\\')
        escaped = true;
      else if (c == '"')
        quoted = false;
    } else if (c == '"')
      quoted = true;
    else if ((c == '{' || c == '[') && ++depth > 16) {
      why = "JSON 嵌套超过 16 层";
      return false;
    } else if (c == '}' || c == ']')
      --depth;
  }
  result = Json::parse(text, nullptr, false);
  if (result.is_discarded() || !values::boundedJson(result, why, true)) {
    why = "非法或超出有界契约的 JSON";
    return false;
  }
  return true;
}

} // namespace lyflow::packs::std_geometry::pose_detail
