// glue 包 1.3 彩色测量算子（docs/glue-measure-ops.md「1.3 彩色测量算子」）：合成 RGB 场景上的胶宽、无信息与断胶的区分、
// 颜色模型的建模语义。图一律现生成（真实帧不进仓库）：粉胶 (230, 120, 130) 落在发黑铸件（暗、纹理重，先做 Bayer 马赛克
// 再去马赛克，带真实假色）或镀锌灰金属（中性灰、大片饱和高光）上，可叠加照明渐变、横穿的高光带、胶内的湿胶高光条。
// 真实帧的评估走 packs/glue/tools/glue_color_eval.py。
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include <nlohmann/json.hpp>
#include <opencv2/imgproc.hpp>

#include "algo/color.h"
#include "exec/executor.h"
#include "exec/result_store.h"
#include "helpers.h"
#include "lyflow_cv/adapter.h"
#include "ops/glue.h"

namespace {

using namespace lyflow;
using namespace lyflow::test;
using lyflow::packs::glue::P2;
namespace glue = lyflow::packs::glue;

// ------------------------------------------------------------------ 合成场景

enum class Substrate { Cast, Metal, Split };

/// 名义中心线从 start 沿 heading 走 length（curvature ≠ 0 时是圆弧，正 = 往法向那一侧拐）；胶中心相对它偏 offset。
struct ColorScene {
  int width = 480;
  int height = 420;
  P2 start{170, 400};
  double headingDeg = -70;
  double curvature = 0;
  double length = 380;
  double beadWidth = 20;
  double offset = 0;
  Substrate substrate = Substrate::Metal;
  double illumLo = 1.0; ///< 照明沿 s 从 illumLo 线性变到 illumHi
  double illumHi = 1.0;
  std::vector<std::pair<double, double>> breaks;     ///< 断口 [s0, s1]
  std::vector<std::pair<double, double>> highlights; ///< 横穿的饱和高光带 [s0, s1]，横向 ±60 px
  double streakLo = NAN;                             ///< 胶内沿胶走的湿胶高光条（横向 [lo, hi]）
  double streakHi = NAN;
  double streakHalo = 0; ///< 高光条两侧发白却没饱和的一圈（px）
  bool bead = true;
  bool grayBand = false; ///< 胶的位置上换成一条中性灰的暗带（阴影、槽）
  unsigned seed = 11;
};

void coords(const ColorScene& sc, double x, double y, double* along, double* across) {
  const P2 u = glue::dirOfDeg(sc.headingDeg);
  const P2 v = glue::perp(u);
  const P2 d(x - sc.start.x, y - sc.start.y);
  if (sc.curvature == 0) {
    *along = glue::dot(d, u);
    *across = glue::dot(d, v);
    return;
  }
  const double R = 1.0 / std::fabs(sc.curvature);
  const double sign = sc.curvature > 0 ? 1.0 : -1.0;
  const P2 c = sc.start + v * (sign * R);
  const P2 r0 = v * -sign;
  const P2 q(x - c.x, y - c.y);
  const double th = std::atan2(glue::dot(q, u), glue::dot(q, r0));
  *along = R * th;
  *across = sign * (R - glue::length(q));
}

/// 名义中心线上的点，用来画示教线、量离真值的距离。
P2 linePoint(const ColorScene& sc, double s) {
  const P2 u = glue::dirOfDeg(sc.headingDeg);
  const P2 v = glue::perp(u);
  if (sc.curvature == 0)
    return sc.start + u * s;
  const double R = 1.0 / std::fabs(sc.curvature);
  const double sign = sc.curvature > 0 ? 1.0 : -1.0;
  const P2 c = sc.start + v * (sign * R);
  const double th = s / R;
  return c + (v * -sign) * (R * std::cos(th)) + u * (R * std::sin(th));
}

std::string taughtLine(const ColorScene& sc, double offset = 0) {
  Json pts = Json::array();
  const P2 v = glue::perp(glue::dirOfDeg(sc.headingDeg));
  for (double s = 0; s <= sc.length + 1e-9; s += sc.curvature == 0 ? sc.length : 10.0) {
    P2 p = linePoint(sc, s);
    if (sc.curvature == 0)
      p += v * offset;
    pts.push_back({p.x, p.y});
  }
  return pts.dump();
}

cv::Vec3d substrateColor(const ColorScene& sc, double across, double texture) {
  const bool cast =
      sc.substrate == Substrate::Cast || (sc.substrate == Substrate::Split && across < 0);
  if (cast)
    return cv::Vec3d(52, 46, 43) * (1.0 + 0.3 * texture);
  return cv::Vec3d(150, 152, 156) * (1.0 + 0.04 * texture);
}

/// 3×3 超采样画，σ = 0.7 的光学模糊，加 σ = 2 的噪声，按 RGGB 排成 Bayer 马赛克再用 BayerBG 去马赛克（与相机链路相同）。
cv::Mat render(const ColorScene& sc) {
  cv::RNG rng(sc.seed);
  cv::Mat texture(sc.height, sc.width, CV_64F);
  rng.fill(texture, cv::RNG::NORMAL, 0.0, 1.0);
  cv::Mat img(sc.height, sc.width, CV_64FC3);
  const cv::Vec3d glueColor(230, 120, 130);
  const cv::Vec3d white(255, 255, 255);
  for (int y = 0; y < sc.height; ++y) {
    for (int x = 0; x < sc.width; ++x) {
      cv::Vec3d acc(0, 0, 0);
      for (int sy = 0; sy < 3; ++sy) {
        for (int sx = 0; sx < 3; ++sx) {
          double s, a;
          coords(sc, x + (sx - 1) / 3.0, y + (sy - 1) / 3.0, &s, &a);
          const double illum =
              sc.illumLo + (sc.illumHi - sc.illumLo) * std::clamp(s / sc.length, 0.0, 1.0);
          const double rel = a - sc.offset;
          const bool inBead = std::fabs(rel) <= 0.5 * sc.beadWidth && s >= 0 && s <= sc.length;
          bool broken = false;
          for (const auto& b : sc.breaks)
            broken = broken || (s >= b.first && s <= b.second);
          bool hot = std::fabs(a) <= 60;
          bool inHighlight = false;
          for (const auto& h : sc.highlights)
            inHighlight = inHighlight || (hot && s >= h.first && s <= h.second);
          cv::Vec3d c = substrateColor(sc, a, texture.at<double>(y, x)) * illum;
          if (sc.substrate != Substrate::Cast && a >= 70 && a <= 110 && s >= 60 && s <= 160)
            c = white; // 镀锌面上一大片饱和高光
          if (inBead && !broken && sc.bead)
            c = sc.grayBand ? cv::Vec3d(62, 62, 64) * illum : glueColor * illum;
          if (inBead && !broken && sc.bead && rel >= sc.streakLo - sc.streakHalo &&
              rel <= sc.streakHi + sc.streakHalo)
            c = cv::Vec3d(235, 205, 205) * illum;
          if (inBead && !broken && sc.bead && rel >= sc.streakLo && rel <= sc.streakHi)
            c = white;
          if (inHighlight)
            c = white;
          acc += c;
        }
      }
      img.at<cv::Vec3d>(y, x) = acc / 9.0;
    }
  }
  cv::GaussianBlur(img, img, cv::Size(5, 5), 0.7);
  cv::Mat noise(img.size(), CV_64FC3);
  rng.fill(noise, cv::RNG::NORMAL, 0.0, 2.0);
  img += noise;
  cv::Mat rgb;
  img.convertTo(rgb, CV_8UC3);
  cv::Mat mosaic(sc.height, sc.width, CV_8U);
  for (int y = 0; y < sc.height; ++y) {
    for (int x = 0; x < sc.width; ++x) {
      const int ch = (y % 2 == 0) ? (x % 2 == 0 ? 0 : 1) : (x % 2 == 0 ? 1 : 2);
      mosaic.at<std::uint8_t>(y, x) = rgb.at<cv::Vec3b>(y, x)[ch];
    }
  }
  cv::Mat out;
  cv::cvtColor(mosaic, out, cv::COLOR_BayerBG2RGB);
  return out;
}

Image imageOf(const cv::Mat& m) {
  Image img;
  REQUIRE(cvx::fromMat(m, img));
  return img;
}

Json recordOf(const Data& d) {
  const Record* r = d.asRecord();
  return r ? r->data : Json();
}

double medianOf(std::vector<double> v) { return glue::median(std::move(v)); }

// ------------------------------------------------------------------ 图

/// 示教帧 n_teach + 示教线 n_line → n_model；生产帧 n_load → n_map（接 n_model.model）→ n_runs（沿 n_line）。
struct ColorParams {
  Json line = Json::object();
  Json model = Json::object();
  Json map = Json::object();
  Json runs = Json::object();
};

ColorParams colorParams(const ColorScene& sc) {
  ColorParams p;
  p.line = Json{{"points", taughtLine(sc)}};
  const double W = sc.beadWidth;
  p.model = Json{{"glueHalf", std::min(4.0, 0.25 * W)}, {"bgBand", {0.5 * W + 8, 0.5 * W + 28}}};
  p.runs = Json{{"searchHalf", 0.5 * W + 30}, {"sRange", {20, sc.length - 20}}};
  return p;
}

Json colorGraph(const ColorParams& p) {
  return makeGraph({N{"n_teach", "io.load_image", Json{{"source", "inputs"}}},
                    N{"n_load", "io.load_image", Json{{"source", "inputs"}}},
                    N{"n_line", "glue.polyline", p.line}, N{"n_model", "glue.color_model", p.model},
                    N{"n_map", "glue.color_map", p.map}, N{"n_runs", "glue.color_runs", p.runs}},
                   {E{"n_teach.image", "n_model.image"}, E{"n_line.line", "n_model.line"},
                    E{"n_load.image", "n_map.image"}, E{"n_model.model", "n_map.model"},
                    E{"n_map.maps", "n_runs.maps"}, E{"n_line.line", "n_runs.line"}});
}

struct ColorRun {
  std::string status;
  Json model, runs;
  cv::Mat score, state;
};

/// 跑一次，把要看的输出拷出来（结果仓在下一次运行时就清掉了）。
ColorRun runColor(const Json& doc, const cv::Mat& teach, const cv::Mat& frame) {
  Session s(doc, {}, {}, /*keepCache=*/false, 0, false,
            {exec::InjectedInput{"n_teach", "image", Data::image(imageOf(teach))},
             exec::InjectedInput{"n_load", "image", Data::image(imageOf(frame))}});
  ColorRun r;
  r.status = s.wait().runStatus();
  if (r.status != "ok")
    return r;
  Data d;
  REQUIRE(exec::ResultStore::instance().get(s.runId(), "n_model", "model", d));
  r.model = recordOf(d);
  REQUIRE(exec::ResultStore::instance().get(s.runId(), "n_runs", "runs", d));
  r.runs = recordOf(d);
  REQUIRE(exec::ResultStore::instance().get(s.runId(), "n_map", "maps", d));
  const Bundle* b = d.asBundle();
  REQUIRE(b);
  r.score = cvx::view(*b->field("score")->asImage()).clone();
  r.state = cvx::view(*b->field("state")->asImage()).clone();
  return r;
}

/// 一站里中心离胶路 ≤ maxMid 的那一段（取最近的）；没有返回 nullptr。
const Json* centralRun(const Json& station, double maxMid = 3.0) {
  const Json* best = nullptr;
  for (const Json& r : station["runs"]) {
    if (std::fabs(r["mid"].get<double>()) <= maxMid &&
        (!best || std::fabs(r["mid"].get<double>()) < std::fabs((*best)["mid"].get<double>())))
      best = &r;
  }
  return best;
}

/// t = 0 落在某个无信息区间里时给出它的 cause，否则空串。
std::string centerUnknown(const Json& station) {
  for (const Json& u : station["unknownSpans"]) {
    if (u["lo"].get<double>() <= 0.0 && u["hi"].get<double>() >= 0.0)
      return u["cause"].get<std::string>();
  }
  return "";
}

std::vector<std::string> errorsOf(const Json& doc) {
  std::vector<std::string> paths;
  for (const Json& d : Json::parse(exec::validateGraphJson(doc.dump(), {}))) {
    if (d.value("severity", "") == "error")
      paths.push_back(d.value("paramPath", ""));
  }
  return paths;
}

Data splitBeadMaps(double gap, float valley, bool sideBand = false, double from = 80,
                   double to = 140) {
  constexpr int width = 201, height = 241;
  cv::Mat score(height, width, CV_32F);
  for (int y = 0; y < height; ++y) {
    const double along = 220 - y;
    for (int x = 0; x < width; ++x) {
      const double across = x - 100;
      double value = std::clamp(24.5 - std::fabs(across), 0.0, 1.0);
      if (along >= from && along <= to) {
        if (sideBand) {
          const double sideHi = -24 - gap;
          if (across >= sideHi - 4 && across <= sideHi)
            value = 0.7;
          else if (across > sideHi && across < -24)
            value = 0.25;
        } else if (std::fabs(across) < 0.5 * gap) {
          value = valley;
        }
      }
      score.at<float>(y, x) = static_cast<float>(value);
    }
  }
  Record frame;
  frame.type = "glue.Frame";
  frame.data = Json{{"x", 0},
                    {"y", 0},
                    {"width", width},
                    {"height", height},
                    {"imageWidth", width},
                    {"imageHeight", height}};
  Bundle maps("glue.ColorMaps");
  maps.set("score", Data::image(imageOf(score)));
  maps.set("state", Data::image(imageOf(cv::Mat::zeros(height, width, CV_32F))));
  maps.set("luma", Data::image(imageOf(cv::Mat(height, width, CV_32F, cv::Scalar(140)))));
  maps.set("frame", Data::record(std::move(frame)));
  return Data::bundle(std::move(maps));
}

Json runColorMaps(const Json& doc, const Data& maps, const char* node, const char* port) {
  Session session(doc, {}, {}, false, 0, false, {exec::InjectedInput{node, "maps", maps}});
  REQUIRE(session.wait().runStatus() == "ok");
  Data result;
  REQUIRE(exec::ResultStore::instance().get(session.runId(), node, port, result));
  return recordOf(result);
}

} // namespace

