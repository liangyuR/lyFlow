#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <limits>

#include <opencv2/calib3d.hpp>
#include <opencv2/imgproc.hpp>

#include "helpers.h"
#include "lyflow/calibration.h"
#include "lyflow/pose.h"
#include "lyflow_cv/adapter.h"

namespace {
using namespace lyflow;
using Json = nlohmann::json;

Data image(const cv::Mat& matrix, int scale = 1) {
  Image image;
  REQUIRE(cvx::fromMat(matrix, image));
  image.scale = scale;
  return Data::image(std::move(image));
}

Record camera(int w = 32, int h = 24) {
  return {calibration::kCameraType,
          {{"imageSize", {w, h}},
           {"cameraMatrix", {500., 0., w / 2., 0., 510., h / 2., 0., 0., 1.}},
           {"distortion", Json::array()},
           {"frame", "camera"},
           {"unit", "m"}}};
}

const Json& data(const test::OpCall& c, const char* name) {
  REQUIRE(c.outputs.at(name).asRecord() != nullptr);
  return c.outputs.at(name).asRecord()->data;
}

struct File {
  std::filesystem::path path;
  File() {
    static std::atomic<int> id{0};
    path = std::filesystem::temp_directory_path() /
           std::filesystem::u8path(
               "lyflow-相机-" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
               std::to_string(++id) + ".json");
  }
  void write(const std::string& text) { std::ofstream(path, std::ios::binary) << text; }
  ~File() {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
};

class CancelContext final : public ExecContext {
public:
  mutable int checks = 0;
  bool cancelled() const override { return ++checks > 1; }
  void progress(float, std::string_view) override {}
  void log(LogLevel, std::string) override {}
  const std::filesystem::path& baseDir() const override { return base; }
  int threadBudget() const override { return 1; }
  std::filesystem::path base;
};

void cancelled(const char* id, const std::unordered_map<std::string, Data>& inputs,
               const ParamMap& overrides = {}) {
  const auto* op = ensureRegistry().find(id);
  REQUIRE(op != nullptr);
  ParamMap params;
  for (const auto& param : op->params)
    params[param.name] = param.def;
  for (const auto& override : overrides)
    params[override.first] = override.second;
  CancelContext ctx;
  std::unordered_map<std::string, Data> outputs;
  Inputs inputView(inputs);
  Outputs outputView(outputs);
  ParamView view(params, ctx.base);
  CHECK(op->compute(inputView, view, outputView, ctx).ok);
  CHECK(ctx.checks > 1);
  CHECK(outputs.empty());
}

cv::Mat circleImage(int width, double inner, double outer = 0) {
  cv::Mat result(width, width, CV_32F);
  for (int y = 0; y < width; ++y)
    for (int x = 0; x < width; ++x) {
      const double radius = std::hypot(x - width / 2.0, y - width / 2.0);
      const double a = 1 / (1 + std::exp((radius - inner) / 0.5));
      const double b = outer > 0 ? 1 / (1 + std::exp((radius - outer) / 0.5)) : 0;
      result.at<float>(y, x) = static_cast<float>(outer > 0 ? b - a : a);
    }
  return result;
}

ParamMap scanParams() {
  return {{"center", Value::vec({64, 64})}, {"radiusMin", Value::number(10)},
          {"radiusMax", Value::number(45)}, {"minGradient", Value::number(0.05)},
          {"samples", Value::integer(72)},  {"radialStep", Value::number(0.5)}};
}
}

TEST_CASE("P1 image concatenation preserves depth channels padding and source pixels") {
  test::OpCall c;
  cv::Mat a(2, 3, CV_16UC1, cv::Scalar(1000)), b(3, 2, CV_16UC1, cv::Scalar(4000));
  c.inputs = {{"a", image(a)}, {"b", image(b)}};
  REQUIRE(c.run("image.concat", {{"padding", Value::number(7)}}).ok);
  const auto* out = c.outputs.at("image").asImage();
  REQUIRE(out != nullptr);
  CHECK(out->width == 5);
  CHECK(out->height == 3);
  CHECK(out->depth == PixelDepth::U16);
  CHECK(out->at(0, 2, 0) == 7);
  CHECK(out->at(3, 2, 0) == 4000);
  CHECK(a.at<std::uint16_t>(0, 0) == 1000);
  CHECK_FALSE(c.run("image.concat", {{"padding", Value::number(1e100)}}).ok);
  REQUIRE(c.run("image.concat", {{"direction", Value::text("vertical")}}).ok);
  CHECK(c.outputs.at("image").asImage()->height == 5);
  c.inputs["b"] = image(cv::Mat(3, 2, CV_8UC1, cv::Scalar(1)));
  CHECK_FALSE(c.run("image.concat").ok);
}

TEST_CASE("P1 color adjustment uses RGB hue and preserves alpha and integer depth") {
  test::OpCall c;
  cv::Mat rgba(2, 2, CV_8UC4, cv::Scalar(255, 0, 0, 37));
  c.inputs = {{"image", image(rgba)}};
  REQUIRE(c.run("image.color_adjust", {{"hue", Value::number(120)}}).ok);
  const auto* out = c.outputs.at("image").asImage();
  CHECK(out->at(0, 0, 0) == 0);
  CHECK(out->at(0, 0, 1) == 255);
  CHECK(out->at(0, 0, 2) == 0);
  CHECK(out->at(0, 0, 3) == 37);
  CHECK(rgba.at<cv::Vec4b>(0, 0)[0] == 255);
  c.inputs = {{"image", image(cv::Mat(1, 1, CV_16UC1, cv::Scalar(12345)))}};
  REQUIRE(c.run("image.color_adjust").ok);
  CHECK(c.outputs.at("image").asImage()->at(0, 0, 0) == 12345);
  CHECK_FALSE(c.run("image.color_adjust", {{"gamma", Value::number(0)}}).ok);
  c.inputs = {
      {"image", image(cv::Mat(1, 1, CV_32F, cv::Scalar(std::numeric_limits<float>::quiet_NaN())))}};
  CHECK_FALSE(c.run("image.color_adjust").ok);
}

TEST_CASE("P1 flat field corrects spatial gain and rejects invalid division") {
  test::OpCall c;
  cv::Mat source = (cv::Mat_<std::uint8_t>(1, 2) << 50, 100);
  cv::Mat reference = (cv::Mat_<std::uint8_t>(1, 2) << 100, 200);
  c.inputs = {{"image", image(source)}, {"reference", image(reference)}};
  REQUIRE(c.run("image.shading_correct").ok);
  CHECK(c.outputs.at("image").asImage()->at(0, 0, 0) == 75);
  CHECK(c.outputs.at("image").asImage()->at(1, 0, 0) == 75);
  CHECK(source.at<std::uint8_t>(0, 0) == 50);
  reference.at<std::uint8_t>(0, 0) = 0;
  c.inputs["reference"] = image(reference);
  CHECK_FALSE(c.run("image.shading_correct").ok);
  REQUIRE(c.run("image.shading_correct", {{"mode", Value::text("subtract")}}).ok);
}

TEST_CASE(
    "P1 instance rasterization and morphology preserve identities and explicit overlap policy") {
  test::OpCall c;
  auto a = poses::make2D("a", "image", "px", {5, 5}, 0, {6, 6});
  auto b = poses::make2D("b", "image", "px", {7, 5}, 0.2, {6, 6});
  Record list = poses::make2DList(Json::array({a.data, b.data}));
  c.inputs = {{"image", image(cv::Mat(16, 16, CV_8U, cv::Scalar(0)))},
              {"instances", Data::record(list)}};
  REQUIRE(c.run("image.instances_mask").ok);
  CHECK(c.outputs.at("labels").asImage()->depth == PixelDepth::U16);
  CHECK(c.outputs.at("labels").asImage()->at(6, 5, 0) == 1);
  CHECK(data(c, "index")["ids"] == Json::array({"a", "b"}));
  auto labels = c.outputs.at("labels"), index = c.outputs.at("index");
  CHECK_FALSE(c.run("image.instances_mask", {{"overlap", Value::text("error")}}).ok);
  REQUIRE(c.run("image.instances_mask", {{"overlap", Value::text("last")}}).ok);
  CHECK(c.outputs.at("labels").asImage()->at(6, 5, 0) == 2);
  c.inputs = {{"labels", labels}, {"index", index}};
  REQUIRE(c.run("image.instances_morphology").ok);
  CHECK(c.outputs.at("labels").asImage()->at(6, 5, 0) == 1);
  CHECK(data(c, "index")["ids"] == Json::array({"a", "b"}));
  CHECK_FALSE(c.run("image.instances_morphology", {{"kernel", Value::integer(2)}}).ok);
  list = poses::make2DList(Json::array());
  c.inputs = {{"image", image(cv::Mat(16, 16, CV_8U, cv::Scalar(0)))},
              {"instances", Data::record(list)}};
  REQUIRE(c.run("image.instances_mask").ok);
  CHECK(cv::countNonZero(cvx::view(*c.outputs.at("mask").asImage())) == 0);
  for (const std::size_t length : {std::size_t(257), values::kMaxStringBytes}) {
    const std::string id(length, 'i'), frame(length, 'f');
    const auto pose = poses::make2D(id, frame, "px", {5, 5}, 0, {4, 4});
    c.inputs = {{"image", image(cv::Mat(12, 12, CV_8U, cv::Scalar(0)))},
                {"instances", Data::record(poses::make2DList(Json::array({pose.data}), frame))}};
    REQUIRE(c.run("image.instances_mask", {{"frame", Value::text(frame)}}).ok);
    c.inputs = {{"labels", c.outputs.at("labels")}, {"index", c.outputs.at("index")}};
    REQUIRE(c.run("image.instances_morphology").ok);
    CHECK(data(c, "index")["ids"] == Json::array({id}));
    CHECK(data(c, "index")["frame"] == frame);
    auto oversized = *c.outputs.at("index").asRecord();
    oversized.data["ids"][0] = std::string(values::kMaxStringBytes + 1, 'i');
    c.inputs["index"] = Data::record(std::move(oversized));
    CHECK_FALSE(c.run("image.instances_morphology").ok);
  }
}

TEST_CASE(
    "P1 camera records load through unicode paths and reject unknown calibration conventions") {
  test::OpCall c;
  File file;
  const auto record = camera();
  file.write(Json{{"type", record.type}, {"data", record.data}}.dump());
  REQUIRE(c.run("image.load_camera_calib", {{"path", Value::text(file.path.u8string())}}).ok);
  CHECK(data(c, "calib") == record.data);
  calibration::Camera parsed;
  std::string why;
  auto invalid = record;
  invalid.data["cameraMatrix"][0] = -500;
  CHECK_FALSE(calibration::parseCamera(invalid, parsed, why));
  invalid = record;
  invalid.data["distortion"] = {1, 2, 3};
  CHECK_FALSE(calibration::parseCamera(invalid, parsed, why));
  invalid = record;
  invalid.data["unit"] = "mm";
  CHECK_FALSE(calibration::parseCamera(invalid, parsed, why));
  file.write("{\"fx\":500}");
  CHECK_FALSE(c.run("image.load_camera_calib", {{"path", Value::text(file.path.u8string())}}).ok);
  file.write(std::string(32, '[') + "0" + std::string(32, ']'));
  CHECK_FALSE(c.run("image.load_camera_calib", {{"path", Value::text(file.path.u8string())}}).ok);
}

TEST_CASE("P1 camera undistortion preserves zero-distortion pixels and enforces image size") {
  test::OpCall c;
  cv::Mat pattern(24, 32, CV_8UC3);
  cv::RNG(9).fill(pattern, cv::RNG::UNIFORM, 0, 256);
  c.inputs = {{"image", image(pattern)}, {"calib", Data::record(camera())}};
  REQUIRE(c.run("image.undistort").ok);
  CHECK(cv::norm(cvx::view(*c.outputs.at("image").asImage()), pattern, cv::NORM_INF) == 0);
  CHECK(data(c, "calib")["distortion"].empty());
  c.inputs["calib"] = Data::record(camera(64, 48));
  CHECK_FALSE(c.run("image.undistort").ok);
}

TEST_CASE("P1 homography warp and calibrated distance respect physical units and coverage") {
  test::OpCall c;
  Record plane{calibration::kPlaneType,
               {{"H", {2., 0., 0., 0., 2., 0., 0., 0., 1.}}, {"unit", "mm"}}};
  cv::Mat pattern(12, 16, CV_8U);
  cv::RNG(19).fill(pattern, cv::RNG::UNIFORM, 0, 256);
  c.inputs = {{"image", image(pattern)}, {"calib", Data::record(plane)}};
  REQUIRE(c.run("image.warp_perspective", {{"width", Value::integer(16)},
                                           {"height", Value::integer(12)},
                                           {"pixelsPerUnit", Value::number(0.5)},
                                           {"interpolation", Value::text("nearest")}})
              .ok);
  CHECK(cv::norm(pattern, cvx::view(*c.outputs.at("image").asImage()), cv::NORM_INF) == 0);
  CHECK(cv::countNonZero(cvx::view(*c.outputs.at("valid").asImage())) == 16 * 12);
  auto wrongSize = plane;
  wrongSize.data["imageSize"] = {32, 24};
  c.inputs["calib"] = Data::record(wrongSize);
  CHECK_FALSE(c.run("image.warp_perspective",
                    {{"width", Value::integer(16)}, {"height", Value::integer(12)}})
                  .ok);
  Point2D a, b;
  a.p[0] = 1;
  a.p[1] = 2;
  a.unit = Unit2D::Pixel;
  b.p[0] = 4;
  b.p[1] = 6;
  b.unit = Unit2D::Pixel;
  c.inputs = {{"a", Data::point2d(a)}, {"b", Data::point2d(b)}, {"calib", Data::record(plane)}};
  REQUIRE(c.run("image.calibrated_distance").ok);
  CHECK(c.outputs.at("distance").asMeasurement()->value == doctest::Approx(0.01));
  CHECK(c.outputs.at("distance").asMeasurement()->unit == "m");
  a.unit = Unit2D::Meter;
  c.inputs["a"] = Data::point2d(a);
  CHECK_FALSE(c.run("image.calibrated_distance").ok);
}

TEST_CASE(
    "P1 board PnP recovers nonzero rotation translation and rejects degenerate correspondences") {
  test::OpCall c;
  auto calibration = camera(400, 300);
  cv::Mat k(3, 3, CV_64F);
  const auto values = calibration.data["cameraMatrix"].get<std::vector<double>>();
  std::copy(values.begin(), values.end(), k.ptr<double>());
  std::vector<cv::Point3d> object;
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 5; ++x)
      object.emplace_back(x * 0.02, y * 0.02, 0);
  cv::Mat rvec = (cv::Mat_<double>(3, 1) << 0.2, -0.1, 0.03);
  cv::Mat tvec = (cv::Mat_<double>(3, 1) << -0.02, -0.01, 0.6);
  std::vector<cv::Point2d> pixels;
  cv::projectPoints(object, rvec, tvec, k, cv::Mat(), pixels);
  Json points = Json::array();
  for (const auto& point : pixels)
    points.push_back({point.x, point.y});
  Record corners{"vision.BoardCorners.v1", {{"imageSize", {400, 300}}, {"points", points}}};
  c.inputs = {{"image", image(cv::Mat(300, 400, CV_8U, cv::Scalar(0)))},
              {"calib", Data::record(calibration)},
              {"corners", Data::record(corners)}};
  const ParamMap params{{"columns", Value::integer(5)},
                        {"rows", Value::integer(4)},
                        {"squareSize", Value::number(0.02)}};
  REQUIRE(c.run("image.board_pose", params).ok);
  const auto* pose = c.outputs.at("pose").asTransform();
  REQUIRE(pose != nullptr);
  CHECK(pose->m[3] == doctest::Approx(-0.02));
  CHECK(pose->m[7] == doctest::Approx(-0.01));
  CHECK(pose->m[11] == doctest::Approx(0.6));
  cv::Mat expectedRotation;
  cv::Rodrigues(rvec, expectedRotation);
  for (int y = 0; y < 3; ++y)
    for (int x = 0; x < 3; ++x)
      CHECK(pose->m[y * 4 + x] == doctest::Approx(expectedRotation.at<double>(y, x)).epsilon(1e-5));
  double squared = 0;
  for (std::size_t i = 0; i < object.size(); ++i) {
    std::array<double, 3> position{};
    for (int row = 0; row < 3; ++row)
      position[row] = double(pose->m[row * 4]) * object[i].x +
                      double(pose->m[row * 4 + 1]) * object[i].y +
                      double(pose->m[row * 4 + 2]) * object[i].z + pose->m[row * 4 + 3];
    const double dx = 500 * position[0] / position[2] + 200 - pixels[i].x;
    const double dy = 510 * position[1] / position[2] + 150 - pixels[i].y;
    squared += dx * dx + dy * dy;
  }
  const double nativeRms = std::sqrt(squared / object.size());
  CHECK(nativeRms > 1e-8);
  CHECK(nativeRms < 1e-5);
  CHECK(c.outputs.at("rms").asMeasurement()->value == doctest::Approx(nativeRms).epsilon(1e-8));
  CHECK(data(c, "quality")["rms"].get<double>() == doctest::Approx(nativeRms).epsilon(1e-8));
  std::string why;
  CHECK(poses::validate(*c.outputs.at("instance").asRecord(), why));
  auto strictParams = params;
  strictParams["maxRms"] = Value::number(1e-9);
  CHECK(c.run("image.board_pose", strictParams).code == "quality_rejected");
  CHECK(c.outputs.empty());
  auto ambiguousParams = params;
  ambiguousParams["ambiguityGap"] = Value::number(100);
  CHECK(c.run("image.board_pose", ambiguousParams).code == "ambiguous_pose");
  corners.data["points"] = Json::array();
  for (int i = 0; i < 20; ++i)
    corners.data["points"].push_back({50, 50});
  c.inputs["corners"] = Data::record(corners);
  CHECK_FALSE(c.run("image.board_pose", params).ok);
  c.inputs.erase("corners");
  CHECK(c.run("image.board_pose", params).code == "no_target");
}

