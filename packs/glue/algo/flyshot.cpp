#include "algo/flyshot.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <opencv2/imgproc.hpp>

namespace lyflow::packs::glue {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

/// 三点抛物线的顶点相对中点的偏移（−0.5 … 0.5）。不是尖峰（开口朝上或平）时返回 0。
double parabolaPeak(double a, double b, double c) {
  const double den = a - 2.0 * b + c;
  if (!(den < 0.0))
    return 0.0;
  return std::clamp(0.5 * (a - c) / den, -0.5, 0.5);
}

/// 双线性采样；落出图外返回 NaN。
double sample(const cv::Mat& g, double x, double y) {
  if (!(x >= 0.0 && y >= 0.0 && x <= g.cols - 1.0 && y <= g.rows - 1.0))
    return kNaN;
  const int x0 = std::min(static_cast<int>(x), g.cols - 2);
  const int y0 = std::min(static_cast<int>(y), g.rows - 2);
  const double ax = x - x0, ay = y - y0;
  const uchar* r0 = g.ptr<uchar>(y0) + x0;
  const uchar* r1 = g.ptr<uchar>(y0 + 1) + x0;
  const double top = r0[0] + ax * (r0[1] - r0[0]);
  const double bottom = r1[0] + ax * (r1[1] - r1[0]);
  return top + ay * (bottom - top);
}

} // namespace

P2 Pose2D::rotate(P2 v) const {
  const double a = angleDeg * CV_PI / 180.0;
  const double c = std::cos(a), s = std::sin(a);
  return P2(c * v.x - s * v.y, s * v.x + c * v.y);
}

P2 Pose2D::apply(P2 p) const { return rotate(p - teachCenter) + center; }

