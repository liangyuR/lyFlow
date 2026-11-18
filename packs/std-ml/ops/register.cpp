// std-ml 包的注册入口（ADR-0015）。onnxruntime 的依赖只在这个包里。
#include "ops.h"

namespace lyflow::packs::std_ml {

void registerPackOps(Registry& r) {
  ops::registerYoloDecode(r);
  ops::registerYoloxDecode(r);
  ops::registerOcrDecode(r);
#if defined(LYFLOW_ML_RUNTIME)
  ops::registerMlOnnxRun(r);
#endif
}

} // namespace lyflow::packs::std_ml
