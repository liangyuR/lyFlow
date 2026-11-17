#include <cmath>

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

Eigen::Matrix2f toRoiMatrix(const lyflow::Box2D& box) {
  Eigen::Matrix2f roi;
  roi << box.min[0], box.max[0], box.min[1], box.max[1];
  return roi;
}

std::vector<double> transformToJson(const Eigen::Matrix3f& m) {
  std::vector<double> out(9);
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c)
      out[static_cast<std::size_t>(r * 3 + c)] = m(r, c);
  }
  return out;
}

std::string fileStamp(const std::filesystem::path& p) {
  if (p.empty())
    return {};
  std::error_code ec;
  const auto size = std::filesystem::file_size(p, ec);
  const auto time = std::filesystem::last_write_time(p, ec);
  if (ec)
    return {};
  return std::to_string(size) + ":" + std::to_string(time.time_since_epoch().count());
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

} // namespace lyflow::packs::gap
