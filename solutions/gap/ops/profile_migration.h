#pragma once

#include <stdexcept>

#include "lyflow/manifest.h"

namespace lyflow::packs::gap {
inline nlohmann::json requireGapGraphUpgrade(const nlohmann::json&) {
  throw std::invalid_argument(
      "graph_upgrade_required: 旧 Gap 节点使用裸点云或旧 Bundle/Record 契约，不能原地转换数据类型。"
      "请使用 gapProfile.v1 图级升级请求，显式声明传感器单位、分析平面、坐标系与采样来源，"
      "检查整图升级报告后再运行。");
}
inline void declareGapGraphUpgrade(OperatorDesc& op, int previousMajorCount = 1) {
  for (int major = 1; major <= previousMajorCount; ++major)
    op.migrations.push_back({major, requireGapGraphUpgrade, nullptr});
}
} // namespace lyflow::packs::gap
