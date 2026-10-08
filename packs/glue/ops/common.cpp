// glue 包的共用件：两种 Bundle 的声明、Record 的读写、标定、参数与格式化小工具。
#include <cmath>
#include <cstdio>
#include <limits>

#include "glue.h"
#include "lyflow_cv/adapter.h"

namespace lyflow::packs::glue {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

double roundTo(double v, int decimals) {
  if (!std::isfinite(v)) return v;
  const double k = std::pow(10.0, decimals);
  return std::round(v * k) / k;
}

}  // namespace

// ------------------------------------------------------------------ Bundle 声明（M8a L2）

void registerBundles(Registry& r) {
  r.addBundle(BundleDesc{
      kPathKind,
      "胶路",
      "glue.bead_path 定下的胶路：一条覆盖整个检测区的光滑折线（跨得过断口），与它是怎么找到的。"
      "没找到胶时 info.ok = false，折线是沿选中方向的一条直线，下游照常跑出「全段无胶」。",
      "",
      {BundleField{"line", "Record",
                   "glue.Polyline：points（图像像素）、s（从喷嘴沿胶路的弧长，px）、tangents（单位切向，"
                   "指向远离喷嘴的一侧），相邻两点 2 px。"},
       BundleField{"info", "Record",
                   "glue.PathInfo：ok、message、reason、heading（度，图像坐标）、headingSource"
                   "（param | search）、coverage、sharpness、residual、zone、nozzle、polarity、widthMax、"
                   "candidates（试过的方向）。"}}});
  r.addBundle(BundleDesc{
      kBeadKind,
      "胶",
      "glue.bead_width 沿胶路逐站量出来的胶：每站的中心、两边、宽度、有无胶。Bundle 不嵌套，"
      "所以把胶路折线本身也带着。",
      "",
      {BundleField{"line", "Record", "glue.Polyline：与 glue.Path 的 line 相同。"},
       BundleField{"stations", "Record",
                   "glue.Stations：数组形式的逐站数据 —— s、center、normal、present、lo / hi（两边沿法向的"
                   "偏移，px）、left / right（两边的像素点）、width（按 unit）、widthPx、contrast。"},
       BundleField{"info", "Record",
                   "glue.BeadInfo：form、stationStep、unit（px | mm）、pathOk、message、zone、coverage、"
                   "wRef、polarity、widthMax、calib（接了标定时的那一份 image.PlaneCalib，否则 null）。"}}});
}

// ------------------------------------------------------------------ 图像

cv::Mat grayOf(const Data& image) {
  const Image* img = image.asImage();
  // 文件里的测量点、模板和标定都以原图为准；预览缩小图不能冒充正式量测。
  return img && img->scale == 1 ? cvx::gray8(*img) : cv::Mat{};
}

// ------------------------------------------------------------------ JSON

double round3(double v) { return roundTo(v, 3); }

Json pxJson(P2 p) {
  if (!finite(p)) return Json();
  return Json::array({round3(p.x), round3(p.y)});
}

P2 pxOf(const Json& j) {
  if (!j.is_array() || j.size() != 2 || !j[0].is_number() || !j[1].is_number()) {
    return P2(kNaN, kNaN);
  }
  return P2(j[0].get<double>(), j[1].get<double>());
}

Json numOrNull(double v, int decimals) {
  if (!std::isfinite(v)) return Json();
  return roundTo(v, decimals);
}

double numOf(const Json& j) { return j.is_number() ? j.get<double>() : kNaN; }

// ------------------------------------------------------------------ 折线

std::vector<P2> Polyline::between(double s0, double s1) const {
  std::vector<P2> out;
  if (s.empty() || s1 < s0) return out;
  const PolylineView v = view();
  P2 p, t;
  v.at(s0, &p, &t);
  out.push_back(p);
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] > s0 + 1e-9 && s[i] < s1 - 1e-9) out.push_back(points[i]);
  }
  v.at(s1, &p, &t);
  out.push_back(p);
  return out;
}

Data polylineRecord(const std::vector<double>& s, const std::vector<P2>& points,
                    const std::vector<P2>& tangents) {
  Json j;
  j["count"] = s.size();
  j["spacing"] = 2;
  Json js = Json::array(), jp = Json::array(), jt = Json::array();
  for (std::size_t i = 0; i < s.size(); ++i) {
    js.push_back(round3(s[i]));
    jp.push_back(pxJson(points[i]));
    jt.push_back(Json::array({roundTo(tangents[i].x, 5), roundTo(tangents[i].y, 5)}));
  }
  j["s"] = std::move(js);
  j["points"] = std::move(jp);
  j["tangents"] = std::move(jt);
  Record rec;
  rec.type = kPolylineType;
  rec.data = std::move(j);
  return Data::record(std::move(rec));
}

