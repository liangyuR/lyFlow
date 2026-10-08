#include "synth.h"

#include "edge.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

#include <opencv2/imgproc.hpp>

namespace lyflow::packs::glue {
namespace {

/// 盖住的带子上的一个截面：胶路点、法向、盖住的横向范围（沿法向的偏移）。
struct Section {
  double s = 0;
  P2 c;
  P2 n;
  double lo = 0;
  double hi = 0;
};

double cross(P2 a, P2 b) { return a.x * b.y - a.y * b.x; }

/// 点在凸四边形 q[0..3]（顺序给出）里吗：四条边的叉积同号。
bool insideQuad(const P2 q[4], P2 p) {
  int pos = 0, neg = 0;
  for (int i = 0; i < 4; ++i) {
    const double c = cross(q[(i + 1) % 4] - q[i], p - q[i]);
    if (c > 1e-9) pos += 1;
    if (c < -1e-9) neg += 1;
  }
  return pos == 0 || neg == 0;
}

std::string pct(double v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.1f%%", 100.0 * v);
  return buf;
}

}  // namespace

SynthResult synthBreak(const cv::Mat& gray, const PolylineView& path,
                       const std::vector<Station>& stations, double stationStep, bool bright,
                       int widthMax, const SynthSpec& spec, cv::Mat& out) {
  SynthResult res;
  res.sFrom = spec.sStart;
  res.sTo = spec.sStart + spec.length;
  out = gray.clone();
  if (!path.valid() || spec.length <= 0) {
    res.reason = "没有胶路或长度不是正数";
    return res;
  }

  std::vector<const Station*> rows;
  for (const Station& st : stations) {
    if (st.present && st.s >= res.sFrom - 1e-6 && st.s < res.sTo - 1e-6) rows.push_back(&st);
  }
  res.stations = static_cast<int>(rows.size());
  const int need = std::max(2, static_cast<int>(spec.length / std::max(stationStep, 1e-6)) - 1);
  if (res.stations < need) {
    res.reason = "这一段有胶的站只有 " + std::to_string(res.stations) + " 个（要 ≥ " +
                 std::to_string(need) + "）";
    return res;
  }

  // 截面：每 2 px 一个，横向范围取离它最近的有胶站的两边再各放宽 margin
  std::vector<Section> sections;
  for (double s = res.sFrom;; s += 2.0) {
    const double sq = std::min(s, res.sTo);
    Section sec;
    sec.s = sq;
    P2 t;
    path.at(sq, &sec.c, &t);
    sec.n = perp(t);
    const Station* near = rows.front();
    for (const Station* st : rows) {
      if (std::fabs(st->s - sq) < std::fabs(near->s - sq)) near = st;
    }
    sec.lo = near->lo - spec.margin;
    sec.hi = near->hi + spec.margin;
    sections.push_back(sec);
    if (sq >= res.sTo) break;
  }

  // 源带干不干净：两侧各看一遍响应
  double x0 = std::numeric_limits<double>::infinity(), y0 = x0, x1 = -x0, y1 = -x0;
  double span = 0;
  for (const Section& sec : sections) {
    x0 = std::min(x0, sec.c.x);
    y0 = std::min(y0, sec.c.y);
    x1 = std::max(x1, sec.c.x);
    y1 = std::max(y1, sec.c.y);
    span = std::max(span, std::max(std::fabs(sec.lo), std::fabs(sec.hi)));
  }
  const double reach = 3.0 * span + 2.0 * spec.shiftExtra + 8.0;
  const cv::Rect roi(static_cast<int>(std::floor(x0 - reach)), static_cast<int>(std::floor(y0 - reach)),
                     static_cast<int>(std::ceil(x1 - x0 + 2 * reach)) + 1,
                     static_cast<int>(std::ceil(y1 - y0 + 2 * reach)) + 1);
  const Field R = responseField(gray, bright, widthMax, roi);
  // 零件边（与 edge_distance 同一个判据、默认参数）：源带跨过零件边就会把外面的暗区搬进来，
  // 而零件边本身在黑顶帽里没有响应 —— 只看响应挡不住它，所以跨过去的那几截也算脏
  const EdgeResult edges = measureEdges(gray, stations, EdgeSpec{});
  auto partEdgeNear = [&](double s) {
    double best = std::numeric_limits<double>::infinity();
    double offset = std::numeric_limits<double>::quiet_NaN();
    for (std::size_t i = 0; i < stations.size(); ++i) {
      if (!edges.stations[i].kept) continue;
      const double d = std::fabs(stations[i].s - s);
      if (d < best) {
        best = d;
        offset = edges.stations[i].edgeOffset;
      }
    }
    return offset;
  };
  auto dirtyOf = [&](int side) {
    int total = 0, dirty = 0;
    for (const Section& sec : sections) {
      const double w = sec.hi - sec.lo;
      const double shift = side * (w + spec.shiftExtra);
      // 源带沿 side·n 的最远端 vs 这一截附近零件边的位置
      const double farEnd = side > 0 ? sec.hi + shift : -(sec.lo + shift);
      const double edgeAt = side == edges.side ? partEdgeNear(sec.s) : std::nan("");
      const bool crossesEdge = std::isfinite(edgeAt) && farEnd >= edgeAt - 2.0;
      for (double t = sec.lo + shift; t < sec.hi + shift; t += 1.0) {
        const double v = R.at(sec.c + sec.n * t);
        total += 1;
        // 图外也算脏：那里没有背景可搬
        if (crossesEdge || v < 0 || v > spec.contrastMin) dirty += 1;
      }
    }
    return total > 0 ? static_cast<double>(dirty) / total : 1.0;
  };
  res.dirtyRight = dirtyOf(1);
  res.dirtyLeft = dirtyOf(-1);
  res.side = res.dirtyRight <= res.dirtyLeft ? 1 : -1;
  const double dirty = std::min(res.dirtyRight, res.dirtyLeft);
  if (dirty > spec.dirtyMax) {
    res.reason = "两侧的源带都不干净（右 " + pct(res.dirtyRight) + "，左 " + pct(res.dirtyLeft) +
                 "，上限 " + pct(spec.dirtyMax) + "）";
    return res;
  }

  // 相邻两个截面围成一个四边形；四边形里的每个像素从源带平移过来
  cv::Mat src;
  gray.convertTo(src, CV_32F);
  const Field srcField(src, cv::Point(0, 0), cv::Rect(0, 0, gray.cols, gray.rows));
  cv::Mat acc = cv::Mat::zeros(gray.size(), CV_32F);
  cv::Mat val = cv::Mat::zeros(gray.size(), CV_32F);
  for (std::size_t i = 0; i + 1 < sections.size(); ++i) {
    const Section& a = sections[i];
    const Section& b = sections[i + 1];
    const double lo = std::min(a.lo, b.lo);
    const double hi = std::max(a.hi, b.hi);
    const P2 q[4] = {a.c + a.n * lo, a.c + a.n * hi, b.c + b.n * hi, b.c + b.n * lo};
    const P2 n = unit(a.n + b.n);
    const P2 shift = n * (res.side * ((hi - lo) + spec.shiftExtra));
    double bx0 = q[0].x, by0 = q[0].y, bx1 = q[0].x, by1 = q[0].y;
    for (const P2& p : q) {
      bx0 = std::min(bx0, p.x);
      by0 = std::min(by0, p.y);
      bx1 = std::max(bx1, p.x);
      by1 = std::max(by1, p.y);
    }
    const int ix0 = std::max(0, static_cast<int>(std::floor(bx0)));
    const int iy0 = std::max(0, static_cast<int>(std::floor(by0)));
    const int ix1 = std::min(gray.cols - 1, static_cast<int>(std::ceil(bx1)));
    const int iy1 = std::min(gray.rows - 1, static_cast<int>(std::ceil(by1)));
    for (int y = iy0; y <= iy1; ++y) {
      for (int x = ix0; x <= ix1; ++x) {
        const P2 p(x, y);
        if (!insideQuad(q, p)) continue;
        const double v = srcField.at(p + shift);
        if (v < 0) continue;
        acc.at<float>(y, x) = 1.0F;
        val.at<float>(y, x) = static_cast<float>(v);
      }
    }
  }
  // 羽化：权重是盖住区域的高斯模糊；填进去的值用同一个核做加权平均，羽化带里不会被拉暗
  cv::Mat fillNum, fillDen;
  cv::GaussianBlur(acc, fillDen, cv::Size(7, 7), spec.feather);
  cv::GaussianBlur(val, fillNum, cv::Size(7, 7), spec.feather);
  cv::Mat soft = fillDen * 1.6;
  cv::threshold(soft, soft, 1.0, 1.0, cv::THRESH_TRUNC);
  cv::Mat den;
  cv::max(fillDen, 1e-6, den);
  cv::Mat fill;
  cv::divide(fillNum, den, fill);
  cv::Mat keep = 1.0 - soft;
  cv::Mat blended = src.mul(keep) + fill.mul(soft);
  blended.convertTo(out, CV_8U);  // convertTo 自带四舍五入与 0–255 截断
  res.ok = true;
  return res;
}

}  // namespace lyflow::packs::glue
