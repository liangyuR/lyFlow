#pragma once
// 坐标一律是图像像素：x 向右、y 向下、像素中心在整数坐标。
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

#include <opencv2/core.hpp>

#include "lyflow/kernel/polyline.h"

namespace lyflow::std_image::line_scan {

using P2 = cv::Point2d;

inline double dot(P2 a, P2 b) { return a.x * b.x + a.y * b.y; }
inline double length(P2 a) { return std::hypot(a.x, a.y); }
inline P2 unit(P2 a) { return kernel::unitTangent2D(a); }
/// 法向 n = (−t.y, t.x)：图像 y 向下，它指向行进方向的右侧。
inline P2 perp(P2 t) { return P2(-t.y, t.x); }
inline P2 dirOfDeg(double deg) {
  const double a = deg * CV_PI / 180.0;
  return P2(std::cos(a), std::sin(a));
}
inline double degOf(P2 d) { return std::atan2(d.y, d.x) * 180.0 / CV_PI; }
inline bool finite(P2 p) { return std::isfinite(p.x) && std::isfinite(p.y); }

/// 采样取不到（落在可信区之外）时的返回值。场里的有效值都 ≥ 0。
constexpr double kInvalid = -1.0;

/// 图像坐标里的一块 CV_32F 场。data 的 (0, 0) 在图像坐标 origin 处；只有 valid 里的值可信
/// （外扩出来算卷积、形态学用的那一圈不算）。NaN 同样按不可信处理。
class Field {
public:
  Field() = default;
  Field(cv::Mat data, cv::Point origin, cv::Rect valid)
      : data_(std::move(data)), origin_(origin), valid_(valid) {}

  /// 整幅的一张 CV_32F 图（零拷贝），NaN 的像素不可信。
  static Field whole(cv::Mat data) {
    const cv::Rect all(0, 0, data.cols, data.rows);
    return Field(std::move(data), cv::Point(0, 0), all);
  }

  bool empty() const { return data_.empty() || valid_.empty(); }
  const cv::Rect& valid() const { return valid_; }

