#include <cmath>
#include <cstdio>
#include <limits>

#include <opencv2/imgproc.hpp>

#include "lyflow_cv/adapter.h"
#include "lyflow_cv/inspection.h"

namespace lyflow::std_image::inspection {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

} // namespace

using line_scan::finite;
using line_scan::perp;
using line_scan::unit;

double roundTo(double v, int decimals) {
  if (!std::isfinite(v))
    return v;
  const double k = std::pow(10.0, decimals);
  return std::round(v * k) / k;
}

void registerInspectionBundles(Registry& r) {
  r.addBundle(BundleDesc{
      kResponseMapsKind,
      "响应图",
      "glue.response 的输出：三张单通道 f32 图只有 frame 那一块（roi ∩ 图像，没设 roi 时是整幅），"
      "下游按 frame 的偏移采样，坐标仍是整幅图像素坐标。",
      "",
      {BundleField{"response", "Image", "顶帽响应（≥ 0）。"},
       BundleField{"gray", "Image", "平滑灰度（亮胶已取反），边缘陡度用。"},
       BundleField{"level", "Image", "局部亮度，对比度归一化用。"},
       BundleField{"frame", "Record",
                   "glue.Frame：x、y（块左上角在整幅图里的整数像素坐标）、width、height（块的宽高，"
                   "与三张图一致）、imageWidth、imageHeight（整幅图的宽高）。"}}});
  r.addBundle(BundleDesc{
      kColorMapsKind,
      "胶色得分图",
      "glue.color_map 的输出：三张单通道 f32 图只有 frame 那一块（roi ∩ 图像，没设 roi 时是整幅），"
      "下游按 frame 的偏移采样，坐标仍是整幅图像素坐标。",
      "",
      {BundleField{"score", "Image",
                   "像胶的得分 ∈ [0, 1]；高光、暗区（无信息）是 NaN，暗背景是 0。"},
       BundleField{"luma", "Image", "平滑后的 BT.601 亮度。"},
       BundleField{"state", "Image",
                   "像素状态：0 有信息、1 高光、2 暗区（无信息）、3 暗背景（得分 0）。"},
       BundleField{"frame", "Record", "glue.Frame：与 glue.ResponseMaps 的 frame 相同。"}}});
}

// ------------------------------------------------------------------ 图像

cv::Mat grayOf(const Data& image) {
  const Image* img = image.asImage();
  // 文件里的测量点、模板和标定都以原图为准；预览缩小图不能冒充正式量测。
  return img && img->scale == 1 ? cvx::gray8(*img) : cv::Mat{};
}

cv::Mat rgbOf(const Data& image) {
  const Image* img = image.asImage();
  if (!img || img->scale != 1 || img->depth != PixelDepth::U8 || !img->consistent() ||
      (img->channels != 3 && img->channels != 4))
    return {};
  const cv::Mat v = cvx::view(*img);
  if (img->channels == 3)
    return v;
  cv::Mat out;
  cv::cvtColor(v, out, cv::COLOR_RGBA2RGB);
  return out;
}

Status badColorImage(const char* port) {
  return Status::Error(
      Phase::Execute, "bad_input",
      "彩色算子需要 RGB：全分辨率 u8 三通道 / 四通道图像（灰度图请走 glue.response 链路）", {},
      port);
}