TEST_CASE(
    "P1 NCC detects multiple instances with stable location and valid empty no-match output") {
  test::OpCall c;
  cv::Mat pattern(9, 11, CV_8U);
  cv::RNG(101).fill(pattern, cv::RNG::UNIFORM, 20, 240);
  c.inputs = {{"image", image(pattern)}};
  REQUIRE(c.run("image.template_build", {{"id", Value::text("part")}}).ok);
  const Data model = c.outputs.at("model"), templ = c.outputs.at("template");
  cv::Mat target(64, 80, CV_8U, cv::Scalar(20));
  pattern.copyTo(target(cv::Rect(7, 9, pattern.cols, pattern.rows)));
  pattern.copyTo(target(cv::Rect(45, 30, pattern.cols, pattern.rows)));
  c.inputs = {{"image", image(target)}, {"template", templ}, {"model", model}};
  REQUIRE(c.run("image.template_match", {{"minScore", Value::number(0.99)}}).ok);
  REQUIRE(data(c, "instances")["items"].size() == 2);
  std::vector<std::array<double, 2>> positions;
  for (const auto& instance : data(c, "instances")["items"])
    positions.push_back(instance["position"].get<std::array<double, 2>>());
  std::sort(positions.begin(), positions.end());
  CHECK(positions[0][0] == doctest::Approx(12));
  CHECK(positions[0][1] == doctest::Approx(13));
  CHECK(positions[1][0] == doctest::Approx(50));
  CHECK(positions[1][1] == doctest::Approx(34));
  c.inputs["image"] = image(cv::Mat(64, 80, CV_8U, cv::Scalar(20)));
  REQUIRE(c.run("image.template_match").ok);
  CHECK(data(c, "instances")["items"].empty());
  c.inputs = {{"image", image(cv::Mat(8, 8, CV_8U, cv::Scalar(20)))}};
  CHECK(c.run("image.template_build").code == "degenerate_template");
}

