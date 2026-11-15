#pragma once
#include <string>
#include <variant>
#include <vector>
#include "lyflow/data.h"
namespace lyflow::kernel {
struct Vec2 {
  double x = 0, y = 0;
  Vec2 operator+(Vec2 b) const { return {x + b.x, y + b.y}; }
  Vec2 operator-(Vec2 b) const { return {x - b.x, y - b.y}; }
  Vec2 operator*(double k) const { return {x * k, y * k}; }
};
double length(Vec2 v);
bool finite(Vec2 v);
bool representable(Vec2 v);
using Geometry2D = std::variant<Point2D, Line2D, Circle2D>;
enum class DistanceMode { Minimum, SignedClearance, FirstAnchor };
struct DistanceOptions {
  DistanceMode mode = DistanceMode::Minimum;
  bool useFirstCenter = false;
  bool useSecondCenter = false;
};
struct GeometryIssue {
  std::string message;
  int operand = -1;
  bool parameter = false;
};
struct Intersections2D {
  std::vector<Vec2> points;
  bool coincident = false;
};
Unit2D geometryUnit(const Geometry2D& geometry);
bool validateGeometry2D(const Geometry2D& geometry, std::string& why);
bool distance2D(const Geometry2D& a, const Geometry2D& b, const DistanceOptions& options,
                double& out, GeometryIssue& issue);
bool intersect2D(const Geometry2D& a, const Geometry2D& b, double tolerance, Intersections2D& out,
                 GeometryIssue& issue);
bool projectPoint2D(const Point2D& point, const Line2D& line, bool clamp, Point2D& out,
                    GeometryIssue& issue);
bool midpoint2D(const Point2D& a, const Point2D& b, Point2D& out, GeometryIssue& issue);
bool transformGeometry2D(const Geometry2D& geometry, const Transform& transform, Unit2D expected,
                         Geometry2D& out, GeometryIssue& issue);
bool transformPlane(const Plane& plane, const Transform& transform, Plane& out,
                    GeometryIssue& issue);
}
