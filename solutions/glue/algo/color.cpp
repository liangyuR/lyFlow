#include "color.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace lyflow::packs::solution_glue {
namespace {

using lyflow::std_image::color::ColorPlanes;
using lyflow::std_image::color::ColorSample;
using lyflow::std_image::color::ColorScorer;
using lyflow::std_image::color::ColorState;
using lyflow::std_image::inspection::caliperOffset;
using lyflow::std_image::inspection::informative;
using lyflow::std_image::inspection::outside;
using lyflow::std_image::inspection::samplesBeyond;
using lyflow::std_image::inspection::scoreKind;
using lyflow::std_image::line_scan::kCaliperStep;
using lyflow::std_image::line_scan::sampleCaliper;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

struct Pose {
  double s;
  P2 c, n;
};

std::vector<Pose> posesOf(const PolylineView& line, double step) {
  std::vector<Pose> out;
  if (!line.valid() || !(step > 0))
    return out;
  const double a = line.s->front(), b = line.s->back();
  for (int k = 0;; ++k) {
    const double s = a + k * step;
    if (s > b + 1e-6)
      break;
    Pose p;
    p.s = s;
    P2 t;
    line.at(s, &p.c, &t);
    p.n = perp(t);
    out.push_back(p);
  }
  return out;
}

bool pixelAt(const ColorPlanes& pl, P2 p, ColorSample* out) {
  if (!finite(p))
    return false;
  const int x = static_cast<int>(std::lround(p.x)), y = static_cast<int>(std::lround(p.y));
  if (!pl.frame.contains(cv::Point(x, y)))
    return false;
  const int px = x - pl.frame.x, py = y - pl.frame.y;
  out->c1 = pl.c1.at<float>(py, px);
  out->c2 = pl.c2.at<float>(py, px);
  out->luma = pl.luma.at<float>(py, px);
  out->sum = pl.sum.at<float>(py, px);
  out->nr = pl.nr.at<float>(py, px);
  out->nb = pl.nb.at<float>(py, px);
  out->saturated = pl.saturated.at<std::uint8_t>(py, px) != 0;
  return true;
}

} // namespace

std::vector<std::pair<std::size_t, std::size_t>>
mergeAcrossGaps(std::vector<Run>* runs, const std::vector<char>& background, double half,
                double gap, double widthMax, const ColorMergeGuide* guide,
                const std::vector<char>* noInfo, bool keepWide) {
  const int count = static_cast<int>(background.size());
  const bool guided = guide && std::isfinite(guide->width) && guide->width > 0;
  const double limit = guided ? std::max(gap, 0.5 * guide->width) : gap;
  std::vector<Run> out;
  std::vector<std::pair<std::size_t, std::size_t>> groups;
  const auto cost = [&](const Run& r) {
    return std::fabs(r.width() - guide->width) + 2.0 * std::fabs(r.mid() - guide->center);
  };
  for (std::size_t first = 0; first < runs->size();) {
    Run candidate = (*runs)[first], best = candidate;
    std::size_t last = first;
    double bestCost = guided ? cost(best) : 0;
    double componentCost = bestCost;
    for (std::size_t k = first + 1; k < runs->size(); ++k) {
      const Run& r = (*runs)[k];
      if (r.hi - candidate.lo > widthMax)
        break;
      const int from =
          static_cast<int>(std::floor((candidate.hi + half) / kCaliperStep + 1e-6)) + 1;
      const int to = static_cast<int>(std::ceil((r.lo + half) / kCaliperStep - 1e-6)) - 1;
      int bg = 0, total = 0, blind = 0;
      for (int i = std::max(from, 0); i <= std::min(to, count - 1); ++i) {
        ++total;
        bg += background[static_cast<std::size_t>(i)] ? 1 : 0;
        blind += noInfo && (*noInfo)[static_cast<std::size_t>(i)] ? 1 : 0;
      }
      // extent：缝里全是高光 / 暗区、不宽于两边较窄的一段（宽胶面上的一条镜面反光带，可以比 limit 宽），
      // 或两段紧挨着、中间没有采样（胶里一道浅谷被半高拆开）：都是同一条胶，直接并
      const bool stripe =
          noInfo && blind == total && r.lo - candidate.hi <= std::min(candidate.width(), r.width());
      if ((r.lo - candidate.hi > limit && !stripe) || (total <= 0 && !keepWide) || bg > 0)
        break;
      candidate.hi = r.hi;
      candidate.hiComplete = r.hiComplete;
      candidate.completeEdges = candidate.completeEdges && r.completeEdges;
      candidate.peak = std::max(candidate.peak, r.peak);
      candidate.parts += r.parts;
      if (guided)
        componentCost = std::min(componentCost, cost(r));
      if (stripe && keepWide) {
        best = candidate;
        bestCost = guided ? cost(candidate) : 0;
        last = k;
        continue;
      }
      // extent 不按参考宽设上限：局部鼓包比邻站宽是要量出来的
      if (guided && (candidate.width() < 0.7 * guide->width ||
                     (!keepWide && candidate.width() > 1.3 * guide->width) ||
                     std::fabs(candidate.mid() - guide->center) > 0.3 * guide->width))
        continue;
      const double nextCost = guided ? cost(candidate) : 0;
      if (!guided || (nextCost < bestCost && nextCost < componentCost)) {
        best = candidate;
        bestCost = nextCost;
        last = k;
      }
    }
    out.push_back(best);
    groups.emplace_back(first, last);
    first = last + 1;
  }
  *runs = std::move(out);
  return groups;
}