// 防：对数色度丢了照明不变性（渐变暗端胶变窄或断）、胶—背景的边不落在混合像素的中点（宽度系统性偏 1 px 以上）、
// 铸件去马赛克的假色在胶里或胶边劈出假段。
TEST_CASE("glue 彩色：宽度 {8, 20, 40} × 底材 {铸件, 镀锌, 两侧各一} × 照明 {平, 0.4–1.0 渐变} —— "
          "每站一段居中的胶，宽度中位误差 ≤ 1 px") {
  for (const double W : {8.0, 20.0, 40.0}) {
    for (const Substrate sub : {Substrate::Cast, Substrate::Metal, Substrate::Split}) {
      for (const double lo : {1.0, 0.4}) {
        ColorScene sc;
        sc.beadWidth = W;
        sc.substrate = sub;
        sc.illumLo = lo;
        CAPTURE(W);
        CAPTURE(static_cast<int>(sub));
        CAPTURE(lo);
        const cv::Mat img = render(sc);
        const ColorRun r = runColor(colorGraph(colorParams(sc)), img, img);
        REQUIRE(r.status == "ok");
        CHECK(r.model["quality"]["usable"] == true);
        std::vector<double> err;
        std::size_t missing = 0;
        for (const Json& st : r.runs["stations"]) {
          const Json* c = centralRun(st);
          if (!c) {
            ++missing;
            continue;
          }
          err.push_back(std::fabs((*c)["width"].get<double>() - W));
        }
        CHECK(missing == 0);
        REQUIRE(!err.empty());
        CHECK(medianOf(err) <= 1.0);
      }
    }
  }
}

