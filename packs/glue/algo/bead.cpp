#include "bead.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numeric>

#include <opencv2/imgproc.hpp>

namespace lyflow::packs::glue {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// ------------------------------------------------------------------ 结构元

/// 近似直径 diameter 的圆盘的正八边形：方形 [−a, a]² ⊕ 两条对角线段 [−b, b]·(1, ±1)。
/// 轴向外沿 a + 2b = r，对角外沿 (a + b)·√2 = r。radius 是它在轴向上的外沿（整数像素）。
struct Octagon {
  cv::Mat square;
  cv::Mat diag;
  cv::Mat anti;
  int radius = 0;
};

Octagon octagon(int diameter) {
  const double r = 0.5 * std::max(diameter, 1);
  const double b = r - r / std::sqrt(2.0);
  const double a = r - 2.0 * b;
  const int ia = std::max(0, static_cast<int>(std::lround(a)));
  const int ib = std::max(0, static_cast<int>(std::lround(b)));
  Octagon o;
  o.square = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(2 * ia + 1, 2 * ia + 1));
  o.diag = cv::Mat::eye(2 * ib + 1, 2 * ib + 1, CV_8U);
  cv::flip(o.diag, o.anti, 1);
  o.radius = ia + 2 * ib;
  return o;
}

/// 闭运算（dark）或开运算（bright）。分解成三次膨胀 + 三次腐蚀，默认边界（图外的像素不参与）。
cv::Mat closeOrOpen(const cv::Mat& src, const Octagon& o, bool open) {
  cv::Mat x = src;
  const cv::Mat* kernels[3] = {&o.square, &o.diag, &o.anti};
  for (int pass = 0; pass < 2; ++pass) {
    const bool dilatePass = (pass == 0) != open;
    for (const cv::Mat* k : kernels) {
      // 每一步写进新的 Mat：x 第一轮还指着 src 的像素，原地写会把原图改掉
      cv::Mat y;
      if (dilatePass) {
        cv::dilate(x, y, *k);
      } else {
        cv::erode(x, y, *k);
      }
      x = y;
    }
  }
  return x;
}

cv::Rect imageRect(const cv::Mat& m) { return cv::Rect(0, 0, m.cols, m.rows); }

cv::Rect inflate(cv::Rect r, int pad) {
  return cv::Rect(r.x - pad, r.y - pad, r.width + 2 * pad, r.height + 2 * pad);
}

// ------------------------------------------------------------------ 方向

/// 一组点的主方向（PCA），朝向与 fallback 同侧。不到 3 个点返回 fallback。
P2 principalDirection(const std::vector<P2>& pts, P2 fallback) {
  if (pts.size() < 3) return fallback;
  P2 m(0, 0);
  for (const P2& p : pts) m += p;
  m *= 1.0 / static_cast<double>(pts.size());
  double sxx = 0, sxy = 0, syy = 0;
  for (const P2& p : pts) {
    const P2 d = p - m;
    sxx += d.x * d.x;
    sxy += d.x * d.y;
    syy += d.y * d.y;
  }
  // 2×2 对称矩阵最大特征值的特征向量
  const double tr = sxx + syy;
  const double det = sxx * syy - sxy * sxy;
  const double disc = std::sqrt(std::max(0.0, 0.25 * tr * tr - det));
  const double lambda = 0.5 * tr + disc;
  P2 v = std::fabs(sxy) > 1e-12 ? P2(lambda - syy, sxy) : (sxx >= syy ? P2(1, 0) : P2(0, 1));
  v = unit(v);
  if (dot(v, fallback) < 0) v = -v;
  return v;
}

double angleDiffDeg(double a, double b) {
  double d = std::fmod(std::fabs(a - b), 360.0);
  return d > 180.0 ? 360.0 - d : d;
}

// ------------------------------------------------------------------ 粗找（D1 / D6 / D15）

