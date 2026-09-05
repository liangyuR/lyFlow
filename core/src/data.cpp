#include "lyflow/data.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

namespace lyflow {
namespace {

std::atomic<std::uint64_t>& cloudIdCounter() {
  static std::atomic<std::uint64_t> counter{1};
  return counter;
}

}  // namespace

void Bounds::extend(float x, float y, float z) {
  if (!valid) {
    min[0] = max[0] = x;
    min[1] = max[1] = y;
    min[2] = max[2] = z;
    valid = true;
    return;
  }
  min[0] = std::min(min[0], x);
  min[1] = std::min(min[1], y);
  min[2] = std::min(min[2], z);
  max[0] = std::max(max[0], x);
  max[1] = std::max(max[1], y);
  max[2] = std::max(max[2], z);
}

PointCloud::PointCloud() : id(cloudIdCounter().fetch_add(1, std::memory_order_relaxed)) {}

void PointCloud::reserve(std::size_t n) {
  xyz.reserve(n * 3);
  if (!intensity.empty()) intensity.reserve(n);
  if (!normals.empty()) normals.reserve(n * 3);
  if (!rgb.empty()) rgb.reserve(n * 3);
}

Bounds PointCloud::bounds() const {
  Bounds b;
  const std::size_t n = pointCount();
  for (std::size_t i = 0; i < n; ++i) {
    const float x = xyz[i * 3], y = xyz[i * 3 + 1], z = xyz[i * 3 + 2];
    // 跳过非有限点：keepOrganized 的滤波器会把删掉的点填成 NaN，
    // 让它们参与 min/max 的话包围盒会整个变成 NaN，3D 视图直接黑屏。
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) continue;
    b.extend(x, y, z);
  }
  return b;
}

bool PointCloud::channelsConsistent() const {
  const std::size_t n = pointCount();
  if (xyz.size() != n * 3) return false;
  if (!intensity.empty() && intensity.size() != n) return false;
  if (!normals.empty() && normals.size() != n * 3) return false;
  if (!rgb.empty() && rgb.size() != n * 3) return false;
  return true;
}

PointCloud PointCloud::select(const std::vector<std::int32_t>& keep) const {
  PointCloud out;  // 新 id：这是一片新的点云，旧的 Indices 不该还能指向它
  const std::int32_t n = static_cast<std::int32_t>(pointCount());
  out.xyz.reserve(keep.size() * 3);
  if (hasIntensity()) out.intensity.reserve(keep.size());
  if (hasNormals()) out.normals.reserve(keep.size() * 3);
  if (hasRgb()) out.rgb.reserve(keep.size() * 3);

  for (std::int32_t idx : keep) {
    if (idx < 0 || idx >= n) continue;  // 越界的下标静默跳过；对账在消费方做
    const std::size_t i = static_cast<std::size_t>(idx);
    out.xyz.push_back(xyz[i * 3]);
    out.xyz.push_back(xyz[i * 3 + 1]);
    out.xyz.push_back(xyz[i * 3 + 2]);
    if (hasIntensity()) out.intensity.push_back(intensity[i]);
    if (hasNormals()) {
      out.normals.push_back(normals[i * 3]);
      out.normals.push_back(normals[i * 3 + 1]);
      out.normals.push_back(normals[i * 3 + 2]);
    }
    if (hasRgb()) {
      out.rgb.push_back(rgb[i * 3]);
      out.rgb.push_back(rgb[i * 3 + 1]);
      out.rgb.push_back(rgb[i * 3 + 2]);
    }
  }
  return out;
}

PointCloud PointCloud::selectInverse(const std::vector<std::int32_t>& drop) const {
  const std::size_t n = pointCount();
  std::vector<bool> dropped(n, false);
  for (std::int32_t idx : drop) {
    if (idx >= 0 && static_cast<std::size_t>(idx) < n) dropped[static_cast<std::size_t>(idx)] = true;
  }
  std::vector<std::int32_t> keep;
  keep.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    if (!dropped[i]) keep.push_back(static_cast<std::int32_t>(i));
  }
  return select(keep);
}

Data Data::cloud(std::shared_ptr<const PointCloud> c) {
  Data d;
  d.kind_ = Kind::PointCloud;
  d.cloud_ = std::move(c);
  return d;
}

Data Data::indices(std::shared_ptr<const Indices> i) {
  Data d;
  d.kind_ = Kind::Indices;
  d.indices_ = std::move(i);
  return d;
}

Data Data::transform(std::shared_ptr<const Transform> t) {
  Data d;
  d.kind_ = Kind::Transform;
  d.transform_ = std::move(t);
  return d;
}

Data Data::plane(std::shared_ptr<const Plane> p) {
  Data d;
  d.kind_ = Kind::Plane;
  d.plane_ = std::move(p);
  return d;
}

const PointCloud* Data::asCloud() const { return kind_ == Kind::PointCloud ? cloud_.get() : nullptr; }
const Indices* Data::asIndices() const { return kind_ == Kind::Indices ? indices_.get() : nullptr; }
const Transform* Data::asTransform() const {
  return kind_ == Kind::Transform ? transform_.get() : nullptr;
}
const Plane* Data::asPlane() const { return kind_ == Kind::Plane ? plane_.get() : nullptr; }

const char* Data::typeName() const { return typeNameFromKind(kind_); }

std::size_t Data::byteSize() const {
  switch (kind_) {
    case Kind::None:
      return 0;
    case Kind::PointCloud: {
      if (!cloud_) return 0;
      return cloud_->xyz.size() * sizeof(float) + cloud_->intensity.size() * sizeof(float) +
             cloud_->normals.size() * sizeof(float) + cloud_->rgb.size();
    }
    case Kind::Indices:
      return indices_ ? indices_->values.size() * sizeof(std::int32_t) : 0;
    case Kind::Transform:
      return sizeof(Transform);
    case Kind::Plane:
      return sizeof(Plane);
  }
  return 0;
}

std::size_t Data::elementCount() const {
  switch (kind_) {
    case Kind::None:
      return 0;
    case Kind::PointCloud:
      return cloud_ ? cloud_->pointCount() : 0;
    case Kind::Indices:
      return indices_ ? indices_->values.size() : 0;
    case Kind::Transform:
    case Kind::Plane:
      return 1;
  }
  return 0;
}

Data::Kind kindFromTypeName(const std::string& typeName) {
  if (typeName == "PointCloud") return Data::Kind::PointCloud;
  if (typeName == "Indices") return Data::Kind::Indices;
  if (typeName == "Transform") return Data::Kind::Transform;
  if (typeName == "Plane") return Data::Kind::Plane;
  return Data::Kind::None;  // 含 "Any"：不约束具体载荷
}

const char* typeNameFromKind(Data::Kind kind) {
  switch (kind) {
    case Data::Kind::None:       return "None";
    case Data::Kind::PointCloud: return "PointCloud";
    case Data::Kind::Indices:    return "Indices";
    case Data::Kind::Transform:  return "Transform";
    case Data::Kind::Plane:      return "Plane";
  }
  return "None";
}

}  // namespace lyflow
