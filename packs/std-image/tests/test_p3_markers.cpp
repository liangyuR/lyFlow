#include <doctest/doctest.h>
#include "lyflow_cv/compat.h"
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#if CV_VERSION_MAJOR > 4 || (CV_VERSION_MAJOR == 4 && CV_VERSION_MINOR >= 7)
#include <opencv2/objdetect/aruco_dictionary.hpp>
#else
#include <opencv2/aruco.hpp>
#endif

#include "helpers.h"
#include "lyflow/calibration.h"
#include "lyflow/pose.h"
#include "lyflow_cv/adapter.h"
#include "../ops/p3_stag.h"

namespace lyflow::ops {
void registerP3Markers(Registry&);
}
namespace {
using namespace lyflow;
Registry markerRegistry() {
  Registry r;
  ops::registerP3Markers(r);
  return r;
}
Record camera(bool distortion = true) {
  return {calibration::kCameraType,
          {{"imageSize", {1280, 960}},
           {"cameraMatrix", {900., 0., 640., 0., 880., 480., 0., 0., 1.}},
           {"distortion", distortion ? nlohmann::json::array({.3, -.12, .005, -.008, .02})
                                     : nlohmann::json::array()},
           {"frame", "camera"},
           {"unit", "m"}}};
}
Data imageData(cv::Mat pixels, int scale = 1) {
  Image image;
  REQUIRE(cvx::fromMat(std::move(pixels), image));
  image.scale = scale;
  return Data::image(std::move(image));
}
cv::Mat marker(int dict, int id) {
  cv::Mat image;
#if CV_VERSION_MAJOR > 4 || (CV_VERSION_MAJOR == 4 && CV_VERSION_MINOR >= 7)
  cv::aruco::getPredefinedDictionary(dict).generateImageMarker(id, 400, image);
#else
  cv::aruco::getPredefinedDictionary(dict)->drawMarker(id, 400, image);
#endif
  return image;
}
struct Scene {
  cv::Mat pixels, rotation;
  cv::Vec3d translation{.12, .06, .65};
};
Scene sceneImage(const cv::Mat& glyph, const std::vector<cv::Point2f>& source) {
  Scene result;
  cv::Vec3d vector(CV_PI - .2, .05, .12);
  cv::Rodrigues(vector, result.rotation);
  const std::vector<cv::Point3d> object{
      {-.05, .05, 0}, {.05, .05, 0}, {.05, -.05, 0}, {-.05, -.05, 0}};
  calibration::Camera parsed;
  std::string why;
  REQUIRE(calibration::parseCamera(camera(), parsed, why));
  cv::Mat k(3, 3, CV_64F, parsed.matrix.data()), d(parsed.distortion);
  std::vector<cv::Point2d> projected;
  cv::projectPoints(object, vector, result.translation, k, d, projected);
  std::vector<cv::Point2f> corners;
  for (auto p : projected)
    corners.emplace_back(p.x, p.y);
  cv::warpPerspective(glyph, result.pixels, cv::getPerspectiveTransform(source, corners),
                      cv::Size(1280, 960), cv::INTER_NEAREST, cv::BORDER_CONSTANT, cv::Scalar(255));
  return result;
}
Scene scene(int dictionary, int id) {
  return sceneImage(marker(dictionary, id),
                    {{-.5f, -.5f}, {399.5f, -.5f}, {399.5f, 399.5f}, {-.5f, 399.5f}});
}
class CancelMarker : public ExecContext {
public:
  bool cancelled() const override { return true; }
  void progress(float, std::string_view) override {}
  void log(LogLevel, std::string) override {}
  const std::filesystem::path& baseDir() const override { return base; }
  int threadBudget() const override { return 1; }
  std::filesystem::path base;
};
}

