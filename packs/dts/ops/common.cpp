#include "dts_ops.h"

namespace lyflow::dts {

Profile profileFromCloud(const PointCloud& cloud) {
  Profile p;
  const std::size_t n = cloud.pointCount();
  p.x.resize(n);
  p.z.resize(n);
  for (std::size_t i = 0; i < n; ++i) {
    p.x[i] = cloud.xyz[3 * i];
    p.z[i] = cloud.xyz[3 * i + 2];
  }
  return p;
}

PointCloud cloudFromProfile(const Profile& p, const std::vector<float>& intensity) {
  PointCloud c;
  const std::size_t n = p.size();
  c.reserve(n);
  for (std::size_t i = 0; i < n; ++i) c.push(p.x[i], 0.0f, p.z[i]);
  if (intensity.size() == n) c.intensity = intensity;
  return c;
}

std::vector<Piece> piecesFromJson(const nlohmann::json& faces) {
  std::vector<Piece> out;
  if (!faces.contains("pieces")) return out;
  for (const auto& j : faces["pieces"]) {
    out.push_back(Piece{j.value("i0", 0), j.value("i1", 0)});
  }
  return out;
}

std::vector<Face> facesFromJson(const nlohmann::json& faces) {
  std::vector<Face> out;
  if (!faces.contains("faces")) return out;
  for (const auto& j : faces["faces"]) {
    Face f;
    f.x0 = j.value("x0", 0.0);
    f.x1 = j.value("x1", 0.0);
    f.z0 = j.value("z0", 0.0);
    f.z1 = j.value("z1", 0.0);
    f.lenMm = j.value("lenMm", 0.0);
    f.angleDeg = j.value("angleDeg", 0.0);
    f.rmsMm = j.value("rmsMm", 0.0);
    f.n = j.value("n", 0);
    f.line.px = j.value("px", 0.0);
    f.line.pz = j.value("pz", 0.0);
    f.line.dx = j.value("dx", 1.0);
    f.line.dz = j.value("dz", 0.0);
    f.line.rms = f.rmsMm;
    f.line.n = f.n;
    out.push_back(f);
  }
  return out;
}

nlohmann::json faceToJson(const Face& f) {
  return nlohmann::json{{"x0", f.x0},           {"x1", f.x1},   {"z0", f.z0},
                        {"z1", f.z1},           {"lenMm", f.lenMm}, {"angleDeg", f.angleDeg},
                        {"rmsMm", f.rmsMm},     {"n", f.n},     {"px", f.line.px},
                        {"pz", f.line.pz},      {"dx", f.line.dx}, {"dz", f.line.dz}};
}

const nlohmann::json* recordData(const Data& d, const char* type) {
  const Record* r = d.asRecord();
  if (r == nullptr) return nullptr;
  if (type != nullptr && r->type != type) return nullptr;
  return &r->data;
}

}  // namespace lyflow::dts
