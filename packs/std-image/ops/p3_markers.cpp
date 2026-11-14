#include "lyflow_cv/compat.h"
#if CV_VERSION_MAJOR > 4 || (CV_VERSION_MAJOR == 4 && CV_VERSION_MINOR >= 7)
#include <opencv2/objdetect/aruco_detector.hpp>
#else
#include <opencv2/aruco.hpp>
#endif

#include <numeric>

#include "p1_common.h"
#include "p3_stag.h"
#include "lyflow/pose.h"

namespace lyflow::ops {
namespace {
using Json = nlohmann::json;
struct DictionaryEntry {
  const char* name;
  int type;
};
const DictionaryEntry dictionaries[] = {{"DICT_4X4_50", cv::aruco::DICT_4X4_50},
                                        {"DICT_4X4_100", cv::aruco::DICT_4X4_100},
                                        {"DICT_4X4_250", cv::aruco::DICT_4X4_250},
                                        {"DICT_4X4_1000", cv::aruco::DICT_4X4_1000},
                                        {"DICT_5X5_50", cv::aruco::DICT_5X5_50},
                                        {"DICT_5X5_100", cv::aruco::DICT_5X5_100},
                                        {"DICT_5X5_250", cv::aruco::DICT_5X5_250},
                                        {"DICT_5X5_1000", cv::aruco::DICT_5X5_1000},
                                        {"DICT_6X6_50", cv::aruco::DICT_6X6_50},
                                        {"DICT_6X6_100", cv::aruco::DICT_6X6_100},
                                        {"DICT_6X6_250", cv::aruco::DICT_6X6_250},
                                        {"DICT_6X6_1000", cv::aruco::DICT_6X6_1000},
                                        {"DICT_7X7_50", cv::aruco::DICT_7X7_50},
                                        {"DICT_7X7_100", cv::aruco::DICT_7X7_100},
                                        {"DICT_7X7_250", cv::aruco::DICT_7X7_250},
                                        {"DICT_7X7_1000", cv::aruco::DICT_7X7_1000},
                                        {"DICT_ARUCO_ORIGINAL", cv::aruco::DICT_ARUCO_ORIGINAL},
                                        {"DICT_APRILTAG_16h5", cv::aruco::DICT_APRILTAG_16h5},
                                        {"DICT_APRILTAG_25h9", cv::aruco::DICT_APRILTAG_25h9},
                                        {"DICT_APRILTAG_36h10", cv::aruco::DICT_APRILTAG_36h10},
                                        {"DICT_APRILTAG_36h11", cv::aruco::DICT_APRILTAG_36h11}};
Param scalar(const char* name, const char* label, double value, double min, double max,
             const char* unit = "") {
  auto p = img::floatParam(name, label, value, "");
  p.min = min;
  p.max = max;
  p.unit = unit;
  return p;
}
struct PoseFit {
  cv::Mat vector, translation, rotation;
  double rms = 0;
};
bool candidate(const std::vector<cv::Point3d>& object, const std::vector<cv::Point2d>& pixels,
               const cv::Mat& k, const cv::Mat& d, const cv::Mat& r, const cv::Mat& t,
               PoseFit& fit) {
  if (r.total() != 3 || t.total() != 3 || !cv::checkRange(r) || !cv::checkRange(t))
    return false;
  cv::Mat rotation;
  cv::Rodrigues(r, rotation);
  if (!cv::checkRange(rotation))
    return false;
  for (const auto& point : object) {
    double z = rotation.at<double>(2, 0) * point.x + rotation.at<double>(2, 1) * point.y +
               rotation.at<double>(2, 2) * point.z + t.at<double>(2);
    if (!std::isfinite(z) || z <= 0)
      return false;
  }
  std::vector<cv::Point2d> projected;
  cv::projectPoints(object, r, t, k, d, projected);
  double sum = 0;
  for (std::size_t i = 0; i < pixels.size(); ++i) {
    const auto delta = projected[i] - pixels[i];
    sum += delta.dot(delta);
  }
  const double rms = std::sqrt(sum / pixels.size());
  if (!std::isfinite(rms))
    return false;
  fit = {r.clone(), t.clone(), std::move(rotation), rms};
  return true;
}
void paint(cv::Mat& image, const std::vector<cv::Point2d>& corners, bool accepted) {
  const cv::Scalar color = image.channels() == 1 ? cv::Scalar(accepted ? 255 : 128)
                           : accepted            ? cv::Scalar(0, 220, 80)
                                                 : cv::Scalar(255, 96, 0);
  for (int i = 0; i < 4; ++i)
    cv::line(image, corners[i], corners[(i + 1) % 4], color, 2, cv::LINE_AA);
}
Status estimate(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  try {
    const Image* input = in.get("image").asImage();
    auto s = image_p1::image(input, "image", false, 4194304);
    if (!s.ok)
      return s;
    if (input->depth != PixelDepth::U8 || input->scale != 1)
      return img::badInput("识别码检测需要全分辨率u8灰度/RGB/RGBA图像");
    calibration::Camera camera;
    s = image_p1::camera(in, *input, camera);
    if (!s.ok)
      return s;
    const double side = p.number("markerSize"), thickness = p.number("thickness"),
                 maxRms = p.number("maxRms"), gap = p.number("ambiguityGap");
    if (!std::isfinite(side) || side <= 0 || side > 10 || !std::isfinite(thickness) ||
        thickness <= 0 || thickness > 10 || !std::isfinite(maxRms) || maxRms <= 0 ||
        maxRms > 1000 || !std::isfinite(gap) || gap < 0)
      return img::badParam("识别码尺寸、厚度或质量阈值非法", "markerSize");
    const DictionaryEntry* entry = nullptr;
    for (const auto& dict : dictionaries)
      if (p.choice("dictionary") == dict.name)
        entry = &dict;
    const bool stag = p.choice("dictionary") == "STAG_HD15";
    if (!entry && !stag)
      return Status::Error(Phase::Execute, "unsupported_dictionary", "不支持该识别码字典",
                           "dictionary");
    const std::string dictionary = p.choice("dictionary");
    if (ctx.cancelled())
      return Status::Ok();
    std::vector<std::vector<cv::Point2f>> found, rejected;
    std::vector<int> ids;
    const cv::Mat gray = image_p1::gray(*input);
    if (stag) {
      s = detectSTagHD15(gray, found, ids, ctx);
      if (!s.ok)
        return s;
    } else {
#if CV_VERSION_MAJOR > 4 || (CV_VERSION_MAJOR == 4 && CV_VERSION_MINOR >= 7)
      cv::aruco::DetectorParameters parameters;
      parameters.cornerRefinementMethod = cv::aruco::CORNER_REFINE_SUBPIX;
      parameters.minMarkerPerimeterRate = p.number("minPerimeterRate");
      cv::aruco::ArucoDetector detector(cv::aruco::getPredefinedDictionary(entry->type),
                                        parameters);
      detector.detectMarkers(gray, found, ids, rejected);
#else
      auto parameters = cv::aruco::DetectorParameters::create();
      parameters->cornerRefinementMethod = cv::aruco::CORNER_REFINE_SUBPIX;
      parameters->minMarkerPerimeterRate = p.number("minPerimeterRate");
      cv::aruco::detectMarkers(gray, cv::aruco::getPredefinedDictionary(entry->type), found, ids,
                               parameters, rejected);
#endif
    }
    if (ctx.cancelled())
      return Status::Ok();
    if (found.size() != ids.size() ||
        ids.size() > static_cast<std::size_t>(p.integer("maxMarkers")) || rejected.size() > 4096)
      return Status::Error(Phase::Execute, "resource_limit", "解码结果或候选数量超过识别码预算");
    for (std::size_t i = 0; i < found.size(); ++i) {
      if (found[i].size() != 4 || ids[i] < 0)
        return Status::Error(Phase::Execute, "invalid_output",
                             "识别码后端返回了非法ID或非四角轮廓");
      for (const auto& q : found[i])
        if (!std::isfinite(q.x) || !std::isfinite(q.y) || q.x < 0 || q.y < 0 ||
            q.x >= input->width || q.y >= input->height)
          return Status::Error(Phase::Execute, "invalid_output",
                               "识别码后端返回了非有限或图像外角点");
    }
    std::vector<std::size_t> order(ids.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
      if (ids[a] != ids[b])
        return ids[a] < ids[b];
      for (int i = 0; i < 4; ++i) {
        if (found[a][i].x != found[b][i].x)
          return found[a][i].x < found[b][i].x;
        if (found[a][i].y != found[b][i].y)
          return found[a][i].y < found[b][i].y;
      }
      return a < b;
    });
    const std::vector<cv::Point3d> object{{-side / 2, side / 2, 0},
                                          {side / 2, side / 2, 0},
                                          {side / 2, -side / 2, 0},
                                          {-side / 2, -side / 2, 0}};
    const cv::Mat k = image_p1::cameraMatrix(camera), d = image_p1::distortion(camera);
    cv::Mat display;
    if (input->channels == 4)
      cv::cvtColor(cvx::view(*input), display, cv::COLOR_RGBA2RGB);
    else
      display = cvx::view(*input).clone();
    Json instances = Json::array(), detections = Json::array();
    for (auto index : order) {
      if (ctx.cancelled())
        return Status::Ok();
      std::vector<cv::Point2d> pixels;
      for (auto q : found[index])
        pixels.emplace_back(q.x, q.y);
      const std::string id =
          dictionary + ":" + std::to_string(ids[index]) + ":" + std::to_string(detections.size());
      Json corners = Json::array();
      for (auto q : pixels)
        corners.push_back({q.x, q.y});
      std::string reason;
      bool ambiguous = false;
      std::vector<PoseFit> fits;
      if (pixels.size() != 4 || !cv::isContourConvex(found[index]) ||
          std::abs(cv::contourArea(found[index])) < p.number("minArea"))
        reason = "degenerate_geometry";
      else {
        std::vector<cv::Mat> rotations, translations;
        cv::solvePnPGeneric(object, pixels, k, d, rotations, translations, false,
                            cv::SOLVEPNP_IPPE_SQUARE);
        for (std::size_t i = 0; i < rotations.size(); ++i) {
          PoseFit fit;
          if (candidate(object, pixels, k, d, rotations[i], translations[i], fit))
            fits.push_back(std::move(fit));
        }
        std::sort(fits.begin(), fits.end(),
                  [](const PoseFit& a, const PoseFit& b) { return a.rms < b.rms; });
        if (fits.empty())
          reason = "no_positive_depth_pose";
        else {
          for (std::size_t i = 1; i < fits.size(); ++i)
            if (fits[i].rms - fits[0].rms <= gap &&
                cv::norm(fits[i].rotation - fits[0].rotation) > 1e-3)
              ambiguous = true;
          if (fits[0].rms > maxRms)
            reason = "reprojection_rejected";
          else if (ambiguous && p.flag("rejectAmbiguous"))
            reason = "ambiguous_pose";
        }
      }
      if (reason.empty()) {
        const auto& best = fits.front();
        poses::Matrix3 matrix;
        for (int row = 0; row < 3; ++row)
          for (int col = 0; col < 3; ++col)
            matrix[3 * row + col] = best.rotation.at<double>(row, col);
        auto pose = poses::make3D(id, camera.frame,
                                  {best.translation.at<double>(0), best.translation.at<double>(1),
                                   best.translation.at<double>(2)},
                                  poses::quaternion(matrix), {side, side, thickness});
        pose.data["classId"] = std::to_string(ids[index]);
        pose.data["classLabel"] = dictionary;
        pose.data["pickable"] = false;
        pose.data["attributes"] = {{"dictionary", dictionary},
                                   {"markerId", ids[index]},
                                   {"reprojectionRms", best.rms},
                                   {"ambiguous", ambiguous}};
        instances.push_back(std::move(pose.data));
      }
      detections.push_back({{"id", id},
                            {"decodedId", ids[index]},
                            {"corners", std::move(corners)},
                            {"accepted", reason.empty()},
                            {"reason", reason},
                            {"rms", fits.empty() ? Json(nullptr) : Json(fits.front().rms)},
                            {"ambiguous", ambiguous}});
      if (pixels.size() == 4)
        paint(display, pixels, reason.empty());
    }
    Record poses = poses::make3DList(std::move(instances), camera.frame);
    Record metadata{"image.Markers.v1",
                    {{"dictionary", dictionary},
                     {"frame", camera.frame},
                     {"unit", "px"},
                     {"imageSize", {input->width, input->height}},
                     {"rejectedCandidates", stag ? Json(nullptr) : Json(rejected.size())},
                     {"items", std::move(detections)}}};
    std::string why;
    if (!poses::validate(poses, why) || !values::boundedJson(metadata.data, why, true))
      return Status::Error(Phase::Execute, "invalid_output", "识别码结果无效：" + why);
    Image overlay;
    if (input->channels == 4) {
      cv::Mat rgba;
      cv::cvtColor(display, rgba, cv::COLOR_RGB2RGBA);
      std::vector<cv::Mat> channels;
      cv::split(rgba, channels);
      cv::extractChannel(cvx::view(*input), channels[3], 3);
      cv::merge(channels, rgba);
      if (!cvx::fromMat(std::move(rgba), overlay))
        return img::badInput("无法构造RGBA叠图");
    } else if (!cvx::fromMat(std::move(display), overlay))
      return img::badInput("无法构造识别码叠图");
    if (ctx.cancelled())
      return Status::Ok();
    out.set("instances", Data::record(std::move(poses)));
    out.set("detections", Data::record(std::move(metadata)));
    out.set("image", Data::image(std::move(overlay)));
    return Status::Ok();
  } catch (const cv::Exception& e) {
    return Status::Error(Phase::Execute, "marker_solver_failed", e.what(), {}, "image");
  }
}
} // namespace
void registerP3Markers(Registry& r) {
  auto op = image_p1::descriptor("image.marker_pose", "识别码相机位姿",
                                 "aruco apriltag marker decode id camera pose PnP fiducial");
  op.capabilities = {true, false, true};
  op.inputs = {img::imageIn(), image_p1::recordPort("calib", calibration::kCameraType)};
  op.outputs = {image_p1::recordPort("instances", poses::kPoses3DType),
                image_p1::recordPort("detections", "image.Markers.v1"), img::imageOut()};
  Param dict = image_p1::text("dictionary", "识别码字典", "DICT_4X4_250");
  dict.type = ParamType::Enum;
  for (const auto& entry : dictionaries)
    dict.options.push_back({entry.name, entry.name, ""});
  dict.options.push_back({"STAG_HD15", "STag HD15", "HD15，纠错7"});
  op.params = {std::move(dict),
               scalar("markerSize", "识别码边长", .05, 1e-6, 10, "m"),
               scalar("thickness", "表示厚度", .001, 1e-6, 10, "m"),
               scalar("maxRms", "最大重投影RMS", 2, 1e-6, 1000, "px"),
               scalar("ambiguityGap", "双解RMS差", .05, 0, 1000, "px"),
               img::boolParam("rejectAmbiguous", "拒绝平面双解", true, ""),
               scalar("minPerimeterRate", "最小轮廓周长比例", .03, .001, 1),
               scalar("minArea", "最小识别码面积", 25, 1, 1e8, "px2"),
               img::intParam("maxMarkers", "最多解码目标", 256, 1, 256, "")};
  op.doc =
      "全分辨率u8识别码真实字典解码与K/D校准PnP。米制边长；局部X向右、Y向上、Z向外，四角TL/TR/BR/BL。无解码为空，质量拒绝逐目标诊断。";
  op.compute = &estimate;
  r.addOperator(std::move(op));
}
} // namespace lyflow::ops