PathCandidate coarseTrack(const Field& R, const Field& g, const PathSpec& spec, double headingDeg,
                          double fanLo, double fanHi) {
  PathCandidate cand;
  cand.headingDeg = headingDeg;
  RunSpec runs;
  runs.contrastMin = spec.contrastMin;
  runs.widthMax = spec.widthMax;
  P2 d = dirOfDeg(headingDeg);
  P2 p = spec.nozzle + d * spec.zoneStart;
  double s = spec.zoneStart;
  double lastFound = -std::numeric_limits<double>::infinity();
  bool stopped = false;
  int stepsAtLastFound = 0;  // 最近一次找到胶时已经走了几步（断口之前那一段的覆盖率）
  std::vector<double> sharp, widths, peaks;
  while (s <= spec.zoneEnd + 1e-6) {
    cand.steps += 1;
    const P2 n = perp(d);
    bool found = false;
    P2 c;
    if (!stopped) {
      // 已经找到过几个胶点之后，卡尺上的暗段不是都能接：
      // - 紧接着上一步（中间没断）时，胶点离预测点横着不超过半个胶宽（此前胶宽的中位数，至少 8 px）——
      //   光滑的胶 8 px 一步横着挪不了这么多；断口一开头卡尺会横着够到胶旁边的阴影、纹理，接上它胶路就被一路拽歪；
      // - 滑过一段没找到胶之后重新接上的那一个，横向不限，但得像此前找到的胶：对比度不到此前中位数的一半的不要
      //   （断口里、翻边上的拉丝纹理淡得多）；此前的胶宽很一致（直胶）时，宽度不到中位数一半的也不要（卡尺斜着
      //   擦过断口端头只量到半截胶，中心是偏的）；边缘陡度不到 sharpMin 的也不要（D15 的判据逐个用在这里：
      //   阴影也是暗带，但边缘是缓的）
      const bool afterGap = !cand.coarse.empty() && s - lastFound > 1.5 * spec.coarseStep;
      auto lookAt = [&](P2 q, P2 nq) {
        std::vector<Run> rs = lateralRuns(R, q, nq, spec.widthMax, runs);
        if (widths.size() >= 3) {
          const double wMed = median(widths);
          const double pMed = median(peaks);
          const bool steady = percentile(widths, 75) - percentile(widths, 25) < 0.3 * wMed;
          rs.erase(std::remove_if(rs.begin(), rs.end(),
                                  [&](const Run& x) {
                                    if (!afterGap) return std::fabs(x.mid()) > std::max(0.5 * wMed, 8.0);
                                    if (x.peak < 0.5 * pMed || (steady && x.width() < 0.5 * wMed)) return true;
                                    const double sh = runSharpness(g, q, nq, x);
                                    return std::isfinite(sh) && sh < spec.sharpMin;
                                  }),
                   rs.end());
        }
        return rs;
      };
      std::vector<Run> rs = lookAt(p, n);
      const Run* r = nearestRun(rs);
      P2 at = p, nAt = n;
      bool onFanRay = false;
      // 断口之前那一段得确实像胶（几乎每步都找到、边缘够陡）才这么做：没胶的帧里粗找跟的是零件上的纹理，
      // 再给它一条退路，覆盖率就凑够了
      const bool looksLikeBead = sharp.size() >= 3 && percentile(sharp, 25.0) >= spec.sharpMin &&
                                 static_cast<double>(cand.coarse.size()) >= 0.6 * stepsAtLastFound;
      if ((!r || std::fabs(r->mid()) >= 0.6 * spec.widthMax) && afterGap && looksLikeBead) {
        // 断口后面跟丢了：退回扇形搜索选中的那条射线上找（断口前那几个胶点定出的方向可能是歪的，
        // 螺旋胶尤其如此；而那条射线本来就是检测区前段响应最强的方向）
        const P2 d0 = dirOfDeg(headingDeg);
        at = spec.nozzle + d0 * s;
        nAt = perp(d0);
        rs = lookAt(at, nAt);
        r = nearestRun(rs);
        onFanRay = true;
      }
      // 胶是从喷嘴沿扇区里的方向往外走出去的：胶点相对喷嘴的方位跑出搜索范围 20° 以外、或者离喷嘴的
      // 距离不到 0.75·s（沿着检测区的圆弧横着走），跟的就不是胶了 —— 零件上一条纹理边能把粗找一路带偏。
      // 演示数据满胶段的胶路上处处是 距离 / s ≥ 0.98
      const auto plausible = [&](P2 q) {
        if (length(q - spec.nozzle) < 0.75 * s) return false;
        if (fanHi - fanLo >= 360.0 - 1e-6) return true;
        const double b = degOf(q - spec.nozzle);
        const double mid = 0.5 * (fanLo + fanHi);
        return angleDiffDeg(b, mid) <= 0.5 * (fanHi - fanLo) + 20.0;
      };
      if (r && std::fabs(r->mid()) < 0.6 * spec.widthMax && plausible(at + nAt * r->mid())) {
        c = at + nAt * r->mid();
        cand.coarse.emplace_back(s, c);
        widths.push_back(r->width());
        peaks.push_back(r->peak);
        const double sh = runSharpness(g, at, nAt, *r);
        if (std::isfinite(sh)) sharp.push_back(sh);
        lastFound = s;
        stepsAtLastFound = cand.steps;
        found = true;
        // 在那条射线上接上的：方向也回到它，断口前的方向不作数了
        if (onFanRay) d = dirOfDeg(headingDeg);
      }
    }
    std::vector<P2> recent;
    for (const auto& [ss, q] : cand.coarse) {
      if (ss >= s - spec.dirWindow) recent.push_back(q);
    }
    if (recent.size() >= 3) d = principalDirection(recent, d);
    p = (found ? c : p) + d * spec.coarseStep;
    s += spec.coarseStep;
    // 在长断口里一直顺着预测方向滑下去，迟早会滑到别的暗带上：已经找到过胶、又连续 maxGap
    // 没找到，就不再接受新的胶点，后面的胶路交给拟合按已有的胶点外推
    if (!cand.coarse.empty() && s - lastFound > spec.maxGap) stopped = true;
  }
  cand.coverage = cand.steps > 0 ? static_cast<double>(cand.coarse.size()) / cand.steps : 0.0;
  cand.sharpness = sharp.size() >= 3 ? percentile(sharp, 25.0) : -1.0;
  cand.beadlike = cand.sharpness >= spec.sharpMin;
  cand.score = cand.beadlike ? cand.coverage : 0.1 * cand.coverage;
  return cand;
}

// ------------------------------------------------------------------ 稳健拟合（D1）

/// 局部线性平滑：每个网格点用 ±window 内的胶点（不到 3 个就取最近的 3–8 个）做加权最小二乘，
/// 三轮之后离得远的胶点权重降到 0.1（残差 ≥ 3·MAD + 2）。与参考实现一致，权重进法方程时取平方。
std::vector<P2> localLinearFit(const std::vector<std::pair<double, P2>>& centers,
                               const std::vector<double>& grid, double window,
                               double* residualOut) {
  const std::size_t n = centers.size();
  std::vector<double> w(n, 1.0);
  std::vector<P2> fits(grid.size());
  std::vector<std::size_t> order(n);
  std::vector<double> resid(n, 0.0);
  for (int it = 0; it < 3; ++it) {
    for (std::size_t gi = 0; gi < grid.size(); ++gi) {
      const double s0 = grid[gi];
      std::vector<std::size_t> pick;
      for (std::size_t i = 0; i < n; ++i) {
        if (std::fabs(centers[i].first - s0) <= window) pick.push_back(i);
      }
      if (pick.size() < 3) {
        std::iota(order.begin(), order.end(), std::size_t{0});
        std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
          return std::fabs(centers[a].first - s0) < std::fabs(centers[b].first - s0);
        });
        const std::size_t k = std::min<std::size_t>(n, std::max<std::size_t>(3, std::min<std::size_t>(8, n)));
        pick.assign(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(k));
      }
      // 法方程：Σ w²·[1, ds]ᵀ[1, ds]·[a, b]ᵀ = Σ w²·[1, ds]ᵀ·x
      double s00 = 0, s01 = 0, s11 = 0, bx0 = 0, bx1 = 0, by0 = 0, by1 = 0;
      for (std::size_t i : pick) {
        const double ww = w[i] * w[i];
        const double ds = centers[i].first - s0;
        s00 += ww;
        s01 += ww * ds;
        s11 += ww * ds * ds;
        bx0 += ww * centers[i].second.x;
        bx1 += ww * ds * centers[i].second.x;
        by0 += ww * centers[i].second.y;
        by1 += ww * ds * centers[i].second.y;
      }
      const double det = s00 * s11 - s01 * s01;
      if (std::fabs(det) > 1e-12) {
        fits[gi] = P2((bx0 * s11 - s01 * bx1) / det, (by0 * s11 - s01 * by1) / det);
      } else {
        fits[gi] = P2(bx0 / std::max(s00, 1e-12), by0 / std::max(s00, 1e-12));
      }
    }
    for (std::size_t i = 0; i < n; ++i) {
      double best = std::numeric_limits<double>::infinity();
      for (const P2& f : fits) best = std::min(best, length(f - centers[i].second));
      resid[i] = best;
    }
    const double mad = median(resid) + 1e-6;
    for (std::size_t i = 0; i < n; ++i) w[i] = resid[i] < 3.0 * mad + 2.0 ? 1.0 : 0.1;
  }
  if (residualOut) *residualOut = median(resid);
  return fits;
}

