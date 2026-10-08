// glue 包的注册入口（glue-plan G2）。LyFlow 的 CMake 生成的 registerOpPacks 调它（ADR-0013）。
#include "glue.h"

namespace lyflow::packs::glue {

void registerPackOps(Registry& r) {
  // Bundle 声明先于用到它的端口（m8-plan L2）。
  registerBundles(r);
  // 顺序即面板里同分类下的排列顺序：一张检测图从左到右
  registerBeadPath(r);
  registerBeadWidth(r);
  registerBeadBreaks(r);
  registerEdgeDistance(r);
  registerJudge(r);
  registerSynthBreak(r);
  // 固定相机飞拍后检（glue-plan §5）：定位 → 逐点卡尺
  registerLocate(r);
  registerStationCalipers(r);
  // 片段引用上面的算子；自检在注册表填满之后才跑
  registerSnippets(r);
}

}  // namespace lyflow::packs::glue
