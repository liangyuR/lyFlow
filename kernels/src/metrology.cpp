#include "lyflow/kernel/metrology.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "lyflow/calibration.h"

namespace lyflow::kernel::metrology {

bool signedPointLine(const std::array<double, 2>& point, const std::array<double, 2>& linePoint,
                     const std::array<double, 2>& lineDir,
                     const std::array<double, 2>& positiveNormal, DirectionPolicy policy,
                     SignedPointLine& output, std::string& why) {
  for (const auto& values : {point, linePoint, lineDir, positiveNormal})
    for (double value : values)
      if (!std::isfinite(value)) {
        why = "点线坐标、方向与正法向参考必须有限";
        return false;
      }
  const double norm = std::hypot(lineDir[0], lineDir[1]);
  if (!std::isfinite(norm) || norm == 0 ||
      (policy == DirectionPolicy::DeclaredUnit && std::abs(norm - 1) > 1e-5) ||
      (policy != DirectionPolicy::Normalize && policy != DirectionPolicy::DeclaredUnit)) {
    why = "直线方向为零、溢出或不符合声明的单位方向策略";
    return false;
  }
  std::array<double, 2> normal{-lineDir[1], lineDir[0]};
  if (policy == DirectionPolicy::Normalize) {
    normal[0] /= norm;
    normal[1] /= norm;
  }
  const double referenceScale = std::max(std::abs(positiveNormal[0]), std::abs(positiveNormal[1]));
  if (referenceScale == 0) {
    why = "正法向参考不能为零";
    return false;
  }
  const double alignment = positiveNormal[0] / referenceScale * normal[0] +
                           positiveNormal[1] / referenceScale * normal[1];
  if (alignment == 0) {
    why = "正法向参考不能与直线法向正交";
    return false;
  }
  if (alignment < 0) {
    normal[0] = -normal[0];
    normal[1] = -normal[1];
  }
  SignedPointLine result;
  result.distance = (point[0] - linePoint[0]) * normal[0] + (point[1] - linePoint[1]) * normal[1];
  result.foot = {point[0] - result.distance * normal[0], point[1] - result.distance * normal[1]};
  if (!std::isfinite(result.distance) || !std::isfinite(result.foot[0]) ||
      !std::isfinite(result.foot[1])) {
    why = "点线有符号距离或垂足超出有限范围";
    return false;
  }
  output = result;
  why.clear();
  return true;
}

bool sampledGapFlush(const SampledLine& left, const SampledLine& right,
                     const SampledGapFlushOptions& options, SampledGapFlush& output,
                     std::string& why) {
  const double x = options.referenceX;
  if (!std::isfinite(x) || !std::isfinite(options.maxResidual) || options.maxResidual <= 0 ||
      !std::isfinite(options.maxExtrapolation) || options.maxExtrapolation < 0) {
    why = "剖面区间/外推/质量参数非法";
    return false;
  }
  for (const auto& line : {left, right}) {
    if (!std::isfinite(line.minimum) || !std::isfinite(line.maximum) ||
        line.minimum > line.maximum || !std::isfinite(line.rms) || line.rms < 0 ||
        !std::isfinite(line.center[0]) || !std::isfinite(line.center[1]) ||
        !std::isfinite(line.direction[0]) || !std::isfinite(line.direction[1]) ||
        line.rms > options.maxResidual || std::abs(line.direction[0]) < 1e-8 ||
        x < line.minimum - options.maxExtrapolation ||
        x > line.maximum + options.maxExtrapolation) {
      why = "拟合质量、竖直线或外推距离不满足门限";
      return false;
    }
  }
  const auto ordinate = [x](const SampledLine& line) {
    return line.center[1] + (x - line.center[0]) * line.direction[1] / line.direction[0];
  };
  const double gap = right.minimum - left.maximum, flush = ordinate(right) - ordinate(left);
  if (!std::isfinite(gap) || !std::isfinite(flush)) {
    why = "剖面计算超出范围";
    return false;
  }
  output = {gap, flush};
  why.clear();
  return true;
}

bool calibratedDistance(const Record* record, const Point2D* a, const Point2D* b,
                        const std::string& imageFrame, Measurement& out, std::string& why,
                        std::string& port) {
  const auto fail = [&](const std::string& message, const char* input) {
    why = message;
    port = input;
    return false;
  };
  calibration::Plane plane;
  std::string diagnostic;
  if (!record || !calibration::parsePlane(*record, plane, diagnostic))
    return fail(diagnostic.empty() ? "缺少平面标定" : diagnostic, "calib");
  if (!plane.imageFrame.empty() && plane.imageFrame != imageFrame)
    return fail("版本化标定的图像帧必须匹配量测点的明确声明", "calib");
  if (!a || !b || a->unit != Unit2D::Pixel || b->unit != Unit2D::Pixel)
    return fail("物理量测输入必须是原图像素Point2D", "a");
  double ax, ay, bx, by;
  if (!plane.map(a->p[0], a->p[1], ax, ay) || !plane.map(b->p[0], b->p[1], bx, by))
    return fail("量测点位于单应性无效区域", "a");
  Measurement measurement;
  measurement.value = std::hypot(ax - bx, ay - by) * plane.metersPerUnit;
  measurement.ok = std::isfinite(measurement.value);
  measurement.unit = "m";
  if (!measurement.ok)
    return fail("物理量测结果非有限", "a");
  out = std::move(measurement);
  why.clear();
  port.clear();
  return true;
}

void ResidualAccumulator::add(double distance, double weight) noexcept {
  squaredSum_ += weight * distance * distance;
  weightSum_ += weight;
  maximum_ = std::max(maximum_, distance);
}

double ResidualAccumulator::weightedRmse() const noexcept {
  return std::sqrt(squaredSum_ / weightSum_);
}

double ResidualAccumulator::maxError() const noexcept { return maximum_; }

} // namespace lyflow::kernel::metrology