// 防：高光挡住的胶被当成断胶（无信息必须是值，不能是「没有段」），以及反过来真断口被当成无信息。
TEST_CASE(
    "glue 彩色：高光横穿 → 中心落在 highlight 无信息区间、不是断胶；真断口 → 有信息、没有段") {
  ColorScene sc;
  sc.beadWidth = 20;
  SUBCASE("高光横穿 s ∈ [150, 166]") {
    sc.highlights = {{150, 166}};
    // 示教帧没有高光
    ColorScene teach = sc;
    teach.highlights.clear();
    const ColorRun r = runColor(colorGraph(colorParams(sc)), render(teach), render(sc));
    REQUIRE(r.status == "ok");
    std::size_t blind = 0;
    for (const Json& st : r.runs["stations"]) {
      CAPTURE(st["s"].get<double>());
      if (centralRun(st))
        continue;
      CHECK(centerUnknown(st) == "highlight");
      ++blind;
    }
    CHECK(blind >= 4);
  }
  SUBCASE("断口 s ∈ [200, 240]") {
    sc.breaks = {{200, 240}};
    ColorScene teach = sc;
    teach.breaks.clear();
    const ColorRun r = runColor(colorGraph(colorParams(sc)), render(teach), render(sc));
    REQUIRE(r.status == "ok");
    std::size_t gap = 0;
    for (const Json& st : r.runs["stations"]) {
      const double s = st["s"].get<double>();
      CAPTURE(s);
      if (s >= 205 && s <= 235) {
        CHECK(centralRun(st, 10.0) == nullptr);
        CHECK(centerUnknown(st).empty());
        CHECK(st["infoFraction"].get<double>() >= 0.9);
        ++gap;
      } else if (s < 196 || s > 244) {
        CHECK(centralRun(st) != nullptr);
      }
    }
    CHECK(gap >= 7);
  }
}

// 防：湿胶上沿胶走的高光条比 mergeGap 宽时把一条胶劈成两段（宽度只量到半条）、高光条带一圈没饱和的发白光晕时
// 就不并（现场湿胶的反光点都是这样），以及并段时把真边也当成无信息。
TEST_CASE(
    "glue 彩色：湿胶高光条劈开胶（含没饱和的光晕）→ mergeUnknownGap 并回一段、宽度完整、两边是真边；"
    "不并时两段内侧是 highlight") {
  ColorScene sc;
  sc.beadWidth = 30;
  sc.streakLo = -3;
  sc.streakHi = 4;
  ColorScene teach = sc;
  teach.streakLo = teach.streakHi = NAN;
  ColorParams p = colorParams(sc);
  const cv::Mat frame = render(sc);
  for (const double halo : {0.0, 3.0}) {
    CAPTURE(halo);
    ColorScene wet = sc;
    wet.streakHalo = halo;
    const ColorRun merged = runColor(colorGraph(p), render(teach), render(wet));
    REQUIRE(merged.status == "ok");
    std::vector<double> err;
    for (const Json& st : merged.runs["stations"]) {
      const Json* c = centralRun(st, 4.0);
      REQUIRE(c);
      CHECK((*c)["parts"] == 2);
      CHECK((*c)["loCause"] == "edge");
      CHECK((*c)["hiCause"] == "edge");
      CHECK((*c)["unknownWidth"].get<double>() > 4.0);
      err.push_back(std::fabs((*c)["width"].get<double>() - 30.0));
    }
    CHECK(medianOf(err) <= 1.0);
  }

  p.runs["mergeUnknownGap"] = 0;
  const ColorRun split = runColor(colorGraph(p), render(teach), frame);
  REQUIRE(split.status == "ok");
  const Json& st = split.runs["stations"][20];
  std::vector<const Json*> halves;
  for (const Json& r : st["runs"]) {
    if (std::fabs(r["mid"].get<double>()) <= 15)
      halves.push_back(&r);
  }
  REQUIRE(halves.size() == 2);
  CHECK((*halves[0])["hiCause"] == "highlight");
  CHECK((*halves[1])["loCause"] == "highlight");
}

// 防：发黑铸件的去马赛克假色、中性灰的暗带（阴影、槽、灰度下与粉胶同样暗）被打成胶。
TEST_CASE("glue 彩色：没有胶 —— 只有铸件、或镀锌面上一条中性灰暗带 → 不出段") {
  ColorScene teach;
  teach.substrate = Substrate::Split;
  for (const bool gray : {false, true}) {
    CAPTURE(gray);
    ColorScene sc = teach;
    sc.bead = gray;
    sc.grayBand = gray;
    sc.substrate = gray ? Substrate::Metal : Substrate::Cast;
    sc.seed = 23;
    const ColorRun r = runColor(colorGraph(colorParams(teach)), render(teach), render(sc));
    REQUIRE(r.status == "ok");
    std::size_t runs = 0;
    double widest = 0;
    for (const Json& st : r.runs["stations"]) {
      for (const Json& x : st["runs"]) {
        ++runs;
        widest = std::max(widest, x["width"].get<double>());
      }
    }
    CHECK(runs == 0);
    CHECK(widest == 0.0);
  }
}

