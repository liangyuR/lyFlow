// glue 包（glue-plan G2，§4 第 13 条）：合成图上的胶宽、边距、断口，外加失败语义、标定、加载期校验与人造断胶。
// 图一律现生成（D12：真实帧不进仓库；合成图才有真值）：渐变背景上一条已知宽度的暗胶，一侧有 亮 → 暗 的零件边，
// 喷嘴处一块暗影，按 4×4 超采样画、再模糊、再加高斯噪声。真实帧的评估走 lyflow eval（packs/glue/tools/）。
#include <doctest/doctest.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <opencv2/imgproc.hpp>

#include "algo/bead.h"
#include "exec/executor.h"
#include "exec/result_store.h"
#include "helpers.h"
#include "lyflow/registry.h"
#include "lyflow_cv/adapter.h"
#include "ops/glue.h"

namespace {

using namespace lyflow;
using namespace lyflow::test;
using lyflow::packs::glue::P2;
namespace glue = lyflow::packs::glue;

constexpr double kPi = 3.14159265358979323846;

// ------------------------------------------------------------------ 合成场景

/// 胶从喷嘴沿 heading 方向直着走；along / across 是相对喷嘴、沿胶路 / 沿法向（右侧为正）的坐标。
struct Scene {
  int width = 640;
  int height = 560;
  P2 nozzle{150, 500};
  double headingDeg = -60;
  bool swirl = false;
  double beadWidth = 20;  ///< 直胶是胶宽；螺旋胶是外包络的宽度
  double rope = 0;        ///< 螺旋胶：一股胶沿法向的宽度
  double amp = 0;         ///< 螺旋胶：这股胶左右摆的幅度（外包络 = 2·amp + rope）
  double pitch = 24;
  double phase = 2;       ///< 摆动的极值落在 s ≡ 0 (mod 4) 上，与站对齐
  double edgeGap = 35;    ///< 胶的近边 → 零件边
  int partSide = +1;      ///< 零件边在胶的哪一侧（+1 = 法向那一侧）
  std::vector<std::pair<double, double>> breaks;  ///< 沿胶路的断口 [a, b]
  bool bead = true;
  bool dent = false;      ///< 没有胶，只有一条边缘很缓的暗带（零件上的压痕）
  double noise = 2;       ///< 高斯噪声的 σ（灰度级）；真实帧的翻边是过曝的 255，胶上的 JPEG 噪声约这么大
  double blur = 1.0;
  unsigned seed = 7;
};

/// 螺旋胶用「沿法向左右摆的一股胶」近似：一股宽 rope、摆幅 amp、螺距 24 px，外包络 W = 2·amp + rope。
/// 摆幅不超过螺距的 0.2 倍（胶边最陡约 51°，与真实螺旋胶的边缘陡度相当）；直胶模式量这种胶只会量到
/// 一股的宽度（比外包络窄 2·amp），螺旋胶模式要量出外包络。
Scene swirlScene(double W) {
  Scene s;
  s.swirl = true;
  s.beadWidth = W;
  if (W <= 8) {
    s.rope = 5, s.amp = 1.5;
  } else if (W <= 20) {
    s.rope = 12, s.amp = 4;
  } else {
    s.amp = 4.8;
    s.rope = W - 2 * s.amp;
  }
  s.partSide = -1;  // 螺旋胶的零件边放在另一侧：「自动判侧」两侧都要验到
  return s;
}

double sceneValue(const Scene& sc, double x, double y) {
  const P2 u = glue::dirOfDeg(sc.headingDeg);
  const P2 v = glue::perp(u);
  const P2 d(x - sc.nozzle.x, y - sc.nozzle.y);
  const double along = glue::dot(d, u);
  const double across = glue::dot(d, v);
  if (d.x * d.x + d.y * d.y <= 35.0 * 35.0) return 30.0;  // 喷嘴的阴影
  const double flange = 190.0 + 0.06 * (x - 320.0) - 0.04 * (y - 280.0);
  const double outside = 45.0 + 0.02 * (x - 320.0);
  const double edgeAt = 0.5 * sc.beadWidth + sc.edgeGap;
  double value = sc.partSide * across < edgeAt ? flange : outside;
  if (sc.dent) {
    // 压痕 / 阴影：一条高斯剖面（σ = 20 px，半深宽约 47 px）的暗带，暗 70 个灰度，边缘是缓的
    value -= 70.0 * std::exp(-0.5 * across * across / 400.0);
  }
  if (!sc.bead || along < 0 || along > 470) return value;
  for (const auto& [a, b] : sc.breaks) {
    if (along >= a && along <= b) return value;
  }
  bool inBead;
  if (sc.swirl) {
    const double o = sc.amp * std::sin(2 * kPi * (along - sc.phase) / sc.pitch);
    inBead = std::fabs(across - o) <= 0.5 * sc.rope;
  } else {
    inBead = std::fabs(across) <= 0.5 * sc.beadWidth;
  }
  return inBead ? 70.0 : value;
}

cv::Mat render(const Scene& sc) {
  cv::Mat img(sc.height, sc.width, CV_32F);
  constexpr int kSub = 4;
  for (int y = 0; y < sc.height; ++y) {
    for (int x = 0; x < sc.width; ++x) {
      double acc = 0;
      for (int j = 0; j < kSub; ++j) {
        for (int i = 0; i < kSub; ++i) {
          acc += sceneValue(sc, x - 0.5 + (i + 0.5) / kSub, y - 0.5 + (j + 0.5) / kSub);
        }
      }
      img.at<float>(y, x) = static_cast<float>(acc / (kSub * kSub));
    }
  }
  cv::GaussianBlur(img, img, cv::Size(0, 0), sc.blur);
  cv::Mat noise(img.size(), CV_32F);
  cv::RNG rng(sc.seed);
  rng.fill(noise, cv::RNG::NORMAL, 0.0, sc.noise);
  img += noise;
  cv::Mat out;
  img.convertTo(out, CV_8U);
  return out;
}

Image imageOf(const cv::Mat& gray) {
  Image img;
  REQUIRE(cvx::fromMat(gray, img));
  return img;
}

// ------------------------------------------------------------------ 检测图

struct Params {
  Json path = Json::object();
  Json width = Json::object();
  Json breaks = Json::object();
  Json edge = Json::object();
  Json judge = Json::object();
};

Json inspectGraph(const Params& p) {
  return makeGraph(
      {N{"n_load", "io.load_image", Json{{"source", "inputs"}}}, N{"n_path", "glue.bead_path", p.path},
       N{"n_width", "glue.bead_width", p.width}, N{"n_breaks", "glue.bead_breaks", p.breaks},
       N{"n_edge", "glue.edge_distance", p.edge}, N{"n_judge", "glue.judge", p.judge}},
      {E{"n_load.image", "n_path.image"}, E{"n_load.image", "n_width.image"},
       E{"n_path.path", "n_width.path"}, E{"n_width.bead", "n_breaks.bead"},
       E{"n_load.image", "n_edge.image"}, E{"n_width.bead", "n_edge.bead"},
       E{"n_width.bead", "n_judge.bead"}, E{"n_breaks.breaks", "n_judge.breaks"},
       E{"n_edge.edge", "n_judge.edge"}});
}

Params sceneParams(const Scene& sc) {
  Params p;
  p.path = Json{{"nozzle", {sc.nozzle.x, sc.nozzle.y}}, {"sector", {-120, 0}}};
  p.width = Json{{"form", sc.swirl ? "swirl" : "straight"}};
  return p;
}

/// 一次运行的结果：几个 Record 的 data 与 Measurement。
struct Result {
  std::string status;
  Json pathInfo, stations, beadInfo, breaks, edge, verdict;
  Json overlay;
  lyflow::Measurement widthMean, distanceMean, ok;
};

Json recordOf(const Data& d) {
  const Record* r = d.asRecord();
  return r ? r->data : Json();
}

Data fieldOf(const Data& bundle, const char* name) {
  const Bundle* b = bundle.asBundle();
  const Data* f = b ? b->field(name) : nullptr;
  return f ? *f : Data();
}

Result run(const Json& doc, const cv::Mat& gray, std::vector<exec::InjectedInput> extra = {}) {
  extra.push_back(exec::InjectedInput{"n_load", "image", Data::image(imageOf(gray))});
  Session s(doc, {}, {}, /*keepCache=*/false, 0, false, std::move(extra));
  RunLog& log = s.wait();
  Result r;
  r.status = log.runStatus();
  if (r.status != "ok") {
    for (const Json& e : log.ofKind("node_state")) {
      if (e.value("state", "") == "error") MESSAGE(e.dump());
    }
    return r;
  }
  auto get = [&](const char* node, const char* port) {
    Data d;
    REQUIRE(exec::ResultStore::instance().get(s.runId(), node, port, d));
    return d;
  };
  const Data path = get("n_path", "path");
  const Data bead = get("n_width", "bead");
  r.pathInfo = recordOf(fieldOf(path, "info"));
  r.stations = recordOf(fieldOf(bead, "stations"));
  r.beadInfo = recordOf(fieldOf(bead, "info"));
  r.breaks = recordOf(get("n_breaks", "breaks"));
  r.edge = recordOf(get("n_edge", "edge"));
  r.verdict = recordOf(get("n_judge", "verdict"));
  r.overlay = recordOf(get("n_judge", "overlay"));
  r.widthMean = *get("n_width", "widthMean").asMeasurement();
  r.distanceMean = *get("n_edge", "distanceMean").asMeasurement();
  r.ok = *get("n_judge", "ok").asMeasurement();
  return r;
}

std::vector<double> numbers(const Json& arr) {
  std::vector<double> out;
  for (const Json& v : arr) {
    if (v.is_number()) out.push_back(v.get<double>());
  }
  return out;
}

double medianOf(std::vector<double> v) { return glue::median(std::move(v)); }

// 断口 {10, 20, 40, 80} px，两端落在两站正中间（s ≡ 2 mod 4）：真值是这几段本身
const std::vector<std::pair<double, double>> kBreaks = {
    {142, 152}, {182, 202}, {234, 274}, {306, 386}};

}  // namespace