TEST_CASE("P1 NCC angle and scale search return corresponding pose conventions") {
  test::OpCall c;
  cv::Mat pattern(9, 11, CV_8U);
  cv::RNG(207).fill(pattern, cv::RNG::UNIFORM, 20, 240);
  c.inputs = {{"image", image(pattern)}};
  REQUIRE(c.run("image.template_build").ok);
  const Data model = c.outputs.at("model"), templ = c.outputs.at("template");
  cv::Mat rotated;
  cv::rotate(pattern, rotated, cv::ROTATE_90_CLOCKWISE);
  cv::Mat target(64, 64, CV_8U, cv::Scalar(0));
  rotated.copyTo(target(cv::Rect(20, 15, rotated.cols, rotated.rows)));
  c.inputs = {{"image", image(target)}, {"template", templ}, {"model", model}};
  REQUIRE(c.run("image.template_match", {{"angleMin", Value::number(90)},
                                         {"angleMax", Value::number(90)},
                                         {"minScore", Value::number(0.99)}})
              .ok);
  REQUIRE(data(c, "instances")["items"].size() == 1);
  CHECK(data(c, "instances")["items"][0]["angle"].get<double>() == doctest::Approx(poses::kPi / 2));
  CHECK(data(c, "instances")["items"][0]["position"][0].get<double>() == doctest::Approx(24));
  CHECK_FALSE(c.run("image.template_match", {{"angleMin", Value::number(-180)},
                                             {"angleMax", Value::number(180)},
                                             {"angleStep", Value::number(1)}})
                  .ok);
}

