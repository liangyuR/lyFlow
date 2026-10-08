// 工作平面标定（glue-plan K6，第 23 条）：已知单应画出的棋盘 → image.board_calib 恢复的单应量距离；
// image.load_calib 读回的与写入的相同（中文路径）。
#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>
#include <opencv2/imgproc.hpp>

#include "exec/result_store.h"
#include "helpers.h"
#include "lyflow_cv/adapter.h"
#include "lyflow_cv/plane_calib.h"

using namespace lyflow;
using namespace lyflow::test;

namespace {

/// 板面 mm → 图像 px：8 px/mm、转 5°、加一点透视。
cv::Matx33d boardToImage() {
  const double a = 5.0 * CV_PI / 180.0;
  return cv::Matx33d(8 * std::cos(a), -8 * std::sin(a), 120, 8 * std::sin(a), 8 * std::cos(a), 90, 0.0004, 0.0002, 1);
}

cv::Point2d mapH(const cv::Matx33d& H, cv::Point2d p) {
  const cv::Vec3d v = H * cv::Vec3d(p.x, p.y, 1);
  return {v[0] / v[2], v[1] / v[2]};
}

/// 9×6 个内角点（10×7 格，格长 5 mm），外面一圈白边，再外面是灰背景。
cv::Mat renderBoard(bool board = true) {
  const cv::Matx33d inv = boardToImage().inv();
  cv::Mat img(480, 640, CV_32F);
  constexpr int kSub = 3;
  for (int y = 0; y < img.rows; ++y) {
    for (int x = 0; x < img.cols; ++x) {
      double acc = 0;
      for (int j = 0; j < kSub; ++j) {
        for (int i = 0; i < kSub; ++i) {
          const cv::Point2d b = mapH(inv, {x - 0.5 + (i + 0.5) / kSub, y - 0.5 + (j + 0.5) / kSub});
          double v = 120;
          if (board && b.x >= -5 && b.x <= 55 && b.y >= -5 && b.y <= 40) {
            v = 220;
            if (b.x >= 0 && b.x < 50 && b.y >= 0 && b.y < 35) {
              v = ((static_cast<int>(std::floor(b.x / 5)) + static_cast<int>(std::floor(b.y / 5))) % 2 == 0) ? 30 : 220;
            }
          }
          acc += v;
        }
      }
      img.at<float>(y, x) = static_cast<float>(acc / (kSub * kSub));
    }
  }
  cv::GaussianBlur(img, img, cv::Size(0, 0), 0.7);
  cv::Mat out;
  img.convertTo(out, CV_8U);
  return out;
}

Json calibGraph() {
  return makeGraph({N{"n_load", "io.load_image", Json{{"source", "inputs"}}},
                    N{"n_calib", "image.board_calib", Json{{"pattern", {9, 6}}, {"square", 5}}}},
                   {E{"n_load.image", "n_calib.image"}});
}

}  // namespace

TEST_CASE("image.board_calib：已知单应画出的棋盘 —— 恢复的单应量任意两点距离误差 ≤ 0.05 mm") {
  Image img;
  REQUIRE(cvx::fromMat(renderBoard(), img));
  Session s(calibGraph(), {}, {}, false, 0, false, {exec::InjectedInput{"n_load", "image", Data::image(img)}});
  REQUIRE(s.wait().runStatus() == "ok");
  Data d;
  REQUIRE(exec::ResultStore::instance().get(s.runId(), "n_calib", "calib", d));
  const Json data = d.asRecord()->data;
  std_image::PlaneCalib calib;
  std::string why;
  REQUIRE(std_image::parsePlaneCalib(data, &calib, &why));
  CHECK(calib.unit == "mm");
  CHECK(data["rms"].get<double>() < 0.02);
  CHECK(std::fabs(data["mmPerPx"].get<double>() - 0.125) < 0.01);
  // 原点落在哪个角、轴朝哪边由找角点的顺序定，所以比距离（与坐标系无关）
  const cv::Matx33d G = boardToImage();
  const cv::Point2d pairs[][2] = {{{5, 5}, {45, 30}}, {{10, 30}, {40, 5}}, {{25, 15}, {30, 20}}};
  for (const auto& pr : pairs) {
    const cv::Point2d a = mapH(G, pr[0]), b = mapH(G, pr[1]);
    const double want = std::hypot(pr[1].x - pr[0].x, pr[1].y - pr[0].y);
    CHECK(std::fabs(calib.distance(a.x, a.y, b.x, b.y) - want) <= 0.05);
  }
}

