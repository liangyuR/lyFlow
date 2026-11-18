#pragma once

// 特征是对数色度 c = (ln R/G, ln B/G)（各通道先做 σ = blurSigma 的高斯，压掉去马赛克的假色），与照明强弱无关。
// 胶是一个二维高斯，背景是 ≤ 3 个高斯，各带一个平均亮度（R + G + B）；每个像素按亮度加一项各向同性的噪声 (noiseK / Y)²。
// 得分 ∈ [0, 1] 是胶的面积占比估计：取马氏距离最近的「胶—背景」连线，在归一化色度 (R, B) / (R + G + B) 上解混出
// 胶占的光通量 β，再按两者的亮度比 ρ = I胶 / I背景 换成面积 α = β / (β + ρ(1 − β))。亮胶压在暗铸件上时，
// 色度在 25 % 面积处就过了中点；按面积算，半高边落在 50 % 处，胶宽不偏。离胶与所有连线都远的颜色另有一道门（gate）压低。
// 三态：任一通道 ≥ satLevel（膨胀 satDilate）是高光，色度无信息；亮度 < darkRatio × 胶的亮度 P5 是暗背景（得分 0）；
// 其余噪声 > noiseMax 的是暗区，也无信息。无信息的像素得分是 NaN。
#include <array>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>

namespace lyflow::std_image::color {

inline constexpr const char* kColorModelType = "glue.ColorModel";

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

struct ColorSample {
  double c1, c2, luma, sum, nr, nb;
  bool saturated;
};

bool isDark(const ColorSpec& spec, double luma);
void accumulateColorStats(ColorStats& stats, const ColorSpec& spec,
                          const std::vector<ColorSample>& foreground,
                          const std::vector<ColorSample>& background, double trimSigma,
                          double bgExcludeSigma);

class ColorScorer {
public:
  explicit ColorScorer(const ColorModel& model);
  double score(const ColorSample& sample, ColorState* state) const;

private:
  struct Part {
    std::array<double, 3> pooled;
    double ex, ey;
    double br, bb;
    double rho;
  };
  const ColorModel& model_;
  double gr_, gb_;
  std::vector<Part> parts_;
};

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

nlohmann::json colorModelJson(const ColorModel& m);
/// 不是合格的 glue.ColorModel 时返回 false，why 写原因。
bool readColorModel(const nlohmann::json& j, ColorModel* out, std::string* why);

} // namespace lyflow::std_image::color