TEST_CASE("glue：合成直胶与螺旋胶，宽度 {8, 20, 40, 70} px 的胶宽、边距中位误差 ≤ 1.0 px，零误报") {
  for (const bool swirl : {false, true}) {
    for (const double W : {8.0, 20.0, 40.0, 70.0}) {
      Scene sc = swirl ? swirlScene(W) : Scene{};
      sc.beadWidth = W;
      CAPTURE(swirl);
      CAPTURE(W);
      const Result r = run(inspectGraph(sceneParams(sc)), render(sc));
      REQUIRE(r.status == "ok");
      CHECK(r.pathInfo.value("ok", false));
      CHECK(r.pathInfo.value("beadlike", false));
      const std::vector<double> widths = numbers(r.stations["width"]);
      REQUIRE(widths.size() == r.stations["count"].get<std::size_t>());  // 每一站都有胶
      const double wMed = medianOf(widths);
      CHECK(std::fabs(wMed - W) <= 1.0);
      MESSAGE("form=" << std::string(swirl ? "swirl" : "straight") << " W=" << W << " 胶宽中位 " << wMed
                      << " 边距中位 " << medianOf(numbers(r.edge["distance"])) << " 侧 " << r.edge["side"]);
      CHECK(r.edge["side"] == (sc.partSide > 0 ? "right" : "left"));
      const double dMed = medianOf(numbers(r.edge["distance"]));
      CHECK(std::fabs(dMed - sc.edgeGap) <= 1.0);
      CHECK(r.breaks["count"] == 0);
      CHECK(r.verdict["ok"] == true);
      CHECK(r.ok.value == 1.0);
      CHECK(r.widthMean.unit == "px");
      if (swirl) {
        // 对照：同一条螺旋胶按直胶量，只量得到一股的宽度 —— 外包络是 form = swirl 量出来的
        Params straight = sceneParams(sc);
        straight.width["form"] = "straight";
        const Result rs = run(inspectGraph(straight), render(sc));
        REQUIRE(rs.status == "ok");
        CHECK(medianOf(numbers(rs.stations["width"])) < W - 1.5);
      }
    }
  }
}

