#pragma once

#include <limits>
#include <vector>

#include "bead.h"
#include "lyflow_cv/color_caliper.h"
#include "lyflow_cv/color_model.h"

namespace lyflow::packs::solution_glue {

using lyflow::std_image::color::ColorModel;
using lyflow::std_image::color::ColorSpec;
using lyflow::std_image::color::ColorStats;

struct ColorMergeGuide {
  double width = std::numeric_limits<double>::quiet_NaN();
  double center = 0;
};

/// 缝内不得有明确背景（有限 score < segmentMin / 2）。有邻站参考时允许缝宽到 max(gap, width / 2)，
/// 只接受宽度、中心与参考一致且比原片段更合理的组合。给了 `noInfo`（无信息采样，不含 ROI / 图像外）时，
/// 缝里全是无信息、且不宽于两边较窄一段的也可以并（宽胶面上的镜面反光带）。`keepWide`（extent 量宽用）时
/// 这种反光带与紧挨着、中间没有采样的两段（胶里的浅谷）直接并，其余组合不按参考宽设上限（局部鼓包要量出来）；
/// 跟踪不开它，免得在交叉、接头处把两条胶并成一段、拉偏胶路。保留外边完成标记，返回各输出对应的输入首末下标。
std::vector<std::pair<std::size_t, std::size_t>>
mergeAcrossGaps(std::vector<Run>* runs, const std::vector<char>& background, double half,
                double gap, double widthMax, const ColorMergeGuide* guide = nullptr,
                const std::vector<char>* noInfo = nullptr, bool keepWide = false);

/// 按 backgroundBeyond 把段 `r` 外侧是背景的不完整边认作真边；`reference` 有限时，认边后的宽度须不小于它的 `minRatio` 倍
/// （半条胶落在高光里时只剩半宽，不能当完整胶宽）。`half` 是卡尺半长，采样间距 0.5 px。返回是否改了边。
using lyflow::std_image::inspection::CaliperSampleKind;
bool completeAgainstBackground(Run* r, const std::vector<CaliperSampleKind>& kinds, double half,
                               double reference, double minRatio = 0.7);

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
  double mergeUnknownGap = 16; ///< 被高光 / 暗区隔开的两段并成一段再挑胶点
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

} // namespace lyflow::packs::solution_glue