TEST_CASE("P1 radial circle and arc calipers fit known edge geometry and reject low contrast") {
  test::OpCall c;
  c.inputs = {{"image", image(circleImage(128, 30))}};
  auto params = scanParams();
  params["polarity"] = Value::text("falling");
  REQUIRE(c.run("image.circle_caliper", params).ok);
  const auto* circle = c.outputs.at("circle").asCircle2D();
  CHECK(circle->unit == Unit2D::Pixel);
  CHECK(circle->center[0] == doctest::Approx(64).epsilon(0.002));
  CHECK(circle->center[1] == doctest::Approx(64).epsilon(0.002));
  CHECK(circle->radius == doctest::Approx(30).epsilon(0.015));
  params["angleEnd"] = Value::number(150);
  REQUIRE(c.run("image.circle_caliper", params).ok);
  CHECK(c.outputs.at("circle").asCircle2D()->radius == doctest::Approx(30).epsilon(0.02));
  params["direction"] = Value::text("outside_in");
  params["polarity"] = Value::text("rising");
  REQUIRE(c.run("image.circle_caliper", params).ok);
  cv::Mat smallSignal = circleImage(128, 30) * 1e-20;
  c.inputs["image"] = image(smallSignal);
  params["minGradient"] = Value::number(1e-22);
  REQUIRE(c.run("image.circle_caliper", params).ok);
  CHECK(c.outputs.at("circle").asCircle2D()->radius == doctest::Approx(30).epsilon(0.02));
  c.inputs["image"] = image(cv::Mat(128, 128, CV_32F, cv::Scalar(0.5)));
  CHECK(c.run("image.circle_caliper", params).code == "no_target");
}

