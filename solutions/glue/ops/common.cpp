// glue 包的共用件：两种 Bundle 的声明、Record 的读写、标定、参数与格式化小工具。
#include <cmath>
#include <limits>

#include "glue.h"

namespace lyflow::packs::solution_glue {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

} // namespace

// ------------------------------------------------------------------ Bundle 声明（M8a L2）

void registerBundles(Registry& r) {
  r.addBundle(BundleDesc{
      kPathKind,
      "胶路",
      "glue.bead_path 定下的胶路：一条覆盖整个检测区的光滑折线（跨得过断口），与它是怎么找到的。"
      "没找到胶时 info.ok = false，折线是沿选中方向的一条直线，下游照常跑出「全段无胶」。"
      "glue.taught_path 出同形的示教胶路：info.lineSource = taught、ok = true，搜索才有的字段是 null。",
      "",
      {BundleField{
           "line", "Record",
           "glue.Polyline：points（图像像素）、s（从喷嘴沿胶路的弧长，px）、tangents（单位切向，"
           "指向远离喷嘴的一侧），相邻两点 2 px。"},
       BundleField{
           "info", "Record",
           "glue.PathInfo：ok、message、reason、heading（度，图像坐标）、headingSource"
           "（param | search | taught）、coverage、sharpness、residual、zone、nozzle、polarity、widthMax、"
           "lineSource（fit | fallback | taught）、candidates（试过的方向）；示教胶路另有 tolerance、sharpMin、"
           "taughtCount、taughtLength。"}}});
  r.addBundle(BundleDesc{
      kBeadKind,
      "胶",
      "glue.bead_width 沿胶路逐站量出来的胶：每站的中心、两边、宽度、有无胶。Bundle 不嵌套，"
      "所以把胶路折线本身也带着。",
      "",
      {BundleField{"line", "Record", "glue.Polyline：与 glue.Path 的 line 相同。"},
       BundleField{
           "stations", "Record",
           "glue.Stations：数组形式的逐站数据 —— s、center、normal、present、lo / hi（两边沿法向的"
           "偏移，px）、left / right（两边的像素点）、width（按 unit）、widthPx、contrast。"},
       BundleField{
           "info", "Record",
           "glue.BeadInfo：form、stationStep、unit（px | mm）、pathOk、message、zone、coverage、"
           "wRef、polarity、widthMax、lineSource、calib（接了标定时的那一份 image.PlaneCalib，否则 null）；"
           "示教胶路另有 tolerance、offset、trackSupport、sharpness、reason。"}}});
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
  if (!b || b->kind != kPathKind)
    return false;
  const Data* l = b->field("line");
  const Data* i = b->field("info");
  if (!l || !i || !i->asRecord() || !readPolyline(*l, line))
    return false;
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
  if (!b || b->kind != kBeadKind)
    return false;
  const Data* l = b->field("line");
  const Data* s = b->field("stations");
  const Data* i = b->field("info");
  if (!l || !s || !i || !s->asRecord() || !i->asRecord())
    return false;
  if (!readPolyline(*l, &out->line))
    return false;
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
       width = Json::array(), widthPx = Json::array(), contrast = Json::array(),
       candidateLo = Json::array(), candidateHi = Json::array(), candidateWidthPx = Json::array();
  for (const Station& st : stations) {
    s.push_back(round3(st.s));
    center.push_back(pxJson(st.c));
    normal.push_back(Json::array({roundTo(st.n.x, 5), roundTo(st.n.y, 5)}));
    present.push_back(st.present);
    const double diagnosticLo = st.present ? st.lo : st.candidateLo;
    const double diagnosticHi = st.present ? st.hi : st.candidateHi;
    candidateLo.push_back(numOrNull(diagnosticLo));
    candidateHi.push_back(numOrNull(diagnosticHi));
    candidateWidthPx.push_back(numOrNull(diagnosticHi - diagnosticLo));
    if (st.present) {
      lo.push_back(round3(st.lo));
      hi.push_back(round3(st.hi));
      left.push_back(pxJson(st.left()));
      right.push_back(pxJson(st.right()));
      width.push_back(numOrNull(metric.distance(st.left(), st.right())));
      widthPx.push_back(round3(st.width()));
      contrast.push_back(roundTo(st.peak, 1));
    } else {
      for (Json* a : {&lo, &hi, &left, &right, &width, &widthPx, &contrast})
        a->push_back(Json());
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
  j["candidateLo"] = std::move(candidateLo);
  j["candidateHi"] = std::move(candidateHi);
  j["candidateWidthPx"] = std::move(candidateWidthPx);
  return j;
}

bool readStations(const Json& j, std::vector<Station>* out) {
  const char* keys[] = {"s", "center", "normal", "present", "lo", "hi", "contrast"};
  std::size_t n = 0;
  for (std::size_t k = 0; k < std::size(keys); ++k) {
    const auto it = j.find(keys[k]);
    if (it == j.end() || !it->is_array())
      return false;
    if (k == 0)
      n = it->size();
    if (it->size() != n)
      return false;
  }
  std::vector<Station> st(n);
  for (std::size_t i = 0; i < n; ++i) {
    Station& x = st[i];
    x.s = numOf(j["s"][i]);
    x.c = pxOf(j["center"][i]);
    x.n = unit(pxOf(j["normal"][i]));
    x.t = P2(x.n.y, -x.n.x); // n = (−t.y, t.x) 反过来
    x.present = j["present"][i].is_boolean() && j["present"][i].get<bool>();
    if (x.present) {
      x.lo = numOf(j["lo"][i]);
      x.hi = numOf(j["hi"][i]);
      x.peak = numOf(j["contrast"][i]);
      if (!std::isfinite(x.lo) || !std::isfinite(x.hi))
        x.present = false;
    }
    if (!std::isfinite(x.s) || !finite(x.c))
      return false;
  }
  *out = std::move(st);
  return true;
}

// ------------------------------------------------------------------ 标定

double Metric::distance(P2 a, P2 b) const {
  if (!finite(a) || !finite(b))
    return kNaN;
  if (!calib)
    return solution_glue::length(b - a);
  double ax, ay, bx, by;
  if (!calib->map(a.x, a.y, ax, ay) || !calib->map(b.x, b.y, bx, by))
    return kNaN;
  return std::hypot(bx - ax, by - ay);
}

double Metric::length(const std::vector<P2>& pts) const {
  double total = 0;
  for (std::size_t i = 1; i < pts.size(); ++i)
    total += distance(pts[i - 1], pts[i]);
  return total;
}

Json Metric::calibJson() const {
  if (!calib)
    return {};
  return Json{{"schemaVersion", 1},
              {"H", calib->H},
              {"unit", calib->unit},
              {"imageFrame", calib->imageFrame},
              {"planeFrame", calib->planeFrame}};
}

static Status readMetric(const Record& record, const char* port, Metric* out) {
  if (!record.data.contains("schemaVersion"))
    return Status::Error(
        Phase::Execute, "bad_input",
        "需要版本化的 image.PlaneCalib；旧图请用 lyflow migrate 的 planeCalib.v1 规则", {}, port);
  calibration::Plane plane;
  std::string why;
  if (!calibration::parsePlane(record, plane, why)) {
    return Status::Error(Phase::Execute, "bad_input", "calib 不是合格的 image.PlaneCalib：" + why,
                         {}, port);
  }
  out->calib = std::move(plane);
  return Status::Ok();
}

Status metricFromInput(const Inputs& inputs, const char* port, Metric* out) {
  out->calib.reset();
  if (!inputs.has(port))
    return Status::Ok();
  return readMetric(*inputs.get(port).asRecord(), port, out);
}

Status metricFromInfo(const Json& info, Metric* out) {
  out->calib.reset();
  const auto it = info.find("calib");
  if (it == info.end() || it->is_null())
    return Status::Ok();
  return readMetric(Record{calibration::kPlaneType, *it}, "bead", out);
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
    while (j + 1 < st.size() && st[j + 1].present)
      ++j;
    out.emplace_back(i, j);
    i = j + 1;
  }
  return out;
}

} // namespace lyflow::packs::solution_glue
