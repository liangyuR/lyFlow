#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <vector>

#include "ops.h"
#include "params.h"

namespace lyflow::ops {
namespace {

constexpr std::size_t kAnalysisPixels = 16u * 1024u * 1024u;
constexpr std::size_t kComponentPixels = 4u * 1024u * 1024u;
constexpr std::size_t kRecordBytes = 1024u * 1024u;

Status checkSize(const Image& image, std::size_t limit = kAnalysisPixels) {
  if (!image.consistent() || image.pixelCount() > limit)
    return img::badInput("图像无效或超过本算子的像素预算 " + std::to_string(limit));
  return Status::Ok();
}

Status putRecord(Outputs& outputs, const char* port, const char* type, nlohmann::json data) {
  if (data.dump().size() > kRecordBytes)
    return Status::Error(Phase::Execute, "resource_limit",
                         "结果超过 1 MiB，请缩小 ROI 或提高筛选阈值");
  outputs.set(port, Data::record(Record{type, std::move(data)}));
  return Status::Ok();
}

Param vec(const char* name, const char* label, std::vector<double> values, bool pixels = false) {
  Param p;
  p.name = name;
  p.label = label;
  p.type = values.size() == 3 ? ParamType::Vec3f : ParamType::Vec4f;
  p.def = Value::vec(std::move(values));
  if (pixels)
    p.unit = "px";
  return p;
}

Port recordOut(const char* name, const char* type, const char* doc) {
  return withContract(Port{name, "Record", name, doc, true}, {{"recordType", type}});
}

Status maskFor(const Inputs& inputs, const Image& image, cv::Mat& mask) {
  mask = cv::Mat(image.height, image.width, CV_8U, cv::Scalar(255));
  if (!inputs.has("mask"))
    return Status::Ok();
  const Image* source = inputs.get("mask").asImage();
  if (!source || !source->consistent() || source->width != image.width ||
      source->height != image.height || source->channels != 1)
    return img::badInput("掩膜必须与图像同尺寸且为单通道", "mask");
  for (int y = 0; y < image.height; ++y)
    for (int x = 0; x < image.width; ++x) {
      const double v = source->at(x, y, 0);
      mask.at<unsigned char>(y, x) = std::isfinite(v) && v != 0 ? 255 : 0;
    }
  return Status::Ok();
}

Port optionalMask() {
  Port p = img::grayIn("mask", "可选非零区域，NaN/Inf 不选中。");
  p.required = false;
  return p;
}

Status convertCompute(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                      ExecContext&) {
  const Image& in = *inputs.get("image").asImage();
  const Status size = checkSize(in);
  if (!size.ok)
    return size;
  const std::string mode = params.choice("channels");
  const int target = mode == "gray" ? 1 : mode == "rgb" ? 3 : 4;
  if (in.channels == target) {
    outputs.set("image", inputs.get("image"));
    return Status::Ok();
  }
  const int code = in.channels == 1   ? (target == 3 ? cv::COLOR_GRAY2RGB : cv::COLOR_GRAY2RGBA)
                   : in.channels == 3 ? (target == 1 ? cv::COLOR_RGB2GRAY : cv::COLOR_RGB2RGBA)
                                      : (target == 1 ? cv::COLOR_RGBA2GRAY : cv::COLOR_RGBA2RGB);
  cv::Mat out;
  cv::cvtColor(cvx::view(in), out, code);
  return img::putMat(outputs, "image", std::move(out));
}

Status arithmeticCompute(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                         ExecContext& ctx) {
  const Image& a = *inputs.get("a").asImage();
  const Image& b = *inputs.get("b").asImage();
  const Status size = checkSize(a);
  if (!size.ok)
    return size;
  if (!b.consistent() || a.width != b.width || a.height != b.height || a.channels != b.channels ||
      a.depth != b.depth)
    return img::badInput("两个图像的尺寸、通道和位深必须相同", "b");
  if (ctx.cancelled())
    return Status::Ok();
  cv::Mat out;
  const std::string op = params.choice("operation");
  if (op == "add")
    cv::add(cvx::view(a), cvx::view(b), out);
  else if (op == "subtract")
    cv::subtract(cvx::view(a), cvx::view(b), out);
  else
    cv::absdiff(cvx::view(a), cvx::view(b), out);
  return img::putMat(outputs, "image", std::move(out));
}

Status changeCompute(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                     ExecContext& ctx) {
  const Image& a = *inputs.get("a").asImage();
  const Image& b = *inputs.get("b").asImage();
  const Status size = checkSize(a);
  if (!size.ok)
    return size;
  if (!b.consistent() || a.width != b.width || a.height != b.height || a.channels != b.channels ||
      a.depth != b.depth)
    return img::badInput("两个图像的尺寸、通道和位深必须相同", "b");
  cv::Mat mask;
  const Status valid = maskFor(inputs, a, mask);
  if (!valid.ok)
    return valid;
  cv::Mat changed(a.height, a.width, CV_8U, cv::Scalar(0));
  std::size_t count = 0, changes = 0;
  double sum = 0, square = 0, maximum = 0;
  const double threshold = params.number("threshold");
  const int openRadius = static_cast<int>(params.integer("openRadius"));
  const double minArea = params.number("minArea");
  if (!std::isfinite(threshold) || threshold < 0)
    return img::badParam("变化阈值必须是非负有限数", "threshold");
  if (!std::isfinite(minArea) || minArea < 0)
    return img::badParam("最小变化面积必须为非负有限数", "minArea");
  Ticker ticker(ctx, a.pixelCount());
  for (int y = 0; y < a.height; ++y)
    for (int x = 0; x < a.width; ++x) {
      if (ticker.tick(static_cast<std::size_t>(y) * a.width + x))
        return Status::Ok();
      if (!mask.at<unsigned char>(y, x))
        continue;
      double delta = 0;
      bool finite = true;
      double luminanceA = 0, luminanceB = 0;
      const int channels = a.channels == 4 ? 3 : a.channels;
      for (int c = 0; c < channels; ++c) {
        const double av = a.at(x, y, c), bv = b.at(x, y, c);
        finite = finite && std::isfinite(av) && std::isfinite(bv);
        delta = std::max(delta, std::abs(av - bv));
        const double weight = channels == 1 ? 1.0 : c == 0 ? 0.299 : c == 1 ? 0.587 : 0.114;
        luminanceA += weight * av;
        luminanceB += weight * bv;
      }
      if (params.choice("reduction") == "luma")
        delta = std::abs(luminanceA - luminanceB);
      if (!finite) {
        mask.at<unsigned char>(y, x) = 0;
        continue;
      }
      ++count;
      sum += delta;
      square += delta * delta;
      maximum = std::max(maximum, delta);
      if (delta > threshold) {
        changed.at<unsigned char>(y, x) = 255;
        ++changes;
      }
    }
  if (openRadius > 0) {
    const int side = openRadius * 2 + 1;
    cv::morphologyEx(changed, changed, cv::MORPH_OPEN,
                     cv::getStructuringElement(cv::MORPH_CROSS, cv::Size(side, side)));
    changed.setTo(0, mask == 0);
  }
  const int filtered = cv::countNonZero(changed);
  nlohmann::json result{{"valid", count > 0},
                        {"sampleCount", count},
                        {"changedSamples", changes},
                        {"filteredChangedSamples", filtered},
                        {"different", filtered * static_cast<double>(a.scale) * a.scale > minArea},
                        {"scale", a.scale},
                        {"channelReduction", params.choice("reduction")}};
  result["changedFraction"] =
      count ? nlohmann::json(static_cast<double>(changes) / count) : nullptr;
  result["meanAbsoluteDifference"] = count ? nlohmann::json(sum / count) : nullptr;
  result["rmsDifference"] = count ? nlohmann::json(std::sqrt(square / count)) : nullptr;
  result["maxAbsoluteDifference"] = count ? nlohmann::json(maximum) : nullptr;
  Status status = putRecord(outputs, "stats", "image.Change.v1", std::move(result));
  if (!status.ok)
    return status;
  return img::putMat(outputs, "mask", std::move(changed));
}

Status rangeCompute(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                    ExecContext& ctx) {
  const Image& in = *inputs.get("image").asImage();
  const Status size = checkSize(in);
  if (!size.ok)
    return size;
  const auto lower = params.vec3("lower"), upper = params.vec3("upper");
  const bool hsv = params.choice("space") == "hsv";
  const bool gray = params.choice("space") == "gray";
  if ((gray && in.channels != 1) || (!gray && in.channels != 3 && in.channels != 4))
    return img::badInput("Gray 模式需要单通道；RGB/HSV 模式需要 RGB 或 RGBA 图像");
  for (int c = 0; c < 3; ++c)
    if (!std::isfinite(lower[c]) || !std::isfinite(upper[c]) ||
        ((!hsv || c != 0) && lower[c] > upper[c]))
      return img::badParam("范围必须有限，除 HSV 色相环绕外下界不得大于上界", "lower");
  if (hsv && (lower[0] < 0 || upper[0] > 360 || lower[0] > 360 || upper[0] < 0 || lower[1] < 0 ||
              upper[1] > 1 || lower[2] < 0 || upper[2] > 1))
    return img::badParam("HSV 使用 H=0..360 度、S/V=0..1", "lower");
  cv::Mat rgb;
  if (in.channels == 4)
    cv::cvtColor(cvx::view(in), rgb, cv::COLOR_RGBA2RGB);
  else
    rgb = cvx::view(in);
  cv::Mat converted;
  if (hsv) {
    rgb.convertTo(converted, CV_32F, 1.0 / cvx::fullScale(rgb.depth()));
    cv::cvtColor(converted, converted, cv::COLOR_RGB2HSV);
  }
  cv::Mat out(in.height, in.width, CV_8U, cv::Scalar(0));
  Ticker ticker(ctx, in.pixelCount());
  for (int y = 0; y < in.height; ++y)
    for (int x = 0; x < in.width; ++x) {
      if (ticker.tick(static_cast<std::size_t>(y) * in.width + x))
        return Status::Ok();
      bool selected = true, finite = true;
      for (int c = 0; c < (gray ? 1 : 3); ++c) {
        const double original = in.at(x, y, c);
        const double v = hsv ? converted.at<cv::Vec3f>(y, x)[c] : original;
        const double epsilon = hsv ? (c == 0 ? 1e-5 : 1e-6) : 0;
        const bool inside = hsv && c == 0 && lower[c] > upper[c]
                                ? v >= lower[c] - epsilon || v <= upper[c] + epsilon
                                : v >= lower[c] - epsilon && v <= upper[c] + epsilon;
        finite = finite && std::isfinite(original) && std::isfinite(v);
        selected = selected && inside;
      }
      if (params.flag("invert"))
        selected = !selected;
      out.at<unsigned char>(y, x) = finite && selected ? 255 : 0;
    }
  return img::putMat(outputs, "mask", std::move(out));
}

Status roiCompute(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                  ExecContext& ctx) {
  const Image& image = *inputs.get("image").asImage();
  const Status size = checkSize(image);
  if (!size.ok)
    return size;
  auto roi = params.vec4("roi");
  if (inputs.has("box")) {
    const Box2D& box = *inputs.get("box").asBox2D();
    if (box.unit != Unit2D::Pixel)
      return img::badInput("ROI 框必须使用像素坐标", "box");
    roi = {box.min[0], box.min[1], box.max[0], box.max[1]};
  }
  for (float v : roi)
    if (!std::isfinite(v))
      return img::badParam("ROI 坐标必须有限", "roi");
  if (roi[2] < roi[0] || roi[3] < roi[1])
    return img::badParam("ROI 右下坐标不得小于左上坐标", "roi");
  cv::Mat mask;
  const Status valid = maskFor(inputs, image, mask);
  if (!valid.ok)
    return valid;
  const int x0 = static_cast<int>(std::floor(std::clamp<double>(roi[0], 0, image.width)));
  const int y0 = static_cast<int>(std::floor(std::clamp<double>(roi[1], 0, image.height)));
  const int x1 = static_cast<int>(std::ceil(std::clamp<double>(roi[2], 0, image.width)));
  const int y1 = static_cast<int>(std::ceil(std::clamp<double>(roi[3], 0, image.height)));
  cv::Mat out(image.height, image.width, CV_8U, cv::Scalar(0));
  const std::string shape = inputs.has("box") ? "rectangle" : params.choice("shape");
  if (shape == "rectangle") {
    if (roi[2] > roi[0] && roi[3] > roi[1] && x1 > x0 && y1 > y0)
      out(cv::Rect(x0, y0, x1 - x0, y1 - y0)).setTo(255);
  } else if (shape == "polygon") {
    const std::string& encoded = params.text("points");
    if (encoded.size() > 65536)
      return img::badParam("多边形 JSON 不得超过 64 KiB", "points");
    auto points = nlohmann::json::parse(encoded, nullptr, false);
    if (!points.is_array() || points.size() < 3 || points.size() > 4096)
      return img::badParam("多边形需要 3..4096 个 [x,y] 点", "points");
    std::vector<cv::Point> path;
    path.reserve(points.size());
    for (const auto& p : points) {
      if (!p.is_array() || p.size() != 2 || !p[0].is_number() || !p[1].is_number())
        return img::badParam("多边形点必须为数值 [x,y]", "points");
      const double x = p[0].get<double>() / image.scale, y = p[1].get<double>() / image.scale;
      if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > 1000000 || std::abs(y) > 1000000)
        return img::badParam("多边形坐标必须有限且绝对值不超过一百万像素", "points");
      path.emplace_back(static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y)));
    }
    cv::fillPoly(out, std::vector<std::vector<cv::Point>>{path}, cv::Scalar(255));
  } else {
    const auto center = params.vec2("center");
    const auto size = params.vec2("size");
    const double angle = params.number("angle") * 3.14159265358979323846 / 180;
    const double radius = params.number("radius"), inner = params.number("innerRadius");
    const double start = params.number("startAngle"), end = params.number("endAngle");
    if (!std::isfinite(center[0]) || !std::isfinite(center[1]) || !std::isfinite(size[0]) ||
        !std::isfinite(size[1]) || !std::isfinite(angle) || !std::isfinite(radius) ||
        !std::isfinite(inner) || !std::isfinite(start) || !std::isfinite(end))
      return img::badParam("ROI 几何参数必须有限", "center");
    if ((shape == "rotated_rectangle" && (size[0] <= 0 || size[1] <= 0)) ||
        ((shape == "circle" || shape == "arc") && radius <= 0) ||
        (shape == "arc" && (inner < 0 || inner > radius)))
      return img::badParam("ROI 尺寸/半径参数无效", "size");
    const auto degrees = [](double v) {
      const double r = std::fmod(v, 360.0);
      return r < 0 ? r + 360 : r;
    };
    const bool full = std::abs(end - start) >= 360;
    const double span = degrees(end - start);
    if (shape == "arc" && !full && span == 0)
      return img::badParam("圆弧角度跨度不得为零", "endAngle");
    const double cosine = std::cos(angle), sine = std::sin(angle);
    for (int y = 0; y < image.height; ++y)
      for (int x = 0; x < image.width; ++x) {
        if (x == 0 && ctx.cancelled())
          return Status::Ok();
        const double dx = x - center[0], dy = y - center[1];
        bool included = false;
        if (shape == "rotated_rectangle") {
          const double localX = cosine * dx + sine * dy;
          const double localY = -sine * dx + cosine * dy;
          included = std::abs(localX) <= size[0] / 2 && std::abs(localY) <= size[1] / 2;
        } else {
          const double r2 = dx * dx + dy * dy;
          included = r2 <= radius * radius;
          if (shape == "arc")
            included = included && r2 >= inner * inner &&
                       (full ||
                        degrees(std::atan2(dy, dx) * 180 / 3.14159265358979323846 - start) <= span);
        }
        out.at<unsigned char>(y, x) = included ? 255 : 0;
      }
  }
  if (params.flag("invert"))
    cv::bitwise_not(out, out);
  out &= mask;
  return img::putMat(outputs, "mask", std::move(out));
}

