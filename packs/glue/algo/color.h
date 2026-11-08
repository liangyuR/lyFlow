#pragma once
// 彩色胶条的颜色模型与彩色跟踪（docs/glue-measure-ops.md「1.3 彩色测量算子」）。纯函数：吃 RGB 的 cv::Mat 与参数结构，
// 吐结构体；Record / Bundle 的读写在 ops/color_*.cpp 里。
//
// 特征是对数色度 c = (ln R/G, ln B/G)（各通道先做 σ = blurSigma 的高斯，压掉去马赛克的假色），与照明强弱无关。
// 胶是一个二维高斯，背景是 ≤ 3 个高斯，各带一个平均亮度（R + G + B）；每个像素按亮度加一项各向同性的噪声 (noiseK / Y)²。
// 得分 ∈ [0, 1] 是胶的面积占比估计：取马氏距离最近的「胶—背景」连线，在归一化色度 (R, B) / (R + G + B) 上解混出
// 胶占的光通量 β，再按两者的亮度比 ρ = I胶 / I背景 换成面积 α = β / (β + ρ(1 − β))。亮胶压在暗铸件上时，
// 色度在 25 % 面积处就过了中点；按面积算，半高边落在 50 % 处，胶宽不偏。离胶与所有连线都远的颜色另有一道门（gate）压低。
// 三态：任一通道 ≥ satLevel（膨胀 satDilate）是高光，色度无信息；亮度 < darkRatio × 胶的亮度 P5 是暗背景（得分 0）；
// 其余噪声 > noiseMax 的是暗区，也无信息。无信息的像素得分是 NaN。
#include <array>
#include <limits>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>

#include "bead.h"
#include "field.h"

