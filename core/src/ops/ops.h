#pragma once
// core 自带的两个算子（S1 / ADR-0014）：合成数据源是测试基础设施，
// reroute 是编辑器语义。点云算法一律在 packs/std-pointcloud/。
#include "lyflow/operator.h"
#include "lyflow/registry.h"

namespace lyflow::ops {

void registerGenSynthetic(Registry& r);
void registerUtilReroute(Registry& r);
void registerFlowOps(Registry& r);

}  // namespace lyflow::ops