/// 把拟合出来的一串点按真实弧长重新参数化：第一个点取离喷嘴恰好 zoneStart 的那一处（s = zoneStart），
/// 之后 s 按弧长累加，每 2 px 取一个点直到 zoneEnd；拟合点不够长就沿末端切向外推。
void resampleByArcLength(const std::vector<P2>& dense, P2 nozzle, double zoneStart, double zoneEnd,
                         BeadPath& out) {
  // 起点：第一次走到离喷嘴 zoneStart 的地方
  std::size_t k0 = 0;
  P2 start = dense.front();
  bool crossed = false;
  for (std::size_t i = 0; i + 1 < dense.size(); ++i) {
    const double d0 = length(dense[i] - nozzle);
    const double d1 = length(dense[i + 1] - nozzle);
    if (d0 <= zoneStart && d1 >= zoneStart) {
      const double f = d1 - d0 > 1e-9 ? (zoneStart - d0) / (d1 - d0) : 0.0;
      start = dense[i] + (dense[i + 1] - dense[i]) * f;
      k0 = i + 1;
      crossed = true;
      break;
    }
  }
  if (!crossed) {
    // 没有跨过那个圆（胶路几乎贴着圆走）：取离那个半径最近的点当起点
    double best = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < dense.size(); ++i) {
      const double e = std::fabs(length(dense[i] - nozzle) - zoneStart);
      if (e < best) {
        best = e;
        k0 = i;
      }
    }
    start = dense[k0];
    k0 += 1;
  }
  std::vector<P2> pts{start};
  std::vector<double> arc{0.0};
  for (std::size_t i = k0; i < dense.size(); ++i) {
    const double step = length(dense[i] - pts.back());
    if (step < 1e-9) continue;
    arc.push_back(arc.back() + step);
    pts.push_back(dense[i]);
  }
  const double total = zoneEnd - zoneStart;
  std::vector<double> targets;
  for (double t = 0.0; t < total - 1e-9; t += 2.0) targets.push_back(t);
  targets.push_back(total);
  P2 endTangent = pts.size() >= 2 ? unit(pts.back() - pts[pts.size() - 2]) : P2(1, 0);
  out.s.clear();
  out.points.clear();
  std::size_t seg = 0;
  for (double t : targets) {
    P2 q;
    if (t >= arc.back()) {
      q = pts.back() + endTangent * (t - arc.back());
    } else {
      while (seg + 1 < arc.size() && arc[seg + 1] < t) ++seg;
      const double span = arc[seg + 1] - arc[seg];
      const double f = span > 1e-12 ? (t - arc[seg]) / span : 0.0;
      q = pts[seg] + (pts[seg + 1] - pts[seg]) * f;
    }
    out.s.push_back(zoneStart + t);
    out.points.push_back(q);
  }
  out.tangents.assign(out.points.size(), P2(1, 0));
  for (std::size_t i = 0; i < out.points.size(); ++i) {
    const std::size_t a = i == 0 ? 0 : i - 1;
    const std::size_t b = i + 1 < out.points.size() ? i + 1 : i;
    out.tangents[i] = unit(out.points[b] - out.points[a]);
  }
}

/// 没找到胶时的胶路：从喷嘴沿选中方向的一条直线，照样覆盖整个检测区（下游据此画出「全段无胶」）。
void straightPath(P2 nozzle, double headingDeg, double zoneStart, double zoneEnd, BeadPath& out) {
  const P2 d = dirOfDeg(headingDeg);
  out.s.clear();
  out.points.clear();
  const double total = zoneEnd - zoneStart;
  for (double t = 0.0; t < total - 1e-9; t += 2.0) out.s.push_back(zoneStart + t);
  out.s.push_back(zoneEnd);
  for (double s : out.s) out.points.push_back(nozzle + d * s);
  out.tangents.assign(out.points.size(), d);
}

std::string fmt2(double v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.2f", v);
  return buf;
}

// ------------------------------------------------------------------ 示教胶路的偏移轨迹（T3 / T4）

constexpr double kTrackWindow = 40.0;     ///< 偏移轨迹逐站细化时沿 s 取中位数的半窗（px）
constexpr double kTrackMinSupport = 0.1;  ///< 落在轨迹上的站不到这个比例（或不到 3 站）= 沿示教线没有胶
constexpr int kTrackPasses = 8;           ///< 逐站细化最多几遍（落在轨迹上的那些站不再变就停）

struct Track {
  std::vector<double> expect;       ///< 每站期望的胶中心（px）
  std::vector<const Run*> onTrack;  ///< 每站落在轨迹上的那一段（nullptr = 这一站没有）
  int count = 0;
  double support = 0;
  std::string reason;               ///< 空 = 认出了胶
};