namespace lyflow::packs::glue {

/// ColorMaps.state 的取值。
enum class ColorState : int { Info = 0, Highlight = 1, Dark = 2, DarkBackground = 3 };

/// 建模与出图共用的像素级参数，存在模型里（训练与生产用同一份）。
struct ColorSpec {
  double blurSigma = 0.8;    ///< 求色度前各通道的高斯 σ（px）；0 = 不平滑
  double satLevel = 250;     ///< 任一通道（平滑前）≥ 它即高光
  int satDilate = 2;         ///< 高光掩膜膨胀半径（px）
  double noiseK = 4;         ///< 色度噪声的标准差 ≈ noiseK / 亮度
  double noiseMax = 0.25;    ///< 噪声大于它的像素是暗区（亮度 < noiseK / noiseMax）
  double darkRatio = 0.5;    ///< 亮度 < darkRatio × 胶亮度 P5 判暗背景（得分 0），不看噪声
  double chromaFloor = 0.03; ///< 各分量协方差的下限（标准差，对数色度）
  double glueGate = 4;       ///< 离胶与全部「胶—背景」连线的马氏距离超过它，得分被压低
};

/// 二维高斯：cov = [xx, xy, yy]；intensity = 这一类像素 R + G + B 的均值。
struct Gauss2 {
  double weight = 1;
  std::array<double, 2> mean{0, 0};
  std::array<double, 3> cov{0, 0, 0};
  double intensity = 0;
};

/// 可相加的样本统计：胶是矩（截尾之后），背景是对数色度的二维直方图（bin 0.04，范围 ±2.5，每格另记 R + G + B 之和），
/// 胶的亮度是 4 级一格的直方图。
struct ColorStats {
  static constexpr int kChromaBins = 125;
  static constexpr double kChromaRange = 2.5;
  static constexpr int kLumaBins = 64;
  double glueN = 0;
  std::array<double, 2> glueSum{0, 0};
  std::array<double, 3> glueSq{0, 0, 0};
  double glueSumRGB = 0; ///< 胶样本 R + G + B 之和
  std::vector<double> glueLuma = std::vector<double>(kLumaBins, 0.0);
  double bgN = 0;
  std::vector<double> bgHist = std::vector<double>(kChromaBins * kChromaBins, 0.0);
  std::vector<double> bgSumRGB = std::vector<double>(kChromaBins * kChromaBins, 0.0);
  double bgGlueLike = 0; ///< 背景带里像胶、没进直方图的样本数
  double saturated = 0;  ///< 两条带里高光的样本数
  double dark = 0;       ///< 两条带里暗区的样本数
  int frames = 0;
  void add(const ColorStats& o);
};

struct ColorModel {
  ColorSpec spec;
  Gauss2 glue;
  std::array<double, 3> glueLuma{0, 0, 0}; ///< 胶亮度的 P5 / P50 / P95
  std::vector<Gauss2> background;
  ColorStats stats;
  bool hasStats = true; ///< 读进来的精简模型没有 stats：能打分，不能当 prior
  bool usable() const { return stats.glueN >= 2 && !background.empty(); }
};

/// 统计 → 模型：胶取矩，背景从直方图做确定性的 EM（≤ maxComponents 个分量，权重 < 2% 的去掉）。
ColorModel fitColorModel(const ColorStats& stats, const ColorSpec& spec, int maxComponents = 3);

/// 一个像素的得分与状态。c1 = ln((R+1)/(G+1))，c2 = ln((B+1)/(G+1))，luma = BT.601 亮度（都是平滑后的）。
/// 无信息时返回 NaN（state = Highlight / Dark），暗背景返回 0。saturated 由调用方按膨胀后的掩膜给。
double colorScore(const ColorModel& m, double c1, double c2, double luma, bool saturated,
                  ColorState* state);

/// 胶与每个背景分量的分离度（合并协方差下两均值的马氏距离）里最小的那个。没有背景分量时 NaN。
double colorSeparation(const ColorModel& m);

/// 一块（roi ∩ 图像）的平滑对数色度、亮度、R + G + B、归一化色度 (R + 1, B + 1) / (R + G + B + 3) 与高光掩膜。
/// CV_32F + 一张 CV_8U，块里的值与整幅计算逐位相同。
struct ColorPlanes {
  cv::Mat c1, c2, luma, sum, nr, nb;
  cv::Mat saturated;
  cv::Rect frame;
};
ColorPlanes colorPlanes(const cv::Mat& rgb, const ColorSpec& spec, cv::Rect roi);

/// glue.color_map 的三张图：score（NaN = 无信息）、luma、state（ColorState 的值，f32），只有 frame 那一块。
struct ColorMaps {
  cv::Mat score, luma, state;
  cv::Rect frame;
};
/// roi 宽高 ≤ 0 = 整幅。roi 与图像不相交时 frame 为空。行按 threads 个线程分块并行，结果与线程数无关。
ColorMaps colorMaps(const cv::Mat& rgb, const ColorModel& m, cv::Rect roi, int threads = 1);

struct ColorMergeGuide {
  double width = std::numeric_limits<double>::quiet_NaN();
  double center = 0;
};

/// 缝内不得有明确背景（有限 score < segmentMin / 2）。有邻站参考时允许缝宽到 max(gap, width / 2)，
/// 只接受宽度、中心与参考一致且比原片段更合理的组合。给了 `noInfo`（无信息采样，不含 ROI / 图像外）时，
/// 缝里全是无信息、且不宽于两边较窄一段的也并（宽胶面上的镜面反光带）。保留外边完成标记，返回各输出对应的输入首末下标。
std::vector<std::pair<std::size_t, std::size_t>>
mergeAcrossGaps(std::vector<Run>* runs, const std::vector<char>& background, double half,
                double gap, double widthMax, const ColorMergeGuide* guide = nullptr,
                const std::vector<char>* noInfo = nullptr);

/// extent：段的一条边被无信息采样截断（`from` 是边外第一个采样，`dir` = ±1 朝外）时，往外走过连续的无信息采样：
/// 先遇到明确不是胶的采样（0 ≤ score < segmentMin），说明胶在这里结束、外侧是饱和或过暗的背景，这条边是看得见的真边；
/// 又接上胶色时，无信息区宽过 `glueSpan`（px）才算（那是隔开的另一处胶），或那段胶色不宽于 max(4 px, glueSpan / 10)、
/// 之后又不是胶（两片背景交界的过渡像素）；
/// 无信息区一直到卡尺端点或 ROI / 图像外（`outside`）时，长度不小于 `boundarySpan`（px）才算。
/// score < 0 为无信息；`from` 本身在 ROI / 图像外时不算（那是观测边界截断）。
bool backgroundBeyond(const std::vector<double>& score, int from, int dir, double segmentMin,
                      double boundarySpan, double glueSpan,
                      const std::vector<char>* outside = nullptr);
/// 按 backgroundBeyond 把段 `r` 外侧是背景的不完整边认作真边；`reference` 有限时，认边后的宽度须不小于它的 0.7 倍
/// （半条胶落在高光里时只剩半宽，不能当完整胶宽）。`half` 是卡尺半长，采样间距 0.5 px。返回是否改了边。
bool completeAgainstBackground(Run* r, const std::vector<double>& score, double half,
                               double segmentMin, double reference,
                               const std::vector<char>* outside = nullptr);

/// 卡尺上一个采样无信息的原因。
enum class ColorCause { None, Highlight, Dark, Outside };
const char* causeName(ColorCause c);
/// state 图（f32，按 frame 摆回图像坐标）上 p 的双线性四邻点里最「重」的无信息原因：出块 > 高光 > 暗区。
ColorCause causeAt(const cv::Mat& state, cv::Rect frame, P2 p);

// ------------------------------------------------------------------ 建模（glue.color_model）

struct ColorTrainSpec {
  ColorSpec pixel;
  double glueHalf = 4; ///< 胶样本：中心 ± glueHalf（px）
  double bgInner = 20; ///< 背景样本：离中心 [bgInner, bgOuter]
  double bgOuter = 40;
  double stationStep = 4; ///< 沿线取样与重新居中的站距
  int recenterRounds = 2;
  double shiftMax = 20;      ///< 重新居中的偏移上限（px）
  double trimSigma = 3;      ///< 胶样本截尾：马氏距离超过它的去掉再拟合一次
  double bgExcludeSigma = 3; ///< 背景带里离胶马氏距离小于它的样本当作像胶、不进背景
  int maxComponents = 3;
};

struct ColorTrainResult {
  ColorModel model;            ///< prior + 这一帧的统计拟合出来的
  ColorStats frame;            ///< 只有这一帧的统计
  std::vector<double> s;       ///< 各站的 s
  std::vector<P2> centers;     ///< 各站最后一轮的胶中心
  std::vector<double> offsets; ///< 各站最后一轮的中心偏移（沿法向，px）
  double centerShift = std::numeric_limits<double>::quiet_NaN(); ///< |偏移| 的中位数
  double recentered = 0;                                         ///< 最后一轮重新量到中心的站占比
};

/// 沿示教线取样建模。每一帧的统计只由这一帧算（重新居中只用这一帧的模型），再与 prior 相加后拟合：
/// 先后累加几帧的结果与顺序无关。
ColorTrainResult trainColorModel(const cv::Mat& rgb, const PolylineView& line,
                                 const ColorTrainSpec& spec, const ColorStats* prior);

// ------------------------------------------------------------------ JSON（glue.ColorModel）

nlohmann::json colorModelJson(const ColorModel& m);
/// 不是合格的 glue.ColorModel 时返回 false，why 写原因。
bool readColorModel(const nlohmann::json& j, ColorModel* out, std::string* why);

// ------------------------------------------------------------------ 彩色跟踪（glue.color_track）

struct ColorTrackSpec {
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
  bool extent = false;
  double acceptOffsetRatio = 0.6;
  double coarseStep = 8;
  double maxGap = 120;
  double gapSteps = 1.5;
  int historyMin = 3;
  double jumpRatio = 0.5;
  double jumpMin = 8;
  double reacquireWidthRatio = 0.5;
  double steadyIqrRatio = 0.3;
  double dirWindow = 48;
  double infoMin = 0.5;
  double mergeUnknownGap = 16; ///< 同 glue.color_runs：被高光 / 暗区隔开的两段并成一段再挑胶点
};

struct ColorTrackResult {
  int steps = 0;
  int unknownSteps = 0;
  std::vector<TrackPoint> points;
  std::vector<double> ambiguousS;
  std::vector<double> incompleteS;
  std::vector<double> unknownS; ///< 无信息步的 s
  double coverage = 0;
  double coverageInformative = std::numeric_limits<double>::quiet_NaN();
  double stoppedAt = std::numeric_limits<double>::quiet_NaN();
};

/// trackBead 的彩色版：在得分图上找段（先按 mergeAcrossGaps 把被反光带隔开的两段并起来，胶点取合并段的中点），
/// 没有陡度门与退回射线；接受窗里有效采样不到 infoMin 且没找到胶的一步是「无信息」，不计入 maxGap。
ColorTrackResult colorTrack(const Field& score, const ColorTrackSpec& spec, double headingDeg);

} // namespace lyflow::packs::glue
