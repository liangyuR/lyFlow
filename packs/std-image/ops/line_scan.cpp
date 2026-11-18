#include "lyflow_cv/line_scan.h"

#include <algorithm>

#include <opencv2/imgproc.hpp>

namespace lyflow::std_image::line_scan {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

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

cv::Rect topHatBounds(cv::Rect inner, cv::Rect image, int radius) {
  return inflate(inner, 2 * radius + 4) & image;
}

cv::Mat topHat(const cv::Mat& src, const cv::Mat& morph, bool bright) {
  cv::Mat response;
  cv::subtract(bright ? src : morph, bright ? morph : src, response, cv::noArray(), CV_32F);
  return response;
}

void blur5(cv::Mat& image, double sigma) {
  if (sigma > 0)
    cv::GaussianBlur(image, image, cv::Size(5, 5), sigma);
}

cv::Mat grayPlane(const cv::Mat& src, double sigma, bool bright) {
  cv::Mat gray;
  src.convertTo(gray, CV_32F);
  blur5(gray, sigma);
  if (bright)
    gray = 255.0 - gray;
  return gray;
}

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

} // namespace

Field responseField(const cv::Mat& gray, bool bright, int widthMax, cv::Rect roi, Field* level) {
  const cv::Rect inner = roi & imageRect(gray);
  if (level)
    *level = Field();
  if (inner.empty())
    return {};
  const Octagon o = octagon(widthMax + 1);
  // 闭运算里膨胀与腐蚀各走一次结构元，再加上 5×5 的高斯：外扩两倍半径 + 4 之后，inner 里的值
  // 与整幅图算出来的一样（外扩碰到图像边界时两边的边界规则也一样）
  const cv::Rect outer = topHatBounds(inner, imageRect(gray), o.radius);
  const cv::Mat src = gray(outer).clone();
  const cv::Mat morph = closeOrOpen(src, o, bright);
  cv::Mat r = topHat(src, morph, bright);
  blur5(r, 1.2);
  if (level) {
    cv::Mat l = grayPlane(bright ? src : morph, 1.2, false);
    *level = Field(l, outer.tl(), inner);
  }
  return Field(r, outer.tl(), inner);
}

Field smoothGrayField(const cv::Mat& gray, bool bright, cv::Rect roi) {
  const cv::Rect inner = roi & imageRect(gray);
  if (inner.empty())
    return {};
  const cv::Rect outer = inflate(inner, 4) & imageRect(gray);
  cv::Mat g = grayPlane(gray(outer), 1.0, bright);
  return Field(g, outer.tl(), inner);
}

ResponseMaps responseMaps(const cv::Mat& gray, const ResponseSpec& spec) {
  const cv::Rect all = imageRect(gray);
  const cv::Rect inner = (spec.roi.width > 0 && spec.roi.height > 0 ? spec.roi : all) & all;
  ResponseMaps out;
  if (inner.empty())
    return out;
  out.frame = inner;
  // 与 responseField 同一套外扩：inner 里的值与整幅图逐位相同。clone 让形态学只看到外扩块
  // （OpenCV 的滤波默认会读子矩阵外的父图像素）
  const Octagon o = octagon(spec.diameter);
  const cv::Rect outer = topHatBounds(inner, all, o.radius);
  const cv::Mat src = gray(outer).clone();
  const cv::Mat morph = closeOrOpen(src, o, spec.bright);
  // 5×5 的平滑只要块外 2 px：之后的几步只在这一圈里做（形态学要的那一大圈外扩到这里已经用完了）。
  // 块里每个点的 5×5 邻域都落在这一圈里，碰到图像边界时与整幅计算一样截在图边，所以结果一位不差
  const cv::Rect near = inflate(inner, 2) & all;
  const cv::Rect nl(near.tl() - outer.tl(), near.size());
  const cv::Mat srcN = src(nl), morphN = morph(nl);
  cv::Mat r = topHat(srcN, morphN, spec.bright);
  // 八边形拆成三次形态学串着做，贴图边（两倍半径以内）时中间那一步出了图，闭运算会比原图还低一点：
  // 负的响应按图层契约截到 0。
  cv::max(r, 0.0, r);
  blur5(r, spec.responseSigma);
  const cv::Mat l = grayPlane(spec.bright ? srcN : morphN, spec.levelSigma, false);
  const cv::Mat g = grayPlane(srcN, spec.graySigma, spec.bright);
  // 只交出块内：整幅不再分配、不再填 NaN，执行器给每张输出做的统计也只扫这一块
  const cv::Rect local(inner.tl() - near.tl(), inner.size());
  const auto place = [&](const cv::Mat& part) {
    return near == inner ? part : part(local).clone();
  };
  out.response = place(r);
  out.level = place(l);
  out.gray = place(g);
  return out;
}

