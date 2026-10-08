// 固定相机飞拍后检（glue-plan §5，第 21 / 22 条）：合成图上的定位与逐点卡尺。
// 场景（示教图坐标）：圆角矩形的暗开口，下沿 y = 330 是内边；翻边是亮的渐变；胶条中线在 y = 350、宽 16 px；
// 翻边上两个安装孔给模板当特征；(300, 382) 有个比胶还暗的斑落在卡尺的找胶窗口里。x ∈ [400, 420] 没胶（断口），x ∈ [560, 580] 的内边被抹到 y = 300（找不到内边）。
// 当前帧 = 示教场景经已知刚体变换 T 画出来，位姿、距离都有真值（D12 / K8）。
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "exec/executor.h"
#include "exec/result_store.h"
#include "helpers.h"
#include "lyflow/c_api.h"
#include "lyflow_cv/adapter.h"
#include "algo/flyshot.h"
#include "lyflow_cv/plane_calib.h"
#include "ops/glue.h"

namespace {

using namespace lyflow;
using namespace lyflow::test;
using lyflow::packs::glue::P2;

constexpr double kPi = 3.14159265358979323846;
const P2 kPivot(450, 350);

struct Rigid {
  double tx = 0, ty = 0, deg = 0;
  P2 apply(P2 p) const {
    const double a = deg * kPi / 180.0, c = std::cos(a), s = std::sin(a);
    const P2 d = p - kPivot;
    return P2(c * d.x - s * d.y, s * d.x + c * d.y) + kPivot + P2(tx, ty);
  }
  P2 inverse(P2 q) const {
    const double a = -deg * kPi / 180.0, c = std::cos(a), s = std::sin(a);
    const P2 d = q - kPivot - P2(tx, ty);
    return P2(c * d.x - s * d.y, s * d.x + c * d.y) + kPivot;
  }
};

bool inOpening(double x, double y) {
  if (x >= 560 && x <= 580 && y > 300) return false;  // 内边被抹掉的一段
  const double x0 = 120, x1 = 780, y0 = 100, y1 = 330, r = 60;
  if (x < x0 || x > x1 || y < y0 || y > y1) return false;
  const double cx = std::clamp(x, x0 + r, x1 - r), cy = std::clamp(y, y0 + r, y1 - r);
  return std::hypot(x - cx, y - cy) <= r;
}

double sceneValue(double x, double y, bool part) {
  const double flange = 190.0 + 0.05 * (x - 450.0) - 0.03 * (y - 350.0);
  if (!part) return flange;
  if (inOpening(x, y)) return 35.0;
  for (const P2 h : {P2(220, 460), P2(660, 480)}) {
    if (std::hypot(x - h.x, y - h.y) <= 18) return 50.0;
  }
  // 胶条外侧卡尺窗口里的一个暗斑（比胶还暗）：逐点卡尺不能把它当成胶
  if (std::hypot(x - 300, y - 382) <= 8) return 45.0;
  if (x >= 180 && x <= 720 && std::fabs(y - 350.0) <= 8.0 && !(x >= 400 && x <= 420)) return 70.0;
  return flange;
}

cv::Mat render(const Rigid& T, bool part = true) {
  cv::Mat img(700, 900, CV_32F);
  // 4×4 超采样：采样点落不到整数坐标上，场景里 y = 330、342 这类整数边界画出来不偏
  constexpr int kSub = 4;
  for (int y = 0; y < img.rows; ++y) {
    for (int x = 0; x < img.cols; ++x) {
      double acc = 0;
      for (int j = 0; j < kSub; ++j) {
        for (int i = 0; i < kSub; ++i) {
          const P2 p = T.inverse(P2(x - 0.5 + (i + 0.5) / kSub, y - 0.5 + (j + 0.5) / kSub));
          acc += sceneValue(p.x, p.y, part);
        }
      }
      img.at<float>(y, x) = static_cast<float>(acc / (kSub * kSub));
    }
  }
  cv::GaussianBlur(img, img, cv::Size(0, 0), 1.0);
  cv::Mat noise(img.size(), CV_32F);
  cv::RNG rng(11);
  rng.fill(noise, cv::RNG::NORMAL, 0.0, 2.0);
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

const P2 kAnchor(590, 250);

struct Files {
  std::filesystem::path dir = std::filesystem::temp_directory_path() / "lyflow-glue-flyshot";
  std::filesystem::path tmpl = dir / "template.png";
  std::filesystem::path stations = dir / "stations.json";
  std::filesystem::path calib = dir / "calib.json";
};

/// 示教：画恒等位姿的一帧，从上面裁模板、写测量点文件（胶中线上每 4 px 一站，法向朝下）。
Files teach() {
  Files f;
  std::filesystem::create_directories(f.dir);
  const cv::Mat teachImg = render(Rigid{});
  std::vector<uchar> bytes;
  REQUIRE(cv::imencode(".png", teachImg(cv::Rect(590, 250, 220, 260)), bytes));
  std::ofstream(f.tmpl, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  Json pts = Json::array(), nrm = Json::array(), ids = Json::array();
  for (int x = 200, j = 0; x <= 700; x += 4, ++j) {
    pts.push_back({x, 350});
    nrm.push_back({0, 1});
    ids.push_back(1000 + j);
  }
  std::ofstream(f.stations, std::ios::binary) << Json{{"points", pts}, {"normals", nrm}, {"ids", ids}}.dump();
  return f;
}

Json flyshotGraph(const Files& f, bool withCalib) {
  std::vector<N> nodes = {
      N{"n_load", "io.load_image", Json{{"source", "inputs"}}},
      N{"n_loc", "glue.locate", Json{{"template", f.tmpl.u8string()}, {"anchor", {kAnchor.x, kAnchor.y}}}},
      N{"n_cal", "glue.station_calipers", Json{{"stations", f.stations.u8string()}}},
  };
  std::vector<E> edges = {E{"n_load.image", "n_loc.image"}, E{"n_load.image", "n_cal.image"},
                          E{"n_loc.pose", "n_cal.pose"}};
  if (withCalib) {
    nodes.push_back(N{"n_calib", "image.load_calib", Json{{"path", f.calib.u8string()}}});
    edges.push_back(E{"n_calib.calib", "n_cal.calib"});
  }
  return makeGraph(nodes, edges);
}

struct Out {
  std::string status;
  Json pose, measure;
};

Out run(const Json& doc, const cv::Mat& gray) {
  Session s(doc, {}, {}, false, 0, false, {exec::InjectedInput{"n_load", "image", Data::image(imageOf(gray))}});
  RunLog& log = s.wait();
  Out o;
  o.status = log.runStatus();
  if (o.status != "ok") {
    for (const Json& e : log.ofKind("node_state")) {
      if (e.value("state", "") == "error") MESSAGE(e.dump());
    }
    return o;
  }
  Data d;
  REQUIRE(exec::ResultStore::instance().get(s.runId(), "n_loc", "pose", d));
  o.pose = d.asRecord()->data;
  REQUIRE(exec::ResultStore::instance().get(s.runId(), "n_cal", "measure", d));
  o.measure = d.asRecord()->data;
  return o;
}

double median(std::vector<double> v) {
  REQUIRE_FALSE(v.empty());
  std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
  return v[v.size() / 2];
}

/// 站的 x（示教图坐标）落在哪一类：断口、内边被抹掉、离它们都远的正常段。边界附近两站不判。
enum class Zone { Normal, Gap, NoInner, Edge };
Zone zoneOf(double x) {
  if (x >= 404 && x <= 416) return Zone::Gap;
  if (x >= 564 && x <= 576) return Zone::NoInner;
  if ((x > 392 && x < 428) || (x > 552 && x < 588)) return Zone::Edge;
  return Zone::Normal;
}

void checkStations(const Json& m, double tolPx) {
  const std::size_t n = m["count"].get<std::size_t>();
  std::vector<double> innerNear, innerCenter, width;
  int wrong = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const double x = 200 + 4.0 * i;
    const std::string st = m["status"][i];
    switch (zoneOf(x)) {
      case Zone::Gap:
        if (st != "no_bead") {
          ++wrong;
          MESSAGE("x=" << x << " 应 no_bead，得 " << st);
        }
        break;
      case Zone::NoInner:
        if (st != "no_inner") {
          ++wrong;
          MESSAGE("x=" << x << " 应 no_inner，得 " << st);
        }
        break;
      case Zone::Normal:
        if (st != "ok") {
          ++wrong;
          MESSAGE("x=" << x << " 应 ok，得 " << st);
          break;
        }
        innerNear.push_back(m["innerNearPx"][i]);
        innerCenter.push_back(m["innerCenterPx"][i]);
        width.push_back(m["widthPx"][i]);
        break;
      case Zone::Edge:
        break;
    }
  }
  CHECK(wrong == 0);
  CHECK(std::fabs(median(innerNear) - 12.0) <= tolPx);
  CHECK(std::fabs(median(innerCenter) - 20.0) <= tolPx);
  CHECK(std::fabs(median(width) - 16.0) <= tolPx);
}

}  // namespace

TEST_CASE("glue 飞拍：恒等位姿 —— 定位分数 ≥ 0.9、位姿≈0；逐点卡尺的距离中位误差 ≤ 0.3 px，断口 no_bead、抹掉的内边 no_inner") {
  const Files f = teach();
  const Out o = run(flyshotGraph(f, false), render(Rigid{}));
  REQUIRE(o.status == "ok");
  CHECK(o.pose["ok"] == true);
  CHECK(o.pose["score"].get<double>() >= 0.9);
  CHECK(std::fabs(o.pose["angle"].get<double>()) <= 0.05);
  CHECK(std::fabs(o.pose["offset"][0].get<double>()) <= 0.3);
  CHECK(std::fabs(o.pose["offset"][1].get<double>()) <= 0.3);
  CHECK(o.measure["unit"] == "px");
  CHECK(o.measure["ids"][0] == 1000);
  checkStations(o.measure, 0.3);
}

TEST_CASE("glue 飞拍：平移 ±40 px、旋转 ±2° —— 位姿误差 ≤ 0.3 px / 0.1°，站跟着位姿走，距离不变") {
  const Files f = teach();
  for (const Rigid T : {Rigid{25, -18, 1.5}, Rigid{-38, 31, -2.0}, Rigid{40, 12, 0.7},
                        Rigid{0, 0, 0.12}, Rigid{0, 0, -0.12}}) {
    CAPTURE(T.tx);
    CAPTURE(T.deg);
    const Out o = run(flyshotGraph(f, false), render(T));
    REQUIRE(o.status == "ok");
    REQUIRE(o.pose["ok"] == true);
    CHECK(o.pose["score"].get<double>() >= 0.9);
    CHECK(std::fabs(o.pose["angle"].get<double>() - T.deg) <= 0.1);
    const P2 teachCenter = kAnchor + P2(219 / 2.0, 259 / 2.0);
    const P2 want = T.apply(teachCenter);
    CHECK(std::fabs(o.pose["center"][0].get<double>() - want.x) <= 0.3);
    CHECK(std::fabs(o.pose["center"][1].get<double>() - want.y) <= 0.3);
    checkStations(o.measure, 0.3);
  }
}

TEST_CASE("glue 飞拍：模板在图上根本没有 —— pose.ok = false，全部站 pose_fail，不是执行错误") {
  const Files f = teach();
  const Out o = run(flyshotGraph(f, false), render(Rigid{}, /*part=*/false));
  REQUIRE(o.status == "ok");
  CHECK(o.pose["ok"] == false);
  CHECK(o.measure["poseOk"] == false);
  for (const Json& st : o.measure["status"]) CHECK(st == "pose_fail");
  CHECK(o.measure["counts"]["poseFail"] == o.measure["count"]);
}

TEST_CASE("glue 飞拍：接 image.load_calib 的合成单应 —— 内边→胶中线、内边→近边出 mm，误差 ≤ 0.02 mm") {
  const Files f = teach();
  std_image::PlaneCalib calib;
  calib.H = {0.05, 0.002, 1.0, 0.001, 0.05, 2.0, 1e-6, 2e-6, 1.0};
  std::ofstream(f.calib, std::ios::binary)
      << Json{{"kind", "Record"}, {"type", "image.PlaneCalib"}, {"data", std_image::planeCalibJson(calib)}}.dump();
  const Rigid T{12, -9, 1.0};
  const Out o = run(flyshotGraph(f, true), render(T));
  REQUIRE(o.status == "ok");
  const Json& m = o.measure;
  CHECK(m["unit"] == "mm");
  int checked = 0;
  for (std::size_t i = 0; i < m["count"].get<std::size_t>(); ++i) {
    const double x = 200 + 4.0 * i;
    if (zoneOf(x) != Zone::Normal) continue;
    REQUIRE(m["status"][i] == "ok");
    const P2 inner = T.apply(P2(x, 330)), mid = T.apply(P2(x, 350)), nearE = T.apply(P2(x, 342));
    const double wantCenter = calib.distance(inner.x, inner.y, mid.x, mid.y);
    const double wantNear = calib.distance(inner.x, inner.y, nearE.x, nearE.y);
    CHECK(std::fabs(m["innerCenter"][i].get<double>() - wantCenter) <= 0.02);
    CHECK(std::fabs(m["innerNear"][i].get<double>() - wantNear) <= 0.02);
    ++checked;
  }
  CHECK(checked > 80);
}

TEST_CASE("glue 飞拍：加载期校验 —— innerSearch / beadSearch 倒了、searchHalf 盖不住，图跑不起来") {
  auto errors = [](const Json& params) {
    const Json doc = makeGraph({N{"n_load", "io.load_image", Json{{"source", "inputs"}}},
                                N{"n_cal", "glue.station_calipers", params}},
                               {E{"n_load.image", "n_cal.image"}});
    std::vector<std::string> paths;
    for (const Json& d : Json::parse(exec::validateGraphJson(doc.dump(), {}))) {
      if (d.value("severity", "") == "error") paths.push_back(d.value("paramPath", ""));
    }
    return paths;
  };
  auto with = [](const char* key, Json value) { return Json{{"stations", "x.json"}, {key, std::move(value)}}; };
  CHECK(errors(Json{{"stations", "x.json"}}).empty());
  CHECK(errors(with("innerSearch", {-3, -40})) == std::vector<std::string>{"innerSearch"});
  CHECK(errors(with("beadSearch", {40, -15})) == std::vector<std::string>{"beadSearch"});
  CHECK(errors(with("searchHalf", 30)) == std::vector<std::string>{"searchHalf"});
}


TEST_CASE("glue 飞拍：searchRadius 是硬边界 —— 真实偏移超出它时，匹配中心不出这个范围、分数明显低于真匹配") {
  const Files f = teach();
  Json doc = flyshotGraph(f, false);
  for (Json& n : doc["nodes"]) {
    if (n["id"] == "n_loc") n["params"]["searchRadius"] = 20;
  }
  const Out o = run(doc, render(Rigid{40, 12, 0.0}));
  REQUIRE(o.status == "ok");
  CHECK(std::fabs(o.pose["offset"][0].get<double>()) <= 21.0);
  CHECK(std::fabs(o.pose["offset"][1].get<double>()) <= 21.0);
  CHECK(o.pose["score"].get<double>() < 0.9);
  const Out near = run(doc, render(Rigid{12, -9, 0.0}));
  REQUIRE(near.status == "ok");
  CHECK(near.pose["ok"] == true);
  CHECK(near.pose["score"].get<double>() >= 0.9);
}

TEST_CASE("glue 飞拍：纯色模板不能产生成功位姿") {
  const cv::Mat frame(100, 100, CV_8UC1, cv::Scalar(180));
  const cv::Mat tmpl(20, 20, CV_8UC1, cv::Scalar(180));
  lyflow::packs::glue::LocateSpec spec;
  spec.anchor = P2(40, 40);
  std::string why;
  const auto pose = lyflow::packs::glue::locateTemplate(frame, tmpl, spec, &why);
  CHECK_FALSE(pose.ok);
  CHECK_FALSE(why.empty());
}

TEST_CASE("glue 飞拍：没有完整胶边时不能返回 ok 或编造窗口边界作为胶边") {
  cv::Mat frame(160, 40, CV_8UC1, cv::Scalar(190));
  frame.rowRange(0, 60).setTo(35);
  frame.rowRange(72, 94).setTo(70);
  cv::GaussianBlur(frame, frame, cv::Size(0, 0), 1.0);
  lyflow::packs::glue::CaliperSpec spec;
  spec.beadTo = 10;  // 胶远边在 t=14，已经落在找胶窗口之外
  auto measured = lyflow::packs::glue::measureCaliper(frame, P2(20, 80), P2(0, 1), spec);
  CHECK(measured.status == "incomplete_bead");
  spec.beadFrom = -3;  // 近边 t=-8 被裁掉，远边在窗口内
  spec.beadTo = 40;
  measured = lyflow::packs::glue::measureCaliper(frame, P2(20, 80), P2(0, 1), spec);
  CHECK(measured.status == "incomplete_bead");
}

TEST_CASE("glue 飞拍：RGB 和 RGBA 转灰度后与单通道结果一致") {
  const Files f = teach();
  const cv::Mat mono = render(Rigid{});
  const Out reference = run(flyshotGraph(f, false), mono);
  REQUIRE(reference.status == "ok");
  for (int code : {cv::COLOR_GRAY2RGB, cv::COLOR_GRAY2RGBA}) {
    cv::Mat color;
    cv::cvtColor(mono, color, code);
    const Out result = run(flyshotGraph(f, false), color);
    REQUIRE(result.status == "ok");
    CHECK(result.pose == reference.pose);
    CHECK(result.measure == reference.measure);
  }
}

TEST_CASE("glue 飞拍：u16 图像要明确拒绝，不能按 u8 解释内存") {
  const Files f = teach();
  cv::Mat wide;
  render(Rigid{}).convertTo(wide, CV_16U, 257);
  Session s(flyshotGraph(f, false), {}, {}, false, 0, false,
            {exec::InjectedInput{"n_load", "image", Data::image(imageOf(wide))}});
  const Json e = s.wait().nodeEvent("n_loc", "error");
  CHECK(e["error"]["code"] == "bad_input");
  CHECK(e["error"]["portName"] == "image");
}

TEST_CASE("glue 飞拍：随包产线图经 C ABI 接收带行填充的相机帧和图参数，逐帧输出毫米点表") {
  const Files f = teach();
  std_image::PlaneCalib calib;
  calib.H = {0.05, 0, 0, 0, 0.05, 0, 0, 0, 1};
  std::ofstream(f.calib, std::ios::binary) << std_image::planeCalibJson(calib).dump();
  std::ifstream in(std::filesystem::path(__FILE__).parent_path().parent_path() / "graphs/flyshot.lyflow.json");
  REQUIRE(in.good());
  const std::string graph(std::istreambuf_iterator<char>(in), {});
  const std::string params = Json{{"template", f.tmpl.u8string()}, {"anchor", {590, 250}},
                                  {"stations", f.stations.u8string()}, {"calib", f.calib.u8string()}}.dump();
  // 同一张图、同一组参数连续注入不同内容，第二帧不能命中第一帧的图像缓存。
  for (bool part : {true, false}) {
    const cv::Mat frame = render(Rigid{}, part);
    const unsigned stride = frame.cols + 16;
    std::vector<uint8_t> pixels(stride * frame.rows, 255);
    for (int y = 0; y < frame.rows; ++y) std::memcpy(pixels.data() + y * stride, frame.ptr(y), frame.cols);
    lyflow_run_image_input input{};
    input.node_id = "n_load";
    input.port = "image";
    input.width = frame.cols;
    input.height = frame.rows;
    input.channels = 1;
    input.depth = 1;
    input.row_bytes = stride;
    input.pixels = pixels.data();
    lyflow_run_options opts{};
    opts.run_id = part ? "flyshot-camera-part" : "flyshot-camera-empty";
    opts.params_json = params.c_str();
    opts.image_inputs = &input;
    opts.image_input_count = 1;
    const auto cleanup = [](lyflow_run* run) { lyflow_run_join(run); lyflow_run_free(run); };
    std::unique_ptr<lyflow_run, decltype(cleanup)> handle(lyflow_run_start(graph.c_str(), &opts, nullptr, nullptr), cleanup);
    REQUIRE(handle != nullptr);
    lyflow_run_join(handle.get());
    char* raw = lyflow_run_outputs(opts.run_id);
    REQUIRE(raw != nullptr);
    const Json outputs = Json::parse(raw);
    lyflow_string_free(raw);
    INFO(outputs.dump());
    REQUIRE(outputs["pose"]["type"] == "Record");
    REQUIRE(outputs["measure"]["type"] == "Record");
    CHECK(outputs["pose"]["value"]["data"]["ok"] == part);
    const Json& measure = outputs["measure"]["value"]["data"];
    CHECK(measure["unit"] == "mm");
    CHECK(measure["ids"].size() == 126);
    if (part) {
      CHECK(std::fabs(measure["innerCenter"][0].get<double>() - 1.0) <= 0.02);
    } else {
      CHECK(measure["counts"]["poseFail"] == measure["count"]);
    }
  }
}

TEST_CASE("glue 飞拍：坏的站点文件以 bad_param 指向 stations，空点表不能算成功") {
  const Files f = teach();
  const cv::Mat frame = render(Rigid{});
  for (const Json& data : {Json::array(), Json{{"points", Json::array()}, {"normals", Json::array()}},
                           Json{{"points", {{20, 30}}}, {"normals", {{0, 0}}}}}) {
    std::ofstream(f.stations, std::ios::binary) << data.dump();
    Session s(flyshotGraph(f, false), {}, {}, false, 0, false,
              {exec::InjectedInput{"n_load", "image", Data::image(imageOf(frame))}});
    const Json e = s.wait().nodeEvent("n_cal", "error");
    CHECK(e["error"]["code"] == "bad_param");
    CHECK(e["error"]["paramPath"] == "stations");
  }
}
