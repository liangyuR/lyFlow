#include <memory>
#include <mutex>

#include <opencv2/calib3d.hpp>

#include "lyflow/pose.h"
#include "p1_common.h"

namespace lyflow::ops {
namespace {

Param pathParam() {
  Param p;
  p.name = "path";
  p.label = "标定文件";
  p.type = ParamType::Path;
  p.def = Value::text("");
  p.mode = "open";
  p.filters = {FileFilter{"JSON", {"json"}}};
  return p;
}

std::string cameraFileKey(const ParamView& p) {
  std::error_code ec;
  const auto path = p.path("path");
  const auto size = std::filesystem::file_size(path, ec);
  if (ec)
    return {};
  const auto stamp = std::filesystem::last_write_time(path, ec);
  return ec ? std::string{}
            : std::to_string(size) + ":" + std::to_string(stamp.time_since_epoch().count());
}

struct RectificationMaps {
  cv::Mat x, y;
};

std::shared_ptr<const RectificationMaps> mapsFor(const calibration::Camera& camera) {
  using Entry = std::pair<std::string, std::shared_ptr<const RectificationMaps>>;
  static std::mutex mutex;
  static std::vector<Entry> cache;
  const std::string key = nlohmann::json{
      {"size", {camera.width, camera.height}},
      {"K", camera.matrix},
      {"D",
       camera.distortion}}.dump();
  {
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& entry : cache)
      if (entry.first == key)
        return entry.second;
  }
  auto maps = std::make_shared<RectificationMaps>();
  const cv::Mat k = image_p1::cameraMatrix(camera);
  cv::initUndistortRectifyMap(k, image_p1::distortion(camera), cv::Mat(), k,
                              cv::Size(camera.width, camera.height), CV_32FC1, maps->x, maps->y);
  const std::size_t bytes = maps->x.total() * sizeof(float) * 2;
  if (bytes <= 67108864) {
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& entry : cache)
      if (entry.first == key)
        return entry.second;
    std::size_t total = 0;
    for (const auto& entry : cache)
      total += entry.second->x.total() * sizeof(float) * 2;
    while (!cache.empty() && (cache.size() >= 4 || total + bytes > 67108864)) {
      total -= cache.front().second->x.total() * sizeof(float) * 2;
      cache.erase(cache.begin());
    }
    cache.emplace_back(key, maps);
  }
  return maps;
}

Status loadCamera(const Inputs&, const ParamView& p, Outputs& out, ExecContext& ctx) {
  const auto path = p.path("path");
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec || size > 1024 * 1024)
    return img::badParam("相机标定文件不可读或超过1MiB", "path");
  std::ifstream file(path, std::ios::binary);
  if (!file)
    return img::badParam("打不开相机标定文件", "path");
  nlohmann::json json;
  try {
    json =
        nlohmann::json::parse(file, [](int depth, nlohmann::json::parse_event_t, nlohmann::json&) {
          if (depth > 16)
            throw std::runtime_error("JSON深度超过16");
          return true;
        });
  } catch (const std::exception& e) {
    return img::badParam(std::string("相机标定JSON解析失败：") + e.what(), "path");
  }
  if (ctx.cancelled())
    return Status::Ok();
  if (!json.is_object() || json.value("type", nlohmann::json()) != calibration::kCameraType ||
      !json.contains("data") || (json.contains("kind") && json["kind"] != "Record") ||
      json.size() != (json.contains("kind") ? 3u : 2u))
    return img::badParam("文件需要type/data相机Record，不猜测未知YAML或设备格式", "path");
  Record record{calibration::kCameraType, json["data"]};
  calibration::Camera camera;
  std::string why;
  if (!calibration::parseCamera(record, camera, why))
    return img::badParam(why, "path");
  out.set("calib", Data::record(std::move(record)));
  return Status::Ok();
}