Field grayFieldAround(const cv::Mat& gray, P2 minCenter, P2 maxCenter, double reach) {
  const cv::Rect roi(static_cast<int>(std::floor(minCenter.x - reach)),
                     static_cast<int>(std::floor(minCenter.y - reach)),
                     static_cast<int>(std::ceil(maxCenter.x - minCenter.x + 2 * reach)) + 1,
                     static_cast<int>(std::ceil(maxCenter.y - minCenter.y + 2 * reach)) + 1);
  const cv::Rect inner = roi & imageRect(gray);
  cv::Mat plane;
  if (!inner.empty())
    gray(inner).convertTo(plane, CV_32F);
  return Field(plane, inner.tl(), inner);
}

std::vector<double> sampleCaliper(const Field& response, P2 c, P2 n, double half) {
  const int count = static_cast<int>(std::floor(2.0 * half / kCaliperStep + 1e-9)) + 1;
  std::vector<double> samples(static_cast<std::size_t>(std::max(count, 0)));
  for (int i = 0; i < count; ++i)
    samples[i] = response.at(c + n * (-half + i * kCaliperStep));
  return samples;
}

std::vector<Run> caliperRuns(const Field& response, P2 c, P2 n, double half,
                             const RunSplitSpec& spec, std::vector<Run>* rawOut,
                             double* validFraction) {
  return caliperRuns(sampleCaliper(response, c, n, half), half, spec, rawOut, validFraction);
}

std::vector<Run> caliperRuns(const std::vector<double>& samples, double half,
                             const RunSplitSpec& spec, std::vector<Run>* rawOut,
                             double* validFraction) {
  if (validFraction) {
    const auto n = std::count_if(samples.begin(), samples.end(), [](double x) { return x >= 0.0; });
    *validFraction = samples.empty() ? 0.0 : static_cast<double>(n) / samples.size();
  }
  auto raw = rawCaliperRuns(samples, half, spec);
  auto merged = mergeCaliperRuns(raw, spec);
  if (rawOut)
    *rawOut = std::move(raw);
  return merged;
}