TEST_CASE("P3 real ArUco and AprilTag decoding yields calibrated metric camera poses") {
  auto registry = markerRegistry();
  test::OpCall call(registry);
  for (const auto item : {std::pair<const char*, int>{"DICT_4X4_250", cv::aruco::DICT_4X4_250},
                          {"DICT_5X5_250", cv::aruco::DICT_5X5_250},
                          {"DICT_6X6_250", cv::aruco::DICT_6X6_250},
                          {"DICT_APRILTAG_36h11", cv::aruco::DICT_APRILTAG_36h11}}) {
    const Scene truth = scene(item.second, 42);
    call.inputs = {{"image", imageData(truth.pixels.clone())}, {"calib", Data::record(camera())}};
    REQUIRE(call.run("image.marker_pose", {{"dictionary", Value::text(item.first)},
                                           {"markerSize", Value::number(.1)},
                                           {"rejectAmbiguous", Value::boolean(false)}})
                .ok);
    const auto& objects = call.out("instances").asRecord()->data["items"];
    REQUIRE(objects.size() == 1);
    const auto& obj = objects[0];
    CHECK(obj["frame"] == "camera");
    CHECK(obj["unit"] == "m");
    CHECK(obj["classId"] == "42");
    CHECK(obj["pickable"] == false);
    for (int i = 0; i < 3; ++i)
      CHECK(std::abs(obj["position"][i].get<double>() - truth.translation[i]) < .004);
    const auto rotation = poses::rotationMatrix(obj["rotation"].get<poses::Quaternion>());
    double error = 0;
    for (int row = 0; row < 3; ++row)
      for (int col = 0; col < 3; ++col)
        error += std::pow(rotation[row * 3 + col] - truth.rotation.at<double>(row, col), 2);
    CHECK(std::sqrt(error) < .1);
    const auto& decoded = call.out("detections").asRecord()->data["items"];
    REQUIRE(decoded.size() == 1);
    CHECK(decoded[0]["decodedId"] == 42);
    CHECK(decoded[0]["accepted"] == true);
    CHECK(decoded[0]["rms"].get<double>() < 1);
    CHECK(decoded[0]["id"] == obj["id"]);
    CHECK(decoded[0]["corners"].size() == 4);
    CHECK(cv::norm(cvx::view(*call.inputs.at("image").asImage()), truth.pixels, cv::NORM_INF) == 0);
    const auto before = call.out("detections").asRecord()->data;
    REQUIRE(call.run("image.marker_pose", {{"dictionary", Value::text(item.first)},
                                           {"markerSize", Value::number(.1)},
                                           {"rejectAmbiguous", Value::boolean(false)}})
                .ok);
    CHECK(call.out("detections").asRecord()->data == before);
  }
}

TEST_CASE("P3 official HD15 pixels decode STag identity orientation and calibrated pose") {
  auto registry = markerRegistry();
  test::OpCall call(registry);
  const auto path = std::filesystem::path(LYFLOW_EXAMPLES_DIR) / "p3-stag-hd15.png";
  const cv::Mat original = cv::imread(path.string(), cv::IMREAD_GRAYSCALE);
  REQUIRE_FALSE(original.empty());
  REQUIRE(original.size() == cv::Size(1000, 1000));
  for (int turns = 0; turns < 4; ++turns) {
    cv::Mat glyph;
    if (turns == 0)
      glyph = original;
    else
      cv::rotate(original, glyph,
                 turns == 1   ? cv::ROTATE_90_CLOCKWISE
                 : turns == 2 ? cv::ROTATE_180
                              : cv::ROTATE_90_COUNTERCLOCKWISE);
    const auto truth =
        sceneImage(glyph, {{99.5f, 99.5f}, {899.5f, 99.5f}, {899.5f, 899.5f}, {99.5f, 899.5f}});
    call.inputs = {{"image", imageData(truth.pixels.clone())}, {"calib", Data::record(camera())}};
    REQUIRE(call.run("image.marker_pose", {{"dictionary", Value::text("STAG_HD15")},
                                           {"markerSize", Value::number(.1)},
                                           {"rejectAmbiguous", Value::boolean(false)}})
                .ok);
    const auto& instances = call.out("instances").asRecord()->data["items"];
    REQUIRE(instances.size() == 1);
    const auto& pose = instances[0];
    CHECK(pose["classId"] == "0");
    CHECK(pose["classLabel"] == "STAG_HD15");
    for (int axis = 0; axis < 3; ++axis)
      CHECK(std::abs(pose["position"][axis].get<double>() - truth.translation[axis]) < .004);
    const auto rotation = poses::rotationMatrix(pose["rotation"].get<poses::Quaternion>());
    cv::Mat localRotation;
    cv::Rodrigues(cv::Vec3d(0, 0, -turns * CV_PI / 2), localRotation);
    const cv::Mat expected = truth.rotation * localRotation;
    double squared = 0;
    for (int row = 0; row < 3; ++row)
      for (int col = 0; col < 3; ++col)
        squared += std::pow(rotation[row * 3 + col] - expected.at<double>(row, col), 2);
    CHECK(std::sqrt(squared) < .1);
    const auto& detection = call.out("detections").asRecord()->data["items"][0];
    CHECK(detection["decodedId"] == 0);
    CHECK(detection["accepted"] == true);
    CHECK(detection["rms"].get<double>() < 1);
    CHECK(call.out("detections").asRecord()->data["rejectedCandidates"].is_null());
    CHECK(cv::norm(cvx::view(*call.inputs.at("image").asImage()), truth.pixels, cv::NORM_INF) == 0);
  }
}

