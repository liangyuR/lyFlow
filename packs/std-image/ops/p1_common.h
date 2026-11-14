#pragma once

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>

#include <opencv2/imgproc.hpp>

#include "lyflow/calibration.h"
#include "ops.h"
#include "params.h"

namespace lyflow::ops::image_p1 {

inline OperatorDesc descriptor(const char* id, const char* label, const char* english) {
  OperatorDesc op;
  op.id = id;
  op.version = "1.0.0";
  op.label = label;
  op.category = "图像/视觉";
  op.keywords = {english, label};
  op.capabilities = {true, false, true};
  return op;
}

inline Param text(const char* name, const char* label, const char* value) {
  Param p;
  p.name = name;
  p.label = label;
  p.type = ParamType::String;
  p.def = Value::text(value);
  return p;
}

inline Param vector(const char* name, const char* label, std::vector<double> value,
                    const char* unit = "") {
  Param p;
  p.name = name;
  p.label = label;
  p.type = value.size() == 2   ? ParamType::Vec2f
           : value.size() == 3 ? ParamType::Vec3f
                               : ParamType::Vec4f;
  p.def = Value::vec(std::move(value));
  p.unit = unit;
  return p;
}

inline Port recordPort(const char* name, const char* type, bool required = true) {
  return withContract(Port{name, "Record", name, type, required}, {{"recordType", type}});
}

inline Status image(const Image* image, const char* port = "image", bool gray = false,
                    std::size_t pixels = 16777216) {
  if (!image || !image->consistent() || image->scale < 1 || image->pixelCount() > pixels ||
      image->byteSize() > 268435456 || (gray && image->channels != 1))
    return img::badInput("图像无效、超过像素/内存预算或不是要求的单通道图", port);
  if (image->depth == PixelDepth::F32 && !cv::checkRange(cvx::view(*image)))
    return img::badInput("浮点图像不得包含 NaN/Inf", port);
  return Status::Ok();
}

inline cv::Mat gray(const Image& image) {
  cv::Mat result;
  if (image.channels == 1)
    result = cvx::view(image);
  else
    cv::cvtColor(cvx::view(image), result,
                 image.channels == 4 ? cv::COLOR_RGBA2GRAY : cv::COLOR_RGB2GRAY);
  return result;
}

inline Status record(Outputs& out, const char* port, const char* type, nlohmann::json data) {
  std::string why;
  if (!values::boundedJson(data, why, true))
    return Status::Error(Phase::Execute, "resource_limit", why);
  out.set(port, Data::record(Record{type, std::move(data)}));
  return Status::Ok();
}

inline Status putImage(Outputs& out, const char* port, cv::Mat matrix, int scale = 1) {
  Image image;
  if (!cvx::fromMat(std::move(matrix), image))
    return img::badInput("输出图像格式不受支持", port);
  if (image.depth == PixelDepth::F32 && !cv::checkRange(cvx::view(image)))
    return img::badInput("输出浮点图像含非有限值", port);
  image.scale = scale;
  out.set(port, Data::image(std::move(image)));
  return Status::Ok();
}

inline Status camera(const Inputs& in, const Image& image, calibration::Camera& camera) {
  const Record* record = in.get("calib").asRecord();
  std::string why;
  if (!record || !calibration::parseCamera(*record, camera, why))
    return img::badInput(why.empty() ? "缺少相机标定" : why, "calib");
  if (image.scale != 1 || camera.width != image.width || camera.height != image.height)
    return img::badInput("相机标定分辨率必须与全分辨率输入图像一致", "calib");
  return Status::Ok();
}

inline cv::Mat cameraMatrix(const calibration::Camera& camera) {
  return cv::Mat(3, 3, CV_64F, const_cast<double*>(camera.matrix.data())).clone();
}

inline cv::Mat distortion(const calibration::Camera& camera) {
  return cv::Mat(camera.distortion).clone();
}

} // namespace lyflow::ops::image_p1
