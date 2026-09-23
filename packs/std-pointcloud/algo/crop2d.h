#pragma once
// XY 平面上的盒裁剪。open 是 gap 的 filterCloudByRoi 语义（四边严格不等），
// 非有限点因为比较恒假而一并丢掉 —— 下游的拟合因此不必再防 NaN。
#include <Eigen/Core>

#include "algo/cloud2d.h"

namespace lyflow::std_pc {

enum class Bounds2D { Open, Closed };

inline bool insideBox2D(float x, float y, const Eigen::Vector2f& lo, const Eigen::Vector2f& hi,
                        Bounds2D bounds) {
  if (bounds == Bounds2D::Open) {
    return x > lo[0] && y > lo[1] && x < hi[0] && y < hi[1];
  }
  return x >= lo[0] && y >= lo[1] && x <= hi[0] && y <= hi[1];
}

/// 复刻 detection::utils::filterCloudByRoi：框内的点**追加**到 dst，返回 dst 非空。
/// roi.col(0) 是 min 角，roi.col(1) 是 max 角。
bool cropBox2D(const Cloud2D& src, Cloud2D* dst, const Eigen::Matrix2f& roi,
               Bounds2D bounds = Bounds2D::Open);

}  // namespace lyflow::std_pc