/// 示教胶路的偏移轨迹：胶相对示教线横着偏了多少（机器人 / 工件偏差、示教没点准），逐站估出来。
/// 候选 = 中心离示教线不超过 tol 的暗段（已按 widthRange、contrastMin 筛过）。
/// 1. 投票定一个常数偏移 h：每个候选的中心（外加 0）各是一个假设，票数 = 离它不到 gate 有候选的站数；
///    票数不低于最多那个一半的假设里取离示教线最近的 —— 断口里零散的纹理拽不走它，旁边一条更长的平行暗边也拽不走；
/// 2. 逐站细化，直到落在轨迹上的站不再变（最多 kTrackPasses 遍）：每站取离当前期望最近、且在 gate 以内的候选，
///    沿 s 在 ±kTrackWindow 里取中位数；没有候选落在轨迹上的站（断口）按两侧线性插值 —— 断口里的期望中心是两头的胶
///    连起来的那条线。一遍最多挪 gate 那么远，所以弯道上胶慢慢偏开示教线也跟得上，跳到旁边一条暗线上去却不行；
/// 3. 夹在 ±tol 里。落在轨迹上的站不到 3 个、或不到 kTrackMinSupport 就算沿示教线没有胶。
Track trackOffsets(const std::vector<Station>& st, double tol, double centerRatio) {
  Track tr;
  const std::size_t n = st.size();
  tr.expect.assign(n, 0.0);
  tr.onTrack.assign(n, nullptr);
  std::vector<double> candWidths, hypotheses{0.0};
  for (const Station& x : st) {
    for (const Run& r : x.runs) {
      if (std::fabs(r.mid()) > tol) continue;
      candWidths.push_back(r.width());
      hypotheses.push_back(r.mid());
    }
  }
  if (candWidths.empty()) {
    tr.reason = "示教线 ± " + fmt2(tol) + " px 以内没有一站量到胶样的暗段";
    return tr;
  }
  const double gate = std::max(6.0, centerRatio * median(candWidths));
  const auto nearestCand = [&](const Station& x, double e) -> const Run* {
    const Run* best = nullptr;
    for (const Run& r : x.runs) {
      if (std::fabs(r.mid()) > tol) continue;
      if (!best || std::fabs(r.mid() - e) < std::fabs(best->mid() - e)) best = &r;
    }
    return best && std::fabs(best->mid() - e) <= gate ? best : nullptr;
  };

  std::vector<int> votes(hypotheses.size(), 0);
  int bestVotes = 0;
  for (std::size_t k = 0; k < hypotheses.size(); ++k) {
    for (const Station& x : st) votes[k] += nearestCand(x, hypotheses[k]) ? 1 : 0;
    bestVotes = std::max(bestVotes, votes[k]);
  }
  // 示教线附近的那一条优先：得票不到最多那条一半的才让位。旁边一条平行的暗边（零件翻边、阴影）往往比断了一截的
  // 胶还长，单比票数它会赢，胶路就被它整条拽走 —— 这正是 bead_path 自由搜索缺胶时跟着零件暗边走的那种错
  double h = 0.0;
  int hVotes = -1;
  for (std::size_t k = 0; k < hypotheses.size(); ++k) {
    if (2 * votes[k] < bestVotes) continue;
    const double a = std::fabs(hypotheses[k]);
    if (hVotes < 0 || a < std::fabs(h) - 1e-9 || (a <= std::fabs(h) + 1e-9 && votes[k] > hVotes)) {
      h = hypotheses[k];
      hVotes = votes[k];
    }
  }
  std::fill(tr.expect.begin(), tr.expect.end(), h);

  std::vector<double> lastMids;
  for (int pass = 0; pass < kTrackPasses; ++pass) {
    std::vector<double> mids(n, kNaN);
    for (std::size_t i = 0; i < n; ++i) {
      if (const Run* r = nearestCand(st[i], tr.expect[i])) mids[i] = r->mid();
    }
    const auto same = [](double a, double b) { return std::isnan(a) ? std::isnan(b) : a == b; };
    if (lastMids.size() == n && std::equal(mids.begin(), mids.end(), lastMids.begin(), same)) break;
    lastMids = mids;
    std::vector<double> e(n, kNaN);
    for (std::size_t i = 0; i < n; ++i) {
      std::vector<double> w;
      for (std::size_t j = 0; j < n; ++j) {
        if (std::isfinite(mids[j]) && std::fabs(st[j].s - st[i].s) <= kTrackWindow) w.push_back(mids[j]);
      }
      if (!w.empty()) e[i] = median(std::move(w));
    }
    // 断口：两侧都有就线性插值，只有一侧就取那一侧，一个都没有就是投票的常数
    std::ptrdiff_t prev = -1;
    for (std::size_t i = 0; i <= n; ++i) {
      if (i < n && !std::isfinite(e[i])) continue;
      for (std::size_t k = static_cast<std::size_t>(prev + 1); k < i; ++k) {
        if (prev < 0 && i == n) {
          e[k] = h;
        } else if (prev < 0) {
          e[k] = e[i];
        } else if (i == n) {
          e[k] = e[static_cast<std::size_t>(prev)];
        } else {
          const double a = st[static_cast<std::size_t>(prev)].s, b = st[i].s;
          const double f = b - a > 1e-9 ? (st[k].s - a) / (b - a) : 0.0;
          e[k] = e[static_cast<std::size_t>(prev)] + (e[i] - e[static_cast<std::size_t>(prev)]) * f;
        }
      }
      prev = static_cast<std::ptrdiff_t>(i);
    }
    for (double& v : e) v = std::clamp(v, -tol, tol);
    tr.expect = std::move(e);
  }
  for (std::size_t i = 0; i < n; ++i) {
    tr.onTrack[i] = nearestCand(st[i], tr.expect[i]);
    tr.count += tr.onTrack[i] ? 1 : 0;
  }
  tr.support = n > 0 ? static_cast<double>(tr.count) / static_cast<double>(n) : 0.0;
  if (tr.count < 3 || tr.support < kTrackMinSupport) {
    tr.reason = "沿示教线只有 " + std::to_string(tr.count) + " 站（" + fmt2(100.0 * tr.support) +
                "%）量到落在同一条偏移轨迹上的胶，不到 3 站或 10%";
  }
  return tr;
}

}  // namespace

// ------------------------------------------------------------------ 小工具

double median(std::vector<double> v) {
  if (v.empty()) return kNaN;
  const std::size_t mid = v.size() / 2;
  std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid), v.end());
  const double hi = v[mid];
  if (v.size() % 2 == 1) return hi;
  const double lo = *std::max_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid));
  return 0.5 * (lo + hi);
}

