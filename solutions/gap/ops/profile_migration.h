#pragma once

#include <stdexcept>

#include "lyflow/manifest.h"

namespace lyflow::packs::gap {
inline nlohmann::json requireGapGraphUpgrade(const nlohmann::json&) {
  throw std::invalid_argument(
      "旧 Gap 节点使用裸点云或历史 Bundle/Record 契约。请在当前版本重新导入 StandardGap 配置，"
      "或按原业务目标与明确的单位、分析平面和坐标帧重新搭图。");
}
inline void declareGapGraphUpgrade(OperatorDesc& op, int previousMajorCount = 1) {
  for (int major = 1; major <= previousMajorCount; ++major)
    op.migrations.push_back({major, requireGapGraphUpgrade, nullptr});
}
} // namespace lyflow::packs::gap