TEST_CASE("P3 independent STag decoder enforces real code correction structure and budgets") {
  const auto path = std::filesystem::path(LYFLOW_EXAMPLES_DIR) / "p3-stag-hd15.png";
  const cv::Mat original = cv::imread(path.string(), cv::IMREAD_GRAYSCALE);
  REQUIRE_FALSE(original.empty());
  std::filesystem::path base;
  test::StubContext context(base, 1);
  const std::array<cv::Point, 8> bitCenters{{{536, 464},
                                             {607, 447},
                                             {553, 393},
                                             {677, 464},
                                             {657, 395},
                                             {605, 343},
                                             {536, 323},
                                             {749, 463}}};
  for (int flipped : {0, 7, 8}) {
    cv::Mat image = original.clone();
    for (int bit = 0; bit < flipped; ++bit)
      cv::circle(image, bitCenters[bit], 8,
                 cv::Scalar(image.at<unsigned char>(bitCenters[bit]) < 128 ? 255 : 0), cv::FILLED);
    std::vector<std::vector<cv::Point2f>> corners;
    std::vector<int> ids;
    REQUIRE(ops::detectSTagHD15(image, corners, ids, context).ok);
    if (flipped <= 7) {
      REQUIRE(ids.size() == 1);
      CHECK(ids[0] == 0);
    } else
      CHECK(ids.empty());
  }
  for (const bool rectangle : {false, true}) {
    cv::Mat impostor(original.size(), CV_8U, cv::Scalar(255));
    if (rectangle)
      cv::rectangle(impostor, {100, 100, 800, 800}, cv::Scalar(0), cv::FILLED);
    std::vector<std::vector<cv::Point2f>> corners;
    std::vector<int> ids;
    REQUIRE(ops::detectSTagHD15(impostor, corners, ids, context).ok);
    CHECK(ids.empty());
  }
  std::vector<std::vector<cv::Point2f>> corners{{{1, 2}}};
  std::vector<int> ids{999};
  const auto saved = corners;
  CHECK_FALSE(
      ops::detectSTagHD15(cv::Mat(1025, 2048, CV_8U, cv::Scalar(255)), corners, ids, context).ok);
  CHECK(corners == saved);
  CHECK(ids == std::vector<int>{999});
  CHECK_FALSE(
      ops::detectSTagHD15(cv::Mat(20, 20, CV_8U, cv::Scalar(255)), corners, ids, context).ok);
  cv::Mat dense(512, 512, CV_8U, cv::Scalar(255));
  for (int y = 2; y < dense.rows - 3; y += 6)
    for (int x = 2; x < dense.cols - 3; x += 6)
      cv::rectangle(dense, {x, y, 3, 3}, cv::Scalar(0), cv::FILLED);
  const auto bounded = ops::detectSTagHD15(dense, corners, ids, context);
  CHECK_FALSE(bounded.ok);
  CHECK(bounded.code == "resource_limit");
  CHECK(corners == saved);
  CHECK(ids == std::vector<int>{999});
  CancelMarker cancelled;
  REQUIRE(ops::detectSTagHD15(original, corners, ids, cancelled).ok);
  CHECK(corners == saved);
  CHECK(ids == std::vector<int>{999});
}