namespace {

bool backgroundBeyond(const std::vector<CaliperSampleKind>& kinds, int from, int dir,
                      double boundarySpan, double glueSpan) {
  const int count = static_cast<int>(kinds.size());
  if (from < 0 || from >= count || informative(kinds[from]) || outside(kinds[from]))
    return false;
  int i = from;
  for (; i >= 0 && i < count; i += dir) {
    const auto kind = kinds[static_cast<std::size_t>(i)];
    if (outside(kind))
      break;
    if (informative(kind)) {
      if (kind != CaliperSampleKind::Signal || std::abs(i - from) * kCaliperStep > glueSpan)
        return true;
      int j = i;
      while (j >= 0 && j < count && kinds[static_cast<std::size_t>(j)] == CaliperSampleKind::Signal)
        j += dir;
      const bool closed = j >= 0 && j < count && !outside(kinds[static_cast<std::size_t>(j)]);
      return closed && std::abs(j - i) * kCaliperStep <= std::max(4.0, 0.1 * glueSpan);
    }
  }
  return std::abs(i - from) * kCaliperStep >= boundarySpan;
}

} // namespace

bool completeAgainstBackground(Run* r, const std::vector<CaliperSampleKind>& kinds, double half,
                               double reference, double minRatio) {
  const int count = static_cast<int>(kinds.size());
  const auto [lo, hi] = samplesBeyond(*r, half, count);
  const double span = std::isfinite(reference) ? 0.0 : r->width();
  const double glueSpan = r->width();
  const bool loBg = !r->loComplete && backgroundBeyond(kinds, lo, -1, span, glueSpan);
  const bool hiBg = !r->hiComplete && backgroundBeyond(kinds, hi, 1, span, glueSpan);
  if (!loBg && !hiBg)
    return false;
  if (std::isfinite(reference) && r->width() < minRatio * reference)
    return false;
  if (loBg) {
    r->lo = caliperOffset(half, lo + 1);
    r->loComplete = true;
  }
  if (hiBg) {
    r->hi = caliperOffset(half, hi - 1);
    r->hiComplete = true;
  }
  r->completeEdges = r->loComplete && r->hiComplete;
  return true;
}

