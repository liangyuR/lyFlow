#include "lyflow/manifest.h"

namespace lyflow {

Value Value::boolean(bool v) {
  Value x; x.kind_ = Kind::Bool; x.bool_ = v; return x;
}
Value Value::integer(std::int64_t v) {
  Value x; x.kind_ = Kind::Int; x.int_ = v; return x;
}
Value Value::number(double v) {
  Value x; x.kind_ = Kind::Float; x.float_ = v; return x;
}
Value Value::text(std::string v) {
  Value x; x.kind_ = Kind::String; x.string_ = std::move(v); return x;
}
Value Value::vec(std::vector<double> v) {
  Value x; x.kind_ = Kind::FloatVec; x.vec_ = std::move(v); return x;
}

const Param* findParam(const OperatorDesc& op, const std::string& name) {
  for (const auto& p : op.params) {
    if (p.name == name) return &p;
  }
  return nullptr;
}

const char* toString(ParamType t) {
  switch (t) {
    case ParamType::Bool:      return "bool";
    case ParamType::Int:       return "int";
    case ParamType::Float:     return "float";
    case ParamType::Vec2f:     return "vec2f";
    case ParamType::Vec3f:     return "vec3f";
    case ParamType::Vec4f:     return "vec4f";
    case ParamType::Enum:      return "enum";
    case ParamType::Flags:     return "flags";
    case ParamType::String:    return "string";
    case ParamType::Text:      return "text";
    case ParamType::Path:      return "path";
    case ParamType::Color:     return "color";
    case ParamType::Transform: return "transform";
    case ParamType::Curve:     return "curve";
  }
  return "float";
}

}  // namespace lyflow
