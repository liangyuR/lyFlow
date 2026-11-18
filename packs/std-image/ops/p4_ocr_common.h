#pragma once

#include <array>
#include <cstring>
#include <iomanip>
#include <set>
#include <sstream>

#include "lyflow/pose.h"
#include "lyflow/value.h"
#include "lyflow_cv/compat.h"
#include "lyflow_ml/ocr_contract.h"
#include "p1_common.h"

namespace lyflow::ops::ocr {

constexpr std::size_t kPixels = 8 * 1024 * 1024;

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
inline void tensorImage(const cv::Mat& matrix, Tensor& tensor, std::size_t index, int width,
                        const std::array<float, 3>& mean, const std::array<float, 3>& deviation) {
  const auto plane = static_cast<std::size_t>(matrix.rows) * width;
  for (int y = 0; y < matrix.rows; ++y)
    for (int x = 0; x < matrix.cols; ++x)
      for (int c = 0; c < 3; ++c)
        tensor.data[(index * 3 + c) * plane + static_cast<std::size_t>(y) * width + x] =
            (matrix.at<cv::Vec3f>(y, x)[c] - mean[c]) / deviation[c];
}
} // namespace lyflow::ops::ocr