TEST_CASE("glue：合成断口 {10, 20, 40, 80} px —— minLength=20 恰好报出 ≥ 20 的那几段，起止误差 ≤ stationStep，其余零误报") {
  // 一帧一个断口（产线上一帧里一般至多一处），两端落在两站正中间（s ≡ 2 mod 4）
  for (const bool swirl : {false, true}) {
    for (const double W : {8.0, 20.0, 40.0, 70.0}) {
      for (const auto& truth : kBreaks) {
        Scene sc = swirl ? swirlScene(W) : Scene{};
        sc.beadWidth = W;
        sc.breaks = {truth};
        CAPTURE(swirl);
        CAPTURE(W);
        CAPTURE(truth.first);
        const Result r = run(inspectGraph(sceneParams(sc)), render(sc));
        REQUIRE(r.status == "ok");
        REQUIRE(r.pathInfo.value("ok", false));
        const Json& got = r.breaks["breaks"];
        std::string seen;
        for (const Json& b : got) seen += "[" + b["sStart"].dump() + ", " + b["sEnd"].dump() + "] ";
        MESSAGE("form=" << std::string(swirl ? "swirl" : "straight") << " W=" << W << " 断口 [" << truth.first
                        << ", " << truth.second << "] 报出 " << seen);
        const bool expected = truth.second - truth.first >= 20.0;
        REQUIRE(got.size() == (expected ? 1u : 0u));
        if (expected) {
          CHECK(std::fabs(got[0]["sStart"].get<double>() - truth.first) <= 4.0);
          CHECK(std::fabs(got[0]["sEnd"].get<double>() - truth.second) <= 4.0);
          CHECK(r.verdict["ok"] == false);
          CHECK(r.verdict["counts"]["break"] == 1);
          CHECK(r.verdict["message"].get<std::string>().rfind("NG：断胶 1 处", 0) == 0);
        } else {
          CHECK(r.verdict["ok"] == true);  // 短于 minLength 的不报，也不判 NG
        }
      }
    }
  }
}

