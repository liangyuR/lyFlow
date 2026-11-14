#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

#include "lyflow/operator.h"
#include "lyflow/registry.h"
#include "lyflow/value.h"

namespace lyflow::packs::std_geometry {
void registerPoseOps(Registry&);
void registerGeometryTransforms(Registry&);
namespace {

struct Vec {
  double x = 0, y = 0;
  Vec operator+(Vec b) const { return {x + b.x, y + b.y}; }
  Vec operator-(Vec b) const { return {x - b.x, y - b.y}; }
  Vec operator*(double k) const { return {x * k, y * k}; }
};

double length(Vec v) { return std::hypot(v.x, v.y); }
bool finite(Vec v) { return std::isfinite(v.x) && std::isfinite(v.y); }
Vec vec(const float* p) { return {p[0], p[1]}; }
bool representable(Vec v) {
  const double max = std::numeric_limits<float>::max();
  return finite(v) && std::abs(v.x) <= max && std::abs(v.y) <= max;
}

struct ExactVec {
  Vec high, low;
};

std::pair<double, double> exactDifference(double a, double b) {
  const double high = a - b;
  const double virtualB = a - high;
  const double low = (a - (high + virtualB)) + (virtualB - b);
  return {high, low};
}

ExactVec exactDifference(Vec a, Vec b) {
  const auto x = exactDifference(a.x, b.x), y = exactDifference(a.y, b.y);
  return {{x.first, y.first}, {x.second, y.second}};
}

class Expansion {
public:
  void add(double value) {
    if (value == 0)
      return;
    std::size_t size = 0;
    for (std::size_t i = 0; i < size_; ++i) {
      const double sum = value + parts_[i];
      const double virtualB = sum - value;
      const double error = (value - (sum - virtualB)) + (parts_[i] - virtualB);
      if (error != 0)
        parts_[size++] = error;
      value = sum;
    }
    if (value != 0)
      parts_[size++] = value;
    size_ = size;
  }

  void addProduct(double a, double b, double sign) {
    const double product = a * b;
    add(sign * std::fma(a, b, -product));
    add(sign * product);
  }

  void addScaled(const Expansion& value, double factor) {
    for (std::size_t i = 0; i < value.size_; ++i)
      addProduct(value.parts_[i], factor, 1);
  }

  void addProduct(const Expansion& a, const Expansion& b, double sign) {
    for (std::size_t i = 0; i < a.size_; ++i)
      for (std::size_t j = 0; j < b.size_; ++j)
        addProduct(a.parts_[i], b.parts_[j], sign);
  }

