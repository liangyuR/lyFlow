#pragma once
// LyFlow 数据模型 ↔ PCL 的唯一转换点（D2 / ADR-0005）。代价是进出各拷一份，
// 所以 PCL 算子尽量产出 Indices 再走 PointCloud::select 出云。
#include <cstdint>
#include <vector>

#include <pcl/PCLPointCloud2.h>
#include <pcl/PointIndices.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>

#include "lyflow/data.h"

namespace lyflow::ops::adapter {

/// 只搬 xyz。属性通道留在 LyFlow 侧，出结果时用 select 搬回去。
pcl::PointCloud<pcl::PointXYZ>::Ptr toPcl(const PointCloud& cloud);

std::vector<std::int32_t> fromPclIndices(const pcl::PointIndices& indices);
std::vector<std::int32_t> fromPclIndices(const pcl::Indices& indices);

/// PCL 点云 → LyFlow 点云。
PointCloud fromPcl(const pcl::PointCloud<pcl::PointXYZ>& cloud);

/// PCLPointCloud2（读盘的原始形态）→ LyFlow 点云，按字段名认通道。
/// 走 blob 而非具体点类型：文件里有哪些通道只有运行时才知道，模板组合覆盖不全。
PointCloud fromBlob(const pcl::PCLPointCloud2& blob);

/// LyFlow 点云 → PCLPointCloud2，只写实际存在的通道。
void toBlob(const PointCloud& cloud, pcl::PCLPointCloud2& blob);

}  // namespace lyflow::ops::adapter
