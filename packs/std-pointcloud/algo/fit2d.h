#pragma once
// XY 平面上的直线与圆拟合。PCL 调用与 xyz-gap-inspector 的 GapUtils::fitLine /
// fitCircle 逐字对应（T3）—— 参数外露，但默认值就是那边的值。
#include <pcl/PointIndices.h>

#include <Eigen/Core>

#include "algo/cloud2d.h"
#include "lyflow/kernel/profile_robust.h"

namespace lyflow::std_pc {

using Line2DFitOptions = kernel::profile_robust::Line2DFitOptions;
using Circle2DFitOptions = kernel::profile_robust::Circle2DFitOptions;
using Circle2DRadiusOutcome = kernel::profile_robust::Circle2DRadiusOutcome;
using Circle2DFitReport = kernel::profile_robust::Circle2DFitReport;

/// 双迹线感知的直线拟合。line 是 6 维 PCL 直线系数（点 + 方向），
/// inliers 是按 distThresh 重收的内点（升序）。点太少返回 false。
bool fitLine2D(const Cloud2D& cloud, Eigen::VectorXf* line, pcl::Indices* inliers,
               const Line2DFitOptions& options = {});

/// 反复拟合直到方向合适（vertical = |dx| < |dy|）。复刻 GapUtils 的 line_type 重载。
bool fitAxisLine2D(const Cloud2D& cloud, Eigen::VectorXf* line, pcl::Indices* inliers,
                   bool vertical, const Line2DFitOptions& options = {});

/// SACMODEL_CIRCLE2D + RANSAC。circle 是 3 维（cx, cy, r）。内点少于 3 个返回 false。
///
/// 与 PCL SACSegmentation（optimize on）逐步相同，只有一处不同：细化（LM，或带先验的细化）
/// 把半径推出 [minRadius, maxRadius] 时，SACSegmentation 拿越界的圆重收内点会收到 0 个、
/// 整个拟合失败 —— RANSAC 找到的界内圆就这样被扔掉。这里改成把半径钉在越过的那个界上、
/// 只重定圆心，再收一次内点。细化后半径本来就在界内的输入，结果与 SACSegmentation 逐位相同。
bool fitCircle2D(const Cloud2D& cloud, Eigen::VectorXf* circle, pcl::Indices* inliers,
                 const Circle2DFitOptions& options = {}, Circle2DFitReport* report = nullptr);

/// 先按 fitCircle2D 拟合，再用定半径最小二乘重定圆心并重筛内点。
bool fitCircleFixedRadius2D(const Cloud2D& cloud, double fixedRadius, Eigen::VectorXf* circle,
                            pcl::Indices* inliers, const Circle2DFitOptions& options = {});

} // namespace lyflow::std_pc