  /// 双线性采样。四个邻点有一个不在可信区里就返回 kInvalid。
  double at(double x, double y) const {
    if (empty() || !std::isfinite(x) || !std::isfinite(y))
      return kInvalid;
    const double fx = std::floor(x);
    const double fy = std::floor(y);
    const int x0 = static_cast<int>(fx);
    const int y0 = static_cast<int>(fy);
    if (x0 < valid_.x || y0 < valid_.y || x0 + 1 >= valid_.x + valid_.width ||
        y0 + 1 >= valid_.y + valid_.height) {
      return kInvalid;
    }
    const double ax = x - fx;
    const double ay = y - fy;
    const float* r0 = data_.ptr<float>(y0 - origin_.y) + (x0 - origin_.x);
    const float* r1 = data_.ptr<float>(y0 + 1 - origin_.y) + (x0 - origin_.x);
    const double top = r0[0] + ax * (r0[1] - r0[0]);
    const double bottom = r1[0] + ax * (r1[1] - r1[0]);
    const double v = top + ay * (bottom - top);
    return std::isnan(v) ? kInvalid : v;
  }
  double at(P2 p) const { return at(p.x, p.y); }

private:
  cv::Mat data_;
  cv::Point origin_;
  cv::Rect valid_;
};

/// 黑顶帽（bright 时白顶帽）：闭运算 − 原图，结构元直径 = widthMax + 1（大于期望最大条带宽度），
/// 再做一次 5×5、σ=1.2 的高斯。结构元是近似圆盘的正八边形（方形 + 两条对角线段的闵可夫斯基和），
/// 比 OpenCV 的椭圆核快三十多倍（整幅 1280×1024 约 4 ms 对 120 ms）。
/// roi 是要用到的那一块（图像坐标）；内部按两倍结构元半径外扩着算，roi 里的值与整幅图算出来的相同。
/// level 不为空时另给一张同样高斯过的局部亮度：暗带取闭运算（条带两旁的背景），亮带取原图（条带本身），
/// 即顶帽两项里亮的那一项，处处 ≥ 响应。
Field responseField(const cv::Mat& gray, bool bright, int widthMax, cv::Rect roi,
                    Field* level = nullptr);

/// 边缘陡度用的平滑灰度（5×5、σ=1.0）。bright 时取反（255 − g）：陡度的符号约定就只有「暗带」一种。
Field smoothGrayField(const cv::Mat& gray, bool bright, cv::Rect roi);

/// 结构元直径、三个 5×5 高斯的 σ 都开放（σ = 0 不平滑）。roi 宽高 ≤ 0 = 整幅。
struct ResponseSpec {
  bool bright = false;
  int diameter = 91;
  cv::Rect roi;
  double responseSigma = 1.2;
  double graySigma = 1.0;
  double levelSigma = 1.2;
};

/// 三张 CV_32F 图，只有 frame（roi ∩ 图像，没给 roi 时是整幅）那一块，(0, 0) 在图像坐标 frame.tl()；
/// 块里的值与整幅计算逐位相同。frame 为空（roi 与图像不相交）时三张图都是空的。
/// response = 顶帽响应，gray = 平滑灰度（bright 时 255 − g），level = 局部亮度（暗带取闭运算、亮带取原图）。
struct ResponseMaps {
  cv::Mat response;
  cv::Mat gray;
  cv::Mat level;
  cv::Rect frame;
  /// 按 frame 摆回图像坐标的采样场（零拷贝，Mat 要活得比它久）。
  Field field(const cv::Mat& m) const { return Field(m, frame.tl(), frame); }
};

ResponseMaps responseMaps(const cv::Mat& gray, const ResponseSpec& spec);

/// 法向卡尺上的一个区段：lo / hi 是两条边相对卡尺中心沿法向的偏移（px），peak 是段内响应的峰值。
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

/// 卡尺分段的全部门限。halfMax = 按连通段的半高拆成子段；否则一个连通段一段。
struct RunSplitSpec {
  double segmentMin = 8;
  double peakMin = 16;
  double mergeGap = 8;
  double mergePeakRatio = 0; ///< 0 = 合并不查峰值比
  double mergeWidthMax = 90;
  bool halfMax = false;
};

inline constexpr double kCaliperStep = 0.5;

/// 沿 c + t·n 等距采样，无效值保留为 kInvalid。
std::vector<double> sampleCaliper(const Field& response, P2 c, P2 n, double half);

/// 沿 c + t·n（t ∈ [−half, half]，0.5 px 一步）的全部暗段，按 lo 升序，不按宽度筛。
/// raw 给了就写合并前的子段；validFraction 给了就写有效采样的比例。
std::vector<Run> caliperRuns(const Field& response, P2 c, P2 n, double half,
                             const RunSplitSpec& spec, std::vector<Run>* raw = nullptr,
                             double* validFraction = nullptr);
std::vector<Run> caliperRuns(const std::vector<double>& samples, double half,
                             const RunSplitSpec& spec, std::vector<Run>* raw = nullptr,
                             double* validFraction = nullptr);

/// 边缘陡度的组成：edgeLo / edgeHi 是两条边内外各 edgeProbe 的灰度差，depth 是暗度。取不到全是 NaN。
struct RunEdges {
  double edgeLo = std::numeric_limits<double>::quiet_NaN();
  double edgeHi = std::numeric_limits<double>::quiet_NaN();
  double depth = std::numeric_limits<double>::quiet_NaN();
  double sharpness = std::numeric_limits<double>::quiet_NaN();
};

RunEdges runEdges(const Field& g, P2 c, P2 n, const Run& run, double edgeProbe = 3,
                  double backgroundProbe = 8);

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
  /// 有效方向不够 peakCount 个峰时，得分为 NaN 的方向（按角度顺序）也拿来补
  bool includeInvalid = false;
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

/// 每个方向一条射线上响应的均值；峰按得分降序贪心取，与已取的角差都 > peakSeparation（includeInvalid 时无效方向排在最后）。
FanResult fanScan(const Field& response, const FanSpec& spec);

struct EdgeScanSpec {
  double contrastMin = 40;
  double farRun = 12;
  double farDropRatio = 0.5;
  double descentRatio = 0.05;
  bool darkToBright = false;
};

struct EdgeHit {
  double t = 0; ///< 从 c 沿 dir 的距离（亚像素）
  double contrast = 0;
  double farMargin = 0;
  double descent = 0;
};

/// 沿 c + dir·t（t 从 start 起 1 px 一步、到 start + length，出图截断）的全部合格跳变，按 t 升序。
/// firstOnly 时找到第一个就停。samples 给了就写实际采到的点数。
std::vector<EdgeHit> findPartEdges(const Field& gray, P2 c, P2 dir, double start, double length,
                                   const EdgeScanSpec& spec, bool firstOnly = false,
                                   int* samples = nullptr);

double angleDiffDeg(double a, double b);
Field grayFieldAround(const cv::Mat& gray, P2 minCenter, P2 maxCenter, double reach);

} // namespace lyflow::std_image::line_scan
