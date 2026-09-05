#pragma once
//
// 内置算子的注册函数声明。每个 .cpp 实现其中一个。
// 由 builtin_ops.cpp 显式调用 —— 原因见 registry.h 里 registerBuiltinOps 的注释。
//
// 目录约定（D2）：
//   src/ops/*.cpp      手写实现，**不许 include 任何 PCL 头**
//   src/ops/pcl/*.cpp  PCL 实现，经 adapter 拷贝进出
// 这条线是「加算子秒级反馈」的保障：改一个手写算子不会触发 PCL 那堆头文件重编。
//
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

/// 长循环里的取消轮询 + 进度上报。
///
/// 每个点都问一次 cancelled() 会让原子读成为热点循环里的瓶颈，
/// 所以按块问。块大小 8192 是个不用调的经验值：百万点的算子最多晚 8192 个点
/// 才响应取消，人感觉不到；而原子读的开销被摊到千分之一。
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