// 防：三态混成一态 —— 暗而明显比胶暗的像素当无信息（漏判断胶）、与胶一样亮却看不清颜色的当暗背景（误判断胶）、高光当有信息；
// 以及得分按色度而不是按面积：亮胶压暗底时 25 % 面积的混合像素就过了 0.5，胶宽每边多量出 1 px 多。
TEST_CASE("glue 彩色：像素三态 —— 高光 NaN、暗区按胶亮度 P5 分成 dark（NaN）与 dark_bg（0）；"
          "有信息的像素得分是胶的面积占比，离群颜色压低") {
  const auto feature = [](cv::Vec3d rgb, double* c1, double* c2, double* luma) {
    *c1 = std::log((rgb[0] + 1) / (rgb[1] + 1));
    *c2 = std::log((rgb[2] + 1) / (rgb[1] + 1));
    *luma = 0.299 * rgb[0] + 0.587 * rgb[1] + 0.114 * rgb[2];
  };
  const cv::Vec3d glueRgb(230, 120, 130), castRgb(52, 46, 43);
  glue::ColorModel m;
  m.spec.noiseK = 4;
  m.spec.noiseMax = 0.1; // 亮度 < 40 是暗区
  m.spec.darkRatio = 0.5;
  double luma;
  feature(glueRgb, &m.glue.mean[0], &m.glue.mean[1], &luma);
  m.glue.cov = {0.002, 0, 0.002};
  m.glue.intensity = 480;
  m.glueLuma = {60, 120, 180}; // 暗背景：亮度 < 30
  glue::Gauss2 bg;
  feature(castRgb, &bg.mean[0], &bg.mean[1], &luma);
  bg.cov = {0.004, 0, 0.004};
  bg.intensity = 141;
  m.background = {bg};
  const double g1 = m.glue.mean[0], g2 = m.glue.mean[1];
  glue::ColorState st;
  CHECK(std::isnan(glue::colorScore(m, g1, g2, 120, true, &st)));
  CHECK(st == glue::ColorState::Highlight);
  CHECK(glue::colorScore(m, g1, g2, 20, false, &st) == 0.0);
  CHECK(st == glue::ColorState::DarkBackground);
  CHECK(std::isnan(glue::colorScore(m, g1, g2, 35, false, &st)));
  CHECK(st == glue::ColorState::Dark);
  // 暗而不噪（噪声门限放宽到亮度 < 8）的暗褐背景：比胶的 P5 暗一半以上，照样是暗背景，色度像不像胶都不算
  glue::ColorModel quiet = m;
  quiet.spec.noiseMax = 0.5;
  CHECK(glue::colorScore(quiet, g1, g2, 20, false, &st) == 0.0);
  CHECK(st == glue::ColorState::DarkBackground);
  CHECK(glue::colorScore(m, g1, g2, 120, false, &st) > 0.99);
  CHECK(st == glue::ColorState::Info);
  CHECK(glue::colorScore(m, bg.mean[0], bg.mean[1], 47, false, &st) < 0.01);
  // 胶与铸件按面积 25 % / 50 % / 75 % 混合的像素（线性 RGB 里混）：得分就是面积占比
  for (const double a : {0.25, 0.5, 0.75}) {
    CAPTURE(a);
    double c1, c2;
    feature(glueRgb * a + castRgb * (1 - a), &c1, &c2, &luma);
    CHECK(std::fabs(glue::colorScore(m, c1, c2, luma, false, &st) - a) <= 0.03);
  }
  // 离胶与连线都远的颜色（偏紫）不算胶
  CHECK(glue::colorScore(m, 0.9, 0.9, 120, false, &st) < 0.01);
}

// 防：示教线画偏时模型学进背景（重新居中失效）、模型不确定、累加依赖顺序、JSON 往返丢精度、并行打分随线程数变。
TEST_CASE(
    "glue 彩色：color_model —— 示教线偏 10 px 重新居中、确定性、prior 累加与顺序无关、模型 JSON 往返、"
    "得分图与线程数无关") {
  ColorScene sc;
  sc.beadWidth = 24;
  sc.substrate = Substrate::Split;
  const cv::Mat img = render(sc);
  const auto modelOf = [&](const ColorParams& p, const cv::Mat& teach) {
    const ColorRun r = runColor(colorGraph(p), teach, teach);
    REQUIRE(r.status == "ok");
    return r;
  };
  SUBCASE("示教线偏 10 px：重新居中量回 10 px，胶的均值与示教线画准时一致，量出的胶宽照样准") {
    const ColorRun good = modelOf(colorParams(sc), img);
    ColorParams p = colorParams(sc);
    p.line["points"] = taughtLine(sc, 10);
    p.runs["searchHalf"] = 60;
    const ColorRun shifted = modelOf(p, img);
    CHECK(std::fabs(shifted.model["quality"]["centerShift"].get<double>() - 10.0) <= 1.0);
    CHECK(shifted.model["quality"]["recentered"].get<double>() >= 0.9);
    const auto mean = [](const Json& m, int i) { return m["glue"]["mean"][i].get<double>(); };
    CHECK(std::fabs(mean(shifted.model, 0) - mean(good.model, 0)) <= 0.03);
    CHECK(std::fabs(mean(shifted.model, 1) - mean(good.model, 1)) <= 0.03);
    std::vector<double> err;
    for (const Json& st : shifted.runs["stations"]) {
      const Json* c = centralRun(st, 13.0);
      REQUIRE(c);
      err.push_back(std::fabs((*c)["width"].get<double>() - 24.0));
    }
    CHECK(medianOf(err) <= 1.0);
    // 不居中时胶样本一半是背景：均值明显偏
    p.model["recenterRounds"] = 0;
    const ColorRun raw = modelOf(p, img);
    CHECK(std::fabs(mean(raw.model, 0) - mean(good.model, 0)) > 0.1);
  }
  SUBCASE("同输入两次逐位相同；A 接 prior B 与 B 接 prior A 拟合出同一个模型") {
    const ColorRun a1 = modelOf(colorParams(sc), img);
    const ColorRun a2 = modelOf(colorParams(sc), img);
    CHECK(a1.model.dump() == a2.model.dump());
    ColorScene other = sc;
    other.seed = 99;
    other.illumLo = 0.5;
    const cv::Mat img2 = render(other);
    const ColorRun b = modelOf(colorParams(sc), img2);
    const auto withPrior = [&](const Json& prior, const cv::Mat& teach) {
      OpCall line;
      REQUIRE(line.run("glue.polyline", {{"points", Value::text(taughtLine(sc))}}).ok);
      Record rec;
      rec.type = glue::kColorModelType;
      rec.data = prior;
      OpCall call;
      call.inputs["image"] = Data::image(imageOf(teach));
      call.inputs["line"] = line.out("line");
      call.inputs["prior"] = Data::record(rec);
      REQUIRE(call.run("glue.color_model",
                       {{"glueHalf", Value::number(4)}, {"bgBand", Value::vec({20, 40})}})
                  .ok);
      return recordOf(call.out("model"));
    };
    const Json ab = withPrior(b.model, img);
    const Json ba = withPrior(a1.model, img2);
    CHECK(ab["stats"]["frames"] == 2);
    for (const char* k : {"glue", "background", "stats"})
      CHECK(ab[k].dump() == ba[k].dump());
  }
  SUBCASE(
      "模型 JSON 填进 color_map 的 model 参数，得分图与接端口逐位相同；roi 那一块与整幅逐位相同") {
    const ColorRun ported = modelOf(colorParams(sc), img);
    ColorParams p = colorParams(sc);
    // 生产用的精简模型：只留 spec 与高斯（stats、train、quality 都去掉），打分照样逐位相同
    Json compact = ported.model;
    for (const char* k : {"stats", "train", "quality"})
      compact.erase(k);
    p.map["model"] = compact.dump();
    p.map["roi"] = {100, 80, 150, 120};
    Json doc = makeGraph({N{"n_load", "io.load_image", Json{{"source", "inputs"}}},
                          N{"n_map", "glue.color_map", p.map}},
                         {E{"n_load.image", "n_map.image"}});
    Session s(doc, {}, {}, false, 0, false,
              {exec::InjectedInput{"n_load", "image", Data::image(imageOf(img))}});
    REQUIRE(s.wait().runStatus() == "ok");
    Data d;
    REQUIRE(exec::ResultStore::instance().get(s.runId(), "n_map", "maps", d));
    const Bundle* b = d.asBundle();
    REQUIRE(b);
    const cv::Mat score = cvx::view(*b->field("score")->asImage());
    const cv::Mat state = cvx::view(*b->field("state")->asImage());
    REQUIRE(score.size() == cv::Size(150, 120));
    int diffs = 0;
    for (int y = 0; y < 120; ++y) {
      for (int x = 0; x < 150; ++x) {
        const float a = score.at<float>(y, x), w = ported.score.at<float>(y + 80, x + 100);
        diffs += !(a == w || (std::isnan(a) && std::isnan(w)));
        diffs += state.at<float>(y, x) != ported.state.at<float>(y + 80, x + 100);
      }
    }
    CHECK(diffs == 0);
    // 行按线程分块并行：结果与线程数无关
    glue::ColorModel m;
    REQUIRE(glue::readColorModel(ported.model, &m, nullptr));
    const glue::ColorMaps one = glue::colorMaps(img, m, cv::Rect(), 1);
    const glue::ColorMaps many = glue::colorMaps(img, m, cv::Rect(), 5);
    CHECK(std::memcmp(one.score.data, many.score.data, one.score.total() * sizeof(float)) == 0);
    CHECK(cv::norm(one.state, many.state, cv::NORM_INF) == 0);
  }
}