TEST_CASE("P1 ring widths preserve radial pairing direction and invalid no-target semantics") {
  test::OpCall c;
  c.inputs = {{"image", image(circleImage(128, 20, 35))}};
  auto params = scanParams();
  REQUIRE(c.run("image.ring_width", params).ok);
  for (const char* port : {"minimum", "maximum", "mean"}) {
    const auto* width = c.outputs.at(port).asMeasurement();
    REQUIRE(width != nullptr);
    CHECK(width->ok);
    CHECK(width->unit == "px");
    CHECK(width->value == doctest::Approx(15).epsilon(0.03));
  }
  const double mean = c.outputs.at("mean").asMeasurement()->value;
  params["direction"] = Value::text("outside_in");
  REQUIRE(c.run("image.ring_width", params).ok);
  CHECK(c.outputs.at("mean").asMeasurement()->value == doctest::Approx(mean));
  c.inputs["image"] = image(cv::Mat(128, 128, CV_32F, cv::Scalar(0.5)));
  REQUIRE(c.run("image.ring_width", params).ok);
  CHECK_FALSE(c.outputs.at("mean").asMeasurement()->ok);
  CHECK_FALSE(std::isfinite(c.outputs.at("mean").asMeasurement()->value));
  CHECK(data(c, "quality")["valid"] == false);
}