Pose2D locateTemplate(const cv::Mat& gray, const cv::Mat& tmpl, const LocateSpec& spec,
                      std::string* why) {
  Pose2D pose;
  auto fail = [&](const char* code, const char* m) {
    pose.ok = false;
    pose.reason = code;
    if (why)
      *why = m;
    return pose;
  };
  if (gray.empty() || tmpl.empty() || gray.type() != CV_8UC1 || tmpl.type() != CV_8UC1) {
    // 算子在进来之前已经把这种输入拒成执行错误；直接调内核时没有图可搜，归到 out_of_image
    return fail("out_of_image", "图像和模板需要非空 u8 灰度图");
  }
  const int tw = tmpl.cols, th = tmpl.rows;
  pose.teachCenter = spec.anchor + P2((tw - 1) / 2.0, (th - 1) / 2.0);
  pose.center = pose.teachCenter;
  cv::Scalar mean, deviation;
  cv::meanStdDev(tmpl, mean, deviation);
  // OpenCV 对常量模板的 TM_CCOEFF_NORMED 返回 1；没有纹理不能作为成功定位的证据。
  if (deviation[0] < 1.0)
    return fail("flat_template", "模板对比度不足，请选择含边缘或纹理的模板");
  // 模板旋转后外接圆的半径 + 搜索半径：任何允许的位姿下模板都完整落在这一块里
  const double half = spec.searchRadius + 0.5 * std::hypot(tw, th) + 4.0;
  const cv::Rect want(static_cast<int>(std::floor(pose.teachCenter.x - half)),
                      static_cast<int>(std::floor(pose.teachCenter.y - half)),
                      static_cast<int>(std::ceil(2.0 * half)) + 1,
                      static_cast<int>(std::ceil(2.0 * half)) + 1);
  const cv::Rect roi = want & cv::Rect(0, 0, gray.cols, gray.rows);
  if (roi.width < tw + 2 || roi.height < th + 2)
    return fail("out_of_image", "搜索区落在图外，或者比模板还小");
  const cv::Mat sub = gray(roi);
  const P2 rc((roi.width - 1) / 2.0, (roi.height - 1) / 2.0);

  /// 一个角度上的匹配：搜索窗（转过的图里、按模板左上角计）里的 NCC 响应与其中的最高点。
  struct Match {
    double phi = 0;
    double score = 0;
    P2 loc;            ///< 最高点，含亚像素插值（转过的图里模板左上角）
    bool atEdge = true; ///< 最高点离搜索窗边 ≤ 1 px；没有搜索窗也算
    int u0 = 0, v0 = 0; ///< 搜索窗左上角在转过的图里的位置
    cv::Mat window;    ///< 搜索窗里的响应；与 full 共享缓冲
  };

  // 把搜索区转 phi 度（绕它的中心），在转过的图里做 NCC：物体在原图里就是转了 −phi
  auto matchAt = [&](double phiDeg) {
    Match m;
    m.phi = phiDeg;
    // rot 不能先指向 sub：warpAffine 会写进共享的缓冲区，把输入图本身改掉
    cv::Mat rot;
    if (phiDeg == 0.0) {
      rot = sub;
    } else {
      const double a = phiDeg * CV_PI / 180.0;
      const double c = std::cos(a), s = std::sin(a);
      const cv::Matx23d M(c, -s, rc.x - c * rc.x + s * rc.y, s, c, rc.y - s * rc.x - c * rc.y);
      cv::warpAffine(sub, rot, M, sub.size(), cv::INTER_LINEAR, cv::BORDER_REPLICATE);
    }
    cv::Mat full;
    cv::matchTemplate(rot, tmpl, full, cv::TM_CCOEFF_NORMED);
    // 只认模板中心离示教位置不超过 searchRadius 的匹配（转过的图里，示教中心也跟着转）
    const P2 tcRoi = pose.teachCenter - P2(roi.x, roi.y);
    const double a = phiDeg * CV_PI / 180.0;
    const P2 d = tcRoi - rc;
    const P2 want(std::cos(a) * d.x - std::sin(a) * d.y + rc.x - (tw - 1) / 2.0,
                  std::sin(a) * d.x + std::cos(a) * d.y + rc.y - (th - 1) / 2.0);
    const int u0 = std::max(0, static_cast<int>(std::floor(want.x - spec.searchRadius)));
    const int v0 = std::max(0, static_cast<int>(std::floor(want.y - spec.searchRadius)));
    const int u1 = std::min(full.cols - 1, static_cast<int>(std::ceil(want.x + spec.searchRadius)));
    const int v1 = std::min(full.rows - 1, static_cast<int>(std::ceil(want.y + spec.searchRadius)));
    if (u1 < u0 || v1 < v0) {
      m.loc = want;
      return m;
    }
    m.u0 = u0;
    m.v0 = v0;
    m.window = full(cv::Rect(u0, v0, u1 - u0 + 1, v1 - v0 + 1));
    const cv::Mat& res = m.window;
    double best = 0;
    cv::Point at;
    cv::minMaxLoc(res, nullptr, &best, nullptr, &at);
    double dx = 0, dy = 0;
    if (at.x > 0 && at.x + 1 < res.cols) {
      dx = parabolaPeak(res.at<float>(at.y, at.x - 1), best, res.at<float>(at.y, at.x + 1));
    }
    if (at.y > 0 && at.y + 1 < res.rows) {
      dy = parabolaPeak(res.at<float>(at.y - 1, at.x), best, res.at<float>(at.y + 1, at.x));
    }
    // 搜索窗被图边裁过时，裁出来的边同样是边：模板再往外就出图了
    m.atEdge = at.x <= 1 || at.y <= 1 || at.x >= res.cols - 2 || at.y >= res.rows - 2;
    m.loc = P2(u0 + at.x + dx, v0 + at.y + dy);
    m.score = std::isfinite(best) ? best : 0.0;
    return m;
  };

  const double step = std::max(spec.angleStep, 0.01);
  const int n =
      spec.angleRange > 0 ? static_cast<int>(std::floor(spec.angleRange / step + 1e-9)) : 0;
  std::vector<Match> coarse;
  coarse.reserve(2 * n + 1);
  std::vector<double> scores(2 * n + 1);
  int bestI = n;
  for (int i = -n; i <= n; ++i) {
    coarse.push_back(matchAt(i * step));
    scores[i + n] = coarse.back().score;
    if (scores[i + n] > scores[bestI])
      bestI = i + n;
  }
  double phi = (bestI - n) * step;
  if (n > 0) {
    // 粗扫的峰附近再按 step/4 细扫五个角度，取最好的三个做抛物线
    const double fine = step / 4.0;
    // 第一轮就会和中间角度比较，必须先填入粗扫分数，不能读未初始化的栈内存。
    const double best = scores[bestI];
    double fineScores[5] = {0, 0, best, 0, 0};
    int bestK = 2;
    for (int k = -2; k <= 2; ++k) {
      fineScores[k + 2] = k == 0 ? best : matchAt(phi + k * fine).score;
      if (fineScores[k + 2] > fineScores[bestK])
        bestK = k + 2;
    }
    phi += (bestK - 2) * fine;
    if (bestK > 0 && bestK < 4) {
      phi += fine * parabolaPeak(fineScores[bestK - 1], fineScores[bestK], fineScores[bestK + 1]);
    }
    phi = std::clamp(phi, -n * step, n * step);
  }
  const Match final = matchAt(phi);
  pose.score = final.score;

  // 转过 phiDeg 的图里的点 q 与原图坐标互换（绕搜索区中心转）
  auto toImage = [&](double phiDeg, P2 q) {
    Pose2D back;
    back.angleDeg = -phiDeg;
    return back.rotate(q - rc) + rc + P2(roi.x, roi.y);
  };
  auto toRotated = [&](double phiDeg, P2 p) {
    Pose2D fwd;
    fwd.angleDeg = phiDeg;
    return fwd.rotate(p - P2(roi.x, roi.y) - rc) + rc;
  };
  const P2 halfT((tw - 1) / 2.0, (th - 1) / 2.0);
  pose.center = toImage(phi, final.loc + halfT);
  pose.angleDeg = -phi;

  // peakRatio：所有粗扫角度的搜索窗里，模板中心离主峰超过 exclusion（原图距离）的最强响应 / 主峰分数。
  // 主峰的侧坡、邻近角度上的同一个峰都落在 exclusion 里，不算第二个峰。
  // 旋转保距：把主峰中心转进每个角度的图里比距离，不必逐点转回原图。
  if (pose.score > 0) {
    const double exclusion = std::max(3.0, std::max(tw, th) / 4.0);
    double second = 0;
    for (const Match& m : coarse) {
      const P2 c = toRotated(m.phi, pose.center) - halfT - P2(m.u0, m.v0); // 窗口坐标
      for (int y = 0; y < m.window.rows; ++y) {
        const float* row = m.window.ptr<float>(y);
        for (int x = 0; x < m.window.cols; ++x) {
          const double v = row[x];
          if (v > second && std::hypot(x - c.x, y - c.y) > exclusion)
            second = v;
        }
      }
    }
    // 粗扫上别处的峰可能略高于细扫后的主峰分数；比 1 大没有更多信息，夹到 1，maxPeakRatio = 1 时门永远不开
    pose.peakRatio = std::min(1.0, second / pose.score);
  }

  if (pose.score < spec.minScore)
    return fail("low_score", "匹配分数低于 minScore");
  if (final.atEdge)
    return fail("at_search_edge", "匹配峰贴在搜索窗边上：真实位置可能超出 searchRadius");
  if (n > 0 && (bestI == 0 || bestI == 2 * n))
    return fail("at_angle_edge", "最佳角度在扫描范围的端点上：真实转角可能超出 angleRange");
  if (pose.peakRatio > spec.maxPeakRatio) {
    return fail("ambiguous", "搜索区里还有一处几乎一样像的匹配（peakRatio 超过 maxPeakRatio），位置不唯一");
  }
  pose.ok = true;
  return pose;
}

