#include "edge.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace lyflow::packs::glue {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

/// 7 个抽头、σ = 1.5 的一维高斯，两头按边界值延拓。
std::vector<double> smooth1d(const std::vector<double>& v) {
  static const double kTaps[7] = {0.0702, 0.1311, 0.1907, 0.2160, 0.1907, 0.1311, 0.0702};
  const int n = static_cast<int>(v.size());
  std::vector<double> out(v.size());
  for (int i = 0; i < n; ++i) {
    double acc = 0, wsum = 0;
    for (int k = -3; k <= 3; ++k) {
      const int j = std::clamp(i + k, 0, n - 1);
      acc += kTaps[k + 3] * v[static_cast<std::size_t>(j)];
      wsum += kTaps[k + 3];
    }
    out[static_cast<std::size_t>(i)] = acc / wsum;
  }
  return out;
}

}  // namespace

double findPartEdge(const Field& gray, P2 c, P2 dir, double start, const EdgeSpec& spec) {
  const int want = static_cast<int>(std::floor(spec.searchLength)) + 1;
  std::vector<double> v;
  v.reserve(static_cast<std::size_t>(want));
  for (int k = 0; k < want; ++k) {
    const double x = gray.at(c + dir * (start + k));
    if (x < 0) break;  // 出了图：截断，不把图外当成暗（否则图像边界本身就成了「零件边」）
    v.push_back(spec.darkToBright ? 255.0 - x : x);
  }
  const int far = std::max(1, static_cast<int>(std::lround(spec.farRun)));
  const int n = static_cast<int>(v.size());
  if (n < far + 10) return kNaN;
  const std::vector<double> p = smooth1d(v);
  auto descent = [&](int k) { return 0.5 * (p[static_cast<std::size_t>(k - 1)] - p[static_cast<std::size_t>(k + 1)]); };
  for (int k = 5; k + 2 + far <= n && k + 1 < n; ++k) {
    const double dk = descent(k);
    if (dk <= 0.05 * spec.contrastMin) continue;
    // 跳变里下降最快的那一点：往前、往后都不比它陡
    if (descent(k - 1) > dk) continue;
    if (k + 2 < n && descent(k + 1) >= dk) continue;
    double nearMean = 0;
    for (int j = k - 5; j <= k - 2; ++j) nearMean += p[static_cast<std::size_t>(j)];
    nearMean /= 4.0;
    double farMean = 0, farMax = -std::numeric_limits<double>::infinity();
    for (int j = k + 2; j < k + 2 + far; ++j) {
      farMean += p[static_cast<std::size_t>(j)];
      farMax = std::max(farMax, p[static_cast<std::size_t>(j)]);
    }
    farMean /= far;
    if (nearMean - farMean < spec.contrastMin) continue;
    if (nearMean - farMax < 0.5 * spec.contrastMin) continue;  // 远侧没持续暗下去：划痕、反光条
    const double a = descent(k - 1);
    const double b = k + 2 < n ? descent(k + 1) : dk;
    const double den = a - 2.0 * dk + b;
    const double delta = den < -1e-12 ? std::clamp(0.5 * (a - b) / den, -0.5, 0.5) : 0.0;
    return start + k + delta;
  }
  return kNaN;
}

EdgeResult measureEdges(const cv::Mat& gray, const std::vector<Station>& stations,
                        const EdgeSpec& spec) {
  EdgeResult out;
  out.stations.resize(stations.size());
  if (stations.empty()) return out;
  // 灰度原样采样（不做二维平滑），卡尺剖面上再做一维平滑
  double x0 = std::numeric_limits<double>::infinity(), y0 = x0, x1 = -x0, y1 = -x0;
  for (const Station& st : stations) {
    x0 = std::min(x0, st.c.x);
    y0 = std::min(y0, st.c.y);
    x1 = std::max(x1, st.c.x);
    y1 = std::max(y1, st.c.y);
  }
  const double reach = spec.searchLength + 100.0;
  const cv::Rect roi(static_cast<int>(std::floor(x0 - reach)), static_cast<int>(std::floor(y0 - reach)),
                     static_cast<int>(std::ceil(x1 - x0 + 2 * reach)) + 1,
                     static_cast<int>(std::ceil(y1 - y0 + 2 * reach)) + 1);
  const cv::Rect inner = roi & cv::Rect(0, 0, gray.cols, gray.rows);
  cv::Mat g;
  if (!inner.empty()) gray(inner).convertTo(g, CV_32F);
  const Field field(g, inner.tl(), inner);

  // 两侧都找（D8）；offsets[side] 是这一站那一侧的零件边偏移
  std::vector<double> right(stations.size(), kNaN), left(stations.size(), kNaN);
  for (std::size_t i = 0; i < stations.size(); ++i) {
    const Station& st = stations[i];
    if (!st.present) continue;
    out.stations[i].searched = true;
    right[i] = findPartEdge(field, st.c, st.n, st.hi + 3.0, spec);
    left[i] = findPartEdge(field, st.c, -st.n, -st.lo + 3.0, spec);
    if (std::isfinite(right[i])) out.votesRight += 1;
    if (std::isfinite(left[i])) out.votesLeft += 1;
  }
  if (out.votesRight == 0 && out.votesLeft == 0) return out;
  out.side = out.votesRight >= out.votesLeft ? 1 : -1;
  const std::vector<double>& chosen = out.side > 0 ? right : left;

  std::vector<std::size_t> idx;
  std::vector<double> d;
  for (std::size_t i = 0; i < stations.size(); ++i) {
    if (!std::isfinite(chosen[i])) continue;
    const Station& st = stations[i];
    EdgeStation& es = out.stations[i];
    es.found = true;
    es.edgeOffset = chosen[i];
    const double nearEdge = out.side > 0 ? st.hi : -st.lo;
    es.refOffset = spec.fromCenter ? out.side * st.mid() : nearEdge;
    es.distancePx = es.edgeOffset - es.refOffset;
    es.edge = st.c + st.n * (out.side * es.edgeOffset);
    es.from = st.c + st.n * (out.side * es.refOffset);
    idx.push_back(i);
    d.push_back(es.distancePx);
  }
  // D9 的逐站剔野：沿 s 取 7 站的滑动中值，离它超过 4·MAD + 2 的不要
  std::vector<double> med(d.size());
  for (std::size_t i = 0; i < d.size(); ++i) {
    const std::size_t a = i >= 3 ? i - 3 : 0;
    const std::size_t b = std::min(d.size(), i + 4);
    med[i] = median(std::vector<double>(d.begin() + static_cast<std::ptrdiff_t>(a),
                                        d.begin() + static_cast<std::ptrdiff_t>(b)));
  }
  std::vector<double> dev(d.size());
  for (std::size_t i = 0; i < d.size(); ++i) dev[i] = std::fabs(d[i] - med[i]);
  const double mad = median(dev) + 0.5;
  for (std::size_t i = 0; i < d.size(); ++i) {
    out.stations[idx[i]].kept = dev[i] <= 4.0 * mad + 2.0;
  }
  return out;
}

}  // namespace lyflow::packs::glue
