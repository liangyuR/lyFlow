#pragma once
// 固定相机飞拍后检的算法内核（glue-plan §5，K2 / K3）：模板定位与名义胶路上的逐点卡尺。
// 与 bead.h 同样只有纯函数，吃 8 位单通道 cv::Mat；Record 的读写在 ops/ 里。
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "field.h"

namespace lyflow::packs::glue {

// ------------------------------------------------------------------ 定位（K2）

/// 示教图 → 当前图的刚体位姿：T(p) = R(angle)·(p − teachCenter) + center。
/// 图像坐标 y 向下，angle > 0 是屏幕上的顺时针。
struct Pose2D {
  bool ok = false;
  double score = 0;
  double angleDeg = 0;
  /// 最终角度的搜索窗里，主峰之外最高的独立峰（突起度 ≥ 0.02 的局部极大）/ 主峰分数；没有别的独立峰时为 0。
  /// 主峰自己的侧坡再宽也不算。接近 1 说明搜索区里还有一处几乎一样像的地方，位置不唯一。主峰分数 ≤ 0 时为 1。
  double peakRatio = 1;
  /// 失败原因码：成功时为空，否则是 low_score / ambiguous / at_search_edge / at_angle_edge /
  /// flat_template / out_of_image 之一。
  std::string reason;
  P2 teachCenter{0, 0};
  P2 center{0, 0};

  P2 rotate(P2 v) const;
  P2 apply(P2 p) const;
};

struct LocateSpec {
  P2 anchor{0, 0};          ///< 模板左上角像素在示教图上的位置
  double searchRadius = 80; ///< 模板中心最多偏开这么远（px）；峰离窗边 ≤ 1 px 判贴边，实际可用约 searchRadius − 2
  double angleRange = 3;    ///< 角度扫描 ±range（度）
  double angleStep = 0.5;
  double minScore = 0.6;
  double maxPeakRatio = 1; ///< peakRatio 超过它判 ambiguous；1 = 不判
};

/// 在 anchor 附近找模板。只在「模板外扩 searchRadius 与旋转余量」那一块里算。
/// 角度在 ±angleRange 外各多扫一步（护栏），细扫后抛物线插值。
/// 失败时 ok = false，reason 给原因码，why 写给人看的原因。几道门按顺序判，先命中的为准：
/// 模板没纹理（flat_template）；示教框离图边不到 2 px、搜索区落出图外或比模板还小（out_of_image）；
/// 分数低于 minScore（low_score）；主峰离搜索窗边 ≤ 1 px（at_search_edge）；细扫后的转角超出 ±angleRange
/// 且这个模板看得出角度（at_angle_edge；圆孔这类旋转对称的模板各角度分数差不出来，不判，角度夹回范围内）；
/// peakRatio 超过 maxPeakRatio（ambiguous）。贴边的峰说明真值可能在扫描范围之外，分数再高也不可信。
/// searchRadius 太小（1 左右）时窗口里没有不贴边的位置，每帧都判 at_search_edge；算子把它的下限设成 3。
Pose2D locateTemplate(const cv::Mat& gray, const cv::Mat& tmpl, const LocateSpec& spec,
                      std::string* why = nullptr);

// ------------------------------------------------------------------ 逐点卡尺（K3）

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

} // namespace lyflow::packs::glue
