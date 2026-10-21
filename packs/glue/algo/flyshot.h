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
  /// 这个模板在这一帧上看不看得出角度。false：各角度的分数差不出来（圆孔这类旋转对称的模板，或真实图上
  /// 角度约束太弱的小窗），angleDeg 报 0，center 是 0° 上最好的平移 —— 位姿只补了平移、没补转角。
  /// 例外：最高分落在护栏角度上、又比 0° 明显高时判 at_angle_edge（ok = false），这时 angleDeg 是细扫出的角度。
  /// angleRange = 0（只找平移）时不扫角度，恒为 true。
  bool angleObservable = true;
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

/// 匹配前预平滑核的半径（px，= ⌈4σ⌉，σ = 1）。templatePad 不小于它时模板框边上的像素平滑时用的全是真实邻域。
constexpr int kLocateSmoothRadius = 4;

struct LocateSpec {
  P2 anchor{0, 0};          ///< 示教框（内框，不含外圈）左上角像素在示教图上的位置
  /// 模板图带的外圈宽度（px）：tmpl = 示教框四边各外扩 templatePad 从示教图上裁的一块，示教框 = tmpl 去掉四边各
  /// templatePad。外圈只给预平滑当真实邻域用，匹配、各道门、teachCenter 都只按示教框算。0 = 模板就是示教框（旧图）。
  int templatePad = 0;
  double searchRadius = 80; ///< 模板中心最多偏开这么远（px）；峰离窗边 ≤ 1 px 判贴边，实际可用约 searchRadius − 2
  double angleRange = 3;    ///< 角度扫描 ±range（度）
  double angleStep = 0.5;
  double minScore = 0.6;
  double maxPeakRatio = 1; ///< peakRatio 超过它判 ambiguous；1 = 不判
};

/// 在 anchor 附近找模板。只在「模板外扩 searchRadius 与旋转余量」那一块里算。
/// 搜索区与模板先做同样的 σ = 1 px 高斯平滑，每个角度（含 0°）都经同一条三次插值旋转再算 NCC，
/// 模板连外圈整块平滑后裁回示教框：templatePad ≥ kLocateSmoothRadius 时示教框里每个像素与图上同一处平滑出来的一样；
/// 外圈不够（含 0）时缺的邻域按 tmpl 边上的像素延伸（BORDER_REPLICATE），示教框外 1.5 px 内有强边时平移偏到 0.6 px（见 .cpp）。
/// 噪声不会把角度推离 0。扫角度时先在 0° 上找一次平移，各角度把转过的图挪到同一个亚像素相位上，
/// 并按两个方向三点抛物线插值到亚像素的峰顶比分（score 仍报整像素上的值），半像素的平移不会把角度推离 0。
/// 角度在 ±angleRange 里按 angleStep 粗扫（angleRange 小于 angleStep 时按 angleRange 一步），两端外各多扫一个
/// 护栏角度 ±(n·step + angleStep)（范围再小，护栏也比端点多出一个 angleStep）；粗扫最好的角度附近按 step/4 细扫，
/// 最好的细扫角度与左右两个做抛物线（落在细扫窗口两端时往外再补一个）。
/// 各角度分数差不出来时角度报 0、angleObservable = false，位置取 0° 上最好的平移（与 angleRange = 0 相同）。
/// angleRange = 0 与看不出角度时最终匹配不挪相位：物体落在半像素上时中心有约 0.1 px 的峰值锁定偏差（1.0.0 也有）。
/// 失败时 ok = false，reason 给原因码，why 写给人看的原因。几道门按顺序判，先命中的为准：
/// tmpl 不比 2·templatePad 大（没有示教框，最先判，out_of_image）；模板没纹理（flat_template）；示教框离图边不到 max(2, templatePad) px
/// （带外圈的模板放不进这一帧）、搜索区落出图外或比模板还小（out_of_image）；
/// 分数低于 minScore（low_score）；主峰离搜索窗边 ≤ 1 px（at_search_edge）；at_angle_edge：看得出角度时
/// 细扫后的转角超出 ±angleRange，看不出角度时最高分落在护栏角度上、又比 0° 高出差值门限的 1/8（angleDeg 报细扫出的角度）；
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
