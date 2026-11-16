#pragma once
#include <cstdint>
#include "lyflow/operator.h"

namespace lyflow::pcl_support {
struct RadiusSelection {
  Indices kept;
  Indices removed;
};
Status radiusOutlier(const PointCloud& cloud, double radius, std::int64_t minNeighbors,
                     RadiusSelection& out, ExecContext& ctx);
}
