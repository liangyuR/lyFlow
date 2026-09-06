#pragma once
// 2D 剖面的法线估计。源码迁自 xyz-gap-inspector 的 gap_core/ProfileGeometry，
// 命名空间改为 lyflow::std_pc，函数名改成 LyFlow 的 lowerCamel（T3）。
#include <Eigen/Core>

#include <optional>
#include <vector>

namespace lyflow::std_pc {

struct PointToPlaneJacobian2D {
  double translationX = 0;
  double translationY = 0;
  double rotation = 0;
};

/// 邻域的最小特征向量。点少于 2 个返回 nullopt。
std::optional<Eigen::Vector2d> estimateNormalFromNeighborhood(
    const std::vector<Eigen::Vector2d>& neighborhood);

/// 逐点 knn 邻域的法线。点少于 2 个时全是零向量。
std::vector<Eigen::Vector2d> estimateProfileNormals(const std::vector<Eigen::Vector2d>& points,
                                                    int knn);

PointToPlaneJacobian2D computePointToPlaneJacobian(const Eigen::Vector2d& point,
                                                   const Eigen::Vector2d& normal);

}  // namespace lyflow::std_pc
