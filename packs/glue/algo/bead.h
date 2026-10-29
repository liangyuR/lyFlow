#pragma once
// 涂胶检测的算法内核（glue-plan §2.2 的 D1–D16）。积木算子、doctest、CLI 走的都是这一份（D11）：
// 这里只有纯函数，吃 8 位单通道的 cv::Mat 与参数结构，吐结构体；Record / Bundle 的读写在 ops/ 里。
//
// 两遍做（D1）：findBeadPath 在检测区里粗找胶点、稳健拟合一条跨得过断口的光滑胶路；
// measureStations 再沿这条胶路布站、逐站卡尺精测。断胶 = 胶路上连续若干站无胶。
// 示教胶路（glue-plan §6）不找：resampleTaught 把示教折线变成同形的胶路，measureStations 照样量，
// 只是期望的胶中心从「就在胶路上」换成逐站估出来的偏移轨迹（T3）。
#include <limits>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "field.h"

namespace lyflow::packs::glue {

// ------------------------------------------------------------------ 响应图（D3）

/// 黑顶帽（bright 时白顶帽）：闭运算 − 原图，结构元直径 = widthMax + 1（大于期望最大胶宽），
/// 再做一次 5×5、σ=1.2 的高斯。结构元是近似圆盘的正八边形（方形 + 两条对角线段的闵可夫斯基和），
/// 比 OpenCV 的椭圆核快三十多倍（整幅 1280×1024 约 4 ms 对 120 ms）。
/// roi 是要用到的那一块（图像坐标）；内部按两倍结构元半径外扩着算，roi 里的值与整幅图算出来的相同。
/// level 不为空时另给一张同样高斯过的局部亮度：暗胶取闭运算（胶两旁的背景），亮胶取原图（胶本身），
/// 即顶帽两项里亮的那一项，处处 ≥ 响应。
Field responseField(const cv::Mat& gray, bool bright, int widthMax, cv::Rect roi,
                    Field* level = nullptr);

/// D15 的边缘陡度用的平滑灰度（5×5、σ=1.0）。bright 时取反（255 − g）：陡度的符号约定就只有「暗胶」一种。
Field smoothGrayField(const cv::Mat& gray, bool bright, cv::Rect roi);

/// glue.response：结构元直径、三个 5×5 高斯的 σ 都开放（σ = 0 不平滑）。roi 宽高 ≤ 0 = 整幅。
struct ResponseSpec {
  bool bright = false;
  int diameter = 91;
  cv::Rect roi;
  double responseSigma = 1.2;
  double graySigma = 1.0;
  double levelSigma = 1.2;
};

/// 三张与输入同尺寸的 CV_32F 图，roi 之外是 NaN；roi 里的值与整幅计算逐位相同。
/// response = 顶帽响应，gray = 平滑灰度（bright 时 255 − g），level = 局部亮度（暗胶取闭运算、亮胶取原图）。
struct ResponseMaps {
  cv::Mat response;
  cv::Mat gray;
  cv::Mat level;
};

ResponseMaps responseMaps(const cv::Mat& gray, const ResponseSpec& spec);

// ------------------------------------------------------------------ 卡尺上的暗段（D4）

/// 法向卡尺上的一段「胶」：lo / hi 是两条边相对卡尺中心沿法向的偏移（px），peak 是段内响应的峰值。
struct Run {
  double lo = 0;
  double hi = 0;
  double peak = 0;
  bool completeEdges = true; ///< 合并进来的每一段两边都完整
  bool loComplete = true;    ///< lo 外侧的采样有效、已降到半高以下
  bool hiComplete = true;
  int parts = 1; ///< 合并进来的子段个数
  double width() const { return hi - lo; }
  double mid() const { return 0.5 * (lo + hi); }
};

struct RunSpec {
  double contrastMin = 16; ///< 段内峰值至少这么高才算
  double mergeGap = 8;     ///< 两段之间的缝不超过它、合并后仍不宽于 widthMax 就合并（高光条）
  double widthMin = 4;
  double widthMax = 90;
};

/// 卡尺分段的全部门限（glue-measure-ops §4）。halfMax = 按连通段的半高拆成子段；否则一个连通段一段。
struct RunSplitSpec {
  double segmentMin = 8;
  double peakMin = 16;
  double mergeGap = 8;
  double mergePeakRatio = 0; ///< 0 = 合并不查峰值比
  double mergeWidthMax = 90;
  bool halfMax = false;
};

/// 沿 c + t·n（t ∈ [−half, half]，0.5 px 一步）的全部暗段，按 lo 升序，不按宽度筛。
/// raw 给了就写合并前的子段；validFraction 给了就写有效采样的比例。
std::vector<Run> caliperRuns(const Field& response, P2 c, P2 n, double half,
                             const RunSplitSpec& spec, std::vector<Run>* raw = nullptr,
                             double* validFraction = nullptr);

/// D15 陡度的组成：edgeLo / edgeHi 是两条边内外各 edgeProbe 的灰度差，depth 是暗度。取不到全是 NaN。
struct RunEdges {
  double edgeLo = std::numeric_limits<double>::quiet_NaN();
  double edgeHi = std::numeric_limits<double>::quiet_NaN();
  double depth = std::numeric_limits<double>::quiet_NaN();
  double sharpness = std::numeric_limits<double>::quiet_NaN();
};

RunEdges runEdges(const Field& g, P2 c, P2 n, const Run& run, double edgeProbe = 3,
                  double backgroundProbe = 8);

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

// ------------------------------------------------------------------ 扇形扫描（glue.fan_scan）

struct FanSpec {
  P2 anchor{0, 0};
  double from = -180;
  double to = 180;
  double r0 = 100;
  double r1 = 250;
  double angleStep = 1;
  double radialStep = 2;
  double invalidMax = 0.3;
  int peakCount = 3;
  double peakSeparation = 15;
};

struct FanPeak {
  std::size_t index = 0;
  double angle = 0;
  double score = 0;
};

struct FanResult {
  int samples = 0;
  std::vector<double> angles;
  std::vector<double> scores; ///< NaN = 无效采样太多
  std::vector<int> invalid;
  std::vector<FanPeak> peaks;
};

/// 每个方向一条射线上响应的均值；峰按得分降序贪心取，与已取的角差都 > peakSeparation。
FanResult fanScan(const Field& response, const FanSpec& spec);

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

/// 折线上按 s 线性插值取点与切向（切向重新归一化）。s 超出两端时取端点。
struct PolylineView {
  const std::vector<double>* s = nullptr;
  const std::vector<P2>* points = nullptr;
  const std::vector<P2>* tangents = nullptr;
  bool valid() const {
    return s && points && tangents && !s->empty() && s->size() == points->size() &&
           s->size() == tangents->size();
  }
  void at(double sq, P2* point, P2* tangent) const;
};

// ------------------------------------------------------------------ 示教胶路（T1 / T2）

/// 折线的总长（相邻两点距离之和）。
double polylineLength(const std::vector<P2>& pts);

/// 示教折线（图像 px，从喷嘴一侧往外排）按弧长重采样成与 findBeadPath 同形的胶路：s 从第一个示教点量起，
/// 只取 [zoneStart, zoneEnd] 那一段（两端各一个点），中间每 2 px 一个点；切向是相邻两点的中心差分。
/// 相邻重合的示教点跳过。zoneEnd ≤ 0 表示到折线末端。点不到两个、有非有限值、总长为 0、
/// zone 不在 [0, 总长] 里时返回 false，输出不动。
bool resampleTaught(const std::vector<P2>& pts, double zoneStart, double zoneEnd,
                    std::vector<double>* s, std::vector<P2>* points, std::vector<P2>* tangents);

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

} // namespace lyflow::packs::glue
