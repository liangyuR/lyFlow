#include <cmath>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <pcl/common/point_tests.h>
#include <pcl/io/pcd_io.h>

#include "gap_ml/SensorXzProfile.hpp"
#include "gap_ops.h"

namespace lyflow::packs::gap {

GapCloud toPcl(const lyflow::PointCloud& cloud) {
  GapCloud out;
  const std::size_t n = cloud.pointCount();
  out.points.reserve(n);
  const bool hasRgb = cloud.hasRgb();
  for (std::size_t i = 0; i < n; ++i) {
    GapPoint p;
    p.x = cloud.xyz[i * 3];
    p.y = cloud.xyz[i * 3 + 1];
    p.z = cloud.xyz[i * 3 + 2];
    p.r = hasRgb ? cloud.rgb[i * 3] : 0;
    p.g = hasRgb ? cloud.rgb[i * 3 + 1] : 0;
    p.b = hasRgb ? cloud.rgb[i * 3 + 2] : 0;
    out.points.push_back(p);
  }
  out.width = static_cast<std::uint32_t>(out.points.size());
  out.height = 1;
  out.is_dense = false;
  return out;
}

lyflow::PointCloud fromPcl(const GapCloud& cloud) {
  lyflow::PointCloud out;
  out.xyz.reserve(cloud.size() * 3);
  out.rgb.reserve(cloud.size() * 3);
  for (const auto& p : cloud) {
    out.push(p.x, p.y, p.z);
    out.rgb.push_back(p.r);
    out.rgb.push_back(p.g);
    out.rgb.push_back(p.b);
  }
  return out;
}

bool loadPcd(const std::string& path, GapCloud* out, std::string* message) {
  try {
    if (pcl::io::loadPCDFile(path, *out) < 0) {
      *message = "读不了 " + path;
      return false;
    }
  } catch (const std::exception& e) {
    *message = std::string("读 ") + path + " 时出错: " + e.what();
    return false;
  }
  return true;
}

void removeNonFinite(GapCloud* cloud) {
  GapCloud finite;
  finite.header = cloud->header;
  finite.sensor_origin_ = cloud->sensor_origin_;
  finite.sensor_orientation_ = cloud->sensor_orientation_;
  finite.points.reserve(cloud->size());
  for (const auto& point : *cloud) {
    if (pcl::isFinite(point)) finite.points.push_back(point);
  }
  finite.width = static_cast<std::uint32_t>(finite.points.size());
  finite.height = 1;
  finite.is_dense = true;
  *cloud = std::move(finite);
}

Eigen::Matrix2f toRoiMatrix(const lyflow::Box2D& box) {
  Eigen::Matrix2f roi;
  roi << box.min[0], box.max[0], box.min[1], box.max[1];
  return roi;
}

lyflow::Box2D boxFromMm(double xMin, double yMin, double xMax, double yMax) {
  lyflow::Box2D box;
  box.min[0] = mmToMRoi(xMin);
  box.min[1] = mmToMRoi(yMin);
  box.max[0] = mmToMRoi(xMax);
  box.max[1] = mmToMRoi(yMax);
  return box;
}

std::vector<double> transformToJson(const Eigen::Matrix3f& m) {
  std::vector<double> out(9);
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) out[static_cast<std::size_t>(r * 3 + c)] = m(r, c);
  }
  return out;
}

Eigen::Matrix3f transformFromJson(const nlohmann::json& j) {
  Eigen::Matrix3f m = Eigen::Matrix3f::Identity();
  if (!j.is_array() || j.size() != 9) return m;
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      m(r, c) = j[static_cast<std::size_t>(r * 3 + c)].get<float>();
    }
  }
  return m;
}

std::string fileStamp(const std::filesystem::path& p) {
  if (p.empty()) return {};
  std::error_code ec;
  const auto size = std::filesystem::file_size(p, ec);
  const auto time = std::filesystem::last_write_time(p, ec);
  if (ec) return {};
  return std::to_string(size) + ":" + std::to_string(time.time_since_epoch().count());
}

const ::gap::ml::OnnxRoiPredictor* predictorFor(const std::filesystem::path& model,
                                                std::string* message) {
  // 长驻缓存（H2）：10 MB 的模型每次运行重载不可接受，live preview 会连着触发。
  // 键带 mtime，换了模型文件自然换实例；旧实例留着不删，进程里最多几个。
  static std::mutex mu;
  static std::unordered_map<std::string, std::shared_ptr<::gap::ml::OnnxRoiPredictor>> cache;
  const std::string key = model.string() + "|" + fileStamp(model);
  std::lock_guard<std::mutex> lock(mu);
  auto it = cache.find(key);
  if (it != cache.end()) return it->second.get();
  try {
    auto made = std::make_shared<::gap::ml::OnnxRoiPredictor>(model);
    return cache.emplace(key, std::move(made)).first->second.get();
  } catch (const std::exception& e) {
    *message = std::string("模型加载失败: ") + e.what();
    return nullptr;
  }
}

bool profileRowOf(const lyflow::PointCloud& cloud, ::gap::ml::ProfileRow* out) {
  if (cloud.pointCount() != ::gap::ml::kProfileSlots) return false;
  *out = ::gap::ml::profileRowsFromSensorXzCloud(toPcl(cloud));
  return true;
}

void setMeasurement(Outputs& outputs, const char* port, double valueMm, bool ok,
                    const std::string& message) {
  lyflow::Measurement m;
  m.value = valueMm;
  m.ok = ok && std::isfinite(valueMm);
  m.unit = "mm";
  m.message = message;
  outputs.set(port, Data::measurement(std::move(m)));
}

}  // namespace lyflow::packs::gap