Status statisticsCompute(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                         ExecContext& ctx) {
  const Image& image = *inputs.get("image").asImage();
  const Status size = checkSize(image);
  if (!size.ok)
    return size;
  const int channel = static_cast<int>(params.integer("channel"));
  if (channel < 0 || channel >= image.channels)
    return img::badParam("通道索引超出输入图像范围", "channel");
  cv::Mat mask;
  const Status valid = maskFor(inputs, image, mask);
  if (!valid.ok)
    return valid;
  std::vector<float> values;
  std::vector<std::uint64_t> histogram;
  if (image.depth == PixelDepth::F32) {
    if (image.pixelCount() > 4u * 1024u * 1024u)
      return Status::Error(Phase::Execute, "resource_limit",
                           "精确浮点统计仅支持不超过 4 MP 的图像");
    values.reserve(image.pixelCount());
  } else {
    histogram.resize(image.depth == PixelDepth::U8 ? 256 : 65536, 0);
  }
  double sum = 0;
  std::size_t invalid = 0, count = 0;
  Ticker ticker(ctx, image.pixelCount());
  for (int y = 0; y < image.height; ++y)
    for (int x = 0; x < image.width; ++x) {
      if (ticker.tick(static_cast<std::size_t>(y) * image.width + x))
        return Status::Ok();
      if (!mask.at<unsigned char>(y, x))
        continue;
      const double v = image.at(x, y, channel);
      if (std::isfinite(v)) {
        if (histogram.empty())
          values.push_back(static_cast<float>(v));
        else
          ++histogram[static_cast<std::size_t>(v)];
        ++count;
        sum += v;
      } else {
        ++invalid;
      }
    }
  nlohmann::json result{{"valid", count > 0},        {"sampleCount", count},
                        {"invalidSamples", invalid}, {"channel", channel},
                        {"scale", image.scale},      {"modeDefinition", "exact_value_lowest_tie"}};
  for (const char* key : {"min", "max", "mean", "median", "mode"})
    result[key] = nullptr;
  if (count > 0 && !histogram.empty()) {
    std::uint64_t cumulative = 0, bestCount = 0;
    double mode = 0, lowMiddle = 0, highMiddle = 0;
    const std::size_t lowRank = (count - 1) / 2, highRank = count / 2;
    bool first = true;
    for (std::size_t i = 0; i < histogram.size(); ++i) {
      const auto frequency = histogram[i];
      if (!frequency)
        continue;
      if (first) {
        result["min"] = i;
        first = false;
      }
      result["max"] = i;
      if (frequency > bestCount) {
        bestCount = frequency;
        mode = static_cast<double>(i);
      }
      if (cumulative <= lowRank && cumulative + frequency > lowRank)
        lowMiddle = static_cast<double>(i);
      if (cumulative <= highRank && cumulative + frequency > highRank)
        highMiddle = static_cast<double>(i);
      cumulative += frequency;
    }
    result["mean"] = sum / count;
    result["median"] = (lowMiddle + highMiddle) / 2;
    result["mode"] = mode;
  } else if (!values.empty()) {
    constexpr std::size_t chunk = 65536;
    for (std::size_t begin = 0; begin < values.size(); begin += chunk) {
      if (ctx.cancelled())
        return Status::Ok();
      std::sort(values.begin() + begin, values.begin() + std::min(begin + chunk, values.size()));
    }
    for (std::size_t width = chunk; width < values.size(); width *= 2)
      for (std::size_t begin = 0; begin < values.size(); begin += width * 2) {
        if (ctx.cancelled())
          return Status::Ok();
        const auto middle = std::min(begin + width, values.size());
        const auto end = std::min(begin + width * 2, values.size());
        std::inplace_merge(values.begin() + begin, values.begin() + middle, values.begin() + end);
      }
    result["min"] = values.front();
    result["max"] = values.back();
    result["mean"] = sum / values.size();
    const std::size_t n = values.size();
    result["median"] =
        n % 2 ? static_cast<double>(values[n / 2])
              : (static_cast<double>(values[n / 2 - 1]) + static_cast<double>(values[n / 2])) / 2;
    std::size_t bestCount = 0, run = 0;
    double best = values.front(), previous = values.front();
    for (double value : values) {
      run = value == previous ? run + 1 : 1;
      if (run > bestCount) {
        bestCount = run;
        best = value;
      }
      previous = value;
    }
    result["mode"] = best;
  }
  return putRecord(outputs, "stats", "image.Statistics.v1", std::move(result));
}

