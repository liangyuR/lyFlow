#include <queue>

#include "lyflow/pose.h"
#include "p1_common.h"

namespace lyflow::ops {
namespace {

Status matchingImage(const Image* input, const char* name, cv::Mat& gray) {
  auto s = image_p1::image(input, name, false, 4194304);
  if (!s.ok)
    return s;
  if (input->depth == PixelDepth::F32) {
    double low, high;
    cv::minMaxLoc(cvx::view(*input).reshape(1), &low, &high);
    if (low < 0 || high > 1)
      return img::badInput("NCC浮点图像需要显式归一化到[0,1]", name);
  }
  image_p1::gray(*input).convertTo(
      gray, CV_32F, 1 / cvx::fullScale(cvx::cvDepthOf(pixelDepthName(input->depth))));
  return Status::Ok();
}

Status build(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  const Image* image = in.get("image").asImage();
  cv::Mat gray;
  auto s = matchingImage(image, "image", gray);
  if (!s.ok)
    return s;
  if (image->width < 3 || image->height < 3 || image->pixelCount() > 1048576)
    return img::badInput("模板须至少3×3且不超过1Mi像素");
  const auto& id = p.text("id");
  if (id.empty() || id.size() > 256)
    return img::badParam("模板ID不能为空且最多256字节", "id");
  cv::Scalar mean, stddev;
  cv::meanStdDev(gray, mean, stddev);
  if (stddev[0] <= 1e-6)
    return Status::Error(Phase::Execute, "degenerate_template",
                         "常量或低变化模板不具备NCC定位信息");
  if (ctx.cancelled())
    return Status::Ok();
  s = image_p1::record(
      out, "model", "vision.NCCTemplate.v1",
      {{"id", id},
       {"unit", "px"},
       {"method", "masked-ncc-v1"},
       {"imageSize",
        {std::int64_t(image->width) * image->scale, std::int64_t(image->height) * image->scale}}});
  if (!s.ok)
    return s;
  return image_p1::putImage(out, "template", gray.clone(), image->scale);
}

struct Candidate {
  cv::RotatedRect box;
  double angle = 0, scale = 1, score = 0;
  int variant = 0, x = 0, y = 0;
};

bool better(const Candidate& a, const Candidate& b) {
  if (a.score != b.score)
    return a.score > b.score;
  if (a.variant != b.variant)
    return a.variant < b.variant;
  return a.y != b.y ? a.y < b.y : a.x < b.x;
}

double overlap(const cv::RotatedRect& a, const cv::RotatedRect& b) {
  std::vector<cv::Point2f> polygon;
  const int relation = cv::rotatedRectangleIntersection(a, b, polygon);
  if (relation == cv::INTERSECT_NONE || polygon.size() < 3)
    return 0;
  const double intersection = std::abs(cv::contourArea(polygon));
  const double total = double(a.size.area()) + b.size.area() - intersection;
  return total > 0 ? intersection / total : 0;
}

bool range(double lo, double hi, double step, double min, double max, std::vector<double>& values) {
  if (!std::isfinite(lo) || !std::isfinite(hi) || !std::isfinite(step) || lo < min || hi > max ||
      lo > hi || step <= 0 || (hi - lo) / step > 80)
    return false;
  const int count = static_cast<int>(std::floor((hi - lo) / step + 1e-10)) + 1;
  for (int i = 0; i < count; ++i)
    values.push_back(lo + i * step);
  return true;
}

Status match(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  const Image* input = in.get("image").asImage();
  const Image* pattern = in.get("template").asImage();
  cv::Mat source, original;
  auto s = matchingImage(input, "image", source);
  if (!s.ok)
    return s;
  s = matchingImage(pattern, "template", original);
  if (!s.ok)
    return s;
  if (input->scale != pattern->scale || pattern->channels != 1 || pattern->pixelCount() > 1048576)
    return img::badInput("模板须为同预览比例的单通道图像，且不超过1Mi像素", "template");
  const Record* model = in.get("model").asRecord();
  std::string why;
  if (!model || model->type != "vision.NCCTemplate.v1" || !values::boundedJson(model->data, why) ||
      !model->data.is_object() || model->data.size() != 4 ||
      model->data.value("method", nlohmann::json()) != "masked-ncc-v1" ||
      model->data.value("unit", nlohmann::json()) != "px" || !model->data.contains("id") ||
      !model->data["id"].is_string() || model->data["id"].get<std::string>().empty() ||
      model->data["id"].get_ref<const std::string&>().size() > 256 ||
      !model->data.contains("imageSize") || !poses::numericArray(model->data["imageSize"], 2, true))
    return img::badInput("模板需要有效的vision.NCCTemplate.v1元数据", "model");
  const double fullWidth = model->data["imageSize"][0].get<double>();
  const double fullHeight = model->data["imageSize"][1].get<double>();
  if (std::ceil(fullWidth / pattern->scale) != pattern->width ||
      std::ceil(fullHeight / pattern->scale) != pattern->height)
    return img::badInput("模板图尺寸与元数据不匹配", "model");
  const auto& frame = p.text("frame");
  if (frame.empty() || frame.size() > 256)
    return img::badParam("参考帧不能为空且最多256字节", "frame");
  std::vector<double> angles, scales;
  if (!range(p.number("angleMin"), p.number("angleMax"), p.number("angleStep"), -180, 180,
             angles) ||
      !range(p.number("scaleMin"), p.number("scaleMax"), p.number("scaleStep"), 0.1, 4, scales) ||
      angles.size() * scales.size() > 81)
    return img::badParam("角度/尺度范围无效或组合超过81个", "angleMin");
  const double threshold = p.number("minScore"), iou = p.number("nmsIou");
  const int limit = static_cast<int>(p.integer("maxDetections"));
  const int candidateLimit = static_cast<int>(p.integer("maxCandidates"));
  if (!std::isfinite(threshold) || threshold < 0 || threshold > 1 || !std::isfinite(iou) ||
      iou < 0 || iou > 1 || limit < 1 || limit > 4096 || candidateLimit < 1 ||
      candidateLimit > 4096)
    return img::badParam("分数/IoU阈值或结果数量上限非法", "minScore");
  if (input->pixelCount() * angles.size() * scales.size() > 67108864)
    return img::badInput("模板搜索超过64Mi响应像素预算");
  cv::Mat sourceSquared = source.mul(source);
  std::priority_queue<Candidate, std::vector<Candidate>, decltype(&better)> candidates(&better);
  int variant = 0;
  for (double scale : scales) {
    for (double angle : angles) {
      if (ctx.cancelled())
        return Status::Ok();
      const int w = std::max(1, cvRound(original.cols * scale));
      const int h = std::max(1, cvRound(original.rows * scale));
      if (w < 3 || h < 3 || std::uint64_t(w) * h > 1048576)
        return img::badParam("缩放模板小于3×3或超过1Mi像素", "scaleMax");
      const cv::Point2f center(float(w - 1) / 2, float(h - 1) / 2);
      const double radians = angle * poses::kPi / 180;
      const double cosine = std::abs(std::cos(radians)), sine = std::abs(std::sin(radians));
      const int rw = std::max(1, int(std::ceil(w * cosine + h * sine - 1e-9)));
      const int rh = std::max(1, int(std::ceil(w * sine + h * cosine - 1e-9)));
      if (std::uint64_t(rw) * rh > 2097152)
        return img::badParam("旋转模板超过2Mi像素", "angleMin");
      if (rw > source.cols || rh > source.rows) {
        ++variant;
        continue;
      }
      cv::Mat resized, rotated, mask;
      cv::resize(original, resized, cv::Size(w, h), 0, 0, cv::INTER_LINEAR);
      cv::Mat transform = cv::getRotationMatrix2D(center, -angle, 1);
      transform.at<double>(0, 2) += (rw - 1) / 2.0 - center.x;
      transform.at<double>(1, 2) += (rh - 1) / 2.0 - center.y;
      cv::warpAffine(resized, rotated, transform, cv::Size(rw, rh), cv::INTER_LINEAR,
                     cv::BORDER_CONSTANT, cv::Scalar(0));
      cv::warpAffine(cv::Mat(h, w, CV_8U, cv::Scalar(255)), mask, transform, cv::Size(rw, rh),
                     cv::INTER_NEAREST, cv::BORDER_CONSTANT, cv::Scalar(0));
      const int count = cv::countNonZero(mask);
      cv::Scalar mean, stddev;
      cv::meanStdDev(rotated, mean, stddev, mask);
      if (count < 9 || stddev[0] <= 1e-6)
        return Status::Error(Phase::Execute, "degenerate_template", "变换后的模板没有足够灰度变化");
      cv::Mat weight, centered;
      mask.convertTo(weight, CV_32F, 1.0 / 255);
      centered = (rotated - mean[0]).mul(weight);
      cv::Mat numerator, sum, squares;
      cv::matchTemplate(source, centered, numerator, cv::TM_CCORR);
      cv::matchTemplate(source, weight, sum, cv::TM_CCORR);
      cv::matchTemplate(sourceSquared, weight, squares, cv::TM_CCORR);
      const double templateVariance = stddev[0] * stddev[0] * count;
      cv::Mat scores(numerator.size(), CV_32F, cv::Scalar(-1));
      for (int y = 0; y < numerator.rows; ++y) {
        if (ctx.cancelled())
          return Status::Ok();
        for (int x = 0; x < numerator.cols; ++x) {
          const double total = sum.at<float>(y, x);
          const double variance = squares.at<float>(y, x) - total * total / count;
          if (variance > 1e-8 * count) {
            const double value = numerator.at<float>(y, x) / std::sqrt(variance * templateVariance);
            if (std::isfinite(value))
              scores.at<float>(y, x) = static_cast<float>(std::clamp(value, -1.0, 1.0));
          }
        }
      }
      cv::Mat maxima;
      cv::dilate(scores, maxima, cv::Mat());
      for (int y = 0; y < scores.rows; ++y) {
        if (ctx.cancelled())
          return Status::Ok();
        for (int x = 0; x < scores.cols; ++x) {
          const double score = scores.at<float>(y, x);
          if (score < threshold || score != maxima.at<float>(y, x))
            continue;
          Candidate candidate{
              cv::RotatedRect(cv::Point2f(float(x + (rw - 1) / 2.0), float(y + (rh - 1) / 2.0)),
                              cv::Size2f(float(w), float(h)), float(angle)),
              angle,
              scale,
              score,
              variant,
              x,
              y};
          if (candidates.size() < static_cast<std::size_t>(candidateLimit))
            candidates.push(candidate);
          else if (better(candidate, candidates.top())) {
            candidates.pop();
            candidates.push(candidate);
          }
        }
      }
      ++variant;
    }
  }
  std::vector<Candidate> sorted;
  while (!candidates.empty()) {
    sorted.push_back(candidates.top());
    candidates.pop();
  }
  std::sort(sorted.begin(), sorted.end(), better);
  std::vector<Candidate> kept;
  for (const auto& candidate : sorted) {
    if (ctx.cancelled())
      return Status::Ok();
    bool suppressed = false;
    for (const auto& previous : kept)
      if (overlap(candidate.box, previous.box) > iou) {
        suppressed = true;
        break;
      }
    if (!suppressed)
      kept.push_back(candidate);
    if (kept.size() >= static_cast<std::size_t>(limit))
      break;
  }
  nlohmann::json items = nlohmann::json::array();
  int number = 0;
  for (const auto& candidate : kept) {
    Record pose = poses::make2D(
        model->data["id"].get<std::string>() + "-" + std::to_string(number++), frame, "px",
        {candidate.box.center.x * input->scale, candidate.box.center.y * input->scale},
        candidate.angle * poses::kPi / 180,
        {candidate.box.size.width * input->scale, candidate.box.size.height * input->scale});
    pose.data["confidence"] = candidate.score;
    pose.data["pickable"] = false;
    pose.data["attributes"] = {{"templateId", model->data["id"]},
                               {"requestedScale", candidate.scale},
                               {"actualScale",
                                {candidate.box.size.width * input->scale / fullWidth,
                                 candidate.box.size.height * input->scale / fullHeight}},
                               {"method", "masked-ncc-v1"}};
    items.push_back(std::move(pose.data));
  }
  Record result = poses::make2DList(std::move(items), frame, "px");
  if (!poses::validate(result, why))
    return img::badInput(why, "model");
  out.set("instances", Data::record(std::move(result)));
  return Status::Ok();
}

} // namespace

void registerImageP1Matching(Registry& r) {
  auto op = image_p1::descriptor("image.template_build", "NCC模板构建",
                                 "NCC normalized cross correlation template build pattern");
  op.inputs = {img::imageIn()};
  op.outputs = {img::imageOut("template"), image_p1::recordPort("model", "vision.NCCTemplate.v1")};
  op.params = {image_p1::text("id", "模板ID", "template")};
  op.doc = "构建有灰度变化的原生单通道模板图和小型元数据；模板像素不写JSON，可在多次运行中复用。";
  op.compute = &build;
  r.addOperator(std::move(op));

  op = image_p1::descriptor("image.template_match", "多目标NCC模板定位",
                            "NCC template matching rotation scale multi instance NMS");
  op.inputs = {img::imageIn(), img::imageIn("template"),
               image_p1::recordPort("model", "vision.NCCTemplate.v1")};
  op.outputs = {image_p1::recordPort("instances", poses::kPoses2DType)};
  op.params = {
      image_p1::text("frame", "图像参考帧", "image"),
      img::floatParam("angleMin", "最小角度(°)", 0, "[-180,180]，正值对应图像x轴向y轴旋转。"),
      img::floatParam("angleMax", "最大角度(°)", 0, ""),
      img::floatParam("angleStep", "角度步长(°)", 5, "有限正数。"),
      img::floatParam("scaleMin", "最小尺度", 1, "[0.1,4]。"),
      img::floatParam("scaleMax", "最大尺度", 1, ""),
      img::floatParam("scaleStep", "尺度步长", 0.1, "有限正数。"),
      img::floatParam("minScore", "最低NCC分数", 0.8, "[0,1]，含等号。"),
      img::floatParam("nmsIou", "旋转框NMS阈值", 0.3, "[0,1]，严格大于才抑制。"),
      img::intParam("maxDetections", "输出上限", 20, 1, 4096, "稳定按分数/变体/位置排序。"),
      img::intParam("maxCandidates", "候选上限", 1000, 1, 4096, "全局有界候选队列。")};
  op.doc =
      "掩膜零均值NCC，旋转padding不计入相似度；全图离散角度/尺度搜索、旋转框NMS。无目标返回有效空集合，不做亚像素或轮廓形状匹配。";
  op.compute = &match;
  r.addOperator(std::move(op));
}

} // namespace lyflow::ops
