#include "lyflow_pcl/adapter.h"

#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>

namespace lyflow::ops::adapter {
namespace {

/// 一个 PCD 字段占几个字节。
std::uint32_t fieldWidth(std::uint8_t datatype) {
  switch (datatype) {
    case pcl::PCLPointField::INT8:
    case pcl::PCLPointField::UINT8:   return 1;
    case pcl::PCLPointField::INT16:
    case pcl::PCLPointField::UINT16:  return 2;
    case pcl::PCLPointField::INT32:
    case pcl::PCLPointField::UINT32:
    case pcl::PCLPointField::FLOAT32: return 4;
    case pcl::PCLPointField::FLOAT64: return 8;
    default: return 0;
  }
}

/// 读一个字段。PCD 文件里同一个语义字段可能是 float32 也可能是 float64
/// （不同厂商的导出工具各有各的习惯），所以按 datatype 分派而不是直接 memcpy。
struct FieldReader {
  bool present = false;
  std::uint32_t offset = 0;
  std::uint32_t width = 0;
  std::uint8_t datatype = 0;

  double read(const std::uint8_t* p) const {
    const std::uint8_t* q = p + offset;
    switch (datatype) {
      case pcl::PCLPointField::INT8:   { std::int8_t v;   std::memcpy(&v, q, 1); return v; }
      case pcl::PCLPointField::UINT8:  { std::uint8_t v;  std::memcpy(&v, q, 1); return v; }
      case pcl::PCLPointField::INT16:  { std::int16_t v;  std::memcpy(&v, q, 2); return v; }
      case pcl::PCLPointField::UINT16: { std::uint16_t v; std::memcpy(&v, q, 2); return v; }
      case pcl::PCLPointField::INT32:  { std::int32_t v;  std::memcpy(&v, q, 4); return v; }
      case pcl::PCLPointField::UINT32: { std::uint32_t v; std::memcpy(&v, q, 4); return v; }
      case pcl::PCLPointField::FLOAT32:{ float v;         std::memcpy(&v, q, 4); return v; }
      case pcl::PCLPointField::FLOAT64:{ double v;        std::memcpy(&v, q, 8); return v; }
      default: return 0.0;
    }
  }

