// 固定相机飞拍后检（glue-plan §5，第 21 / 22 条）：合成图上的定位与逐点卡尺。
// 场景（示教图坐标）：圆角矩形的暗开口，下沿 y = 330 是内边；翻边是亮的渐变；胶条中线在 y = 350、宽 16 px；
// 翻边上两个安装孔给模板当特征（twinHole 时 (580, 480) 再多一个一样的孔，给定位的歧义门用）；(300, 382) 有个比胶还暗的斑落在卡尺的找胶窗口里。x ∈ [400, 420] 没胶（断口），x ∈ [560, 580] 的内边被抹到 y = 300（找不到内边）。
// 当前帧 = 示教场景经已知刚体变换 T 画出来，位姿、距离都有真值（D12 / K8）。
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
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
  if (x >= 560 && x <= 580 && y > 300)
    return false; // 内边被抹掉的一段
  const double x0 = 120, x1 = 780, y0 = 100, y1 = 330, r = 60;
  if (x < x0 || x > x1 || y < y0 || y > y1)
    return false;
  const double cx = std::clamp(x, x0 + r, x1 - r), cy = std::clamp(y, y0 + r, y1 - r);
  return std::hypot(x - cx, y - cy) <= r;
}

double sceneValue(double x, double y, bool part, bool twinHole) {
  const double flange = 190.0 + 0.05 * (x - 450.0) - 0.03 * (y - 350.0);
  if (!part)
    return flange;
  if (inOpening(x, y))
    return 35.0;
  for (const P2 h : {P2(220, 460), P2(660, 480)}) {
    if (std::hypot(x - h.x, y - h.y) <= 18)
      return 50.0;
  }
  if (twinHole && std::hypot(x - 580, y - 480) <= 18)
    return 50.0;
  // 胶条外侧卡尺窗口里的一个暗斑（比胶还暗）：逐点卡尺不能把它当成胶
  if (std::hypot(x - 300, y - 382) <= 8)
    return 45.0;
  if (x >= 180 && x <= 720 && std::fabs(y - 350.0) <= 8.0 && !(x >= 400 && x <= 420))
    return 70.0;
  return flange;
}