std::vector<Run> rawCaliperRuns(const std::vector<double>& samples, double half,
                                const RunSplitSpec& spec) {
  const int count = static_cast<int>(samples.size());
  const auto valid = [&](int i) { return samples[i] >= 0.0; };
  const auto value = [&](int i) { return valid(i) ? samples[i] : 0.0; };
  auto tAt = [&](int i) { return -half + i * kCaliperStep; };
  const double thr = spec.segmentMin;
  std::vector<Run> raw;
  for (int i = 0; i < count;) {
    if (!(valid(i) && samples[i] > thr)) {
      ++i;
      continue;
    }
    int j = i;
    while (j + 1 < count && valid(j + 1) && samples[j + 1] > thr)
      ++j;
    double peak = 0;
    for (int k = i; k <= j; ++k)
      peak = std::max(peak, samples[k]);
    if (peak >= spec.peakMin) {
      // 半高处取边，按这一段自己的峰值
      const double lvl = 0.5 * peak;
      for (int a = i; a <= j;) {
        while (a <= j && samples[a] < lvl)
          ++a;
        if (a > j)
          break;
        int b = a;
        if (spec.halfMax) {
          while (b < j && samples[b + 1] >= lvl)
            ++b;
        } else {
          b = j;
          while (b > a && samples[b] < lvl)
            --b;
        }
        const double la = a > 0 ? tAt(a) - (samples[a] - lvl) /
                                               std::max(samples[a] - value(a - 1), 1e-6) *
                                               kCaliperStep
                                : tAt(a);
        const double lb = b < count - 1 ? tAt(b) + (samples[b] - lvl) /
                                                       std::max(samples[b] - value(b + 1), 1e-6) *
                                                       kCaliperStep
                                        : tAt(b);
        double localPeak = 0;
        for (int k = a; k <= b; ++k)
          localPeak = std::max(localPeak, samples[k]);
        if (localPeak >= spec.peakMin) {
          Run r;
          r.lo = la;
          r.hi = lb;
          r.peak = localPeak;
          r.loComplete = a > 0 && valid(a - 1) && samples[a - 1] < lvl;
          r.hiComplete = b < count - 1 && valid(b + 1) && samples[b + 1] < lvl;
          r.completeEdges = r.loComplete && r.hiComplete;
          raw.push_back(r);
        }
        a = b + 1;
      }
    }
    i = j + 1;
  }
  return raw;
}

std::vector<Run> mergeCaliperRuns(const std::vector<Run>& raw, const RunSplitSpec& spec) {
  std::vector<Run> merged;
  for (const Run& r : raw) {
    if (!merged.empty() && r.lo - merged.back().hi <= spec.mergeGap &&
        r.hi - merged.back().lo <= spec.mergeWidthMax &&
        (spec.mergePeakRatio <= 0 ||
         std::min(r.peak, merged.back().peak) >=
             spec.mergePeakRatio * std::max(r.peak, merged.back().peak))) {
      Run& m = merged.back();
      m.completeEdges = m.completeEdges && r.completeEdges;
      m.hiComplete = r.hiComplete;
      m.hi = r.hi;
      m.peak = std::max(m.peak, r.peak);
      m.parts += r.parts;
    } else {
      merged.push_back(r);
    }
  }
  return merged;
}

RunEdges runEdges(const Field& g, P2 c, P2 n, const Run& run, double edgeProbe,
                  double backgroundProbe) {
  RunEdges out;
  auto at = [&](double t) { return g.at(c + n * t); };
  const double w = run.width();
  const double samples[6] = {at(run.lo - edgeProbe),       at(run.lo + edgeProbe),
                             at(run.hi + edgeProbe),       at(run.hi - edgeProbe),
                             at(run.lo - backgroundProbe), at(run.hi + backgroundProbe)};
  double inside = 0;
  for (int k = 0; k < 5; ++k) {
    const double v = at(run.lo + 0.25 * w + (0.5 * w) * k / 4.0);
    if (v < 0)
      return out;
    inside += v;
  }
  for (double v : samples) {
    if (v < 0)
      return out;
  }
  inside /= 5.0;
  out.edgeLo = samples[0] - samples[1];
  out.edgeHi = samples[2] - samples[3];
  out.depth = std::max(1.0, 0.5 * (samples[4] + samples[5]) - inside);
  out.sharpness = std::min(out.edgeLo, out.edgeHi) / out.depth;
  return out;
}

double angleDiffDeg(double a, double b) {
  double d = std::fmod(std::fabs(a - b), 360.0);
  return d > 180.0 ? 360.0 - d : d;
}

