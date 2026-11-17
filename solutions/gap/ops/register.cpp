#include "gap_ops.h"
#include "profile_bundle_ops.h"
#include "profile_circles.h"
#include "profile_guard.h"
#include "profile_feature_bundle.h"
#include "profile_inspection.h"
#include "profile_measurement_ops.h"
#include "profile_model_ops.h"
#include "profile_regions.h"
#include "profile_scan.h"
#include "profile_scene.h"
#ifdef LYFLOW_GAP_TEMPLATE
#include "profile_locate_template.h"
#include "profile_template.h"
#endif

namespace lyflow::packs::gap {
void registerPackOps(Registry& registry) {
  profile_scene::registerProfileBundles(registry);
  profile_scene::registerProfileFeatureBundles(registry);
  profile_scene::registerProfileScanOps(registry);
  profile_scene::registerProfileCameraGuard(registry);
  profile_scene::registerProfileRegionOps(registry);
  profile_scene::registerProfileBundleOps(registry);
  profile_model::registerProfileModelOps(registry);
#ifdef LYFLOW_GAP_TEMPLATE
  profile_template::registerProfileTemplateOps(registry);
  profile_template::registerProfileLocateTemplate(registry);
#endif
  profile_scene::registerProfileGapCircles(registry);
  profile_scene::registerProfileMeasurementOps(registry);
  profile_scene::registerProfileInspection(registry);
  registerJudge(registry);
  registerResultBundle(registry);
#ifdef LYFLOW_GAP_COMPAT
  registerMeasureReference(registry);
#endif
#ifdef LYFLOW_GAP_IMPORT
  registerStandardGapImporter(registry);
#endif
  registerSnippets(registry);
}
} // namespace lyflow::packs::gap
namespace lyflow::packs::solution_gap {
void registerPackOps(Registry& registry) { gap::registerPackOps(registry); }
}
