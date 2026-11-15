#include "lyflow/kernel/geometry.h"
#include "lyflow/kernel/rigid.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
namespace lyflow::kernel {
double length(Vec2 v) { return std::hypot(v.x, v.y); }
bool finite(Vec2 v) { return std::isfinite(v.x) && std::isfinite(v.y); }
bool representable(Vec2 v) {
  const double max = std::numeric_limits<float>::max();
  return finite(v) && std::abs(v.x) <= max && std::abs(v.y) <= max;
}
namespace {
using Vec = Vec2;
bool fail(std::string& why, const char* message) {
  why = message;
  return false;
}
Vec vec(const float* p) { return {p[0], p[1]}; }

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

bool read(const Geometry2D& data, Geometry& out, std::string& why) {
  if (const Point2D* p = std::get_if<Point2D>(&data)) {
    out.kind = Geometry::Point;
    out.p = vec(p->p);
    out.unit = p->unit;
  } else if (const Line2D* l = std::get_if<Line2D>(&data)) {
    out.kind = Geometry::Line;
    out.unit = l->unit;
    out.segment = l->hasSegment;
    out.p = vec(l->point);
    out.d = vec(l->dir);
    if (!finite(out.d) || !(length(out.d) > 0))
      return fail(why, "直线方向必须有限且非零");
    if (l->hasSegment) {
      out.p = vec(l->start);
      const Vec end = vec(l->end);
      if (!finite(end))
        return fail(why, "线段端点必须有限");
      out.end = end;
      out.rawDirection = exactDifference(end, out.p);
      out.d = out.rawDirection.high;
      if (!(length(out.d) > 0))
        return fail(why, "线段两个端点不能重合");
    } else {
      out.rawDirection = {out.d, {}};
    }
    out.directionLength = length(out.d);
    out.d = out.d * (1.0 / out.directionLength);
  } else if (const Circle2D* c = std::get_if<Circle2D>(&data)) {
    out.kind = Geometry::Circle;
    out.p = vec(c->center);
    out.radius = c->radius;
    out.unit = c->unit;
    if (!std::isfinite(out.radius) || !(out.radius > 0))
      return fail(why, "圆半径必须有限且为正");
  } else {
    return fail(why, "只接受 Point2D、Line2D 或 Circle2D");
  }
  if (!finite(out.p) || (out.kind == Geometry::Line && !finite(out.d)))
    return fail(why, "几何坐标必须有限");
  if (out.unit != Unit2D::Meter && out.unit != Unit2D::Pixel)
    return fail(why, "未知二维几何单位");
  return true;
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

bool pair(const Geometry2D& first, const Geometry2D& second, Geometry& a, Geometry& b,
          GeometryIssue& issue) {
  issue = {};
  if (!read(first, a, issue.message)) {
    issue.operand = 0;
    return false;
  }
  if (!read(second, b, issue.message)) {
    issue.operand = 1;
    return false;
  }
  if (a.unit != b.unit) {
    issue.message = "两个几何体的单位必须一致，不自动换算";
    issue.operand = 1;
    return false;
  }
  return true;
}
}
Unit2D geometryUnit(const Geometry2D& geometry) {
  return std::visit([](const auto& value) { return value.unit; }, geometry);
}
bool validateGeometry2D(const Geometry2D& geometry, std::string& why) {
  Geometry parsed;
  why.clear();
  return read(geometry, parsed, why);
}
bool distance2D(const Geometry2D& first, const Geometry2D& second, const DistanceOptions& options,
                double& out, GeometryIssue& issue) {
  Geometry a, b;
  if (!pair(first, second, a, b, issue))
    return false;
  if (options.mode != DistanceMode::Minimum && options.mode != DistanceMode::SignedClearance &&
      options.mode != DistanceMode::FirstAnchor) {
    issue.message = "未知距离定义";
    issue.parameter = true;
    return false;
  }
  if (options.useFirstCenter && a.kind == Geometry::Circle) {
    a.kind = Geometry::Point;
    a.radius = 0;
  }
  if (options.useSecondCenter && b.kind == Geometry::Circle) {
    b.kind = Geometry::Point;
    b.radius = 0;
  }
  if ((options.mode == DistanceMode::FirstAnchor &&
       (a.kind != Geometry::Line || b.kind != Geometry::Line)) ||
      (options.mode == DistanceMode::SignedClearance && a.kind == Geometry::Line &&
       b.kind == Geometry::Line)) {
    issue.message = "此距离定义不支持输入的几何组合";
    issue.parameter = true;
    return false;
  }
  const double result = options.mode == DistanceMode::FirstAnchor       ? pointLineDistance(a.p, b)
                        : options.mode == DistanceMode::SignedClearance ? clearance(a, b)
                                                                        : distance(a, b);
  if (!std::isfinite(result)) {
    issue.message = "距离计算超出可表示范围";
    return false;
  }
  out = result;
  return true;
}
bool intersect2D(const Geometry2D& first, const Geometry2D& second, double tolerance,
                 Intersections2D& out, GeometryIssue& issue) {
  Geometry a, b;
  if (!pair(first, second, a, b, issue))
    return false;
  if (!std::isfinite(tolerance) || tolerance <= 0) {
    issue.message = "坐标容差必须有限且为正";
    issue.parameter = true;
    return false;
  }
  auto result = intersect(a, b, tolerance);
  std::sort(result.points.begin(), result.points.end(),
            [](Vec u, Vec v) { return u.x < v.x || (u.x == v.x && u.y < v.y); });
  for (const auto point : result.points)
    if (!representable(point)) {
      issue.message = "交点超出 Point2D 可表示范围";
      return false;
    }
  out = {std::move(result.points), result.coincident};
  return true;
}
bool projectPoint2D(const Point2D& point, const Line2D& line, bool clamp, Point2D& out,
                    GeometryIssue& issue) {
  Geometry a, b;
  if (!pair(point, line, a, b, issue)) {
    if (a.unit != b.unit && issue.operand == 1 &&
        issue.message == "两个几何体的单位必须一致，不自动换算")
      issue.message = "点和直线的单位必须一致";
    return false;
  }
  const auto value = projectedPoint(a.p, b, clamp);
  if (!representable(value)) {
    issue.message = "投影点超出 Point2D 范围";
    return false;
  }
  Point2D result;
  result.p[0] = static_cast<float>(value.x);
  result.p[1] = static_cast<float>(value.y);
  result.unit = a.unit;
  out = result;
  return true;
}
bool midpoint2D(const Point2D& first, const Point2D& second, Point2D& out, GeometryIssue& issue) {
  Geometry a, b;
  if (!pair(first, second, a, b, issue))
    return false;
  const Vec mid = (a.p + b.p) * 0.5;
  Point2D result;
  result.p[0] = static_cast<float>(mid.x);
  result.p[1] = static_cast<float>(mid.y);
  result.unit = a.unit;
  out = result;
  return true;
}
namespace {
bool transformIssue(GeometryIssue& issue, const std::string& message, int operand,
                    bool parameter = false) {
  issue.message = message;
  issue.operand = operand;
  issue.parameter = parameter;
  return false;
}
}
bool transformGeometry2D(const Geometry2D& geometry, const Transform& transform, Unit2D expected,
                         Geometry2D& out, GeometryIssue& issue) {
  issue = {};
  if (expected != Unit2D::Meter && expected != Unit2D::Pixel)
    return transformIssue(issue, "未知二维几何单位", -1, true);
  M4 matrix{};
  double scale = 1;
  std::string why;
  if (!similarity2D(&transform, matrix, scale, false, why))
    return transformIssue(issue, why, 1);
  const auto map = [&](const float* xy, float* target, bool direction = false) {
    const V3 value = kernel::apply(matrix, {xy[0], xy[1], 0}, direction);
    if (!nativeFloat(value[0]) || !nativeFloat(value[1]))
      return false;
    target[0] = static_cast<float>(value[0]);
    target[1] = static_cast<float>(value[1]);
    return true;
  };
  if (const auto* point = std::get_if<Point2D>(&geometry)) {
    if (point->unit != expected)
      return transformIssue(issue, "几何单位必须与矩阵平移单位一致", -1, true);
    Point2D result = *point;
    if (!map(point->p, result.p))
      return transformIssue(issue, "点坐标超出原生 float32 范围", 0);
    out = result;
  } else if (const auto* line = std::get_if<Line2D>(&geometry)) {
    if (line->unit != expected)
      return transformIssue(issue, "几何单位必须与矩阵平移单位一致", -1, true);
    Line2D result = *line;
    if (!map(line->point, result.point))
      return transformIssue(issue, "直线坐标超出 float32 范围", 0);
    V3 direction{};
    if (line->hasSegment) {
      if (!map(line->start, result.start) || !map(line->end, result.end))
        return transformIssue(issue, "线段端点超出 float32 范围", 0);
      direction = {double(result.end[0]) - result.start[0], double(result.end[1]) - result.start[1],
                   0};
      result.point[0] = result.start[0];
      result.point[1] = result.start[1];
    } else {
      direction = kernel::apply(matrix, {line->dir[0], line->dir[1], 0}, true);
    }
    if (!normalize(direction))
      return transformIssue(issue, "直线方向非法或变换后线段端点在float32精度下重合", 0);
    result.dir[0] = static_cast<float>(direction[0]);
    result.dir[1] = static_cast<float>(direction[1]);
    out = result;
  } else if (const auto* circle = std::get_if<Circle2D>(&geometry)) {
    if (circle->unit != expected)
      return transformIssue(issue, "几何单位必须与矩阵平移单位一致", -1, true);
    const double radius = double(circle->radius) * scale;
    Circle2D result = *circle;
    if (!map(circle->center, result.center) || !nativeFloat(radius) || radius <= 0 ||
        static_cast<float>(radius) == 0)
      return transformIssue(issue, "圆心或半径非法，或超出float32可表示范围", 0);
    result.radius = static_cast<float>(radius);
    out = result;
  } else {
    return transformIssue(issue, "仅支持 Point2D、Line2D 和 Circle2D", 0);
  }
  return true;
}

bool transformPlane(const Plane& source, const Transform& transform, Plane& out,
                    GeometryIssue& issue) {
  issue = {};
  M4 m{};
  std::string why;
  if (!affine(&transform, m, why))
    return transformIssue(issue, why, 1);
  const Plane* plane = &source;
  if (!plane || !std::isfinite(plane->d))
    return transformIssue(issue, "需要有效平面", 0);
  V3 normal{plane->normal[0], plane->normal[1], plane->normal[2]};
  const double oldNorm = std::hypot(normal[0], normal[1], normal[2]);
  if (!std::isfinite(oldNorm) || oldNorm == 0 || std::abs(oldNorm - 1) > 1e-5)
    return transformIssue(issue, "平面需要单位法向", 0);
  double scale = 0;
  for (int row = 0; row < 3; ++row)
    for (int col = 0; col < 3; ++col)
      scale = std::max(scale, std::abs(m[row * 4 + col]));
  if (scale == 0)
    return transformIssue(issue, "仿射线性部分不可逆", 1);
  poses::Matrix3 a{};
  for (int row = 0; row < 3; ++row)
    for (int col = 0; col < 3; ++col)
      a[row * 3 + col] = m[row * 4 + col] / scale;
  const double det = a[0] * (a[4] * a[8] - a[5] * a[7]) - a[1] * (a[3] * a[8] - a[5] * a[6]) +
                     a[2] * (a[3] * a[7] - a[4] * a[6]);
  if (!std::isfinite(det) || std::abs(det) <= 1e-12)
    return transformIssue(issue, "仿射线性部分奇异或接近奇异（相对行列式≤1e-12）", 1);
  const poses::Matrix3 cofactor{
      a[4] * a[8] - a[5] * a[7], a[5] * a[6] - a[3] * a[8], a[3] * a[7] - a[4] * a[6],
      a[2] * a[7] - a[1] * a[8], a[0] * a[8] - a[2] * a[6], a[1] * a[6] - a[0] * a[7],
      a[1] * a[5] - a[2] * a[4], a[2] * a[3] - a[0] * a[5], a[0] * a[4] - a[1] * a[3]};
  V3 transformed{};
  for (int i = 0; i < 3; ++i)
    transformed[i] = productSum({{cofactor[i * 3], normal[0]},
                                 {cofactor[i * 3 + 1], normal[1]},
                                 {cofactor[i * 3 + 2], normal[2]}}) /
                     (det * scale);
  const double length = std::hypot(transformed[0], transformed[1], transformed[2]);
  if (!std::isfinite(length) || length == 0)
    return transformIssue(issue, "变换后的法向不可表示", 1);
  const double d = productSum({{plane->d, 1},
                               {-transformed[0], m[3]},
                               {-transformed[1], m[7]},
                               {-transformed[2], m[11]}}) /
                   length;
  if (!nativeFloat(d))
    return transformIssue(issue, "变换后的平面系数超出float32范围", 1);
  Plane result;
  for (int i = 0; i < 3; ++i)
    result.normal[i] = static_cast<float>(transformed[i] / length);
  result.d = static_cast<float>(d);
  out = result;
  return true;
}

}
