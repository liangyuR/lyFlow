#include "dts_ops.h"

namespace lyflow::packs::solution_dts {

void registerPackOps(Registry& r) {
  lyflow::dts::registerSealDome(r);
  lyflow::dts::registerPickMetal(r);
  lyflow::dts::registerSealRoot(r);
  lyflow::dts::registerFlush(r);
  lyflow::dts::registerProfileBundle(r);
  r.addRetiredOperator("dts.profile_in", "用 profile.input 提供有明确单位、帧和采样顺序的 Profile");
  r.addRetiredOperator("dts.geometry_to_metric", "连接 DTS 节点发布的米制 Point2D 或 Line2D");
  r.addRetiredOperator("dts.profile_clean", "用 profile.clean 处理原生 Profile");
  r.addRetiredOperator("dts.split_faces", "用 profile.split_faces 分割原生 Profile");
}

} // namespace lyflow::packs::solution_dts