double percentile(std::vector<double> v, double q) {
  if (v.empty()) return kNaN;
  std::sort(v.begin(), v.end());
  const double pos = (q / 100.0) * static_cast<double>(v.size() - 1);
  const std::size_t i = static_cast<std::size_t>(std::floor(pos));
  const std::size_t j = std::min(i + 1, v.size() - 1);
  return v[i] + (v[j] - v[i]) * (pos - static_cast<double>(i));
}

// ------------------------------------------------------------------ 响应图

Field responseField(const cv::Mat& gray, bool bright, int widthMax, cv::Rect roi) {
  const cv::Rect inner = roi & imageRect(gray);
  if (inner.empty()) return {};
  const Octagon o = octagon(widthMax + 1);
  // 闭运算里膨胀与腐蚀各走一次结构元，再加上 5×5 的高斯：外扩两倍半径 + 4 之后，inner 里的值
  // 与整幅图算出来的一样（外扩碰到图像边界时两边的边界规则也一样）
  const cv::Rect outer = inflate(inner, 2 * o.radius + 4) & imageRect(gray);
  const cv::Mat src = gray(outer).clone();
  const cv::Mat morph = closeOrOpen(src, o, bright);
  cv::Mat r;
  if (bright) {
    cv::subtract(src, morph, r, cv::noArray(), CV_32F);
  } else {
    cv::subtract(morph, src, r, cv::noArray(), CV_32F);
  }
  cv::GaussianBlur(r, r, cv::Size(5, 5), 1.2);
  return Field(r, outer.tl(), inner);
}

Field smoothGrayField(const cv::Mat& gray, bool bright, cv::Rect roi) {
  const cv::Rect inner = roi & imageRect(gray);
  if (inner.empty()) return {};
  const cv::Rect outer = inflate(inner, 4) & imageRect(gray);
  cv::Mat g;
  gray(outer).convertTo(g, CV_32F);
  cv::GaussianBlur(g, g, cv::Size(5, 5), 1.0);
  if (bright) g = 255.0 - g;
  return Field(g, outer.tl(), inner);
}

// ------------------------------------------------------------------ 卡尺

std::vector<Run> lateralRuns(const Field& response, P2 c, P2 n, double half, const RunSpec& spec) {
  constexpr double kRes = 0.5;
  const int count = static_cast<int>(std::floor(2.0 * half / kRes + 1e-9)) + 1;
  std::vector<double> v(static_cast<std::size_t>(std::max(count, 0)));
  std::vector<char> ok(v.size());
  for (int i = 0; i < count; ++i) {
    const double t = -half + i * kRes;
    const double x = response.at(c + n * t);
    ok[i] = x >= 0.0;
    v[i] = ok[i] ? x : 0.0;
  }
  // 不扣局部底：零件上成片的纹理、渐暗区响应整体抬高，「> contrastMin / 2」连成一整条、宽过 widthMax
  // 被筛掉 —— 胶是干净背景上一条分明的暗带，这正是把纹理挡在外面的那一道（Glue2 #1–2 的无胶帧）
  auto tAt = [&](int i) { return -half + i * kRes; };
  const double thr = 0.5 * spec.contrastMin;
  std::vector<Run> raw;
  for (int i = 0; i < count;) {
    if (!(ok[i] && v[i] > thr)) {
      ++i;
      continue;
    }
    int j = i;
    while (j + 1 < count && ok[j + 1] && v[j + 1] > thr) ++j;
    double peak = 0;
    for (int k = i; k <= j; ++k) peak = std::max(peak, v[k]);
    if (peak >= spec.contrastMin) {
      // D4：半高处取边，按这一段自己的峰值
      const double lvl = 0.5 * peak;
      int a = i;
      while (a < j && v[a] < lvl) ++a;
      int b = j;
      while (b > i && v[b] < lvl) --b;
      const double la =
          a > 0 ? tAt(a) - (v[a] - lvl) / std::max(v[a] - v[a - 1], 1e-6) * kRes : tAt(a);
      const double lb = b < count - 1
                            ? tAt(b) + (v[b] - lvl) / std::max(v[b] - v[b + 1], 1e-6) * kRes
                            : tAt(b);
      raw.push_back(Run{la, lb, peak});
    }
    i = j + 1;
  }
  // 高光条会把一条胶劈成两段（教训 3）：缝不超过 mergeGap、合并后不宽于 widthMax 就并回去
  std::vector<Run> merged;
  for (const Run& r : raw) {
    if (!merged.empty() && r.lo - merged.back().hi <= spec.mergeGap &&
        r.hi - merged.back().lo <= spec.widthMax) {
      merged.back().hi = r.hi;
      merged.back().peak = std::max(merged.back().peak, r.peak);
    } else {
      merged.push_back(r);
    }
  }
  std::vector<Run> out;
  for (const Run& r : merged) {
    if (r.width() >= spec.widthMin && r.width() <= spec.widthMax) out.push_back(r);
  }
  return out;
}

const Run* nearestRun(const std::vector<Run>& runs, double expect) {
  const Run* best = nullptr;
  for (const Run& r : runs) {
    if (!best || std::fabs(r.mid() - expect) < std::fabs(best->mid() - expect)) best = &r;
  }
  return best;
}

double runSharpness(const Field& g, P2 c, P2 n, const Run& run) {
  auto at = [&](double t) { return g.at(c + n * t); };
  const double w = run.width();
  const double samples[6] = {at(run.lo - 3), at(run.lo + 3), at(run.hi + 3),
                             at(run.hi - 3), at(run.lo - 8), at(run.hi + 8)};
  double inside = 0;
  for (int k = 0; k < 5; ++k) {
    const double v = at(run.lo + 0.25 * w + (0.5 * w) * k / 4.0);
    if (v < 0) return kNaN;
    inside += v;
  }
  for (double v : samples) {
    if (v < 0) return kNaN;
  }
  inside /= 5.0;
  const double dl = samples[0] - samples[1];
  const double dr = samples[2] - samples[3];
  const double depth = std::max(1.0, 0.5 * (samples[4] + samples[5]) - inside);
  return std::min(dl, dr) / depth;
}

// ------------------------------------------------------------------ 定胶路

