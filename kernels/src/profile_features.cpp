#include "lyflow/kernel/profile_features.h"

#include "lyflow/kernel/metrology.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace lyflow::kernel::profile_features {
namespace {
namespace robust = profile_robust;
constexpr double kPi = 3.14159265358979323846;
constexpr double kScale = 1000.0;

void checkCancellation(const robust::CancelCheck& cancelled) {
  if (cancelled && cancelled())
    throw robust::Cancelled();
}

std::array<double, 2> canonicalDirection(double dx, double dy) {
  const double len = std::hypot(dx, dy);
  if (!(len > 0))
    return {1, 0};
  std::array<double, 2> u{dx / len, dy / len};
  if (u[0] < 0 || (u[0] == 0 && u[1] < 0)) {
    u[0] = -u[0];
    u[1] = -u[1];
  }
  return u;
}

double angleDelta(double ax, double ay, double bx, double by) {
  const double delta = (std::atan2(ay, ax) - std::atan2(by, bx)) * 180.0 / kPi;
  double result = std::fmod(delta, 180.0);
  if (result > 90.0)
    result -= 180.0;
  if (result <= -90.0)
    result += 180.0;
  return result;
}

double projection(const robust::Point& point, const std::array<double, 2>& direction) {
  return point.x * direction[0] + point.y * direction[1];
}

bool fixedDirection(const std::vector<robust::Point>& points, double dx, double dy, float distance,
                    robust::LineFit& result, const robust::CancelCheck& cancelled) {
  const auto count = points.size();
  if (count < 2 || !(std::hypot(dx, dy) > 0))
    return false;
  const auto direction = canonicalDirection(dx, dy);
  const double nx = -direction[1];
  const double ny = direction[0];
  std::vector<double> projected;
  projected.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    if ((i & 1023U) == 0)
      checkCancellation(cancelled);
    projected.push_back(points[i].x * nx + points[i].y * ny);
  }
  const auto median = [](std::vector<double> values) {
    std::nth_element(values.begin(),
                     values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2), values.end());
    return values[values.size() / 2];
  };
  double offset = median(projected);
  std::vector<double> kept;
  std::vector<int> inliers;
  for (int pass = 0; pass < 2; ++pass) {
    checkCancellation(cancelled);
    kept.clear();
    inliers.clear();
    for (std::size_t i = 0; i < count; ++i) {
      if ((i & 1023U) == 0)
        checkCancellation(cancelled);
      if (std::fabs(projected[i] - offset) <= distance) {
        inliers.push_back(static_cast<int>(i));
        kept.push_back(projected[i]);
      }
    }
    if (kept.size() < 2) {
      std::vector<std::size_t> order(count);
      for (std::size_t i = 0; i < count; ++i)
        order[i] = i;
      const auto half = std::max<std::size_t>(2, count / 2);
      std::partial_sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(half),
                        order.end(), [&](std::size_t a, std::size_t b) {
                          return std::fabs(projected[a] - offset) <
                                 std::fabs(projected[b] - offset);
                        });
      order.resize(half);
      std::sort(order.begin(), order.end());
      for (const auto index : order) {
        inliers.push_back(static_cast<int>(index));
        kept.push_back(projected[index]);
      }
    }
    offset = median(kept);
  }
  const double x = offset * nx, y = offset * ny;
  if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > std::numeric_limits<float>::max() ||
      std::abs(y) > std::numeric_limits<float>::max())
    return false;
  result.coefficients = {static_cast<float>(x),
                         static_cast<float>(y),
                         0,
                         static_cast<float>(direction[0]),
                         static_cast<float>(direction[1]),
                         0};
  result.inliers = std::move(inliers);
  return true;
}

