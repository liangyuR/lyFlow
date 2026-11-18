#include "lyflow_cv/caliper_sampling.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace lyflow::std_image::line_scan {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

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

/// 三点抛物线的顶点相对中点的偏移（−0.5 … 0.5）。不是尖峰（开口朝上或平）时返回 0。
double parabolaPeak(double a, double b, double c) {
  const double den = a - 2.0 * b + c;
  if (!(den < 0.0))
    return 0.0;
  return std::clamp(0.5 * (a - c) / den, -0.5, 0.5);
}

std::vector<double> sampleCaliperStrip(const cv::Mat& gray, P2 center, P2 normal, double half,
                                       double width) {
  if (gray.type() != CV_8UC1 || gray.cols < 2 || gray.rows < 2 || !finite(center) ||
      !finite(normal))
    return {};
  const int m = static_cast<int>(std::round(4.0 * half)) + 1;
  const P2 across = perp(normal);
  const int w = std::max(1, static_cast<int>(std::round(width)));
  std::vector<double> raw(m);
  for (int i = 0; i < m; ++i) {
    double acc = 0;
    for (int k = 0; k < w; ++k) {
      const double u = k - (w - 1) / 2.0;
      const P2 p = center + normal * (-half + 0.5 * i) + across * u;
      const double v = sample(gray, p.x, p.y);
      if (!std::isfinite(v))
        return {};
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
  return v;
}

} // namespace lyflow::std_image::line_scan