namespace {

Data f32Image(cv::Mat m) {
  Image img;
  cvx::fromMat(std::move(m), img);
  return Data::image(std::move(img));
}

Record frameRecord(cv::Rect frame, cv::Size image) {
  Record rec;
  rec.type = kFrameType;
  rec.data = Json{{"x", frame.x},
                  {"y", frame.y},
                  {"width", frame.width},
                  {"height", frame.height},
                  {"imageWidth", image.width},
                  {"imageHeight", image.height}};
  return rec;
}

/// Bundle 里的 frame 与同尺寸的单通道 f32 图。
bool frameOf(const Bundle& b, cv::Rect* out) {
  const Data* f = b.field("frame");
  const Record* rec = f ? f->asRecord() : nullptr;
  if (!rec || rec->type != kFrameType)
    return false;
  const Json& j = rec->data;
  const auto num = [&](const char* k) { return j.contains(k) && j[k].is_number_integer(); };
  if (!num("x") || !num("y") || !num("width") || !num("height"))
    return false;
  *out =
      cv::Rect(j["x"].get<int>(), j["y"].get<int>(), j["width"].get<int>(), j["height"].get<int>());
  return true;
}

bool planeOf(const Bundle& b, const char* name, cv::Rect frame, cv::Mat* out) {
  const Data* x = b.field(name);
  const Image* img = x ? x->asImage() : nullptr;
  if (!img || img->scale != 1 || img->channels != 1 || img->depth != PixelDepth::F32 ||
      !img->consistent() || img->width != frame.width || img->height != frame.height)
    return false;
  *out = cvx::view(*img);
  return true;
}

} // namespace

Data colorMapsBundle(ColorMaps maps, cv::Size image) {
  Bundle b(kColorMapsKind);
  b.set("score", f32Image(std::move(maps.score)));
  b.set("luma", f32Image(std::move(maps.luma)));
  b.set("state", f32Image(std::move(maps.state)));
  b.set("frame", Data::record(frameRecord(maps.frame, image)));
  return Data::bundle(std::move(b));
}

bool readColorMaps(const Data& d, ColorMapsView* out) {
  const Bundle* b = d.asBundle();
  if (!b || b->kind != kColorMapsKind)
    return false;
  cv::Rect frame;
  cv::Mat score;
  if (!frameOf(*b, &frame) || !planeOf(*b, "score", frame, &score) ||
      !planeOf(*b, "state", frame, &out->state) || !planeOf(*b, "luma", frame, &out->luma))
    return false;
  out->image = {};
  const Json& meta = b->field("frame")->asRecord()->data;
  if (meta.contains("imageWidth") && meta["imageWidth"].is_number_integer() &&
      meta.contains("imageHeight") && meta["imageHeight"].is_number_integer())
    out->image = cv::Size(meta["imageWidth"].get<int>(), meta["imageHeight"].get<int>());
  out->score = Field(score, frame.tl(), frame);
  out->frame = frame;
  return true;
}

Data mapsBundle(ResponseMaps maps, cv::Size image) {
  Bundle b(kResponseMapsKind);
  b.set("response", f32Image(std::move(maps.response)));
  b.set("gray", f32Image(std::move(maps.gray)));
  b.set("level", f32Image(std::move(maps.level)));
  b.set("frame", Data::record(frameRecord(maps.frame, image)));
  return Data::bundle(std::move(b));
}

bool readMaps(const Data& d, MapsView* out) {
  const Bundle* b = d.asBundle();
  if (!b || b->kind != kResponseMapsKind)
    return false;
  cv::Rect frame;
  cv::Mat response, gray, level;
  if (!frameOf(*b, &frame) || !planeOf(*b, "response", frame, &response) ||
      !planeOf(*b, "gray", frame, &gray) || !planeOf(*b, "level", frame, &level))
    return false;
  out->response = Field(response, frame.tl(), frame);
  out->gray = Field(gray, frame.tl(), frame);
  out->level = Field(level, frame.tl(), frame);
  out->frame = frame;
  return true;
}

Status badImage(const char* port, bool maps) {
  return Status::Error(
      Phase::Execute, "bad_input",
      maps ? "需要 glue.response 输出的 maps（Bundle<glue.ResponseMaps>）"
           : "需要全分辨率非空 u8 图像；其他位深先接 image.normalize，缩小预览请改用完整运行",
      {}, port);
}

// ------------------------------------------------------------------ JSON

double round3(double v) { return roundTo(v, 3); }

Json pxJson(P2 p) {
  if (!finite(p))
    return Json();
  return Json::array({round3(p.x), round3(p.y)});
}

