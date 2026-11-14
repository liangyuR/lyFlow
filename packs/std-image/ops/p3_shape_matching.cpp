#include <algorithm>
#include <array>
#include <cstring>
#include <iomanip>
#include <map>
#include <numeric>
#include <sstream>
#include <tuple>

#include "lyflow/pose.h"
#include "p1_common.h"

namespace lyflow::ops {
namespace {
using Json = nlohmann::json;
constexpr std::size_t kPixels = 8 * 1024 * 1024;
constexpr std::size_t kEdges = 262144;
constexpr std::size_t kWork = 256 * 1024 * 1024;
constexpr const char* kModelType = "vision.ShapeTemplate.v1";
constexpr const char* kQualityType = "image.ShapeMatches.v1";

struct Failure {
  Status status;
};

void check(ExecContext& ctx) {
  if (ctx.cancelled())
    throw Failure{Status::Error(Phase::Execute, "cancelled", "形状匹配已取消")};
}

void require(bool condition, const std::string& message, const char* port = "image") {
  if (!condition)
    throw Failure{img::badInput(message, port)};
}

[[noreturn]] void limit(const char* message) {
  throw Failure{Status::Error(Phase::Execute, "resource_limit", message)};
}

struct Budget {
  ExecContext& ctx;
  std::size_t work = 0;
  void add(std::size_t count) {
    if (count > kWork - work)
      limit("形状投票与支持检查超过256Mi预算");
    const auto previous = work;
    work += count;
    if (previous / 8192 != work / 8192)
      check(ctx);
  }
};

double number(const ParamView& p, const char* name, double low, double high, int scale = 1) {
  const double value = p.number(name);
  const double full = value * scale;
  if (!std::isfinite(value) || !std::isfinite(full) || full < low || full > high)
    throw Failure{img::badParam("参数超出声明的原图范围", name)};
  return value;
}

int integer(const ParamView& p, const char* name, int low, int high) {
  const auto value = p.integer(name);
  if (value < low || value > high)
    throw Failure{img::badParam("整数参数超出范围", name)};
  return static_cast<int>(value);
}

std::string text(const ParamView& p, const char* name) {
  const auto value = p.text(name);
  if (value.empty() || value.size() > 256)
    throw Failure{img::badParam("标识或参考帧必须为1..256字节", name)};
  return value;
}

cv::Mat normalized(const Image* image, const char* port) {
  require(image && image->consistent() && image->scale >= 1, "需要有效图像", port);
  if (image->pixelCount() > kPixels || image->byteSize() > 128 * 1024 * 1024)
    limit("形状图像超过8Mi像素或128MiB输入预算");
  if (2 * image->byteSize() + 32 * image->pixelCount() + 64 * 1024 * 1024 > 512 * 1024 * 1024)
    limit("形状图像处理预计峰值超过512MiB");
  const auto source = cvx::view(*image);
  if (image->depth == PixelDepth::F32)
    require(cv::checkRange(source, true, nullptr, 0, 1.00000001), "f32图像各通道须为有限0..1",
            port);
  cv::Mat gray = image_p1::gray(*image), result;
  gray.convertTo(result, CV_32F,
                 image->depth == PixelDepth::U8    ? 1.0 / 255
                 : image->depth == PixelDepth::U16 ? 1.0 / 65535
                                                   : 1);
  return result;
}

cv::Mat selection(const Inputs& in, const Image& image, const std::string& frame,
                  ExecContext& ctx) {
  require(!(in.has("roi") && in.has("region")), "roi与region互斥", "region");
  cv::Mat allowed(image.height, image.width, CV_8U, cv::Scalar(255));
  if (in.has("mask")) {
    const auto* mask = in.get("mask").asImage();
    require(mask && mask->consistent() && mask->width == image.width &&
                mask->height == image.height && mask->channels == 1 &&
                mask->depth == PixelDepth::U8 && mask->scale == image.scale,
            "mask须为同尺寸、同scale的u8单通道图像", "mask");
    cv::compare(cvx::view(*mask), 0, allowed, cv::CMP_NE);
  }
  const Box2D* box = nullptr;
  const Record* region = nullptr;
  double cx = 0, cy = 0, width = 0, height = 0, cosine = 1, sine = 0;
  if (in.has("roi")) {
    box = in.get("roi").asBox2D();
    require(box && box->unit == Unit2D::Pixel && std::isfinite(box->min[0]) &&
                std::isfinite(box->min[1]) && std::isfinite(box->max[0]) &&
                std::isfinite(box->max[1]) && box->max[0] > box->min[0] &&
                box->max[1] > box->min[1],
            "roi须为有限、正面积的像素Box2D", "roi");
  }
  if (in.has("region")) {
    region = in.get("region").asRecord();
    std::string why;
    require(region && region->type == poses::kPose2DType && poses::validate(*region, why),
            "region须为有效Pose2D：" + why, "region");
    const auto& d = region->data;
    require(d["frame"] == frame && d["unit"] == "px", "region参考帧/单位不匹配", "region");
    cx = d["position"][0].get<double>() / image.scale;
    cy = d["position"][1].get<double>() / image.scale;
    width = d["size"][0].get<double>() / image.scale;
    height = d["size"][1].get<double>() / image.scale;
    require(width > 0 && height > 0, "region在工作比例下尺寸无效", "region");
    cosine = std::cos(d["angle"].get<double>());
    sine = std::sin(d["angle"].get<double>());
  }
  if (box || region) {
    std::size_t geometric = 0;
    const double normalizer = std::max(width, height);
    for (int y = 0; y < image.height; ++y) {
      check(ctx);
      auto* row = allowed.ptr<std::uint8_t>(y);
      for (int x = 0; x < image.width; ++x) {
        bool inside;
        if (box)
          inside = x >= box->min[0] && x <= box->max[0] && y >= box->min[1] && y <= box->max[1];
        else {
          const double dx = (x - cx) / normalizer, dy = (y - cy) / normalizer;
          inside = std::abs(cosine * dx + sine * dy) <= width / normalizer / 2 &&
                   std::abs(-sine * dx + cosine * dy) <= height / normalizer / 2;
        }
        geometric += inside;
        if (!inside)
          row[x] = 0;
      }
    }
    require(geometric != 0, "ROI与图像没有像素交集", box ? "roi" : "region");
  }
  return allowed;
}

struct Feature {
  cv::Point2d point, normal;
  double strength = 0;
};

struct EdgeMap {
  cv::Mat image;
  std::vector<Feature> points;
};

EdgeMap extract(const cv::Mat& gray, const cv::Mat& mask, double sigma, double contrast,
                ExecContext& ctx) {
  cv::Mat smoothed = gray.clone();
  if (sigma > 0) {
    cv::Mat weights, weighted;
    mask.convertTo(weights, CV_32F, 1.0 / 255);
    cv::GaussianBlur(gray.mul(weights), weighted, {}, sigma, sigma, cv::BORDER_CONSTANT);
    cv::GaussianBlur(weights, weights, {}, sigma, sigma, cv::BORDER_CONSTANT);
    cv::divide(weighted, cv::max(weights, 1e-12), smoothed);
  }
  cv::Mat gx, gy, magnitudes, interior;
  cv::Sobel(smoothed, gx, CV_32F, 1, 0, 3, 1.0 / 8);
  cv::Sobel(smoothed, gy, CV_32F, 0, 1, 3, 1.0 / 8);
  cv::magnitude(gx, gy, magnitudes);
  cv::erode(mask, interior, cv::Mat::ones(3, 3, CV_8U), cv::Point(-1, -1), 2, cv::BORDER_CONSTANT,
            cv::Scalar(0));
  EdgeMap result{cv::Mat(gray.size(), CV_8U, cv::Scalar(0)), {}};
  for (int y = 1; y + 1 < gray.rows; ++y) {
    check(ctx);
    for (int x = 1; x + 1 < gray.cols; ++x) {
      if (!interior.at<std::uint8_t>(y, x))
        continue;
      const double dx = gx.at<float>(y, x), dy = gy.at<float>(y, x);
      const double magnitude = magnitudes.at<float>(y, x);
      if (magnitude < contrast || magnitude == 0)
        continue;
      double nx = dx / magnitude, ny = dy / magnitude;
      if (nx < 0 || (nx == 0 && ny < 0)) {
        nx = -nx;
        ny = -ny;
      }
      auto sample = [&](double px, double py) {
        const int ix = static_cast<int>(std::floor(px)), iy = static_cast<int>(std::floor(py));
        const int jx = std::min(ix + 1, gray.cols - 1), jy = std::min(iy + 1, gray.rows - 1);
        const double fx = px - ix, fy = py - iy;
        return (1 - fy) *
                   ((1 - fx) * magnitudes.at<float>(iy, ix) + fx * magnitudes.at<float>(iy, jx)) +
               fy * ((1 - fx) * magnitudes.at<float>(jy, ix) + fx * magnitudes.at<float>(jy, jx));
      };
      const double before = sample(x - nx, y - ny), after = sample(x + nx, y + ny);
      if (magnitude <= before || magnitude < after)
        continue;
      const double denominator = before - 2 * magnitude + after;
      const double offset =
          denominator < -1e-15 ? std::clamp(.5 * (before - after) / denominator, -.5, .5) : 0;
      if (result.points.size() == kEdges)
        limit("场景或模板边缘超过262144点预算");
      result.points.push_back(
          {{x + offset * nx, y + offset * ny}, {dx / magnitude, dy / magnitude}, magnitude});
      result.image.at<std::uint8_t>(y, x) = 255;
    }
  }
  return result;
}

std::vector<std::size_t> spread(const std::vector<Feature>& points, std::size_t count,
                                double spacing, Budget& budget) {
  std::vector<std::size_t> selected;
  if (points.empty())
    return selected;
  std::vector<double> distance(points.size(), std::numeric_limits<double>::infinity());
  std::size_t next = static_cast<std::size_t>(
      std::max_element(points.begin(), points.end(),
                       [](const auto& a, const auto& b) { return a.strength < b.strength; }) -
      points.begin());
  while (selected.size() < count) {
    selected.push_back(next);
    distance[next] = -1;
    budget.add(points.size());
    double farthest = -1;
    std::size_t farthestIndex = 0;
    for (std::size_t i = 0; i < points.size(); ++i) {
      if (distance[i] < 0)
        continue;
      const auto delta = points[i].point - points[next].point;
      distance[i] = std::min(distance[i], delta.dot(delta));
      if (distance[i] > farthest) {
        farthest = distance[i];
        farthestIndex = i;
      }
    }
    if (farthest < spacing * spacing)
      break;
    next = farthestIndex;
  }
  return selected;
}

std::string hash(const Tensor& tensor) {
  std::uint64_t value = 14695981039346656037ULL;
  for (float entry : tensor.data) {
    std::uint32_t bits;
    std::memcpy(&bits, &entry, sizeof(bits));
    for (int byte = 0; byte < 4; ++byte) {
      value ^= (bits >> (8 * byte)) & 255;
      value *= 1099511628211ULL;
    }
  }
  std::ostringstream stream;
  stream << std::hex << std::setfill('0') << std::setw(16) << value;
  return stream.str();
}

Image imageOutput(cv::Mat matrix, int scale) {
  Image result;
  require(cvx::fromMat(std::move(matrix), result), "输出图像格式无效");
  result.scale = scale;
  return result;
}

void preserveAlpha(const Image& source, cv::Mat& overlay) {
  if (source.channels == 4) {
    const int channels[] = {3, 3};
    const auto original = cvx::view(source);
    cv::mixChannels(&original, 1, &overlay, 1, channels, 1);
  }
}

void bounded(const Record& record) {
  std::string why;
  if (!values::boundedJson(record.data, why))
    limit("形状元数据或结果超过Record预算");
}

std::string geometry(const std::vector<Feature>& features, cv::Size2d size, cv::Point2d center,
                     cv::Size2d imageSize) {
  if (features.size() < 12)
    return "模板至少需要12个独立特征";
  double loX = 1e300, loY = 1e300, hiX = -1e300, hiY = -1e300;
  std::map<std::pair<int, int>, std::vector<cv::Point2d>> cells;
  cv::Point2d mean;
  for (const auto& feature : features) {
    const auto point = feature.point;
    const int x = static_cast<int>(std::floor(point.x * 4)),
              y = static_cast<int>(std::floor(point.y * 4));
    for (int j = y - 1; j <= y + 1; ++j)
      for (int i = x - 1; i <= x + 1; ++i) {
        const auto found = cells.find({i, j});
        if (found == cells.end())
          continue;
        for (const auto& other : found->second)
          if (cv::norm(point - other) < .2499)
            return "模板特征不能重复或在0.25原像素内重复计数";
      }
    cells[{x, y}].push_back(point);
    loX = std::min(loX, point.x);
    loY = std::min(loY, point.y);
    hiX = std::max(hiX, point.x);
    hiY = std::max(hiY, point.y);
    mean += point / static_cast<double>(features.size());
  }
  const double width = hiX - loX, height = hiY - loY;
  if (width < 2 || height < 2)
    return "模板特征须具有至少2原像素的二维跨度";
  const double extent = std::max(width, height);
  double xx = 0, xy = 0, yy = 0;
  for (const auto& feature : features) {
    const auto delta = (feature.point - mean) / extent;
    xx += delta.x * delta.x;
    xy += delta.x * delta.y;
    yy += delta.y * delta.y;
  }
  if (xx * yy - xy * xy <= 1e-12 * (xx + yy) * (xx + yy))
    return "模板特征共线或二维几何退化";
  const double tolerance = std::max(1e-4, 2 * std::numeric_limits<float>::epsilon() * extent);
  const double paddingX = size.width - width, paddingY = size.height - height;
  const double scale = std::round((paddingX + paddingY) / 2);
  if (std::abs(loX + hiX) > tolerance || std::abs(loY + hiY) > tolerance || !std::isfinite(scale) ||
      scale < 1 || scale > std::numeric_limits<std::int32_t>::max() ||
      std::abs(paddingX - scale) > tolerance || std::abs(paddingY - scale) > tolerance)
    return "模型中心或size与真实特征范围及工作像素宽度不一致";
  const double workingWidth = imageSize.width / scale, workingHeight = imageSize.height / scale;
  if (workingWidth < 1 || workingHeight < 1 ||
      workingWidth > std::numeric_limits<std::int32_t>::max() ||
      workingHeight > std::numeric_limits<std::int32_t>::max() ||
      std::abs(workingWidth - std::round(workingWidth)) > 1e-6 ||
      std::abs(workingHeight - std::round(workingHeight)) > 1e-6 ||
      workingWidth * workingHeight > kPixels || center.x - size.width / 2 < -tolerance ||
      center.y - size.height / 2 < -tolerance ||
      center.x + size.width / 2 > imageSize.width + tolerance ||
      center.y + size.height / 2 > imageSize.height + tolerance)
    return "模型尺寸、中心或工作像素比例与训练图范围不一致";
  return {};
}

Status train(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  try {
    check(ctx);
    const auto* source = in.get("image").asImage();
    const cv::Mat gray = normalized(source, "image");
    const auto frame = text(p, "frame"), id = text(p, "id");
    const double contrast = number(p, "minContrast", 1e-6, 1);
    const double sigma = number(p, "smoothSigma", 0, 8, source->scale);
    const double spacing = number(p, "featureSpacing", .25, 64, source->scale);
    const int count = integer(p, "maxFeatures", 12, 1024);
    const auto mask = selection(in, *source, frame, ctx);
    const auto edge = extract(gray, mask, sigma, contrast, ctx);
    Budget budget{ctx};
    const auto selected = spread(edge.points, count, spacing, budget);
    if (selected.size() < 12)
      return Status::Error(Phase::Execute, "degenerate_template", "模板至少需要12个分散边缘特征");
    double loX = source->width, loY = source->height, hiX = 0, hiY = 0;
    for (const auto i : selected) {
      loX = std::min(loX, edge.points[i].point.x);
      loY = std::min(loY, edge.points[i].point.y);
      hiX = std::max(hiX, edge.points[i].point.x);
      hiY = std::max(hiY, edge.points[i].point.y);
    }
    if (hiX - loX < 2 || hiY - loY < 2)
      return Status::Error(Phase::Execute, "degenerate_template", "模板边缘须具有二维空间跨度");
    require((hiX - loX + 1) * source->scale <= 1e7 && (hiY - loY + 1) * source->scale <= 1e7,
            "模板尺寸超过1e7原图像素范围");
    const cv::Point2d center((loX + hiX) / 2, (loY + hiY) / 2);
    Tensor features;
    features.shape = {static_cast<std::int64_t>(selected.size()), 4};
    features.data.reserve(selected.size() * 4);
    cv::Mat overlay = cvx::view(*source).clone();
    const double full = source->depth == PixelDepth::U8    ? 255
                        : source->depth == PixelDepth::U16 ? 65535
                                                           : 1;
    for (const auto i : selected) {
      const auto& f = edge.points[i];
      features.data.insert(features.data.end(),
                           {static_cast<float>((f.point.x - center.x) * source->scale),
                            static_cast<float>((f.point.y - center.y) * source->scale),
                            static_cast<float>(f.normal.x), static_cast<float>(f.normal.y)});
      cv::circle(overlay, f.point, 1,
                 source->channels == 1 ? cv::Scalar(full) : cv::Scalar(0, full, 0, full), -1);
    }
    std::vector<Feature> deliveredFeatures;
    for (std::size_t i = 0; i < features.data.size(); i += 4)
      deliveredFeatures.push_back({{features.data[i], features.data[i + 1]},
                                   {features.data[i + 2], features.data[i + 3]},
                                   1});
    const auto geometryError = geometry(
        deliveredFeatures, {(hiX - loX + 1) * source->scale, (hiY - loY + 1) * source->scale},
        center * source->scale,
        {static_cast<double>(source->width) * source->scale,
         static_cast<double>(source->height) * source->scale});
    if (!geometryError.empty())
      return Status::Error(Phase::Execute, "degenerate_template", geometryError);
    Record model{kModelType,
                 {{"method", "oriented-chamfer-v1"},
                  {"id", id},
                  {"frame", frame},
                  {"unit", "px"},
                  {"imageSize",
                   {static_cast<double>(source->width) * source->scale,
                    static_cast<double>(source->height) * source->scale}},
                  {"center", {center.x * source->scale, center.y * source->scale}},
                  {"size", {(hiX - loX + 1) * source->scale, (hiY - loY + 1) * source->scale}},
                  {"featureCount", selected.size()},
                  {"featuresHash", hash(features)}}};
    bounded(model);
    preserveAlpha(*source, overlay);
    auto image = imageOutput(std::move(overlay), source->scale);
    check(ctx);
    out.set("features", Data::tensor(std::move(features)));
    out.set("model", Data::record(std::move(model)));
    out.set("image", Data::image(std::move(image)));
    return Status::Ok();
  } catch (const Failure& e) {
    return e.status;
  } catch (const cv::Exception& e) {
    return img::fromCvError(e);
  } catch (const std::bad_alloc&) {
    return Status::Error(Phase::Execute, "resource_limit", "形状建模内存不足");
  }
}

struct Model {
  std::vector<Feature> features;
  std::string id;
  cv::Size2d size;
};

Model readModel(const Inputs& in) {
  const auto* record = in.get("model").asRecord();
  const auto* features = in.get("features").asTensor();
  std::string why;
  require(record && record->type == kModelType && values::boundedJson(record->data, why) &&
              record->data.is_object() && record->data.size() == 9,
          "需要完整vision.ShapeTemplate.v1元数据", "model");
  const auto& d = record->data;
  require(d.value("method", Json()) == "oriented-chamfer-v1" && d.value("unit", Json()) == "px" &&
              d.contains("id") && d["id"].is_string() &&
              !d["id"].get_ref<const std::string&>().empty() &&
              d["id"].get_ref<const std::string&>().size() <= 256 && d.contains("frame") &&
              d["frame"].is_string() && !d["frame"].get_ref<const std::string&>().empty() &&
              d["frame"].get_ref<const std::string&>().size() <= 256 && d.contains("imageSize") &&
              poses::numericArray(d["imageSize"], 2, true) && d.contains("center") &&
              poses::numericArray(d["center"], 2) && d.contains("size") &&
              poses::numericArray(d["size"], 2, true) && d.contains("featureCount") &&
              d["featureCount"].is_number_integer() &&
              d["featureCount"].get<std::int64_t>() >= 12 &&
              d["featureCount"].get<std::int64_t>() <= 1024 && d.contains("featuresHash") &&
              d["featuresHash"].is_string(),
          "形状模板字段或方法非法", "model");
  require(features && features->consistent() && features->shape.size() == 2 &&
              features->shape[0] == d["featureCount"].get<std::int64_t>() &&
              features->shape[1] == 4 && hash(*features) == d["featuresHash"].get<std::string>(),
          "features须与模型绑定的Nx4张量一致", "features");
  Model result;
  result.id = d["id"].get<std::string>();
  result.size = {d["size"][0].get<double>(), d["size"][1].get<double>()};
  require(result.size.width <= 1e7 && result.size.height <= 1e7, "模型原图尺寸超过范围", "model");
  for (const auto& value : d["imageSize"]) {
    const double dimension = value.get<double>();
    require(dimension <= 4.6116860141324206e18 && std::floor(dimension) == dimension,
            "模型训练图像尺寸须为原生图像可表示的正整数", "model");
  }
  const double cx = d["center"][0].get<double>(), cy = d["center"][1].get<double>();
  require(cx >= 0 && cy >= 0 && cx < d["imageSize"][0].get<double>() &&
              cy < d["imageSize"][1].get<double>(),
          "模型中心不在训练图像内", "model");
  for (std::size_t i = 0; i < features->data.size(); i += 4) {
    Feature f{{features->data[i], features->data[i + 1]},
              {features->data[i + 2], features->data[i + 3]},
              1};
    require(std::isfinite(f.point.x) && std::isfinite(f.point.y) && std::isfinite(f.normal.x) &&
                std::isfinite(f.normal.y) && std::abs(f.normal.dot(f.normal) - 1) < 1e-5 &&
                std::abs(f.point.x) <= result.size.width / 2 &&
                std::abs(f.point.y) <= result.size.height / 2 && cx + f.point.x >= 0 &&
                cy + f.point.y >= 0 && cx + f.point.x < d["imageSize"][0].get<double>() &&
                cy + f.point.y < d["imageSize"][1].get<double>(),
            "形状特征坐标/单位法向非法", "features");
    result.features.push_back(f);
  }
  const auto geometryError =
      geometry(result.features, result.size, {cx, cy},
               {d["imageSize"][0].get<double>(), d["imageSize"][1].get<double>()});
  require(geometryError.empty(), geometryError, "model");
  return result;
}

std::vector<double> range(const ParamView& p, const char* loName, const char* hiName,
                          const char* stepName, double low, double high) {
  const double lo = number(p, loName, low, high), hi = number(p, hiName, low, high);
  const double step = number(p, stepName, 1e-5, high - low + 1);
  if (lo > hi || (hi - lo) / step > 720)
    throw Failure{img::badParam("搜索范围倒置或单轴超过721项", loName)};
  std::vector<double> result;
  const int n = static_cast<int>(std::floor((hi - lo) / step + 1e-9));
  for (int i = 0; i <= n; ++i)
    result.push_back(lo + i * step);
  return result;
}

struct Settings {
  std::string frame;
  double contrast, sigma, spatial, distance, cosine, coverage, score, rms, iou;
  bool polarity;
  int detections, candidates, iterations;
  std::vector<double> angles, scales;
  double minAngle, maxAngle, minScale, maxScale, angleStep, scaleStep;
};

Settings settings(const ParamView& p, int scale) {
  Settings s;
  s.frame = text(p, "frame");
  s.contrast = number(p, "minContrast", 1e-6, 1);
  s.sigma = number(p, "smoothSigma", 0, 8, scale);
  s.spatial = number(p, "spatialStep", .5, 16, scale);
  s.distance = number(p, "maxDistance", .25, 64, scale);
  s.rms = number(p, "maxRmse", .1, 64, scale);
  s.cosine = std::cos(number(p, "orientationTolerance", .1, 90) * poses::kPi / 180);
  s.coverage = number(p, "minCoverage", .05, 1);
  s.score = number(p, "minScore", 0, 1);
  s.iou = number(p, "nmsIou", 0, 1);
  s.polarity = p.flag("ignorePolarity");
  s.detections = integer(p, "maxDetections", 1, 256);
  s.candidates = integer(p, "maxCandidates", 1, 16384);
  s.iterations = integer(p, "refineIterations", 0, 16);
  s.angles = range(p, "angleMin", "angleMax", "angleStep", -180, 180);
  s.scales = range(p, "scaleMin", "scaleMax", "scaleStep", .1, 4);
  if (s.angles.size() * s.scales.size() > 721)
    throw Failure{img::badParam("角度/尺度搜索组合超过721项", "angleStep")};
  s.minAngle = number(p, "angleMin", -180, 180) * poses::kPi / 180;
  s.maxAngle = number(p, "angleMax", -180, 180) * poses::kPi / 180;
  s.angleStep = p.number("angleStep") * poses::kPi / 180;
  s.minScale = p.number("scaleMin");
  s.maxScale = p.number("scaleMax");
  s.scaleStep = p.number("scaleStep");
  return s;
}

struct Candidate {
  cv::Point2d center;
  double angle = 0, scale = 1, score = 0, coverage = 0, rms = 0;
  std::size_t support = 0;
};

bool better(const Candidate& a, const Candidate& b) {
  return std::make_tuple(-a.score, -a.coverage, a.rms, a.center.y, a.center.x, a.angle, a.scale) <
         std::make_tuple(-b.score, -b.coverage, b.rms, b.center.y, b.center.x, b.angle, b.scale);
}

cv::Point2d rotate(cv::Point2d point, double c, double s) {
  return {c * point.x - s * point.y, s * point.x + c * point.y};
}

struct SceneIndex {
  const EdgeMap& edge;
  int width, height;
  double cell;
  std::vector<std::int32_t> buckets, next;
  SceneIndex(const EdgeMap& input, int w, int h, double radius)
      : edge(input), width(std::max(1, static_cast<int>(std::ceil(w / radius)))),
        height(std::max(1, static_cast<int>(std::ceil(h / radius)))), cell(radius) {
    if (std::uint64_t(width) * height > kPixels)
      limit("边缘空间索引超过8Mi桶预算");
    buckets.resize(static_cast<std::size_t>(width) * height, -1);
    next.resize(edge.points.size(), -1);
    for (std::size_t i = 0; i < edge.points.size(); ++i) {
      const auto& point = edge.points[i].point;
      const auto bucket = static_cast<std::size_t>(point.y / cell) * width +
                          static_cast<std::size_t>(point.x / cell);
      next[i] = buckets[bucket];
      buckets[bucket] = static_cast<std::int32_t>(i);
    }
  }
};

Candidate evaluate(Candidate pose, const Model& model, const SceneIndex& scene, const Settings& s,
                   int scale, Budget& budget) {
  const double c = std::cos(pose.angle), sn = std::sin(pose.angle);
  double squared = 0, score = 0;
  std::size_t count = 0;
  for (const auto& feature : model.features) {
    budget.add(1);
    const auto point = pose.center + rotate(feature.point, c, sn) * (pose.scale / scale);
    const auto normal = rotate(feature.normal, c, sn);
    if (point.x < 0 || point.y < 0 || point.x >= scene.edge.image.cols ||
        point.y >= scene.edge.image.rows)
      continue;
    const int bx = static_cast<int>(point.x / scene.cell),
              by = static_cast<int>(point.y / scene.cell);
    double best = s.distance * s.distance + 1, bestNormal = 0;
    for (int y = std::max(0, by - 1); y <= std::min(scene.height - 1, by + 1); ++y)
      for (int x = std::max(0, bx - 1); x <= std::min(scene.width - 1, bx + 1); ++x) {
        for (auto i = scene.buckets[static_cast<std::size_t>(y) * scene.width + x]; i >= 0;
             i = scene.next[i]) {
          budget.add(1);
          const auto& candidate = scene.edge.points[i];
          const double cosine = std::clamp(s.polarity ? std::abs(normal.dot(candidate.normal))
                                                      : normal.dot(candidate.normal),
                                           -1.0, 1.0);
          if (cosine < s.cosine)
            continue;
          const auto delta = candidate.point - point;
          const double distance = delta.dot(delta);
          if (distance < best || (distance == best && cosine > bestNormal)) {
            best = distance;
            bestNormal = cosine;
          }
        }
      }
    if (best > s.distance * s.distance)
      continue;
    ++count;
    squared += best;
    score += bestNormal * std::max(0.0, 1 - std::sqrt(best) / s.distance);
  }
  pose.support = count;
  pose.coverage = static_cast<double>(count) / model.features.size();
  pose.score = std::clamp(score / model.features.size(), 0.0, 1.0);
  pose.rms = count ? std::sqrt(squared / count) : s.distance;
  return pose;
}

bool accepted(const Candidate& c, const Settings& s) {
  return c.coverage >= s.coverage && c.score >= s.score && c.rms <= s.rms;
}

bool centerAllowed(cv::Point2d point, const cv::Mat& mask) {
  if (!std::isfinite(point.x) || !std::isfinite(point.y) || point.x < 0 || point.y < 0 ||
      point.x > mask.cols - 1 || point.y > mask.rows - 1)
    return false;
  const int x0 = static_cast<int>(std::floor(point.x)), x1 = static_cast<int>(std::ceil(point.x)),
            y0 = static_cast<int>(std::floor(point.y)), y1 = static_cast<int>(std::ceil(point.y));
  return mask.at<std::uint8_t>(y0, x0) && mask.at<std::uint8_t>(y0, x1) &&
         mask.at<std::uint8_t>(y1, x0) && mask.at<std::uint8_t>(y1, x1);
}

Candidate refine(Candidate current, const Model& model, const SceneIndex& scene, const Settings& s,
                 int scale, const cv::Mat& mask, Budget& budget) {
  double translationStep = s.spatial / 2, angleStep = s.angleStep / 2, sizeStep = s.scaleStep / 2;
  for (int iteration = 0; iteration < s.iterations; ++iteration) {
    check(budget.ctx);
    Candidate best = current;
    for (int axis = 0; axis < 4; ++axis)
      for (int direction : {-1, 1}) {
        Candidate trial = current;
        if (axis == 0)
          trial.center.x += translationStep * direction;
        else if (axis == 1)
          trial.center.y += translationStep * direction;
        else if (axis == 2)
          trial.angle += angleStep * direction;
        else
          trial.scale += sizeStep * direction;
        if (!centerAllowed(trial.center, mask) || trial.angle < s.minAngle ||
            trial.angle > s.maxAngle || trial.scale < s.minScale || trial.scale > s.maxScale)
          continue;
        trial = evaluate(trial, model, scene, s, scale, budget);
        if (better(trial, best))
          best = trial;
      }
    if (better(best, current))
      current = best;
    else {
      translationStep /= 2;
      angleStep /= 2;
      sizeStep /= 2;
    }
  }
  return current;
}

cv::RotatedRect rectangle(const Candidate& c, const Model& model, int scale) {
  return {cv::Point2f(c.center),
          cv::Size2f(static_cast<float>(model.size.width * c.scale / scale),
                     static_cast<float>(model.size.height * c.scale / scale)),
          static_cast<float>(c.angle * 180 / poses::kPi)};
}

double overlap(const Candidate& a, const Candidate& b, const Model& model, int scale) {
  std::vector<cv::Point2f> points;
  const auto r1 = rectangle(a, model, scale), r2 = rectangle(b, model, scale);
  const int intersection = cv::rotatedRectangleIntersection(r1, r2, points);
  if (intersection == cv::INTERSECT_NONE || points.empty())
    return 0;
  const double common = std::abs(cv::contourArea(points));
  const double total = static_cast<double>(r1.size.area()) + r2.size.area() - common;
  return total > 0 ? std::clamp(common / total, 0.0, 1.0) : 0;
}

std::vector<Candidate> search(const Model& model, const EdgeMap& edge, const cv::Mat& mask,
                              const Settings& s, int scale, Budget& budget, std::size_t& verified,
                              std::size_t& peaks) {
  if (edge.points.empty())
    return {};
  SceneIndex scene(edge, edge.image.cols, edge.image.rows, std::max(1.0, s.distance));
  const auto anchors =
      spread(model.features, std::min<std::size_t>(16, model.features.size()), 0, budget);
  const double step = std::max(.5, s.spatial);
  const int width = static_cast<int>(std::ceil(edge.image.cols / step));
  const int height = static_cast<int>(std::ceil(edge.image.rows / step));
  if (std::uint64_t(width) * height > kPixels)
    limit("中心投票网格超过8Mi格预算");
  std::vector<std::uint16_t> votes(static_cast<std::size_t>(width) * height);
  std::vector<Candidate> result;
  const int minVotes = std::max(2, static_cast<int>(std::ceil(anchors.size() * s.coverage * .3)));
  for (const double size : s.scales)
    for (const double degrees : s.angles) {
      budget.add(anchors.size() * edge.points.size() + votes.size());
      std::fill(votes.begin(), votes.end(), 0);
      const double angle = degrees * poses::kPi / 180;
      const double c = std::cos(angle), sn = std::sin(angle);
      for (const auto anchor : anchors) {
        const auto point = rotate(model.features[anchor].point, c, sn) * (size / scale);
        const auto normal = rotate(model.features[anchor].normal, c, sn);
        for (const auto& target : edge.points) {
          const double cosine = std::clamp(s.polarity ? std::abs(normal.dot(target.normal))
                                                      : normal.dot(target.normal),
                                           -1.0, 1.0);
          if (cosine < s.cosine)
            continue;
          const auto center = target.point - point;
          if (center.x < 0 || center.y < 0 || center.x >= edge.image.cols ||
              center.y >= edge.image.rows)
            continue;
          const int x = std::min(width - 1, static_cast<int>(std::lround(center.x / step)));
          const int y = std::min(height - 1, static_cast<int>(std::lround(center.y / step)));
          auto& value = votes[static_cast<std::size_t>(y) * width + x];
          if (value != 65535)
            ++value;
        }
      }
      for (int y = 0; y < height; ++y) {
        check(budget.ctx);
        for (int x = 0; x < width; ++x) {
          const auto index = static_cast<std::size_t>(y) * width + x;
          if (votes[index] < minVotes)
            continue;
          bool maximum = true;
          for (int j = std::max(0, y - 1); j <= std::min(height - 1, y + 1); ++j)
            for (int i = std::max(0, x - 1); i <= std::min(width - 1, x + 1); ++i) {
              const auto neighbor = static_cast<std::size_t>(j) * width + i;
              if (votes[neighbor] > votes[index] ||
                  (votes[neighbor] == votes[index] && neighbor < index))
                maximum = false;
            }
          if (!maximum)
            continue;
          const cv::Point2d center(std::min(edge.image.cols - 1.0, x * step),
                                   std::min(edge.image.rows - 1.0, y * step));
          if (!centerAllowed(center, mask))
            continue;
          if (++peaks > static_cast<std::size_t>(s.candidates))
            limit("形状中心峰值超过maxCandidates；请缩小ROI或搜索范围");
          Candidate candidate = evaluate({center, angle, size}, model, scene, s, scale, budget);
          ++verified;
          if (candidate.coverage < s.coverage * .7 || candidate.score < s.score * .5)
            continue;
          candidate = refine(candidate, model, scene, s, scale, mask, budget);
          if (centerAllowed(candidate.center, mask) && accepted(candidate, s))
            result.push_back(candidate);
        }
      }
    }
  std::sort(result.begin(), result.end(), better);
  std::vector<Candidate> keep;
  for (const auto& candidate : result) {
    bool repeated = false;
    for (const auto& old : keep) {
      budget.add(1);
      if (overlap(candidate, old, model, scale) > s.iou) {
        repeated = true;
        break;
      }
    }
    if (!repeated)
      keep.push_back(candidate);
  }
  return keep;
}

Status match(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  try {
    check(ctx);
    const auto* source = in.get("image").asImage();
    const auto gray = normalized(source, "image");
    const auto model = readModel(in);
    const auto s = settings(p, source->scale);
    const auto mask = selection(in, *source, s.frame, ctx);
    const auto edges = extract(gray, mask, s.sigma, s.contrast, ctx);
    Budget budget{ctx};
    std::size_t verified = 0, peaks = 0;
    auto matches = search(model, edges, mask, s, source->scale, budget, verified, peaks);
    const auto beforeLimit = matches.size();
    if (matches.size() > static_cast<std::size_t>(s.detections))
      matches.resize(s.detections);
    cv::Mat overlay = cvx::view(*source).clone();
    Json instances = Json::array(), items = Json::array();
    const double full = source->depth == PixelDepth::U8    ? 255
                        : source->depth == PixelDepth::U16 ? 65535
                                                           : 1;
    for (std::size_t i = 0; i < matches.size(); ++i) {
      const auto& candidate = matches[i];
      const auto id = model.id + "-" + std::to_string(i);
      auto pose = poses::make2D(
          id, s.frame, "px",
          {candidate.center.x * source->scale, candidate.center.y * source->scale}, candidate.angle,
          {model.size.width * candidate.scale, model.size.height * candidate.scale});
      pose.data["confidence"] = candidate.score;
      pose.data["pickable"] = false;
      instances.push_back(std::move(pose.data));
      items.push_back({{"id", id},
                       {"modelId", model.id},
                       {"scale", candidate.scale},
                       {"score", candidate.score},
                       {"coverage", candidate.coverage},
                       {"rmse", candidate.rms * source->scale},
                       {"support", candidate.support},
                       {"featureCount", model.features.size()}});
      cv::Point2f corners[4];
      rectangle(candidate, model, source->scale).points(corners);
      const auto color = source->channels == 1 ? cv::Scalar(full) : cv::Scalar(0, full, 0, full);
      for (int j = 0; j < 4; ++j)
        cv::line(overlay, corners[j], corners[(j + 1) % 4], color, 1, cv::LINE_8);
      const double c = std::cos(candidate.angle), sn = std::sin(candidate.angle);
      for (const auto& feature : model.features) {
        const auto point =
            candidate.center + rotate(feature.point, c, sn) * (candidate.scale / source->scale);
        if (point.x >= 0 && point.y >= 0 && point.x < source->width && point.y < source->height)
          cv::circle(overlay, point, 1, color, -1);
      }
    }
    Record posesRecord = poses::make2DList(std::move(instances), s.frame, "px");
    Record quality{kQualityType,
                   {{"method", "oriented-chamfer-v1"},
                    {"frame", s.frame},
                    {"unit", "px"},
                    {"items", std::move(items)},
                    {"sceneEdges", edges.points.size()},
                    {"variants", s.angles.size() * s.scales.size()},
                    {"centerPeaks", peaks},
                    {"verified", verified},
                    {"work", budget.work},
                    {"acceptedBeforeLimit", beforeLimit},
                    {"limited", beforeLimit > matches.size()}}};
    bounded(posesRecord);
    bounded(quality);
    preserveAlpha(*source, overlay);
    auto drawn = imageOutput(std::move(overlay), source->scale);
    auto edgeImage = imageOutput(edges.image, source->scale);
    check(ctx);
    out.set("instances", Data::record(std::move(posesRecord)));
    out.set("quality", Data::record(std::move(quality)));
    out.set("image", Data::image(std::move(drawn)));
    out.set("edges", Data::image(std::move(edgeImage)));
    return Status::Ok();
  } catch (const Failure& e) {
    return e.status;
  } catch (const cv::Exception& e) {
    return img::fromCvError(e);
  } catch (const std::bad_alloc&) {
    return Status::Error(Phase::Execute, "resource_limit", "形状匹配内存不足");
  }
}

Param pixel(const char* name, const char* label, double value, const char* doc) {
  auto result = img::floatParam(name, label, value, doc);
  result.unit = "px";
  return result;
}

std::vector<Port> optionalSelection() {
  auto mask = img::grayIn("mask", "同尺寸/比例u8非零有效掩膜；不制造掩膜边缘。");
  mask.required = false;
  return {mask, Port{"roi", "Box2D", "矩形搜索区域", "像素，和region互斥。", false},
          image_p1::recordPort("region", poses::kPose2DType, false)};
}

} // namespace

void registerP3ShapeMatching(Registry& registry) {
  auto op = image_p1::descriptor("image.shape_template", "梯度形状建模",
                                 "shape based contour gradient model template train");
  op.inputs = {img::imageIn()};
  const auto optional = optionalSelection();
  op.inputs.insert(op.inputs.end(), optional.begin(), optional.end());
  op.outputs = {
      Port{"features", "Tensor", "形状梯度特征", "Nx4 float32 [dx,dy,nx,ny]，原图像素偏移。", true},
      image_p1::recordPort("model", kModelType),
      img::imageOut("image", "保原位深/通道/alpha的采样特征叠图。")};
  op.params = {
      image_p1::text("id", "模型标识", "shape"),
      image_p1::text("frame", "图像参考帧", "image"),
      img::floatParam("minContrast", "最小梯度对比度", .04, "归一化(0,1]，不使用灰度相关。"),
      pixel("smoothSigma", "平滑标准差", .8, "[0,8]原图像素；ROI内归一化平滑。"),
      img::intParam("maxFeatures", "模板特征数上限", 256, 12, 1024,
                    "确定性最远点采样，完整模型由Tensor承载。"),
      pixel("featureSpacing", "特征最小间距", 2, "[0.25,64]原图像素。")};
  op.doc =
      "从图像或预先渲染的CAD投影建立梯度形状模型。Tensor储存原图像素特征，Record仅储存有界元数据；重新加载图像可确定性重建。";
  op.compute = train;
  registry.addOperator(std::move(op));
  op = image_p1::descriptor(
      "image.shape_match", "梯度形状匹配",
      "shape based gradient contour oriented chamfer angle scale occlusion matching");
  op.inputs = {img::imageIn(),
               Port{"features", "Tensor", "模型特征", "与model绑定的Nx4原像素特征。", true},
               image_p1::recordPort("model", kModelType)};
  op.inputs.insert(op.inputs.end(), optional.begin(), optional.end());
  op.outputs = {image_p1::recordPort("instances", poses::kPoses2DType),
                image_p1::recordPort("quality", kQualityType),
                img::imageOut("image", "旋转框与形状特征叠图，保留输入位深/通道/alpha。"),
                img::imageOut("edges", "u8有效搜索边缘。")};
  op.params = {
      image_p1::text("frame", "图像参考帧", "image"),
      img::floatParam("minContrast", "最小梯度对比度", .04, "归一化(0,1]。"),
      pixel("smoothSigma", "平滑标准差", .8, "[0,8]原图像素。"),
      img::floatParam("angleMin", "最小旋转角(°)", -180, "[-180,180]，图像y向下的正向旋转。"),
      img::floatParam("angleMax", "最大旋转角(°)", 180, "[-180,180]。"),
      img::floatParam("angleStep", "旋转搜索步长(°)", 5, "大于0；角度/尺度组合最多721项。"),
      img::floatParam("scaleMin", "最小尺寸比例", 1, "[0.1,4]。"),
      img::floatParam("scaleMax", "最大尺寸比例", 1, "[0.1,4]。"),
      img::floatParam("scaleStep", "尺寸搜索步长", .1, "大于0。"),
      pixel("spatialStep", "中心投票步长", 2, "[0.5,16]原图像素；工作网格最小0.5像素。"),
      pixel("maxDistance", "特征最大残差", 2, "[0.25,64]原图像素，超出者计遮挡/不匹配。"),
      img::floatParam("orientationTolerance", "法向容差(°)", 30, "[0.1,90]。"),
      img::boolParam("ignorePolarity", "忽略对比度极性", true, "允许明暗翻转，仍校验边缘方向。"),
      img::floatParam("minCoverage", "最小可见覆盖", .6, "[0.05,1]，匹配特征数/全部模板特征数。"),
      img::floatParam("minScore", "最小形状分数", .45,
                      "[0,1]；全部特征上的法向相似×线性距离权平均，不是概率。"),
      pixel("maxRmse", "最大支持RMS", 1.5, "[0.1,64]原图像素。"),
      img::intParam("maxDetections", "返回实例上限", 32, 1, 256,
                    "NMS后按分数排序，截断会在quality明示。"),
      img::floatParam("nmsIou", "旋转框NMS交并比", .3, "[0,1]。"),
      img::intParam("maxCandidates", "中心峰值预算", 4096, 1, 16384,
                    "超过时resource_limit，不静默丢候选。"),
      img::intParam("refineIterations", "局部姿态精化次数", 8, 0, 16,
                    "对x/y/角度/尺度做有界全特征残差搜索。")};
  op.doc =
      "梯度方向中心投票、完整特征定向Chamfer质量门控、遮挡覆盖率、局部姿态精化和旋转IoU NMS；输出原图像素Pose2D与原生叠图。不是NCC。输入须先按需要去畸变。";
  op.compute = match;
  registry.addOperator(std::move(op));
}
} // namespace lyflow::ops
