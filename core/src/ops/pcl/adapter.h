#pragma once
//
// LyFlow 数据模型 ↔ PCL 的唯一转换点（D2 / ADR-0005）。
//
// 这里是整个 core 里少数几个见得到 `pcl::` 的地方之一。核心数据模型不焊死在
// `pcl::PointCloud<PointXYZ>` 上的理由有两条：
//
//   1. 编译时间。include/ 里出现一个 PCL 头，整棵依赖树就都要吃 Eigen 的模板，
//      「加一个算子」的反馈循环从秒级变成分钟级。
//   2. M5 要接 Image 域（OpenCV）。数据模型如果是 PCL 的形状，那时要么塞不进去，
//      要么长成一个「点云的特例 + 图像的特例」的怪物。
//
// 代价是每次进出 PCL 都要拷一份。这是明码标价的：**PCL 算子的产出尽量是
// Indices 而不是点云**，再用 PointCloud::select 出云 —— 这样拷贝只有一次，
// 而且属性通道（intensity/normals/rgb）由 select 统一搬运，不会被 PCL 的
// PointXYZ 悄悄吃掉。
//
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
///
/// 走 blob 而不是 `pcl::PointCloud<PointXYZI>` 之类的具体点类型，是因为
/// 「文件里到底有哪些通道」只有运行时才知道。用具体点类型就得为每种组合
/// 实例化一份模板（XYZ / XYZI / XYZRGB / PointNormal / ...），编译时间爆炸，
/// 而且组合起来还是覆盖不全。
PointCloud fromBlob(const pcl::PCLPointCloud2& blob);

/// LyFlow 点云 → PCLPointCloud2，只写实际存在的通道。
void toBlob(const PointCloud& cloud, pcl::PCLPointCloud2& blob);

}  // namespace lyflow::ops::adapter
