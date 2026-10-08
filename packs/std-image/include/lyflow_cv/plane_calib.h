#pragma once
// 工作平面标定 image.PlaneCalib（glue-plan §2.1 / F3）：图像像素 → 工作平面 mm 的 3×3 单应。
//
//   Record { type: "image.PlaneCalib", data: { H: [9 个数，行主序], unit: "mm" } }
//   [X, Y, W]ᵀ = H · [x, y, 1]ᵀ，工作平面上的点是 (X / W, Y / W)
//
// 量测积木（packs/glue 的 bead_width / edge_distance）只「用」它：不接出 px，接了宽度、距离、长度都按
// 映射后两点的距离算。产出它的算子（已知 mm/px、标定板）在 G3。头文件自带实现，不依赖 OpenCV。
#include <array>
#include <cmath>
#include <limits>
#include <string>

#include <nlohmann/json.hpp>

namespace lyflow::std_image {

inline constexpr const char* kPlaneCalibType = "image.PlaneCalib";

struct PlaneCalib {
  std::array<double, 9> H{1, 0, 0, 0, 1, 0, 0, 0, 1};
  std::string unit = "mm";

  /// 像素 (x, y) → 工作平面 (X, Y)。齐次分量接近 0（点在地平线上）或结果非有限时返回 false。
  bool map(double x, double y, double* X, double* Y) const {
    const double w = H[6] * x + H[7] * y + H[8];
    if (!std::isfinite(w) || std::fabs(w) < 1e-12) return false;
    *X = (H[0] * x + H[1] * y + H[2]) / w;
    *Y = (H[3] * x + H[4] * y + H[5]) / w;
    return std::isfinite(*X) && std::isfinite(*Y);
  }

  /// 两个像素点映射到工作平面之后的距离；映射不了返回 NaN。
  double distance(double x0, double y0, double x1, double y1) const {
    double X0 = 0, Y0 = 0, X1 = 0, Y1 = 0;
    if (!map(x0, y0, &X0, &Y0) || !map(x1, y1, &X1, &Y1)) {
      return std::numeric_limits<double>::quiet_NaN();
    }
    return std::hypot(X1 - X0, Y1 - Y0);
  }
};

/// data 是 Record 的 data。H 要恰好 9 个有限数、行列式不为 0；unit 缺省 mm，给了就要是非空字符串。
/// 不合格时返回 false，why 里是给人看的原因。
inline bool parsePlaneCalib(const nlohmann::json& data, PlaneCalib* out, std::string* why) {
  auto fail = [&](const std::string& m) {
    if (why) *why = m;
    return false;
  };
  if (!data.is_object()) return fail("image.PlaneCalib 的 data 不是对象");
  const auto h = data.find("H");
  if (h == data.end() || !h->is_array() || h->size() != 9) return fail("H 要是 9 个数（行主序的 3×3）");
  PlaneCalib c;
  for (std::size_t i = 0; i < 9; ++i) {
    const auto& v = (*h)[i];
    if (!v.is_number() || !std::isfinite(v.get<double>())) return fail("H 里有不是有限数的元素");
    c.H[i] = v.get<double>();
  }
  const auto& m = c.H;
  const double det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
                     m[2] * (m[3] * m[7] - m[4] * m[6]);
  if (!std::isfinite(det) || std::fabs(det) < 1e-18) return fail("H 是奇异的（行列式为 0）");
  if (const auto u = data.find("unit"); u != data.end()) {
    if (!u->is_string() || u->get<std::string>().empty()) return fail("unit 要是非空字符串");
    c.unit = u->get<std::string>();
  }
  *out = c;
  return true;
}

inline nlohmann::json planeCalibJson(const PlaneCalib& c) {
  return nlohmann::json{{"H", c.H}, {"unit", c.unit}};
}

}  // namespace lyflow::std_image
