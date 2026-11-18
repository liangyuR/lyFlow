#pragma once

#include <initializer_list>
#include <string>

#include "lyflow/manifest.h"

namespace lyflow::op_params {

inline Param intParam(const char* name, const char* label, std::int64_t def, double min, double max,
                      const char* doc) {
  Param p;
  p.name = name;
  p.type = ParamType::Int;
  p.label = label;
  p.doc = doc;
  p.def = Value::integer(def);
  p.min = min;
  p.max = max;
  return p;
}

inline Param floatParam(const char* name, const char* label, double def, const char* doc) {
  Param p;
  p.name = name;
  p.type = ParamType::Float;
  p.label = label;
  p.doc = doc;
  p.def = Value::number(def);
  return p;
}

inline Param boolParam(const char* name, const char* label, bool def, const char* doc) {
  Param p;
  p.name = name;
  p.type = ParamType::Bool;
  p.label = label;
  p.doc = doc;
  p.def = Value::boolean(def);
  return p;
}

inline Param enumParam(const char* name, const char* label, const char* def,
                       std::initializer_list<EnumOption> options, const char* doc) {
  Param p;
  p.name = name;
  p.type = ParamType::Enum;
  p.label = label;
  p.doc = doc;
  p.def = Value::text(def);
  p.options = options;
  return p;
}

inline Status badInput(const std::string& message, const char* port = "image") {
  return Status::Error(Phase::Execute, "bad_input", message, {}, port);
}

inline Status badParam(const std::string& message, const char* param) {
  return Status::Error(Phase::Execute, "bad_param", message, param);
}

inline Param text(const char* name, const char* label, const char* value) {
  Param p;
  p.name = name;
  p.label = label;
  p.type = ParamType::String;
  p.def = Value::text(value);
  return p;
}

inline Port recordPort(const char* name, const char* type, bool required = true) {
  return withContract(Port{name, "Record", name, type, required}, {{"recordType", type}});
}

} // namespace lyflow::op_params
