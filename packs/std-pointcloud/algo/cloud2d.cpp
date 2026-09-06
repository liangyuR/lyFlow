#include "algo/cloud2d.h"

#include <cmath>

namespace lyflow::std_pc {

Cloud2D toCloud2D(const lyflow::PointCloud& cloud) {
  Cloud2D out;
  out.resize(cloud.pointCount());
  for (std::size_t i = 0; i < cloud.pointCount(); ++i) {
    Point2DT& p = out[i];
    p.x = cloud.xyz[i * 3];
    p.y = cloud.xyz[i * 3 + 1];
    p.z = cloud.xyz[i * 3 + 2];
    const float v = cloud.hasIntensity() ? cloud.intensity[i] : 0.0f;
    const auto level = static_cast<std::uint8_t>(std::isfinite(v) ? std::min(255.0f, std::max(0.0f, v)) : 0.0f);
    p.r = p.g = p.b = level;
  }
  return out;
}

}  // namespace lyflow::std_pc
