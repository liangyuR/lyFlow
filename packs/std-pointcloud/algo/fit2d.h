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
  /// 半径软先验，米。radiusPriorSigma > 0 时生效：细化的目标函数多一项
  /// ((r − radiusPrior) / radiusPriorSigma)²，点残差按 distThresh / 2 归一。
  /// 弧长时点的信息压过先验，弧短时半径往 radiusPrior 收。0 = 不加（默认）。
  double radiusPrior = 0.0;
  double radiusPriorSigma = 0.0;
};

/// fitCircle2D 的半径最后是怎么定下来的。
enum class Circle2DRadiusOutcome {
  Free,        ///< 细化后半径在界内，照常
  Prior,       ///< 走了带先验的细化，半径在界内
  ClampedMin,  ///< 细化把半径推到下界以外，钉在下界上只重定圆心
  ClampedMax,  ///< 同上，上界
  Unrefined,   ///< 细化发散（非有限值），用 RANSAC 的原始圆
};

struct Circle2DFitReport {
  Circle2DRadiusOutcome radius = Circle2DRadiusOutcome::Free;
  /// 钉之前细化给出的半径（米）；没钉时等于最终半径。
  double unclampedRadius = 0.0;
};

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

}  // namespace lyflow::std_pc
