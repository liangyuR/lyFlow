#include "dts_ops.h"

namespace lyflow::packs::solution_dts {

void registerPackOps(Registry& r) {
  lyflow::dts::registerProfileIn(r);
  lyflow::dts::registerSealDome(r);
  lyflow::dts::registerPickMetal(r);
  lyflow::dts::registerSealRoot(r);
  lyflow::dts::registerFlush(r);
  lyflow::dts::registerProfileBundle(r);
  lyflow::dts::registerGeometryAdapter(r);
}

} // namespace lyflow::packs::solution_dts
