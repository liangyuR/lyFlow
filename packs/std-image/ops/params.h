#pragma once
// 本包算子共用的参数、端口与出错写法。只为少抄几遍样板，不引入新概念。
#include <initializer_list>
#include <string>

#include <opencv2/core.hpp>

#include "lyflow/data.h"
#include "lyflow/manifest.h"
#include "lyflow/operator.h"
#include "lyflow_cv/adapter.h"

namespace lyflow::ops::img {

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

/// 位深下拉：u8 / u16 / f32。
inline Param depthParam(const char* name, const char* label, const char* def, const char* doc) {
  return enumParam(name, label, def,
                   {EnumOption{"u8", "u8", "0..255"}, EnumOption{"u16", "u16", "0..65535"},
                    EnumOption{"f32", "f32", "浮点，约定 0..1"}},
                   doc);
}

inline Condition when(const char* param, Value eq) {
  Condition c;
  c.param = param;
  c.eq = std::move(eq);
  return c;
}

inline Port imageIn(const char* name = "image", const char* doc = "输入图像。") {
  return Port{name, "Image", "Image", doc, true};
}

inline Port imageOut(const char* name = "image", const char* doc = "输出图像。") {
  return Port{name, "Image", "Image", doc, true};
}

/// 只收单通道图：ADR-0026 的图像 shape 契约按 [高, 宽, 通道] 读。
inline Port grayIn(const char* name = "image",
                   const char* doc = "单通道图像；彩色图先接 image.to_gray。") {
  return withContract(Port{name, "Image", "Image", doc, true},
                      nlohmann::json{{"shape", {-1, -1, 1}}});
}

inline Status badInput(const std::string& message, const char* port = "image") {
  return Status::Error(Phase::Execute, "bad_input", message, {}, port);
}

inline Status badParam(const std::string& message, const char* param) {
  return Status::Error(Phase::Execute, "bad_param", message, param);
}

/// OpenCV 的结果写到输出端口。位深 / 通道不被 core 接受时报 internal —— 那是算子自己的 bug。
inline Status putMat(Outputs& outputs, const char* port, cv::Mat m) {
  Image out;
  if (!cvx::fromMat(cvx::toSupportedDepth(m), out)) {
    return Status::Error(Phase::Execute, "internal",
                         std::string("算子产出的图像 core 接不住（") + std::to_string(m.channels()) +
                             " 通道）",
                         {}, port);
  }
  outputs.set(port, Data::image(std::move(out)));
  return Status::Ok();
}

/// OpenCV 抛的异常转成一条人话，指回参数面板。
inline Status fromCvError(const cv::Exception& e, const char* param = "") {
  return Status::Error(Phase::Execute, "bad_param", std::string("OpenCV：") + e.what(), param);
}

}  // namespace lyflow::ops::img