FanResult fanScan(const Field& R, const FanSpec& spec) {
  FanResult out;
  for (double t = spec.r0; t < spec.r1 - 1e-9; t += spec.radialStep)
    out.samples += 1;
  for (double a = spec.from; a <= spec.to + 1e-6; a += spec.angleStep) {
    const P2 d = dirOfDeg(a);
    int total = 0, bad = 0;
    double sum = 0;
    for (double t = spec.r0; t < spec.r1 - 1e-9; t += spec.radialStep) {
      const double v = R.at(spec.anchor + d * t);
      total += 1;
      if (v < 0) {
        bad += 1;
      } else {
        sum += v;
      }
    }
    out.angles.push_back(a);
    out.scores.push_back(total > 0 && bad < spec.invalidMax * total ? sum / total : kNaN);
    out.invalid.push_back(bad);
  }
  // includeInvalid：无效方向按得分 −1 一起排（同分保持角度顺序）
  const auto key = [&](std::size_t i) {
    return std::isfinite(out.scores[i]) ? out.scores[i] : -1.0;
  };
  std::vector<std::size_t> order;
  for (std::size_t i = 0; i < out.scores.size(); ++i) {
    if (spec.includeInvalid || std::isfinite(out.scores[i]))
      order.push_back(i);
  }
  std::stable_sort(order.begin(), order.end(),
                   [&](std::size_t x, std::size_t y) { return key(x) > key(y); });
  for (std::size_t i : order) {
    if (static_cast<int>(out.peaks.size()) >= spec.peakCount)
      break;
    const bool far = std::all_of(out.peaks.begin(), out.peaks.end(), [&](const FanPeak& p) {
      return angleDiffDeg(p.angle, out.angles[i]) > spec.peakSeparation;
    });
    if (far)
      out.peaks.push_back(FanPeak{i, out.angles[i], out.scores[i]});
  }
  return out;
}

std::vector<EdgeHit> findPartEdges(const Field& gray, P2 c, P2 dir, double start, double length,
                                   const EdgeScanSpec& spec, bool firstOnly, int* samples) {
  std::vector<EdgeHit> hits;
  const int want = static_cast<int>(std::floor(length)) + 1;
  std::vector<double> v;
  v.reserve(static_cast<std::size_t>(want));
  for (int k = 0; k < want; ++k) {
    const double x = gray.at(c + dir * (start + k));
    if (x < 0)
      break; // 出了图：截断，不把图外当成暗（否则图像边界本身就成了「零件边」）
    v.push_back(spec.darkToBright ? 255.0 - x : x);
  }
  if (samples)
    *samples = static_cast<int>(v.size());
  const int far = std::max(1, static_cast<int>(std::lround(spec.farRun)));
  const int n = static_cast<int>(v.size());
  if (n < far + 10)
    return hits;
  const std::vector<double> p = smooth1d(v);
  auto descent = [&](int k) {
    return 0.5 * (p[static_cast<std::size_t>(k - 1)] - p[static_cast<std::size_t>(k + 1)]);
  };
  for (int k = 5; k + 2 + far <= n && k + 1 < n; ++k) {
    const double dk = descent(k);
    if (dk <= spec.descentRatio * spec.contrastMin)
      continue;
    // 跳变里下降最快的那一点：往前、往后都不比它陡
    if (descent(k - 1) > dk)
      continue;
    if (k + 2 < n && descent(k + 1) >= dk)
      continue;
    double nearMean = 0;
    for (int j = k - 5; j <= k - 2; ++j)
      nearMean += p[static_cast<std::size_t>(j)];
    nearMean /= 4.0;
    double farMean = 0, farMax = -std::numeric_limits<double>::infinity();
    for (int j = k + 2; j < k + 2 + far; ++j) {
      farMean += p[static_cast<std::size_t>(j)];
      farMax = std::max(farMax, p[static_cast<std::size_t>(j)]);
    }
    farMean /= far;
    if (nearMean - farMean < spec.contrastMin)
      continue;
    if (nearMean - farMax < spec.farDropRatio * spec.contrastMin)
      continue; // 远侧没持续暗下去：划痕、反光条
    const double a = descent(k - 1);
    const double b = k + 2 < n ? descent(k + 1) : dk;
    const double den = a - 2.0 * dk + b;
    const double delta = den < -1e-12 ? std::clamp(0.5 * (a - b) / den, -0.5, 0.5) : 0.0;
    hits.push_back(EdgeHit{start + k + delta, nearMean - farMean, nearMean - farMax, dk});
    if (firstOnly)
      break;
  }
  return hits;
}

} // namespace lyflow::std_image::line_scan