TEST_CASE("P3 decoded markers retain rejection evidence without publishing false poses") {
  auto registry = markerRegistry();
  test::OpCall call(registry);
  const auto truth = scene(cv::aruco::DICT_4X4_250, 42);
  call.inputs = {{"image", imageData(truth.pixels.clone())}, {"calib", Data::record(camera())}};
  REQUIRE(call.run("image.marker_pose", {{"minArea", Value::number(1e8)}}).ok);
  CHECK(call.out("instances").asRecord()->data["items"].empty());
  REQUIRE(call.out("detections").asRecord()->data["items"].size() == 1);
  CHECK(call.out("detections").asRecord()->data["items"][0]["reason"] == "degenerate_geometry");
  REQUIRE(call.run("image.marker_pose", {{"ambiguityGap", Value::number(1000)},
                                         {"rejectAmbiguous", Value::boolean(true)}})
              .ok);
  CHECK(call.out("instances").asRecord()->data["items"].empty());
  CHECK(call.out("detections").asRecord()->data["items"][0]["reason"] == "ambiguous_pose");
  REQUIRE(call.run("image.marker_pose", {{"dictionary", Value::text("DICT_6X6_250")}}).ok);
  CHECK(call.out("instances").asRecord()->data["items"].empty());
  CHECK(call.out("detections").asRecord()->data["items"].empty());
  const std::vector<cv::Point2f> from{{0, 0}, {399, 0}, {399, 399}, {0, 399}},
      to{{300, 200}, {800, 240}, {700, 750}, {350, 650}};
  cv::Mat impossible;
  cv::warpPerspective(marker(cv::aruco::DICT_4X4_250, 42), impossible,
                      cv::getPerspectiveTransform(from, to), cv::Size(1280, 960), cv::INTER_NEAREST,
                      cv::BORDER_CONSTANT, cv::Scalar(255));
  call.inputs["image"] = imageData(impossible);
  REQUIRE(call.run("image.marker_pose",
                   {{"maxRms", Value::number(.01)}, {"rejectAmbiguous", Value::boolean(false)}})
              .ok);
  CHECK(call.out("instances").asRecord()->data["items"].empty());
  REQUIRE(call.out("detections").asRecord()->data["items"].size() == 1);
  CHECK(call.out("detections").asRecord()->data["items"][0]["reason"] == "reprojection_rejected");
}

TEST_CASE("P3 repeated decoded IDs remain distinct and marker limits are atomic") {
  auto registry = markerRegistry();
  test::OpCall call(registry);
  cv::Mat canvas(960, 1280, CV_8U, cv::Scalar(255));
  const auto glyph = marker(cv::aruco::DICT_4X4_250, 42);
  glyph.copyTo(canvas(cv::Rect(100, 250, 400, 400)));
  glyph.copyTo(canvas(cv::Rect(750, 250, 400, 400)));
  auto isotropic = camera(false);
  isotropic.data["cameraMatrix"][4] = 900.;
  call.inputs = {{"image", imageData(canvas)}, {"calib", Data::record(isotropic)}};
  REQUIRE(call.run("image.marker_pose", {{"rejectAmbiguous", Value::boolean(false)}}).ok);
  const auto& items = call.out("instances").asRecord()->data["items"];
  REQUIRE(items.size() == 2);
  CHECK(items[0]["id"] != items[1]["id"]);
  CHECK(items[0]["classId"] == items[1]["classId"]);
  const auto status = call.run("image.marker_pose", {{"maxMarkers", Value::integer(1)}});
  CHECK_FALSE(status.ok);
  CHECK(status.code == "resource_limit");
  CHECK(call.outputs.empty());
}