TEST_CASE("image.board_calib：图里没有棋盘是执行错误 no_board，指到 pattern") {
  Image img;
  REQUIRE(cvx::fromMat(renderBoard(false), img));
  Session s(calibGraph(), {}, {}, false, 0, false, {exec::InjectedInput{"n_load", "image", Data::image(img)}});
  const RunLog& log = s.wait();
  const Json e = log.nodeEvent("n_calib", "error");
  REQUIRE(e.is_object());
  REQUIRE_FALSE(e.empty());
  CHECK(e["error"]["code"] == "no_board");
  CHECK(e["error"]["paramPath"] == "pattern");
}

TEST_CASE("image.load_calib：中文路径下的标定文件读回的 H 与写入的逐位相同；整个 Record 或只有 data 都认") {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / std::filesystem::u8path("lyflow-标定");
  std::filesystem::create_directories(dir);
  std_image::PlaneCalib calib;
  calib.H = {0.04, 0.001, -3.5, -0.002, 0.041, 7.25, 1e-6, -2e-6, 1.0};
  const Json data = std_image::planeCalibJson(calib);
  const std::filesystem::path full = dir / std::filesystem::u8path("工位标定.json");
  const std::filesystem::path bare = dir / std::filesystem::u8path("只有data.json");
  std::ofstream(full, std::ios::binary) << Json{{"kind", "Record"}, {"type", "image.PlaneCalib"}, {"data", data}}.dump();
  std::ofstream(bare, std::ios::binary) << data.dump();
  for (const auto& file : {full, bare}) {
    Session s(makeGraph({N{"n", "image.load_calib", Json{{"path", file.u8string()}}}}, {}));
    REQUIRE(s.wait().runStatus() == "ok");
    Data d;
    REQUIRE(exec::ResultStore::instance().get(s.runId(), "n", "calib", d));
    CHECK(d.asRecord()->type == "image.PlaneCalib");
    CHECK(d.asRecord()->data["H"] == data["H"]);
  }
}

TEST_CASE("image.board_calib：少于三个、非整数和超大角点数在校验期拒绝") {
  for (const Json& pattern : {Json{2, 6}, Json{9.5, 6}, Json{101, 6}}) {
    Json doc = calibGraph();
    doc["nodes"][1]["params"]["pattern"] = pattern;
    const Json issues = Json::parse(exec::validateGraphJson(doc.dump(), {}));
    bool found = false;
    for (const auto& issue : issues) {
      if (issue.value("severity", "") == "error" && issue.value("paramPath", "") == "pattern") found = true;
    }
    CHECK(found);
  }
}

TEST_CASE("image.load_calib：不能把其他类型的 Record 当作标定") {
  const auto file = std::filesystem::temp_directory_path() / "lyflow-wrong-calib-type.json";
  std::ofstream(file) << Json{{"kind", "Record"}, {"type", "other.Type"},
                             {"data", std_image::planeCalibJson(std_image::PlaneCalib{})}}.dump();
  Session s(makeGraph({N{"n", "image.load_calib", Json{{"path", file.u8string()}}}}, {}));
  const Json e = s.wait().nodeEvent("n", "error");
  CHECK(e["error"]["code"] == "bad_param");
  CHECK(e["error"]["paramPath"] == "path");
}
