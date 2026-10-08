#include "lyflow/overlay.h"

#include <algorithm>
#include <cmath>

namespace lyflow {
namespace {

bool finite(const Px& p) { return std::isfinite(p[0]) && std::isfinite(p[1]); }

nlohmann::json point(const Px& p) { return nlohmann::json::array({p[0], p[1]}); }

nlohmann::json item(const char* kind, const std::string& role) {
  return nlohmann::json{{"kind", kind}, {"role", role}};
}

void withLabel(nlohmann::json& j, const std::string& label) {
  if (!label.empty()) j["label"] = label;
}

nlohmann::json finitePoints(const std::vector<Px>& pts) {
  nlohmann::json out = nlohmann::json::array();
  for (const Px& p : pts) {
    if (finite(p)) out.push_back(point(p));
  }
  return out;
}

}  // namespace

Overlay2D& Overlay2D::points(const std::string& role, const std::vector<Px>& pts,
                             const std::string& label) {
  nlohmann::json kept = finitePoints(pts);
  if (kept.empty()) return *this;
  nlohmann::json j = item("points", role);
  j["points"] = std::move(kept);
  withLabel(j, label);
  items_.push_back(std::move(j));
  return *this;
}

Overlay2D& Overlay2D::polyline(const std::string& role, const std::vector<Px>& pts, bool closed,
                               const std::string& label) {
  nlohmann::json kept = finitePoints(pts);
  if (kept.size() < 2) return *this;
  nlohmann::json j = item("polyline", role);
  j["points"] = std::move(kept);
  if (closed) j["closed"] = true;
  withLabel(j, label);
  items_.push_back(std::move(j));
  return *this;
}

Overlay2D& Overlay2D::segments(const std::string& role, const std::vector<std::array<Px, 2>>& segs,
                               const std::string& label) {
  nlohmann::json kept = nlohmann::json::array();
  for (const auto& s : segs) {
    // 一段要么两端都在，要么整段不要：丢一端会让后面所有段错位
    if (!finite(s[0]) || !finite(s[1])) continue;
    kept.push_back(point(s[0]));
    kept.push_back(point(s[1]));
  }
  if (kept.empty()) return *this;
  nlohmann::json j = item("segments", role);
  j["points"] = std::move(kept);
  withLabel(j, label);
  items_.push_back(std::move(j));
  return *this;
}

Overlay2D& Overlay2D::circle(const std::string& role, Px center, double radius,
                             const std::string& label) {
  if (!finite(center) || !std::isfinite(radius) || radius < 0) return *this;
  nlohmann::json j = item("circle", role);
  j["center"] = point(center);
  j["radius"] = radius;
  withLabel(j, label);
  items_.push_back(std::move(j));
  return *this;
}

Overlay2D& Overlay2D::box(const std::string& role, Px corner0, Px corner1,
                          const std::string& label) {
  if (!finite(corner0) || !finite(corner1)) return *this;
  const Px lo{std::min(corner0[0], corner1[0]), std::min(corner0[1], corner1[1])};
  const Px hi{std::max(corner0[0], corner1[0]), std::max(corner0[1], corner1[1])};
  nlohmann::json j = item("box", role);
  j["min"] = point(lo);
  j["max"] = point(hi);
  withLabel(j, label);
  items_.push_back(std::move(j));
  return *this;
}

Overlay2D& Overlay2D::text(const std::string& role, const std::string& text, Px at) {
  if (!finite(at)) return *this;
  nlohmann::json j = item("text", role);
  j["text"] = text;
  j["at"] = point(at);
  items_.push_back(std::move(j));
  return *this;
}

Overlay2D& Overlay2D::append(const Overlay2D& other) {
  for (const auto& j : other.items_) items_.push_back(j);
  return *this;
}

nlohmann::json Overlay2D::json() const {
  return nlohmann::json{{"frame", "image"}, {"items", items_}};
}

Record Overlay2D::record() const {
  Record r;
  r.type = kOverlay2DType;
  r.data = json();
  return r;
}

Data Overlay2D::data() const { return Data::record(record()); }

}  // namespace lyflow