ColorTrainResult trainColorModel(const cv::Mat& rgb, const PolylineView& line,
                                 const ColorTrainSpec& spec, const ColorStats* prior) {
  ColorTrainResult out;
  const std::vector<Pose> poses = posesOf(line, spec.stationStep);
  out.frame.frames = 1;
  std::vector<double> offsets(poses.size(), 0.0);
  if (!poses.empty()) {
    const double reach = spec.shiftMax + std::max(spec.bgOuter, spec.glueHalf) + 2.0;
    double x0 = poses[0].c.x, x1 = x0, y0 = poses[0].c.y, y1 = y0;
    for (const Pose& p : poses) {
      x0 = std::min(x0, p.c.x);
      x1 = std::max(x1, p.c.x);
      y0 = std::min(y0, p.c.y);
      y1 = std::max(y1, p.c.y);
    }
    const cv::Rect box(static_cast<int>(std::floor(x0 - reach)),
                       static_cast<int>(std::floor(y0 - reach)),
                       static_cast<int>(std::ceil(x1 - x0 + 2 * reach)) + 2,
                       static_cast<int>(std::ceil(y1 - y0 + 2 * reach)) + 2);
    const ColorPlanes pl = colorPlanes(rgb, spec.pixel, box & cv::Rect(0, 0, rgb.cols, rgb.rows));
    ColorModel cur;
    std::vector<int> measured(poses.size(), 0);
    for (int round = 0; round <= spec.recenterRounds; ++round) {
      ColorStats st;
      st.frames = 1;
      std::vector<ColorSample> glue, bg;
      const auto take = [&](P2 p, std::vector<ColorSample>* to) {
        ColorSample px;
        if (!pixelAt(pl, p, &px))
          return;
        if (px.saturated)
          st.saturated += 1;
        else if (isDark(spec.pixel, px.luma))
          st.dark += 1;
        else
          to->push_back(px);
      };
      for (std::size_t i = 0; i < poses.size(); ++i) {
        const Pose& q = poses[i];
        const double o = offsets[i];
        for (double t = -spec.glueHalf; t <= spec.glueHalf + 1e-9; t += 1.0)
          take(q.c + q.n * (o + t), &glue);
        for (double t = spec.bgInner; t <= spec.bgOuter + 1e-9; t += 1.0) {
          take(q.c + q.n * (o - t), &bg);
          take(q.c + q.n * (o + t), &bg);
        }
      }
      accumulateColorStats(st, spec.pixel, glue, bg, spec.trimSigma, spec.bgExcludeSigma);
      out.frame = st;
      cur = fitColorModel(st, spec.pixel, spec.maxComponents);
      if (round == spec.recenterRounds || !cur.usable())
        break;
      // 重新居中：按这一帧的模型给每站卡尺打分，取离当前中心最近的一段（得分 ≥ 0.5）的中点
      const double half = spec.shiftMax + spec.bgInner;
      std::vector<double> next(poses.size(), kNaN);
      const ColorScorer scorer(cur);
      for (std::size_t i = 0; i < poses.size(); ++i) {
        const Pose& q = poses[i];
        std::vector<char> in;
        const int count = static_cast<int>(std::floor(2.0 * half / kCaliperStep + 1e-9)) + 1;
        in.resize(static_cast<std::size_t>(count));
        for (int k = 0; k < count; ++k) {
          ColorSample px;
          ColorState state;
          const P2 p = q.c + q.n * (-half + kCaliperStep * k);
          in[k] = pixelAt(pl, p, &px) && scorer.score(px, &state) >= 0.5;
        }
        double best = kNaN;
        for (int a = 0; a < count;) {
          if (!in[a]) {
            ++a;
            continue;
          }
          int b = a;
          while (b + 1 < count && in[b + 1])
            ++b;
          const double mid = -half + kCaliperStep * 0.5 * (a + b);
          if (a > 0 && b < count - 1 && std::fabs(mid) <= spec.shiftMax &&
              (!std::isfinite(best) || std::fabs(mid - offsets[i]) < std::fabs(best - offsets[i])))
            best = mid;
          a = b + 1;
        }
        next[i] = best;
      }
      // 五站滑动中值抹掉个别站的误认；一站都没量到的保留原来的偏移
      std::vector<double> smoothed(poses.size());
      for (std::size_t i = 0; i < poses.size(); ++i) {
        std::vector<double> win;
        for (std::size_t j = i >= 2 ? i - 2 : 0; j <= std::min(poses.size() - 1, i + 2); ++j) {
          if (std::isfinite(next[j]))
            win.push_back(next[j]);
        }
        smoothed[i] = win.empty() ? offsets[i] : median(win);
        measured[i] = std::isfinite(next[i]) ? 1 : 0;
      }
      offsets = smoothed;
    }
    std::vector<double> mags;
    int nMeasured = 0;
    for (std::size_t i = 0; i < poses.size(); ++i) {
      out.s.push_back(poses[i].s);
      out.centers.push_back(poses[i].c + poses[i].n * offsets[i]);
      out.offsets.push_back(offsets[i]);
      mags.push_back(std::fabs(offsets[i]));
      nMeasured += measured[i];
    }
    out.centerShift = median(mags);
    out.recentered = spec.recenterRounds > 0 ? static_cast<double>(nMeasured) / poses.size() : 0.0;
  }
  ColorStats total;
  if (prior)
    total.add(*prior);
  total.add(out.frame);
  out.model = fitColorModel(total, spec.pixel, spec.maxComponents);
  return out;
}

