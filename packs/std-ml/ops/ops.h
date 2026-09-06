#pragma once
// std-ml 包内算子的注册函数声明，由 register.cpp 显式调用。
#include "lyflow/operator.h"
#include "lyflow/registry.h"

namespace lyflow::ops {

void registerMlOnnxRun(Registry& r);

}  // namespace lyflow::ops