BeadPath findBeadPath(const cv::Mat& gray, const PathSpec& spec) {
  BeadPath out;
  out.headingSource = spec.useHeading ? "param" : "search";
  // 响应图只算喷嘴周围用得到的那一块：检测区外沿再加一个最大胶宽的卡尺
  const double reach = spec.zoneEnd + spec.widthMax + 8.0;
  const cv::Rect roi(static_cast<int>(std::floor(spec.nozzle.x - reach)),
                     static_cast<int>(std::floor(spec.nozzle.y - reach)),
                     static_cast<int>(std::ceil(2 * reach)) + 1,
                     static_cast<int>(std::ceil(2 * reach)) + 1);
  const Field R = responseField(gray, spec.bright, spec.widthMax, roi);
  const Field g = smoothGrayField(gray, spec.bright, roi);

  // D6：扇形搜索。每条射线在检测区前段（≤ 150 px）上取响应的均值
  const double lo = spec.useHeading ? spec.headingDeg - spec.headingTolDeg : spec.sectorFromDeg;
  const double hi = spec.useHeading ? spec.headingDeg + spec.headingTolDeg : spec.sectorToDeg;
  const double fanLen = std::min(spec.fanLength, spec.zoneEnd - spec.zoneStart);
  std::vector<std::pair<double, double>> fan;  // (score, angle)
  for (double a = lo; a <= hi + 1e-6; a += 1.0) {
    const P2 d = dirOfDeg(a);
    int total = 0, bad = 0;
    double sum = 0;
    for (double t = spec.zoneStart; t < spec.zoneStart + fanLen - 1e-9; t += spec.fanStep) {
      const double v = R.at(spec.nozzle + d * t);
      total += 1;
      if (v < 0) {
        bad += 1;
      } else {
        sum += v;
      }
    }
    const double score = total > 0 && bad < 0.3 * total ? sum / total : -1.0;
    fan.emplace_back(score, a);
  }
  std::stable_sort(fan.begin(), fan.end(),
                   [](const auto& x, const auto& y) { return x.first > y.first; });
  std::vector<std::pair<double, double>> picks;
  for (const auto& f : fan) {
    bool far = true;
    for (const auto& p : picks) {
      if (angleDiffDeg(p.second, f.second) <= spec.fanSeparationDeg) far = false;
    }
    if (far) picks.push_back(f);
    if (picks.size() == 3) break;
  }

  // 前三个峰各粗找一遍，按「覆盖率 ×（像胶 ? 1 : 0.1）」留一个（D15）。射线本身几乎没有响应（不到最强那条的
  // 四分之一）的方向再打个对折：它是靠 ±widthMax 的卡尺从旁边「蹭」上胶的，起点落在胶外面；而真正的方向
  // 只要胶路上有一段断口，覆盖率就会比它低一点（断口正好在检测区前段时尤其如此）
  const double fanMax = picks.empty() ? 0.0 : picks.front().first;
  int best = -1;
  for (const auto& pk : picks) {
    PathCandidate c = coarseTrack(R, g, spec, pk.second, lo, hi);
    c.fanScore = pk.first;
    if (fanMax > 0 && pk.first < 0.25 * fanMax) c.score *= 0.5;
    out.candidates.push_back(std::move(c));
    const PathCandidate& cc = out.candidates.back();
    if (best < 0 || cc.score > out.candidates[static_cast<std::size_t>(best)].score) {
      best = static_cast<int>(out.candidates.size()) - 1;
    }
  }
  if (best < 0) {
    out.reason = "扇区里一个方向都没有（检查 sector / heading）";
    out.headingDeg = 0.5 * (lo + hi);
    straightPath(spec.nozzle, out.headingDeg, spec.zoneStart, spec.zoneEnd, out);
    out.residual = kNaN;
    return out;
  }
  const PathCandidate& chosen = out.candidates[static_cast<std::size_t>(best)];
  out.headingDeg = chosen.headingDeg;
  out.coverage = chosen.coverage;
  out.sharpness = chosen.sharpness;
  out.beadlike = chosen.beadlike;
  for (const auto& c : chosen.coarse) out.coarse.push_back(c.second);

  if (chosen.coverage < spec.minCoverage) {
    out.reason = "覆盖率 " + fmt2(chosen.coverage) + " 低于 minCoverage " + fmt2(spec.minCoverage);
  } else if (!chosen.beadlike) {
    out.reason = "找到的暗带边缘太缓（陡度 P25 = " + fmt2(chosen.sharpness) + " < sharpMin " +
                 fmt2(spec.sharpMin) + "），像压痕或阴影，不像胶";
  } else if (chosen.coarse.size() < 3) {
    out.reason = "粗找到的胶点不到 3 个";
  }
  if (!out.reason.empty()) {
    straightPath(spec.nozzle, out.headingDeg, spec.zoneStart, spec.zoneEnd, out);
    out.residual = kNaN;
    return out;
  }

  // 拟合网格往两头各多走一截（局部线性拟合在那里就是沿端点切向外推）：起点要落在「离喷嘴 zoneStart」
  // 的那一处，终点要够得着 zoneEnd，下面的高斯平滑也不能让两头的截断影响到检测区里
  std::vector<double> grid;
  for (double s = spec.zoneStart - 48.0; s <= spec.zoneEnd + 56.0 + 1e-6; s += 1.0) grid.push_back(s);
  std::vector<P2> dense = localLinearFit(chosen.coarse, grid, spec.smoothWindow, nullptr);
  // 检测区开头一段没找到胶（断口正好落在那里）：局部线性拟合会沿后面几个胶点的切向往回外推，拐错了方向
  // 就是一条横着的胶路。胶是从喷嘴出来的 —— 第一个胶点之前的那一段取喷嘴到它的连线
  const double sFirst = chosen.coarse.front().first;
  if (sFirst > spec.zoneStart + 2.0 * spec.coarseStep) {
    std::size_t k = 0;
    while (k + 1 < grid.size() && grid[k] < sFirst) ++k;
    const P2 anchor = dense[k];
    for (std::size_t i = 0; i < k; ++i) {
      dense[i] = spec.nozzle + (anchor - spec.nozzle) * (grid[i] / sFirst);
    }
  }
  // 再做一次 σ = 12 px 的高斯平滑：粗找 8 px 一步，螺旋胶的胶点左右摆，局部线性拟合会留下与螺距同频的
  // 小摆动 —— 胶路不光滑，按弧长算的 s 就比实际长出一截（300 px 的检测区能长出 20 px），断口的位置跟着漂
  std::vector<P2> smooth(dense.size());
  constexpr int kHalf = 36;
  constexpr double kSigma = 12.0;
  for (std::size_t i = 0; i < dense.size(); ++i) {
    P2 acc(0, 0);
    double wsum = 0;
    for (int k = -kHalf; k <= kHalf; ++k) {
      const std::ptrdiff_t j = static_cast<std::ptrdiff_t>(i) + k;
      if (j < 0 || j >= static_cast<std::ptrdiff_t>(dense.size())) continue;
      const double w = std::exp(-0.5 * k * k / (kSigma * kSigma));
      acc += dense[static_cast<std::size_t>(j)] * w;
      wsum += w;
    }
    smooth[i] = acc * (1.0 / wsum);
  }
  resampleByArcLength(smooth, spec.nozzle, spec.zoneStart, spec.zoneEnd, out);
  std::vector<double> resid;
  for (const P2& q : out.coarse) {
    double bestD = std::numeric_limits<double>::infinity();
    for (const P2& p : out.points) bestD = std::min(bestD, length(p - q));
    resid.push_back(bestD);
  }
  out.residual = median(resid);
  out.ok = true;
  out.fallback = false;
  return out;
}

