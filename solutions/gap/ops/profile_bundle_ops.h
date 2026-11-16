#pragma once

#include "lyflow/operator.h"
#include "profile_scene.h"

namespace lyflow::packs::gap::profile_scene {
Status seamTowardGeometry(const RoiView& rois, Data& box, Record& context, ExecContext& ctx);
void registerProfileBundleOps(Registry& registry);
}