cv::Mat render(const Rigid& T, bool part = true, bool twinHole = false) {
  cv::Mat img(700, 900, CV_32F);
  // 4×4 超采样：采样点落不到整数坐标上，场景里 y = 330、342 这类整数边界画出来不偏
  constexpr int kSub = 4;
  for (int y = 0; y < img.rows; ++y) {
    for (int x = 0; x < img.cols; ++x) {
      double acc = 0;
      for (int j = 0; j < kSub; ++j) {
        for (int i = 0; i < kSub; ++i) {
          const P2 p = T.inverse(P2(x - 0.5 + (i + 0.5) / kSub, y - 0.5 + (j + 0.5) / kSub));
          acc += sceneValue(p.x, p.y, part, twinHole);
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

void writeTemplate(const std::filesystem::path& file, const cv::Mat& teachImg, cv::Rect crop) {
  std::vector<uchar> bytes;
  REQUIRE(cv::imencode(".png", teachImg(crop), bytes));
  std::ofstream(file, std::ios::binary)
      .write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

/// 示教：画恒等位姿的一帧，从上面裁模板、写测量点文件（胶中线上每 4 px 一站，法向朝下）。
Files teach() {
  Files f;
  std::filesystem::create_directories(f.dir);
  writeTemplate(f.tmpl, render(Rigid{}), cv::Rect(590, 250, 220, 260));
  Json pts = Json::array(), nrm = Json::array(), ids = Json::array();
  for (int x = 200, j = 0; x <= 700; x += 4, ++j) {
    pts.push_back({x, 350});
    nrm.push_back({0, 1});
    ids.push_back(1000 + j);
  }
  std::ofstream(f.stations, std::ios::binary)
      << Json{{"points", pts}, {"normals", nrm}, {"ids", ids}}.dump();
  return f;
}

Json flyshotGraph(const Files& f, bool withCalib) {
  std::vector<N> nodes = {
      N{"n_load", "io.load_image", Json{{"source", "inputs"}}},
      N{"n_loc", "glue.locate",
        Json{{"template", f.tmpl.u8string()}, {"anchor", {kAnchor.x, kAnchor.y}}}},
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

/// 改 n_loc 的参数（定位门的用例只动定位）。
Json withLocate(Json doc, const Json& params) {
  for (Json& n : doc["nodes"]) {
    if (n["id"] == "n_loc")
      n["params"].update(params);
  }
  return doc;
}

Out run(const Json& doc, const cv::Mat& gray) {
  Session s(doc, {}, {}, false, 0, false,
            {exec::InjectedInput{"n_load", "image", Data::image(imageOf(gray))}});
  RunLog& log = s.wait();
  Out o;
  o.status = log.runStatus();
  if (o.status != "ok") {
    for (const Json& e : log.ofKind("node_state")) {
      if (e.value("state", "") == "error")
        MESSAGE(e.dump());
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
  if (x >= 404 && x <= 416)
    return Zone::Gap;
  if (x >= 564 && x <= 576)
    return Zone::NoInner;
  if ((x > 392 && x < 428) || (x > 552 && x < 588))
    return Zone::Edge;
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

} // namespace

TEST_CASE(
    "glue 飞拍：恒等位姿 —— 定位分数 ≥ 0.9、位姿≈0；逐点卡尺的距离中位误差 ≤ 0.3 px，断口 no_bead、抹掉的内边 no_inner") {
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

TEST_CASE(
    "glue 飞拍：只加噪声（σ≈6）或只平移半个像素、不转 —— 看得出角度的模板估出的角度 < 0.1°，中心不被假角度带偏") {
  // 防的回归：1.0.0 在 0° 直接用原图、别的角度经双线性插值，插值相当于轻微平滑，噪声下非零角度的 NCC 反而更高：
  // 噪声帧报 0.25°（2026-10-17 样本集 σ = 6 时中位 0.63°），杠杆臂末端偏出零点几 mm，比不定位还差。
  // 半像素帧防的是平移的峰值锁定：NCC 只在整像素上取分，半像素处的峰按三点抛物线插值会偏向整像素，
  // 上一版这一帧中心偏 0.12 px（angleRange = 0、根本不扫角度时也一样，角度≈0，与假角度无关）。
  // 扫角度时最终匹配按估计的位置挪亚像素相位，让峰落在整像素上，这一偏差没了；
  // angleRange = 0 与看不出角度的帧最终匹配不挪相位，仍有约 0.12 px（与 1.0.0 相同）。
  // 半像素相位造成的假角度在这个 220 px 模板上不明显，由下面 96 px 模板那一例防
  const Files f = teach();
  cv::Mat noisy;
  render(Rigid{}).convertTo(noisy, CV_32F);
  cv::Mat noise(noisy.size(), CV_32F);
  cv::RNG rng(101);
  rng.fill(noise, cv::RNG::NORMAL, 0.0, 6.0);
  noisy += noise;
  noisy.convertTo(noisy, CV_8U);
  const Rigid half{0.5, 0.5, 0.0};
  for (const auto& [frame, shift] :
       {std::pair{noisy, P2(0, 0)}, std::pair{render(half), P2(half.tx, half.ty)}}) {
    CAPTURE(shift.x);
    const Out o = run(flyshotGraph(f, false), frame);
    REQUIRE(o.status == "ok");
    INFO(o.pose.dump());
    CHECK(o.pose["ok"] == true);
    CHECK(o.pose["angleObservable"] == true); // 角度是估出来的，不是「看不出」被置 0
    CHECK(std::fabs(o.pose["angle"].get<double>()) < 0.1);
    CHECK(std::fabs(o.pose["offset"][0].get<double>() - shift.x) <= 0.1);
    CHECK(std::fabs(o.pose["offset"][1].get<double>() - shift.y) <= 0.1);
  }
}

TEST_CASE(
    "glue 飞拍：96 px 模板、只平移半个像素、不转 —— 看得出角度时估出的角度 < 0.15°") {
  // 防的回归：各角度不在同一个亚像素相位上比时，物体落在半像素上 0° 的分数被压低得多、别的角度转过去的小数部分
  // 不同、压低得少，最好的角度被推到恰好对齐整像素的那个。这个框（开口右边的竖边与胶条的一端）上
  // 1ffdc06 报 0.61°，只去掉相位对齐报 0.32°，这一版 ≤ 0.07°（复拍对 S001→S034 的 0.49° 也是这个原因）
  const cv::Rect box(700, 260, 96, 96);
  const cv::Mat tmpl = render(Rigid{})(box).clone();
  lyflow::packs::glue::LocateSpec spec;
  spec.anchor = P2(box.x, box.y);
  spec.searchRadius = 40;
  for (const P2 s : {P2(0.5, 0.5), P2(0.5, 0), P2(0, 0.5)}) {
    CAPTURE(s.x);
    CAPTURE(s.y);
    const auto pose = lyflow::packs::glue::locateTemplate(render(Rigid{s.x, s.y, 0.0}), tmpl, spec);
    INFO("score " << pose.score << " angle " << pose.angleDeg << " observable "
                  << pose.angleObservable);
    CHECK(pose.ok);
    CHECK(pose.angleObservable); // 角度是估出来的，不是「看不出」被置 0
    CHECK(std::fabs(pose.angleDeg) < 0.15);
  }
}

TEST_CASE(
    "glue 飞拍：真实转角离最近的粗扫角度约 0.4 步（−1.3°、0.2°）或 0.6° —— 角度误差 ≤ 0.02°") {
  // 防的回归：细扫五个角度里最好的落在窗口两端时不做抛物线，角度停在粗扫角度 ± 0.5 步上，
  // −1.3°、0.2° 整整差 0.05°；细扫中间那个沿用粗扫的分数（按 0° 上的位置对齐的相位）、与另外四个混着比，
  // 0.6° 差 0.023°
  const Files f = teach();
  for (const double deg : {-1.3, 0.2, 0.6}) {
    CAPTURE(deg);
    const Out o = run(flyshotGraph(f, false), render(Rigid{12, -7, deg}));
    REQUIRE(o.status == "ok");
    INFO(o.pose.dump());
    CHECK(o.pose["ok"] == true);
    CHECK(std::fabs(o.pose["angle"].get<double>() - deg) <= 0.02);
  }
}

TEST_CASE("glue 飞拍：模板里唯一的特征贴着框边（离框边 < 4 px）—— 平移照样准，不漂 1…2 px") {
  // 防的回归：上一版预平滑后把模板四边各裁 4 px，贴着框边的细条整个被裁掉，只剩平滑的尾巴，
  // 平移漂 1.2…1.9 px、分数 0.95 以上照常成功（不裁的 1.0.0 是 0.01…0.13 px）
  constexpr int kAx = 100, kAy = 80, kBox = 40;
  // 均匀背景上一个 L 形暗角：两条 2 px 宽的条落在模板局部 x = 1…2、y = 1…2，往框外延伸
  auto frame = [&](double tx, double ty, int seed) {
    cv::Mat img(260, 320, CV_32F);
    constexpr int kSub = 4;
    for (int y = 0; y < img.rows; ++y) {
      for (int x = 0; x < img.cols; ++x) {
        double acc = 0;
        for (int j = 0; j < kSub; ++j) {
          for (int i = 0; i < kSub; ++i) {
            const double px = x - 0.5 + (i + 0.5) / kSub - tx - kAx;
            const double py = y - 0.5 + (j + 0.5) / kSub - ty - kAy;
            const bool bar = (px >= 0.5 && px < 2.5 && py >= 0.5 && py < 150) ||
                             (py >= 0.5 && py < 2.5 && px >= 0.5 && px < 150);
            acc += bar ? 40.0 : 150.0;
          }
        }
        img.at<float>(y, x) = static_cast<float>(acc / (kSub * kSub));
      }
    }
    cv::Mat noise(img.size(), CV_32F);
    cv::RNG rng(seed);
    rng.fill(noise, cv::RNG::NORMAL, 0.0, 4.0);
    img += noise;
    cv::Mat out;
    img.convertTo(out, CV_8U);
    return out;
  };
  lyflow::packs::glue::LocateSpec spec;
  spec.anchor = P2(kAx, kAy);
  spec.searchRadius = 20;
  spec.angleRange = 0;
  const cv::Mat tmpl = frame(0, 0, 1)(cv::Rect(kAx, kAy, kBox, kBox)).clone();
  const P2 teachCenter = P2(kAx, kAy) + P2((kBox - 1) / 2.0, (kBox - 1) / 2.0);
  for (const P2 t : {P2(0, 0), P2(4.5, 3.15), P2(-6.3, -4.41), P2(9.2, 6.44)}) {
    CAPTURE(t.x);
    const auto pose = lyflow::packs::glue::locateTemplate(frame(t.x, t.y, 7), tmpl, spec);
    INFO("score " << pose.score << " reason " << pose.reason << " center " << pose.center.x << ", "
                  << pose.center.y);
    CHECK(pose.ok);
    CHECK(std::hypot(pose.center.x - teachCenter.x - t.x, pose.center.y - teachCenter.y - t.y) <=
          0.3);
  }
}

TEST_CASE(
    "glue 飞拍：示教框外 0…1.5 px 紧挨着平行于框边的强暗边、模板带 4 px 外圈 —— 平移误差 ≤ 0.3 px（只找平移与扫 ±3° 都是）") {
  // 防的回归：模板只有示教框自己时整块平滑、框外按框边像素延伸（BORDER_REPLICATE），搜索区却按真实邻域平滑，
  // 框边上少了那条暗边的平滑尾巴，平移被拉偏：dcc726b 上离框边 0 px 时 0.61 px（±3° 时 0.65 px）、0.5 px 时
  // 0.33…0.38 px，分数仍 > 0.96、照常成功。带外圈后示教框与图上同一处平滑得一样（无噪声、整像素平移时分数 1.00000）。
  // 剩下的 ≤ 0.23 px 不是平滑造成的：暗边紧挨着框边时 NCC 峰两侧不对称，三点抛物线插值的峰顶偏向平坦一侧，
  // 无噪声、整像素平移、分数 1.00000 时也偏 0.097 px；暗边离框边 10 px 时 ≤ 0.03 px
  constexpr int kAx = 100, kAy = 80, kBox = 48, kPad = 4;
  auto frame = [&](double gap, double tx, double ty, int seed) {
    cv::Mat img(260, 320, CV_32F);
    constexpr int kSub = 4;
    for (int y = 0; y < img.rows; ++y) {
      for (int x = 0; x < img.cols; ++x) {
        double acc = 0;
        for (int j = 0; j < kSub; ++j) {
          for (int i = 0; i < kSub; ++i) {
            const double px = x - 0.5 + (i + 0.5) / kSub - tx - kAx;
            const double py = y - 0.5 + (j + 0.5) / kSub - ty - kAy;
            double v = 160; // 示教框里的特征：暗十字 + 暗圆盘
            if (std::hypot(px - 24, py - 24) < 8 || (std::fabs(px - 24) < 1.5 && py > 6 && py < 42) ||
                (std::fabs(py - 24) < 1.5 && px > 6 && px < 42))
              v = 60;
            if (px < -0.5 - gap || py < -0.5 - gap) // 示教框左、上两边外 gap px 起的暗半平面
              v = 30;
            acc += v;
          }
        }
        img.at<float>(y, x) = static_cast<float>(acc / (kSub * kSub));
      }
    }
    cv::Mat noise(img.size(), CV_32F);
    cv::RNG rng(seed);
    rng.fill(noise, cv::RNG::NORMAL, 0.0, 1.0);
    img += noise;
    cv::Mat out;
    img.convertTo(out, CV_8U);
    return out;
  };
  const P2 teachCenter = P2(kAx, kAy) + P2((kBox - 1) / 2.0, (kBox - 1) / 2.0);
  for (const double gap : {0.0, 0.5, 1.5}) {
    CAPTURE(gap);
    const cv::Mat tmpl = frame(gap, 0, 0, 1)(cv::Rect(kAx - kPad, kAy - kPad, kBox + 2 * kPad,
                                                      kBox + 2 * kPad))
                             .clone();
    for (const double range : {0.0, 3.0}) {
      CAPTURE(range);
      lyflow::packs::glue::LocateSpec spec;
      spec.anchor = P2(kAx, kAy);
      spec.templatePad = kPad;
      spec.searchRadius = 20;
      spec.angleRange = range;
      for (const P2 t : {P2(0, 0), P2(3.25, 2.5), P2(6.5, 4.75), P2(9.0, 7.0), P2(12.5, 10.25)}) {
        CAPTURE(t.x);
        const auto pose = lyflow::packs::glue::locateTemplate(frame(gap, t.x, t.y, 7), tmpl, spec);
        INFO("score " << pose.score << " reason " << pose.reason << " center " << pose.center.x
                      << ", " << pose.center.y);
        CHECK(pose.ok);
        CHECK(pose.teachCenter.x == teachCenter.x); // teachCenter 按示教框算，不含外圈
        CHECK(std::hypot(pose.center.x - teachCenter.x - t.x, pose.center.y - teachCenter.y - t.y) <=
              0.3);
      }
    }
  }
}

TEST_CASE(
    "glue 飞拍：templatePad —— 1…3 加载期判错；模板文件去掉外圈后没有示教框判 bad_param，不当成别的大小去匹配") {
  // 防的回归：外圈比预平滑核半径窄时框边照样按边延伸、偏差还在，不能悄悄接受；模板文件与 templatePad 对不上
  // （例如宿主把没带外圈的小模板配上 templatePad）时不能裁出空的或负尺寸的示教框
  const Files f = teach();
  auto loadErrors = [&](int pad) {
    std::vector<std::string> paths;
    const Json doc = withLocate(flyshotGraph(f, false), {{"templatePad", pad}});
    for (const Json& d : Json::parse(exec::validateGraphJson(doc.dump(), {}))) {
      if (d.value("severity", "") == "error")
        paths.push_back(d.value("paramPath", ""));
    }
    return paths;
  };
  CHECK(loadErrors(0).empty());
  CHECK(loadErrors(2) == std::vector<std::string>{"templatePad"});
  CHECK(loadErrors(4).empty());

  const auto small = f.dir / "small.png";
  writeTemplate(small, render(Rigid{}), cv::Rect(650, 470, 8, 20)); // 8 px 宽，去掉两边各 4 px 后宽 0
  const Json doc = withLocate(flyshotGraph(f, false),
                              {{"template", small.u8string()}, {"anchor", {654, 474}}, {"templatePad", 4}});
  Session s(doc, {}, {}, false, 0, false,
            {exec::InjectedInput{"n_load", "image", Data::image(imageOf(render(Rigid{})))}});
  const Json e = s.wait().nodeEvent("n_loc", "error");
  CHECK(e["error"]["code"] == "bad_param");
  CHECK(e["error"]["paramPath"] == "template");
}

TEST_CASE("glue 飞拍：模板在图上根本没有 —— pose.ok = false，全部站 pose_fail，不是执行错误") {
  const Files f = teach();
  const Out o = run(flyshotGraph(f, false), render(Rigid{}, /*part=*/false));
  REQUIRE(o.status == "ok");
  CHECK(o.pose["ok"] == false);
  CHECK(o.measure["poseOk"] == false);
  for (const Json& st : o.measure["status"])
    CHECK(st == "pose_fail");
  CHECK(o.measure["counts"]["poseFail"] == o.measure["count"]);
}

TEST_CASE(
    "glue 飞拍：接 image.load_calib 的合成单应 —— 内边→胶中线、内边→近边出 mm，误差 ≤ 0.02 mm") {
  const Files f = teach();
  std_image::PlaneCalib calib;
  calib.H = {0.05, 0.002, 1.0, 0.001, 0.05, 2.0, 1e-6, 2e-6, 1.0};
  std::ofstream(f.calib, std::ios::binary) << Json{
      {"kind", "Record"},
      {"type", "image.PlaneCalib"},
      {"data", std_image::planeCalibJson(calib)}}.dump();
  const Rigid T{12, -9, 1.0};
  const Out o = run(flyshotGraph(f, true), render(T));
  REQUIRE(o.status == "ok");
  const Json& m = o.measure;
  CHECK(m["unit"] == "mm");
  int checked = 0;
  for (std::size_t i = 0; i < m["count"].get<std::size_t>(); ++i) {
    const double x = 200 + 4.0 * i;
    if (zoneOf(x) != Zone::Normal)
      continue;
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
      if (d.value("severity", "") == "error")
        paths.push_back(d.value("paramPath", ""));
    }
    return paths;
  };
  auto with = [](const char* key, Json value) {
    return Json{{"stations", "x.json"}, {key, std::move(value)}};
  };
  CHECK(errors(Json{{"stations", "x.json"}}).empty());
  CHECK(errors(with("innerSearch", {-3, -40})) == std::vector<std::string>{"innerSearch"});
  CHECK(errors(with("beadSearch", {40, -15})) == std::vector<std::string>{"beadSearch"});
  CHECK(errors(with("searchHalf", 30)) == std::vector<std::string>{"searchHalf"});
}

TEST_CASE(
    "glue 飞拍：searchRadius 是硬边界 —— 真实偏移超出它时，匹配中心不出这个范围，峰贴在窗边判 at_search_edge") {
  // 防的回归：真值刚出窗时，贴着窗边的匹配分数仍很高（这里 > 0.9），1.0.0 报成功、位姿差几个 px
  const Files f = teach();
  const Json doc = withLocate(flyshotGraph(f, false), {{"searchRadius", 20}});
  for (const Rigid T : {Rigid{24, 0, 0.0}, Rigid{40, 12, 0.0}}) {
    CAPTURE(T.tx);
    const Out o = run(doc, render(T));
    REQUIRE(o.status == "ok");
    INFO(o.pose.dump());
    CHECK(std::fabs(o.pose["offset"][0].get<double>()) <= 21.0);
    CHECK(std::fabs(o.pose["offset"][1].get<double>()) <= 21.0);
    CHECK(o.pose["score"].get<double>() >= 0.6); // 分数过线，挡住它的只能是贴边门
    CHECK(o.pose["ok"] == false);
    CHECK(o.pose["reason"] == "at_search_edge");
  }
  const Out near = run(doc, render(Rigid{12, -9, 0.0}));
  REQUIRE(near.status == "ok");
  CHECK(near.pose["ok"] == true);
  CHECK(near.pose["reason"] == "");
  CHECK(near.pose["score"].get<double>() >= 0.9);
}

TEST_CASE(
    "glue 飞拍：angleRange 是转角的边界 —— 2.8° 贴着 3° 照常成功、角度不截断；4.5° 超出判 at_angle_edge") {
  // 防的回归：只看「最佳粗扫角度是不是端点」时，真值离 ±angleRange 不到半步（2.8° 离 3° 端点最近）也判贴边；
  // 转角真超范围时 1.0.0 报成功、角度被截在 angleRange 上
  const Files f = teach();
  const Out inside = run(flyshotGraph(f, false), render(Rigid{0, 0, 2.8})); // 默认 angleRange = 3°
  REQUIRE(inside.status == "ok");
  INFO(inside.pose.dump());
  CHECK(inside.pose["ok"] == true);
  CHECK(std::fabs(inside.pose["angle"].get<double>() - 2.8) <= 0.1);

  const Out outside = run(flyshotGraph(f, false), render(Rigid{0, 0, 4.5}));
  REQUIRE(outside.status == "ok");
  INFO(outside.pose.dump());
  CHECK(outside.pose["score"].get<double>() >= 0.6);
  CHECK(outside.pose["ok"] == false);
  CHECK(outside.pose["reason"] == "at_angle_edge");

  // angleRange 比 angleStep（默认 0.5°）还小：上一版一个角度都不扫，转 1° 也报成功、angle 0、angleObservable = true
  const Json small = withLocate(flyshotGraph(f, false), {{"angleRange", 0.4}});
  const Out smallIn = run(small, render(Rigid{0, 0, 0.3}));
  REQUIRE(smallIn.status == "ok");
  INFO(smallIn.pose.dump());
  CHECK(smallIn.pose["ok"] == true);
  CHECK(smallIn.pose["angleObservable"] == true);
  CHECK(std::fabs(smallIn.pose["angle"].get<double>() - 0.3) <= 0.1);
  const Out smallOut = run(small, render(Rigid{0, 0, 1.0}));
  REQUIRE(smallOut.status == "ok");
  INFO(smallOut.pose.dump());
  CHECK(smallOut.pose["ok"] == false);
  CHECK(smallOut.pose["reason"] == "at_angle_edge");
  // angleRange = 0.1°：上一版护栏跟着缩到 ±0.2°，这么窄的范围里分数差不出 0.004，这个模板也判看不出角度，
  // 转 1.1°、1.5° 照报成功（只补平移，中心偏 1.2…1.8 px）。护栏放在 ±(angleRange + angleStep)
  const Json tiny = withLocate(flyshotGraph(f, false), {{"angleRange", 0.1}});
  for (const double deg : {1.1, 1.5}) {
    CAPTURE(deg);
    const Out tinyOut = run(tiny, render(Rigid{0, 0, deg}));
    REQUIRE(tinyOut.status == "ok");
    INFO(tinyOut.pose.dump());
    CHECK(tinyOut.pose["ok"] == false);
    CHECK(tinyOut.pose["reason"] == "at_angle_edge");
  }
}

TEST_CASE(
    "glue 飞拍：角度约束弱、看不出角度的模板转出 angleRange —— 判 at_angle_edge，不按 0° 的平移报成功") {
  // 防的回归：看不出角度时直接取 0° 上的平移，从不判 at_angle_edge；真实转角在范围外时 0° 上的平移也跟着偏，
  // 上一版这几帧都报成功、中心偏 3.4…4 px（2026-10-17 样本集 +4° 时 1–2/36 张报成功、偏 7.5…12.7 px）。
  // 场景：三团很软的明暗斑（σ 21…37 px），各角度分数差 < 0.004；整图绕模板外的 (200, 150) 转
  constexpr double kPx = 200, kPy = 150;
  auto frame = [&](double deg, double sigma, int seed) {
    const double a = -deg * kPi / 180.0, c = std::cos(a), s = std::sin(a);
    cv::Mat img(400, 500, CV_32F);
    for (int y = 0; y < img.rows; ++y) {
      for (int x = 0; x < img.cols; ++x) {
        const double dx = x - kPx, dy = y - kPy;
        const double u = c * dx - s * dy + kPx, v = s * dx + c * dy + kPy;
        double value = 128;
        for (const auto& [bx, by, bs, amp] :
             {std::array{351.0, 308.0, 37.0, -21.0}, std::array{390.0, 230.0, 21.0, -54.0},
              std::array{338.0, 326.0, 34.0, 31.0}}) {
          const double r2 = (u - bx) * (u - bx) + (v - by) * (v - by);
          value += amp * std::exp(-0.5 * r2 / (bs * bs));
        }
        img.at<float>(y, x) = static_cast<float>(value);
      }
    }
    cv::Mat noise(img.size(), CV_32F);
    cv::RNG rng(seed);
    rng.fill(noise, cv::RNG::NORMAL, 0.0, sigma);
    img += noise;
    cv::Mat out;
    img.convertTo(out, CV_8U);
    return out;
  };
  lyflow::packs::glue::LocateSpec spec;
  spec.anchor = P2(300, 220);
  spec.searchRadius = 40;
  spec.angleRange = 3;
  const cv::Mat tmpl = frame(0, 0.0, 1)(cv::Rect(300, 220, 96, 96)).clone();
  for (const double deg : {4.0, -4.0, 4.5}) {
    CAPTURE(deg);
    const auto pose = lyflow::packs::glue::locateTemplate(frame(deg, 2.0, 9), tmpl, spec);
    INFO("score " << pose.score << " angle " << pose.angleDeg << " observable "
                  << pose.angleObservable);
    CHECK_FALSE(pose.angleObservable); // 走的确实是看不出角度的分支
    CHECK_FALSE(pose.ok);
    CHECK(pose.reason == "at_angle_edge");
  }
  // 范围内照旧：看不出角度、报成功（0° 的平移，宿主按「转角没补偿」处理）
  const auto inRange = lyflow::packs::glue::locateTemplate(frame(0, 2.0, 9), tmpl, spec);
  CHECK(inRange.ok);
  CHECK_FALSE(inRange.angleObservable);
}

TEST_CASE(
    "glue 飞拍：圆孔模板看不出角度 —— angle 报 0、angleObservable = false，平移照常求、中心准，不判 at_angle_edge") {
  // 防的回归：旋转对称的模板各角度分数几乎一样，以前照样挑一个角度报出来（这几帧上 0.75…3°），
  // 宿主按这个假角度绕模板中心转测量点，杠杆臂一长就偏出零点几 mm；更早还会因为挑到扫描端点判 at_angle_edge。
  // 而设计里定位特征首选的就是孔
  const Files f = teach();
  const auto hole = f.dir / "hole.png";
  writeTemplate(hole, render(Rigid{}), cv::Rect(620, 440, 80, 80)); // (660, 480) 的孔
  const Json doc =
      withLocate(flyshotGraph(f, false), {{"template", hole.u8string()}, {"anchor", {620, 440}}});
  const P2 teachCenter(620 + 79 / 2.0, 440 + 79 / 2.0);
  for (const Rigid T : {Rigid{3.3, -2.6, -1.75}, Rigid{-4.2, 1.7, -0.75}, Rigid{2.6, 3.4, 0.5},
                        Rigid{-1.3, -3.8, 1.5}}) {
    CAPTURE(T.deg);
    const Out o = run(doc, render(T));
    REQUIRE(o.status == "ok");
    INFO(o.pose.dump());
    CHECK(o.pose["ok"] == true);
    CHECK(o.pose["angleObservable"] == false);
    CHECK(o.pose["angle"].get<double>() == 0.0);
    const P2 want = T.apply(teachCenter);
    CHECK(std::fabs(o.pose["center"][0].get<double>() - want.x) <= 0.3);
    CHECK(std::fabs(o.pose["center"][1].get<double>() - want.y) <= 0.3);
  }
}

TEST_CASE(
    "glue 飞拍：搜索区里有两个一样的孔 —— peakRatio≈1，maxPeakRatio = 0.9 时判 ambiguous；只有一个孔时 peakRatio 明显低、照常成功") {
  // 防的回归：模板在搜索区里还有一处同样的特征时，NCC 随便挑一处报成功，位置可能差一个孔距。
  // 不写 maxPeakRatio 时按算子默认（1 = 不判），1.0.0 存下的图结果不变。
  const Files f = teach();
  const auto hole = f.dir / "hole.png";
  writeTemplate(hole, render(Rigid{}), cv::Rect(620, 440, 80, 80)); // (660, 480) 的孔
  auto graph = [&](const Json& extra) {
    Json params = {{"template", hole.u8string()}, {"anchor", {620, 440}}, {"searchRadius", 100}};
    params.update(extra);
    return withLocate(flyshotGraph(f, false), params);
  };
  const cv::Mat twinFrame = render(Rigid{}, true, /*twinHole=*/true);
  const Out twin = run(graph({{"maxPeakRatio", 0.9}}), twinFrame);
  REQUIRE(twin.status == "ok");
  INFO(twin.pose.dump());
  CHECK(twin.pose["peakRatio"].get<double>() >= 0.95);
  CHECK(twin.pose["ok"] == false);
  CHECK(twin.pose["reason"] == "ambiguous");

  const Out byDefault = run(graph(Json::object()), twinFrame);
  REQUIRE(byDefault.status == "ok");
  CHECK(byDefault.pose["ok"] == true);

  const Out unique = run(graph({{"maxPeakRatio", 0.9}}), render(Rigid{}));
  REQUIRE(unique.status == "ok");
  INFO(unique.pose.dump());
  CHECK(unique.pose["ok"] == true);
  CHECK(unique.pose["reason"] == "");
  CHECK(unique.pose["peakRatio"].get<double>() <= 0.7);
}

TEST_CASE(
    "glue 飞拍：peakRatio 只认独立的第二个峰 —— 主峰再宽也不算；整窗高台上的第二个孔照样判 ambiguous") {
  // 防的回归：真实帧上峰很宽，按固定半径排除时主峰自己的侧坡（离开 20 px 仍 0.96）被当成第二个峰，
  // maxPeakRatio = 0.9 把唯一的特征判 ambiguous；反过来按「主峰半高连通」排除时，模板以明暗渐变为主、
  // 整个窗口都在 0.9 上下，高台上的第二个孔和主峰连成一片被一起排除，peakRatio 变成 0
  auto frame = [](double slope, double barSigma, bool twin, double tx, double ty, int seed) {
    cv::Mat img(400, 400, CV_32F);
    for (int y = 0; y < img.rows; ++y) {
      for (int x = 0; x < img.cols; ++x) {
        const double px = x - tx, py = y - ty;
        double v = 120.0 + slope * (px - 200.0);
        const double bx = (px - 200) / std::max(barSigma, 1.0), by = (py - 200) / 6;
        if (barSigma > 0) // 两头渐隐的软暗条：沿 x 很宽，沿 y 窄
          v -= 100.0 * std::exp(-0.5 * (bx * bx + by * by));
        else if (std::hypot(px - 200, py - 200) <= 6 ||
                 (twin && std::hypot(px - 200, py - 250) <= 6))
          v -= 40.0;
        img.at<float>(y, x) = static_cast<float>(v);
      }
    }
    cv::GaussianBlur(img, img, cv::Size(0, 0), 1.0);
    cv::Mat noise(img.size(), CV_32F);
    cv::RNG rng(seed);
    rng.fill(noise, cv::RNG::NORMAL, 0.0, 2.0);
    img += noise;
    cv::Mat out;
    img.convertTo(out, CV_8U);
    return out;
  };
  lyflow::packs::glue::LocateSpec spec;
  spec.anchor = P2(160, 160);
  spec.searchRadius = 70;
  spec.maxPeakRatio = 0.9;
  const cv::Rect box(160, 160, 80, 80);

  const cv::Mat barTeach = frame(0.05, 40, false, 0, 0, 1);
  const auto bar = lyflow::packs::glue::locateTemplate(frame(0.05, 40, false, 4.3, -2.7, 9),
                                                       barTeach(box).clone(), spec);
  INFO("bar peakRatio " << bar.peakRatio << " reason " << bar.reason);
  CHECK(bar.ok);
  CHECK(bar.peakRatio <= 0.5);

  const cv::Mat rampTeach = frame(0.6, 0, true, 0, 0, 1);
  const auto ramp = lyflow::packs::glue::locateTemplate(frame(0.6, 0, true, 4.3, -2.7, 9),
                                                        rampTeach(box).clone(), spec);
  INFO("ramp peakRatio " << ramp.peakRatio << " reason " << ramp.reason);
  CHECK(ramp.peakRatio >= 0.95);
  CHECK(ramp.reason == "ambiguous");
}

TEST_CASE("glue 飞拍：示教框离图边不到 2 px —— 判 out_of_image，不是永远判 at_search_edge") {
  // 防的回归：示教框贴着图边时，连示教图本身（零位移、分数 1）的峰都落在被图边裁过的窗边上，
  // 贴边门让这个夹具永远失败、原因还指向 searchRadius
  cv::Mat img(200, 300, CV_8UC1);
  cv::RNG rng(3);
  rng.fill(img, cv::RNG::UNIFORM, 40, 220);
  cv::GaussianBlur(img, img, cv::Size(0, 0), 2.0);
  lyflow::packs::glue::LocateSpec spec;
  spec.searchRadius = 30;
  for (const int x : {0, 1, 300 - 70 - 1, 2, 300 - 70 - 2}) {
    CAPTURE(x);
    spec.anchor = P2(x, 60);
    const auto pose =
        lyflow::packs::glue::locateTemplate(img, img(cv::Rect(x, 60, 70, 70)).clone(), spec);
    const bool tooClose = x < 2 || x + 70 > 300 - 2;
    CHECK(pose.ok == !tooClose);
    CHECK(pose.reason == (tooClose ? "out_of_image" : ""));
  }
  // 带 4 px 外圈的模板：示教框离图边不到 4 px 时带外圈的模板放不进这一帧，同样判 out_of_image
  // （外圈按示教图四边各镜像 4 px 后裁，模板大小总是对的）
  spec.templatePad = 4;
  cv::Mat teachImg;
  cv::copyMakeBorder(img, teachImg, 4, 4, 4, 4, cv::BORDER_REFLECT);
  for (const int x : {3, 300 - 70 - 3, 4, 300 - 70 - 4}) {
    CAPTURE(x);
    spec.anchor = P2(x, 60);
    const auto pose = lyflow::packs::glue::locateTemplate(
        img, teachImg(cv::Rect(x, 60, 78, 78)).clone(), spec);
    const bool tooClose = x < 4 || x + 70 > 300 - 4;
    CHECK(pose.ok == !tooClose);
    CHECK(pose.reason == (tooClose ? "out_of_image" : ""));
  }
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
  spec.beadTo = 10; // 胶远边在 t=14，已经落在找胶窗口之外
  auto measured = lyflow::packs::glue::measureCaliper(frame, P2(20, 80), P2(0, 1), spec);
  CHECK(measured.status == "incomplete_bead");
  spec.beadFrom = -3; // 近边 t=-8 被裁掉，远边在窗口内
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
  std::ifstream in(std::filesystem::path(__FILE__).parent_path().parent_path() /
                   "graphs/flyshot.lyflow.json");
  REQUIRE(in.good());
  const std::string graph(std::istreambuf_iterator<char>(in), {});
  const std::string params = Json{
      {"template", f.tmpl.u8string()},
      {"anchor", {590, 250}},
      {"stations", f.stations.u8string()},
      {"calib",
       f.calib.u8string()}}.dump();
  // 同一张图、同一组参数连续注入不同内容，第二帧不能命中第一帧的图像缓存。
  for (bool part : {true, false}) {
    const cv::Mat frame = render(Rigid{}, part);
    const unsigned stride = frame.cols + 16;
    std::vector<uint8_t> pixels(stride * frame.rows, 255);
    for (int y = 0; y < frame.rows; ++y)
      std::memcpy(pixels.data() + y * stride, frame.ptr(y), frame.cols);
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
    const auto cleanup = [](lyflow_run* run) {
      lyflow_run_join(run);
      lyflow_run_free(run);
    };
    std::unique_ptr<lyflow_run, decltype(cleanup)> handle(
        lyflow_run_start(graph.c_str(), &opts, nullptr, nullptr), cleanup);
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
  for (const Json& data :
       {Json::array(), Json{{"points", Json::array()}, {"normals", Json::array()}},
        Json{{"points", {{20, 30}}}, {"normals", {{0, 0}}}}}) {
    std::ofstream(f.stations, std::ios::binary) << data.dump();
    Session s(flyshotGraph(f, false), {}, {}, false, 0, false,
              {exec::InjectedInput{"n_load", "image", Data::image(imageOf(frame))}});
    const Json e = s.wait().nodeEvent("n_cal", "error");
    CHECK(e["error"]["code"] == "bad_param");
    CHECK(e["error"]["paramPath"] == "stations");
  }
}