void PolylineView::at(double sq, P2* point, P2* tangent) const {
  const std::vector<double>& S = *s;
  const std::vector<P2>& P = *points;
  const std::vector<P2>& T = *tangents;
  if (sq <= S.front()) {
    *point = P.front();
    *tangent = T.front();
    return;
  }
  if (sq >= S.back()) {
    *point = P.back();
    *tangent = T.back();
    return;
  }
  const auto it = std::upper_bound(S.begin(), S.end(), sq);
  const std::size_t j = static_cast<std::size_t>(it - S.begin());
  const std::size_t i = j - 1;
  const double span = S[j] - S[i];
  const double f = span > 1e-12 ? (sq - S[i]) / span : 0.0;
  *point = P[i] + (P[j] - P[i]) * f;
  *tangent = unit(T[i] + (T[j] - T[i]) * f);
}

// ------------------------------------------------------------------ 示教胶路

double polylineLength(const std::vector<P2>& pts) {
  double total = 0;
  for (std::size_t i = 1; i < pts.size(); ++i) total += length(pts[i] - pts[i - 1]);
  return total;
}

bool resampleTaught(const std::vector<P2>& pts, double zoneStart, double zoneEnd,
                    std::vector<double>* s, std::vector<P2>* points, std::vector<P2>* tangents) {
  std::vector<P2> q;
  std::vector<double> arc;
  for (const P2& p : pts) {
    if (!finite(p)) return false;
    if (!q.empty() && length(p - q.back()) < 1e-9) continue;
    arc.push_back(q.empty() ? 0.0 : arc.back() + length(p - q.back()));
    q.push_back(p);
  }
  if (q.size() < 2) return false;
  const double total = arc.back();
  const double z1 = zoneEnd > 0 ? zoneEnd : total;
  if (!(zoneStart >= 0) || !(z1 > zoneStart) || z1 > total + 1e-6) return false;

  std::vector<double> targets;
  for (double t = zoneStart; t < z1 - 1e-9; t += 2.0) targets.push_back(t);
  targets.push_back(std::min(z1, total));
  std::vector<P2> out;
  out.reserve(targets.size());
  std::size_t seg = 0;  // 这一段是 q[seg] → q[seg + 1]
  for (double t : targets) {
    while (seg + 2 < q.size() && arc[seg + 1] < t) ++seg;
    const double span = arc[seg + 1] - arc[seg];
    const double f = span > 1e-12 ? std::clamp((t - arc[seg]) / span, 0.0, 1.0) : 0.0;
    out.push_back(q[seg] + (q[seg + 1] - q[seg]) * f);
  }
  std::vector<P2> tan(out.size(), P2(1, 0));
  for (std::size_t i = 0; i < out.size(); ++i) {
    const std::size_t a = i == 0 ? 0 : i - 1;
    const std::size_t b = i + 1 < out.size() ? i + 1 : i;
    // 只有一个点（zone 短到不足 2 px 不会发生，validate 拦着）时退回所在那一段的方向
    tan[i] = b > a ? unit(out[b] - out[a]) : unit(q[seg + 1] - q[seg]);
  }
  *s = std::move(targets);
  *points = std::move(out);
  *tangents = std::move(tan);
  return true;
}

// ------------------------------------------------------------------ 量胶宽

