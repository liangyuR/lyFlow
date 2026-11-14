#pragma once

#include <array>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <tuple>

#include "lyflow/pose.h"
#include "lyflow/value.h"
#include "lyflow_cv/compat.h"
#include "p1_common.h"

namespace lyflow::ops::ocr {
using Json = nlohmann::json;
constexpr std::size_t kPixels = 8 * 1024 * 1024;
constexpr std::size_t kTensorValues = 64 * 1024 * 1024;
constexpr std::size_t kRegions = 128;
constexpr const char* kMapping = "image.OCRMapping.v1";
constexpr const char* kRegionsType = "image.TextRegions.v1";
constexpr const char* kLayout = "image.OCRCrops.v1";
constexpr const char* kResults = "image.OCR.v1";

struct Failure {
  Status status;
};
inline void require(bool condition, const std::string& message, const char* port) {
  if (!condition)
    throw Failure{img::badInput(message, port)};
}
inline void require(bool condition, const char* message, const char* port) {
  if (!condition)
    throw Failure{img::badInput(message, port)};
}
inline void limit(bool condition, const char* message) {
  if (!condition)
    throw Failure{Status::Error(Phase::Execute, "resource_limit", message)};
}
inline void check(ExecContext& ctx) {
  if (ctx.cancelled())
    throw Failure{Status::Error(Phase::Execute, "cancelled", "OCR已取消")};
}
inline double number(const ParamView& p, const char* key, double minimum, double maximum) {
  const auto value = p.number(key);
  if (!std::isfinite(value) || value < minimum || value > maximum)
    throw Failure{img::badParam("参数超出声明范围", key)};
  return value;
}
inline int integer(const ParamView& p, const char* key, int minimum, int maximum) {
  const auto value = p.integer(key);
  if (value < minimum || value > maximum)
    throw Failure{img::badParam("整数参数超出范围", key)};
  return static_cast<int>(value);
}
inline std::string text(const ParamView& p, const char* key) {
  const auto value = p.text(key);
  std::string why;
  if (value.empty() || value.size() > 256 || !values::boundedJson(value, why))
    throw Failure{img::badParam("文本须为1..256字节有效UTF-8", key)};
  return value;
}
inline void bounded(const Record& value) {
  std::string why;
  limit(values::boundedJson(value.data, why), "OCR元数据超过Record预算或包含非法值");
}
inline const Image& image(const Inputs& in, ExecContext& ctx) {
  check(ctx);
  const auto* value = in.get("image").asImage();
  require(value && value->consistent() && value->scale == 1,
          "OCR需要完整分辨率图像；Image.scale必须为1", "image");
  limit(value->pixelCount() <= kPixels && value->byteSize() <= 128 * 1024 * 1024,
        "OCR图像超过8Mi像素或128MiB预算");
  if (value->depth == PixelDepth::F32)
    require(cv::checkRange(cvx::view(*value), true, nullptr, 0, std::nextafter(1.f, 2.f)),
            "f32图像所有通道须为有限0..1", "image");
  return *value;
}
inline cv::Mat color(const Image& source, const std::string& order) {
  cv::Mat result;
  auto sourceView = cvx::view(source);
  if (source.channels == 1)
    cv::cvtColor(sourceView, result, cv::COLOR_GRAY2RGB);
  else if (source.channels == 4)
    cv::cvtColor(sourceView, result, cv::COLOR_RGBA2RGB);
  else
    result = sourceView;
  if (order == "bgr") {
    cv::Mat reordered;
    cv::cvtColor(result, reordered, cv::COLOR_RGB2BGR);
    result = std::move(reordered);
  } else
    require(order == "rgb", "channelOrder必须为rgb或bgr", "layout");
  cv::Mat normalized;
  result.convertTo(normalized, CV_32FC3,
                   source.depth == PixelDepth::U8    ? 1. / 255
                   : source.depth == PixelDepth::U16 ? 1. / 65535
                                                     : 1);
  return normalized;
}
inline void finite(const Tensor* value, const char* port, ExecContext& ctx) {
  require(value && value->consistent(), "张量形状和数据不一致", port);
  limit(value->data.size() <= kTensorValues, "OCR张量超过64Mi个float32值");
  for (std::size_t i = 0; i < value->data.size(); ++i) {
    if (!(i % 8192))
      check(ctx);
    require(std::isfinite(value->data[i]), "张量不得含NaN/Inf", port);
  }
}
inline std::string geometryHash(const Tensor& tensor) {
  std::uint64_t hash = 14695981039346656037ull;
  for (float value : tensor.data) {
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    for (int byte = 0; byte < 4; ++byte) {
      hash ^= (bits >> (byte * 8)) & 255;
      hash *= 1099511628211ull;
    }
  }
  std::ostringstream out;
  out << std::hex << std::setw(16) << std::setfill('0') << hash;
  return out.str();
}
inline bool dimension(const Json& value, int& result, int low = 1, int high = 1 << 20) {
  if (!value.is_number_integer() || value < low || value > high)
    return false;
  result = value.get<int>();
  return true;
}
inline const Json& record(const Inputs& in, const char* port, const char* type) {
  const auto* value = in.get(port).asRecord();
  std::string why;
  require(value && value->type == type && value->data.is_object() &&
              values::boundedJson(value->data, why),
          "OCR元数据类型或内容无效", port);
  return value->data;
}
inline bool stringField(const Json& data, const char* key) {
  return data.contains(key) && data[key].is_string() &&
         !data[key].get_ref<const std::string&>().empty() &&
         data[key].get_ref<const std::string&>().size() <= 256;
}
struct Regions {
  std::string frame, hash;
  int width = 0, height = 0;
  Json items;
};
inline Regions regions(const Inputs& in, const Tensor& quads, const Image& source) {
  const auto& data = record(in, "regions", kRegionsType);
  Regions result;
  require(data.size() == 5 && stringField(data, "frame") && data.value("unit", Json()) == "px" &&
              data.contains("imageSize") && data["imageSize"].is_array() &&
              data["imageSize"].size() == 2 && dimension(data["imageSize"][0], result.width) &&
              dimension(data["imageSize"][1], result.height) && result.width == source.width &&
              result.height == source.height && stringField(data, "geometryHash") &&
              data["geometryHash"] == geometryHash(quads) && data.contains("items") &&
              data["items"].is_array() && data["items"].size() <= kRegions &&
              quads.shape ==
                  std::vector<std::int64_t>{static_cast<std::int64_t>(data["items"].size()), 4, 2},
          "regions尺寸、四边形配对或形状不匹配", "regions");
  std::set<std::string> ids;
  for (const auto& item : data["items"])
    require(item.is_object() && item.size() == 2 && stringField(item, "id") &&
                ids.insert(item["id"].get<std::string>()).second && item.contains("score") &&
                item["score"].is_number() && item["score"] >= 0 && item["score"] <= 1,
            "文字区域id须唯一且score为0..1", "regions");
  result.frame = data["frame"];
  result.hash = data["geometryHash"];
  result.items = data["items"];
  return result;
}
struct Layout {
  Json data;
  int width = 0, height = 0;
  std::size_t count = 0;
};
inline Layout layout(const Inputs& in) {
  Layout result;
  result.data = record(in, "layout", kLayout);
  const auto& data = result.data;
  require(data.size() == 10 && stringField(data, "frame") && data.value("unit", Json()) == "px" &&
              stringField(data, "geometryHash") &&
              (data.value("channelOrder", Json()) == "bgr" ||
               data.value("channelOrder", Json()) == "rgb") &&
              data.contains("imageSize") && data["imageSize"].is_array() &&
              data["imageSize"].size() == 2 && data.contains("recHeight") &&
              dimension(data["recHeight"], result.height, 48, 48) && data.contains("recWidth") &&
              dimension(data["recWidth"], result.width, 32, 2048) &&
              data.value("clsHeight", Json()) == 48 && data.value("clsWidth", Json()) == 192 &&
              data.contains("items") && data["items"].is_array() &&
              data["items"].size() <= kRegions,
          "OCR裁切布局字段无效", "layout");
  int imageWidth, imageHeight;
  require(dimension(data["imageSize"][0], imageWidth) &&
              dimension(data["imageSize"][1], imageHeight),
          "OCR原图尺寸无效", "layout");
  std::set<std::string> ids;
  for (const auto& item : data["items"]) {
    int width;
    require(
        item.is_object() && item.size() == 7 && stringField(item, "id") &&
            ids.insert(item["id"].get<std::string>()).second && item.contains("validWidth") &&
            dimension(item["validWidth"], width, 1, result.width) &&
            item.contains("detectorScore") && item["detectorScore"].is_number() &&
            item["detectorScore"] >= 0 && item["detectorScore"] <= 1 &&
            item.contains("rotated90") && item["rotated90"].is_boolean() &&
            (item.value("orientation", Json()) == 0 || item.value("orientation", Json()) == 180) &&
            (item.value("classifierLabel", Json()) == 0 ||
             item.value("classifierLabel", Json()) == 180) &&
            item.contains("orientationConfidence") && item["orientationConfidence"].is_number() &&
            item["orientationConfidence"] >= 0 && item["orientationConfidence"] <= 1,
        "OCR裁切条目字段无效", "layout");
  }
  result.count = data["items"].size();
  return result;
}
inline void tensorImage(const cv::Mat& matrix, Tensor& tensor, std::size_t index, int width,
                        const std::array<float, 3>& mean, const std::array<float, 3>& deviation) {
  const auto plane = static_cast<std::size_t>(matrix.rows) * width;
  for (int y = 0; y < matrix.rows; ++y)
    for (int x = 0; x < matrix.cols; ++x)
      for (int c = 0; c < 3; ++c)
        tensor.data[(index * 3 + c) * plane + static_cast<std::size_t>(y) * width + x] =
            (matrix.at<cv::Vec3f>(y, x)[c] - mean[c]) / deviation[c];
}
using Body = void (*)(const Inputs&, const ParamView&, Outputs&, ExecContext&);
inline Status guarded(Body body, const Inputs& in, const ParamView& p, Outputs& out,
                      ExecContext& ctx) {
  try {
    check(ctx);
    std::unordered_map<std::string, Data> staged;
    Outputs pending(staged);
    body(in, p, pending, ctx);
    check(ctx);
    for (auto& item : staged)
      out.set(item.first, std::move(item.second));
    return Status::Ok();
  } catch (const Failure& error) {
    return error.status;
  } catch (const cv::Exception& error) {
    return img::badInput(error.what());
  } catch (const std::bad_alloc&) {
    return Status::Error(Phase::Execute, "resource_limit", "OCR内存分配失败");
  } catch (const std::exception& error) {
    return img::badInput(error.what());
  }
}
inline Param real(const char* name, const char* label, double value, double low, double high) {
  auto p = img::floatParam(name, label, value, "");
  p.min = low;
  p.max = high;
  return p;
}
inline OperatorDesc descriptor(const char* id, const char* label, Body body) {
  auto op = image_p1::descriptor(
      id, label,
      "OCR optical character recognition text detection CTC Unicode 文字识别 光学字符识别");
  op.category = "图像/文字识别";
  op.compute = [body](const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
    return guarded(body, in, p, out, ctx);
  };
  return op;
}
} // namespace lyflow::ops::ocr
