#pragma once
// 涂胶检测的算法内核（glue-plan §2.2 的 D1–D16）。积木算子、doctest、CLI 走的都是这一份（D11）：
// 这里只有纯函数，吃 8 位单通道的 cv::Mat 与参数结构，吐结构体；Record / Bundle 的读写在 ops/ 里。
//
// 两遍做（D1）：findBeadPath 在检测区里粗找胶点、稳健拟合一条跨得过断口的光滑胶路；
// measureStations 再沿这条胶路布站、逐站卡尺精测。断胶 = 胶路上连续若干站无胶。
// 示教胶路（glue-plan §6）不找：resamplePolyline 把示教折线变成同形的胶路，measureStations 照样量，
// 只是期望的胶中心从「就在胶路上」换成逐站估出来的偏移轨迹（T3）。
#include <limits>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "lyflow_cv/polyline.h"

namespace lyflow::packs::solution_glue {

using lyflow::std_image::line_scan::angleDiffDeg;
using lyflow::std_image::line_scan::caliperRuns;
using lyflow::std_image::line_scan::degOf;
using lyflow::std_image::line_scan::dirOfDeg;
using lyflow::std_image::line_scan::dot;
using lyflow::std_image::line_scan::FanPeak;
using lyflow::std_image::line_scan::FanResult;
using lyflow::std_image::line_scan::fanScan;
using lyflow::std_image::line_scan::FanSpec;
using lyflow::std_image::line_scan::Field;
using lyflow::std_image::line_scan::finite;
using lyflow::std_image::line_scan::kInvalid;
using lyflow::std_image::line_scan::length;
using lyflow::std_image::line_scan::P2;
using lyflow::std_image::line_scan::perp;
using lyflow::std_image::line_scan::responseField;
using lyflow::std_image::line_scan::ResponseMaps;
using lyflow::std_image::line_scan::responseMaps;
using lyflow::std_image::line_scan::ResponseSpec;
using lyflow::std_image::line_scan::Run;
using lyflow::std_image::line_scan::runEdges;
using lyflow::std_image::line_scan::RunSplitSpec;
using lyflow::std_image::line_scan::smoothGrayField;
using lyflow::std_image::line_scan::unit;

struct RunSpec {
  double contrastMin = 16; ///< 段内峰值至少这么高才算
  double mergeGap = 8;     ///< 两段之间的缝不超过它、合并后仍不宽于 widthMax 就合并（高光条）
  double widthMin = 4;
  double widthMax = 90;
};

/// 沿 c + t·n（t ∈ [−half, half]，0.5 px 一步）取响应，响应 > contrastMin / 2 的连成一段、峰值 ≥ contrastMin
/// 才算，按半高找每一段的两条边，缝小的相邻段合并，再按宽度筛。
/// separatePeaks 分离半高子段，合并前的子段保留在 candidates，供示教直胶按已有轨迹消歧。
std::vector<Run> lateralRuns(const Field& response, P2 c, P2 n, double half, const RunSpec& spec,
                             std::vector<Run>* candidates = nullptr, bool separatePeaks = false);

/// 中心离 expect 最近的那一段；没有返回 nullptr。
const Run* nearestRun(const std::vector<Run>& runs, double expect = 0.0);

/// D15 的边缘陡度：min(左右两边各 ±3 px 的灰度差) / 胶的暗度（两侧 8 px 外的均值 − 暗段中间一半的均值）。
/// 取不到（采样落出图外）返回 NaN。g 是 smoothGrayField。
double runSharpness(const Field& g, P2 c, P2 n, const Run& run);

// ------------------------------------------------------------------ 定胶路（D1 / D2 / D6 / D15）

struct PathSpec {
  P2 nozzle{0, 0};
  double zoneStart = 100; ///< D2：检测区离开喷嘴，粗找从这里开始
  double zoneEnd = 400;
  bool bright = false; ///< polarity = bright：胶比背景亮
  int widthMax = 90;   ///< 期望最大胶宽，决定结构元
  bool useHeading =
      false; ///< 给了方向（G3 由机器人经顶层图参数喂进来）就只在 heading ± headingTol 里找
  double headingDeg = 0;
  double headingTolDeg = 15;
  double sectorFromDeg = -180; ///< 没给方向时扇形搜索的范围（图像坐标，y 向下，−90° 朝上）
  double sectorToDeg = 180;
  double minCoverage = 0.3;
  double sharpMin = 0.4;
  double maxGap = 120; ///< 粗找连续没找到胶的最长距离，超过就停，后面的胶路按已有胶点外推
  double contrastMin = 16;
  // 下面几个是参考实现定下的内部常数，不开参数
  double coarseStep = 8;
  double dirWindow = 48;
  double smoothWindow = 40;
  double fanLength = 150;
  double fanStep = 2;
  double fanSeparationDeg = 15;
};

/// 一个试过的方向（D6 的「前三个峰各试一遍」）。
struct PathCandidate {
  double headingDeg = 0;
  double fanScore = 0;   ///< 扇形搜索里这条射线上响应的均值
  double coverage = 0;   ///< 粗找找到胶的步数 / 总步数
  double sharpness = 0;  ///< 各暗段边缘陡度的第 25 百分位（NaN = 暗段不到 3 个）
  bool beadlike = false; ///< sharpness ≥ sharpMin
  double score = 0;      ///< coverage ×（像胶 ? 1 : 0.1）
  int steps = 0;
  std::vector<std::pair<double, P2>> coarse; ///< (s, 胶点)
};

struct BeadPath {
  bool ok = false;
  std::string reason;        ///< 没找到时的原因（给人看的）
  std::string headingSource; ///< param | search
  double headingDeg = 0;     ///< 选中的方向
  double coverage = 0;
  double sharpness = 0;
  bool beadlike = false;
  double residual = 0;  ///< 粗找胶点到胶路的距离中位数（px），没拟合时 NaN
  bool fallback = true; ///< true = 没拟合出胶路，下面的折线是沿选中方向的一条直线
  std::vector<PathCandidate> candidates;
  std::vector<P2> coarse; ///< 选中方向粗找到的胶点
  /// 胶路：s 从喷嘴沿胶路的弧长（第一个点离喷嘴 zoneStart），相邻两点 2 px，覆盖整个检测区
  std::vector<double> s;
  std::vector<P2> points;
  std::vector<P2> tangents; ///< 单位向量，指向远离喷嘴的方向
};

BeadPath findBeadPath(const cv::Mat& gray, const PathSpec& spec);

// ------------------------------------------------------------------ 粗跟踪与拟合（glue.track_path）

/// findBeadPath 的粗找，门限全部开放（glue-measure-ops §3）。默认值即 1.1 的取值。
struct TrackSpec {
  P2 anchor{0, 0};
  double zoneStart = 100;
  double zoneEnd = 400;
  double bearingFrom = -180;
  double bearingTo = 180;
  double bearingMargin = 20;
  double outwardRatio = 0.75;
  double searchHalf = 90;
  RunSplitSpec runs;
  double widthMin = 4;
  double widthMax = 90;
  double acceptOffsetRatio = 0.6;
  double coarseStep = 8;
  double maxGap = 120;
  double gapSteps = 1.5;
  int historyMin = 3;
  double jumpRatio = 0.5;
  double jumpMin = 8;
  double reacquirePeakRatio = 0.5;
  double reacquireWidthRatio = 0.5;
  double steadyIqrRatio = 0.3;
  double sharpMin = 0.4;
  bool rayFallback = true;
  double rayFallbackCoverage = 0.6;
  double edgeProbe = 3;
  double backgroundProbe = 8;
  double dirWindow = 48;
};

struct TrackPoint {
  double s = 0;
  P2 point;
  double mid = 0; ///< 相对预测点（或退回的射线点）沿法向的偏移
  double width = 0;
  double peak = 0;
  double sharpness = std::numeric_limits<double>::quiet_NaN();
  bool onRay = false;
};

struct TrackResult {
  int steps = 0;
  std::vector<TrackPoint> points;
  double coverage = 0;
  double sharpness = std::numeric_limits<double>::quiet_NaN(); ///< 可算陡度不到 3 个时 NaN
  double stoppedAt = std::numeric_limits<double>::quiet_NaN();
};

TrackResult trackBead(const Field& response, const Field& g, const TrackSpec& spec,
                      double headingDeg);

/// 胶点 (s, 点) → 光滑胶路：局部线性稳健拟合（半窗 fitWindow）、σ = fitSigma 的高斯平滑（0 = 不平滑），
/// 按弧长重采样：第一个点离 anchor 恰好 zoneStart，每 2 px 一点到 zoneEnd。residual 是胶点到胶路的距离中位数。
void fitBeadPath(const std::vector<std::pair<double, P2>>& coarse, P2 anchor, double zoneStart,
                 double zoneEnd, double coarseStep, double fitWindow, double fitSigma,
                 std::vector<double>* s, std::vector<P2>* points, std::vector<P2>* tangents,
                 double* residual);

/// 从 anchor 沿 headingDeg 的直线，同样覆盖 [zoneStart, zoneEnd]、每 2 px 一点。
void straightBeadPath(P2 anchor, double headingDeg, double zoneStart, double zoneEnd,
                      std::vector<double>* s, std::vector<P2>* points, std::vector<P2>* tangents);

using lyflow::std_image::inspection::PolylineView;

// ------------------------------------------------------------------ 量胶宽（D4 / D5 / D16）

struct StationSpec {
  bool swirl = false; ///< form = swirl：胶两边取窗口内所有暗段的并集（外包络，D5）
  double step = 4;
  double searchHalf = 60;
  double window = 30;
  double presentRatio = 0.5;
  double centerRatio = 0.3;
  /// 一站的峰值至少是参考峰值（候选暗段峰值的中位数）的这么多倍；示教胶路比的是峰值 / 局部亮度
  double contrastRatio = 0.3;
  /// 示教胶路（T3）：≥ 0 时胶中心离示教线最远这么多 px，期望的胶中心按「偏移轨迹」逐站估（不是 0）。
  /// < 0 = 胶路是 findBeadPath 从图里拟合出来的，胶就在胶路上，期望中心恒为 0（原来的做法，一位不差）。
  double lateralTol = -1;
  /// 示教胶路的 D15（T5）：轨迹上各站暗段边缘陡度的第 25 百分位低于它就算没有胶（像压痕、阴影）。
  double sharpMin = 0.4;
  RunSpec runs;
};

struct Station {
  double s = 0;
  P2 c, t, n; ///< 胶路上的点、切向、法向
  bool present = false;
  double lo = 0, hi = 0; ///< 胶两边沿法向的偏移（px），只在 present 时有意义
  double peak = 0;       ///< 对比度：这一站胶的响应峰值
  double candidateLo = std::numeric_limits<double>::quiet_NaN();
  double candidateHi = std::numeric_limits<double>::quiet_NaN();
  std::vector<Run> candidates;
  std::vector<Run> runs; ///< 这一站卡尺上的全部暗段
  P2 left() const { return c + n * lo; }
  P2 right() const { return c + n * hi; }
  double width() const { return hi - lo; }
  double mid() const { return 0.5 * (lo + hi); }
};

struct StationResult {
  std::vector<Station> stations;
  double wRef = 0;    ///< D16 的参考宽：全检测区候选暗段宽度的中位数（示教胶路：只取轨迹上的站）
  double peakRef = 0; ///< 参考峰值：同一批候选暗段峰值的中位数
  double envelopeHalf = 0; ///< 螺旋胶的包络半宽（直胶是 0）
  /// 每站期望的胶中心（沿法向相对胶路的偏移，px）。从图里拟合的胶路恒为 0；示教胶路是偏移轨迹（T3）。
  std::vector<double> expect;
  // 下面几个只在示教胶路（spec.lateralTol ≥ 0）时有意义，否则是 NaN / 空
  double trackSupport = std::numeric_limits<double>::quiet_NaN(); ///< 落在偏移轨迹上的站占比
  double trackSharpness =
      std::numeric_limits<double>::quiet_NaN(); ///< 轨迹上暗段边缘陡度的 P25（T5）
  double trackOffset =
      std::numeric_limits<double>::quiet_NaN(); ///< 轨迹上各站期望中心的中位数（px）
  std::string trackReason;                      ///< 沿示教线没认出胶时的原因（空 = 认出了）
};

/// pathOk = false（没找到胶）时一站都不量，全部判无胶 —— 「没有胶」本身就是结果（§2.3 失败语义）。
/// 示教胶路（spec.lateralTol ≥ 0）沿示教线没认出胶（T4 / T5）时同样全部判无胶，原因写在 trackReason。
StationResult measureStations(const cv::Mat& gray, const PolylineView& path, bool pathOk,
                              double zoneStart, double zoneEnd, bool bright, int responseWidthMax,
                              const StationSpec& spec);

// ------------------------------------------------------------------ 小工具

double median(std::vector<double> v);
double percentile(std::vector<double> v, double q);
/// 一组点的主方向（PCA），朝向与 fallback 同侧。不到 3 个点返回 fallback。
P2 principalDirection(const std::vector<P2>& pts, P2 fallback);

} // namespace lyflow::packs::solution_glue