// 防：颜色模型的坏输入被静默用掉（宿主配方里存坏了要在加载期报出来）、灰度图被当彩色量、roi 填错。
TEST_CASE(
    "glue 彩色：坏 model → bad_param 指到 model；灰度图 → bad_input 指到 image；roi 不相交 → bad_param；"
    "随包参考图校验干净") {
  const auto mapGraph = [](const Json& params) {
    return makeGraph({N{"n_load", "io.load_image", Json{{"source", "inputs"}}},
                      N{"n_map", "glue.color_map", params}},
                     {E{"n_load.image", "n_map.image"}});
  };
  const std::vector<std::string> model{"model"};
  CHECK(errorsOf(mapGraph(Json::object())) == model);
  CHECK(errorsOf(mapGraph(Json{{"model", "{bad"}})) == model);
  CHECK(errorsOf(mapGraph(Json{{"model", "{}"}})) == model);

  ColorScene sc;
  const cv::Mat img = render(sc);
  const ColorRun r = runColor(colorGraph(colorParams(sc)), img, img);
  REQUIRE(r.status == "ok");
  Json broken = r.model;
  broken["glue"]["cov"] = {{1, 2}, {2, 1}}; // 不正定
  CHECK(errorsOf(mapGraph(Json{{"model", broken.dump()}})) == model);
  broken = r.model;
  broken["background"] = Json::array();
  CHECK(errorsOf(mapGraph(Json{{"model", broken.dump()}})) == model);
  CHECK(errorsOf(mapGraph(Json{{"model", r.model.dump()}})).empty());
  // 精简模型（不带 stats）能打分，不能当 prior 累加
  Json compact = r.model;
  compact.erase("stats");
  CHECK(errorsOf(mapGraph(Json{{"model", compact.dump()}})).empty());
  {
    OpCall line;
    REQUIRE(line.run("glue.polyline", {{"points", Value::text(taughtLine(sc))}}).ok);
    Record prior;
    prior.type = glue::kColorModelType;
    prior.data = compact;
    OpCall call;
    call.inputs["image"] = Data::image(imageOf(img));
    call.inputs["line"] = line.out("line");
    call.inputs["prior"] = Data::record(prior);
    const Status st = call.run("glue.color_model");
    CHECK_FALSE(st.ok);
    CHECK(st.code == "bad_input");
    CHECK(st.portName == "prior");
  }
  CHECK(errorsOf(mapGraph(Json{{"model", r.model.dump()}, {"roi", {0, 0, 10, 0}}})) ==
        std::vector<std::string>{"roi"});

  const auto runMap = [&](const Json& params, const cv::Mat& frame) {
    Session s(mapGraph(params), {}, {}, false, 0, false,
              {exec::InjectedInput{"n_load", "image", Data::image(imageOf(frame))}});
    RunLog& log = s.wait();
    return log.nodeEvent("n_map", "error");
  };
  cv::Mat gray;
  cv::cvtColor(img, gray, cv::COLOR_RGB2GRAY);
  const Json grayErr = runMap(Json{{"model", r.model.dump()}}, gray);
  CHECK(grayErr["error"]["code"] == "bad_input");
  CHECK(grayErr["error"]["portName"] == "image");
  const Json roiErr = runMap(Json{{"model", r.model.dump()}, {"roi", {5000, 5000, 10, 10}}}, img);
  CHECK(roiErr["error"]["code"] == "bad_param");

  std::ifstream in(std::filesystem::path(__FILE__).parent_path().parent_path() /
                   "graphs/color.lyflow.json");
  REQUIRE(in.good());
  CHECK(errorsOf(Json::parse(in)).empty());
}

// 防：彩色跟踪把高光挡住的几步算进 maxGap（跟到一半停下、后半段胶路外推跑偏），覆盖率把无信息当没找到；
// 以及湿胶中间一条反光带把胶劈成两半时只跟其中一半，胶点偏出半个胶宽、拟合胶路漂到胶边上（现场 cam3 s169 / s176）。
TEST_CASE("glue 彩色：color_fan → color_track 跟圆弧胶 —— 高光横穿两步：coverageInformative ≥ 0.9、"
          "无信息步不计 maxGap；中间一条反光带：胶点取合并段中点、不跟半条；拟合胶路贴着真胶") {
  struct Tracked {
    Json track, line;
  };
  const auto runTrack = [](const ColorScene& sc, const ColorScene& teach, const Json& extra) {
    ColorParams p = colorParams(teach);
    p.line["points"] = taughtLine(teach);
    const Json anchor = {sc.start.x, sc.start.y};
    Json track = {{"anchor", anchor}, {"zone", {40, 330}}, {"bearingRange", {-130, 0}},
                  {"maxGap", 12},     {"widthMax", 40},    {"searchHalf", 40}};
    track.update(extra);
    Json doc =
        makeGraph({N{"n_teach", "io.load_image", Json{{"source", "inputs"}}},
                   N{"n_load", "io.load_image", Json{{"source", "inputs"}}},
                   N{"n_line", "glue.polyline", p.line}, N{"n_model", "glue.color_model", p.model},
                   N{"n_map", "glue.color_map", Json::object()},
                   N{"n_fan", "glue.color_fan",
                     Json{{"anchor", anchor}, {"sector", {-130, 0}}, {"radial", {40, 120}}}},
                   N{"n_track", "glue.color_track", track}},
                  {E{"n_teach.image", "n_model.image"}, E{"n_line.line", "n_model.line"},
                   E{"n_load.image", "n_map.image"}, E{"n_model.model", "n_map.model"},
                   E{"n_map.maps", "n_fan.maps"}, E{"n_map.maps", "n_track.maps"},
                   E{"n_fan.scan", "n_track.fan"}});
    Session s(doc, {}, {}, false, 0, false,
              {exec::InjectedInput{"n_teach", "image", Data::image(imageOf(render(teach)))},
               exec::InjectedInput{"n_load", "image", Data::image(imageOf(render(sc)))}});
    REQUIRE(s.wait().runStatus() == "ok");
    Tracked t;
    Data d;
    REQUIRE(exec::ResultStore::instance().get(s.runId(), "n_track", "track", d));
    t.track = recordOf(d);
    REQUIRE(exec::ResultStore::instance().get(s.runId(), "n_track", "line", d));
    t.line = recordOf(d);
    return t;
  };
  const auto lineOffset = [](const ColorScene& sc, const Json& line, bool max) {
    std::vector<double> dist;
    for (const Json& q : line["points"]) {
      double along, across;
      coords(sc, q[0].get<double>(), q[1].get<double>(), &along, &across);
      dist.push_back(std::fabs(across));
    }
    return max ? *std::max_element(dist.begin(), dist.end()) : medianOf(dist);
  };
  ColorScene sc;
  sc.curvature = 1.0 / 300.0;
  sc.length = 360;
  SUBCASE("高光横穿 s ∈ [196, 214]") {
    sc.beadWidth = 18;
    sc.highlights = {{196, 214}};
    ColorScene teach = sc;
    teach.highlights.clear();
    const Tracked t = runTrack(sc, teach, Json::object());
    CHECK(t.track["valid"] == true);
    CHECK(t.track["fitted"] == true);
    CHECK(t.track["unknownSteps"].get<int>() >= 1);
    CHECK(t.track["stoppedAt"].is_null());
    CHECK(t.track["coverageInformative"].get<double>() >= 0.9);
    CHECK(lineOffset(sc, t.line, false) <= 1.5);
  }
  SUBCASE("湿胶中间一条反光带（芯饱和、两侧各 3 px 没饱和的发白光晕）") {
    sc.beadWidth = 30;
    sc.streakLo = -3;
    sc.streakHi = 4;
    sc.streakHalo = 3;
    ColorScene teach = sc;
    teach.streakLo = teach.streakHi = NAN;
    const auto halves = [](const Json& track) {
      std::size_t narrow = 0;
      for (const Json& q : track["points"])
        narrow += q["width"].get<double>() < 0.7 * 30 ? 1 : 0;
      return static_cast<double>(narrow) / std::max<std::size_t>(track["points"].size(), 1);
    };
    const Tracked t = runTrack(sc, teach, Json::object());
    CHECK(t.track["coverageInformative"].get<double>() >= 0.9);
    CHECK(halves(t.track) <= 0.1);
    CHECK(lineOffset(sc, t.line, false) <= 1.5);
    CHECK(lineOffset(sc, t.line, true) <= 4.0);
    // 不合并时胶点大多是半条胶（这就是现场漂到胶边上的起因）
    const Tracked split = runTrack(sc, teach, Json{{"mergeUnknownGap", 0}});
    CHECK(halves(split.track) > 0.5);
  }
}