bool readPolyline(const Data& d, Polyline* out) {
  const Record* rec = d.asRecord();
  if (!rec) return false;
  const Json& j = rec->data;
  const auto s = j.find("s");
  const auto p = j.find("points");
  const auto t = j.find("tangents");
  if (s == j.end() || p == j.end() || t == j.end() || !s->is_array() || !p->is_array() ||
      !t->is_array() || s->size() != p->size() || s->size() != t->size() || s->empty()) {
    return false;
  }
  Polyline line;
  for (std::size_t i = 0; i < s->size(); ++i) {
    line.s.push_back(numOf((*s)[i]));
    line.points.push_back(pxOf((*p)[i]));
    line.tangents.push_back(unit(pxOf((*t)[i])));
    if (!std::isfinite(line.s.back()) || !finite(line.points.back())) return false;
    if (i > 0 && line.s[i] < line.s[i - 1]) return false;
  }
  *out = std::move(line);
  return true;
}

// ------------------------------------------------------------------ Bundle 读写

Data pathBundle(Data line, Json info) {
  Record rec;
  rec.type = kPathInfoType;
  rec.data = std::move(info);
  Bundle b(kPathKind);
  b.set("line", std::move(line));
  b.set("info", Data::record(std::move(rec)));
  return Data::bundle(std::move(b));
}

bool readPath(const Data& d, Polyline* line, Json* info) {
  const Bundle* b = d.asBundle();
  if (!b || b->kind != kPathKind) return false;
  const Data* l = b->field("line");
  const Data* i = b->field("info");
  if (!l || !i || !i->asRecord() || !readPolyline(*l, line)) return false;
  *info = i->asRecord()->data;
  return true;
}

Data beadBundle(Data line, Json stations, Json info) {
  Record st;
  st.type = kStationsType;
  st.data = std::move(stations);
  Record in;
  in.type = kBeadInfoType;
  in.data = std::move(info);
  Bundle b(kBeadKind);
  b.set("line", std::move(line));
  b.set("stations", Data::record(std::move(st)));
  b.set("info", Data::record(std::move(in)));
  return Data::bundle(std::move(b));
}

bool readBead(const Data& d, BeadView* out) {
  const Bundle* b = d.asBundle();
  if (!b || b->kind != kBeadKind) return false;
  const Data* l = b->field("line");
  const Data* s = b->field("stations");
  const Data* i = b->field("info");
  if (!l || !s || !i || !s->asRecord() || !i->asRecord()) return false;
  if (!readPolyline(*l, &out->line)) return false;
  out->stations = s->asRecord()->data;
  out->info = i->asRecord()->data;
  return true;
}

// ------------------------------------------------------------------ 逐站

Json stationsJson(const std::vector<Station>& stations, double step, const std::string& form,
                  const Metric& metric) {
  Json j;
  j["count"] = stations.size();
  j["step"] = step;
  j["form"] = form;
  j["unit"] = metric.unit();
  Json s = Json::array(), center = Json::array(), normal = Json::array(), present = Json::array(),
       lo = Json::array(), hi = Json::array(), left = Json::array(), right = Json::array(),
       width = Json::array(), widthPx = Json::array(), contrast = Json::array();
  for (const Station& st : stations) {
    s.push_back(round3(st.s));
    center.push_back(pxJson(st.c));
    normal.push_back(Json::array({roundTo(st.n.x, 5), roundTo(st.n.y, 5)}));
    present.push_back(st.present);
    if (st.present) {
      lo.push_back(round3(st.lo));
      hi.push_back(round3(st.hi));
      left.push_back(pxJson(st.left()));
      right.push_back(pxJson(st.right()));
      width.push_back(numOrNull(metric.distance(st.left(), st.right())));
      widthPx.push_back(round3(st.width()));
      contrast.push_back(roundTo(st.peak, 1));
    } else {
      for (Json* a : {&lo, &hi, &left, &right, &width, &widthPx, &contrast}) a->push_back(Json());
    }
  }
  j["s"] = std::move(s);
  j["center"] = std::move(center);
  j["normal"] = std::move(normal);
  j["present"] = std::move(present);
  j["lo"] = std::move(lo);
  j["hi"] = std::move(hi);
  j["left"] = std::move(left);
  j["right"] = std::move(right);
  j["width"] = std::move(width);
  j["widthPx"] = std::move(widthPx);
  j["contrast"] = std::move(contrast);
  return j;
}

bool readStations(const Json& j, std::vector<Station>* out) {
  const char* keys[] = {"s", "center", "normal", "present", "lo", "hi", "contrast"};
  std::size_t n = 0;
  for (std::size_t k = 0; k < std::size(keys); ++k) {
    const auto it = j.find(keys[k]);
    if (it == j.end() || !it->is_array()) return false;
    if (k == 0) n = it->size();
    if (it->size() != n) return false;
  }
  std::vector<Station> st(n);
  for (std::size_t i = 0; i < n; ++i) {
    Station& x = st[i];
    x.s = numOf(j["s"][i]);
    x.c = pxOf(j["center"][i]);
    x.n = unit(pxOf(j["normal"][i]));
    x.t = P2(x.n.y, -x.n.x);  // n = (−t.y, t.x) 反过来
    x.present = j["present"][i].is_boolean() && j["present"][i].get<bool>();
    if (x.present) {
      x.lo = numOf(j["lo"][i]);
      x.hi = numOf(j["hi"][i]);
      x.peak = numOf(j["contrast"][i]);
      if (!std::isfinite(x.lo) || !std::isfinite(x.hi)) x.present = false;
    }
    if (!std::isfinite(x.s) || !finite(x.c)) return false;
  }
  *out = std::move(st);
  return true;
}

