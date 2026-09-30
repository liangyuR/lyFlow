#include "lyflow/data.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>

#include "lyflow/json_writer.h"

namespace lyflow {
namespace {

void writePair(JsonWriter& w, const char* key, const float v[2]) {
  w.key(key);
  w.beginArray();
  w.value(static_cast<double>(v[0]));
  w.value(static_cast<double>(v[1]));
  w.endArray();
}

/// 只有像素才写：米是缺省，老的消费方看到的 JSON 一个字都不变。
void writeUnit(JsonWriter& w, Unit2D unit) {
  if (unit == Unit2D::Pixel) w.field("unit", std::string("px"));
}

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

const char* pixelDepthName(PixelDepth d) {
  switch (d) {
    case PixelDepth::U8:  return "u8";
    case PixelDepth::U16: return "u16";
    case PixelDepth::F32: return "f32";
  }
  return "u8";
}

std::size_t Image::pixelCount() const {
  if (width <= 0 || height <= 0) return 0;
  return static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
}

std::size_t Image::bytesPerPixel() const {
  return channels > 0 ? static_cast<std::size_t>(channels) * static_cast<std::size_t>(depth) : 0;
}

std::size_t Image::rowBytes() const {
  return width > 0 ? static_cast<std::size_t>(width) * bytesPerPixel() : 0;
}

std::size_t Image::byteSize() const {
  return height > 0 ? rowBytes() * static_cast<std::size_t>(height) : 0;
}

bool Image::consistent() const {
  if (width <= 0 || height <= 0) return false;
  if (channels != 1 && channels != 3 && channels != 4) return false;
  if (depth != PixelDepth::U8 && depth != PixelDepth::U16 && depth != PixelDepth::F32) return false;
  return pixels != nullptr;
}

Image Image::allocate(std::int32_t w, std::int32_t h, std::int32_t c, PixelDepth d) {
  Image img;
  img.width = w;
  img.height = h;
  img.channels = c;
  img.depth = d;
  // vector 当所有者、别名指针指向它的数据 —— 与外部所有者（cv::Mat）是同一种形状
  auto owner = std::make_shared<std::vector<std::uint8_t>>(img.byteSize());
  img.pixels = std::shared_ptr<const std::uint8_t>(owner, owner->data());
  return img;
}

double Image::at(std::int32_t x, std::int32_t y, std::int32_t c) const {
  const std::uint8_t* p = pixels.get() + static_cast<std::size_t>(y) * rowBytes() +
                          (static_cast<std::size_t>(x) * static_cast<std::size_t>(channels) +
                           static_cast<std::size_t>(c)) *
                              static_cast<std::size_t>(depth);
  switch (depth) {
    case PixelDepth::U8:
      return static_cast<double>(*p);
    case PixelDepth::U16: {
      std::uint16_t v;
      std::memcpy(&v, p, sizeof v);
      return static_cast<double>(v);
    }
    case PixelDepth::F32: {
      float v;
      std::memcpy(&v, p, sizeof v);
      return static_cast<double>(v);
    }
  }
  return 0;
}

namespace {

template <typename T>
void shrinkInto(const Image& src, Image& dst, std::size_t block) {
  const std::size_t w = static_cast<std::size_t>(src.width), h = static_cast<std::size_t>(src.height);
  const std::size_t c = static_cast<std::size_t>(src.channels);
  const std::size_t ow = static_cast<std::size_t>(dst.width);
  const T* in = reinterpret_cast<const T*>(src.pixels.get());
  T* out = reinterpret_cast<T*>(dst.mutablePixels());
  std::vector<double> sum(ow * c);
  std::vector<std::uint32_t> n(ow * c);
  for (std::size_t oy = 0; oy < static_cast<std::size_t>(dst.height); ++oy) {
    std::fill(sum.begin(), sum.end(), 0.0);
    std::fill(n.begin(), n.end(), 0u);
    const std::size_t y1 = std::min(h, (oy + 1) * block);
    for (std::size_t y = oy * block; y < y1; ++y) {
      const T* row = in + y * w * c;
      for (std::size_t x = 0; x < w; ++x) {
        const std::size_t o = (x / block) * c;
        for (std::size_t k = 0; k < c; ++k) {
          const double v = static_cast<double>(row[x * c + k]);
          if (!std::isfinite(v)) continue;
          sum[o + k] += v;
          n[o + k] += 1;
        }
      }
    }
    T* orow = out + oy * ow * c;
    for (std::size_t i = 0; i < ow * c; ++i) {
      if constexpr (std::is_floating_point_v<T>) {
        orow[i] = n[i] ? static_cast<T>(sum[i] / n[i]) : std::numeric_limits<T>::quiet_NaN();
      } else {
        orow[i] = n[i] ? static_cast<T>(std::lround(sum[i] / n[i])) : T(0);
      }
    }
  }
}

}  // namespace

Image shrinkImage(const Image& src, unsigned level) {
  if (!src.consistent()) return Image{};
  if (level == 0) return src;
  const std::size_t block = std::size_t{1} << std::min(level, 30u);
  const auto scaled = [block](std::int32_t v) {
    return static_cast<std::int32_t>((static_cast<std::size_t>(v) + block - 1) / block);
  };
  Image dst = Image::allocate(scaled(src.width), scaled(src.height), src.channels, src.depth);
  switch (src.depth) {
    case PixelDepth::U8:  shrinkInto<std::uint8_t>(src, dst, block); break;
    case PixelDepth::U16: shrinkInto<std::uint16_t>(src, dst, block); break;
    case PixelDepth::F32: shrinkInto<float>(src, dst, block); break;
  }
  return dst;
}

std::size_t Tensor::elementCount() const {
  if (shape.empty()) return 0;
  std::size_t n = 1;
  for (std::int64_t d : shape) {
    if (d <= 0) return 0;
    n *= static_cast<std::size_t>(d);
  }
  return n;
}

std::string Tensor::shapeString() const {
  std::string s = "[";
  for (std::size_t i = 0; i < shape.size(); ++i) {
    if (i) s += ",";
    s += std::to_string(shape[i]);
  }
  return s + "]";
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

Data Data::box2d(lyflow::Box2D b) {
  Data d;
  d.kind_ = Kind::Box2D;
  d.box2d_ = std::make_shared<const lyflow::Box2D>(b);
  return d;
}

Data Data::line2d(lyflow::Line2D l) {
  Data d;
  d.kind_ = Kind::Line2D;
  d.line2d_ = std::make_shared<const lyflow::Line2D>(l);
  return d;
}

Data Data::circle2d(lyflow::Circle2D c) {
  Data d;
  d.kind_ = Kind::Circle2D;
  d.circle2d_ = std::make_shared<const lyflow::Circle2D>(c);
  return d;
}

Data Data::point2d(lyflow::Point2D p) {
  Data d;
  d.kind_ = Kind::Point2D;
  d.point2d_ = std::make_shared<const lyflow::Point2D>(p);
  return d;
}

Data Data::measurement(lyflow::Measurement m) {
  Data d;
  d.kind_ = Kind::Measurement;
  d.measurement_ = std::make_shared<const lyflow::Measurement>(std::move(m));
  return d;
}

Data Data::record(lyflow::Record r) {
  Data d;
  d.kind_ = Kind::Record;
  d.record_ = std::make_shared<const lyflow::Record>(std::move(r));
  return d;
}

Data Data::tensor(std::shared_ptr<const lyflow::Tensor> t) {
  Data d;
  d.kind_ = Kind::Tensor;
  d.tensor_ = std::move(t);
  return d;
}

Data Data::error(lyflow::Status s) {
  Data d;
  d.kind_ = Kind::Error;
  d.error_ = std::make_shared<const lyflow::Status>(std::move(s));
  return d;
}

Data Data::bundle(lyflow::Bundle b) {
  if (b.typeName.empty()) b.typeName = bundleTypeName(b.kind);
  Data d;
  d.kind_ = Kind::Bundle;
  d.bundle_ = std::make_shared<const lyflow::Bundle>(std::move(b));
  return d;
}

struct Data::ValueCache {
  std::once_flag once;
  std::string json;
};

Data Data::image(std::shared_ptr<const lyflow::Image> i) {
  Data d;
  d.kind_ = Kind::Image;
  d.image_ = std::move(i);
  d.valueCache_ = std::make_shared<ValueCache>();
  return d;
}

Bundle& Bundle::set(const std::string& name, Data value) {
  for (auto& f : fields) {
    if (f.first == name) {
      f.second = std::move(value);
      return *this;
    }
  }
  fields.emplace_back(name, std::move(value));
  return *this;
}

const Data* Bundle::field(const std::string& name) const {
  for (const auto& f : fields) {
    if (f.first == name) return &f.second;
  }
  return nullptr;
}

const PointCloud* Data::asCloud() const { return kind_ == Kind::PointCloud ? cloud_.get() : nullptr; }
const Indices* Data::asIndices() const { return kind_ == Kind::Indices ? indices_.get() : nullptr; }
const Transform* Data::asTransform() const {
  return kind_ == Kind::Transform ? transform_.get() : nullptr;
}
const Plane* Data::asPlane() const { return kind_ == Kind::Plane ? plane_.get() : nullptr; }
const Box2D* Data::asBox2D() const { return kind_ == Kind::Box2D ? box2d_.get() : nullptr; }
const Line2D* Data::asLine2D() const { return kind_ == Kind::Line2D ? line2d_.get() : nullptr; }
const Circle2D* Data::asCircle2D() const {
  return kind_ == Kind::Circle2D ? circle2d_.get() : nullptr;
}
const Point2D* Data::asPoint2D() const { return kind_ == Kind::Point2D ? point2d_.get() : nullptr; }
const Measurement* Data::asMeasurement() const {
  return kind_ == Kind::Measurement ? measurement_.get() : nullptr;
}
const Record* Data::asRecord() const { return kind_ == Kind::Record ? record_.get() : nullptr; }
const Tensor* Data::asTensor() const { return kind_ == Kind::Tensor ? tensor_.get() : nullptr; }
const Status* Data::asError() const { return kind_ == Kind::Error ? error_.get() : nullptr; }
const Bundle* Data::asBundle() const { return kind_ == Kind::Bundle ? bundle_.get() : nullptr; }
const Image* Data::asImage() const { return kind_ == Kind::Image ? image_.get() : nullptr; }

const char* Data::typeName() const {
  if (kind_ == Kind::Bundle && bundle_) return bundle_->typeName.c_str();
  return typeNameFromKind(kind_);
}

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
    case Kind::Box2D:
      return sizeof(Box2D);
    case Kind::Line2D:
      return sizeof(Line2D);
    case Kind::Circle2D:
      return sizeof(Circle2D);
    case Kind::Point2D:
      return sizeof(Point2D);
    case Kind::Measurement:
      return measurement_ ? sizeof(Measurement) + measurement_->message.size() : 0;
    case Kind::Record:
      // 粗估：序列化一遍太贵，节点数乘个常数够 LRU 预算用了
      return record_ ? sizeof(Record) + record_->type.size() + record_->data.size() * 64 : 0;
    case Kind::Tensor:
      return tensor_ ? tensor_->data.size() * sizeof(float) +
                           tensor_->shape.size() * sizeof(std::int64_t)
                     : 0;
    case Kind::Error:
      return error_ ? sizeof(Status) + error_->code.size() + error_->message.size() : 0;
    case Kind::Bundle: {
      // 字段各算各的：一片点云装进 Bundle 不该让 LRU 预算以为它没了
      if (!bundle_) return 0;
      std::size_t total = sizeof(Bundle) + bundle_->kind.size();
      for (const auto& f : bundle_->fields) total += f.first.size() + f.second.byteSize();
      return total;
    }
    case Kind::Image:
      return image_ ? image_->byteSize() : 0;
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
    case Kind::Box2D:
    case Kind::Line2D:
    case Kind::Circle2D:
    case Kind::Point2D:
    case Kind::Measurement:
    case Kind::Record:
    case Kind::Error:
      return 1;
    case Kind::Tensor:
      return tensor_ ? tensor_->data.size() : 0;
    case Kind::Bundle:
      return bundle_ ? bundle_->fields.size() : 0;
    case Kind::Image:
      return image_ ? image_->pixelCount() : 0;
  }
  return 0;
}

std::string Data::valueJson() const {
  if (valueCache_) {
    std::call_once(valueCache_->once, [this] { valueCache_->json = valueJsonUncached(); });
    return valueCache_->json;
  }
  return valueJsonUncached();
}

std::string Data::valueJsonUncached() const {
  // 点云与 Indices 走二进制通道（ADR-0006），这里给空串。
  if (kind_ == Kind::None || kind_ == Kind::PointCloud || kind_ == Kind::Indices) return {};

  JsonWriter w;
  w.setIndent(0);
  w.beginObject();
  w.field("kind", std::string(typeName()));
  switch (kind_) {
    case Kind::Transform: {
      w.key("m");
      w.beginArray();
      for (int i = 0; i < 16; ++i) w.value(static_cast<double>(transform_->m[i]));
      w.endArray();
      break;
    }
    case Kind::Plane: {
      w.key("normal");
      w.beginArray();
      for (int i = 0; i < 3; ++i) w.value(static_cast<double>(plane_->normal[i]));
      w.endArray();
      w.field("d", static_cast<double>(plane_->d));
      break;
    }
    case Kind::Box2D:
      writePair(w, "min", box2d_->min);
      writePair(w, "max", box2d_->max);
      writeUnit(w, box2d_->unit);
      break;
    case Kind::Line2D:
      writePair(w, "point", line2d_->point);
      writePair(w, "dir", line2d_->dir);
      w.field("hasSegment", line2d_->hasSegment);
      if (line2d_->hasSegment) {
        writePair(w, "start", line2d_->start);
        writePair(w, "end", line2d_->end);
      }
      writeUnit(w, line2d_->unit);
      break;
    case Kind::Circle2D:
      writePair(w, "center", circle2d_->center);
      w.field("radius", static_cast<double>(circle2d_->radius));
      writeUnit(w, circle2d_->unit);
      break;
    case Kind::Point2D:
      writePair(w, "p", point2d_->p);
      writeUnit(w, point2d_->unit);
      break;
    case Kind::Measurement: {
      const Measurement& m = *measurement_;
      w.field("value", m.value);  // 非有限值写成 null，见 jsonNumber
      w.field("ok", m.ok);
      w.fieldIfSet("unit", m.unit);
      w.fieldIfSet("message", m.message);
      w.fieldIfSet("verdict", m.verdict);
      if (m.hasLimits) {
        w.field("nominal", m.nominal);
        w.field("upper", m.upper);
        w.field("lower", m.lower);
      }
      break;
    }
    case Kind::Record:
      w.field("type", record_->type);
      w.key("data");
      w.raw(record_->data.dump());
      break;
    case Kind::Tensor: {
      const Tensor& t = *tensor_;
      w.key("shape");
      w.beginArray();
      for (std::int64_t d : t.shape) w.value(d);
      w.endArray();
      w.field("count", static_cast<std::int64_t>(t.data.size()));
      // 张量本身不进 IPC，Inspector 只看得到这三个数（T7）
      double lo = 0, hi = 0, sum = 0;
      std::size_t finite = 0;
      for (float v : t.data) {
        if (!std::isfinite(v)) continue;
        const double x = static_cast<double>(v);
        if (finite == 0) { lo = hi = x; } else { lo = std::min(lo, x); hi = std::max(hi, x); }
        sum += x;
        ++finite;
      }
      const double nan = std::numeric_limits<double>::quiet_NaN();
      w.field("min", finite ? lo : nan);
      w.field("max", finite ? hi : nan);
      w.field("mean", finite ? sum / static_cast<double>(finite) : nan);
      break;
    }
    case Kind::Image: {
      // 像素走二进制（lyflow_output_image）；这里只给尺寸与逐通道统计，只算有限值（同 Tensor）
      const Image& img = *image_;
      w.field("width", static_cast<std::int64_t>(img.width));
      w.field("height", static_cast<std::int64_t>(img.height));
      w.field("channels", static_cast<std::int64_t>(img.channels));
      w.field("depth", std::string(pixelDepthName(img.depth)));
      const int nc = img.consistent() ? img.channels : 0;
      std::vector<double> lo(nc, 0), hi(nc, 0), sum(nc, 0);
      std::vector<std::size_t> finite(nc, 0);
      for (std::int32_t y = 0; nc > 0 && y < img.height; ++y) {
        for (std::int32_t x = 0; x < img.width; ++x) {
          for (int c = 0; c < nc; ++c) {
            const double v = img.at(x, y, c);
            if (!std::isfinite(v)) continue;
            if (finite[c] == 0) {
              lo[c] = hi[c] = v;
            } else {
              lo[c] = std::min(lo[c], v);
              hi[c] = std::max(hi[c], v);
            }
            sum[c] += v;
            ++finite[c];
          }
        }
      }
      const double nan = std::numeric_limits<double>::quiet_NaN();
      const auto series = [&](const char* key, auto pick) {
        w.key(key);
        w.beginArray();
        for (int c = 0; c < nc; ++c) w.value(finite[c] ? pick(c) : nan);
        w.endArray();
      };
      series("min", [&](int c) { return lo[c]; });
      series("max", [&](int c) { return hi[c]; });
      series("mean", [&](int c) { return sum[c] / static_cast<double>(finite[c]); });
      break;
    }
    case Kind::Error: {
      const Status& s = *error_;
      w.field("phase", std::string(toString(s.phase)));
      w.field("code", s.code);
      w.field("message", s.message);
      w.fieldIfSet("paramPath", s.paramPath);
      w.fieldIfSet("portName", s.portName);
      break;
    }
    case Kind::Bundle: {
      // 字段按声明顺序列出，每项是那个字段自己的类型与元素数；非点云字段带 value。
      // 点云字段仍走二进制（`<port>.<field>` 寻址，m8-plan L3）。
      w.field("bundleKind", bundle_->kind);
      w.key("fields");
      w.beginArray();
      for (const auto& f : bundle_->fields) {
        w.beginObject();
        w.field("name", f.first);
        w.field("type", std::string(f.second.typeName()));
        w.field("elementCount", static_cast<std::int64_t>(f.second.elementCount()));
        const std::string inner = f.second.valueJson();
        if (!inner.empty()) {
          w.key("value");
          w.raw(inner);
        }
        w.endObject();
      }
      w.endArray();
      break;
    }
    default:
      break;
  }
  w.endObject();
  return w.str();
}

Data::Kind kindFromTypeName(const std::string& typeName) {
  if (typeName == "PointCloud") return Data::Kind::PointCloud;
  if (typeName == "Indices") return Data::Kind::Indices;
  if (typeName == "Transform") return Data::Kind::Transform;
  if (typeName == "Plane") return Data::Kind::Plane;
  if (typeName == "Box2D") return Data::Kind::Box2D;
  if (typeName == "Line2D") return Data::Kind::Line2D;
  if (typeName == "Circle2D") return Data::Kind::Circle2D;
  if (typeName == "Point2D") return Data::Kind::Point2D;
  if (typeName == "Measurement") return Data::Kind::Measurement;
  if (typeName == "Record") return Data::Kind::Record;
  if (typeName == "Tensor") return Data::Kind::Tensor;
  if (typeName == "Error") return Data::Kind::Error;
  if (typeName == "Image") return Data::Kind::Image;
  if (parseBundleType(typeName, nullptr)) return Data::Kind::Bundle;
  return Data::Kind::None;  // 含 "Any"：不约束具体载荷
}

bool parseBundleType(const std::string& typeName, std::string* bundleKind) {
  static const std::string kPrefix = "Bundle<";
  if (typeName.size() <= kPrefix.size() + 1) return false;
  if (typeName.compare(0, kPrefix.size(), kPrefix) != 0 || typeName.back() != '>') return false;
  const std::string inner = typeName.substr(kPrefix.size(), typeName.size() - kPrefix.size() - 1);
  if (inner.empty() || inner.find_first_of("<> ") != std::string::npos) return false;
  if (bundleKind) *bundleKind = inner;
  return true;
}

std::string bundleTypeName(const std::string& bundleKind) { return "Bundle<" + bundleKind + ">"; }

const char* typeNameFromKind(Data::Kind kind) {
  switch (kind) {
    case Data::Kind::None:        return "None";
    case Data::Kind::PointCloud:  return "PointCloud";
    case Data::Kind::Indices:     return "Indices";
    case Data::Kind::Transform:   return "Transform";
    case Data::Kind::Plane:       return "Plane";
    case Data::Kind::Box2D:       return "Box2D";
    case Data::Kind::Line2D:      return "Line2D";
    case Data::Kind::Circle2D:    return "Circle2D";
    case Data::Kind::Point2D:     return "Point2D";
    case Data::Kind::Measurement: return "Measurement";
    case Data::Kind::Record:      return "Record";
    case Data::Kind::Tensor:      return "Tensor";
    case Data::Kind::Error:       return "Error";
    case Data::Kind::Bundle:      return "Bundle";
    case Data::Kind::Image:       return "Image";
  }
  return "None";
}

}  // namespace lyflow
