#include "dts_ops.h"

namespace lyflow::packs::dts {

void registerPackOps(Registry& r) {
  lyflow::dts::registerProfileIn(r);
  lyflow::dts::registerProfileClean(r);
  lyflow::dts::registerSplitFaces(r);
  lyflow::dts::registerSealDome(r);
  lyflow::dts::registerPickMetal(r);
  lyflow::dts::registerSealRoot(r);
  lyflow::dts::registerFlush(r);
  lyflow::dts::registerProfileBundle(r);
}

}  // namespace lyflow::packs::dts
