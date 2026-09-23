#pragma once
// lyflow_std_algo 的公共点类型。用 PointXYZRGB 而不是 PointXYZ：2D 量测域的算法
// 全部从 gap-inspector 迁来（ADR-0015），强度就放在 r 通道上跟着点走。
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>

#include "lyflow/data.h"

namespace lyflow::std_pc {

using Point2DT = pcl::PointXYZRGB;
using Cloud2D = pcl::PointCloud<Point2DT>;

/// LyFlow 点云 → Cloud2D。intensity 进 r/g/b 三个通道（与 gap 的 toPcl 同一约定）。
Cloud2D toCloud2D(const lyflow::PointCloud& cloud);

}  // namespace lyflow::std_pc
