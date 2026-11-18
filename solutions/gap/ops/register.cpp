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
  profile_scene::registerCameraSelection(registry);
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
  registerPatrolPolicy(registry);
  registerResultBundle(registry);
  registry.addRetiredOperator("gap.measure_reference",
                              "请在当前版本重新导入 StandardGap 配置，再读取图的测量输出");
  registry.addRetiredOperator("gap.datum_window", "请用 geometry.anchored_window 构造米制窗口");
  registry.addRetiredOperator("gap.camera_guard",
                              "请连接 profile.compare_overlap 与 gap.select_camera");
  registry.addRetiredOperator("gap.judge",
                              "请连接 metrology.compare_tolerance 与 gap.apply_patrol");
  for (const char* id :
       {"gap.fit_line", "gap.flush", "gap.gap", "gap.nearest_to_line", "gap.point_offset",
        "gap.selected_point", "gap.to_measurement_frame", "gap.drop_non_finite"})
    registry.addRetiredOperator(id, "请在当前版本重新导入 StandardGap 配置或按业务目标重新搭图");
#ifdef LYFLOW_GAP_IMPORT
  registerStandardGapImporter(registry);
#endif
  registerSnippets(registry);
}
} // namespace lyflow::packs::gap
namespace lyflow::packs::solution_gap {
void registerPackOps(Registry& registry) { gap::registerPackOps(registry); }
}