  double value() const {
    double result = 0;
    for (std::size_t i = 0; i < size_; ++i)
      result += parts_[i];
    return result;
  }

private:
  std::array<double, 128> parts_{};
  std::size_t size_ = 0;
};

Expansion crossExpansion(const ExactVec& a, const ExactVec& b) {
  Expansion result;
  for (Vec av : {a.high, a.low}) {
    for (Vec bv : {b.high, b.low}) {
      result.addProduct(av.x, bv.y, 1);
      result.addProduct(av.y, bv.x, -1);
    }
  }
  return result;
}

double cross(const ExactVec& a, const ExactVec& b) { return crossExpansion(a, b).value(); }

void addDot(Expansion& result, const ExactVec& a, const ExactVec& b, double sign) {
  for (Vec av : {a.high, a.low}) {
    for (Vec bv : {b.high, b.low}) {
      result.addProduct(av.x, bv.x, sign);
      result.addProduct(av.y, bv.y, sign);
    }
  }
}

double dot(const ExactVec& a, const ExactVec& b) {
  Expansion result;
  addDot(result, a, b, 1);
  return result.value();
}

Expansion dotExpansion(const ExactVec& a, const ExactVec& b) {
  Expansion result;
  addDot(result, a, b, 1);
  return result;
}

struct Geometry {
  enum Kind { Point, Line, Circle } kind = Point;
  Vec p, d, end;
  ExactVec rawDirection;
  double directionLength = 1;
  double radius = 0;
  bool segment = false;
  Unit2D unit = Unit2D::Meter;
};

Status bad(const std::string& message, const std::string& port = {}) {
  return Status::Error(Phase::Execute, "bad_input", message, {}, port);
}
Status badParam(const std::string& message, const std::string& name) {
  return Status::Error(Phase::Execute, "bad_param", message, name);
}

Status read(const Data& data, const std::string& port, Geometry& out) {
  if (const Point2D* p = data.asPoint2D()) {
    out.kind = Geometry::Point;
    out.p = vec(p->p);
    out.unit = p->unit;
  } else if (const Line2D* l = data.asLine2D()) {
    out.kind = Geometry::Line;
    out.unit = l->unit;
    out.segment = l->hasSegment;
    out.p = vec(l->point);
    out.d = vec(l->dir);
    if (!finite(out.d) || !(length(out.d) > 0))
      return bad("直线方向必须有限且非零", port);
    if (l->hasSegment) {
      out.p = vec(l->start);
      const Vec end = vec(l->end);
      if (!finite(end))
        return bad("线段端点必须有限", port);
      out.end = end;
      out.rawDirection = exactDifference(end, out.p);
      out.d = out.rawDirection.high;
      if (!(length(out.d) > 0))
        return bad("线段两个端点不能重合", port);
    } else {
      out.rawDirection = {out.d, {}};
    }
    out.directionLength = length(out.d);
    out.d = out.d * (1.0 / out.directionLength);
  } else if (const Circle2D* c = data.asCircle2D()) {
    out.kind = Geometry::Circle;
    out.p = vec(c->center);
    out.radius = c->radius;
    out.unit = c->unit;
    if (!std::isfinite(out.radius) || !(out.radius > 0))
      return bad("圆半径必须有限且为正", port);
  } else {
    return bad("只接受 Point2D、Line2D 或 Circle2D", port);
  }
  if (!finite(out.p) || (out.kind == Geometry::Line && !finite(out.d)))
    return bad("几何坐标必须有限", port);
  if (out.unit != Unit2D::Meter && out.unit != Unit2D::Pixel)
    return bad("未知二维几何单位", port);
  return Status::Ok();
}

Status pair(const Inputs& inputs, Geometry& a, Geometry& b) {
  Status s = read(inputs.get("a"), "a", a);
  if (!s.ok)
    return s;
  s = read(inputs.get("b"), "b", b);
  if (!s.ok)
    return s;
  return a.unit == b.unit ? Status::Ok() : bad("两个几何体的单位必须一致，不自动换算", "b");
}

Vec pointFromAnchor(Vec anchor, const Geometry& line, double parameter) {
  if (parameter == 0)
    return anchor;
  return {std::fma(line.rawDirection.low.x, parameter,
                   std::fma(line.rawDirection.high.x, parameter, anchor.x)),
          std::fma(line.rawDirection.low.y, parameter,
                   std::fma(line.rawDirection.high.y, parameter, anchor.y))};
}

double endpointProjection(Vec point, Vec anchor, const Geometry& line) {
  return dot(exactDifference(point, anchor), line.rawDirection) / line.directionLength;
}

bool inSegment(Vec point, const Geometry& line, double eps) {
  return !line.segment || (endpointProjection(point, line.p, line) >= -eps &&
                           endpointProjection(point, line.end, line) <= eps);
}

double perpendicular(Vec point, const Geometry& line) {
  return cross(line.rawDirection, exactDifference(point, line.p)) / line.directionLength;
}

Vec rationalPoint(Vec anchor, const ExactVec& direction, const Expansion& numerator,
                  const Expansion& denominator) {
  Expansion x, y;
  x.addScaled(denominator, anchor.x);
  x.addScaled(numerator, direction.high.x);
  x.addScaled(numerator, direction.low.x);
  y.addScaled(denominator, anchor.y);
  y.addScaled(numerator, direction.high.y);
  y.addScaled(numerator, direction.low.y);
  return {x.value() / denominator.value(), y.value() / denominator.value()};
}

Vec projectedPoint(Vec point, const Geometry& line, bool clamp) {
  if (clamp && line.segment) {
    if (endpointProjection(point, line.p, line) < 0)
      return line.p;
    if (endpointProjection(point, line.end, line) > 0)
      return line.end;
  }
  const Expansion denominator = dotExpansion(line.rawDirection, line.rawDirection);
  const Expansion along = dotExpansion(exactDifference(point, line.p), line.rawDirection);
  return rationalPoint(line.p, line.rawDirection, along, denominator);
}

double pointLineDistance(Vec p, const Geometry& line) {
  if (line.segment && endpointProjection(p, line.p, line) < 0)
    return length(p - line.p);
  if (line.segment && endpointProjection(p, line.end, line) > 0)
    return length(p - line.end);
  return std::abs(perpendicular(p, line));
}

struct Intersection {
  std::vector<Vec> points;
  bool coincident = false;
};

bool onLine(Vec p, const Geometry& line, double eps) {
  return std::abs(perpendicular(p, line)) <= eps && inSegment(p, line, eps);
}

struct LinePosition {
  double start = 0, end = 0;
};

struct LineParameters {
  bool parallel;
  LinePosition a, b;
};

LineParameters lineParameters(const Geometry& a, const Geometry& b) {
  const double denominator = cross(a.rawDirection, b.rawDirection);
  if (denominator == 0)
    return {true};
  const ExactVec delta = exactDifference(b.p, a.p);
  return {false,
          {cross(delta, b.rawDirection) / denominator,
           a.segment ? cross(exactDifference(b.p, a.end), b.rawDirection) / denominator : 0},
          {cross(delta, a.rawDirection) / denominator,
           b.segment ? cross(exactDifference(b.end, a.p), a.rawDirection) / denominator : 0}};
}

bool inSegment(LinePosition position, const Geometry& line, double eps) {
  return !line.segment || (position.start * line.directionLength >= -eps &&
                           position.end * line.directionLength <= eps);
}

Vec intersectionPoint(const Geometry& a, const Geometry& b) {
  const Expansion denominator = crossExpansion(a.rawDirection, b.rawDirection);
  const Expansion along = crossExpansion(exactDifference(b.p, a.p), b.rawDirection);
  return rationalPoint(a.p, a.rawDirection, along, denominator);
}

Intersection collinearOverlap(const Geometry& a, const Geometry& b, double eps) {
  const bool xAxis = std::abs(a.rawDirection.high.x) >= std::abs(a.rawDirection.high.y);
  const auto axis = [&](Vec p) { return xAxis ? p.x : p.y; };
  const double component = xAxis ? a.rawDirection.high.x : a.rawDirection.high.y;
  const double axisScale = a.directionLength / std::abs(component);
  const double inf = std::numeric_limits<double>::infinity();
  double lo = a.segment ? std::min(axis(a.p), axis(a.end)) : -inf;
  double hi = a.segment ? std::max(axis(a.p), axis(a.end)) : inf;
  if (b.segment) {
    lo = std::max(lo, std::min(axis(b.p), axis(b.end)));
    hi = std::min(hi, std::max(axis(b.p), axis(b.end)));
  }
  if ((lo - hi) * axisScale > eps)
    return {};
  if (std::isfinite(lo) && std::isfinite(hi) && (hi - lo) * axisScale <= eps) {
    const double coordinate = lo + (hi - lo) * 0.5;
    const Geometry* reference = &a;
    Vec anchor = a.p;
    const auto consider = [&](const Geometry& line, Vec p) {
      if (std::abs(coordinate - axis(p)) < std::abs(coordinate - axis(anchor))) {
        reference = &line;
        anchor = p;
      }
    };
    if (a.segment)
      consider(a, a.end);
    consider(b, b.p);
    if (b.segment)
      consider(b, b.end);
    const auto delta = exactDifference(coordinate, axis(anchor));
    const double direction =
        xAxis ? reference->rawDirection.high.x : reference->rawDirection.high.y;
    return {{pointFromAnchor(anchor, *reference, (delta.first + delta.second) / direction)}, false};
  }
  return {{}, true};
}

Intersection lineLine(const Geometry& a, const Geometry& b, double eps) {
  const LineParameters parameters = lineParameters(a, b);
  if (!parameters.parallel) {
    if (!inSegment(parameters.a, a, eps) || !inSegment(parameters.b, b, eps))
      return {};
    return {{intersectionPoint(a, b)}, false};
  }
  const ExactVec delta = exactDifference(b.p, a.p);
  if (std::abs(cross(delta, a.rawDirection)) / a.directionLength > eps)
    return {};
  return collinearOverlap(a, b, eps);
}

double perpendicularGap(const Geometry& line, const Geometry& circle) {
  const Expansion normal = crossExpansion(line.rawDirection, exactDifference(circle.p, line.p));
  const Expansion squaredLength = dotExpansion(line.rawDirection, line.rawDirection);
  Expansion power;
  power.addProduct(normal, normal, 1);
  power.addScaled(squaredLength, -circle.radius * circle.radius);
  const double d = std::abs(normal.value()) / line.directionLength;
  return power.value() / (squaredLength.value() * (d + circle.radius));
}

Intersection circleOnImplicitLine(const ExactVec& direction, const Expansion& lineIntercept,
                                  const Geometry& circle, double eps, bool single = false) {
  const auto coordinates = [&](bool xAxis) {
    const auto axis = [&](Vec p) { return xAxis ? p.x : p.y; };
    const auto other = [&](Vec p) { return xAxis ? p.y : p.x; };
    Expansion u, v;
    u.add(axis(direction.high));
    u.add(axis(direction.low));
    v.add(other(direction.high));
    v.add(other(direction.low));
    Expansion offset;
    offset.addScaled(lineIntercept, xAxis ? 1 : -1);
    offset.addScaled(u, -other(circle.p));
    Expansion uu, a, b, c;
    uu.addProduct(u, u, 1);
    a.addProduct(u, u, 1);
    a.addProduct(v, v, 1);
    b.addProduct(v, offset, 1);
    b.addScaled(uu, -axis(circle.p));
    c.addProduct(offset, offset, 1);
    c.addScaled(uu, axis(circle.p) * axis(circle.p));
    c.addScaled(uu, -circle.radius * circle.radius);
    Expansion discriminant;
    discriminant.addProduct(b, b, 1);
    discriminant.addProduct(a, c, -1);
    const double root = std::sqrt(std::max(0.0, discriminant.value()));
    const double q = -b.value() - std::copysign(root, b.value());
    std::array<double, 2> result{q / a.value(), q != 0 ? c.value() / q : -b.value() / a.value()};
    if (single || discriminant.value() <= 0)
      result.fill(-b.value() / a.value());
    std::sort(result.begin(), result.end());
    return result;
  };
  const auto x = coordinates(true);
  auto y = coordinates(false);
  if (std::signbit(direction.high.x) != std::signbit(direction.high.y))
    std::reverse(y.begin(), y.end());
  Intersection result;
  for (std::size_t i = 0; i < x.size(); ++i) {
    const Vec point{x[i], y[i]};
    if (result.points.empty() || length(point - result.points.front()) > eps)
      result.points.push_back(point);
  }
  return result;
}

Intersection lineCircle(const Geometry& line, const Geometry& circle, double eps) {
  if (perpendicularGap(line, circle) > eps)
    return {};
  const Expansion intercept = crossExpansion(line.rawDirection, ExactVec{line.p, {}});
  Intersection result = circleOnImplicitLine(line.rawDirection, intercept, circle, eps);
  result.points.erase(std::remove_if(result.points.begin(), result.points.end(),
                                     [&](Vec point) { return !inSegment(point, line, eps); }),
                      result.points.end());
  return result;
}

struct CircleRelation {
  double outsideGap = 0, insideGap = 0, height = 0;
};

CircleRelation circleRelation(const Geometry& a, const Geometry& b) {
  const ExactVec delta = exactDifference(b.p, a.p);
  const double d = length(b.p - a.p);
  const double big = std::max(a.radius, b.radius), small = std::min(a.radius, b.radius);
  Expansion outside, inside;
  addDot(outside, delta, delta, 1);
  outside.addProduct(big, big, -1);
  outside.addProduct(big, small, -2);
  outside.addProduct(small, small, -1);
  addDot(inside, delta, delta, -1);
  inside.addProduct(big, big, 1);
  inside.addProduct(big, small, -2);
  inside.addProduct(small, small, 1);
  const double outerPower = outside.value(), innerPower = inside.value();
  const double innerDenominator = big - small + d;
  return {outerPower / (d + big + small), innerDenominator > 0 ? innerPower / innerDenominator : 0,
          d > 0 ? std::sqrt(std::max(0.0, (-outerPower / (2 * d)) * (-innerPower / (2 * d)))) : 0};
}

double endpointCircleGap(Vec point, const Geometry& circle) {
  Geometry endpoint;
  endpoint.p = point;
  return circleRelation(endpoint, circle).outsideGap;
}

double lineCircleGap(const Geometry& line, const Geometry& circle) {
  if (line.segment && endpointProjection(circle.p, line.p, line) < 0)
    return endpointCircleGap(line.p, circle);
  if (line.segment && endpointProjection(circle.p, line.end, line) > 0)
    return endpointCircleGap(line.end, circle);
  return perpendicularGap(line, circle);
}

Intersection circleCircle(Geometry a, Geometry b, double eps) {
  if (a.radius > b.radius ||
      (a.radius == b.radius && (a.p.x > b.p.x || (a.p.x == b.p.x && a.p.y > b.p.y))))
    std::swap(a, b);
  const ExactVec delta = exactDifference(b.p, a.p);
  const double d = length(b.p - a.p);
  if (d <= eps)
    return {{}, std::abs(a.radius - b.radius) <= eps};
  const CircleRelation relation = circleRelation(a, b);
  if (relation.outsideGap > eps || relation.insideGap > eps)
    return {};
  Expansion intercept;
  addDot(intercept, ExactVec{b.p, {}}, ExactVec{b.p, {}}, 0.5);
  addDot(intercept, ExactVec{a.p, {}}, ExactVec{a.p, {}}, -0.5);
  intercept.addProduct(a.radius, a.radius, 0.5);
  intercept.addProduct(b.radius, b.radius, -0.5);
  const ExactVec direction{{delta.high.y, -delta.high.x}, {delta.low.y, -delta.low.x}};
  return circleOnImplicitLine(direction, intercept, a, eps, relation.height <= eps);
}

Intersection intersect(Geometry a, Geometry b, double eps) {
  if (a.kind > b.kind)
    std::swap(a, b);
  if (a.kind == Geometry::Point) {
    bool contains = false;
    if (b.kind == Geometry::Point)
      contains = length(a.p - b.p) <= eps;
    else if (b.kind == Geometry::Line)
      contains = onLine(a.p, b, eps);
    else
      contains = std::abs(circleRelation(a, b).outsideGap) <= eps;
    return contains ? Intersection{{a.p}, false} : Intersection{};
  }
  if (a.kind == Geometry::Line)
    return b.kind == Geometry::Line ? lineLine(a, b, eps) : lineCircle(a, b, eps);
  return circleCircle(a, b, eps);
}

double distance(Geometry a, Geometry b) {
  if (a.kind > b.kind)
    std::swap(a, b);
  if (a.kind == Geometry::Point) {
    if (b.kind == Geometry::Point)
      return length(a.p - b.p);
    if (b.kind == Geometry::Line)
      return pointLineDistance(a.p, b);
    return std::abs(circleRelation(a, b).outsideGap);
  }
  if (a.kind == Geometry::Circle) {
    const CircleRelation relation = circleRelation(a, b);
    return std::max({0.0, relation.outsideGap, relation.insideGap});
  }
  if (b.kind == Geometry::Circle) {
    const double minGap = lineCircleGap(a, b);
    if (!a.segment)
      return std::max(0.0, minGap);
    const double maxGap = std::max(endpointCircleGap(a.p, b), endpointCircleGap(a.end, b));
    return std::max({0.0, minGap, -maxGap});
  }
  const LineParameters parameters = lineParameters(a, b);
  if (!parameters.parallel) {
    if (!a.segment && !b.segment)
      return 0.0;
    if (inSegment(parameters.a, a, 0) && inSegment(parameters.b, b, 0))
      return 0.0;
  } else if (!a.segment && !b.segment) {
    return pointLineDistance(a.p, b);
  }
  double result = std::numeric_limits<double>::infinity();
  if (a.segment) {
    result = std::min(pointLineDistance(a.p, b), pointLineDistance(a.end, b));
  }
  if (b.segment) {
    result = std::min({result, pointLineDistance(b.p, a), pointLineDistance(b.end, a)});
  }
  return result;
}

double clearance(Geometry a, Geometry b) {
  if (a.kind > b.kind)
    std::swap(a, b);
  if (b.kind == Geometry::Line)
    return pointLineDistance(a.p, b);
  if (a.kind == Geometry::Line)
    return lineCircleGap(a, b);
  if (a.kind == Geometry::Circle || b.kind == Geometry::Circle)
    return circleRelation(a, b).outsideGap;
  return length(a.p - b.p) - a.radius - b.radius;
}

Status scalar(const Inputs& in, const char* port, double fallback, const std::string& expectedUnit,
              double& value) {
  value = fallback;
  if (!in.has(port))
    return Status::Ok();
  const Record* record = nullptr;
  std::string why;
  if (!values::readValue(in.get(port), record, why))
    return bad(why, port);
  if (record->data["kind"] != "number" || record->data["unit"] != expectedUnit)
    return bad("数值输入的 kind 必须为 number，单位必须为 " + expectedUnit, port);
  value = record->data["value"].get<double>();
  return Status::Ok();
}

Port scalarInput(const char* name, const char* doc) {
  return withContract(Port{name, "Record", name, doc, false}, {{"recordType", values::kValueType}});
}

Status coordinates(const Inputs& in, const ParamView& p, const char* parameter, Vec& out) {
  const auto fallback = p.vec2(parameter);
  const std::string u = p.choice("unit");
  Status s = scalar(in, "x", fallback[0], u, out.x);
  if (!s.ok)
    return s;
  s = scalar(in, "y", fallback[1], u, out.y);
  if (!s.ok)
    return s;
  return representable(out) ? Status::Ok() : bad("坐标超出有限 Point2D 范围");
}

Param number(const char* name, const char* label, double def) {
  Param p;
  p.name = name;
  p.label = label;
  p.type = ParamType::Float;
  p.def = Value::number(def);
  return p;
}
Param vector2(const char* name, const char* label, float x = 0, float y = 0) {
  Param p;
  p.name = name;
  p.label = label;
  p.type = ParamType::Vec2f;
  p.def = Value::vec({x, y});
  return p;
}
Param choice(const char* name, const char* label, const char* def,
             std::vector<EnumOption> options) {
  Param p;
  p.name = name;
  p.label = label;
  p.type = ParamType::Enum;
  p.def = Value::text(def);
  p.options = std::move(options);
  return p;
}
Param unitParam() { return choice("unit", "坐标单位", "m", {{"m", "米"}, {"px", "像素"}}); }
Unit2D unit(const ParamView& p) { return p.choice("unit") == "px" ? Unit2D::Pixel : Unit2D::Meter; }
Port geometryInput(const char* name, int group) {
  Port p{name, "Any", name, "Point2D、Line2D 或 Circle2D；两个端口允许不同几何类型。", true};
  p.anyGroup = group;
  return p;
}
OperatorDesc descriptor(const char* id, const char* label, const char* english) {
  OperatorDesc op;
  op.id = id;
  op.label = label;
  op.version = "1.0.0";
  op.category = "几何/基础";
  op.keywords = {english, label, "geometry", "几何", "2d"};
  op.capabilities = {false, true, true};
  return op;
}

void registerConstructors(Registry& r) {
  OperatorDesc op = descriptor("geometry.point_2d", "二维点构造", "construct 2D point");
  op.inputs = {scalarInput("x", "可选 x 坐标，单位必须与 unit 一致；覆盖参数。"),
               scalarInput("y", "可选 y 坐标，单位必须与 unit 一致；覆盖参数。")};
  op.params = {vector2("position", "坐标"), unitParam()};
  op.outputs = {Port{"point", "Point2D", "点", "显式单位的二维点。", true}};
  op.compute = [](const Inputs& in, const ParamView& p, Outputs& out, ExecContext&) {
    Vec v;
    const Status s = coordinates(in, p, "position", v);
    if (!s.ok)
      return s;
    Point2D point;
    point.p[0] = static_cast<float>(v.x);
    point.p[1] = static_cast<float>(v.y);
    point.unit = unit(p);
    out.set("point", Data::point2d(point));
    return Status::Ok();
  };
  r.addOperator(std::move(op));

  op = descriptor("geometry.line_2d", "二维直线与线段构造", "construct 2D line segment");
  op.inputs = {scalarInput("x", "经过点 x；线段模式为起点 x，单位与 unit 一致。"),
               scalarInput("y", "经过点 y；线段模式为起点 y，单位与 unit 一致。"),
               scalarInput("angle", "可选直线角度，单位必须为 deg 或 rad；覆盖方向参数。")};
  op.params = {
      choice("mode", "类型", "line", {{"line", "无限直线"}, {"segment", "线段"}}),
      vector2("position", "直线经过点"),
      vector2("direction", "直线方向", 1, 0),
      choice("representation", "方向表示", "direction", {{"direction", "向量"}, {"angle", "角度"}}),
      number("angle", "方向角", 0),
      choice("angleUnit", "角度单位", "deg", {{"deg", "度"}, {"rad", "弧度"}}),
      vector2("start", "线段起点"),
      vector2("end", "线段终点", 1, 0),
      unitParam()};
  op.doc =
      "无限线使用经过点和向量/角度，线段使用端点。角度输入显式 deg/rad 并覆盖方向参数；线段不接受角度输入。方向归一化，拒绝零方向与重合端点。";
  op.outputs = {Port{"line", "Line2D", "直线或线段", "线段以 start/end 为权威端点。", true}};
  op.compute = [](const Inputs& in, const ParamView& p, Outputs& out, ExecContext&) {
    const bool segment = p.choice("mode") == "segment";
    Vec position;
    Status s = coordinates(in, p, segment ? "start" : "position", position);
    if (!s.ok)
      return s;
    position = {static_cast<float>(position.x), static_cast<float>(position.y)};
    const auto v = p.vec2(segment ? "end" : "direction");
    const Vec endOrDir{v[0], v[1]};
    Vec d = segment ? endOrDir - position : endOrDir;
    if (segment && in.has("angle"))
      return bad("线段模式不接受 angle，使用端点定义方向", "angle");
    if (!segment && (in.has("angle") || p.choice("representation") == "angle")) {
      double angle = p.number("angle");
      std::string u = p.choice("angleUnit");
      if (in.has("angle")) {
        const Record* record = nullptr;
        std::string why;
        if (!values::readValue(in.get("angle"), record, why))
          return bad(why, "angle");
        if (record->data["kind"] != "number")
          return bad("角度必须为数值", "angle");
        u = record->data["unit"].get<std::string>();
        angle = record->data["value"].get<double>();
      }
      if ((u != "deg" && u != "rad") || !std::isfinite(angle))
        return bad("角度单位必须为 deg 或 rad，数值必须有限", "angle");
      constexpr double pi = 3.14159265358979323846;
      angle =
          u == "deg" ? std::remainder(angle, 360.0) * pi / 180.0 : std::remainder(angle, 2 * pi);
      d = {std::cos(angle), std::sin(angle)};
    }
    if (!finite(position) || !finite(endOrDir) || !(length(d) > 0))
      return badParam("坐标必须有限且方向非零；线段端点不能重合", segment ? "end" : "direction");
    const Vec dir = d * (1.0 / length(d));
    Line2D line;
    line.point[0] = static_cast<float>(position.x);
    line.point[1] = static_cast<float>(position.y);
    line.dir[0] = static_cast<float>(dir.x);
    line.dir[1] = static_cast<float>(dir.y);
    line.hasSegment = segment;
    line.unit = unit(p);
    if (segment) {
      line.start[0] = line.point[0];
      line.start[1] = line.point[1];
      std::copy(v.begin(), v.end(), line.end);
    }
    out.set("line", Data::line2d(line));
    return Status::Ok();
  };
  r.addOperator(std::move(op));

  op = descriptor("geometry.circle_2d", "二维圆构造", "construct 2D circle");
  op.inputs = {scalarInput("x", "可选圆心 x，单位必须与 unit 一致。"),
               scalarInput("y", "可选圆心 y，单位必须与 unit 一致。"),
               scalarInput("radius", "可选半径，单位必须与 unit 一致。")};
  op.params = {vector2("center", "圆心"), number("radius", "半径", 1), unitParam()};
  op.outputs = {Port{"circle", "Circle2D", "圆", "半径为正的圆边界。", true}};
  op.compute = [](const Inputs& in, const ParamView& p, Outputs& out, ExecContext&) {
    Vec center;
    Status s = coordinates(in, p, "center", center);
    if (!s.ok)
      return s;
    double radius = 0;
    s = scalar(in, "radius", p.number("radius"), p.choice("unit"), radius);
    if (!s.ok)
      return s;
    if (!std::isfinite(radius) || radius <= 0 || radius > std::numeric_limits<float>::max())
      return badParam("半径必须为可表示的有限正数", "radius");
    Circle2D circle;
    circle.center[0] = static_cast<float>(center.x);
    circle.center[1] = static_cast<float>(center.y);
    circle.radius = static_cast<float>(radius);
    if (!(circle.radius > 0))
      return badParam("半径小于可表示范围", "radius");
    circle.unit = unit(p);
    out.set("circle", Data::circle2d(circle));
    return Status::Ok();
  };
  r.addOperator(std::move(op));

  op = descriptor("geometry.plane", "三维平面构造", "construct 3D plane normal offset");
  Param normal;
  normal.name = "normal";
  normal.label = "法向量";
  normal.type = ParamType::Vec3f;
  normal.def = Value::vec({0, 0, 1});
  Param offset = number("offset", "平面系数 d", 0);
  offset.unit = "m";
  op.params = {normal, offset};
  op.doc = "输入平面方程 n·p+d=0，坐标为米；法向和 d 同时除以 |n|，不改变原平面。";
  op.outputs = {Port{"plane", "Plane", "平面", "单位法向与米制偏移。", true}};
  op.compute = [](const Inputs&, const ParamView& p, Outputs& out, ExecContext&) {
    const auto n = p.vec3("normal");
    const double norm =
        std::hypot(static_cast<double>(n[0]), static_cast<double>(n[1]), static_cast<double>(n[2]));
    if (!std::isfinite(norm) || norm <= 0)
      return badParam("平面法向必须有限且非零", "normal");
    const double d = p.number("offset") / norm;
    if (!std::isfinite(d) || std::abs(d) > std::numeric_limits<float>::max())
      return badParam("归一化后的平面偏移超出范围", "offset");
    Plane plane;
    for (int i = 0; i < 3; ++i)
      plane.normal[i] = static_cast<float>(n[i] / norm);
    plane.d = static_cast<float>(d);
    out.set("plane", Data::plane(plane));
    return Status::Ok();
  };
  r.addOperator(std::move(op));
}

} // namespace

void registerPackOps(Registry& r) {
  registerPoseOps(r);
  registerGeometryTransforms(r);
  registerConstructors(r);
  OperatorDesc op = descriptor("geometry.distance_2d", "二维几何距离", "2D geometry distance");
  op.inputs = {geometryInput("a", 1), geometryInput("b", 2)};
  Param firstCenter;
  firstCenter.name = "useFirstCenter";
  firstCenter.label = "圆 A 使用圆心";
  firstCenter.type = ParamType::Bool;
  firstCenter.def = Value::boolean(false);
  Param secondCenter = firstCenter;
  secondCenter.name = "useSecondCenter";
  secondCenter.label = "圆 B 使用圆心";
  op.params = {choice("mode", "距离定义", "minimum",
                      {{"minimum", "无向最小边界距离"},
                       {"signed_clearance", "有符号间隙"},
                       {"first_anchor", "直线 A 参考点到 B"}}),
               firstCenter, secondCenter};
  op.outputs = {Port{"distance", "Measurement", "距离", "单位继承输入，符号由 mode 定义。", true}};
  op.doc =
      "minimum 为最小圆周/线段边界距离，非负；signed_clearance 对圆间为中心距减半径和、圆线为中心到线距离减半径，重叠可为负，不接受两条线；first_anchor 仅两条线，计算 A 参考点到 B 的距离，可不对称。圆心开关将圆显式视为点。";
  op.compute = [](const Inputs& in, const ParamView& p, Outputs& out, ExecContext&) {
    Geometry a, b;
    const Status s = pair(in, a, b);
    if (!s.ok)
      return s;
    if (p.flag("useFirstCenter") && a.kind == Geometry::Circle) {
      a.kind = Geometry::Point;
      a.radius = 0;
    }
    if (p.flag("useSecondCenter") && b.kind == Geometry::Circle) {
      b.kind = Geometry::Point;
      b.radius = 0;
    }
    const std::string mode = p.choice("mode");
    if ((mode == "first_anchor" && (a.kind != Geometry::Line || b.kind != Geometry::Line)) ||
        (mode == "signed_clearance" && a.kind == Geometry::Line && b.kind == Geometry::Line))
      return badParam("此距离定义不支持输入的几何组合", "mode");
    const double d = mode == "first_anchor"       ? pointLineDistance(a.p, b)
                     : mode == "signed_clearance" ? clearance(a, b)
                                                  : distance(a, b);
    if (!std::isfinite(d))
      return bad("距离计算超出可表示范围");
    Measurement result;
    result.value = d;
    result.ok = true;
    result.unit = a.unit == Unit2D::Pixel ? "px" : "m";
    out.set("distance", Data::measurement(std::move(result)));
    return Status::Ok();
  };
  r.addOperator(std::move(op));

  op = descriptor("geometry.intersect_2d", "二维几何求交", "2D geometry intersection");
  op.inputs = {geometryInput("a", 1), geometryInput("b", 2)};
  op.params = {number("tolerance", "坐标容差", 1e-6)};
  op.doc =
      "容差使用输入坐标单位。无交点为有效空数组，重合或线段连续重叠为 coincident（不伪造单点）；有限交点按 x/y 排序。";
  op.outputs = {withExample(
      withContract(Port{"intersections", "Record", "交点集", "geometry.Intersections2D.v1", true},
                   {{"recordType", "geometry.Intersections2D.v1"}}),
      {{"unit", "m"}, {"relation", "finite"}, {"points", {{0.0, 0.0}}}})};
  op.compute = [](const Inputs& in, const ParamView& p, Outputs& out, ExecContext&) {
    Geometry a, b;
    const Status s = pair(in, a, b);
    if (!s.ok)
      return s;
    const double eps = p.number("tolerance");
    if (!std::isfinite(eps) || eps <= 0)
      return badParam("坐标容差必须有限且为正", "tolerance");
    Intersection result = intersect(a, b, eps);
    std::sort(result.points.begin(), result.points.end(),
              [](Vec u, Vec v) { return u.x < v.x || (u.x == v.x && u.y < v.y); });
    Record record;
    record.type = "geometry.Intersections2D.v1";
    record.data = {{"unit", a.unit == Unit2D::Pixel ? "px" : "m"},
                   {"relation", result.coincident       ? "coincident"
                                : result.points.empty() ? "empty"
                                                        : "finite"},
                   {"points", nlohmann::json::array()}};
    for (Vec point : result.points) {
      if (!representable(point))
        return bad("交点超出 Point2D 可表示范围");
      record.data["points"].push_back({point.x, point.y});
    }
    out.set("intersections", Data::record(std::move(record)));
    return Status::Ok();
  };
  r.addOperator(std::move(op));

  op = descriptor("geometry.midpoint_2d", "二维点中点", "2D midpoint");
  op.inputs = {Port{"a", "Point2D", "点 A", "", true}, Port{"b", "Point2D", "点 B", "", true}};
  op.outputs = {Port{"point", "Point2D", "中点", "同单位两个点的中点。", true}};
  op.compute = [](const Inputs& in, const ParamView&, Outputs& out, ExecContext&) {
    Geometry a, b;
    const Status s = pair(in, a, b);
    if (!s.ok)
      return s;
    if (a.kind != Geometry::Point || b.kind != Geometry::Point)
      return bad("中点只接受两个 Point2D");
    const Vec mid = (a.p + b.p) * 0.5;
    Point2D point;
    point.p[0] = static_cast<float>(mid.x);
    point.p[1] = static_cast<float>(mid.y);
    point.unit = a.unit;
    out.set("point", Data::point2d(point));
    return Status::Ok();
  };
  r.addOperator(std::move(op));

  op = descriptor("geometry.project_point_2d", "二维点正交投影",
                  "2D point orthogonal projection foot");
  op.inputs = {Port{"point", "Point2D", "点", "待投影的点。", true},
               Port{"line", "Line2D", "直线或线段", "目标直线或线段。", true}};
  Param clamp;
  clamp.name = "clampToSegment";
  clamp.label = "限制在线段内";
  clamp.type = ParamType::Bool;
  clamp.def = Value::boolean(true);
  op.params = {clamp};
  op.outputs = {Port{"point", "Point2D", "投影点", "无限线垂足或线段上的最近点。", true}};
  op.doc =
      "无限线输出正交垂足；有限线段默认将投影限制在端点之间，关闭 clampToSegment 则投影到延长线。";
  op.compute = [](const Inputs& in, const ParamView& p, Outputs& out, ExecContext&) {
    Geometry point, line;
    Status s = read(in.get("point"), "point", point);
    if (!s.ok)
      return s;
    s = read(in.get("line"), "line", line);
    if (!s.ok)
      return s;
    if (point.kind != Geometry::Point || line.kind != Geometry::Line)
      return bad("需要一个 Point2D 和一个 Line2D");
    if (point.unit != line.unit)
      return bad("点和直线的单位必须一致", "line");
    const Vec projection = projectedPoint(point.p, line, p.flag("clampToSegment"));
    if (!representable(projection))
      return bad("投影点超出 Point2D 范围");
    Point2D result;
    result.p[0] = static_cast<float>(projection.x);
    result.p[1] = static_cast<float>(projection.y);
    result.unit = point.unit;
    out.set("point", Data::point2d(result));
    return Status::Ok();
  };
  r.addOperator(std::move(op));

  op = descriptor("geometry.point_at", "交点索引提取", "intersection point by index");
  op.inputs = {withContract(Port{"intersections", "Record", "交点集", "", true},
                            {{"recordType", "geometry.Intersections2D.v1"}})};
  Param index;
  index.name = "index";
  index.label = "索引";
  index.type = ParamType::Int;
  index.def = Value::integer(0);
  index.min = 0;
  op.params = {index};
  op.outputs = {Port{"point", "Point2D", "点", "提取有限交点；空或重合不产生伪点。", true}};
  op.compute = [](const Inputs& in, const ParamView& p, Outputs& out, ExecContext&) {
    const Record* r = in.get("intersections").asRecord();
    if (!r || r->type != "geometry.Intersections2D.v1" || !r->data.is_object())
      return bad("需要 geometry.Intersections2D.v1", "intersections");
    const auto& data = r->data;
    if (!data.contains("unit") || !data["unit"].is_string() ||
        (data["unit"] != "m" && data["unit"] != "px") || !data.contains("relation") ||
        !data["relation"].is_string() || !data.contains("points") || !data["points"].is_array() ||
        data["points"].size() > 2)
      return bad("交点集结构或单位非法", "intersections");
    const std::string relation = data["relation"].get<std::string>();
    const auto& points = data["points"];
    if ((relation != "finite" && relation != "empty" && relation != "coincident") ||
        ((relation == "finite") == points.empty()))
      return bad("交点集状态和点数不一致", "intersections");
    for (const auto& xy : points) {
      if (!xy.is_array() || xy.size() != 2 || !xy[0].is_number() || !xy[1].is_number() ||
          !representable({xy[0].get<double>(), xy[1].get<double>()}))
        return bad("交点坐标必须为有限二维数组", "intersections");
    }
    const auto i = p.integer("index");
    if (i < 0 || static_cast<std::uint64_t>(i) >= points.size())
      return Status::Error(Phase::Execute, "index_out_of_range", "没有该索引的有限交点", "index");
    Point2D point;
    point.p[0] = points[static_cast<std::size_t>(i)][0].get<float>();
    point.p[1] = points[static_cast<std::size_t>(i)][1].get<float>();
    point.unit = data["unit"] == "px" ? Unit2D::Pixel : Unit2D::Meter;
    out.set("point", Data::point2d(point));
    return Status::Ok();
  };
  r.addOperator(std::move(op));
}

} // namespace lyflow::packs::std_geometry