TEST_CASE("glue 彩色：有相邻完整胶宽的长低分内谷恢复整胶，真背景与主胶旁弱条不并入") {
  struct Case {
    const char* name;
    double gap;
    float valley;
    bool sideBand;
    bool recover;
  };
  const Case cases[] = {{"17 px 有限低分内谷", 17, 0.25F, false, true},
                        {"21 px 有限低分内谷", 21, 0.25F, false, true},
                        {"4 px 真实背景", 4, 0.0F, false, false},
                        {"21 px 真实背景", 21, 0.0F, false, false},
                        {"正常主胶旁的远侧弱色条", 18, 0.25F, true, true},
                        {"正常主胶旁的近侧弱色条", 9, 0.25F, true, true}};
  const Json doc =
      makeGraph({N{"n_line", "glue.polyline", Json{{"points", "[[100,220],[100,20]]"}}},
                 N{"n_runs", "glue.color_runs",
                   Json{{"searchHalf", 80},
                        {"sRange", {20, 180}},
                        {"stationStep", 4},
                        {"split", "half_max"},
                        {"mergeUnknownGap", 16},
                        {"mergeWidthMax", 90}}}},
                {E{"n_line.line", "n_runs.line"}});
  for (const auto& c : cases) {
    CAPTURE(c.name);
    const Json runs =
        runColorMaps(doc, splitBeadMaps(c.gap, c.valley, c.sideBand), "n_runs", "runs");
    REQUIRE_FALSE(runs["stations"].empty());
    for (const Json& station : runs["stations"]) {
      const double along = station["s"].get<double>();
      CAPTURE(along);
      const Json* central = centralRun(station, 2.0);
      if (!c.recover && along >= 84 && along <= 136) {
        CHECK(central == nullptr);
        CHECK(station["runs"].size() == 2);
        CHECK(station["unknownSpans"].empty());
      } else if (c.recover || along < 76 || along > 144) {
        REQUIRE(central);
        CHECK(std::fabs((*central)["width"].get<double>() - 48.0) <= 1.0);
        CHECK((*central)["loComplete"] == true);
        CHECK((*central)["hiComplete"] == true);
      }
    }
  }
}

TEST_CASE("glue 彩色：稳定整胶进入长低分内谷后，跟踪保持胶中心而不吸附左侧半条胶") {
  const Json doc = makeGraph({N{"n_track", "glue.color_track",
                                Json{{"anchor", {100, 220}},
                                     {"heading", -90},
                                     {"zone", {20, 180}},
                                     {"searchHalf", 80},
                                     {"widthMax", 90},
                                     {"split", "half_max"},
                                     {"mergeUnknownGap", 16},
                                     {"mergeWidthMax", 90}}}},
                             {});
  const Json track =
      runColorMaps(doc, splitBeadMaps(21, 0.25F, false, 88, 200), "n_track", "track");
  CHECK(track["fitted"] == true);
  CHECK(track["stoppedAt"].is_null());
  CHECK(track["coverageInformative"].get<double>() >= 0.95);
  REQUIRE_FALSE(track["points"].empty());
  for (const Json& point : track["points"]) {
    CAPTURE(point["s"].get<double>());
    CHECK(std::fabs(point["point"][0].get<double>() - 100.0) <= 2.0);
    CHECK(std::fabs(point["width"].get<double>() - 48.0) <= 1.0);
  }
}

TEST_CASE("glue 彩色 extent：宽胶与局部鼓包超过旧上限仍量完整，旧策略仍拒绝超宽") {
  for (bool bulge : {false, true}) {
    glue::ColorMaps maps;
    maps.frame = cv::Rect(0, 0, 201, 241);
    maps.score = cv::Mat(241, 201, CV_32F);
    maps.state = cv::Mat::zeros(241, 201, CV_32F);
    maps.luma = cv::Mat(241, 201, CV_32F, cv::Scalar(140));
    for (int y = 0; y < maps.score.rows; ++y) {
      const double half = bulge && (y < 80 || y > 160) ? 12 : 24;
      for (int x = 0; x < maps.score.cols; ++x)
        maps.score.at<float>(y, x) =
            static_cast<float>(std::clamp(half + 0.5 - std::abs(x - 100), 0.0, 1.0));
    }
    const Data input = glue::colorMapsBundle(std::move(maps), cv::Size(201, 241));
    for (const char* policy : {"range", "extent"}) {
      CAPTURE(bulge);
      CAPTURE(policy);
      const Json doc = makeGraph({N{"n_track", "glue.color_track",
                                    Json{{"anchor", {100, 220}},
                                         {"heading", -90},
                                         {"zone", {20, 180}},
                                         {"searchHalf", 80},
                                         {"widthMax", 30},
                                         {"mergeWidthMax", 30},
                                         {"widthPolicy", policy}}}},
                                 {});
      const Json track = runColorMaps(doc, input, "n_track", "track");
      if (std::string(policy) == "extent") {
        CHECK(track["coverage"].get<double>() >= 0.95);
        CHECK(track["incompleteS"].empty());
        CHECK(track["ambiguousS"].empty());
        bool wide = false;
        for (const Json& point : track["points"]) {
          CHECK(std::fabs(point["point"][0].get<double>() - 100) <= 1);
          wide |= point["width"].get<double>() > 40;
        }
        CHECK(wide);
      } else {
        CHECK(track["coverage"].get<double>() < 0.8);
        for (const Json& point : track["points"])
          CHECK(point["width"].get<double>() <= 30);
      }
    }
  }
}

