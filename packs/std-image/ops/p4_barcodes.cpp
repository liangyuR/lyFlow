#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <tuple>
#include <vector>

#include "../barcode/backend.h"
#include "lyflow/pose.h"
#include "p1_common.h"

namespace lyflow::ops {
namespace {
using Json = nlohmann::json;
using Quad = std::array<std::array<double, 2>, 4>;
constexpr std::size_t kPixels = 8 * 1024 * 1024;
constexpr const char* kDetections = "image.Barcodes.v1";
struct Failure {
  Status status;
};
void require(bool value, const std::string& message, const char* port = "image") {
  if (!value)
    throw Failure{img::badInput(message, port)};
}
void budget(bool value, const char* message) {
  if (!value)
    throw Failure{Status::Error(Phase::Execute, "resource_limit", message)};
}
void check(ExecContext& ctx) {
  if (ctx.cancelled())
    throw Failure{Status::Error(Phase::Execute, "cancelled", "条码处理已取消")};
}
int integer(const ParamView& p, const char* name, int low, int high) {
  const auto value = p.integer(name);
  if (value < low || value > high)
    throw Failure{img::badParam("整数参数超出范围", name)};
  return static_cast<int>(value);
}
std::string frame(const ParamView& p, const char* name = "frame") {
  const auto value = p.text(name);
  if (value.empty() || value.size() > 256)
    throw Failure{img::badParam("参考帧须为1..256字节", name)};
  return value;
}
cv::Mat gray8(const Image* image, ExecContext& ctx) {
  require(image && image->consistent() && image->scale >= 1, "输入必须为有效图像");
  budget(image->pixelCount() <= kPixels && image->byteSize() <= 128 * 1024 * 1024,
         "输入超过8Mi像素或128MiB预算");
  budget(image->width <= 65535 && image->height <= 65535, "ZXing输入宽高不得超过65535");
  budget(2 * image->byteSize() + 24 * image->pixelCount() + 64 * 1024 * 1024 <= 512 * 1024 * 1024,
         "条码处理预计峰值超过512MiB");
  check(ctx);
  if (image->depth == PixelDepth::F32) {
    const auto source = cvx::view(*image);
    for (int y = 0; y < source.rows; ++y) {
      check(ctx);
      const float* row = source.ptr<float>(y);
      for (int i = 0; i < source.cols * source.channels(); ++i)
        require(std::isfinite(row[i]) && row[i] >= 0 && row[i] <= 1, "f32各通道必须为有限0..1");
    }
  }
  cv::Mat gray = image_p1::gray(*image), result;
  gray.convertTo(result, CV_8U,
                 image->depth == PixelDepth::U8    ? 1
                 : image->depth == PixelDepth::U16 ? 255.0 / 65535
                                                   : 255);
  return result;
}
cv::Rect box(const Data& input, const Image& image) {
  const auto* roi = input.asBox2D();
  require(roi && roi->unit == Unit2D::Pixel, "roi必须是像素Box2D", "roi");
  for (int axis = 0; axis < 2; ++axis)
    require(std::isfinite(roi->min[axis]) && std::isfinite(roi->max[axis]) &&
                roi->max[axis] > roi->min[axis],
            "roi必须有限且面积为正", "roi");
  const double x0 = std::max(0.0, std::ceil(static_cast<double>(roi->min[0])));
  const double y0 = std::max(0.0, std::ceil(static_cast<double>(roi->min[1])));
  const double x1 =
      std::min(static_cast<double>(image.width - 1), std::floor(static_cast<double>(roi->max[0])));
  const double y1 =
      std::min(static_cast<double>(image.height - 1), std::floor(static_cast<double>(roi->max[1])));
  require(x1 >= x0 && y1 >= y0, "roi与图像没有像素交集", "roi");
  return {static_cast<int>(x0), static_cast<int>(y0), static_cast<int>(x1 - x0 + 1),
          static_cast<int>(y1 - y0 + 1)};
}
cv::Mat selection(const Inputs& in, const Image& image, const std::string& frame,
                  ExecContext& ctx) {
  require(!(in.has("roi") && in.has("region")), "roi与region互斥", "region");
  cv::Mat result(image.height, image.width, CV_8U, cv::Scalar(255));
  if (in.has("roi")) {
    const auto roi = box(in.get("roi"), image);
    result.setTo(0);
    result(roi).setTo(255);
  }
  if (in.has("region")) {
    const auto* region = in.get("region").asRecord();
    std::string why;
    require(region && region->type == poses::kPose2DType && poses::validate(*region, why),
            "region必须是有效Pose2D：" + why, "region");
    const auto& d = region->data;
    require(d.at("frame") == frame && d.at("unit") == "px", "region帧或单位不匹配", "region");
    const double cx = d.at("position")[0].get<double>() / image.scale;
    const double cy = d.at("position")[1].get<double>() / image.scale;
    const double width = d.at("size")[0].get<double>() / image.scale;
    const double height = d.at("size")[1].get<double>() / image.scale;
    const double normalizer = std::max(width, height);
    require(width > 0 && height > 0 && std::isfinite(normalizer), "region尺寸无效", "region");
    const double c = std::cos(d.at("angle").get<double>());
    const double s = std::sin(d.at("angle").get<double>());
    std::size_t count = 0;
    for (int y = 0; y < image.height; ++y) {
      check(ctx);
      auto* row = result.ptr<std::uint8_t>(y);
      for (int x = 0; x < image.width; ++x) {
        const double dx = x / normalizer - cx / normalizer;
        const double dy = y / normalizer - cy / normalizer;
        row[x] = std::abs(c * dx + s * dy) <= width / normalizer / 2 &&
                         std::abs(-s * dx + c * dy) <= height / normalizer / 2
                     ? 255
                     : 0;
        count += row[x] != 0;
      }
    }
    require(count != 0, "region与图像没有像素交集", "region");
  }
  return result;
}
std::string hex(const std::vector<std::uint8_t>& bytes) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(bytes.size() * 2);
  for (const auto byte : bytes) {
    result += digits[byte >> 4];
    result += digits[byte & 15];
  }
  return result;
}
Json content(const barcode::Decoded& code) {
  budget(code.bytes.size() <= 8192 && code.text.size() <= 32768 && code.error.size() <= 4096,
         "条码内容超过每码8KiB原始字节或文本/错误预算");
  return {
      {"valid", code.valid},         {"format", code.format},           {"text", code.text},
      {"bytesHex", hex(code.bytes)}, {"contentType", code.contentType}, {"hasECI", code.hasECI},
      {"error", code.error},         {"inverted", code.inverted},       {"mirrored", code.mirrored},
      {"lineCount", code.lineCount}};
}
std::vector<barcode::Decoded> decode(const cv::Mat& image, const barcode::Options& options,
                                     int limit, ExecContext& ctx) {
  check(ctx);
  auto result = barcode::read(image.ptr<std::uint8_t>(), image.cols, image.rows,
                              static_cast<int>(image.step), options);
  check(ctx);
  budget(result.size() <= static_cast<std::size_t>(limit), "检测数量超过maxSymbols预算");
  for (const auto& item : result)
    content(item);
  return result;
}
void bounded(const Record& record) {
  std::string why;
  if (!values::boundedJson(record.data, why, true))
    throw Failure{Status::Error(Phase::Execute, "resource_limit", why)};
}
Image native(cv::Mat matrix, int scale) {
  Image result;
  require(cvx::fromMat(std::move(matrix), result), "无法创建原生输出图像");
  result.scale = scale;
  return result;
}
Status decodeOperator(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  try {
    const auto* input = in.get("image").asImage();
    auto gray = gray8(input, ctx);
    const auto reference = frame(p);
    const auto allowed = selection(in, *input, reference, ctx);
    gray.setTo(255, allowed == 0);
    const int limit = integer(p, "maxSymbols", 1, 128);
    barcode::Options options;
    options.formats = p.choice("formats");
    options.tryHarder = p.flag("tryHarder");
    options.tryRotate = p.flag("tryRotate");
    options.tryInvert = p.flag("tryInvert");
    options.tryDownscale = p.flag("tryDownscale");
    options.limit = limit + 1;
    auto codes = decode(gray, options, limit, ctx);
    codes.erase(std::remove_if(codes.begin(), codes.end(),
                               [&](const auto& code) {
                                 for (const auto& point : code.corners) {
                                   const int x = static_cast<int>(std::lround(point[0]));
                                   const int y = static_cast<int>(std::lround(point[1]));
                                   if (x < 0 || y < 0 || x >= allowed.cols || y >= allowed.rows ||
                                       !allowed.at<std::uint8_t>(y, x))
                                     return true;
                                 }
                                 return false;
                               }),
                codes.end());
    std::sort(codes.begin(), codes.end(), [](const auto& a, const auto& b) {
      return std::tie(a.corners[0][1], a.corners[0][0], a.format, a.bytes) <
             std::tie(b.corners[0][1], b.corners[0][0], b.format, b.bytes);
    });
    cv::Mat overlay = cvx::view(*input).clone();
    const double maximum = cvx::fullScale(overlay.depth());
    Json poseItems = Json::array(), items = Json::array();
    for (std::size_t index = 0; index < codes.size(); ++index) {
      check(ctx);
      const auto& code = codes[index];
      const std::string id = "barcode-" + std::to_string(index);
      Json corners = Json::array();
      std::vector<cv::Point> polygon;
      std::vector<cv::Point2f> rectangle;
      for (const auto& point : code.corners) {
        corners.push_back({point[0] * input->scale, point[1] * input->scale});
        polygon.emplace_back(static_cast<int>(std::lround(point[0])),
                             static_cast<int>(std::lround(point[1])));
        rectangle.emplace_back(static_cast<float>(point[0]), static_cast<float>(point[1]));
      }
      auto item = content(code);
      item["id"] = id;
      item["corners"] = corners;
      item["orientationDegrees"] = code.orientation;
      items.push_back(std::move(item));
      if (code.valid) {
        const auto rect = cv::minAreaRect(rectangle);
        auto pose = poses::make2D(id, reference, "px",
                                  {rect.center.x * input->scale, rect.center.y * input->scale},
                                  rect.angle * poses::kPi / 180,
                                  {std::max(1.0f, rect.size.width) * input->scale,
                                   std::max(1.0f, rect.size.height) * input->scale});
        pose.data["pickable"] = false;
        pose.data["classId"] = code.format;
        pose.data["attributes"] = {{"decoded", true}, {"positionSource", "ZXing quadrilateral"}};
        poseItems.push_back(std::move(pose.data));
      }
      const cv::Scalar color = input->channels == 1 ? cv::Scalar(maximum * .6)
                               : code.valid
                                   ? cv::Scalar(maximum * .1, maximum * .9, maximum * .2, 0)
                                   : cv::Scalar(maximum, maximum * .3, maximum * .1, 0);
      cv::polylines(overlay, polygon, true, color, 1, cv::LINE_8);
    }
    if (input->channels == 4) {
      cv::Mat alpha;
      cv::extractChannel(cvx::view(*input), alpha, 3);
      cv::insertChannel(alpha, overlay, 3);
    }
    auto instances = poses::make2DList(std::move(poseItems), reference, "px");
    Record detections{kDetections,
                      {{"frame", reference},
                       {"unit", "px"},
                       {"imageSize",
                        {static_cast<double>(input->width) * input->scale,
                         static_cast<double>(input->height) * input->scale}},
                       {"backend", "ZXing-C++ 3.1.1"},
                       {"backendPatchRevision", 1},
                       {"textEncoding", "UTF-8 via ECI or ZXing charset inference"},
                       {"items", std::move(items)}}};
    bounded(instances);
    bounded(detections);
    auto image = native(std::move(overlay), input->scale);
    check(ctx);
    out.set("instances", Data::record(std::move(instances)));
    out.set("detections", Data::record(std::move(detections)));
    out.set("image", Data::image(std::move(image)));
    return Status::Ok();
  } catch (const Failure& error) {
    return error.status;
  } catch (const std::bad_alloc&) {
    return Status::Error(Phase::Execute, "resource_limit", "条码内存分配失败");
  } catch (const std::exception& error) {
    return img::badInput(std::string("条码解码失败：") + error.what());
  }
}

Quad sourceQuad(const Inputs& in, const Image& image, const std::string& reference,
                const std::string& symbolId) {
  require(!(in.has("roi") && in.has("detections")), "roi与detections互斥", "detections");
  if (!in.has("detections")) {
    require(symbolId.empty(), "未提供detections时不得指定symbolId", "detections");
    const auto roi =
        in.has("roi") ? box(in.get("roi"), image) : cv::Rect(0, 0, image.width, image.height);
    return {
        {{static_cast<double>(roi.x), static_cast<double>(roi.y)},
         {static_cast<double>(roi.x + roi.width - 1), static_cast<double>(roi.y)},
         {static_cast<double>(roi.x + roi.width - 1), static_cast<double>(roi.y + roi.height - 1)},
         {static_cast<double>(roi.x), static_cast<double>(roi.y + roi.height - 1)}}};
  }
  const auto* record = in.get("detections").asRecord();
  require(record && record->type == kDetections, "需要image.Barcodes.v1检测记录", "detections");
  bounded(*record);
  const auto& d = record->data;
  require(d.is_object() && d.contains("frame") && d.at("frame") == reference &&
              d.contains("unit") && d.at("unit") == "px" && d.contains("imageSize") &&
              d.at("imageSize") == Json::array({static_cast<double>(image.width) * image.scale,
                                                static_cast<double>(image.height) * image.scale}) &&
              d.contains("items") && d.at("items").is_array() && d.at("items").size() <= 128,
          "检测记录帧、分辨率或集合不匹配", "detections");
  const Json* selected = nullptr;
  for (const auto& item : d.at("items")) {
    require(item.is_object() && item.contains("id") && item.at("id").is_string() &&
                item.contains("format") && item.at("format").is_string() &&
                item.contains("valid") && item.at("valid").is_boolean(),
            "检测项schema无效", "detections");
    if ((!symbolId.empty() && item.at("id") != symbolId) || !item.at("valid").get<bool>())
      continue;
    const auto format = item.at("format").get<std::string>();
    if (format != "QRCode" && format != "QRCodeModel2")
      continue;
    require(!selected, "多个标准QR候选需要显式symbolId", "detections");
    selected = &item;
  }
  require(selected && selected->contains("corners") && selected->at("corners").is_array() &&
              selected->at("corners").size() == 4,
          "未找到指定的已解码标准QR四角", "detections");
  Quad result{};
  for (int i = 0; i < 4; ++i) {
    require(poses::numericArray(selected->at("corners")[i], 2), "QR四角必须有限", "detections");
    for (int axis = 0; axis < 2; ++axis) {
      result[i][axis] = selected->at("corners")[i][axis].get<double>() / image.scale;
      require(result[i][axis] >= 0 && result[i][axis] <= (axis ? image.height : image.width) - 1,
              "QR四角超出当前图像", "detections");
    }
  }
  return result;
}
Json feedbackCodes(const std::vector<barcode::Decoded>& codes) {
  Json items = Json::array();
  for (const auto& code : codes)
    items.push_back(content(code));
  return items;
}
Status refineOperator(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  try {
    const auto* input = in.get("image").asImage();
    const auto gray = gray8(input, ctx);
    const auto reference = frame(p), outputFrame = frame(p, "outputFrame");
    const int modules = integer(p, "moduleCount", 21, 177);
    if ((modules - 21) % 4 != 0)
      throw Failure{img::badParam("标准QR格数须为21+4k，k=0..39", "moduleCount")};
    const int pixels = integer(p, "modulePixels", 1, 32);
    const int quiet = integer(p, "quietZone", 4, 16);
    const int radius = integer(p, "localRadius", 1, 32);
    const double contrast = p.number("minCellContrast");
    if (!std::isfinite(contrast) || contrast < 0 || contrast > 1)
      throw Failure{img::badParam("格子对比阈值须为0..1", "minCellContrast")};
    const auto polarity = p.choice("polarity"), threshold = p.choice("thresholdMode");
    if ((polarity != "dark" && polarity != "light") ||
        (threshold != "localRange" && threshold != "otsu"))
      throw Failure{img::badParam("极性或阈值模式无效", "thresholdMode")};
    const int side = (modules + 2 * quiet) * pixels, codeSide = modules * pixels;
    budget(static_cast<std::size_t>(side) * side <= kPixels, "增强输出超过8Mi像素预算");
    const auto quad = sourceQuad(in, *input, reference, p.text("symbolId"));
    std::vector<cv::Point2f> source;
    for (const auto& point : quad)
      source.emplace_back(static_cast<float>(point[0]), static_cast<float>(point[1]));
    require(cv::isContourConvex(source) && std::abs(cv::contourArea(source)) > 1,
            "QR四角必须按TL/TR/BR/BL组成非退化凸四边形", "detections");
    for (int i = 0; i < 4; ++i)
      require(cv::norm(source[i] - source[(i + 1) % 4]) + 1 >= modules,
              "源QR每边采样不足每模块一个工作像素", "image");
    const std::vector<cv::Point2f> target{
        {0, 0},
        {static_cast<float>(codeSide - 1), 0},
        {static_cast<float>(codeSide - 1), static_cast<float>(codeSide - 1)},
        {0, static_cast<float>(codeSide - 1)}};
    const auto transform = cv::getPerspectiveTransform(source, target);
    require(cv::checkRange(transform) && std::abs(cv::determinant(transform)) > 1e-12,
            "QR裁正变换退化", "detections");
    cv::Mat rectified;
    cv::warpPerspective(gray, rectified, transform, {codeSide, codeSide}, cv::INTER_LINEAR,
                        cv::BORDER_CONSTANT, cv::Scalar(polarity == "dark" ? 255 : 0));
    cv::Mat before;
    cv::copyMakeBorder(rectified, before, quiet * pixels, quiet * pixels, quiet * pixels,
                       quiet * pixels, cv::BORDER_CONSTANT,
                       cv::Scalar(polarity == "dark" ? 255 : 0));
    barcode::Options options;
    options.formats = "QRCodeModel2";
    const auto beforeCodes = decode(before, options, 128, ctx);
    cv::Mat means;
    cv::resize(rectified, means, {modules, modules}, 0, 0, cv::INTER_AREA);
    if (polarity == "light")
      cv::bitwise_not(means, means);
    cv::Mat global;
    const double globalThreshold =
        cv::threshold(means, global, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);
    cv::Mat low, high;
    const auto kernel = cv::getStructuringElement(cv::MORPH_RECT, {2 * radius + 1, 2 * radius + 1});
    cv::erode(means, low, kernel, {-1, -1});
    cv::dilate(means, high, kernel, {-1, -1});
    cv::Mat cells(modules, modules, CV_8U);
    std::size_t uncertain = 0;
    double minimumContrast = 1;
    for (int y = 0; y < modules; ++y) {
      check(ctx);
      for (int x = 0; x < modules; ++x) {
        const double localContrast =
            (high.at<std::uint8_t>(y, x) - low.at<std::uint8_t>(y, x)) / 255.0;
        uncertain += localContrast < contrast;
        minimumContrast = std::min(minimumContrast, localContrast);
        const double cut =
            threshold == "localRange" && localContrast >= contrast && localContrast > 0
                ? (low.at<std::uint8_t>(y, x) + high.at<std::uint8_t>(y, x)) / 2.0
                : globalThreshold;
        cells.at<std::uint8_t>(y, x) = means.at<std::uint8_t>(y, x) > cut ? 255 : 0;
      }
    }
    cv::Mat expanded, enhanced;
    cv::resize(cells, expanded, {codeSide, codeSide}, 0, 0, cv::INTER_NEAREST);
    cv::copyMakeBorder(expanded, enhanced, quiet * pixels, quiet * pixels, quiet * pixels,
                       quiet * pixels, cv::BORDER_CONSTANT, cv::Scalar(255));
    const auto afterCodes = decode(enhanced, options, 128, ctx);
    const auto decodedCount = [](const auto& codes) {
      return std::count_if(codes.begin(), codes.end(), [](const auto& code) { return code.valid; });
    };
    const auto beforeCount = decodedCount(beforeCodes), afterCount = decodedCount(afterCodes);
    Json originalQuad = Json::array();
    for (const auto& point : quad)
      originalQuad.push_back({point[0] * input->scale, point[1] * input->scale});
    cv::Mat shift = cv::Mat::eye(3, 3, CV_64F);
    shift.at<double>(0, 2) = quiet * pixels;
    shift.at<double>(1, 2) = quiet * pixels;
    cv::Mat inverse = (shift * transform).inv();
    inverse.row(0) *= input->scale;
    inverse.row(1) *= input->scale;
    Json matrix = Json::array();
    for (int y = 0; y < 3; ++y)
      for (int x = 0; x < 3; ++x)
        matrix.push_back(inverse.at<double>(y, x));
    Record feedback{
        "image.QrRefinement.v1",
        {{"sourceFrame", reference},
         {"outputFrame", outputFrame},
         {"sourceUnit", "px"},
         {"outputUnit", "px"},
         {"sourceCorners", std::move(originalQuad)},
         {"outputToSource", std::move(matrix)},
         {"moduleCount", modules},
         {"modulePixels", pixels},
         {"quietZone", quiet},
         {"thresholdMode", threshold},
         {"polarity", polarity},
         {"uncertainCells", uncertain},
         {"minimumLocalContrast", minimumContrast},
         {"beforeDecoded", beforeCount > 0},
         {"afterDecoded", afterCount > 0},
         {"improved", beforeCount == 0 && afterCount > 0},
         {"before", feedbackCodes(beforeCodes)},
         {"after", feedbackCodes(afterCodes)},
         {"backend", "ZXing-C++ 3.1.1"},
         {"backendPatchRevision", 1},
         {"beforeDefinition", "same rectification and quiet zone, before cell reconstruction"}}};
    bounded(feedback);
    auto image = native(std::move(enhanced), 1);
    check(ctx);
    out.set("image", Data::image(std::move(image)));
    out.set("feedback", Data::record(std::move(feedback)));
    return Status::Ok();
  } catch (const Failure& error) {
    return error.status;
  } catch (const std::bad_alloc&) {
    return Status::Error(Phase::Execute, "resource_limit", "QR增强内存分配失败");
  } catch (const std::exception& error) {
    return img::badInput(std::string("QR增强失败：") + error.what());
  }
}
} // namespace
void registerP4Barcodes(Registry& registry) {
  auto decode = image_p1::descriptor("image.barcode_decode", "条码定位解码",
                                     "barcode QR code 1D 2D ZXing UTF8 Chinese 条形码 二维码 解码");
  decode.inputs = {img::imageIn(),
                   Port{"roi", "Box2D", "ROI", "可选像素裁剪，与region互斥。", false},
                   image_p1::recordPort("region", poses::kPose2DType, false)};
  decode.outputs = {image_p1::recordPort("instances", poses::kPoses2DType),
                    image_p1::recordPort("detections", kDetections), img::imageOut()};
  auto formats =
      img::enumParam("formats", "条码类型", "Any", {}, "ZXing支持的格式；无目标返回空集合。");
  for (const char* format :
       {"Any",        "Linear",  "Matrix", "QRCode",      "QRCodeModel2", "MicroQRCode",
        "RMQRCode",   "Code128", "Code39", "Code93",      "Codabar",      "EAN13",
        "EAN8",       "UPCA",    "UPCE",   "ITF",         "DataBar",      "DataBarExp",
        "DataMatrix", "Aztec",   "PDF417", "MicroPDF417", "MaxiCode",     "Telepen"})
    formats.options.push_back({format, format, ""});
  decode.params = {
      image_p1::text("frame", "图像参考帧", "image"),
      std::move(formats),
      img::boolParam("tryHarder", "深入搜索", true, "增加扫描工作，不保证成功。"),
      img::boolParam("tryRotate", "搜索旋转码", true, ""),
      img::boolParam("tryInvert", "搜索反色码", true, ""),
      img::boolParam("tryDownscale", "搜索多尺度", true, ""),
      img::intParam("maxSymbols", "结果数量预算", 128, 1, 128, "超过预算报错，不静默丢码。")};
  decode.doc =
      "ZXing-C++真实多格式解码，UTF-8文本与原始字节hex分别保留；valid独立于空内容。原图像素四角与非抓取位姿，叠图保留输入位深/通道/alpha。取消在库阶段边界检查。";
  decode.compute = &decodeOperator;
  registry.addOperator(std::move(decode));
  auto refine = image_p1::descriptor(
      "image.qr_refine", "二维码模块增强",
      "QR refine reconstruct modules threshold contrast 二维码 增强 模块 重绘");
  refine.inputs = {img::imageIn(),
                   Port{"roi", "Box2D", "ROI", "已裁正QR模块区域；不含静区。", false},
                   image_p1::recordPort("detections", kDetections, false)};
  refine.outputs = {img::imageOut(), image_p1::recordPort("feedback", "image.QrRefinement.v1")};
  auto modulePixels = img::intParam("modulePixels", "输出每格像素", 8, 1, 32,
                                    "输出栅格像素，不随输入预览scale改变。");
  modulePixels.unit = "px";
  modulePixels.absolute = true;
  refine.params = {
      image_p1::text("frame", "源图参考帧", "image"),
      image_p1::text("outputFrame", "增强图参考帧", "qr_refined"),
      image_p1::text("symbolId", "指定QR目标ID", ""),
      img::intParam("moduleCount", "标准QR边长格数", 21, 21, 177,
                    "21+4k，须已知真实QR版本；不自动猜格数。"),
      std::move(modulePixels),
      img::intParam("quietZone", "静区模块数", 4, 4, 16, "标准QR至少四格静区。"),
      img::enumParam("polarity", "源码极性", "dark",
                     {{"dark", "黑码白底", ""}, {"light", "白码黑底", ""}}, "输出统一黑码白底。"),
      img::enumParam("thresholdMode", "模块阈值", "localRange",
                     {{"localRange", "局部亮暗范围", ""}, {"otsu", "全局Otsu", ""}}, ""),
      img::intParam("localRadius", "局部邻域半径", 3, 1, 32, "单位为模块格。"),
      img::floatParam("minCellContrast", "局部对比度下限", .08,
                      "0..1；不足时使用全局阈值并计入不确定格。")};
  refine.doc =
      "已知标准QR格数的透视裁正、模块均值阈值及静区重绘；不发明纠错内容。输入detections时需有效标准QR四角，多目标指定symbolId。缺省输入已裁好的QR模块区域。before与after采用相同裁正/静区几何，输出u8单通道新坐标系及明确反馈。";
  refine.compute = &refineOperator;
  registry.addOperator(std::move(refine));
}
} // namespace lyflow::ops
