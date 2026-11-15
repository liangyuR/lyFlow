#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "lyflow/value.h"

namespace lyflow::calibration {

inline constexpr const char* kCameraType = "vision.CameraCalib.v1";
inline constexpr const char* kPlaneType = "image.PlaneCalib";

struct Camera {
  int width = 0, height = 0;
  std::array<double, 9> matrix{};
  std::vector<double> distortion;
  std::string frame;
};

inline bool parseCamera(const Record& record, Camera& out, std::string& why) {
  const auto& d = record.data;
  if (record.type != kCameraType || !d.is_object() || !values::boundedJson(d, why)) {
    why = "需要有界 vision.CameraCalib.v1 标定记录";
    return false;
  }
  if (d.size() != 5 || !d.contains("imageSize") || !d.contains("cameraMatrix") ||
      !d.contains("distortion") || !d.contains("frame") || !d.contains("unit") ||
      d["unit"] != "m" || !d["frame"].is_string() || d["frame"].get<std::string>().empty() ||
      d["frame"].get_ref<const std::string&>().size() > 256 || !d["imageSize"].is_array() ||
      d["imageSize"].size() != 2 || !d["cameraMatrix"].is_array() ||
      d["cameraMatrix"].size() != 9 || !d["distortion"].is_array()) {
    why = "相机标定字段、米制单位或参考帧不合法";
    return false;
  }
  Camera camera;
  for (int i = 0; i < 2; ++i) {
    if (!d["imageSize"][i].is_number_integer()) {
      why = "imageSize 必须是整数宽高";
      return false;
    }
    const auto n = d["imageSize"][i].get<std::int64_t>();
    if (n <= 0 || n > 65536) {
      why = "相机标定尺寸超出范围";
      return false;
    }
    (i == 0 ? camera.width : camera.height) = static_cast<int>(n);
  }
  if (static_cast<std::uint64_t>(camera.width) * camera.height > 67108864) {
    why = "相机标定超过 64 Mi 像素";
    return false;
  }
  for (std::size_t i = 0; i < 9; ++i) {
    if (!values::safeNumber(d["cameraMatrix"][i])) {
      why = "相机矩阵必须为有限数";
      return false;
    }
    camera.matrix[i] = d["cameraMatrix"][i].get<double>();
  }
  const auto& k = camera.matrix;
  if (!(k[0] > 0 && k[4] > 0) || k[1] != 0 || k[3] != 0 || k[6] != 0 || k[7] != 0 || k[8] != 1) {
    why = "仅支持 fx/fy 为正、零 skew、末行为 [0,0,1] 的针孔相机矩阵";
    return false;
  }
  const auto n = d["distortion"].size();
  if (n != 0 && n != 4 && n != 5 && n != 8 && n != 12 && n != 14) {
    why = "畸变系数数量必须为 0/4/5/8/12/14";
    return false;
  }
  for (const auto& v : d["distortion"]) {
    if (!values::safeNumber(v)) {
      why = "畸变系数必须有限";
      return false;
    }
    camera.distortion.push_back(v.get<double>());
  }
  camera.frame = d["frame"].get<std::string>();
  out = std::move(camera);
  return true;
}

struct Plane {
  std::array<double, 9> H{};
  std::string unit;
  double metersPerUnit = 1;
  std::string imageFrame;
  std::string planeFrame;

  bool map(double x, double y, double& X, double& Y) const {
    const double w = H[6] * x + H[7] * y + H[8];
    if (w == 0 || !std::isfinite(w))
      return false;
    X = (H[0] * x + H[1] * y + H[2]) / w;
    Y = (H[3] * x + H[4] * y + H[5]) / w;
    return std::isfinite(X) && std::isfinite(Y);
  }
};

inline bool parsePlane(const Record& record, Plane& out, std::string& why) {
  const auto& d = record.data;
  if (record.type != kPlaneType || !d.is_object() || !values::boundedJson(d, why) ||
      !d.contains("H") || !d["H"].is_array() || d["H"].size() != 9 || !d.contains("unit") ||
      (d["unit"] != "m" && d["unit"] != "mm")) {
    why = "需要 image.PlaneCalib，H 为九个有限数且 unit 为 m/mm";
    return false;
  }
  if (d.contains("schemaVersion")) {
    if (!d["schemaVersion"].is_number_integer() || d["schemaVersion"] != 1 ||
        !d.contains("imageFrame") || !d["imageFrame"].is_string() || !d.contains("planeFrame") ||
        !d["planeFrame"].is_string() || d["imageFrame"].get_ref<const std::string&>().empty() ||
        d["planeFrame"].get_ref<const std::string&>().empty() ||
        d["imageFrame"].get_ref<const std::string&>().size() > 1024 ||
        d["planeFrame"].get_ref<const std::string&>().size() > 1024) {
      why = "版本化PlaneCalib需要schemaVersion=1及明确的imageFrame/planeFrame";
      return false;
    }
  }
  Plane plane;
  double scale = 0;
  for (std::size_t i = 0; i < 9; ++i) {
    if (!values::safeNumber(d["H"][i])) {
      why = "H 包含非法数值";
      return false;
    }
    plane.H[i] = d["H"][i].get<double>();
    scale = std::max(scale, std::abs(plane.H[i]));
  }
  if (scale == 0) {
    why = "H 不能为零矩阵";
    return false;
  }
  for (double& v : plane.H)
    v /= scale;
  const auto& h = plane.H;
  const double det = h[0] * (h[4] * h[8] - h[5] * h[7]) - h[1] * (h[3] * h[8] - h[5] * h[6]) +
                     h[2] * (h[3] * h[7] - h[4] * h[6]);
  if (det == 0 || !std::isfinite(det)) {
    why = "H 必须可逆";
    return false;
  }
  plane.unit = d["unit"].get<std::string>();
  plane.metersPerUnit = plane.unit == "mm" ? 0.001 : 1;
  if (d.contains("schemaVersion")) {
    plane.imageFrame = d["imageFrame"].get<std::string>();
    plane.planeFrame = d["planeFrame"].get<std::string>();
  }
  out = std::move(plane);
  why.clear();
  return true;
}

inline bool upgradeLegacyPlane(const Record& input, const std::string& missingUnit,
                               const std::string& imageFrame, const std::string& planeFrame,
                               Record& out, std::string& why) {
  if (input.type != kPlaneType || !input.data.is_object()) {
    why = "只能显式升级image.PlaneCalib记录";
    return false;
  }
  if (input.data.contains("schemaVersion")) {
    Plane parsed;
    if (!parsePlane(input, parsed, why))
      return false;
    if (parsed.imageFrame != imageFrame || parsed.planeFrame != planeFrame) {
      why = "已有标定帧与声明不一致，不能用升级操作重新标记坐标系";
      return false;
    }
    out = input;
    return true;
  }
  Record candidate = input;
  if (!candidate.data.contains("unit")) {
    if (missingUnit != "m" && missingUnit != "mm") {
      why = "旧标定缺少单位，必须显式选择m或mm，不能猜测";
      return false;
    }
    candidate.data["unit"] = missingUnit;
  }
  candidate.data["schemaVersion"] = 1;
  candidate.data["imageFrame"] = imageFrame;
  candidate.data["planeFrame"] = planeFrame;
  Plane parsed;
  if (!parsePlane(candidate, parsed, why))
    return false;
  candidate.data["H"] = parsed.H;
  out = std::move(candidate);
  return true;
}

} // namespace lyflow::calibration
