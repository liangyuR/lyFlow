#pragma once

#include <string>

#include "lyflow_cv/line_scan.h"

namespace lyflow::packs::solution_glue {

using lyflow::std_image::line_scan::P2;

struct CaliperSpec {
  double searchHalf = 48;  ///< 卡尺沿法向 ±searchHalf（px）
  double caliperWidth = 5; ///< 沿切向平均的宽度（px）
  double innerFrom = -40;  ///< 内边只在 t ∈ [innerFrom, innerTo] 里找
  double innerTo = -3;
  double beadFrom = -15; ///< 胶只在 t ∈ [max(内边 + 1, beadFrom), beadTo] 里找
  double beadTo = 40;
  bool innerDarkToBright = true; ///< 沿法向看内边：开口暗 → 翻边亮（默认）
  bool innerNearest = true;      ///< 有几条内边时取离胶最近的；false = 取最强的
  bool beadDark = true;
  double contrastMin = 25;
  double widthMin = 3;
};

/// 一个站的结果。偏移都是沿法向、相对站点的 t（px）；找不到的是 NaN。
struct CaliperStation {
  std::string status = "ok"; ///< ok / no_inner / no_bead / incomplete_bead / out_of_image
  double inner = 0;
  double nearEdge = 0;
  double farEdge = 0;
  double innerContrast = 0;
  double beadContrast = 0;
};

/// 站点 c、法向 n（单位向量，指向翻边）上量一次。
CaliperStation measureCaliper(const cv::Mat& gray, P2 c, P2 n, const CaliperSpec& spec);

} // namespace lyflow::packs::solution_glue
