// image.board_calib「标定板标定」（glue-plan F3 / K6）：棋盘格图 → 工作平面单应 image.PlaneCalib。
#include <cmath>
#include <vector>

#include <opencv2/calib3d.hpp>
#include <opencv2/imgproc.hpp>

#include "lyflow/overlay.h"
#include "lyflow_cv/adapter.h"
#include "lyflow_cv/plane_calib.h"
#include "ops.h"

namespace lyflow::std_image {
namespace {

Status compute(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext& ctx) {
  const Image* img = inputs.get("image").asImage();
  const cv::Mat gray = img && img->scale == 1 ? cvx::gray8(*img) : cv::Mat{};
  if (gray.empty()) {
    return Status::Error(Phase::Execute, "bad_input", "标定需要全分辨率非空 u8 图像；其他位深先接 image.normalize，缩小预览请改用完整运行", {}, "image");
  }
  const auto pattern = params.vec2("pattern");
  const int cols = static_cast<int>(std::lround(pattern[0]));
  const int rows = static_cast<int>(std::lround(pattern[1]));
  const double square = params.number("square");

  std::vector<cv::Point2f> corners;
  const bool found = cv::findChessboardCornersSB(gray, cv::Size(cols, rows), corners,
                                                 cv::CALIB_CB_NORMALIZE_IMAGE | cv::CALIB_CB_ACCURACY);
  if (!found || corners.size() != static_cast<std::size_t>(cols * rows)) {
    return Status::Error(Phase::Execute, "no_board",
                         "没找到 " + std::to_string(cols) + "×" + std::to_string(rows) +
                             " 个内角点的棋盘格（看看 pattern 是不是内角点数、整块板是否都在图里）",
                         "pattern");
  }
  // 角点按行排（先沿一行的 cols 个，再下一行）；板上坐标原点在第一个角点，单位 mm
  std::vector<cv::Point2f> board;
  for (int j = 0; j < rows; ++j) {
    for (int i = 0; i < cols; ++i) board.emplace_back(static_cast<float>(i * square), static_cast<float>(j * square));
  }
  const cv::Mat Hm = cv::findHomography(corners, board, 0);
  if (Hm.empty()) {
    return Status::Error(Phase::Execute, "no_board", "角点退化，算不出单应", "pattern");
  }
  PlaneCalib calib;
  for (int k = 0; k < 9; ++k) calib.H[k] = Hm.at<double>(k / 3, k % 3);
  calib.unit = "mm";

  double sumSq = 0, maxErr = 0, spacingPx = 0;
  int spacingN = 0;
  for (std::size_t k = 0; k < corners.size(); ++k) {
    double X = 0, Y = 0;
    calib.map(corners[k].x, corners[k].y, &X, &Y);
    const double e = std::hypot(X - board[k].x, Y - board[k].y);
    sumSq += e * e;
    maxErr = std::max(maxErr, e);
    if ((k + 1) % cols != 0) {
      spacingPx += std::hypot(corners[k + 1].x - corners[k].x, corners[k + 1].y - corners[k].y);
      ++spacingN;
    }
  }
  const double rms = std::sqrt(sumSq / corners.size());
  const double mmPerPx = spacingN ? square / (spacingPx / spacingN) : 0;
  ctx.log(LogLevel::Info, "标定残差 RMS " + std::to_string(rms) + " mm，约 " + std::to_string(mmPerPx) + " mm/px");

  nlohmann::json data = planeCalibJson(calib);
  data["rms"] = rms;
  data["maxError"] = maxErr;
  data["mmPerPx"] = mmPerPx;
  data["pattern"] = {cols, rows};
  data["square"] = square;
  data["imageSize"] = {img->width, img->height};
  Record rec;
  rec.type = kPlaneCalibType;
  rec.data = std::move(data);
  outputs.set("calib", Data::record(std::move(rec)));

  Measurement m;
  m.value = rms;
  m.ok = true;
  m.unit = "mm";
  outputs.set("rms", Data::measurement(std::move(m)));

  Overlay2D ov;
  std::vector<Px> pts;
  for (const auto& c : corners) pts.push_back(Px{c.x, c.y});
  ov.points("part", pts, std::to_string(cols) + "×" + std::to_string(rows) + " 角点");
  ov.points("nozzle", {pts.front()}, "原点");
  outputs.set("overlay", ov.data());
  return Status::Ok();
}

std::vector<Issue> validate(const ParamView& params, const std::set<std::string>&) {
  const auto pattern = params.vec2("pattern");
  for (double n : pattern) {
    if (!std::isfinite(n) || n < 3 || n > 100 || n != std::floor(n)) {
      return {Issue::error("bad_param", "棋盘内角点数需要 3–100 之间的整数", "pattern")};
    }
  }
  return {};
}

}  // namespace

void registerBoardCalib(Registry& r) {
  OperatorDesc op;
  op.id = "image.board_calib";
  op.version = "1.0.0";
  op.label = "标定板标定";
  op.category = "图像/标定";
  op.keywords = {"calib", "calibration", "chessboard", "homography", "标定", "棋盘格", "单应", "毫米"};
  op.doc =
      "棋盘格图 → 工作平面标定 image.PlaneCalib（图像 px → 板面 mm 的 3×3 单应）。"
      "findChessboardCornersSB 找内角点，全部角点一起求单应，报残差 RMS 与平均 mm/px。"
      "板要放在要量的那个平面上（涂胶飞拍：内边所在的高度）。原点在第一个角点。\n"
      "图里找不到棋盘是执行错误 no_board（这是一次性的离线步骤，要人去看）。";
  op.inputs = {Port{"image", "Image", "Image", "标定板图。三通道先转灰度。", true}};
  op.outputs = {
      Port{"calib", "Record", "Calib", "image.PlaneCalib：H、unit，另有 rms、maxError、mmPerPx、pattern、square、imageSize。", true},
      Port{"rms", "Measurement", "RMS", "角点映射回板面的残差 RMS（mm）。", true},
      Port{"overlay", "Record", "Overlay", "lyflow.overlay2d：找到的角点与原点。", true},
  };
  Param pattern;
  pattern.name = "pattern";
  pattern.type = ParamType::Vec2f;
  pattern.label = "Pattern";
  pattern.doc = "棋盘格的内角点数（列 × 行），不是格子数。";
  pattern.def = Value::vec({11, 8});
  pattern.componentLabels = {"Cols", "Rows"};
  pattern.min = 3.0;
  pattern.max = 100.0;
  pattern.step = 1.0;
  Param square;
  square.name = "square";
  square.type = ParamType::Float;
  square.label = "Square";
  square.doc = "一格的边长。";
  square.def = Value::number(5.0);
  square.unit = "mm";
  square.min = 0.01;
  op.params = {pattern, square};
  for (auto& p : op.params) p.tuningRole = "geometry";
  op.capabilities = {/*cancellable=*/false, /*previewable=*/false, /*deterministic=*/true};
  op.compute = &compute;
  op.validate = &validate;
  r.addOperator(std::move(op));
}

}  // namespace lyflow::std_image
