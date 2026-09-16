#pragma once
// std-pointcloud 包内算子的注册函数声明，由 register.cpp 显式调用。
// 算子实现留在 lyflow::ops 命名空间，与它们还在 core 里时逐字不变。
#include "lyflow/operator.h"
#include "lyflow/registry.h"

namespace lyflow::ops {

// -- 手写 -------------------------------------------------------------------
void registerFilterPassthrough(Registry& r);
void registerFilterVoxelGrid(Registry& r);
void registerFilterCropBox(Registry& r);
void registerFilterRandomSample(Registry& r);
void registerSegmentExtractIndices(Registry& r);
void registerTransformMake(Registry& r);
void registerTransformApply(Registry& r);
void registerUtilMerge(Registry& r);
void registerEditTranslateRegion(Registry& r);

// -- PCL --------------------------------------------------------------------
void registerIoLoadPcd(Registry& r);
void registerIoSavePcd(Registry& r);
void registerFilterStatisticalOutlier(Registry& r);
void registerFilterRadiusOutlier(Registry& r);
void registerSegmentRansacPlane(Registry& r);
void registerFeaturesNormals(Registry& r);

// -- 2D 量测域（ADR-0015）。算法在包内 algo/，经 lyflow_std_algo 也给别的包用 ---
void registerFitLine2D(Registry& r);
void registerFitCircle2D(Registry& r);
void registerRegisterIcp2D(Registry& r);
void registerFilterCropBox2D(Registry& r);

/// 把一片点云写到磁盘（按扩展名选 PCD 或 PLY）。装进 core 的 setCloudWriter，
/// C ABI 的 lyflow_output_save 与 CLI 的 `lyflow dump` 都经它落盘。
Status saveCloudToFile(const PointCloud& cloud, const std::filesystem::path& file,
                       const std::string& format);

}  // namespace lyflow::ops
