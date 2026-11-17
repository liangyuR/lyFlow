// GAP 旧图的离线迁移规则 gapProfile.v1（lyflow migrate）：把一套已知 StandardGap 组件
// 改写成原生 Profile 场景节点。规则校验见 graph_migration_validate.h，改写见 graph_migration_rewrite.h。
#include "gap_ops.h"
#include "graph_migration_validate.h"

namespace lyflow::packs::gap {
void registerGraphMigrations(Registry& r) {
  r.addGraphMigration(
      {"gapProfile.v1",
       {},
       [](nlohmann::json& level, const nlohmann::json& rule, nlohmann::json* outer) {
         graph_migration_rules::upgrade(level, rule, outer);
       }});
  for (const char* id :
       {"gap.fit_line", "gap.flush", "gap.gap", "gap.nearest_to_line", "gap.point_offset",
        "gap.selected_point", "gap.to_measurement_frame", "gap.drop_non_finite"})
    r.addRetiredOperator(id, "运行 lyflow migrate <图> --request <gapProfile.v1 请求文件> --write，"
                             "请求写法见 docs/gap-profile-migration.md");
}
} // namespace lyflow::packs::gap