Status undistort(const Inputs& in, const ParamView&, Outputs& out, ExecContext& ctx) {
  const Image* input = in.get("image").asImage();
  auto s = image_p1::image(input);
  if (!s.ok)
    return s;
  calibration::Camera camera;
  s = image_p1::camera(in, *input, camera);
  if (!s.ok)
    return s;
  if (ctx.cancelled())
    return Status::Ok();
  cv::Mat result, coverage, valid;
  const auto maps = mapsFor(camera);
  cv::remap(cvx::view(*input), result, maps->x, maps->y, cv::INTER_LINEAR, cv::BORDER_CONSTANT,
            cv::Scalar(0));
  cv::remap(cv::Mat(input->height, input->width, CV_32F, cv::Scalar(1)), coverage, maps->x, maps->y,
            cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(0));
  cv::compare(coverage, 1.0, valid, cv::CMP_GE);
  Record record{calibration::kCameraType,
                {{"imageSize", {camera.width, camera.height}},
                 {"cameraMatrix", camera.matrix},
                 {"distortion", nlohmann::json::array()},
                 {"frame", camera.frame},
                 {"unit", "m"}}};
  out.set("calib", Data::record(std::move(record)));
  s = image_p1::putImage(out, "valid", std::move(valid));
  if (!s.ok)
    return s;
  return image_p1::putImage(out, "image", std::move(result));
}

Status warp(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  const Image* input = in.get("image").asImage();
  auto s = image_p1::image(input);
  if (!s.ok)
    return s;
  const Record* record = in.get("calib").asRecord();
  calibration::Plane plane;
  std::string why;
  if (!record || !calibration::parsePlane(*record, plane, why))
    return img::badInput(why.empty() ? "缺少平面标定" : why, "calib");
  if (record->data.contains("imageSize")) {
    const auto& size = record->data["imageSize"];
    if (!poses::numericArray(size, 2, true) ||
        std::ceil(size[0].get<double>() / input->scale) != input->width ||
        std::ceil(size[1].get<double>() / input->scale) != input->height)
      return img::badInput("平面标定的已知图像尺寸与输入不匹配", "calib");
  }
  const auto width = p.integer("width"), height = p.integer("height");
  const auto origin = p.vec2("origin");
  const double pixelsPerUnit = p.number("pixelsPerUnit"), border = p.number("borderValue");
  if (width < 1 || height < 1 || width > 16384 || height > 16384 || width * height > 16777216 ||
      !std::isfinite(pixelsPerUnit) || pixelsPerUnit <= 0 || pixelsPerUnit > 1e9 ||
      !std::isfinite(origin[0]) || !std::isfinite(origin[1]) || !std::isfinite(border) ||
      (input->depth == PixelDepth::U8 && (border < 0 || border > 255)) ||
      (input->depth == PixelDepth::U16 && (border < 0 || border > 65535)) ||
      (input->depth == PixelDepth::F32 && std::abs(border) > std::numeric_limits<float>::max()))
    return img::badParam("输出尺寸、平面原点或像素密度非法", "width");
  cv::Mat h(3, 3, CV_64F, plane.H.data());
  const cv::Mat target = (cv::Mat_<double>(3, 3) << pixelsPerUnit, 0, -origin[0] * pixelsPerUnit, 0,
                          pixelsPerUnit, -origin[1] * pixelsPerUnit, 0, 0, 1);
  const cv::Mat source =
      (cv::Mat_<double>(3, 3) << input->scale, 0, 0, 0, input->scale, 0, 0, 0, 1);
  const cv::Mat mapping = target * h * source;
  if (!cv::checkRange(mapping))
    return img::badParam("透视变换系数超出有限范围", "pixelsPerUnit");
  if (ctx.cancelled())
    return Status::Ok();
  const int interpolation =
      p.choice("interpolation") == "nearest" ? cv::INTER_NEAREST : cv::INTER_LINEAR;
  cv::Mat result, sourceCoverage(input->height, input->width, CV_32F, cv::Scalar(1)), coverage,
      valid;
  cv::warpPerspective(cvx::view(*input), result, mapping, cv::Size(int(width), int(height)),
                      interpolation, cv::BORDER_CONSTANT, cv::Scalar::all(border));
  cv::warpPerspective(sourceCoverage, coverage, mapping, result.size(), interpolation,
                      cv::BORDER_CONSTANT, cv::Scalar(0));
  cv::compare(coverage, 1.0, valid, cv::CMP_GE);
  s = image_p1::putImage(out, "valid", std::move(valid));
  if (!s.ok)
    return s;
  s = image_p1::record(out, "mapping", "vision.PlaneWarp.v1",
                       {{"origin", {origin[0], origin[1]}},
                        {"pixelsPerUnit", pixelsPerUnit},
                        {"unit", plane.unit},
                        {"imageSize", {width, height}}});
  if (!s.ok)
    return s;
  return image_p1::putImage(out, "image", std::move(result));
}