// ------------------------------------------------------------------ 标定

double Metric::distance(P2 a, P2 b) const {
  if (!finite(a) || !finite(b)) return kNaN;
  if (!calib) return glue::length(b - a);
  return calib->distance(a.x, a.y, b.x, b.y);
}

double Metric::length(const std::vector<P2>& pts) const {
  double total = 0;
  for (std::size_t i = 1; i < pts.size(); ++i) total += distance(pts[i - 1], pts[i]);
  return total;
}

Status metricFromInput(const Inputs& inputs, const char* port, Metric* out) {
  out->calib.reset();
  if (!inputs.has(port)) return Status::Ok();
  const Record* rec = inputs.get(port).asRecord();
  if (!rec) return Status::Error(Phase::Execute, "bad_input", "calib 不是 Record", {}, port);
  std_image::PlaneCalib c;
  std::string why;
  if (!std_image::parsePlaneCalib(rec->data, &c, &why)) {
    return Status::Error(Phase::Execute, "bad_input", "calib 不是合格的 image.PlaneCalib：" + why,
                         {}, port);
  }
  out->calib = c;
  return Status::Ok();
}

Metric metricFromInfo(const Json& info) {
  Metric m;
  const auto it = info.find("calib");
  if (it != info.end() && it->is_object()) {
    std_image::PlaneCalib c;
    if (std_image::parsePlaneCalib(*it, &c, nullptr)) m.calib = c;
  }
  return m;
}

// ------------------------------------------------------------------ Measurement

Data measurement(double value, const std::string& unitName, const std::string& messageIfMissing) {
  lyflow::Measurement m;
  m.value = value;
  m.ok = std::isfinite(value);
  m.unit = unitName;
  if (!m.ok) m.message = messageIfMissing;
  return Data::measurement(std::move(m));
}

// ------------------------------------------------------------------ 参数

Param floatParam(const char* name, const char* label, double def, const char* unitName,
                 const char* doc, bool advanced) {
  Param p;
  p.name = name;
  p.tuningRole = "detection";
  p.type = ParamType::Float;
  p.label = label;
  p.doc = doc;
  p.def = Value::number(def);
  p.unit = unitName;
  p.advanced = advanced;
  return p;
}

Param vec2Param(const char* name, const char* label, double a, double b, const char* unitName,
                std::vector<std::string> components, const char* doc, bool advanced) {
  Param p;
  p.name = name;
  p.tuningRole = "detection";
  p.type = ParamType::Vec2f;
  p.label = label;
  p.doc = doc;
  p.def = Value::vec({a, b});
  p.unit = unitName;
  p.componentLabels = std::move(components);
  p.advanced = advanced;
  return p;
}

Param enumParam(const char* name, const char* label, const char* def, const char* doc,
                std::vector<EnumOption> options, bool advanced) {
  Param p;
  p.name = name;
  p.tuningRole = "detection";
  p.type = ParamType::Enum;
  p.label = label;
  p.doc = doc;
  p.def = Value::text(def);
  p.options = std::move(options);
  p.advanced = advanced;
  return p;
}

Param boolParam(const char* name, const char* label, bool def, const char* doc, bool advanced) {
  Param p;
  p.name = name;
  p.tuningRole = "detection";
  p.type = ParamType::Bool;
  p.label = label;
  p.doc = doc;
  p.def = Value::boolean(def);
  p.advanced = advanced;
  return p;
}

Param visibleWhen(Param p, const char* param, const char* eq) {
  p.visibleWhen.param = param;
  p.visibleWhen.eq = Value::text(eq);
  return p;
}

// ------------------------------------------------------------------ 叠画

std::vector<std::pair<std::size_t, std::size_t>> presentRuns(const std::vector<Station>& st) {
  std::vector<std::pair<std::size_t, std::size_t>> out;
  for (std::size_t i = 0; i < st.size();) {
    if (!st[i].present) {
      ++i;
      continue;
    }
    std::size_t j = i;
    while (j + 1 < st.size() && st[j + 1].present) ++j;
    out.emplace_back(i, j);
    i = j + 1;
  }
  return out;
}

std::string fmt(double v, int decimals) {
  if (!std::isfinite(v)) return "—";
  char buf[48];
  std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
  return buf;
}

}  // namespace lyflow::packs::glue
