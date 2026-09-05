#pragma once
//
// 内置算子的注册函数声明。每个 src/ops/*.cpp 实现其中一个。
// 由 builtin_ops.cpp 显式调用 —— 原因见 registry.h 里 registerBuiltinOps 的注释。
//
#include "lyflow/registry.h"

namespace lyflow::ops {

void registerIoLoadPcd(Registry& r);
void registerFilterVoxelGrid(Registry& r);
void registerFilterPassthrough(Registry& r);

}  // namespace lyflow::ops