TEST_CASE("glue 彩色 extent：反光裂分合成超预期宽胶，真实背景双胶不合并也不任意选边") {
  for (bool background : {false, true}) {
    const Json params = {{"anchor", {100, 220}},   {"heading", -90}, {"zone", {20, 180}},
                         {"searchHalf", 80},       {"widthMax", 30}, {"mergeWidthMax", 30},
                         {"widthPolicy", "extent"}};
    const Data input = splitBeadMaps(4, background ? 0.0F : 0.25F, false, 0, 240);
    const Json track = runColorMaps(makeGraph({N{"n_track", "glue.color_track", params}}, {}),
                                    input, "n_track", "track");
    const Json runsDoc =
        makeGraph({N{"n_line", "glue.polyline", Json{{"points", "[[100,220],[100,20]]"}}},
                   N{"n_runs", "glue.color_runs",
                     Json{{"searchHalf", 80},
                          {"sRange", {20, 180}},
                          {"widthPolicy", "extent"},
                          {"mergeWidthMax", 30}}}},
                  {E{"n_line.line", "n_runs.line"}});
    const Json runs = runColorMaps(runsDoc, input, "n_runs", "runs");
    if (background) {
      CHECK(track["points"].empty());
      CHECK_FALSE(track["ambiguousS"].empty());
      for (const Json& station : runs["stations"])
        CHECK(station["runs"].size() == 2);
    } else {
      CHECK(track["coverage"].get<double>() >= 0.95);
      for (const Json& station : runs["stations"]) {
        REQUIRE(station["runs"].size() == 1);
        CHECK(station["runs"][0]["width"].get<double>() > 45);
      }
    }
  }
}

TEST_CASE("glue 彩色 extent：搜索、ROI、图像截断保留原因；只有贴画面边、另一侧是真边的才出跟踪点") {
  for (const std::string cause : {"caliper", "roi", "image"}) {
    CAPTURE(cause);
    glue::ColorMaps maps;
    maps.frame = cv::Rect(0, 0, (cause == "roi" || cause == "image") ? 116 : 201, 241);
    maps.score = cv::Mat(maps.frame.size(), CV_32F);
    maps.state = cv::Mat::zeros(maps.frame.size(), CV_32F);
    maps.luma = cv::Mat(maps.frame.size(), CV_32F, cv::Scalar(140));
    for (int y = 0; y < maps.score.rows; ++y) {
      for (int x = 0; x < maps.score.cols; ++x) {
        maps.score.at<float>(y, x) =
            static_cast<float>(std::clamp(24.5 - std::abs(x - 100), 0.0, 1.0));
      }
    }
    const Data input =
        glue::colorMapsBundle(std::move(maps), cv::Size(cause == "image" ? 116 : 201, 241));
    const double half = cause == "caliper" ? 16 : 80;
    const Json runsDoc =
        makeGraph({N{"n_line", "glue.polyline", Json{{"points", "[[100,220],[100,20]]"}}},
                   N{"n_runs", "glue.color_runs",
                     Json{{"searchHalf", half},
                          {"sRange", {20, 180}},
                          {"widthPolicy", "extent"},
                          {"mergeWidthMax", 30}}}},
                  {E{"n_line.line", "n_runs.line"}});
    const Json runs = runColorMaps(runsDoc, input, "n_runs", "runs");
    for (const Json& station : runs["stations"]) {
      REQUIRE(station["runs"].size() == 1);
      const Json& run = station["runs"][0];
      CHECK(run["hiComplete"] == false);
      CHECK(run["hiCause"] == cause);
      CHECK(run["hi"].get<double>() == doctest::Approx(cause == "caliper" ? 16.0 : 14.5));
    }
    const Json trackDoc = makeGraph({N{"n_track", "glue.color_track",
                                       Json{{"anchor", {100, 220}},
                                            {"heading", -90},
                                            {"zone", {20, 180}},
                                            {"widthPolicy", "extent"},
                                            {"searchHalf", half},
                                            {"widthMax", 30}}}},
                                    {});
    const Json track = runColorMaps(trackDoc, input, "n_track", "track");
    if (cause == "caliper") {
      CHECK(track["points"].empty());
      CHECK_FALSE(track["incompleteS"].empty());
      CHECK(track["unknownSteps"] == track["steps"]);
    } else {
      CHECK(track["coverage"].get<double>() >= 0.95);
      CHECK(track["incompleteS"].empty());
      for (const Json& point : track["points"])
        CHECK(point["point"][0].get<double>() == doctest::Approx(95.5).epsilon(0.02));
    }
  }
}

TEST_CASE("glue 彩色 extent：胶边外侧是饱和背景时认作真边，接胶色或只剩半宽仍不完整") {
  // 胶宽 49 px（x 76–124）。beyond：高光之后是什么 —— background 明确背景、caliper 一直到卡尺端点、
  // roi 走出得分图（卡尺按邻站参考宽认边，跟踪没有参考宽、高光窄于胶宽时不认）、glue 隔着窄于胶的高光又接上一段胶色、
  // far 隔着宽过胶的高光才在卡尺端点见到一点胶色（另一处胶）、sliver 高光与明确背景之间夹一条 3 px 的过渡色；
  // half：中间 40 行高光盖住胶的右半，邻站完整
  for (const std::string beyond :
       {"background", "caliper", "roi", "glue", "far", "sliver", "half"}) {
    CAPTURE(beyond);
    const int width = beyond == "roi" ? 150 : 201;
    glue::ColorMaps maps;
    maps.frame = cv::Rect(0, 0, width, 241);
    maps.score = cv::Mat(maps.frame.size(), CV_32F);
    maps.state = cv::Mat::zeros(maps.frame.size(), CV_32F);
    maps.luma = cv::Mat(maps.frame.size(), CV_32F, cv::Scalar(140));
    const auto halfRow = [&](int y) { return beyond == "half" && y >= 100 && y < 140; };
    for (int y = 0; y < maps.score.rows; ++y) {
      const int from = halfRow(y) ? 100 : 125;
      const int to = beyond == "background" || beyond == "half" ? 160
                     : beyond == "glue"                         ? 165
                     : beyond == "far"                          ? 176
                     : beyond == "sliver"                       ? 150
                                                                : width;
      for (int x = 0; x < maps.score.cols; ++x) {
        const bool lit = beyond == "half" ? halfRow(y) && x >= from && x < to : x >= from && x < to;
        const bool tail = (beyond == "glue" && x >= 165 && x < 180) ||
                          (beyond == "far" && x >= 176 && x < 179) ||
                          (beyond == "sliver" && x >= 150 && x < 153);
        maps.score.at<float>(y, x) =
            lit    ? NAN
            : tail ? 1.0F
                   : static_cast<float>(std::clamp(24.5 - std::abs(x - 100), 0.0, 1.0));
        maps.state.at<float>(y, x) = lit ? 1.0F : 0.0F;
      }
    }
    const Data input = glue::colorMapsBundle(std::move(maps), cv::Size(201, 241));
    const Json runsDoc =
        makeGraph({N{"n_line", "glue.polyline", Json{{"points", "[[100,220],[100,20]]"}}},
                   N{"n_runs", "glue.color_runs",
                     Json{{"searchHalf", 80},
                          {"sRange", {20, 180}},
                          {"widthPolicy", "extent"},
                          {"mergeWidthMax", 30}}}},
                  {E{"n_line.line", "n_runs.line"}});
    const Json runs = runColorMaps(runsDoc, input, "n_runs", "runs");
    for (const Json& station : runs["stations"]) {
      const double y = station["center"][1].get<double>();
      REQUIRE(station["runs"].size() ==
              (beyond == "glue" || beyond == "far" || beyond == "sliver" ? 2 : 1));
      const Json& run = station["runs"][0];
      CAPTURE(y);
      CHECK(run["loComplete"] == true);
      if (beyond == "glue" || (beyond == "half" && y > 102 && y < 138)) {
        CHECK(run["hiComplete"] == false);
        CHECK(run["hiCause"] == "highlight");
      } else if (beyond != "half") {
        CHECK(run["hiComplete"] == true);
        CHECK(run["hiBackground"] == "highlight");
        CHECK(run["width"].get<double>() == doctest::Approx(48.0).epsilon(0.03));
      }
    }
    const Json trackDoc = makeGraph({N{"n_track", "glue.color_track",
                                       Json{{"anchor", {100, 220}},
                                            {"heading", -90},
                                            {"zone", {20, 180}},
                                            {"widthPolicy", "extent"},
                                            {"searchHalf", 80},
                                            {"widthMax", 30}}}},
                                    {});
    const Json track = runColorMaps(trackDoc, input, "n_track", "track");
    if (beyond == "roi" || beyond == "glue") {
      CHECK(track["points"].empty());
    } else {
      CHECK(track["coverage"].get<double>() >= (beyond == "half" ? 0.7 : 0.95));
      for (const Json& point : track["points"])
        CHECK(std::fabs(point["point"][0].get<double>() - 100) <= 1);
    }
    if (beyond == "half")
      CHECK_FALSE(track["incompleteS"].empty());
  }
}

