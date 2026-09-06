// std-ml 包的注册入口（ADR-0015）。onnxruntime 的依赖只在这个包里。
#include "ops.h"

namespace lyflow::packs::std_ml {

void registerPackOps(Registry& r) {
  ops::registerMlOnnxRun(r);
}

}  // namespace lyflow::packs::std_ml