Status calibratedDistance(const Inputs& in, const ParamView&, Outputs& out, ExecContext&) {
  const Record* record = in.get("calib").asRecord();
  const Point2D* a = in.get("a").asPoint2D();
  const Point2D* b = in.get("b").asPoint2D();
  calibration::Plane plane;
  std::string why;
  if (!record || !calibration::parsePlane(*record, plane, why))
    return img::badInput(why.empty() ? "缺少平面标定" : why, "calib");
  if (!a || !b || a->unit != Unit2D::Pixel || b->unit != Unit2D::Pixel)
    return img::badInput("物理量测输入必须是原图像素Point2D", "a");
  double ax, ay, bx, by;
  if (!plane.map(a->p[0], a->p[1], ax, ay) || !plane.map(b->p[0], b->p[1], bx, by))
    return img::badInput("量测点位于单应性无效区域", "a");
  Measurement measurement;
  measurement.value = std::hypot(ax - bx, ay - by) * plane.metersPerUnit;
  measurement.ok = std::isfinite(measurement.value);
  measurement.unit = "m";
  if (!measurement.ok)
    return img::badInput("物理量测结果非有限", "a");
  out.set("distance", Data::measurement(std::move(measurement)));
  return Status::Ok();
}

Status boardPose(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  const Image* input = in.get("image").asImage();
  auto s = image_p1::image(input);
  if (!s.ok)
    return s;
  calibration::Camera camera;
  s = image_p1::camera(in, *input, camera);
  if (!s.ok)
    return s;
  if (input->depth != PixelDepth::U8)
    return img::badInput("棋盘格检测支持u8灰度/RGB/RGBA图像");
  const int cols = static_cast<int>(p.integer("columns")),
            rows = static_cast<int>(p.integer("rows"));
  const double square = p.number("squareSize"), thickness = p.number("thickness");
  const double maxRms = p.number("maxRms"), ambiguity = p.number("ambiguityGap");
  if (cols < 2 || rows < 2 || cols > 32 || rows > 32 || !std::isfinite(square) || square <= 0 ||
      square > 10 || !std::isfinite(thickness) || thickness <= 0 || thickness > 10 ||
      !std::isfinite(maxRms) || maxRms <= 0 || !std::isfinite(ambiguity) || ambiguity < 0)
    return img::badParam("棋盘格尺寸、误差阈值或米制边长非法", "columns");
  if (!in.has("corners") && (input->width < 16 || input->height < 16))
    return Status::Error(Phase::Execute, "no_target", "图像尺寸不足以检测棋盘格角点");
  std::vector<cv::Point2d> pixels;
  if (in.has("corners")) {
    const Record* corners = in.get("corners").asRecord();
    std::string why;
    if (!corners || corners->type != "vision.BoardCorners.v1" ||
        !values::boundedJson(corners->data, why) || !corners->data.is_object() ||
        corners->data.size() != 2 ||
        corners->data.value("imageSize", nlohmann::json()) !=
            nlohmann::json::array({input->width, input->height}) ||
        !corners->data.contains("points") || !corners->data["points"].is_array() ||
        corners->data["points"].size() != std::size_t(cols * rows))
      return img::badInput("角点记录与图像尺寸/棋盘行列不匹配", "corners");
    for (const auto& point : corners->data["points"]) {
      if (!poses::numericArray(point, 2))
        return img::badInput("角点必须为有限像素坐标", "corners");
      const double x = point[0].get<double>(), y = point[1].get<double>();
      if (x < 0 || y < 0 || x >= input->width || y >= input->height)
        return img::badInput("角点超出图像边界", "corners");
      pixels.emplace_back(x, y);
    }
  } else {
    std::vector<cv::Point2f> found;
    const cv::Mat gray = image_p1::gray(*input);
    if (!cv::findChessboardCornersSB(gray, cv::Size(cols, rows), found,
                                     cv::CALIB_CB_NORMALIZE_IMAGE | cv::CALIB_CB_ACCURACY))
      return Status::Error(Phase::Execute, "no_target", "未检测到完整棋盘格角点");
    cv::cornerSubPix(gray, found, cv::Size(3, 3), cv::Size(-1, -1),
                     cv::TermCriteria(cv::TermCriteria::COUNT | cv::TermCriteria::EPS, 40, 1e-5));
    for (const auto& point : found)
      pixels.emplace_back(point.x, point.y);
  }
  if (ctx.cancelled())
    return Status::Ok();
  std::vector<cv::Point2f> hullInput, hull;
  for (const auto& point : pixels)
    hullInput.emplace_back(static_cast<float>(point.x), static_cast<float>(point.y));
  cv::convexHull(hullInput, hull);
  if (hull.size() < 3 || cv::contourArea(hull) < 1)
    return Status::Error(Phase::Execute, "degenerate_geometry", "棋盘格角点没有有效二维面积");
  std::vector<cv::Point3d> object;
  for (int y = 0; y < rows; ++y)
    for (int x = 0; x < cols; ++x)
      object.emplace_back(x * square, y * square, 0);
  std::vector<cv::Mat> rotations, translations;
  const cv::Mat k = image_p1::cameraMatrix(camera), d = image_p1::distortion(camera);
  cv::solvePnPGeneric(object, pixels, k, d, rotations, translations, false, cv::SOLVEPNP_IPPE);
  cv::Mat iterativeRotation, iterativeTranslation;
  if (cv::solvePnP(object, pixels, k, d, iterativeRotation, iterativeTranslation, false,
                   cv::SOLVEPNP_ITERATIVE)) {
    rotations.push_back(iterativeRotation);
    translations.push_back(iterativeTranslation);
  }
  struct Fit {
    cv::Mat rotation, translation;
    double rms = 0;
  };
  std::vector<Fit> fits;
  for (std::size_t i = 0; i < rotations.size(); ++i) {
    cv::Mat rotation;
    cv::Rodrigues(rotations[i], rotation);
    bool positiveDepth = cv::checkRange(rotation) && cv::checkRange(translations[i]);
    for (const auto& point : object)
      positiveDepth = positiveDepth && rotation.at<double>(2, 0) * point.x +
                                               rotation.at<double>(2, 1) * point.y +
                                               translations[i].at<double>(2) >
                                           0;
    if (!positiveDepth)
      continue;
    std::vector<cv::Point2d> projected;
    cv::projectPoints(object, rotations[i], translations[i], k, d, projected);
    double squared = 0;
    for (std::size_t j = 0; j < pixels.size(); ++j) {
      const auto delta = projected[j] - pixels[j];
      squared += delta.x * delta.x + delta.y * delta.y;
    }
    const double rms = std::sqrt(squared / pixels.size());
    if (std::isfinite(rms))
      fits.push_back({rotation, translations[i], rms});
  }
  std::sort(fits.begin(), fits.end(), [](const Fit& a, const Fit& b) { return a.rms < b.rms; });
  if (fits.empty() || fits.front().rms > maxRms)
    return Status::Error(Phase::Execute, "quality_rejected",
                         "棋盘格位姿正深度或重投影质量不满足阈值");
  bool ambiguous = false;
  for (std::size_t i = 1; i < fits.size(); ++i)
    ambiguous = ambiguous || (fits[i].rms - fits[0].rms <= ambiguity &&
                              cv::norm(fits[0].rotation - fits[i].rotation) > 0.01);
  if (ambiguous && p.flag("rejectAmbiguous"))
    return Status::Error(Phase::Execute, "ambiguous_pose", "平面PnP存在相近重投影误差的不同位姿");
  const auto& fit = fits.front();
  Transform transform;
  poses::Matrix3 matrix;
  for (int y = 0; y < 3; ++y)
    for (int x = 0; x < 3; ++x) {
      transform.m[y * 4 + x] = static_cast<float>(fit.rotation.at<double>(y, x));
      matrix[y * 3 + x] = fit.rotation.at<double>(y, x);
    }
  std::array<double, 3> center{};
  for (int y = 0; y < 3; ++y) {
    const double translation = fit.translation.at<double>(y);
    if (std::abs(translation) > std::numeric_limits<float>::max())
      return img::badInput("PnP平移超出Transform范围", "corners");
    transform.m[y * 4 + 3] = static_cast<float>(translation);
    center[y] = translation + matrix[y * 3] * (cols - 1) * square / 2 +
                matrix[y * 3 + 1] * (rows - 1) * square / 2;
  }
  std::vector<cv::Point3d> cameraPoints;
  cameraPoints.reserve(object.size());
  for (const auto& point : object) {
    std::array<double, 3> xyz{};
    for (int row = 0; row < 3; ++row)
      xyz[row] = double(transform.m[row * 4]) * point.x +
                 double(transform.m[row * 4 + 1]) * point.y +
                 double(transform.m[row * 4 + 2]) * point.z + double(transform.m[row * 4 + 3]);
    const cv::Point3d transformed(xyz[0], xyz[1], xyz[2]);
    if (!std::isfinite(transformed.x) || !std::isfinite(transformed.y) ||
        !std::isfinite(transformed.z) || transformed.z <= 0)
      return Status::Error(Phase::Execute, "quality_rejected", "原生位姿矩阵的棋盘格深度无效");
    cameraPoints.push_back(transformed);
  }
  std::vector<cv::Point2d> nativePixels;
  cv::projectPoints(cameraPoints, cv::Vec3d::all(0), cv::Vec3d::all(0), k, d, nativePixels);
  double nativeSquared = 0;
  for (std::size_t i = 0; i < pixels.size(); ++i) {
    const auto delta = nativePixels[i] - pixels[i];
    nativeSquared += delta.x * delta.x + delta.y * delta.y;
  }
  const double nativeRms = std::sqrt(nativeSquared / pixels.size());
  if (!std::isfinite(nativeRms) || nativeRms > maxRms)
    return Status::Error(Phase::Execute, "quality_rejected", "原生位姿矩阵的重投影误差超过阈值");
  Record pose = poses::make3D("calibration-board", camera.frame, center, poses::quaternion(matrix),
                              {(cols - 1) * square, (rows - 1) * square, thickness});
  std::string why;
  if (!poses::validate(pose, why))
    return img::badInput(why, "corners");
  out.set("pose", Data::transform(transform));
  out.set("instance", Data::record(std::move(pose)));
  Measurement rms;
  rms.value = nativeRms;
  rms.unit = "px";
  rms.ok = true;
  out.set("rms", Data::measurement(rms));
  nlohmann::json points = nlohmann::json::array();
  for (const auto& point : pixels)
    points.push_back({point.x, point.y});
  s = image_p1::record(
      out, "corners", "vision.BoardCorners.v1",
      {{"imageSize", {input->width, input->height}}, {"points", std::move(points)}});
  if (!s.ok)
    return s;
  return image_p1::record(out, "quality", "vision.BoardPoseQuality.v1",
                          {{"sourceFrame", "board"},
                           {"targetFrame", camera.frame},
                           {"cornerCount", pixels.size()},
                           {"rms", nativeRms},
                           {"ambiguous", ambiguous},
                           {"solver", "IPPE+iterative"},
                           {"unit", "m"}});
}

} // namespace

