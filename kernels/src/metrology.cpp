#include "lyflow/kernel/metrology.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "lyflow/calibration.h"

namespace lyflow::kernel::metrology {

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
