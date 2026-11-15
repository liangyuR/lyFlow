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
#include "lyflow/kernel/rigid.h"
#include "lyflow/registry.h"

namespace lyflow::packs::std_geometry::pose_detail {

using namespace lyflow::kernel;
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
