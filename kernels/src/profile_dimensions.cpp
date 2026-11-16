#include "lyflow/kernel/profile_dimensions.h"

#include <cmath>
#include <limits>

namespace lyflow::kernel::profile_dimensions {
namespace {
using Pair = std::array<float, 2>;

bool finite(Pair value) { return std::isfinite(value[0]) && std::isfinite(value[1]); }
Pair difference(Pair a, Pair b) { return {a[0] - b[0], a[1] - b[1]}; }
float dot(Pair a, Pair b) { return a[0] * b[0] + a[1] * b[1]; }
float norm(Pair value) { return std::sqrt(dot(value, value)); }
bool normalize(Pair& value) {
  const float length = norm(value);
  if (!(length > 0) || !std::isfinite(length))
    return false;
  value[0] /= length;
  value[1] /= length;
  return finite(value);
}
bool pointValid(const Point2D& point) {
  return point.unit == Unit2D::Meter && finite({point.p[0], point.p[1]});
}
bool lineValid(const Line2D& line) {
  return line.unit == Unit2D::Meter && finite({line.point[0], line.point[1]}) &&
         finite({line.dir[0], line.dir[1]}) && (line.dir[0] != 0 || line.dir[1] != 0) &&
         (!line.hasSegment ||
          (finite({line.start[0], line.start[1]}) && finite({line.end[0], line.end[1]})));
}
bool circleValid(const Circle2D& circle) {
  return circle.unit == Unit2D::Meter && finite({circle.center[0], circle.center[1]}) &&
         std::isfinite(circle.radius) && circle.radius > 0;
}
Line2D segment(Pair a, Pair b) {
  Line2D result;
  result.hasSegment = true;
  Pair direction = difference(b, a);
  const float length = norm(direction);
  result.point[0] = result.start[0] = a[0];
  result.point[1] = result.start[1] = a[1];
  result.end[0] = b[0];
  result.end[1] = b[1];
  result.dir[0] = length > 0 ? direction[0] / length : 1.f;
  result.dir[1] = length > 0 ? direction[1] / length : 0.f;
  return result;
}
bool fail(std::string& why, const char* message) {
  why = message;
  return false;
}
} // namespace

bool flushPointLine(const Line2D& base, const Point2D& reference, const FlushOptions& options,
                    FlushResult& output, std::string& why) {
  if (!lineValid(base) || !pointValid(reference))
    return fail(why, "面差需要有限、米制的点和非零方向直线");
  if (!std::isfinite(options.gain) || !std::isfinite(options.offsetMm))
    return fail(why, "面差增益和毫米偏置必须有限");
  if (options.sign != FlushSign::CanonicalAxis && options.sign != FlushSign::PositiveNormal)
    return fail(why, "面差符号策略非法");
  Pair normal{base.dir[1], -base.dir[0]};
  if (!normalize(normal))
    return fail(why, "面差直线方向无法归一化");
  if (normal[1] > 0)
    normal = {-normal[0], -normal[1]};
  const Pair end{reference.p[0], reference.p[1]};
  const float distance = dot(normal, difference(end, {base.point[0], base.point[1]}));
  const Pair start{end[0] - distance * normal[0], end[1] - distance * normal[1]};
  if (!finite(start))
    return fail(why, "面差垂足超出float32范围");
  const Pair displacement = difference(end, start);
  double valueMm = static_cast<double>(norm(displacement)) * 1000;
  if (options.signedDistance) {
    if (options.sign == FlushSign::CanonicalAxis) {
      const double length =
          std::hypot(static_cast<double>(base.dir[0]), static_cast<double>(base.dir[1]));
      double x = base.dir[0] / length, y = base.dir[1] / length;
      if (x < 0 || (x == 0 && y < 0)) {
        x = -x;
        y = -y;
      }
      normal = {static_cast<float>(y), static_cast<float>(-x)};
    } else {
      if (!finite(options.positiveNormal) ||
          (options.positiveNormal[0] == 0 && options.positiveNormal[1] == 0))
        return fail(why, "正法向参考必须为有限非零向量");
      const double alignment = static_cast<double>(normal[0]) * options.positiveNormal[0] +
                               static_cast<double>(normal[1]) * options.positiveNormal[1];
      if (!std::isfinite(alignment) || alignment == 0)
        return fail(why, "正法向参考不能与直线法向正交");
      if (alignment < 0)
        normal = {-normal[0], -normal[1]};
    }
    valueMm = static_cast<double>(dot(normal, displacement)) * 1000;
  }
  FlushResult result;
  result.valueMm = valueMm * options.gain + options.offsetMm;
  if (!std::isfinite(result.valueMm))
    return fail(why, "面差结果非有限");
  result.foot.p[0] = start[0];
  result.foot.p[1] = start[1];
  result.segment = segment(start, end);
  result.baseLine = base;
  if (base.hasSegment) {
    const Pair a{base.start[0], base.start[1]}, b{base.end[0], base.end[1]};
    const double length = norm(difference(b, a));
    const double first = norm(difference(start, a)), second = norm(difference(start, b));
    if (!std::isfinite(length) || !std::isfinite(first) || !std::isfinite(second))
      return fail(why, "基准线段扩展距离非有限");
    if (first > length || second > length) {
      float* target = first > second ? result.baseLine.end : result.baseLine.start;
      target[0] = start[0];
      target[1] = start[1];
    }
  }
  if (!lineValid(result.segment))
    return fail(why, "面差显示线段无法表示");
  output = result;
  why.clear();
  return true;
}

bool circleGap(const Circle2D& left, const Circle2D& right, const Line2D* base,
               const CircleGapOptions& options, CircleGapResult& output, std::string& why) {
  if (!circleValid(left) || !circleValid(right))
    return fail(why, "间隙需要有限米制圆且半径大于零");
  if (!std::isfinite(options.offsetMm))
    return fail(why, "间隙毫米偏置必须有限");
  const Pair c1{left.center[0], left.center[1]}, c2{right.center[0], right.center[1]};
  Pair start, end;
  if (options.definition == CircleGapDefinition::DatumTangents) {
    if (!base || !lineValid(*base) || !base->hasSegment)
      return fail(why, "切线定义需要带有限端点的米制基准线段");
    Pair direction{base->end[0] - base->start[0], base->end[1] - base->start[1]};
    if (!normalize(direction))
      return fail(why, "切线定义的基准线段端点重合或无法归一化");
    if (direction[0] < 0)
      direction = {-direction[0], -direction[1]};
    const float gap = dot(difference(c2, c1), direction) - left.radius - right.radius;
    if (!(gap >= 0) || !std::isfinite(gap))
      return fail(why, "definition A：两条切线交叉了（gap < 0）");
    start = {c1[0] + left.radius * direction[0], c1[1] + left.radius * direction[1]};
    end = {start[0] + gap * direction[0], start[1] + gap * direction[1]};
  } else if (options.definition == CircleGapDefinition::CenterLine) {
    Pair direction = difference(c2, c1);
    if (!normalize(direction))
      return fail(why, "圆心重合或圆心连线无法归一化");
    start = {direction[0] * left.radius + c1[0], direction[1] * left.radius + c1[1]};
    end = {c2[0] - direction[0] * right.radius, c2[1] - direction[1] * right.radius};
  } else {
    return fail(why, "圆间隙定义非法");
  }
  if (!finite(start) || !finite(end))
    return fail(why, "圆间隙端点超出float32范围");
  CircleGapResult result;
  result.crossed = start[0] > end[0];
  result.valid = !result.crossed;
  result.valueMm =
      result.crossed
          ? std::numeric_limits<double>::quiet_NaN()
          : std::fabs(static_cast<double>(norm(difference(end, start))) * 1000) + options.offsetMm;
  result.segment = segment(start, end);
  if ((!result.crossed && !std::isfinite(result.valueMm)) || !lineValid(result.segment))
    return fail(why, "圆间隙结果无法表示");
  output = result;
  why.clear();
  return true;
}

bool pointOffset(const Point2D& a, const Point2D& b, const PointOffsetOptions& options,
                 PointOffsetResult& output, std::string& why) {
  if (!pointValid(a) || !pointValid(b))
    return fail(why, "位移分解需要有限米制点");
  if (!std::isfinite(options.gainDx) || !std::isfinite(options.gainDy) ||
      !std::isfinite(options.dxOffsetMm) || !std::isfinite(options.dyOffsetMm))
    return fail(why, "位移增益和毫米偏置必须有限");
  const Pair start{a.p[0], a.p[1]}, end{b.p[0], b.p[1]};
  const Pair displacement = difference(end, start);
  PointOffsetResult result;
  result.dxMm = static_cast<double>(displacement[0]) * 1000 * options.gainDx;
  if (options.absDx)
    result.dxMm = std::fabs(result.dxMm);
  result.dxMm += options.dxOffsetMm;
  result.dyMm = static_cast<double>(displacement[1]) * 1000 * options.gainDy;
  if (options.absDy)
    result.dyMm = std::fabs(result.dyMm);
  result.dyMm += options.dyOffsetMm;
  result.distanceMm = static_cast<double>(norm(displacement)) * 1000;
  result.segment = segment(start, end);
  if (!std::isfinite(result.dxMm) || !std::isfinite(result.dyMm) ||
      !std::isfinite(result.distanceMm) || !lineValid(result.segment))
    return fail(why, "位移结果超出有限数值范围");
  output = result;
  why.clear();
  return true;
}

} // namespace lyflow::kernel::profile_dimensions
