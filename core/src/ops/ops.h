#pragma once
// 内置算子的注册函数声明，由 builtin_ops.cpp 显式调用。
// 目录约定（D2）：src/ops/*.cpp 不许 include PCL 头，PCL 实现一律放 src/ops/pcl/。
#include "lyflow/operator.h"
#include "lyflow/registry.h"

namespace lyflow::ops {

// -- 手写 -------------------------------------------------------------------
void registerGenSynthetic(Registry& r);
void registerFilterPassthrough(Registry& r);
void registerFilterVoxelGrid(Registry& r);
void registerFilterCropBox(Registry& r);
void registerFilterRandomSample(Registry& r);
void registerSegmentExtractIndices(Registry& r);
void registerTransformMake(Registry& r);
void registerTransformApply(Registry& r);
void registerUtilMerge(Registry& r);

// -- PCL --------------------------------------------------------------------
void registerIoLoadPcd(Registry& r);
void registerIoSavePcd(Registry& r);
void registerFilterStatisticalOutlier(Registry& r);
void registerFilterRadiusOutlier(Registry& r);
void registerSegmentRansacPlane(Registry& r);
void registerFeaturesNormals(Registry& r);

/// 长循环里的取消轮询 + 进度上报。按 8192 个点问一次：
/// 每点一次会让原子读成为热点循环的瓶颈，而晚 8192 个点响应取消人感觉不到。
class Ticker {
 public:
  Ticker(ExecContext& ctx, std::size_t total) : ctx_(ctx), total_(total ? total : 1) {}

  /// 返回 true 表示应当立刻退出。
  bool tick(std::size_t i) {
    if ((i & 0x1FFF) != 0) return false;
    if (ctx_.cancelled()) return true;
    ctx_.progress(static_cast<float>(static_cast<double>(i) / static_cast<double>(total_)));
    return false;
  }

 private:
  ExecContext& ctx_;
  std::size_t total_;
};

}  // namespace lyflow::ops