TEST_CASE("glue 彩色 extent：宽胶中间的反光带宽过合并上限仍并成一条，缝比两边都宽的两段不并") {
  // 两段胶各宽 w、中间一条全无信息的缝 g（x 从 60 起），外侧是明确背景
  for (const auto& [w, g, merged] : {std::tuple{30, 20, true}, std::tuple{15, 24, false}}) {
    CAPTURE(w);
    CAPTURE(g);
    glue::ColorMaps maps;
    maps.frame = cv::Rect(0, 0, 201, 241);
    maps.score = cv::Mat(maps.frame.size(), CV_32F, cv::Scalar(0));
    maps.state = cv::Mat::zeros(maps.frame.size(), CV_32F);
    maps.luma = cv::Mat(maps.frame.size(), CV_32F, cv::Scalar(140));
    for (int y = 0; y < maps.score.rows; ++y) {
      for (int x = 60; x < 60 + 2 * w + g; ++x) {
        const bool stripe = x >= 60 + w && x < 60 + w + g;
        maps.score.at<float>(y, x) = stripe ? NAN : 1.0F;
        maps.state.at<float>(y, x) = stripe ? 1.0F : 0.0F;
      }
    }
    const double centre = 60 + w + 0.5 * g;
    const Data input = glue::colorMapsBundle(std::move(maps), cv::Size(201, 241));
    const std::string line =
        "[[" + std::to_string(centre) + ",220],[" + std::to_string(centre) + ",20]]";
    const Json runsDoc = makeGraph(
        {N{"n_line", "glue.polyline", Json{{"points", line}}}, N{"n_runs", "glue.color_runs",
                                                                 Json{{"searchHalf", 80},
                                                                      {"sRange", {20, 180}},
                                                                      {"widthPolicy", "extent"},
                                                                      {"mergeWidthMax", 30}}}},
        {E{"n_line.line", "n_runs.line"}});
    const Json runs = runColorMaps(runsDoc, input, "n_runs", "runs");
    for (const Json& station : runs["stations"]) {
      REQUIRE(station["runs"].size() == (merged ? 1 : 2));
      if (merged) {
        CHECK(station["runs"][0]["loComplete"] == true);
        CHECK(station["runs"][0]["hiComplete"] == true);
        CHECK(station["runs"][0]["width"].get<double>() ==
              doctest::Approx(2 * w + g).epsilon(0.05));
      }
    }
    const Json track = runColorMaps(makeGraph({N{"n_track", "glue.color_track",
                                                 Json{{"anchor", {centre, 220}},
                                                      {"heading", -90},
                                                      {"zone", {20, 180}},
                                                      {"widthPolicy", "extent"},
                                                      {"searchHalf", 80},
                                                      {"widthMax", 30}}}},
                                              {}),
                                    input, "n_track", "track");
    if (merged) {
      CHECK(track["coverage"].get<double>() >= 0.95);
      for (const Json& point : track["points"])
        CHECK(std::fabs(point["point"][0].get<double>() - centre) <= 1.5);
    } else {
      CHECK(track["points"].empty());
    }
  }
}

TEST_CASE("glue 彩色 extent：局部鼓包比邻站宽、中间被反光或浅色带劈开，仍并成一条完整胶") {
  // 胶宽 48 px（x 76–124），中间 40 行鼓到 70 px（x 60–130），鼓包里 x 92–100 是一条反光（无信息）或浅色（低分）带
  for (const bool blind : {true, false}) {
    CAPTURE(blind);
    glue::ColorMaps maps;
    maps.frame = cv::Rect(0, 0, 201, 241);
    maps.score = cv::Mat(maps.frame.size(), CV_32F, cv::Scalar(0));
    maps.state = cv::Mat::zeros(maps.frame.size(), CV_32F);
    maps.luma = cv::Mat(maps.frame.size(), CV_32F, cv::Scalar(140));
    for (int y = 0; y < maps.score.rows; ++y) {
      const bool bulge = y >= 100 && y < 140;
      for (int x = bulge ? 60 : 76; x < (bulge ? 130 : 124); ++x) {
        const bool band = bulge && x >= 92 && x < 100;
        maps.score.at<float>(y, x) = band ? (blind ? NAN : 0.2F) : 1.0F;
        maps.state.at<float>(y, x) = band && blind ? 1.0F : 0.0F;
      }
    }
    const Data input = glue::colorMapsBundle(std::move(maps), cv::Size(201, 241));
    const Json runs = runColorMaps(
        makeGraph({N{"n_line", "glue.polyline", Json{{"points", "[[100,220],[100,20]]"}}},
                   N{"n_runs", "glue.color_runs",
                     Json{{"searchHalf", 80},
                          {"sRange", {20, 180}},
                          {"widthPolicy", "extent"},
                          {"mergeWidthMax", 30}}}},
                  {E{"n_line.line", "n_runs.line"}}),
        input, "n_runs", "runs");
    int bulges = 0;
    for (const Json& station : runs["stations"]) {
      const double y = station["center"][1].get<double>();
      if (y < 104 || y > 136)
        continue;
      ++bulges;
      CAPTURE(y);
      REQUIRE(station["runs"].size() == 1);
      CHECK(station["runs"][0]["loComplete"] == true);
      CHECK(station["runs"][0]["hiComplete"] == true);
      CHECK(station["runs"][0]["width"].get<double>() == doctest::Approx(70.0).epsilon(0.05));
    }
    CHECK(bulges >= 8);
  }
}
