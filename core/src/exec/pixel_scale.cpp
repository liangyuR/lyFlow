#include "exec/pixel_scale.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "exec/plan.h"

namespace lyflow::exec {

unsigned previewLevel(const Image& img, std::size_t maxPixels) {
  if (!img.consistent() || maxPixels == 0) return 0;
  // 与 shrinkImage 同一种取整：每一级都是向上取整的一半
  std::size_t w = static_cast<std::size_t>(img.width);
  std::size_t h = static_cast<std::size_t>(img.height);
  unsigned level = 0;
  while (w * h > maxPixels && level < 30) {
    w = (w + 1) / 2;
    h = (h + 1) / 2;
    ++level;
  }
  return level;
}

std::int32_t inputScale(const std::unordered_map<std::string, Data>& inputs) {
  std::int32_t s = 1;
  for (const auto& kv : inputs) {
    if (const Image* img = kv.second.asImage()) s = std::max(s, img->scale);
    // 从缩小过的图转来的张量也算（review 第二轮）：tensor.to_image 的输出才知道自己是小图
    if (kv.second.asTensor()) s = std::max(s, kv.second.tensorScale());
  }
  return s;
}

namespace {

/// 只换算「图上的距离」：绝对尺寸（输出图的宽高）在预览里照旧是用户填的那个数。
bool scalesWithImage(const Param& p) { return p.unit == "px" && !p.absolute; }

double clampTo(const Param& p, double v) {
  if (p.min) v = std::max(v, *p.min);
  if (p.max) v = std::min(v, *p.max);
  return v;
}

void scale2(float v[2], double f) {
  v[0] = static_cast<float>(v[0] * f);
  v[1] = static_cast<float>(v[1] * f);
}

}  // namespace

ParamMap scalePixelParams(const OperatorDesc& op, const ParamMap& params, std::int32_t s) {
  ParamMap out = params;
  if (s <= 1) return out;
  const double f = 1.0 / static_cast<double>(s);
  for (const Param& p : op.params) {
    if (!scalesWithImage(p)) continue;
    auto it = out.find(p.name);
    if (it == out.end()) continue;
    const Value& v = it->second;
    switch (v.kind()) {
      case Value::Kind::Float:
        it->second = Value::number(clampTo(p, v.floatValue() * f));
        break;
      case Value::Kind::Int: {
        double x = static_cast<double>(v.intValue()) * f;
        if (p.step && *p.step > 0) {
          // 落回 min + n × step：奇数核（min 1、step 2）换算后还是奇数
          const double base = p.min.value_or(0.0);
          x = base + std::round((x - base) / *p.step) * *p.step;
        } else {
          x = std::round(x);
        }
        it->second = Value::integer(static_cast<std::int64_t>(clampTo(p, x)));
        break;
      }
      case Value::Kind::FloatVec: {
        std::vector<double> vec = v.vecValue();
        for (double& c : vec) c = clampTo(p, c * f);
        it->second = Value::vec(std::move(vec));
        break;
      }
      default:
        break;
    }
  }
  return out;
}

bool hasActiveAbsoluteSize(const OperatorDesc& op, const ParamMap& params) {
  for (const Param& p : op.params) {
    if (p.absolute && p.unit == "px" && conditionHolds(p.visibleWhen, params)) return true;
  }
  return false;
}

Data scalePixelData(const Data& d, double factor) {
  switch (d.kind()) {
    case Data::Kind::Box2D: {
      Box2D b = *d.asBox2D();
      if (b.unit != Unit2D::Pixel) return d;
      scale2(b.min, factor);
      scale2(b.max, factor);
      return Data::box2d(b);
    }
    case Data::Kind::Line2D: {
      Line2D l = *d.asLine2D();
      if (l.unit != Unit2D::Pixel) return d;
      // dir 是单位向量，等比缩放下不变
      scale2(l.point, factor);
      scale2(l.start, factor);
      scale2(l.end, factor);
      return Data::line2d(l);
    }
    case Data::Kind::Circle2D: {
      Circle2D c = *d.asCircle2D();
      if (c.unit != Unit2D::Pixel) return d;
      scale2(c.center, factor);
      c.radius = static_cast<float>(c.radius * factor);
      return Data::circle2d(c);
    }
    case Data::Kind::Point2D: {
      Point2D p = *d.asPoint2D();
      if (p.unit != Unit2D::Pixel) return d;
      scale2(p.p, factor);
      return Data::point2d(p);
    }
    case Data::Kind::Measurement: {
      Measurement m = *d.asMeasurement();
      double k = 0;
      if (m.unit == "px") {
        k = factor;
      } else if (m.unit == "px²") {
        k = factor * factor;
      } else {
        return d;
      }
      m.value *= k;
      m.nominal *= k;
      m.upper *= k;
      m.lower *= k;
      return Data::measurement(m);
    }
    case Data::Kind::Bundle: {
      const Bundle& b = *d.asBundle();
      Bundle out(b.kind);
      for (const auto& field : b.fields) out.set(field.first, scalePixelData(field.second, factor));
      return Data::bundle(std::move(out));
    }
    default:
      return d;
  }
}

Data toNodeScale(const Data& d, std::int32_t s) {
  if (s <= 1) return d;
  if (const Image* img = d.asImage()) {
    if (img->scale >= s) return d;
    // 比例都是 2 的幂：差几倍就再缩几级
    unsigned level = 0;
    while ((static_cast<std::int64_t>(img->scale) << level) < s && level < 30) ++level;
    return Data::image(shrinkImage(*img, level));
  }
  return scalePixelData(d, 1.0 / static_cast<double>(s));
}

void scaleOutputs(std::unordered_map<std::string, Data>& outputs, std::int32_t s,
                  bool absoluteSize) {
  if (s <= 1) return;
  for (auto& kv : outputs) {
    if (kv.second.asTensor()) {
      if (kv.second.tensorScale() == 1 && !absoluteSize) kv.second = kv.second.withTensorScale(s);
      continue;
    }
    if (const Image* img = kv.second.asImage()) {
      // 原样传出去的输入图本来就带着 s；新产出的图默认是 1，在这里补上
      if (img->scale == 1 && !absoluteSize) {
        Image copy = *img;
        copy.scale = s;
        kv.second = Data::image(std::move(copy));
      }
      continue;
    }
    kv.second = scalePixelData(kv.second, static_cast<double>(s));
  }
}

}  // namespace lyflow::exec