  /// rgb 在 PCD 里可能声明成 float32 也可能是 uint32，两种都要认。
  /// 按字段声明的宽度读 —— 无条件读 4 字节会在 SIZE 为 1 的头上越过 data 末尾。
  std::uint32_t readPacked(const std::uint8_t* p) const {
    std::uint32_t v = 0;
    std::memcpy(&v, p + offset, width < 4 ? width : 4);
    return v;
  }
};

/// 按名字找字段，**并且确认它真的落在 point_step 之内**。
/// 越界的字段当作不存在 —— 与其相信一个自相矛盾的头，不如少读一个通道。
FieldReader findField(const pcl::PCLPointCloud2& blob, const char* name) {
  FieldReader r;
  for (const auto& f : blob.fields) {
    if (f.name != name) continue;
    const std::uint32_t w = fieldWidth(f.datatype);
    if (w == 0) return r;
    // count > 1 的字段（比如打包成数组的描述子）这里只读第一个分量，
    // 但仍然要按整段长度算边界。
    const std::uint64_t end =
        static_cast<std::uint64_t>(f.offset) + static_cast<std::uint64_t>(w) *
                                                   (f.count == 0 ? 1u : f.count);
    if (end > blob.point_step) return r;
    r.present = true;
    r.offset = f.offset;
    r.width = w;
    r.datatype = f.datatype;
    return r;
  }
  return r;
}

void addField(pcl::PCLPointCloud2& blob, const char* name, std::uint32_t offset,
              std::uint8_t datatype) {
  pcl::PCLPointField f;
  f.name = name;
  f.offset = offset;
  f.datatype = datatype;
  f.count = 1;
  blob.fields.push_back(f);
}

}  // namespace

pcl::PointCloud<pcl::PointXYZ>::Ptr toPcl(const PointCloud& cloud) {
  auto out = pcl::make_shared<pcl::PointCloud<pcl::PointXYZ>>();
  const std::size_t n = cloud.pointCount();
  out->resize(n);
  out->width = static_cast<std::uint32_t>(n);
  out->height = 1;
  out->is_dense = true;
  for (std::size_t i = 0; i < n; ++i) {
    (*out)[i].x = cloud.xyz[i * 3];
    (*out)[i].y = cloud.xyz[i * 3 + 1];
    (*out)[i].z = cloud.xyz[i * 3 + 2];
    if (!std::isfinite((*out)[i].x) || !std::isfinite((*out)[i].y) || !std::isfinite((*out)[i].z)) {
      out->is_dense = false;
    }
  }
  return out;
}

std::vector<std::int32_t> fromPclIndices(const pcl::PointIndices& indices) {
  return fromPclIndices(indices.indices);
}

std::vector<std::int32_t> fromPclIndices(const pcl::Indices& indices) {
  std::vector<std::int32_t> out;
  out.reserve(indices.size());
  for (const auto i : indices) out.push_back(static_cast<std::int32_t>(i));
  return out;
}

PointCloud fromPcl(const pcl::PointCloud<pcl::PointXYZ>& cloud) {
  PointCloud out;
  out.xyz.reserve(cloud.size() * 3);
  for (const auto& p : cloud) {
    out.xyz.push_back(p.x);
    out.xyz.push_back(p.y);
    out.xyz.push_back(p.z);
  }
  return out;
}

PointCloud fromBlob(const pcl::PCLPointCloud2& blob) {
  PointCloud out;
  const FieldReader fx = findField(blob, "x");
  const FieldReader fy = findField(blob, "y");
  const FieldReader fz = findField(blob, "z");
  if (!fx.present || !fy.present || !fz.present) return out;

  const FieldReader fi = findField(blob, "intensity");
  const FieldReader fnx = findField(blob, "normal_x");
  const FieldReader fny = findField(blob, "normal_y");
  const FieldReader fnz = findField(blob, "normal_z");
  FieldReader frgb = findField(blob, "rgb");
  if (!frgb.present) frgb = findField(blob, "rgba");

  const bool hasNormals = fnx.present && fny.present && fnz.present;
  const std::size_t n = static_cast<std::size_t>(blob.width) * blob.height;
  const std::size_t step = blob.point_step;
  if (step == 0 || blob.data.size() < n * step) return out;

  out.xyz.reserve(n * 3);
  if (fi.present) out.intensity.reserve(n);
  if (hasNormals) out.normals.reserve(n * 3);
  if (frgb.present) out.rgb.reserve(n * 3);

  for (std::size_t i = 0; i < n; ++i) {
    const std::uint8_t* p = blob.data.data() + i * step;
    out.push(static_cast<float>(fx.read(p)), static_cast<float>(fy.read(p)),
             static_cast<float>(fz.read(p)));
    if (fi.present) out.intensity.push_back(static_cast<float>(fi.read(p)));
    if (hasNormals) {
      out.normals.push_back(static_cast<float>(fnx.read(p)));
      out.normals.push_back(static_cast<float>(fny.read(p)));
      out.normals.push_back(static_cast<float>(fnz.read(p)));
    }
    if (frgb.present) {
      const std::uint32_t packed = frgb.readPacked(p);
      out.rgb.push_back(static_cast<std::uint8_t>((packed >> 16) & 0xFF));
      out.rgb.push_back(static_cast<std::uint8_t>((packed >> 8) & 0xFF));
      out.rgb.push_back(static_cast<std::uint8_t>(packed & 0xFF));
    }
  }
  return out;
}

void toBlob(const PointCloud& cloud, pcl::PCLPointCloud2& blob) {
  const std::size_t n = cloud.pointCount();
  blob.fields.clear();
  std::uint32_t offset = 0;
  addField(blob, "x", offset, pcl::PCLPointField::FLOAT32); offset += 4;
  addField(blob, "y", offset, pcl::PCLPointField::FLOAT32); offset += 4;
  addField(blob, "z", offset, pcl::PCLPointField::FLOAT32); offset += 4;
  if (cloud.hasIntensity()) {
    addField(blob, "intensity", offset, pcl::PCLPointField::FLOAT32);
    offset += 4;
  }
  if (cloud.hasNormals()) {
    addField(blob, "normal_x", offset, pcl::PCLPointField::FLOAT32); offset += 4;
    addField(blob, "normal_y", offset, pcl::PCLPointField::FLOAT32); offset += 4;
    addField(blob, "normal_z", offset, pcl::PCLPointField::FLOAT32); offset += 4;
  }
  if (cloud.hasRgb()) {
    // 写成 float32 而不是 uint32：PCL 自己的点类型就是这么定义的，
    // 换成 uint32 的话 CloudCompare / pcl_viewer 这些工具认不出颜色。
    addField(blob, "rgb", offset, pcl::PCLPointField::FLOAT32);
    offset += 4;
  }

  blob.point_step = offset;
  blob.width = static_cast<std::uint32_t>(n);
  blob.height = 1;
  // row_step 是 uint32：28 字节步长下 1.53 亿点就溢出，而 assign 用的正是它，
  // 底下的写循环会直接冲出缓冲区。宁可报错也不要写坏堆。
  const std::uint64_t rowStep =
      static_cast<std::uint64_t>(blob.point_step) * static_cast<std::uint64_t>(n);
  if (rowStep > 0xFFFFFFFFull) {
    throw std::length_error("点云太大，PCLPointCloud2 的 row_step 放不下（超过 4GB）");
  }
  blob.row_step = static_cast<std::uint32_t>(rowStep);
  blob.is_bigendian = false;
  blob.is_dense = true;
  blob.data.assign(static_cast<std::size_t>(blob.row_step), 0);

  for (std::size_t i = 0; i < n; ++i) {
    std::uint8_t* p = blob.data.data() + i * blob.point_step;
    std::uint32_t o = 0;
    auto putFloat = [&](float v) {
      std::memcpy(p + o, &v, 4);
      o += 4;
    };
    putFloat(cloud.xyz[i * 3]);
    putFloat(cloud.xyz[i * 3 + 1]);
    putFloat(cloud.xyz[i * 3 + 2]);
    if (cloud.hasIntensity()) putFloat(cloud.intensity[i]);
    if (cloud.hasNormals()) {
      putFloat(cloud.normals[i * 3]);
      putFloat(cloud.normals[i * 3 + 1]);
      putFloat(cloud.normals[i * 3 + 2]);
    }
    if (cloud.hasRgb()) {
      const std::uint32_t packed = (static_cast<std::uint32_t>(cloud.rgb[i * 3]) << 16) |
                                   (static_cast<std::uint32_t>(cloud.rgb[i * 3 + 1]) << 8) |
                                   static_cast<std::uint32_t>(cloud.rgb[i * 3 + 2]);
      float asFloat;
      std::memcpy(&asFloat, &packed, 4);
      putFloat(asFloat);
    }
  }
}

}  // namespace lyflow::ops::adapter