Status componentsCompute(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                         ExecContext& ctx) {
  const Image& image = *inputs.get("image").asImage();
  const Status size = checkSize(image, kComponentPixels);
  if (!size.ok)
    return size;
  const int connectivity = static_cast<int>(params.integer("connectivity"));
  if (connectivity != 4 && connectivity != 8)
    return img::badParam("连通性仅支持 4 或 8", "connectivity");
  cv::Mat mask;
  const Status valid = maskFor(inputs, image, mask);
  if (!valid.ok)
    return valid;
  Ticker ticker(ctx, image.pixelCount());
  for (int y = 0; y < image.height; ++y)
    for (int x = 0; x < image.width; ++x) {
      if (ticker.tick(static_cast<std::size_t>(y) * image.width + x))
        return Status::Ok();
      const double v = image.at(x, y, 0);
      if (!std::isfinite(v) || v == 0)
        mask.at<unsigned char>(y, x) = 0;
    }
  cv::Mat labels, stats, centroids;
  const int count = cv::connectedComponentsWithStats(mask, labels, stats, centroids, connectivity,
                                                     CV_32S, cv::CCL_SAUF);
  const double scale = image.scale;
  const double minArea = params.number("minArea") / (scale * scale);
  const double maxArea = params.number("maxArea") / (scale * scale);
  if (!std::isfinite(minArea) || !std::isfinite(maxArea) || minArea < 0 || maxArea < minArea)
    return img::badParam("面积范围必须有限且 0 ≤ minArea ≤ maxArea", "minArea");
  const int limit = static_cast<int>(params.integer("maxRegions"));
  const bool contours = params.flag("contours");
  const int contourLimit = static_cast<int>(params.integer("maxContourPoints"));
  const double minAspect = params.number("minAspect"), maxAspect = params.number("maxAspect");
  if (!std::isfinite(minAspect) || !std::isfinite(maxAspect) || minAspect < 0 || maxAspect > 1 ||
      minAspect > maxAspect)
    return img::badParam("长宽比范围需满足 0 ≤ minAspect ≤ maxAspect ≤ 1", "minAspect");
  std::size_t recordBytes = 256, boxPixels = 0, derivedPoints = 0, exportedPoints = 0;
  nlohmann::json regions = nlohmann::json::array();
  cv::Mat selected(image.height, image.width, CV_8U, cv::Scalar(0));
  std::vector<int> labelsInOrder;
  for (int i = 1; i < count; ++i)
    labelsInOrder.push_back(i);
  if (params.choice("order") == "area_desc")
    std::stable_sort(labelsInOrder.begin(), labelsInOrder.end(), [&](int a, int b) {
      return stats.at<int>(a, cv::CC_STAT_AREA) > stats.at<int>(b, cv::CC_STAT_AREA);
    });
  for (int i : labelsInOrder) {
    if (ctx.cancelled())
      return Status::Ok();
    const double area = stats.at<int>(i, cv::CC_STAT_AREA);
    if (area < minArea || area > maxArea)
      continue;
    const int x = stats.at<int>(i, cv::CC_STAT_LEFT), y = stats.at<int>(i, cv::CC_STAT_TOP);
    const int w = stats.at<int>(i, cv::CC_STAT_WIDTH), h = stats.at<int>(i, cv::CC_STAT_HEIGHT);
    boxPixels += static_cast<std::size_t>(w) * h;
    if (boxPixels > 32u * 1024u * 1024u)
      return Status::Error(Phase::Execute, "resource_limit",
                           "连通域包围框扫描总预算超过 32 MP，请缩小 ROI 或筛选区域");
    const cv::Rect box(x, y, w, h);
    const cv::Mat local = labels(box) == i;
    std::vector<std::vector<cv::Point>> paths;
    cv::findContours(local, paths, cv::RETR_LIST, cv::CHAIN_APPROX_SIMPLE);
    std::vector<cv::Point> allPoints;
    for (const auto& path : paths) {
      derivedPoints += path.size();
      if (derivedPoints > 262144)
        return Status::Error(Phase::Execute, "resource_limit",
                             "连通域几何轮廓总预算超过 262144 点");
      allPoints.insert(allPoints.end(), path.begin(), path.end());
    }
    const cv::RotatedRect oriented = cv::minAreaRect(allPoints);
    const double small = std::min(oriented.size.width, oriented.size.height);
    const double large = std::max(oriented.size.width, oriented.size.height);
    const double aspect = large > 0 ? small / large : 0;
    if (params.flag("filterAspect") && (aspect < minAspect || aspect > maxAspect))
      continue;
    if (regions.size() >= static_cast<std::size_t>(limit)) {
      if (params.flag("takeLargest"))
        break;
      return Status::Error(Phase::Execute, "resource_limit",
                           "连通域数量超过 maxRegions，请先筛选区域");
    }
    selected(box).setTo(255, local);
    nlohmann::json region{
        {"id", i},
        {"area", area * scale * scale},
        {"centroid", {centroids.at<double>(i, 0) * scale, centroids.at<double>(i, 1) * scale}},
        {"bbox", {x * scale, y * scale, (x + w) * scale, (y + h) * scale}}};
    region["aspectRatio"] = aspect;
    region["rotatedBox"] = {
        {"center", {(x + oriented.center.x) * scale, (y + oriented.center.y) * scale}},
        {"size", {oriented.size.width * scale, oriented.size.height * scale}},
        {"angleDegrees", oriented.angle}};
    region["contours"] = nlohmann::json::array();
    if (contours) {
      std::size_t points = 0;
      for (const auto& path : paths) {
        points += path.size();
        exportedPoints += path.size();
        if (points > static_cast<std::size_t>(contourLimit))
          return Status::Error(Phase::Execute, "resource_limit", "轮廓点超过 maxContourPoints");
        if (exportedPoints > 32768)
          return Status::Error(Phase::Execute, "resource_limit", "导出轮廓总预算超过 32768 点");
        nlohmann::json vertices = nlohmann::json::array();
        for (const auto& point : path)
          vertices.push_back({(x + point.x) * scale, (y + point.y) * scale});
        region["contours"].push_back(std::move(vertices));
      }
    }
    recordBytes += region.dump().size() + 1;
    if (recordBytes > kRecordBytes)
      return Status::Error(Phase::Execute, "resource_limit",
                           "连通域结果超过 1 MiB，请减少轮廓或筛选区域");
    regions.push_back(std::move(region));
  }
  nlohmann::json result{
      {"unit", "px"},         {"areaUnit", "px²"},       {"coordinateSpace", "source"},
      {"scale", image.scale}, {"count", regions.size()}, {"regions", std::move(regions)}};
  Status status = putRecord(outputs, "regions", "image.Regions.v1", std::move(result));
  if (!status.ok)
    return status;
  return img::putMat(outputs, "mask", std::move(selected));
}