TEST_CASE("P1 full chessboard corner detection yields a physically scaled camera pose") {
  test::OpCall c;
  cv::Mat board(300, 400, CV_8U, cv::Scalar(127));
  for (int y = 0; y < 5; ++y)
    for (int x = 0; x < 6; ++x)
      board(cv::Rect(110 + x * 30, 75 + y * 30, 30, 30)).setTo((x + y) % 2 ? 255 : 0);
  Record calib = camera(400, 300);
  calib.data["cameraMatrix"][4] = 500;
  c.inputs = {{"image", image(board)}, {"calib", Data::record(calib)}};
  const auto status = c.run("image.board_pose", {{"columns", Value::integer(5)},
                                                 {"rows", Value::integer(4)},
                                                 {"squareSize", Value::number(0.02)},
                                                 {"rejectAmbiguous", Value::boolean(false)}});
  REQUIRE_MESSAGE(status.ok, status.message);
  CHECK(data(c, "corners")["points"].size() == 20);
  CHECK(c.outputs.at("pose").asTransform()->m[11] == doctest::Approx(1.0 / 3).epsilon(0.01));
  CHECK(c.outputs.at("rms").asMeasurement()->value < 0.2);
  CHECK(data(c, "quality")["ambiguous"].is_boolean());
}

TEST_CASE("P1 resampling validity excludes missing interpolation support") {
  test::OpCall c;
  Record calib = camera();
  calib.data["cameraMatrix"] = {20., 0., 16., 0., 20., 12., 0., 0., 1.};
  calib.data["distortion"] = {0.4, 0., 0., 0., 0.};
  c.inputs = {{"image", image(cv::Mat(24, 32, CV_8U, cv::Scalar(100)))},
              {"calib", Data::record(calib)}};
  REQUIRE(c.run("image.undistort").ok);
  const auto valid = cvx::view(*c.outputs.at("valid").asImage());
  CHECK(cv::countNonZero(valid) > 0);
  CHECK(cv::countNonZero(valid) < valid.rows * valid.cols);
  Record plane{calibration::kPlaneType,
               {{"H", {1., 0., 0.25, 0., 1., 0., 0., 0., 1.}}, {"unit", "mm"}}};
  c.inputs["calib"] = Data::record(plane);
  REQUIRE(c.run("image.warp_perspective",
                {{"width", Value::integer(32)}, {"height", Value::integer(24)}})
              .ok);
  CHECK(c.outputs.at("valid").asImage()->at(0, 0, 0) == 0);
  CHECK(c.outputs.at("valid").asImage()->at(1, 1, 0) == 255);
}