TEST_CASE("glue：没有胶（只有一条边缘很缓的压痕）→ info.ok = false，判定只报「检测区内没找到胶」") {
  Scene sc;
  sc.bead = false;
  sc.dent = true;
  sc.beadWidth = 40;
  const Result r = run(inspectGraph(sceneParams(sc)), render(sc));
  REQUIRE(r.status == "ok");
  CHECK_FALSE(r.pathInfo.value("ok", true));
  CHECK(r.pathInfo["message"] == glue::kNoBeadMessage);
  MESSAGE("reason: " << r.pathInfo["reason"].get<std::string>()
                     << " sharpness=" << r.pathInfo["sharpness"].dump()
                     << " coverage=" << r.pathInfo["coverage"].dump());
  CHECK(r.verdict["ok"] == false);
  CHECK(r.verdict["message"] == glue::kNoBeadMessage);
  REQUIRE(r.verdict["defects"].size() == 1);
  CHECK(r.verdict["defects"][0]["type"] == "missing");
  CHECK(r.ok.value == 0.0);
  CHECK(r.ok.verdict == "ng");
  // 下游照常跑出「全段无胶」：每一站都是无胶，断胶就是整个检测区
  CHECK(numbers(r.stations["width"]).empty());
  REQUIRE(r.breaks["breaks"].size() == 1);
  CHECK(r.breaks["breaks"][0]["sStart"] == 100.0);
  CHECK(r.breaks["breaks"][0]["sEnd"] == 400.0);
  CHECK_FALSE(r.widthMean.ok);
}