OperatorDesc base(const char* id, const char* label, const char* category, const char* doc,
                  ComputeFn compute) {
  OperatorDesc op;
  op.id = id;
  op.version = "1.0.0";
  op.label = label;
  op.category = category;
  op.doc = doc;
  op.capabilities = {true, false, true};
  op.compute = std::move(compute);
  return op;
}

} // namespace

void registerImageAnalysis(Registry& registry) {
  {
    auto op =
        base("image.convert_channels", "图像通道转换", "图像/基础",
             "灰度/RGB/RGBA 转换，保持 u8/u16/f32 位深；新增 alpha 为满量程。", convertCompute);
    op.inputs = {img::imageIn()};
    op.outputs = {img::imageOut()};
    op.params = {img::enumParam("channels", "目标通道", "rgb",
                                {{"gray", "灰度", ""}, {"rgb", "RGB", ""}, {"rgba", "RGBA", ""}},
                                "")};
    op.keywords = {"convert image type", "channels", "RGB", "RGBA", "灰度转彩色", "图像类型转换"};
    registry.addOperator(std::move(op));
  }
  {
    auto op = base("image.arithmetic", "图像算术运算", "图像/基础",
                   "同尺寸/通道/位深图像加、减及绝对差。整数饱和，f32 保留带符号与非有限值。",
                   arithmeticCompute);
    op.inputs = {img::imageIn("a"), img::imageIn("b")};
    op.outputs = {img::imageOut()};
    op.params = {img::enumParam(
        "operation", "运算", "absdiff",
        {{"add", "加法", ""}, {"subtract", "减法", ""}, {"absdiff", "绝对差", ""}}, "")};
    op.keywords = {"image difference", "subtract", "absdiff", "图像差分", "图像加减"};
    registry.addOperator(std::move(op));
  }
  {
    auto op = base(
        "image.change_stats", "图像变化统计", "图像/量测",
        "按灰度亮度或最大通道绝对差统计变化，支持十字开运算抗孤立噪声和最小变化面积判定；忽略非有限像素及 alpha。",
        changeCompute);
    op.inputs = {img::imageIn("a"), img::imageIn("b"), optionalMask()};
    op.outputs = {img::imageOut("mask"),
                  recordOut("stats", "image.Change.v1", "变化统计，空区域 valid=false。")};
    Param area = img::floatParam("minArea", "最小变化面积", 0,
                                 "滤波后变化像素面积严格大于该值时 different=true。");
    area.unit = "px²";
    Param radius = img::intParam("openRadius", "开运算半径", 0, 0, 31, "零表示关闭；十字结构元。");
    radius.unit = "px";
    op.params = {img::floatParam("threshold", "变化阈值", 10, "严格大于该阈值才计为变化。"),
                 img::enumParam("reduction", "通道归约", "luma",
                                {{"luma", "灰度亮度", "RGB BT.601"}, {"max_abs", "最大通道差", ""}},
                                ""),
                 radius, area};
    op.keywords = {"check image change", "difference statistics", "变化检测", "变化比例"};
    registry.addOperator(std::move(op));
  }
  {
    auto op = base(
        "image.range_mask", "颜色范围分割", "图像/分割",
        "灰度/RGB 原值范围或 HSV 范围，闭区间；HSV 使用 H 度、S/V 0..1，H 下界大于上界表示跨红色色相环绕。",
        rangeCompute);
    op.inputs = {img::imageIn()};
    op.outputs = {img::imageOut("mask")};
    op.params = {
        img::enumParam(
            "space", "颜色空间", "rgb",
            {{"gray", "灰度", "只使用范围第一个分量"}, {"rgb", "RGB", ""}, {"hsv", "HSV", ""}}, ""),
        vec("lower", "下界", {0, 0, 0}), vec("upper", "上界", {255, 255, 255}),
        img::boolParam("invert", "反选", false, "非有限像素始终不选中。")};
    op.keywords = {"color threshold", "HSV in range", "颜色阈值", "颜色掩膜", "RGB分割"};
    registry.addOperator(std::move(op));
  }
  {
    auto op = base(
        "image.roi_mask", "几何区域掩膜", "图像/分割",
        "矩形、旋转矩形、圆、圆环弧或多边形生成掩膜，与可选输入掩膜相交，保持图像尺寸；不携带外部隐式 ROI 元数据。",
        roiCompute);
    Port box{"box", "Box2D", "ROI", "可选像素框，连接后覆盖参数 ROI。", false};
    op.inputs = {img::imageIn(), box, optionalMask()};
    op.outputs = {img::imageOut("mask")};
    Param center;
    center.name = "center";
    center.label = "中心";
    center.type = ParamType::Vec2f;
    center.def = Value::vec({50, 50});
    center.unit = "px";
    Param size = center;
    size.name = "size";
    size.label = "尺寸";
    size.def = Value::vec({50, 30});
    Param radius = img::floatParam("radius", "外半径", 20, "");
    Param inner = img::floatParam("innerRadius", "内半径", 10, "圆环弧使用。");
    radius.unit = inner.unit = "px";
    Param points;
    points.name = "points";
    points.label = "多边形点";
    points.type = ParamType::String;
    points.def = Value::text("[[0,0],[100,0],[0,100]]");
    points.doc = "原图像素坐标的 JSON 点数组，不随预览参数隐式缩放。";
    op.params = {img::enumParam("shape", "形状", "rectangle",
                                {{"rectangle", "矩形", ""},
                                 {"rotated_rectangle", "旋转矩形", ""},
                                 {"circle", "圆", ""},
                                 {"arc", "圆环弧", ""},
                                 {"polygon", "多边形", ""}},
                                ""),
                 vec("roi", "矩形 ROI", {0, 0, 100, 100}, true),
                 center,
                 size,
                 img::floatParam("angle", "旋转角度", 0, "度；从 +X 向 +Y 旋转。"),
                 radius,
                 inner,
                 img::floatParam("startAngle", "圆弧起角", 0, "度；从 +X 向 +Y。"),
                 img::floatParam("endAngle", "圆弧终角", 360, "跨度至少 360 度为完整圆环。"),
                 points,
                 img::boolParam("invert", "反选", false, "几何区域反选后与输入掩膜相交。")};
    op.keywords = {"ROI generator", "region mask", "感兴趣区域", "生成掩膜", "矩形ROI"};
    registry.addOperator(std::move(op));
  }
  {
    auto op = base(
        "image.statistics", "灰度统计", "图像/量测",
        "对选定通道与掩膜计算 min/max/mean/median/mode；忽略 NaN/Inf，众数按精确值，次数相同取较小值。",
        statisticsCompute);
    op.inputs = {img::imageIn(), optionalMask()};
    op.outputs = {recordOut("stats", "image.Statistics.v1",
                            "空区域 valid=false 且数值字段 null；sampleCount 是采样网格像素数。")};
    op.params = {img::intParam("channel", "通道", 0, 0, 3, "RGB 顺序，从零计数。")};
    op.keywords = {
        "gray scale statistics", "histogram median mode", "灰度统计", "中值", "众数", "平均灰度"};
    registry.addOperator(std::move(op));
  }
  {
    auto op = base(
        "image.components", "连通域分析", "图像/分割",
        "有限非零前景的连通域面积、质心、边界框和可选轮廓；空前景有效返回空集合，超预算报错而非截断。",
        componentsCompute);
    op.inputs = {img::grayIn(), optionalMask()};
    op.outputs = {
        recordOut("regions", "image.Regions.v1", "原图像素坐标，稳定扫描标签；面积为 px²。"),
        img::imageOut("mask")};
    Param minArea = img::floatParam("minArea", "最小面积", 1, "原图像素面积下界。");
    Param maxArea = img::floatParam("maxArea", "最大面积", static_cast<double>(kComponentPixels),
                                    "原图像素面积上界。");
    minArea.unit = maxArea.unit = "px²";
    op.params = {
        img::intParam("connectivity", "连通性", 8, 4, 8, "仅支持 4 或 8。"),
        minArea,
        maxArea,
        img::intParam("maxRegions", "最大区域数", 1024, 1, 4096, "超过时返回资源诊断。"),
        img::boolParam("contours", "输出轮廓", false, "包含内部孔洞边界。"),
        img::intParam("maxContourPoints", "每域轮廓点预算", 4096, 1, 16384, "不静默截断轮廓。"),
        img::boolParam("filterAspect", "筛选长宽比", false, "旋转包围框短边/长边。"),
        img::floatParam("minAspect", "最小长宽比", 0, ""),
        img::floatParam("maxAspect", "最大长宽比", 1, ""),
        img::enumParam(
            "order", "排序", "scan",
            {{"scan", "扫描顺序", ""}, {"area_desc", "面积降序", "相同面积保留标签顺序"}}, ""),
        img::boolParam("takeLargest", "显式截取前N个", false,
                       "开启时按当前排序截取 maxRegions 个；关闭时超限报错。")};
    op.keywords = {"blob detector",   "connected components",
                   "region centroid", "斑点检测",
                   "连通区域",        "区域筛选"};
    registry.addOperator(std::move(op));
  }
}

} // namespace lyflow::ops
