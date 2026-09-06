#pragma once
// XY 平面上的直线与圆拟合。PCL 调用与 xyz-gap-inspector 的 GapUtils::fitLine /
// fitCircle 逐字对应（T3）—— 参数外露，但默认值就是那边的值。
#include <pcl/PointIndices.h>

#include <Eigen/Core>

#include "algo/cloud2d.h"

namespace lyflow::std_pc {

struct Line2DFitOptions {
  /// 内点判定距离，米。假设选择阶段用它的 1/3（双迹线剖面靠这个隔开两条轨迹）。
  float distThresh = 0.0001F;
  int maxIterations = 1000;
  bool optimize = true;
};

struct Circle2DFitOptions {
  float distThresh = 0.00003F;
  double minRadius = 0.0005;
  double maxRadius = 0.01;
  int maxIterations = 10000;
  bool optimizeCoefficients = true;
};

/// 双迹线感知的直线拟合。line 是 6 维 PCL 直线系数（点 + 方向），
/// inliers 是按 distThresh 重收的内点（升序）。点太少返回 false。
bool fitLine2D(const Cloud2D& cloud, Eigen::VectorXf* line, pcl::Indices* inliers,
               const Line2DFitOptions& options = {});

/// 反复拟合直到方向合适（vertical = |dx| < |dy|）。复刻 GapUtils 的 line_type 重载。
bool fitAxisLine2D(const Cloud2D& cloud, Eigen::VectorXf* line, pcl::Indices* inliers,
                   bool vertical, const Line2DFitOptions& options = {});

/// SACMODEL_CIRCLE2D + RANSAC。circle 是 3 维（cx, cy, r）。内点少于 3 个返回 false。
bool fitCircle2D(const Cloud2D& cloud, Eigen::VectorXf* circle, pcl::Indices* inliers,
                 const Circle2DFitOptions& options = {});

/// 先按 fitCircle2D 拟合，再用定半径最小二乘重定圆心并重筛内点。
bool fitCircleFixedRadius2D(const Cloud2D& cloud, double fixedRadius, Eigen::VectorXf* circle,
                            pcl::Indices* inliers, const Circle2DFitOptions& options = {});

}  // namespace lyflow::std_pc