void registerImageP1Calibration(Registry& r) {
  auto op = image_p1::descriptor("image.load_camera_calib", "相机内参读取",
                                 "camera calibration intrinsics distortion JSON loading");
  op.outputs = {image_p1::recordPort("calib", calibration::kCameraType)};
  op.params = {pathParam()};
  op.compute = &loadCamera;
  op.externalKey = &cameraFileKey;
  r.addOperator(std::move(op));

  op = image_p1::descriptor("image.undistort", "相机图像去畸变",
                            "pinhole camera distortion correction remap");
  op.inputs = {img::imageIn(), image_p1::recordPort("calib", calibration::kCameraType)};
  op.outputs = {img::imageOut(), img::imageOut("valid"),
                image_p1::recordPort("calib", calibration::kCameraType)};
  op.doc = "相机标定必须与全分辨率输入一致；输出相同K、零畸变标定，线性重采样，边界填0。";
  op.compute = &undistort;
  r.addOperator(std::move(op));

  op = image_p1::descriptor("image.warp_perspective", "平面透视重采样",
                            "homography plane warp perspective physical resolution");
  op.inputs = {img::imageIn(), image_p1::recordPort("calib", calibration::kPlaneType)};
  op.outputs = {img::imageOut(), img::imageOut("valid"),
                image_p1::recordPort("mapping", "vision.PlaneWarp.v1")};
  auto width = img::intParam("width", "输出宽度", 640, 1, 16384, "固定输出像素。");
  auto height = img::intParam("height", "输出高度", 480, 1, 16384, "固定输出像素。");
  width.unit = height.unit = "px";
  width.absolute = height.absolute = true;
  op.params = {width,
               height,
               image_p1::vector("origin", "平面左上原点", {0, 0}),
               img::floatParam("pixelsPerUnit", "每标定单位像素数", 1, "单位沿用H的m/mm。"),
               img::floatParam("borderValue", "边界填充值", 0, "输入原值。"),
               img::enumParam("interpolation", "重采样", "linear",
                              {{"linear", "线性", ""}, {"nearest", "最近邻", ""}}, "")};
  op.doc = "H是原图像素到平面m/mm，输出密度与原点显式；valid仅标记完整插值采样有效像素。";
  op.compute = &warp;
  r.addOperator(std::move(op));

  op = image_p1::descriptor("image.calibrated_distance", "平面标定距离量测",
                            "calibrated physical planar point distance homography");
  op.inputs = {Port{"a", "Point2D", "A", "原图像素点。", true},
               Port{"b", "Point2D", "B", "原图像素点。", true},
               image_p1::recordPort("calib", calibration::kPlaneType)};
  op.outputs = {Port{"distance", "Measurement", "Distance", "米制平面距离。", true}};
  op.compute = &calibratedDistance;
  r.addOperator(std::move(op));

  op = image_p1::descriptor("image.board_pose", "棋盘格三维位姿估计",
                            "chessboard corner detection planar PnP camera pose reprojection");
  op.inputs = {img::imageIn(), image_p1::recordPort("calib", calibration::kCameraType),
               image_p1::recordPort("corners", "vision.BoardCorners.v1", false)};
  op.outputs = {Port{"pose", "Transform", "Pose", "board首内角点坐标→camera，行主序，米。", true},
                image_p1::recordPort("instance", poses::kPose3DType),
                Port{"rms", "Measurement", "RMS", "像素重投影RMS。", true},
                image_p1::recordPort("corners", "vision.BoardCorners.v1"),
                image_p1::recordPort("quality", "vision.BoardPoseQuality.v1")};
  op.params = {
      img::intParam("columns", "内角点列数", 7, 2, 32, "不是棋盘格方块数量。"),
      img::intParam("rows", "内角点行数", 5, 2, 32, "行优先角点顺序。"),
      img::floatParam("squareSize", "方格边长(m)", 0.01, "米制已知尺寸。"),
      img::floatParam("thickness", "标定板显示厚度(m)", 0.001, "仅工件尺寸元数据，不参与PnP。"),
      img::floatParam("maxRms", "最大重投影RMS(px)", 1, "超限拒绝。"),
      img::floatParam("ambiguityGap", "歧义误差差值(px)", 0.05,
                      "候选旋转不同且误差相近时标记歧义。"),
      img::boolParam("rejectAmbiguous", "拒绝歧义位姿", true, "平面PnP候选质量相近则报告错误。")};
  op.compute = &boardPose;
  r.addOperator(std::move(op));
}

} // namespace lyflow::ops