TEST_CASE("glue：一片空白（连暗带都没有）也是「检测区内没找到胶」，不是执行错误") {
  Scene sc;
  sc.bead = false;
  const Result r = run(inspectGraph(sceneParams(sc)), render(sc));
  REQUIRE(r.status == "ok");
  CHECK_FALSE(r.pathInfo.value("ok", true));
  CHECK(r.verdict["message"] == glue::kNoBeadMessage);
}

TEST_CASE("glue：calib 接上合成单应（0.1 mm/px 加一点透视）—— 宽度、边距、断口长度按映射后两点的距离出 mm") {
  // [X, Y, W] = H·[x, y, 1]：0.1 mm/px 的缩放，外加随 x、y 变化的透视（图的两头差约 4%）
  const std::array<double, 9> H = {0.1, 0.004, -3.0, -0.002, 0.1, 5.0, 4e-5, 3e-5, 1.0};
  std_image::PlaneCalib calib;
  calib.H = H;
  Record rec;
  rec.type = std_image::kPlaneCalibType;
  rec.data = std_image::planeCalibJson(calib);
  Scene sc;
  sc.beadWidth = 40;
  sc.breaks = {{234, 274}};
  Params p = sceneParams(sc);
  p.breaks = Json{{"minLength", 2.0}};  // 接了标定，minLength 跟着 bead 的单位走：mm
  const std::vector<exec::InjectedInput> inject = {
      exec::InjectedInput{"n_width", "calib", Data::record(rec)},
      exec::InjectedInput{"n_edge", "calib", Data::record(rec)}};
  const Result r = run(inspectGraph(p), render(sc), inject);
  REQUIRE(r.status == "ok");
  CHECK(r.beadInfo["unit"] == "mm");
  CHECK(r.widthMean.unit == "mm");
  CHECK(r.distanceMean.unit == "mm");
  CHECK(r.breaks["unit"] == "mm");

  auto mapDist = [&](P2 a, P2 b) { return calib.distance(a.x, a.y, b.x, b.y); };
  // 宽度：每站 = 两边的像素点映射之后的距离（独立算一遍对照）
  std::size_t checked = 0;
  for (std::size_t i = 0; i < r.stations["count"].get<std::size_t>(); ++i) {
    if (!r.stations["present"][i].get<bool>()) continue;
    const P2 a = glue::pxOf(r.stations["left"][i]);
    const P2 b = glue::pxOf(r.stations["right"][i]);
    CHECK(r.stations["width"][i].get<double>() == doctest::Approx(mapDist(a, b)).epsilon(1e-3));
    // 与 px 宽度差一个随位置变的比例，量级是 0.1 mm/px
    const double ratio = r.stations["width"][i].get<double>() / r.stations["widthPx"][i].get<double>();
    CHECK(ratio > 0.08);
    CHECK(ratio < 0.12);
    ++checked;
  }
  CHECK(checked > 50);
  // 边距：量起点 → 零件边点，映射之后的距离
  checked = 0;
  for (std::size_t i = 0; i < r.edge["count"].get<std::size_t>(); ++i) {
    if (r.edge["status"][i] != "ok") continue;
    const P2 a = glue::pxOf(r.edge["from"][i]);
    const P2 b = glue::pxOf(r.edge["edge"][i]);
    CHECK(r.edge["distance"][i].get<double>() == doctest::Approx(mapDist(a, b)).epsilon(1e-3));
    ++checked;
  }
  CHECK(checked > 50);
  // 断口：沿胶路映射之后的长度；胶路在这一段是直的，与两端点映射后的距离相同
  REQUIRE(r.breaks["breaks"].size() == 1);
  const Json& b = r.breaks["breaks"][0];
  const double chord = mapDist(glue::pxOf(b["start"]), glue::pxOf(b["end"]));
  CHECK(b["length"].get<double>() == doctest::Approx(chord).epsilon(2e-3));
  CHECK(b["lengthPx"].get<double>() == doctest::Approx(40.0).epsilon(0.1));
  CHECK(b["length"].get<double>() > 3.0);
  CHECK(b["length"].get<double>() < 5.0);
}