TEST_CASE("P1 NCC remains brightness invariant and records actual sampled scale") {
  test::OpCall c;
  cv::Mat pattern(9, 11, CV_8U);
  cv::RNG(421).fill(pattern, cv::RNG::UNIFORM, 20, 220);
  c.inputs = {{"image", image(pattern)}};
  REQUIRE(c.run("image.template_build").ok);
  const auto model = c.outputs.at("model"), templ = c.outputs.at("template");
  cv::Mat resized, adjusted;
  const int w = cvRound(11 * 1.5), h = cvRound(9 * 1.5);
  cv::resize(pattern, resized, cv::Size(w, h), 0, 0, cv::INTER_LINEAR);
  resized.convertTo(adjusted, CV_8U, 0.6, 50);
  cv::Mat target(64, 64, CV_8U, cv::Scalar(20));
  adjusted.copyTo(target(cv::Rect(13, 18, w, h)));
  c.inputs = {{"image", image(target)}, {"template", templ}, {"model", model}};
  REQUIRE(c.run("image.template_match", {{"scaleMin", Value::number(1.5)},
                                         {"scaleMax", Value::number(1.5)},
                                         {"minScore", Value::number(0.99)}})
              .ok);
  REQUIRE(data(c, "instances")["items"].size() == 1);
  const auto& found = data(c, "instances")["items"][0];
  CHECK(found["position"][0].get<double>() == doctest::Approx(13 + (w - 1) / 2.0));
  CHECK(found["position"][1].get<double>() == doctest::Approx(18 + (h - 1) / 2.0));
  CHECK(found["size"][0] == w);
  CHECK(found["size"][1] == h);
}

TEST_CASE("P1 radial caliper tolerates missing sectors and rejects straight edges as circles") {
  test::OpCall c;
  cv::Mat disk = circleImage(128, 30);
  disk(cv::Rect(65, 0, 63, 64)).setTo(0);
  c.inputs = {{"image", image(disk)}};
  auto params = scanParams();
  params["polarity"] = Value::text("falling");
  REQUIRE(c.run("image.circle_caliper", params).ok);
  CHECK(c.outputs.at("circle").asCircle2D()->radius == doctest::Approx(30).epsilon(0.015));
  CHECK(data(c, "quality")["validFraction"].get<double>() < 1);
  cv::Mat straight(128, 128, CV_32F, cv::Scalar(1));
  straight(cv::Rect(95, 0, 33, 128)).setTo(0);
  c.inputs = {{"image", image(straight)}};
  params["angleStart"] = Value::number(-20);
  params["angleEnd"] = Value::number(20);
  CHECK_FALSE(c.run("image.circle_caliper", params).ok);
}