bool roiIntersections(const Bounds& bounds, const std::array<float, 6>& coefficients,
                      std::array<float, 2>& start, std::array<float, 2>& end) {
  const std::array<std::array<float, 2>, 4> corners{
      bounds.min, {bounds.min[0], bounds.max[1]}, bounds.max, {bounds.max[0], bounds.min[1]}};
  const std::array<double, 2> linePoint{coefficients[0], coefficients[1]};
  const std::array<double, 2> direction{coefficients[3], coefficients[4]};
  const auto reference = metrology::canonicalNormalReference(direction);
  std::array<double, 4> distances{};
  for (std::size_t i = 0; i < corners.size(); ++i) {
    metrology::SignedPointLine projection;
    std::string why;
    if (!metrology::signedPointLine({corners[i][0], corners[i][1]}, linePoint, direction, reference,
                                    metrology::DirectionPolicy::Normalize, projection, why))
      return false;
    distances[i] = projection.distance;
  }
  std::array<std::array<float, 2>, 2> intersections{};
  std::size_t count = 0;
  for (std::size_t i = 0; i < corners.size(); ++i) {
    const auto& a = corners[i];
    const auto& b = corners[(i + 1) % corners.size()];
    if (distances[i] * distances[(i + 1) % corners.size()] < 0 && count < intersections.size()) {
      if (a[0] == b[0]) {
        const double x = a[0];
        const double y =
            (x - coefficients[0]) / coefficients[3] * coefficients[4] + coefficients[1];
        intersections[count] = {static_cast<float>(x), static_cast<float>(y)};
      } else {
        const double y = a[1];
        const double x =
            (y - coefficients[1]) / coefficients[4] * coefficients[3] + coefficients[0];
        intersections[count] = {static_cast<float>(x), static_cast<float>(y)};
      }
      ++count;
    }
  }
  if (count != 2 || !std::isfinite(intersections[0][0]) || !std::isfinite(intersections[1][0]))
    return false;
  start = intersections[0];
  end = intersections[1];
  return true;
}

LineQuality quality(const std::vector<robust::Point>& points, std::size_t slotCount,
                    const robust::LineFit& fit, const robust::CancelCheck& cancelled) {
  LineQuality out;
  out.pointCount = slotCount;
  out.inlierCount = fit.inliers.size();
  out.inlierRatio = static_cast<double>(fit.inliers.size()) / slotCount;
  const auto& coefficients = fit.coefficients;
  out.linePointMm = {coefficients[0] * kScale, coefficients[1] * kScale};
  const double directionNorm = std::hypot(coefficients[3], coefficients[4]);
  out.lineDirection = {coefficients[3] / directionNorm, coefficients[4] / directionNorm};
  const double dx = coefficients[3];
  const double dy = coefficients[4];
  const double norm = std::hypot(dx, dy);
  double squareSum = 0;
  double worst = 0;
  for (std::size_t i = 0; i < fit.inliers.size(); ++i) {
    if ((i & 1023U) == 0)
      checkCancellation(cancelled);
    const auto& point = points[static_cast<std::size_t>(fit.inliers[i])];
    const double residual =
        std::fabs((point.x - coefficients[0]) * dy - (point.y - coefficients[1]) * dx) / norm *
        kScale;
    squareSum += residual * residual;
    worst = std::max(worst, residual);
  }
  out.rmsResidualMm = std::sqrt(squareSum / static_cast<double>(fit.inliers.size()));
  out.maxResidualMm = worst;
  return out;
}

void validate(robust::Samples samples, const LineOptions& options) {
  if ((options.type != LineType::Free && options.type != LineType::Vertical &&
       options.type != LineType::Horizontal) ||
      (options.directionMode != DirectionMode::Free &&
       options.directionMode != DirectionMode::Band &&
       options.directionMode != DirectionMode::Fixed) ||
      (options.endpoints != Endpoints::RoiIntersection &&
       options.endpoints != Endpoints::InlierEnds))
    throw std::invalid_argument("Invalid line feature mode");
  if ((samples.size != 0 && samples.data == nullptr) ||
      samples.size > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    throw std::invalid_argument("Invalid profile feature samples");
  if (!std::isfinite(options.fit.distThresh) || options.fit.distThresh <= 0 ||
      options.fit.maxIterations < 1 || !std::isfinite(options.toward[0]) ||
      !std::isfinite(options.toward[1]) || !std::isfinite(options.nominalDeg) ||
      !std::isfinite(options.toleranceDeg) || options.toleranceDeg < 0)
    throw std::invalid_argument("Invalid line feature options");
  if (options.directionMode != DirectionMode::Free &&
      (!options.hasReference || !std::isfinite(options.referenceDirection[0]) ||
       !std::isfinite(options.referenceDirection[1]) ||
       !(std::hypot(options.referenceDirection[0], options.referenceDirection[1]) > 0)))
    throw std::invalid_argument("A finite nonzero reference direction is required");
  if (options.endpoints == Endpoints::RoiIntersection &&
      (!std::isfinite(options.roi.min[0]) || !std::isfinite(options.roi.min[1]) ||
       !std::isfinite(options.roi.max[0]) || !std::isfinite(options.roi.max[1]) ||
       options.roi.min[0] > options.roi.max[0] || options.roi.min[1] > options.roi.max[1]))
    throw std::invalid_argument("Invalid profile feature ROI");
}

} // namespace