TEST_CASE("P3 marker overlay preserves RGBA alpha source channels and empty observation") {
  auto registry = markerRegistry();
  test::OpCall call(registry);
  const auto truth = scene(cv::aruco::DICT_4X4_250, 42);
  cv::Mat rgba;
  cv::cvtColor(truth.pixels, rgba, cv::COLOR_GRAY2RGBA);
  for (int y = 0; y < rgba.rows; ++y)
    for (int x = 0; x < rgba.cols; ++x)
      rgba.at<cv::Vec4b>(y, x)[3] = static_cast<unsigned char>((x + y) % 256);
  call.inputs = {{"image", imageData(rgba.clone())}, {"calib", Data::record(camera())}};
  REQUIRE(call.run("image.marker_pose", {{"rejectAmbiguous", Value::boolean(false)}}).ok);
  REQUIRE(call.out("instances").asRecord()->data["items"].size() == 1);
  const auto* output = call.out("image").asImage();
  CHECK(output->channels == 4);
  CHECK(output->depth == PixelDepth::U8);
  cv::Mat alphaBefore, alphaAfter;
  cv::extractChannel(rgba, alphaBefore, 3);
  cv::extractChannel(cvx::view(*output), alphaAfter, 3);
  CHECK(cv::norm(alphaBefore, alphaAfter, cv::NORM_INF) == 0);
  CHECK(cv::norm(rgba, cvx::view(*call.inputs.at("image").asImage()), cv::NORM_INF) == 0);
  for (int channels : {1, 3, 4}) {
    call.inputs["image"] =
        imageData(cv::Mat(960, 1280, CV_MAKETYPE(CV_8U, channels), cv::Scalar::all(255)));
    REQUIRE(call.run("image.marker_pose").ok);
    CHECK(call.out("instances").asRecord()->data["items"].empty());
    CHECK(call.out("image").asImage()->channels == channels);
  }
}

TEST_CASE("P3 marker calibration units formats and cancellation have explicit diagnostics") {
  auto registry = markerRegistry();
  test::OpCall call(registry);
  const auto truth = scene(cv::aruco::DICT_4X4_250, 42);
  call.inputs = {{"image", imageData(truth.pixels.clone(), 2)}, {"calib", Data::record(camera())}};
  CHECK_FALSE(call.run("image.marker_pose").ok);
  call.inputs["image"] = imageData(truth.pixels.clone());
  auto invalid = camera();
  invalid.data["unit"] = "mm";
  call.inputs["calib"] = Data::record(invalid);
  CHECK_FALSE(call.run("image.marker_pose").ok);
  invalid = camera();
  invalid.data["imageSize"] = {640, 480};
  call.inputs["calib"] = Data::record(invalid);
  CHECK_FALSE(call.run("image.marker_pose").ok);
  call.inputs["calib"] = Data::record(camera());
  CHECK_FALSE(call.run("image.marker_pose", {{"markerSize", Value::number(0)}}).ok);
  CHECK_FALSE(call.run("image.marker_pose", {{"dictionary", Value::text("UNKNOWN")}}).ok);
  call.inputs["image"] = imageData(cv::Mat(960, 1280, CV_16U, cv::Scalar(1000)));
  CHECK_FALSE(call.run("image.marker_pose").ok);
  call.inputs["image"] = imageData(truth.pixels.clone());
  const auto* op = registry.find("image.marker_pose");
  REQUIRE(op);
  ParamMap params;
  for (const auto& item : op->params)
    params[item.name] = item.def;
  std::unordered_map<std::string, Data> outputs;
  Inputs in(call.inputs);
  Outputs out(outputs);
  std::filesystem::path base;
  ParamView view(params, base);
  CancelMarker ctx;
  REQUIRE(op->compute(in, view, out, ctx).ok);
  CHECK(outputs.empty());
}
