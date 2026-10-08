#pragma once
// 涂胶算法的几何小工具与「图像坐标里的一张 float 场」（响应图、平滑灰度）。
// 坐标一律是图像像素：x 向右、y 向下、像素中心在整数坐标（glue-plan §2.1）。
#include <cmath>

#include <opencv2/core.hpp>

namespace lyflow::packs::glue {

using P2 = cv::Point2d;

inline double dot(P2 a, P2 b) { return a.x * b.x + a.y * b.y; }
inline double length(P2 a) { return std::hypot(a.x, a.y); }
inline P2 unit(P2 a) {
  const double l = length(a);
  return l > 1e-12 ? P2(a.x / l, a.y / l) : P2(1.0, 0.0);
}
/// 法向 n = (−t.y, t.x)：图像 y 向下，它指向行进方向的右侧（glue-plan §2.1）。
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
/// （外扩出来算卷积、形态学用的那一圈不算）。
class Field {
 public:
  Field() = default;
  Field(cv::Mat data, cv::Point origin, cv::Rect valid)
      : data_(std::move(data)), origin_(origin), valid_(valid) {}

  bool empty() const { return data_.empty() || valid_.empty(); }
  const cv::Rect& valid() const { return valid_; }

  /// 双线性采样。四个邻点有一个不在可信区里就返回 kInvalid。
  double at(double x, double y) const {
    if (empty() || !std::isfinite(x) || !std::isfinite(y)) return kInvalid;
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
    return top + ay * (bottom - top);
  }
  double at(P2 p) const { return at(p.x, p.y); }

 private:
  cv::Mat data_;
  cv::Point origin_;
  cv::Rect valid_;
};

}  // namespace lyflow::packs::glue