bool fitLine(robust::Samples samples, const LineOptions& options, LineFeature& out,
             LineFailure* failure, const robust::CancelCheck& cancelled) {
  checkCancellation(cancelled);
  validate(samples, options);
  if (failure)
    *failure = {};
  const auto fail = [&](LineFailureStage stage, std::size_t inlierCount = 0) {
    if (failure)
      *failure = {stage, inlierCount};
    return false;
  };
  std::vector<robust::Point> points;
  std::vector<int> slots;
  points.reserve(samples.size);
  slots.reserve(samples.size);
  for (std::size_t i = 0; i < samples.size; ++i) {
    if ((i & 1023U) == 0)
      checkCancellation(cancelled);
    const auto& point = samples.data[i];
    if (std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z)) {
      points.push_back(point);
      slots.push_back(static_cast<int>(i));
    }
  }
  if (points.size() < 2)
    return fail(LineFailureStage::InsufficientSamples, points.size());
  const robust::Samples finite{points.data(), points.size(), samples.dense};
  double pinX = 1;
  double pinY = 0;
  if (options.hasReference) {
    const double angle = std::atan2(options.referenceDirection[1], options.referenceDirection[0]) +
                         options.nominalDeg * kPi / 180.0;
    pinX = std::cos(angle);
    pinY = std::sin(angle);
  }
  robust::LineFit fit;
  bool fitted = options.type == LineType::Free
                    ? robust::fitLine(finite, fit, options.fit, cancelled)
                    : robust::fitAxisLine(finite, options.type == LineType::Vertical, fit,
                                          options.fit, cancelled);
  if (!fitted && options.directionMode == DirectionMode::Free)
    return fail(LineFailureStage::InitialFit);
  LineFeature result;
  result.initialInlierCount = fit.inliers.size();
  if (fitted && options.segmentPoints > 0 && options.segmentPoints < fit.inliers.size()) {
    const auto direction = canonicalDirection(fit.coefficients[3], fit.coefficients[4]);
    const double target = options.toward[0] * direction[0] + options.toward[1] * direction[1];
    std::stable_sort(fit.inliers.begin(), fit.inliers.end(), [&](int a, int b) {
      return std::fabs(projection(points[static_cast<std::size_t>(a)], direction) - target) <
             std::fabs(projection(points[static_cast<std::size_t>(b)], direction) - target);
    });
    fit.inliers.resize(options.segmentPoints);
    std::sort(fit.inliers.begin(), fit.inliers.end());
    result.segmentApplied = true;
    std::vector<robust::Point> segment;
    segment.reserve(fit.inliers.size());
    for (const auto index : fit.inliers)
      segment.push_back(points[static_cast<std::size_t>(index)]);
    robust::LineFit second;
    auto segmentOptions = options.fit;
    segmentOptions.distThresh = options.fit.distThresh / 3;
    if (!robust::fitLine({segment.data(), segment.size(), samples.dense}, second, segmentOptions,
                         cancelled)) {
      if (options.directionMode == DirectionMode::Free)
        return fail(LineFailureStage::SegmentRefit);
      fitted = false;
    } else {
      std::vector<int> mapped;
      mapped.reserve(second.inliers.size());
      for (const auto index : second.inliers)
        mapped.push_back(fit.inliers[static_cast<std::size_t>(index)]);
      fit.coefficients = second.coefficients;
      fit.inliers = std::move(mapped);
    }
  } else if (fitted && options.segmentPoints > 0) {
    result.segmentSkipped = true;
  }
  if (fitted && fit.inliers.empty()) {
    if (options.directionMode == DirectionMode::Free)
      return fail(LineFailureStage::EmptyInliers);
    fitted = false;
  }
  bool pinned = options.directionMode == DirectionMode::Fixed;
  if (options.directionMode == DirectionMode::Band && !pinned)
    pinned = !fitted || std::fabs(angleDelta(fit.coefficients[3], fit.coefficients[4], pinX,
                                             pinY)) > options.toleranceDeg;
  if (pinned && !fixedDirection(points, pinX, pinY, options.fit.distThresh, fit, cancelled))
    return fail(LineFailureStage::FixedFit);
  if (!pinned && !fitted)
    return fail(LineFailureStage::InitialFit);
  if (fit.inliers.size() < options.minInliers)
    return fail(LineFailureStage::MinInliers, fit.inliers.size());
  if (fit.inliers.empty())
    return fail(LineFailureStage::EmptyInliers);
  if (!std::all_of(fit.coefficients.begin(), fit.coefficients.end(),
                   [](float value) { return std::isfinite(value); }) ||
      !(std::hypot(fit.coefficients[3], fit.coefficients[4]) > 0))
    return fail(LineFailureStage::InvalidResult);
  std::sort(fit.inliers.begin(), fit.inliers.end());
  const auto direction = canonicalDirection(fit.coefficients[3], fit.coefficients[4]);
  const double target = options.toward[0] * direction[0] + options.toward[1] * direction[1];
  std::size_t first = 0;
  std::size_t last = 0;
  std::size_t inner = 0;
  for (std::size_t k = 1; k < fit.inliers.size(); ++k) {
    if ((k & 1023U) == 0)
      checkCancellation(cancelled);
    const double projected =
        projection(points[static_cast<std::size_t>(fit.inliers[k])], direction);
    if (projected < projection(points[static_cast<std::size_t>(fit.inliers[first])], direction))
      first = k;
    if (projected > projection(points[static_cast<std::size_t>(fit.inliers[last])], direction))
      last = k;
    if (std::fabs(projected - target) <
        std::fabs(projection(points[static_cast<std::size_t>(fit.inliers[inner])], direction) -
                  target))
      inner = k;
  }
  result.coefficients = fit.coefficients;
  result.point = {fit.coefficients[0], fit.coefficients[1]};
  result.direction = {static_cast<float>(direction[0]), static_cast<float>(direction[1])};
  result.directionPinned = pinned;
  if (options.endpoints == Endpoints::InlierEnds) {
    const auto& a = points[static_cast<std::size_t>(fit.inliers[first])];
    const auto& b = points[static_cast<std::size_t>(fit.inliers[last])];
    result.hasSegment = true;
    result.start = {a.x, a.y};
    result.end = {b.x, b.y};
  } else {
    result.hasSegment = roiIntersections(options.roi, fit.coefficients, result.start, result.end);
    result.roiIntersectionMissing = !result.hasSegment;
  }
  result.innerSlot = slots[static_cast<std::size_t>(fit.inliers[inner])];
  result.quality = quality(points, samples.size, fit, cancelled);
  const auto allFinite = [](const auto& values) {
    return std::all_of(values.begin(), values.end(),
                       [](auto value) { return std::isfinite(value); });
  };
  if (!allFinite(result.point) || !allFinite(result.direction) ||
      (result.hasSegment && (!allFinite(result.start) || !allFinite(result.end))) ||
      !allFinite(result.quality.linePointMm) || !allFinite(result.quality.lineDirection) ||
      !std::isfinite(result.quality.rmsResidualMm) || !std::isfinite(result.quality.maxResidualMm))
    return fail(LineFailureStage::InvalidResult);
  result.inliers.reserve(fit.inliers.size());
  for (const auto index : fit.inliers)
    result.inliers.push_back(slots[static_cast<std::size_t>(index)]);
  checkCancellation(cancelled);
  out = std::move(result);
  return true;
}

} // namespace lyflow::kernel::profile_features