CaliperStation measureCaliper(const cv::Mat& gray, P2 c, P2 n, const CaliperSpec& spec) {
  CaliperStation st;
  st.inner = st.nearEdge = st.farEdge = kNaN;
  if (gray.type() != CV_8UC1 || gray.cols < 2 || gray.rows < 2 || !finite(c) || !finite(n)) {
    st.status = "out_of_image";
    return st;
  }
  const double half = std::max(spec.searchHalf, 8.0);
  const int m = static_cast<int>(std::round(4.0 * half)) + 1; // 0.5 px 一步
  auto tOf = [&](int i) { return -half + 0.5 * i; };
  auto iOf = [&](double t) { return static_cast<int>(std::ceil((t + half) * 2.0 - 1e-9)); };

  const P2 across = perp(n);
  const int w = std::max(1, static_cast<int>(std::round(spec.caliperWidth)));
  std::vector<double> raw(m);
  for (int i = 0; i < m; ++i) {
    double acc = 0;
    for (int k = 0; k < w; ++k) {
      const double u = k - (w - 1) / 2.0;
      const P2 p = c + n * tOf(i) + across * u;
      const double v = sample(gray, p.x, p.y);
      if (!std::isfinite(v)) {
        st.status = "out_of_image";
        return st;
      }
      acc += v;
    }
    raw[i] = acc / w;
  }
  // σ = 1 px（两个样本）的高斯
  std::vector<double> v(m);
  {
    constexpr int r = 4;
    double kernel[2 * r + 1];
    for (int k = -r; k <= r; ++k)
      kernel[k + r] = std::exp(-0.5 * (k * k) / 4.0);
    for (int i = 0; i < m; ++i) {
      double acc = 0, wsum = 0;
      for (int k = -r; k <= r; ++k) {
        const int j = i + k;
        if (j < 0 || j >= m)
          continue;
        acc += kernel[k + r] * raw[j];
        wsum += kernel[k + r];
      }
      v[i] = acc / wsum;
    }
  }

  // 内边：±1…3 px 两侧均值之差按极性取正，局部极大、≥ contrastMin 的都是候选
  const double sign = spec.innerDarkToBright ? 1.0 : -1.0;
  auto edgeAt = [&](int i) {
    double lo = 0, hi = 0;
    for (int k = 2; k <= 6; ++k) {
      lo += v[i - k];
      hi += v[i + k];
    }
    return sign * (hi - lo) / 5.0;
  };
  const int a = std::max(iOf(spec.innerFrom), 7);
  const int b = std::min(iOf(spec.innerTo), m - 8);
  int pick = -1;
  double pickE = 0;
  for (int i = a; i <= b; ++i) {
    const double e = edgeAt(i);
    if (e < spec.contrastMin || e < edgeAt(i - 1) || e <= edgeAt(i + 1))
      continue;
    if (pick < 0 || (spec.innerNearest ? i > pick : e > pickE)) {
      pick = i;
      pickE = e;
    }
  }
  if (pick < 0) {
    st.status = "no_inner";
    return st;
  }
  st.inner = tOf(pick) + 0.5 * parabolaPeak(edgeAt(pick - 1), pickE, edgeAt(pick + 1));
  st.innerContrast = pickE;

  // 胶：内边之后的窗口里取最暗处，按本站的半高定两条边
  const int i0 = std::max(iOf(std::max(st.inner + 1.0, spec.beadFrom)), 0);
  const int i1 = std::min(iOf(spec.beadTo), m - 1);
  if (i1 - i0 < 4) {
    st.status = "no_bead";
    return st;
  }
  std::vector<double> d(v.begin() + i0, v.begin() + i1 + 1);
  if (!spec.beadDark) {
    for (double& x : d)
      x = 255.0 - x;
  }
  std::vector<double> sorted = d;
  const std::size_t q80 = (sorted.size() * 4) / 5;
  std::nth_element(sorted.begin(), sorted.begin() + q80, sorted.end());
  const double bg = sorted[q80];
  // 窗口里可能还有别的暗东西（安装孔、阴影）：取离站点（名义胶条中线 t = 0）最近的暗谷，而不是最暗的那个
  const int nd = static_cast<int>(d.size());
  int valley = -1;
  for (int j = 1; j + 1 < nd; ++j) {
    if (d[j] > d[j - 1] || d[j] >= d[j + 1] || bg - d[j] < spec.contrastMin)
      continue;
    if (valley < 0 || std::fabs(tOf(i0 + j)) < std::fabs(tOf(i0 + valley)))
      valley = j;
  }
  if (valley < 0) {
    st.status = "no_bead";
    return st;
  }
  // 把这个暗谷按它自己的半高展开，谷底取展开范围里的最小值（胶底的噪声会造出好几个局部极小）
  int mi = valley;
  {
    const double level0 = 0.5 * (bg + d[valley]);
    int a = valley, b = valley;
    while (a > 0 && d[a - 1] < level0)
      --a;
    while (b + 1 < nd && d[b + 1] < level0)
      ++b;
    mi = static_cast<int>(std::min_element(d.begin() + a, d.begin() + b + 1) - d.begin());
  }
  st.beadContrast = bg - d[mi];
  const double level = 0.5 * (bg + d[mi]);
  // 没有跨过半高的边不能用窗口边界代替；否则会把截断的胶宽当成合格量测。
  for (int j = mi; j >= 0; --j) {
    if (d[j] >= level) {
      const double frac = (d[j] - level) / std::max(d[j] - d[j + 1], 1e-9);
      st.nearEdge = tOf(i0 + j) + 0.5 * frac;
      break;
    }
  }
  for (int j = mi; j < static_cast<int>(d.size()); ++j) {
    if (d[j] >= level) {
      const double frac = (level - d[j - 1]) / std::max(d[j] - d[j - 1], 1e-9);
      st.farEdge = tOf(i0 + j - 1) + 0.5 * frac;
      break;
    }
  }
  if (!std::isfinite(st.nearEdge) || !std::isfinite(st.farEdge)) {
    st.status = "incomplete_bead";
    st.nearEdge = st.farEdge = kNaN;
  } else if (st.farEdge - st.nearEdge < spec.widthMin) {
    st.status = "no_bead";
    st.nearEdge = st.farEdge = kNaN;
  }
  return st;
}

} // namespace lyflow::packs::glue