// ------------------------------------------------------------------ 彩色跟踪

ColorTrackResult colorTrack(const Field& R, const ColorTrackSpec& spec, double headingDeg) {
  ColorTrackResult out;
  P2 d = dirOfDeg(headingDeg);
  P2 p = spec.anchor + d * spec.zoneStart;
  double s = spec.zoneStart;
  double lastFound = -std::numeric_limits<double>::infinity();
  double unknownSince = 0; // 上一个胶点之后无信息步走过的长度
  bool stopped = false;
  std::vector<double> widths;
  const double accept = spec.acceptOffsetRatio * (spec.extent ? spec.searchHalf : spec.widthMax);
  const double window = std::min(accept, spec.searchHalf);
  RunSplitSpec rawSpec = spec.runs;
  rawSpec.mergeGap = 0;
  while (s <= spec.zoneEnd + 1e-6) {
    out.steps += 1;
    const P2 n = perp(d);
    bool found = false;
    P2 c;
    if (!stopped) {
      const bool afterGap = !out.points.empty() && s - lastFound > spec.gapSteps * spec.coarseStep;
      const bool hasHistory = static_cast<int>(widths.size()) >= spec.historyMin;
      const double wMed = hasHistory ? median(widths) : kNaN;
      const bool steady = hasHistory && percentile(widths, 75) - percentile(widths, 25) <
                                            spec.steadyIqrRatio * wMed;
      ColorMergeGuide guide;
      if (steady && spec.mergeUnknownGap > 0)
        guide.width = wMed;
      const auto score = sampleCaliper(R, p, n, spec.searchHalf);
      std::vector<Run> rs = caliperRuns(score, spec.searchHalf, rawSpec);
      std::vector<char> background(score.size());
      for (std::size_t i = 0; i < score.size(); ++i)
        background[i] = score[i] >= 0.0 && score[i] < 0.5 * spec.runs.segmentMin;
      std::vector<CaliperSampleKind> kinds;
      std::vector<char> noInfo;
      if (spec.extent) {
        kinds.resize(score.size());
        noInfo.resize(score.size());
        const cv::Rect& in = R.valid();
        for (std::size_t i = 0; i < score.size(); ++i) {
          const P2 q = p + n * caliperOffset(spec.searchHalf, static_cast<int>(i));
          const bool off = !(q.x >= in.x && q.y >= in.y && q.x + 1 < in.x + in.width &&
                             q.y + 1 < in.y + in.height);
          kinds[i] = off ? CaliperSampleKind::Outside : scoreKind(score[i], spec.runs.segmentMin);
          noInfo[i] = score[i] < 0 && !off;
        }
      }
      mergeAcrossGaps(&rs, background, spec.searchHalf,
                      std::max(spec.runs.mergeGap, spec.mergeUnknownGap),
                      spec.extent ? 2.0 * spec.searchHalf : spec.runs.mergeWidthMax, &guide,
                      spec.extent ? &noInfo : nullptr);
      if (spec.extent)
        for (Run& x : rs)
          completeAgainstBackground(&x, kinds, spec.searchHalf, wMed);
      rs.erase(std::remove_if(rs.begin(), rs.end(),
                              [&](const Run& x) {
                                return (x.width() < spec.widthMin &&
                                        (!spec.extent || (x.loComplete && x.hiComplete))) ||
                                       (!spec.extent && x.width() > spec.widthMax);
                              }),
               rs.end());
      if (hasHistory) {
        rs.erase(std::remove_if(rs.begin(), rs.end(),
                                [&](const Run& x) {
                                  if (spec.extent && (!x.loComplete || !x.hiComplete))
                                    return false;
                                  if (!afterGap)
                                    return std::fabs(x.mid()) >
                                               std::max(spec.jumpRatio * wMed, spec.jumpMin) ||
                                           (steady && x.width() < spec.reacquireWidthRatio * wMed &&
                                            std::fabs(x.mid()) > 0.15 * wMed);
                                  return steady && x.width() < spec.reacquireWidthRatio * wMed;
                                }),
                 rs.end());
      }
      const Run* r = nearestRun(rs);
      bool ambiguous = false, incomplete = false;
      double edgeOnly = kNaN; // 贴画面边的胶点中心（法向偏移），NaN = 两边都看得见
      if (spec.extent && r && std::fabs(r->mid()) < accept) {
        for (const Run& other : rs) {
          if (&other != r && std::fabs(other.mid()) < accept &&
              std::fabs(other.mid()) - std::fabs(r->mid()) <= std::max(1.0, 0.1 * spec.widthMax) &&
              other.peak >= 0.8 * r->peak)
            ambiguous = true;
        }
        incomplete = !r->loComplete || !r->hiComplete;
        // 一侧是真边、另一侧走出 ROI / 图像（胶贴着画面边）：位置仍可信，只是看不到全宽。
        // 有稳定宽度时按真边 + 半个参考宽取中心，否则取可见片段中点；宽度不进历史
        if (incomplete && !ambiguous && r->loComplete != r->hiComplete) {
          const int count = static_cast<int>(kinds.size());
          const auto [lo, hi] = samplesBeyond(*r, spec.searchHalf, count);
          const int beyond = r->loComplete ? hi : lo;
          if (beyond >= 0 && beyond < count && outside(kinds[static_cast<std::size_t>(beyond)])) {
            const bool known = steady && r->width() < wMed;
            edgeOnly = known ? (r->loComplete ? r->lo + 0.5 * wMed : r->hi - 0.5 * wMed) : r->mid();
            incomplete = false;
          }
        }
        if (ambiguous)
          out.ambiguousS.push_back(s);
        else if (incomplete)
          out.incompleteS.push_back(s);
        if (ambiguous || incomplete)
          r = nullptr;
      }
      const auto plausible = [&](P2 q) {
        if (length(q - spec.anchor) < spec.outwardRatio * s)
          return false;
        if (spec.bearingTo - spec.bearingFrom >= 360.0 - 1e-6)
          return true;
        const double b = degOf(q - spec.anchor);
        const double mid = 0.5 * (spec.bearingFrom + spec.bearingTo);
        return angleDiffDeg(b, mid) <=
               0.5 * (spec.bearingTo - spec.bearingFrom) + spec.bearingMargin;
      };
      const double mid = r && std::isfinite(edgeOnly) ? edgeOnly : r ? r->mid() : 0.0;
      if (r && std::fabs(mid) < accept && plausible(p + n * mid)) {
        c = p + n * mid;
        TrackPoint tp;
        tp.s = s;
        tp.point = c;
        tp.mid = mid;
        tp.width = r->width();
        tp.peak = r->peak;
        out.points.push_back(tp);
        if (!std::isfinite(edgeOnly))
          widths.push_back(r->width());
        lastFound = s;
        unknownSince = 0;
        found = true;
      } else {
        int total = 0, valid = 0;
        for (double t = -window; t <= window + 1e-9; t += kCaliperStep) {
          total += 1;
          valid += R.at(p + n * t) >= 0.0 ? 1 : 0;
        }
        if (ambiguous || incomplete ||
            (total > 0 && static_cast<double>(valid) / total < spec.infoMin)) {
          out.unknownSteps += 1;
          out.unknownS.push_back(s);
          unknownSince += spec.coarseStep;
        }
      }
    }
    std::vector<P2> recent;
    for (const TrackPoint& q : out.points) {
      if (q.s >= s - spec.dirWindow)
        recent.push_back(q.point);
    }
    if (recent.size() >= 3)
      d = principalDirection(recent, d);
    p = (found ? c : p) + d * spec.coarseStep;
    s += spec.coarseStep;
    if (!stopped && !out.points.empty() && s - lastFound - unknownSince > spec.maxGap) {
      stopped = true;
      out.stoppedAt = s;
    }
  }
  out.coverage = out.steps > 0 ? static_cast<double>(out.points.size()) / out.steps : 0.0;
  const int informative = out.steps - out.unknownSteps;
  out.coverageInformative =
      informative > 0 ? static_cast<double>(out.points.size()) / informative : kNaN;
  return out;
}

} // namespace lyflow::packs::solution_glue
