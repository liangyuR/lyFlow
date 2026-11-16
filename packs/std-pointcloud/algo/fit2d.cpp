#include "algo/fit2d.h"

#include <vector>

namespace lyflow::std_pc {
namespace {
namespace robust = kernel::profile_robust;

std::vector<robust::Point> coordinates(const Cloud2D& cloud) {
  std::vector<robust::Point> points;
  points.reserve(cloud.size());
  for (const auto& p : cloud)
    points.push_back({p.x, p.y, p.z});
  return points;
}

template <std::size_t N>
void copyCoefficients(const std::array<float, N>& input, Eigen::VectorXf* output) {
  output->resize(static_cast<Eigen::Index>(N));
  std::copy(input.begin(), input.end(), output->data());
}
} // namespace

bool fitLine2D(const Cloud2D& cloud, Eigen::VectorXf* line, pcl::Indices* inliers,
               const Line2DFitOptions& options) {
  const auto points = coordinates(cloud);
  robust::LineFit result;
  if (!robust::fitLine({points.data(), points.size(), cloud.is_dense}, result, options))
    return false;
  copyCoefficients(result.coefficients, line);
  *inliers = std::move(result.inliers);
  return true;
}

bool fitAxisLine2D(const Cloud2D& cloud, Eigen::VectorXf* line, pcl::Indices* inliers,
                   bool vertical, const Line2DFitOptions& options) {
  const auto points = coordinates(cloud);
  robust::LineFit result;
  if (!robust::fitAxisLine({points.data(), points.size(), cloud.is_dense}, vertical, result,
                           options))
    return false;
  copyCoefficients(result.coefficients, line);
  *inliers = std::move(result.inliers);
  return true;
}

bool fitCircle2D(const Cloud2D& cloud, Eigen::VectorXf* circle, pcl::Indices* inliers,
                 const Circle2DFitOptions& options, Circle2DFitReport* report) {
  const auto points = coordinates(cloud);
  robust::CircleFit result;
  if (!robust::fitCircle({points.data(), points.size(), cloud.is_dense}, result, options))
    return false;
  copyCoefficients(result.coefficients, circle);
  *inliers = std::move(result.inliers);
  if (report)
    *report = result.report;
  return true;
}

bool fitCircleFixedRadius2D(const Cloud2D& cloud, double fixedRadius, Eigen::VectorXf* circle,
                            pcl::Indices* inliers, const Circle2DFitOptions& options) {
  const auto points = coordinates(cloud);
  robust::CircleFit result;
  if (!robust::fitCircleFixedRadius({points.data(), points.size(), cloud.is_dense}, fixedRadius,
                                    result, options))
    return false;
  copyCoefficients(result.coefficients, circle);
  *inliers = std::move(result.inliers);
  return true;
}

} // namespace lyflow::std_pc
