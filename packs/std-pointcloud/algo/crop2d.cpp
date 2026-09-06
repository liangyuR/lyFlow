#include "algo/crop2d.h"

namespace lyflow::std_pc {

bool cropBox2D(const Cloud2D& src, Cloud2D* dst, const Eigen::Matrix2f& roi, Bounds2D bounds) {
  const Eigen::Vector2f lo = roi.col(0);
  const Eigen::Vector2f hi = roi.col(1);
  for (const auto& p : src) {
    if (insideBox2D(p.x, p.y, lo, hi, bounds)) dst->emplace_back(p);
  }
  return !dst->empty();
}

}  // namespace lyflow::std_pc
