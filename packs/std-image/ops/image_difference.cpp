#include <cmath>

#include "ops.h"
#include "params.h"

namespace lyflow::ops {
namespace {

cv::Mat grayFloat(const Image& image) {
  cv::Mat gray;
  if (image.channels == 1)
    gray = cvx::view(image);
  else
    cv::cvtColor(cvx::view(image), gray,
                 image.channels == 3 ? cv::COLOR_RGB2GRAY : cv::COLOR_RGBA2GRAY);
  cv::Mat result;
  gray.convertTo(result, CV_32F);
  return result;
}

Status differenceCompute(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                         ExecContext& ctx) {
  const Image& a = *inputs.get("a").asImage();
  const Image& b = *inputs.get("b").asImage();
  if (!a.consistent() || !b.consistent() || a.width != b.width || a.height != b.height ||
      a.channels != b.channels || a.depth != b.depth)
    return img::badInput("差分图像必须具有相同尺寸、通道和位深", "b");
  if (a.pixelCount() > 4u * 1024u * 1024u)
    return Status::Error(Phase::Execute, "resource_limit", "抗边缘差分仅支持不超过 4 MP 的图像");
  const double threshold = params.number("threshold");
  const double edgeDistance = params.number("edgeDistance");
  if (!std::isfinite(threshold) || threshold < 0 || !std::isfinite(edgeDistance) ||
      edgeDistance < 0)
    return img::badParam("差分阈值及边缘距离必须为非负有限数", "threshold");
  cv::Mat source = grayFloat(a), target = grayFloat(b);
  cv::Mat valid(a.height, a.width, CV_8U, cv::Scalar(255));
  cv::Mat sourceValid = valid.clone();
  Ticker ticker(ctx, a.pixelCount());
  for (int y = 0; y < a.height; ++y)
    for (int x = 0; x < a.width; ++x) {
      if (ticker.tick(static_cast<std::size_t>(y) * a.width + x))
        return Status::Ok();
      if (!std::isfinite(source.at<float>(y, x))) {
        sourceValid.at<unsigned char>(y, x) = 0;
        source.at<float>(y, x) = 0;
      }
      if (!std::isfinite(target.at<float>(y, x))) {
        valid.at<unsigned char>(y, x) = 0;
        target.at<float>(y, x) = 0;
      }
    }
  cv::Point2d shift;
  double response = 1;
  cv::Mat affine = cv::Mat::eye(2, 3, CV_64F);
  cv::Mat aligned = cvx::view(a).clone();
  if (params.choice("alignment") == "translation") {
    if (a.width < 4 || a.height < 4)
      return img::badInput("平移配准需要宽高至少为 4 的图像");
    cv::Mat window;
    cv::createHanningWindow(window, source.size(), CV_32F);
    shift = cv::phaseCorrelate(source.clone(), target.clone(), window, &response);
    const double maxShift = params.number("maxShift");
    const double minResponse = params.number("minResponse");
    if (!std::isfinite(maxShift) || maxShift < 0 || !std::isfinite(minResponse) ||
        minResponse < 0 || minResponse > 1)
      return img::badParam("配准范围和最低响应参数无效", "maxShift");
    if (!std::isfinite(shift.x) || !std::isfinite(shift.y) || !std::isfinite(response) ||
        response < minResponse || std::abs(shift.x) > maxShift || std::abs(shift.y) > maxShift)
      return Status::Error(Phase::Execute, "alignment_failed",
                           "平移配准置信度或位移范围不满足要求，不进行静默退回差分");
    affine.at<double>(0, 2) = shift.x;
    affine.at<double>(1, 2) = shift.y;
    cv::warpAffine(cvx::view(a), aligned, affine, source.size(), cv::INTER_LINEAR,
                   cv::BORDER_CONSTANT, cv::Scalar::all(0));
    cv::Mat warped, warpedValid, coverage;
    cv::warpAffine(source, warped, affine, source.size(), cv::INTER_LINEAR, cv::BORDER_CONSTANT,
                   cv::Scalar(0));
    sourceValid.convertTo(coverage, CV_32F, 1.0 / 255.0);
    cv::warpAffine(coverage, warpedValid, affine, source.size(), cv::INTER_LINEAR,
                   cv::BORDER_CONSTANT, cv::Scalar(0));
    source = std::move(warped);
    sourceValid = warpedValid == 1.0f;
  }
  valid &= sourceValid;
  if (inputs.has("mask")) {
    const Image& mask = *inputs.get("mask").asImage();
    if (mask.width != a.width || mask.height != a.height || mask.channels != 1)
      return img::badInput("差分掩膜必须与目标图像同尺寸且为单通道", "mask");
    for (int y = 0; y < a.height; ++y)
      for (int x = 0; x < a.width; ++x) {
        const double v = mask.at(x, y, 0);
        if (!std::isfinite(v) || v == 0)
          valid.at<unsigned char>(y, x) = 0;
      }
  }
  if (ctx.cancelled())
    return Status::Ok();
  cv::Mat delta = source - target;
  cv::Mat result;
  const std::string change = params.choice("change");
  if (change == "positive")
    result = delta > threshold;
  else if (change == "negative")
    result = delta < -threshold;
  else
    result = cv::abs(delta) > threshold;
  result &= valid;
  if (params.flag("ignoreEdges") && edgeDistance > 0) {
    const double low = params.number("cannyLow"), high = params.number("cannyHigh");
    if (!std::isfinite(low) || !std::isfinite(high) || low < 0 || high < low)
      return img::badParam("Canny 阈值需满足 0 ≤ low ≤ high", "cannyLow");
    cv::Mat s8, t8, se, te, sd, td;
    const double scale = 255.0 / cvx::fullScale(cvx::view(a).depth());
    source.convertTo(s8, CV_8U, scale);
    target.convertTo(t8, CV_8U, scale);
    cv::Canny(s8, se, low, high);
    cv::Canny(t8, te, low, high);
    cv::distanceTransform(255 - se, sd, cv::DIST_L2, cv::DIST_MASK_PRECISE);
    cv::distanceTransform(255 - te, td, cv::DIST_L2, cv::DIST_MASK_PRECISE);
    result.setTo(0, (sd < edgeDistance) & (td < edgeDistance));
  }
  Record info{"image.Difference.v1",
              {{"alignment", params.choice("alignment")},
               {"shift", {shift.x * a.scale, shift.y * a.scale}},
               {"response", response},
               {"coordinateSpace", "source"},
               {"scale", a.scale},
               {"unit", "px"},
               {"validSamples", cv::countNonZero(valid)},
               {"changedSamples", cv::countNonZero(result)}}};
  outputs.set("stats", Data::record(std::move(info)));
  Status status = img::putMat(outputs, "aligned", std::move(aligned));
  if (!status.ok)
    return status;
  return img::putMat(outputs, "mask", std::move(result));
}

} // namespace

void registerImageDifference(Registry& registry) {
  OperatorDesc op;
  op.id = "image.difference_mask";
  op.version = "1.0.0";
  op.label = "抗边缘图像差分";
  op.category = "图像/分割";
  op.doc =
      "先按显式配置进行有置信度和位移上限的平移配准，再做正/负/双向差分；可排除两幅图共同边缘附近噪声。不进行旋转或尺度配准。";
  Port mask = img::grayIn("mask", "可选目标坐标系掩膜。");
  mask.required = false;
  op.inputs = {img::imageIn("a"), img::imageIn("b"), mask};
  op.outputs = {img::imageOut("mask"), img::imageOut("aligned"),
                withContract(Port{"stats", "Record", "配准质量", "图像差分质量。", true},
                             {{"recordType", "image.Difference.v1"}})};
  Param maxShift = img::floatParam("maxShift", "最大平移", 32, "每轴位移绝对值上限。");
  Param distance =
      img::floatParam("edgeDistance", "共同边缘容差", 2, "同时靠近两幅图的边缘时抑制该差异。");
  maxShift.unit = distance.unit = "px";
  op.params = {
      img::enumParam("alignment", "配准", "none",
                     {{"none", "无", "已对齐图像"}, {"translation", "平移", "相位相关"}}, ""),
      maxShift,
      img::floatParam("minResponse", "最低配准响应", 0.1, "配准不满足时明确失败。"),
      img::enumParam("change", "变化方向", "both",
                     {{"both", "双向", "绝对差"},
                      {"positive", "正向", "a 大于 b"},
                      {"negative", "负向", "a 小于 b"}},
                     ""),
      img::floatParam("threshold", "差分阈值", 10, "输入原始强度单位，严格大于。"),
      img::boolParam("ignoreEdges", "抑制共同边缘", false, "不抑制仅在一幅图出现的新边缘。"),
      distance,
      img::floatParam("cannyLow", "Canny 下阈值", 50, "按满量程归一化到 u8 的边缘阈值。"),
      img::floatParam("cannyHigh", "Canny 上阈值", 100, "")};
  op.keywords = {"image difference",
                 "edge tolerant subtraction",
                 "translation registration",
                 "轮廓抗噪",
                 "图像差分",
                 "共同边缘抑制"};
  op.capabilities = {true, false, true};
  op.compute = differenceCompute;
  registry.addOperator(std::move(op));
}

} // namespace lyflow::ops