StationResult measureStations(const cv::Mat& gray, const PolylineView& path, bool pathOk,
                              double zoneStart, double zoneEnd, bool bright, int responseWidthMax,
                              const StationSpec& spec) {
  StationResult out;
  if (!path.valid()) return out;
  for (double s = zoneStart; s <= zoneEnd + 1e-6; s += spec.step) {
    Station st;
    st.s = s;
    path.at(s, &st.c, &st.t);
    st.n = perp(st.t);
    out.stations.push_back(std::move(st));
  }
  out.expect.assign(out.stations.size(), 0.0);
  if (!pathOk) return out;

  // 示教胶路（T3）：胶离示教线最远 lateralTol，卡尺至少要够得着那里的整条胶（最宽 widthRange 上限）
  const bool taught = spec.lateralTol >= 0;
  const double half = taught ? std::max(spec.searchHalf, spec.lateralTol + 0.5 * spec.runs.widthMax)
                             : spec.searchHalf;
  // 响应图只算胶路两侧卡尺够得着的那一块
  double x0 = std::numeric_limits<double>::infinity(), y0 = x0, x1 = -x0, y1 = -x0;
  for (const P2& p : *path.points) {
    x0 = std::min(x0, p.x);
    y0 = std::min(y0, p.y);
    x1 = std::max(x1, p.x);
    y1 = std::max(y1, p.y);
  }
  const double pad = half + 4.0;
  const cv::Rect roi(static_cast<int>(std::floor(x0 - pad)), static_cast<int>(std::floor(y0 - pad)),
                     static_cast<int>(std::ceil(x1 - x0 + 2 * pad)) + 1,
                     static_cast<int>(std::ceil(y1 - y0 + 2 * pad)) + 1);
  const Field R = responseField(gray, bright, responseWidthMax, roi);
  for (Station& st : out.stations) st.runs = lateralRuns(R, st.c, st.n, half, spec.runs);

  // 每站拿哪一段去算参考宽 / 参考峰值：拟合的胶路取离胶路最近的那一段（原来的做法）；
  // 示教胶路只取落在偏移轨迹上的那一段（断口里、轨迹外的暗段不进参考）
  std::vector<const Run*> refRun(out.stations.size(), nullptr);
  if (!taught) {
    for (std::size_t i = 0; i < out.stations.size(); ++i) refRun[i] = nearestRun(out.stations[i].runs);
  } else {
    Track tr = trackOffsets(out.stations, spec.lateralTol, spec.centerRatio);
    out.expect = tr.expect;
    out.trackSupport = tr.support;
    std::vector<double> sharp, onTrackExpect;
    if (tr.count > 0) {
      // D15 用在整条轨迹上（T5）：压痕、阴影也是暗带，边缘却是缓的。逐站不卡（D15 的教训）
      const Field g = smoothGrayField(gray, bright, roi);
      for (std::size_t i = 0; i < out.stations.size(); ++i) {
        if (!tr.onTrack[i]) continue;
        onTrackExpect.push_back(tr.expect[i]);
        const double sh = runSharpness(g, out.stations[i].c, out.stations[i].n, *tr.onTrack[i]);
        if (std::isfinite(sh)) sharp.push_back(sh);
      }
      out.trackOffset = median(onTrackExpect);
    }
    out.trackSharpness = sharp.size() >= 3 ? percentile(sharp, 25.0) : kNaN;
    if (tr.reason.empty() && std::isfinite(out.trackSharpness) && out.trackSharpness < spec.sharpMin) {
      tr.reason = "示教线上的暗带边缘太缓（陡度 P25 = " + fmt2(out.trackSharpness) + " < sharpMin " +
                  fmt2(spec.sharpMin) + "），像压痕或阴影，不像胶";
    }
    if (!tr.reason.empty()) {
      out.trackReason = tr.reason;
      return out;  // 全部判无胶（T4）
    }
    refRun = tr.onTrack;
  }

  // D16：参考宽取全检测区候选暗段宽度的中位数（断口里的局部窗口会被零件表面的拉丝纹主导）
  std::vector<double> widths, peaks;
  for (const Run* r : refRun) {
    if (r) {
      widths.push_back(r->width());
      peaks.push_back(r->peak);
    }
  }
  out.wRef = widths.empty() ? 0.0 : median(widths);
  out.peakRef = peaks.empty() ? 0.0 : median(peaks);
  const double minWidth = spec.presentRatio * out.wRef;
  // 对比度也要一致：断口两头模糊出来的「胶的残影」宽度对、位置对，只是淡（真实满胶帧上每站的峰值都在
  // 中位数的 0.72 倍以上）
  const double minPeak = spec.contrastRatio * out.peakRef;

  if (!spec.swirl) {
    const double maxOffset = std::max(6.0, spec.centerRatio * out.wRef);
    for (std::size_t i = 0; i < out.stations.size(); ++i) {
      Station& st = out.stations[i];
      // 中心偏离的是期望的胶中心：拟合的胶路恒为 0，示教胶路是偏移轨迹（T3）
      const double e = out.expect[i];
      const Run* r = nearestRun(st.runs, e);
      if (!r || widths.empty() || r->width() < minWidth || std::fabs(r->mid() - e) > maxOffset ||
          r->peak < minPeak) {
        continue;
      }
      st.present = true;
      st.lo = r->lo;
      st.hi = r->hi;
      st.peak = r->peak;
    }
    return out;
  }

  // D5：螺旋胶取外包络。每站的池子 = ±window/2 内各站、离（期望的）胶中心不远的全部暗段
  const double halfWindow = 0.5 * spec.window;
  const double poolReach = 0.8 * spec.searchHalf;
  std::vector<std::vector<const Run*>> pools(out.stations.size());
  for (std::size_t i = 0; i < out.stations.size(); ++i) {
    for (std::size_t j = 0; j < out.stations.size(); ++j) {
      const Station& y = out.stations[j];
      if (std::fabs(y.s - out.stations[i].s) > halfWindow) continue;
      for (const Run& r : y.runs) {
        if (std::fabs(r.mid() - out.expect[j]) < poolReach) pools[i].push_back(&r);
      }
    }
  }
  std::vector<double> env;
  for (const auto& pool : pools) {
    if (pool.empty()) continue;
    double lo = std::numeric_limits<double>::infinity(), hi = -lo;
    for (const Run* r : pool) {
      lo = std::min(lo, r->lo);
      hi = std::max(hi, r->hi);
    }
    env.push_back(hi - lo);
  }
  out.envelopeHalf = env.empty() ? poolReach : 0.5 * median(env) + 4.0;
  for (std::size_t i = 0; i < out.stations.size(); ++i) {
    Station& st = out.stations[i];
    auto qualifies = [&](const Run& r) {
      return std::fabs(r.mid() - out.expect[i]) <= out.envelopeHalf && r.width() >= minWidth;
    };
    // 有无胶按本站自己判（窗口只用来算外包络）：按窗口判会把比窗口短的断口整个抹掉。
    // 对比度的一致性只卡「本站有没有胶」，不卡外包络：螺圈外沿的那几股本来就淡一些
    bool own = false;
    for (const Run& r : st.runs) own = own || (qualifies(r) && r.peak >= minPeak);
    if (!own || widths.empty()) continue;
    double lo = std::numeric_limits<double>::infinity(), hi = -lo, peak = 0;
    for (const Run* r : pools[i]) {
      if (!qualifies(*r)) continue;
      lo = std::min(lo, r->lo);
      hi = std::max(hi, r->hi);
      peak = std::max(peak, r->peak);
    }
    st.present = true;
    st.lo = lo;
    st.hi = hi;
    st.peak = peak;
  }
  return out;
}

}  // namespace lyflow::packs::glue