TEST_CASE(
    "P1 perspective chessboard detection resolves a tilted board without bypassing ambiguity gates") {
  test::OpCall c;
  cv::Mat pattern(600, 720, CV_8U);
  for (int y = 0; y < 5; ++y)
    for (int x = 0; x < 6; ++x)
      pattern(cv::Rect(x * 120, y * 120, 120, 120)).setTo((x + y) % 2 ? 255 : 0);
  Record calib = camera(400, 300);
  cv::Mat k(3, 3, CV_64F);
  const auto entries = calib.data["cameraMatrix"].get<std::vector<double>>();
  std::copy(entries.begin(), entries.end(), k.ptr<double>());
  cv::Mat rotation = (cv::Mat_<double>(3, 1) << 0.35, -0.2, 0.02);
  cv::Mat translation = (cv::Mat_<double>(3, 1) << -0.04, -0.03, 0.5);
  std::vector<cv::Point3d> vertices{
      {-0.02, -0.02, 0}, {0.1, -0.02, 0}, {0.1, 0.08, 0}, {-0.02, 0.08, 0}};
  std::vector<cv::Point2d> projected;
  cv::projectPoints(vertices, rotation, translation, k, cv::Mat(), projected);
  std::vector<cv::Point2f> destination;
  for (const auto& point : projected)
    destination.emplace_back(static_cast<float>(point.x * 4), static_cast<float>(point.y * 4));
  const auto homography = cv::getPerspectiveTransform(
      std::vector<cv::Point2f>{{0, 0}, {720, 0}, {720, 600}, {0, 600}}, destination);
  cv::Mat highResolution, board;
  cv::warpPerspective(pattern, highResolution, homography, cv::Size(1600, 1200), cv::INTER_LINEAR,
                      cv::BORDER_CONSTANT, cv::Scalar(127));
  cv::resize(highResolution, board, cv::Size(400, 300), 0, 0, cv::INTER_AREA);
  c.inputs = {{"image", image(board)}, {"calib", Data::record(calib)}};
  const auto status = c.run("image.board_pose", {{"columns", Value::integer(5)},
                                                 {"rows", Value::integer(4)},
                                                 {"squareSize", Value::number(0.02)}});
  REQUIRE_MESSAGE(status.ok, status.message);
  CHECK(data(c, "quality")["ambiguous"] == false);
  CHECK(c.outputs.at("rms").asMeasurement()->value < 0.2);
  cv::Mat matrix;
  cv::Rodrigues(rotation, matrix);
  const double expectedZ =
      translation.at<double>(2) + matrix.at<double>(2, 0) * 0.04 + matrix.at<double>(2, 1) * 0.03;
  CHECK(data(c, "instance")["position"][2].get<double>() ==
        doctest::Approx(expectedZ).epsilon(0.01));
}

TEST_CASE("P1 long image loops cooperate with cancellation before publishing results") {
  cancelled("image.circle_caliper", {{"image", image(circleImage(128, 30))}}, scanParams());
  const auto a = poses::make2D("a", "image", "px", {5, 5}, 0, {4, 4});
  const auto b = poses::make2D("b", "image", "px", {12, 12}, 0, {4, 4});
  cancelled("image.instances_mask",
            {{"image", image(cv::Mat(32, 32, CV_8U, cv::Scalar(0)))},
             {"instances", Data::record(poses::make2DList(Json::array({a.data, b.data})))}});
  test::OpCall c;
  cv::Mat pattern(9, 11, CV_8U);
  cv::RNG(801).fill(pattern, cv::RNG::UNIFORM, 0, 255);
  c.inputs = {{"image", image(pattern)}};
  REQUIRE(c.run("image.template_build").ok);
  cancelled("image.template_match", {{"image", image(cv::Mat(64, 64, CV_8U, cv::Scalar(0)))},
                                     {"template", c.outputs.at("template")},
                                     {"model", c.outputs.at("model")}});
}