TEST_CASE("glue：坏的 calib（8 个数 / 奇异矩阵）是 bad_input，指到 calib 端口") {
  Scene sc;
  const cv::Mat gray = render(sc);
  for (const Json& data : {Json{{"H", {1, 0, 0, 0, 1, 0, 0, 0}}, {"unit", "mm"}},
                           Json{{"H", {1, 2, 3, 2, 4, 6, 0, 0, 1}}, {"unit", "mm"}}}) {
    Record rec;
    rec.type = std_image::kPlaneCalibType;
    rec.data = data;
    const Json doc = inspectGraph(sceneParams(sc));
    std::vector<exec::InjectedInput> inject = {
        exec::InjectedInput{"n_load", "image", Data::image(imageOf(gray))},
        exec::InjectedInput{"n_width", "calib", Data::record(rec)}};
    Session s(doc, {}, {}, false, 0, false, std::move(inject));
    const RunLog& log = s.wait();
    const Json e = log.nodeEvent("n_width", "error");
    REQUIRE(e.is_object());
    REQUIRE_FALSE(e.empty());
    CHECK(e["error"]["code"] == "bad_input");
    CHECK(e["error"]["portName"] == "calib");
  }
}

TEST_CASE("glue：加载期校验 —— zone / sector / widthRange 填反了、限值上下倒置，图跑不起来") {
  auto errors = [](const Json& doc) {
    std::vector<std::string> paths;
    for (const Json& d : Json::parse(exec::validateGraphJson(doc.dump(), {}))) {
      if (d.value("severity", "") == "error") paths.push_back(d.value("paramPath", ""));
    }
    return paths;
  };
  Params p;
  CHECK(errors(inspectGraph(p)).empty());
  p.path = Json{{"zone", {400, 100}}};
  CHECK(errors(inspectGraph(p)) == std::vector<std::string>{"zone"});
  p.path = Json{{"sector", {10, -10}}};
  CHECK(errors(inspectGraph(p)) == std::vector<std::string>{"sector"});
  p.path = Json{{"sector", {10, -10}}, {"headingSource", "param"}};  // 给了方向就不看扇区
  CHECK(errors(inspectGraph(p)).empty());
  p.path = Json::object();
  p.width = Json{{"widthRange", {60, 20}}};
  CHECK(errors(inspectGraph(p)) == std::vector<std::string>{"widthRange"});
  p.width = Json::object();
  p.judge = Json{{"widthLimits", {40, 20}}};
  CHECK(errors(inspectGraph(p)) == std::vector<std::string>{"widthLimits"});
  p.judge = Json{{"distanceLimits", {-1, 0}}};
  CHECK_FALSE(errors(inspectGraph(p)).empty());
}

TEST_CASE("glue：响应图只算一块（ROI）时，块里的值与整幅图算出来的逐位相同") {
  Scene sc;
  const cv::Mat gray = render(sc);
  const cv::Rect roi(200, 150, 180, 160);
  const glue::Field whole = glue::responseField(gray, false, 90, cv::Rect(0, 0, gray.cols, gray.rows));
  const glue::Field part = glue::responseField(gray, false, 90, roi);
  double maxDiff = 0;
  for (int y = roi.y; y < roi.y + roi.height - 1; y += 3) {
    for (int x = roi.x; x < roi.x + roi.width - 1; x += 3) {
      maxDiff = std::max(maxDiff, std::fabs(whole.at(x + 0.25, y + 0.5) - part.at(x + 0.25, y + 0.5)));
    }
  }
  CHECK(maxDiff == 0.0);
  CHECK(part.at(roi.x - 1.0, roi.y + 5.0) == glue::kInvalid);  // 块外不可信
}

