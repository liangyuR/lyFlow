#include "p4_ocr_common.h"

namespace lyflow::ops {
namespace {
using namespace ocr;

void preprocess(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  const auto& source = image(in, ctx);
  const auto frame = text(p, "frame");
  const auto order = p.choice("channelOrder");
  const int maxSide = integer(p, "maxSide", 32, 2048);
  if (maxSide % 32)
    throw Failure{img::badParam("maxSide须为32的倍数", "maxSide")};
  const auto mean = p.vec3("mean"), deviation = p.vec3("std");
  for (int c = 0; c < 3; ++c)
    if (!std::isfinite(mean[c]) || mean[c] < 0 || mean[c] > 1 || !std::isfinite(deviation[c]) ||
        deviation[c] < 1e-6 || deviation[c] > 1)
      throw Failure{img::badParam("mean须为0..1，std须为1e-6..1", "std")};
  cv::Rect crop(0, 0, source.width, source.height);
  if (in.has("roi")) {
    const auto* box = in.get("roi").asBox2D();
    require(box && box->unit == Unit2D::Pixel && std::isfinite(box->min[0]) &&
                std::isfinite(box->min[1]) && std::isfinite(box->max[0]) &&
                std::isfinite(box->max[1]) && box->max[0] > box->min[0] &&
                box->max[1] > box->min[1],
            "roi须为有限正面积像素Box2D", "roi");
    const double left = std::max(0., std::ceil(static_cast<double>(box->min[0])));
    const double top = std::max(0., std::ceil(static_cast<double>(box->min[1])));
    const double right = std::min(source.width - 1., std::floor(static_cast<double>(box->max[0])));
    const double bottom =
        std::min(source.height - 1., std::floor(static_cast<double>(box->max[1])));
    require(right > left && bottom > top, "roi与图像至少须有2×2像素交集", "roi");
    crop = {static_cast<int>(left), static_cast<int>(top), static_cast<int>(right - left + 1),
            static_cast<int>(bottom - top + 1)};
  }
  double ratio = static_cast<double>(maxSide) / std::max(crop.width, crop.height);
  if (!p.flag("allowUpscale"))
    ratio = std::min(1., ratio);
  const auto roundSize = [&](double pixels) {
    const double base = std::floor(pixels / 32), fraction = pixels / 32 - base;
    return std::clamp(
        static_cast<int>(base + (fraction > .5 || (fraction == .5 && std::fmod(base, 2) != 0))) *
            32,
        32, maxSide);
  };
  const int width = roundSize(crop.width * ratio), height = roundSize(crop.height * ratio);
  auto colored = color(source, order);
  cv::Mat resized;
  cv::resize(colored(crop), resized, {width, height}, 0, 0, cv::INTER_LINEAR);
  Tensor input;
  input.shape = {1, 3, height, width};
  input.data.resize(static_cast<std::size_t>(3) * height * width);
  tensorImage(resized, input, 0, width, mean, deviation);
  Record mapping{kMapping,
                 {{"frame", frame},
                  {"unit", "px"},
                  {"imageSize", {source.width, source.height}},
                  {"crop", {crop.x, crop.y, crop.width, crop.height}},
                  {"inputSize", {width, height}},
                  {"channelOrder", order}}};
  bounded(mapping);
  out.set("input", Data::tensor(std::move(input)));
  out.set("mapping", Data::record(std::move(mapping)));
}

struct Quad {
  std::array<cv::Point2f, 4> points;
  double score = 0;
};
std::array<cv::Point2f, 4> canonical(cv::RotatedRect rectangle) {
  std::array<cv::Point2f, 4> points;
  rectangle.points(points.data());
  const auto center = rectangle.center;
  std::sort(points.begin(), points.end(), [&](const auto& a, const auto& b) {
    return std::atan2(a.y - center.y, a.x - center.x) < std::atan2(b.y - center.y, b.x - center.x);
  });
  auto first = std::min_element(points.begin(), points.end(), [](const auto& a, const auto& b) {
    return std::make_tuple(a.x + a.y, a.y, a.x) < std::make_tuple(b.x + b.y, b.y, b.x);
  });
  std::rotate(points.begin(), first, points.end());
  return points;
}

void dbDecode(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  const auto& source = image(in, ctx);
  const auto& mapping = record(in, "mapping", kMapping);
  int width, height, originalWidth, originalHeight, x, y, cropWidth, cropHeight;
  require(mapping.size() == 6 && stringField(mapping, "frame") &&
              mapping.value("unit", Json()) == "px" && mapping.contains("imageSize") &&
              mapping["imageSize"].is_array() && mapping["imageSize"].size() == 2 &&
              dimension(mapping["imageSize"][0], originalWidth) &&
              dimension(mapping["imageSize"][1], originalHeight) && originalWidth == source.width &&
              originalHeight == source.height && mapping.contains("inputSize") &&
              mapping["inputSize"].is_array() && mapping["inputSize"].size() == 2 &&
              dimension(mapping["inputSize"][0], width, 32, 2048) &&
              dimension(mapping["inputSize"][1], height, 32, 2048) && width % 32 == 0 &&
              height % 32 == 0 && mapping.contains("crop") && mapping["crop"].is_array() &&
              mapping["crop"].size() == 4 &&
              dimension(mapping["crop"][0], x, 0, originalWidth - 1) &&
              dimension(mapping["crop"][1], y, 0, originalHeight - 1) &&
              dimension(mapping["crop"][2], cropWidth, 2, originalWidth) &&
              dimension(mapping["crop"][3], cropHeight, 2, originalHeight) &&
              x <= originalWidth - cropWidth && y <= originalHeight - cropHeight &&
              (mapping.value("channelOrder", Json()) == "rgb" ||
               mapping.value("channelOrder", Json()) == "bgr"),
          "检测映射与原图或固定DB预处理不匹配", "mapping");
  const auto* prediction = in.get("prediction").asTensor();
  finite(prediction, "prediction", ctx);
  require(prediction->shape == std::vector<std::int64_t>{1, 1, height, width},
          "DB输出必须为单头float32 [1,1,inputHeight,inputWidth]概率图", "prediction");
  for (float value : prediction->data)
    require(value >= 0 && value <= 1, "DB概率必须为0..1", "prediction");
  const double threshold = number(p, "pixelThreshold", .001, .999);
  const double boxThreshold = number(p, "boxThreshold", 0, 1);
  const double unclip = number(p, "unclipRatio", 0, 5);
  const double minSide = number(p, "minSide", 1, 1024);
  const int maxRegions = integer(p, "maxRegions", 1, static_cast<int>(kRegions));
  cv::Mat scores(height, width, CV_32F, const_cast<float*>(prediction->data.data()));
  cv::Mat binary;
  cv::compare(scores, threshold, binary, cv::CMP_GE);
  std::vector<std::vector<cv::Point>> contours;
  cv::findContours(binary, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
  limit(contours.size() <= 4096, "DB轮廓超过4096候选预算");
  std::size_t contourPoints = 0, scoreWork = 0;
  std::vector<Quad> candidates;
  for (const auto& contour : contours) {
    check(ctx);
    contourPoints += contour.size();
    limit(contourPoints <= 4 * 1024 * 1024, "DB轮廓点超过4Mi预算");
    if (contour.size() < 3)
      continue;
    const double area = cv::contourArea(contour), perimeter = cv::arcLength(contour, true);
    if (area <= 0 || perimeter <= 0)
      continue;
    const auto bounds = cv::boundingRect(contour) & cv::Rect(0, 0, width, height);
    scoreWork += bounds.area();
    limit(scoreWork <= 64 * 1024 * 1024, "DB候选打分超过64Mi像素预算");
    cv::Mat mask(bounds.size(), CV_8U, cv::Scalar(0));
    std::vector<cv::Point> shifted;
    shifted.reserve(contour.size());
    for (auto point : contour)
      shifted.push_back(point - bounds.tl());
    cv::fillPoly(mask, std::vector<std::vector<cv::Point>>{shifted}, cv::Scalar(255));
    const double score = cv::mean(scores(bounds), mask)[0];
    if (score < boxThreshold)
      continue;
    auto rectangle = cv::minAreaRect(contour);
    const double offset = area * unclip / perimeter;
    rectangle.size.width += static_cast<float>(2 * offset);
    rectangle.size.height += static_cast<float>(2 * offset);
    Quad quad{canonical(rectangle), score};
    for (auto& point : quad.points) {
      point.x = static_cast<float>(std::clamp(x + (point.x + .5) * cropWidth / width - .5,
                                              double(x), double(x + cropWidth - 1)));
      point.y = static_cast<float>(std::clamp(y + (point.y + .5) * cropHeight / height - .5,
                                              double(y), double(y + cropHeight - 1)));
    }
    const std::vector<cv::Point2f> polygon(quad.points.begin(), quad.points.end());
    if (!cv::isContourConvex(polygon) || cv::contourArea(polygon) <= 1 ||
        std::min(cv::norm(quad.points[1] - quad.points[0]),
                 cv::norm(quad.points[3] - quad.points[0])) < minSide)
      continue;
    candidates.push_back(quad);
    limit(candidates.size() <= static_cast<std::size_t>(maxRegions),
          "DB合格文字超过maxRegions；不截断输出");
  }
  auto top = [](const Quad& q) {
    return std::min({q.points[0].y, q.points[1].y, q.points[2].y, q.points[3].y});
  };
  auto left = [](const Quad& q) {
    return std::min({q.points[0].x, q.points[1].x, q.points[2].x, q.points[3].x});
  };
  std::sort(candidates.begin(), candidates.end(), [&](const auto& a, const auto& b) {
    return std::make_tuple(top(a), left(a), -a.score) < std::make_tuple(top(b), left(b), -b.score);
  });
  for (std::size_t begin = 0; begin < candidates.size();) {
    std::size_t end = begin + 1;
    const auto& first = candidates[begin];
    const double lineHeight = std::min(cv::norm(first.points[1] - first.points[0]),
                                       cv::norm(first.points[3] - first.points[0]));
    while (end < candidates.size() && top(candidates[end]) - top(first) <= .5 * lineHeight)
      ++end;
    std::stable_sort(candidates.begin() + begin, candidates.begin() + end,
                     [&](const auto& a, const auto& b) { return left(a) < left(b); });
    begin = end;
  }
  Tensor quads;
  quads.shape = {static_cast<std::int64_t>(candidates.size()), 4, 2};
  Json items = Json::array(), poses = Json::array();
  cv::Mat overlay = cvx::view(source).clone();
  const double full = source.depth == PixelDepth::U8    ? 255
                      : source.depth == PixelDepth::U16 ? 65535
                                                        : 1;
  for (std::size_t index = 0; index < candidates.size(); ++index) {
    check(ctx);
    const auto& candidate = candidates[index];
    const auto id = "text-" + std::to_string(index);
    cv::Point2d center;
    std::vector<cv::Point> polygon;
    for (const auto point : candidate.points) {
      center += cv::Point2d(point) * .25;
      quads.data.insert(quads.data.end(), {point.x, point.y});
      polygon.emplace_back(cvRound(point.x), cvRound(point.y));
    }
    const auto direction = candidate.points[1] - candidate.points[0];
    auto pose = poses::make2D(
        id, mapping["frame"], "px", {center.x, center.y}, std::atan2(direction.y, direction.x),
        {cv::norm(direction), cv::norm(candidate.points[3] - candidate.points[0])});
    pose.data["confidence"] = candidate.score;
    pose.data["pickable"] = false;
    poses.push_back(pose.data);
    items.push_back({{"id", id}, {"score", candidate.score}});
    cv::polylines(overlay, std::vector<std::vector<cv::Point>>{polygon}, true,
                  source.channels == 1 ? cv::Scalar(full) : cv::Scalar(0, full, 0, full), 1,
                  cv::LINE_8);
  }
  if (source.channels == 4) {
    cv::Mat alpha;
    cv::extractChannel(cvx::view(source), alpha, 3);
    cv::insertChannel(alpha, overlay, 3);
  }
  Record regionsRecord{kRegionsType,
                       {{"frame", mapping["frame"]},
                        {"unit", "px"},
                        {"imageSize", {source.width, source.height}},
                        {"geometryHash", geometryHash(quads)},
                        {"items", items}}};
  Record instances{poses::kPoses2DType,
                   {{"frame", mapping["frame"]}, {"unit", "px"}, {"items", poses}}};
  bounded(regionsRecord);
  bounded(instances);
  std::string why;
  require(poses::validate(instances, why), "DB位姿输出无效：" + why, "prediction");
  Image output;
  require(cvx::fromMat(std::move(overlay), output), "OCR叠图输出失败", "image");
  out.set("quads", Data::tensor(std::move(quads)));
  out.set("regions", Data::record(std::move(regionsRecord)));
  out.set("instances", Data::record(std::move(instances)));
  out.set("image", Data::image(std::move(output)));
}

void crops(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  const auto& source = image(in, ctx);
  const auto* quads = in.get("quads").asTensor();
  finite(quads, "quads", ctx);
  const auto metadata = regions(in, *quads, source);
  const auto order = p.choice("channelOrder");
  const int maxWidth = integer(p, "maxWidth", 32, 2048),
            minWidth = integer(p, "minWidth", 32, maxWidth);
  auto colored = color(source, order);
  std::vector<cv::Mat> rectified;
  std::vector<int> widths;
  Json items = Json::array();
  int width = minWidth;
  std::size_t warpPixels = 0;
  for (std::size_t i = 0; i < metadata.items.size(); ++i) {
    check(ctx);
    std::vector<cv::Point2f> points;
    for (int corner = 0; corner < 4; ++corner) {
      const float x = quads->data[i * 8 + corner * 2], y = quads->data[i * 8 + corner * 2 + 1];
      require(x >= 0 && x <= source.width - 1 && y >= 0 && y <= source.height - 1,
              "文字四边形必须位于原图内", "quads");
      points.emplace_back(x, y);
    }
    require(cv::isContourConvex(points) && cv::contourArea(points, true) > 1,
            "文字四边形须为TL/TR/BR/BL顺序、非退化凸四边形", "quads");
    const int w =
        std::max(2, static_cast<int>(std::ceil(std::max(cv::norm(points[1] - points[0]),
                                                        cv::norm(points[2] - points[3])))));
    const int h =
        std::max(2, static_cast<int>(std::ceil(std::max(cv::norm(points[3] - points[0]),
                                                        cv::norm(points[2] - points[1])))));
    warpPixels += static_cast<std::size_t>(w) * h;
    limit(warpPixels <= 16 * 1024 * 1024, "OCR透视裁切累计超过16Mi像素预算");
    const std::vector<cv::Point2f> target{
        {0, 0}, {float(w - 1), 0}, {float(w - 1), float(h - 1)}, {0, float(h - 1)}};
    cv::Mat crop;
    cv::warpPerspective(colored, crop, cv::getPerspectiveTransform(points, target), {w, h},
                        cv::INTER_LINEAR, cv::BORDER_REPLICATE);
    const bool vertical = p.flag("rotateVertical") && h >= 1.5 * w;
    if (vertical)
      cv::rotate(crop, crop, cv::ROTATE_90_COUNTERCLOCKWISE);
    const int validWidth = std::max(1, static_cast<int>(std::ceil(48. * crop.cols / crop.rows)));
    limit(validWidth <= maxWidth, "文字行宽度超过maxWidth；不会截断字符或压缩长行");
    width = std::max(width, validWidth);
    widths.push_back(validWidth);
    rectified.push_back(std::move(crop));
    items.push_back({{"id", metadata.items[i]["id"]},
                     {"detectorScore", metadata.items[i]["score"]},
                     {"validWidth", validWidth},
                     {"rotated90", vertical},
                     {"orientation", 0},
                     {"classifierLabel", 0},
                     {"orientationConfidence", 0.}});
  }
  const auto count = metadata.items.size();
  const auto recCount = count * 3 * 48 * width, clsCount = count * 3 * 48 * 192;
  limit((recCount + clsCount) * sizeof(float) + warpPixels * 12 + source.byteSize() * 2 <=
            512 * 1024 * 1024,
        "OCR裁切预计内存超过512MiB预算");
  Tensor recInput, clsInput;
  recInput.shape = {static_cast<std::int64_t>(count), 3, 48, width};
  recInput.data.resize(recCount);
  clsInput.shape = {static_cast<std::int64_t>(count), 3, 48, 192};
  clsInput.data.resize(clsCount);
  for (std::size_t i = 0; i < count; ++i) {
    check(ctx);
    cv::Mat rec, cls;
    cv::resize(rectified[i], rec, {widths[i], 48}, 0, 0, cv::INTER_LINEAR);
    cv::resize(rectified[i], cls, {std::min(192, widths[i]), 48}, 0, 0, cv::INTER_LINEAR);
    tensorImage(rec, recInput, i, width, {.5f, .5f, .5f}, {.5f, .5f, .5f});
    tensorImage(cls, clsInput, i, 192, {.5f, .5f, .5f}, {.5f, .5f, .5f});
  }
  Record layoutRecord{kLayout,
                      {{"frame", metadata.frame},
                       {"unit", "px"},
                       {"imageSize", {source.width, source.height}},
                       {"geometryHash", metadata.hash},
                       {"channelOrder", order},
                       {"recHeight", 48},
                       {"recWidth", width},
                       {"clsHeight", 48},
                       {"clsWidth", 192},
                       {"items", items}}};
  bounded(layoutRecord);
  out.set("recInput", Data::tensor(std::move(recInput)));
  out.set("clsInput", Data::tensor(std::move(clsInput)));
  out.set("layout", Data::record(std::move(layoutRecord)));
}

} // namespace

void registerP4OcrImage(Registry& registry) {
  auto prep = descriptor("image.ocr_preprocess", "文字检测预处理", preprocess);
  prep.inputs = {img::imageIn(), Port{"roi", "Box2D", "区域", "可选原图像素区域", false}};
  prep.outputs = {Port{"input", "Tensor", "检测输入", "float32 [1,3,H,W]", true},
                  image_p1::recordPort("mapping", kMapping)};
  prep.params = {
      image_p1::text("frame", "参考帧", "image"),
      img::intParam("maxSide", "检测最长边", 960, 32, 2048, "32的倍数"),
      img::boolParam("allowUpscale", "允许放大", false, ""),
      img::enumParam("channelOrder", "模型颜色顺序", "bgr",
                     {{"bgr", "BGR", "Paddle/RapidOCR"}, {"rgb", "RGB", "显式自定义模型"}}, ""),
      image_p1::vector("mean", "均值", {.5, .5, .5}),
      image_p1::vector("std", "标准差", {.5, .5, .5})};
  prep.doc =
      "全分辨率OCR检测输入，保持RGB(A)源只读；默认PP-OCRv6 BGR/0.5归一化，逆映射保留原图ROI坐标。";
  registry.addOperator(std::move(prep));
  auto db = descriptor("image.ocr_db_decode", "文字区域检测解码", dbDecode);
  db.inputs = {img::imageIn(), Port{"prediction", "Tensor", "DB概率图", "[1,1,H,W]", true},
               image_p1::recordPort("mapping", kMapping)};
  db.outputs = {Port{"quads", "Tensor", "四边形", "原图像素[N,4,2] TL/TR/BR/BL", true},
                image_p1::recordPort("regions", kRegionsType),
                image_p1::recordPort("instances", poses::kPoses2DType), img::imageOut()};
  db.params = {real("pixelThreshold", "像素阈值", .3, .001, .999),
               real("boxThreshold", "区域阈值", .5, 0, 1),
               real("unclipRatio", "矩形扩张比例", 1.5, 0, 5),
               real("minSide", "最短边", 3, 1, 1024),
               img::intParam("maxRegions", "区域预算", 128, 1, 128, "超出报错，不静默截断")};
  db.doc =
      "DB类单头概率解码，轮廓内均值评分和area/perimeter距离扩张有限旋转矩形，非任意多边形offset。稳定行排序；原图坐标四边形与id配对。叠图只画框，Unicode文字由results呈现。";
  registry.addOperator(std::move(db));
  auto crop = descriptor("image.ocr_crops", "文字透视裁切", crops);
  crop.inputs = {img::imageIn(), Port{"quads", "Tensor", "四边形", "[N,4,2]", true},
                 image_p1::recordPort("regions", kRegionsType)};
  crop.outputs = {Port{"recInput", "Tensor", "识别输入", "[N,3,48,W]", true},
                  Port{"clsInput", "Tensor", "方向输入", "[N,3,48,192]", true},
                  image_p1::recordPort("layout", kLayout)};
  crop.params = {
      img::intParam("minWidth", "识别最小宽度", 320, 32, 2048, ""),
      img::intParam("maxWidth", "识别宽度预算", 2048, 32, 2048, "长行超限报错"),
      img::boolParam("rotateVertical", "旋转竖行", true, "长宽比至少1.5时逆时针90度"),
      img::enumParam("channelOrder", "模型颜色顺序", "bgr",
                     {{"bgr", "BGR", "Paddle/RapidOCR"}, {"rgb", "RGB", "显式自定义模型"}}, "")};
  crop.doc =
      "对每个真实四边形执行透视校正，保持批次id顺序；识别按等比48高右填零，方向模型48×192。空区域生成零批次张量。";
  registry.addOperator(std::move(crop));
}
} // namespace lyflow::ops
