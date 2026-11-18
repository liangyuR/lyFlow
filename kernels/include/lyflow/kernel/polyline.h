#pragma once

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace lyflow::kernel {

template <class Point> double polylineLength(const std::vector<Point>& points) {
  double total = 0;
  for (std::size_t i = 1; i < points.size(); ++i) {
    const Point delta = points[i] - points[i - 1];
    total += std::hypot(delta.x, delta.y);
  }
  return total;
}

template <class Point> Point unitTangent2D(Point delta) {
  const double length = std::hypot(delta.x, delta.y);
  return length > 1e-12 ? Point{delta.x / length, delta.y / length} : Point{1.0, 0.0};
}

template <class Point> struct PolylineView {
  const std::vector<double>* s = nullptr;
  const std::vector<Point>* points = nullptr;
  const std::vector<Point>* tangents = nullptr;
  bool valid() const {
    return s && points && tangents && !s->empty() && s->size() == points->size() &&
           s->size() == tangents->size();
  }

  void at(double distance, Point* point, Point* tangent) const {
    const auto& arc = *s;
    if (distance <= arc.front()) {
      *point = points->front();
      *tangent = tangents->front();
      return;
    }
    if (distance >= arc.back()) {
      *point = points->back();
      *tangent = tangents->back();
      return;
    }
    const auto upper = std::upper_bound(arc.begin(), arc.end(), distance);
    const std::size_t j = static_cast<std::size_t>(upper - arc.begin());
    const std::size_t i = j - 1;
    const double span = arc[j] - arc[i];
    const double fraction = span > 1e-12 ? (distance - arc[i]) / span : 0.0;
    *point = (*points)[i] + ((*points)[j] - (*points)[i]) * fraction;
    *tangent = unitTangent2D((*tangents)[i] + ((*tangents)[j] - (*tangents)[i]) * fraction);
  }
};

template <class Point>
bool resamplePolyline(const std::vector<Point>& input, double start, double end, double step,
                      std::vector<double>* s, std::vector<Point>* points,
                      std::vector<Point>* tangents) {
  if (!(step > 0) || !std::isfinite(step))
    return false;
  std::vector<Point> vertices;
  std::vector<double> arc;
  for (const Point& p : input) {
    if (!std::isfinite(p.x) || !std::isfinite(p.y))
      return false;
    const double span =
        vertices.empty() ? 0.0 : std::hypot(p.x - vertices.back().x, p.y - vertices.back().y);
    if (!vertices.empty() && span < 1e-9)
      continue;
    arc.push_back(arc.empty() ? 0.0 : arc.back() + span);
    vertices.push_back(p);
  }
  if (vertices.size() < 2 || !std::isfinite(arc.back()))
    return false;
  const double total = arc.back();
  const double last = end > 0 ? end : total;
  if (!(start >= 0) || !(last > start) || last > total + 1e-6)
    return false;

  std::vector<double> targets;
  for (double t = start; t < last - 1e-9; t += step) {
    if (t + step == t)
      return false;
    targets.push_back(t);
  }
  targets.push_back(std::min(last, total));
  std::vector<Point> sampled;
  sampled.reserve(targets.size());
  std::size_t seg = 0;
  for (double t : targets) {
    while (seg + 2 < vertices.size() && arc[seg + 1] < t)
      ++seg;
    const double span = arc[seg + 1] - arc[seg];
    const double fraction = span > 1e-12 ? std::clamp((t - arc[seg]) / span, 0.0, 1.0) : 0.0;
    sampled.push_back(vertices[seg] + (vertices[seg + 1] - vertices[seg]) * fraction);
  }
  std::vector<Point> directions;
  directions.reserve(sampled.size());
  for (std::size_t i = 0; i < sampled.size(); ++i) {
    const std::size_t a = i == 0 ? 0 : i - 1;
    const std::size_t b = i + 1 < sampled.size() ? i + 1 : i;
    const Point delta = b > a ? sampled[b] - sampled[a] : vertices[seg + 1] - vertices[seg];
    directions.push_back(unitTangent2D(delta));
  }
  *s = std::move(targets);
  *points = std::move(sampled);
  *tangents = std::move(directions);
  return true;
}

} // namespace lyflow::kernel