TEST_CASE("glue：人造断胶 —— 源带取干净的一侧，另一条检测链查得出，起止误差 ≤ 5 px") {
  Scene sc;
  sc.beadWidth = 30;
  const Json doc = makeGraph(
      {N{"n_load", "io.load_image", Json{{"source", "inputs"}}},
       N{"n_path", "glue.bead_path", sceneParams(sc).path},
       N{"n_width", "glue.bead_width", Json::object()},
       N{"n_synth", "glue.synth_break", Json{{"sStart", 202}, {"length", 60}}},
       N{"n_path2", "glue.bead_path", sceneParams(sc).path},
       N{"n_width2", "glue.bead_width", Json::object()},
       N{"n_breaks2", "glue.bead_breaks", Json::object()}},
      {E{"n_load.image", "n_path.image"}, E{"n_load.image", "n_width.image"},
       E{"n_path.path", "n_width.path"}, E{"n_load.image", "n_synth.image"},
       E{"n_width.bead", "n_synth.bead"}, E{"n_synth.image", "n_path2.image"},
       E{"n_synth.image", "n_width2.image"}, E{"n_path2.path", "n_width2.path"},
       E{"n_width2.bead", "n_breaks2.bead"}});
  std::vector<exec::InjectedInput> inject = {
      exec::InjectedInput{"n_load", "image", Data::image(imageOf(render(sc)))}};
  Session s(doc, {}, {}, false, 0, false, std::move(inject));
  REQUIRE(s.wait().runStatus() == "ok");
  Data info, breaks, image;
  REQUIRE(exec::ResultStore::instance().get(s.runId(), "n_synth", "info", info));
  REQUIRE(exec::ResultStore::instance().get(s.runId(), "n_breaks2", "breaks", breaks));
  REQUIRE(exec::ResultStore::instance().get(s.runId(), "n_synth", "image", image));
  const Json in = info.asRecord()->data;
  MESSAGE(in.dump());
  CHECK(in["ok"] == true);
  CHECK(in["side"] == "left");  // 零件边在右侧：右边的源带里有零件边的暗区，不干净
  CHECK(image.asImage()->channels == 1);
  const Json& got = breaks.asRecord()->data["breaks"];
  REQUIRE(got.size() == 1);
  CHECK(std::fabs(got[0]["sStart"].get<double>() - 202.0) <= 5.0);
  CHECK(std::fabs(got[0]["sEnd"].get<double>() - 262.0) <= 5.0);
}

TEST_CASE("glue：叠画是合格的 lyflow.overlay2d（顺手写一份样例给 pnpm check 的 schema 校验）") {
  Scene sc;
  sc.beadWidth = 30;
  sc.breaks = {{234, 274}};
  const Result r = run(inspectGraph(sceneParams(sc)), render(sc));
  REQUIRE(r.status == "ok");
  REQUIRE(r.overlay.is_object());
  CHECK(r.overlay["frame"] == "image");
  std::set<std::string> roles;
  for (const Json& item : r.overlay["items"]) roles.insert(item.value("role", ""));
  for (const char* want : {"path", "edge.left", "edge.right", "part", "defect", "ng"}) {
    CHECK_MESSAGE(roles.count(want) == 1, "缺 role " << want);
  }
  // check.ps1「glue 叠画 vs schema」校验这一份；它在构建之前先删掉旧的
  const std::filesystem::path sample =
      std::filesystem::temp_directory_path() / "lyflow-glue-overlay-sample.json";
  std::ofstream out(sample, std::ios::binary);
  out << Json{{"kind", "Record"}, {"type", "lyflow.overlay2d"}, {"data", r.overlay}}.dump(2);
}
