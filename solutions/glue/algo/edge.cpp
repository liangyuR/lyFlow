#include "edge.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace lyflow::packs::solution_glue {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

using lyflow::std_image::line_scan::EdgeHit;
using lyflow::std_image::line_scan::EdgeScanSpec;

double findPartEdge(const Field& gray, P2 c, P2 dir, double start, const EdgeSpec& spec) {
  EdgeScanSpec es;
  es.contrastMin = spec.contrastMin;
  es.farRun = spec.farRun;
  es.darkToBright = spec.darkToBright;
  const std::vector<EdgeHit> hits =
      findPartEdges(gray, c, dir, start, spec.searchLength, es, /*firstOnly=*/true);
  return hits.empty() ? kNaN : hits.front().t;
}

} // namespace

EdgeResult measureEdges(const cv::Mat& gray, const std::vector<Station>& stations,
                        const EdgeSpec& spec) {
  EdgeResult out;
  out.stations.resize(stations.size());
  if (stations.empty())
    return out;
  // 灰度原样采样（不做二维平滑），卡尺剖面上再做一维平滑
  double x0 = std::numeric_limits<double>::infinity(), y0 = x0, x1 = -x0, y1 = -x0;
  for (const Station& st : stations) {
    x0 = std::min(x0, st.c.x);
    y0 = std::min(y0, st.c.y);
    x1 = std::max(x1, st.c.x);
    y1 = std::max(y1, st.c.y);
  }
  const double reach = spec.searchLength + 100.0;
  const Field field = std_image::line_scan::grayFieldAround(gray, P2(x0, y0), P2(x1, y1), reach);

  // 两侧都找（D8）；offsets[side] 是这一站那一侧的零件边偏移
  std::vector<double> right(stations.size(), kNaN), left(stations.size(), kNaN);
  for (std::size_t i = 0; i < stations.size(); ++i) {
    const Station& st = stations[i];
    if (!st.present)
      continue;
    out.stations[i].searched = true;
    right[i] = findPartEdge(field, st.c, st.n, st.hi + 3.0, spec);
    left[i] = findPartEdge(field, st.c, -st.n, -st.lo + 3.0, spec);
    if (std::isfinite(right[i]))
      out.votesRight += 1;
    if (std::isfinite(left[i]))
      out.votesLeft += 1;
  }
  if (out.votesRight == 0 && out.votesLeft == 0)
    return out;
  out.side = out.votesRight >= out.votesLeft ? 1 : -1;
  const std::vector<double>& chosen = out.side > 0 ? right : left;

  std::vector<std::size_t> idx;
  std::vector<double> d;
  for (std::size_t i = 0; i < stations.size(); ++i) {
    if (!std::isfinite(chosen[i]))
      continue;
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
  for (std::size_t i = 0; i < d.size(); ++i)
    dev[i] = std::fabs(d[i] - med[i]);
  const double mad = median(dev) + 0.5;
  for (std::size_t i = 0; i < d.size(); ++i) {
    out.stations[idx[i]].kept = dev[i] <= 4.0 * mad + 2.0;
  }
  return out;
}

} // namespace lyflow::packs::solution_glue