P2 pxOf(const Json& j) {
  if (!j.is_array() || j.size() != 2 || !j[0].is_number() || !j[1].is_number()) {
    return P2(kNaN, kNaN);
  }
  return P2(j[0].get<double>(), j[1].get<double>());
}

Json numOrNull(double v, int decimals) {
  if (!std::isfinite(v))
    return Json();
  return roundTo(v, decimals);
}

Json exact(double v) { return std::isfinite(v) ? Json(v) : Json(); }

Json exactPx(P2 p) { return finite(p) ? Json::array({p.x, p.y}) : Json(); }

double numOf(const Json& j) { return j.is_number() ? j.get<double>() : kNaN; }

// ------------------------------------------------------------------ 折线

std::vector<P2> Polyline::between(double s0, double s1) const {
  std::vector<P2> out;
  if (s.empty() || s1 < s0)
    return out;
  const PolylineView v = view();
  P2 p, t;
  v.at(s0, &p, &t);
  out.push_back(p);
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] > s0 + 1e-9 && s[i] < s1 - 1e-9)
      out.push_back(points[i]);
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
  if (!rec)
    return false;
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
    if (!std::isfinite(line.s.back()) || !finite(line.points.back()))
      return false;
    if (i > 0 && line.s[i] < line.s[i - 1])
      return false;
  }
  *out = std::move(line);
  return true;
}

bool readLine(const Data& d, Polyline* out) {
  const Record* rec = d.asRecord();
  if (!rec || rec->type != kPolylineType)
    return false;
  const auto count = rec->data.find("count");
  if (count != rec->data.end() && count->is_number() && count->get<double>() == 0) {
    *out = Polyline();
    return true;
  }
  return readPolyline(d, out);
}

std::vector<StationPose> layStations(const Polyline& line, double step, double rangeA,
                                     double rangeB, double* usedA, double* usedB) {
  std::vector<StationPose> out;
  *usedA = *usedB = kNaN;
  if (line.s.empty() || !(step > 0))
    return out;
  double a = line.s.front(), b = line.s.back();
  if (!(rangeA == 0 && rangeB == 0)) {
    a = std::max(a, rangeA);
    b = std::min(b, rangeB);
  }
  if (!(b >= a))
    return out;
  *usedA = a;
  *usedB = b;
  const PolylineView v = line.view();
  for (int k = 0;; ++k) {
    const double s = a + k * step;
    if (s > b + 1e-6)
      break;
    StationPose st;
    st.s = s;
    v.at(s, &st.c, &st.t);
    st.n = perp(st.t);
    out.push_back(st);
  }
  return out;
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

Param intParam(const char* name, const char* label, int def, const char* doc, int lo, int hi,
               bool advanced) {
  Param p;
  p.name = name;
  p.tuningRole = "detection";
  p.type = ParamType::Int;
  p.label = label;
  p.doc = doc;
  p.def = Value::integer(def);
  p.min = lo;
  p.max = hi;
  p.advanced = advanced;
  return p;
}

Param rangedFloat(const char* name, const char* label, double def, const char* unitName,
                  const char* doc, double lo, double hi, bool advanced) {
  Param p = floatParam(name, label, def, unitName, doc, advanced);
  if (std::isfinite(lo))
    p.min = lo;
  if (std::isfinite(hi))
    p.max = hi;
  return p;
}

Param visibleWhen(Param p, const char* param, const char* eq) {
  p.visibleWhen.param = param;
  p.visibleWhen.eq = Value::text(eq);
  return p;
}

Data measurement(double value, const std::string& unitName, const std::string& messageIfMissing) {
  lyflow::Measurement m;
  m.value = value;
  m.ok = std::isfinite(value);
  m.unit = unitName;
  if (!m.ok)
    m.message = messageIfMissing;
  return Data::measurement(std::move(m));
}

std::string fmt(double v, int decimals) {
  if (!std::isfinite(v))
    return "—";
  char buf[48];
  std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
  return buf;
}

} // namespace lyflow::std_image::inspection
