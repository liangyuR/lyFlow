#include <array>
#include <cmath>
#include <limits>
#include <vector>

#include "p1_common.h"

namespace lyflow::ops {
namespace {
Param scalar(const char* name, const char* label, double def, double min, double max,
             const char* unit = "") {
  auto p = img::floatParam(name, label, def, "");
  p.min = min;
  p.max = max;
  p.unit = unit;
  return p;
}
cv::Vec<double, 6> basis(double x, double y) { return {1, x, y, x * x, x * y, y * y}; }
Status extractHeight(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  const Image* input = in.get("image").asImage();
  if (!input || !input->consistent() || input->channels != 1 || input->scale != 1 ||
      (input->depth != PixelDepth::F32 && input->depth != PixelDepth::U16))
    return img::badInput("需要全分辨率单通道f32米制或u16深度；缩放输入不能复用原始正交标定");
  if (input->pixelCount() > 2097152)
    return Status::Error(Phase::Execute, "resource_limit", "深度图超过2Mi像素预算");
  if (p.text("frame").empty() || p.text("frame").size() > 1024)
    return img::badParam("参考帧非法", "frame");
  const double dx = p.number("pixelSizeX"), dy = p.number("pixelSizeY"), ox = p.number("originX"),
               oy = p.number("originY");
  const cv::Mat source = cvx::view(*input);
  cv::Mat depth;
  source.convertTo(depth, CV_64F, input->depth == PixelDepth::U16 ? p.number("depthScale") : 1.0);
  cv::Mat observed(input->height, input->width, CV_8U, cv::Scalar(0));
  for (int y = 0; y < depth.rows; ++y)
    for (int x = 0; x < depth.cols; ++x)
      if (std::isfinite(depth.at<double>(y, x)) && depth.at<double>(y, x) > 0)
        observed.at<unsigned char>(y, x) = 255;
  const auto method = p.choice("reference");
  cv::Vec3d normal(0, 0, 1);
  double offset = -p.number("z");
  if (method == "three_points") {
    std::array<cv::Vec3d, 3> points;
    int i = 0;
    for (const char* name : {"point0", "point1", "point2"}) {
      auto xy = p.vec3(name);
      if (!std::isfinite(xy[0]) || !std::isfinite(xy[1]) || xy[0] != std::floor(xy[0]) ||
          xy[1] != std::floor(xy[1]) || xy[0] < 0 || xy[1] < 0 || xy[0] >= depth.cols ||
          xy[1] >= depth.rows || xy[2] != 0)
        return img::badParam("三点坐标必须是有效整像素[x,y,0]", name);
      int x = static_cast<int>(xy[0]), y = static_cast<int>(xy[1]);
      if (!observed.at<unsigned char>(y, x))
        return img::badInput("参考三点必须来自有效深度");
      points[i++] = {ox + dx * x, oy + dy * y, depth.at<double>(y, x)};
    }
    normal = (points[1] - points[0]).cross(points[2] - points[0]);
    const double n = cv::norm(normal);
    if (!std::isfinite(n) || n < 1e-15)
      return img::badParam("参考三点重合或共线", "point2");
    normal /= n;
    if (normal[2] < 0)
      normal = -normal;
    if (normal[2] < 1e-6)
      return img::badParam("参考平面与深度轴平行", "point2");
    offset = -normal.dot(points[0]);
  }
  cv::Mat reference(depth.size(), CV_64F, cv::Scalar(0)), valid = observed.clone();
  if (method == "surface") {
    const double radius = p.number("radius");
    const int stride = static_cast<int>(p.integer("sampleStep")),
              degree = static_cast<int>(p.integer("degree")), terms = degree == 1 ? 3 : 6;
    const double rx = std::ceil(radius / dx), ry = std::ceil(radius / dy);
    if (rx > 100000 || ry > 100000)
      return Status::Error(Phase::Execute, "resource_limit", "局部曲面窗口超过预算");
    const int nx = static_cast<int>(rx), ny = static_cast<int>(ry);
    const std::uint64_t neighbors = (2ULL * nx / stride + 1) * (2ULL * ny / stride + 1);
    if (neighbors * input->pixelCount() > 50000000)
      return Status::Error(Phase::Execute, "resource_limit",
                           "局部多项式曲面计算超过50M邻域采样预算");
    for (int y = 0; y < depth.rows; ++y) {
      if (ctx.cancelled())
        return Status::Ok();
      ctx.progress(static_cast<float>(y) / depth.rows);
      for (int x = 0; x < depth.cols; ++x) {
        cv::Mat a = cv::Mat::zeros(terms, terms, CV_64F), b = cv::Mat::zeros(terms, 1, CV_64F);
        int support = 0;
        for (int yy = std::max(0, y - ny); yy <= std::min(depth.rows - 1, y + ny); yy += stride)
          for (int xx = std::max(0, x - nx); xx <= std::min(depth.cols - 1, x + nx); xx += stride) {
            if (!observed.at<unsigned char>(yy, xx))
              continue;
            double u = (xx - x) * dx / radius, v = (yy - y) * dy / radius, d = u * u + v * v;
            if (d > 1)
              continue;
            const auto q = basis(u, v);
            double weight = std::exp(-2 * d);
            ++support;
            for (int i = 0; i < terms; ++i) {
              b.at<double>(i) += weight * q[i] * depth.at<double>(yy, xx);
              for (int j = 0; j < terms; ++j)
                a.at<double>(i, j) += weight * q[i] * q[j];
            }
          }
        cv::Mat singular;
        cv::SVD::compute(a, singular);
        const bool rank =
            support >= terms && singular.at<double>(terms - 1) > singular.at<double>(0) * 1e-10;
        cv::Mat solution;
        if (!rank || !cv::solve(a, b, solution, cv::DECOMP_SVD) ||
            !std::isfinite(solution.at<double>(0)) || solution.at<double>(0) <= 0) {
          valid.at<unsigned char>(y, x) = 0;
          continue;
        }
        reference.at<double>(y, x) = solution.at<double>(0);
      }
    }
  } else {
    for (int y = 0; y < depth.rows; ++y) {
      if (ctx.cancelled())
        return Status::Ok();
      for (int x = 0; x < depth.cols; ++x) {
        double z = -(normal[0] * (ox + dx * x) + normal[1] * (oy + dy * y) + offset) / normal[2];
        if (!std::isfinite(z) || z <= 0) {
          valid.at<unsigned char>(y, x) = 0;
          continue;
        }
        reference.at<double>(y, x) = z;
      }
    }
  }
  cv::Mat height(depth.size(), CV_32F, cv::Scalar(0)),
      referenceOut(depth.size(), CV_32F, cv::Scalar(0)), gray(depth.size(), CV_8U, cv::Scalar(0));
  double farthest = 0;
  for (int y = 0; y < depth.rows; ++y)
    for (int x = 0; x < depth.cols; ++x) {
      const double ref = reference.at<double>(y, x);
      if (ref > 0 && std::isfinite(ref) && ref <= std::numeric_limits<float>::max())
        referenceOut.at<float>(y, x) = static_cast<float>(ref);
      else
        valid.at<unsigned char>(y, x) = 0;
      if (!valid.at<unsigned char>(y, x))
        continue;
      const double residual =
          (depth.at<double>(y, x) - ref) * (method == "surface" ? 1 : normal[2]);
      if (!std::isfinite(residual) || std::abs(residual) > std::numeric_limits<float>::max())
        return img::badInput("高度超出有限f32范围");
      height.at<float>(y, x) = static_cast<float>(residual);
      farthest = std::max(farthest, std::abs(residual));
    }
  const auto invalid = p.choice("invalid"), direction = p.choice("direction");
  for (int y = 0; y < depth.rows; ++y) {
    if (ctx.cancelled())
      return Status::Ok();
    for (int x = 0; x < depth.cols; ++x) {
      if (!valid.at<unsigned char>(y, x)) {
        double replacement = 0;
        if (invalid == "farthest")
          replacement = farthest;
        else if (invalid == "custom" && reference.at<double>(y, x) > 0)
          replacement = (p.number("customDepth") - reference.at<double>(y, x)) *
                        (method == "surface" ? 1 : normal[2]);
        if (!std::isfinite(replacement) ||
            std::abs(replacement) > std::numeric_limits<float>::max())
          return img::badParam("替代深度超出有限f32范围", "customDepth");
        height.at<float>(y, x) = static_cast<float>(replacement);
      }
      const double value = height.at<float>(y, x), noise = p.number("noise"),
                   gain = p.number("gain") * 1000;
      double pixel = 0;
      if (direction == "high" && value > noise)
        pixel = (value - noise) * gain;
      else if (direction == "low" && value < -noise)
        pixel = (-value - noise) * gain;
      else if (direction == "both") {
        pixel = 128;
        if (value > noise)
          pixel = 128 + (value - noise) * gain;
        else if (value < -noise)
          pixel = 128 + (value + noise) * gain;
      }
      if (!valid.at<unsigned char>(y, x) && invalid == "preserve")
        pixel = 0;
      gray.at<unsigned char>(y, x) =
          static_cast<unsigned char>(std::clamp(std::round(pixel), 0.0, 255.0));
    }
  }
  if (ctx.cancelled())
    return Status::Ok();
  Image h, ref, mask, g;
  if (!cvx::fromMat(height, h) || !cvx::fromMat(referenceOut, ref) || !cvx::fromMat(valid, mask) ||
      !cvx::fromMat(gray, g))
    return img::badInput("无法构造高度输出");
  out.set("height", Data::image(std::move(h)));
  out.set("reference", Data::image(std::move(ref)));
  out.set("mask", Data::image(std::move(mask)));
  out.set("image", Data::image(std::move(g)));
  return Status::Ok();
}
} // namespace
void registerP2HeightExtraction(Registry& r) {
  auto op = image_p1::descriptor("image.height_extract", "正交深度高度提取",
                                 "orthographic depth height plane local polynomial MLS extraction");
  op.capabilities = {true, false, true};
  op.inputs = {img::grayIn()};
  op.outputs = {img::imageOut("height", "米制有符号f32高度；无效值由mask区分"),
                img::imageOut("reference", "f32参考深度，米"),
                img::imageOut("mask", "u8原始深度和参考面联合有效性"),
                img::imageOut("image", "u8高度映射")};
  op.params = {
      image_p1::text("frame", "正交参考帧", "input"),
      scalar("pixelSizeX", "横向物理分辨率", .001, 1e-9, 1e3, "m/px"),
      scalar("pixelSizeY", "纵向物理分辨率", .001, 1e-9, 1e3, "m/px"),
      scalar("originX", "像素0横向坐标", 0, -1e6, 1e6, "m"),
      scalar("originY", "像素0纵向坐标", 0, -1e6, 1e6, "m"),
      scalar("depthScale", "u16深度比例", .001, 1e-12, 1e3, "m"),
      img::enumParam("reference", "参考面", "z_plane",
                     {{"z_plane", "等深平面", ""},
                      {"three_points", "三点平面", ""},
                      {"surface", "局部多项式曲面", ""}},
                     ""),
      scalar("z", "平面深度", 1, 1e-9, 1e6, "m"),
      image_p1::vector("point0", "第一参考像素", {0, 0, 0}, "px"),
      image_p1::vector("point1", "第二参考像素", {1, 0, 0}, "px"),
      image_p1::vector("point2", "第三参考像素", {0, 1, 0}, "px"),
      scalar("radius", "曲面邻域半径", .01, 1e-9, 1e3, "m"),
      img::intParam("degree", "曲面多项式阶数", 2, 1, 2, ""),
      img::intParam("sampleStep", "邻域采样间隔", 1, 1, 1024, ""),
      img::enumParam("invalid", "无效深度填充", "preserve",
                     {{"preserve", "零占位保留无效掩码", ""},
                      {"on_plane", "参考面", ""},
                      {"farthest", "最远残差", ""},
                      {"custom", "指定深度", ""}},
                     "填充始终不改变mask"),
      scalar("customDepth", "替代深度", 1, 1e-9, 1e6, "m"),
      scalar("noise", "平面噪声带", 0, 0, 1e3, "m"),
      scalar("gain", "每毫米灰度增益", 10, 0, 1e6),
      img::enumParam(
          "direction", "高度方向", "both",
          {{"high", "高于参考面", ""}, {"low", "低于参考面", ""}, {"both", "有符号双向", ""}}, "")};
  op.doc =
      "全分辨率正交深度。正高度沿+Z；平面输出有符号法向距离，曲面输出轴向差。low映射为正亮度，both零点128；无效值始终mask=0。";
  op.compute = &extractHeight;
  r.addOperator(std::move(op));
}
} // namespace lyflow::ops
