#pragma once
// lyflow_std_algo 的签名适配：把老的 detection::utils 调用形状转成 std 的
// options 结构。这里只有参数打包，一行算法都没有（算法在 packs/std-pointcloud/algo/）。
#include <pcl/PointIndices.h>

#include <Eigen/Core>
#include <string>

#include "algo/crop2d.h"
#include "algo/fit2d.h"
#include "domain/detection/DetectionTypes.hpp"

namespace gap_std {

inline lyflow::std_pc::Line2DFitOptions lineOptions(double distThresh) {
  lyflow::std_pc::Line2DFitOptions o;
  o.distThresh = static_cast<float>(distThresh);
  return o;
}

inline lyflow::std_pc::Circle2DFitOptions circleOptions(double distThresh, double minRadius,
                                                        double maxRadius) {
  lyflow::std_pc::Circle2DFitOptions o;
  o.distThresh = static_cast<float>(distThresh);
  o.minRadius = minRadius;
  o.maxRadius = maxRadius;
  return o;
}

inline bool lineFit2D(const PointCloud &cloud, Eigen::VectorXf *line, pcl::Indices *inliers,
                      double distThresh = 0.0001) {
  return lyflow::std_pc::fitLine2D(cloud, line, inliers, lineOptions(distThresh));
}

/// line_type 是 "vertical line" / "horizontal line" 时约束方向，其余按普通拟合。
inline bool lineFit2D(const PointCloud &cloud, Eigen::VectorXf *line, pcl::Indices *inliers,
                      const std::string &lineType, double distThresh) {
  if (lineType != "vertical line" && lineType != "horizontal line") {
    return lineFit2D(cloud, line, inliers, distThresh);
  }
  return lyflow::std_pc::fitAxisLine2D(cloud, line, inliers, lineType == "vertical line",
                                       lineOptions(distThresh));
}

inline bool circleFit2D(const PointCloud &cloud, Eigen::VectorXf *circle, pcl::Indices *inliers,
                        double distThresh = 0.00003, double minRadius = 0.0005,
                        double maxRadius = 0.01, double fixedRadius = 0.0) {
  const auto options = circleOptions(distThresh, minRadius, maxRadius);
  return fixedRadius > 0
             ? lyflow::std_pc::fitCircleFixedRadius2D(cloud, fixedRadius, circle, inliers, options)
             : lyflow::std_pc::fitCircle2D(cloud, circle, inliers, options);
}

/// 四边严格开区间（原 filterCloudByRoi 的语义）。roi.col(0) 是 min 角。
inline bool roiCrop2D(const PointCloud &src, PointCloud *dst, const Eigen::Matrix2f &roi) {
  return lyflow::std_pc::cropBox2D(src, dst, roi, lyflow::std_pc::Bounds2D::Open);
}

}  // namespace gap_std
