#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <opencv2/imgcodecs.hpp>

#include "helpers.h"
#include "lyflow/cloud_groups.h"
#include "lyflow/pose.h"
#include "lyflow_cv/adapter.h"
#include "../ops/p5_projection_common.h"

namespace lyflow::ops {
void registerP5Inspection(Registry&);
}
namespace {
using namespace lyflow;
using Json = nlohmann::json;
Registry inspectionRegistry() {
  Registry r;
  ops::registerP5Inspection(r);
  return r;
}
Data imageData(cv::Mat matrix) {
  Image out;
  REQUIRE(cvx::fromMat(std::move(matrix), out));
  return Data::image(std::move(out));
}
struct Fixture {
  PointCloud cloud;
  Image depth, status;
  Tensor map;
  ops::projection5::Grid grid;
  test::StubContext ctx;
  Fixture(
      int width = 9, int height = 9, double step = .001,
      std::function<double(int, int)> value = [](int, int) { return 1.; }) {
    grid.width = grid.fullWidth = width;
    grid.height = grid.fullHeight = height;
    grid.scale = 1;
    grid.imageFrame = "image";
    grid.gridFrame = grid.cloudFrame = "input";
    grid.projection = "orthographic";
    grid.origin = {0, 0};
    grid.resolution = {step, step};
    depth = Image::allocate(width, height, 1, PixelDepth::F32);
    status = Image::allocate(width, height, 1, PixelDepth::U8);
    map.shape = {height, width};
    for (int y = 0; y < height; ++y)
      for (int x = 0; x < width; ++x) {
        const float z = static_cast<float>(value(x, y));
        map.data.push_back(static_cast<float>(cloud.pointCount()));
        cloud.push(static_cast<float>(x * step), static_cast<float>(y * step), z);
        cloud.intensity.push_back(static_cast<float>(x + y));
        cloud.normals.insert(cloud.normals.end(), {0, 0, 1});
        cloud.rgb.insert(cloud.rgb.end(), {std::uint8_t(x), std::uint8_t(y), 99});
        reinterpret_cast<float*>(depth.mutablePixels())[y * width + x] = z;
        status.mutablePixels()[y * width + x] = 1;
      }
  }
  void hole(int x, int y) {
    const auto i = y * grid.width + x;
    status.mutablePixels()[i] = 0;
    reinterpret_cast<float*>(depth.mutablePixels())[i] = 0;
    map.data[i] = -1;
  }
  void assign(test::OpCall& call) {
    call.inputs = {
        {"cloud", Data::cloud(cloud)},
        {"depth", Data::image(depth)},
        {"status", Data::image(status)},
        {"indexMap", Data::tensor(map)},
        {"mapping", Data::record(ops::projection5::make(grid, cloud, depth, status, map, ctx))}};
    Plane plane;
    plane.d = -1;
    call.inputs["plane"] = Data::plane(plane);
  }
};
Record cameraRecord(int width, int height, double focal = 500) {
  return {calibration::kCameraType,
          {{"imageSize", {width, height}},
           {"cameraMatrix", {focal, 0., width / 2., 0., focal, height / 2., 0., 0., 1.}},
           {"distortion", Json::array()},
           {"frame", "camera"},
           {"unit", "m"}}};
}
class InspectionCancelled : public ExecContext {
public:
  explicit InspectionCancelled(int limit = 1) : limit(limit) {}
  bool cancelled() const override { return ++calls >= limit; }
  void progress(float, std::string_view) override {}
  void log(LogLevel, std::string) override {}
  const std::filesystem::path& baseDir() const override { return base; }
  int threadBudget() const override { return 1; }
  std::filesystem::path base;
  int limit;
  mutable int calls = 0;
};
}
TEST_CASE(
    "P5 hole fill appends synthetic points and preserves original identity channels and observations") {
  auto registry = inspectionRegistry();
  test::OpCall call(registry);
  Fixture f;
  f.hole(4, 4);
  f.hole(0, 0);
  f.assign(call);
  const auto original = f.cloud.xyz;
  const auto originalId = f.cloud.id;
  REQUIRE_MESSAGE(call.run("cloud.fill_holes").ok, call.status.message);
  const auto* cloud = call.outputs.at("cloud").asCloud();
  REQUIRE(cloud);
  CHECK(cloud->id != originalId);
  CHECK(cloud->pointCount() == f.cloud.pointCount() + 1);
  CHECK(cloud->channelsConsistent());
  CHECK(std::equal(original.begin(), original.end(), cloud->xyz.begin()));
  CHECK(f.cloud.xyz == original);
  const auto* indices = call.outputs.at("filledIndices").asIndices();
  REQUIRE(indices);
  REQUIRE(indices->values.size() == 1);
  CHECK(indices->sourceCloudId == cloud->id);
  const auto index = indices->values[0];
  CHECK(index == 81);
  CHECK(std::abs(cloud->xyz[3 * index] - .004) < 1e-8);
  CHECK(std::abs(cloud->xyz[3 * index + 1] - .004) < 1e-8);
  CHECK(cloud->xyz[3 * index + 2] == 1);
  CHECK(call.outputs.at("status").asImage()->at(4, 4, 0) == 2);
  CHECK(call.outputs.at("status").asImage()->at(0, 0, 0) == 0);
  CHECK(call.outputs.at("indexMap").asTensor()->data[40] == index);
  CHECK(call.outputs.at("filledMask").asImage()->at(4, 4, 0) == 255);
  ops::projection5::Grid g;
  CHECK(ops::projection5::parse(*cloud, *call.outputs.at("depth").asImage(),
                                *call.outputs.at("status").asImage(),
                                *call.outputs.at("indexMap").asTensor(),
                                *call.outputs.at("mapping").asRecord(), g, f.ctx)
            .ok);
  CHECK(call.outputs.at("mapping").asRecord()->data["parentCloudId"] == std::to_string(originalId));
}
TEST_CASE(
    "P5 filling respects signed orthographic height and discontinuities without recursive synthetic support") {
  auto registry = inspectionRegistry();
  test::OpCall call(registry);
  for (double value : {0., -.01}) {
    Fixture f(7, 7, .001, [=](int, int) { return value; });
    f.hole(3, 3);
    f.assign(call);
    REQUIRE(call.run("cloud.fill_holes").ok);
    CHECK(call.outputs.at("depth").asImage()->at(3, 3, 0) == doctest::Approx(value));
    CHECK(call.outputs.at("status").asImage()->at(3, 3, 0) == 2);
  }
  Fixture step(7, 7, .001, [](int x, int) { return x < 3 ? 1. : 1.1; });
  step.hole(3, 3);
  step.assign(call);
  REQUIRE(call.run("cloud.fill_holes").ok);
  CHECK(call.outputs.at("filledIndices").asIndices()->values.empty());
  Fixture patch(9, 9);
  for (int y = 3; y <= 5; ++y)
    for (int x = 3; x <= 5; ++x)
      patch.hole(x, y);
  patch.assign(call);
  REQUIRE(call.run("cloud.fill_holes", {{"closeKernel", Value::integer(5)}}).ok);
  CHECK(call.outputs.at("status").asImage()->at(4, 4, 0) == 0);
}
TEST_CASE(
    "P5 physical signed defects measure full boundary cells and independent property decisions") {
  auto registry = inspectionRegistry();
  test::OpCall call(registry);
  Fixture f(7, 7, .001, [](int x, int y) {
    return x >= 2 && x <= 4 && y >= 2 && y <= 4 ? 1.002 : x == 6 && y == 6 ? .997 : 1.;
  });
  f.assign(call);
  REQUIRE_MESSAGE(call.run("depth.defects").ok, call.status.message);
  const auto& result = call.outputs.at("defects").asRecord()->data;
  CHECK(result["state"] == "defect");
  REQUIRE(result["items"].size() == 2);
  const auto& up = result["items"][0];
  const auto& down = result["items"][1];
  CHECK(up["support"] == 9);
  CHECK(down["support"] == 1);
  CHECK(std::abs(up["length"].get<double>() - .003) < 1e-8);
  CHECK(std::abs(up["width"].get<double>() - .003) < 1e-8);
  CHECK(std::abs(up["area"].get<double>() - 9e-6) < 1e-12);
  CHECK(std::abs(up["volume"].get<double>() - 9e-6 * (1.002f - 1)) < 1e-12);
  CHECK(std::abs(down["area"].get<double>() - 1e-6) < 1e-12);
  CHECK(call.outputs.at("mask").asImage()->at(6, 6, 0) == 128);
  for (const char* mode : {"up", "down"}) {
    REQUIRE(call.run("depth.defects", {{"mode", Value::text(mode)}}).ok);
    CHECK(call.outputs.at("defects").asRecord()->data["items"].size() == 1);
  }
  REQUIRE(call.run("depth.defects", {{"lengthThreshold", Value::number(.004)},
                                     {"widthThreshold", Value::number(.004)},
                                     {"areaThreshold", Value::number(1e-4)},
                                     {"volumeThreshold", Value::number(1e-5)}})
              .ok);
  CHECK(call.outputs.at("defects").asRecord()->data["state"] == "clear");
  REQUIRE(call.run("depth.defects", {{"checkLength", Value::boolean(false)},
                                     {"checkWidth", Value::boolean(false)},
                                     {"checkArea", Value::boolean(false)},
                                     {"checkVolume", Value::boolean(false)}})
              .ok);
  CHECK(call.outputs.at("defects").asRecord()->data["state"] == "clear");
}
TEST_CASE(
    "P5 defect coverage and closing never turn missing or zero residual pixels into measured defect volume") {
  auto registry = inspectionRegistry();
  test::OpCall call(registry);
  Fixture f(7, 7, .001, [](int x, int y) { return y == 3 && (x == 2 || x == 4) ? 1.002 : 1.; });
  f.assign(call);
  REQUIRE(call.run("depth.defects", {{"mode", Value::text("up")},
                                     {"denoise", Value::boolean(true)},
                                     {"denoiseKernel", Value::integer(3)}})
              .ok);
  const auto& result = call.outputs.at("defects").asRecord()->data;
  REQUIRE(result["items"].size() == 1);
  CHECK(result["items"][0]["support"] == 2);
  CHECK(std::abs(result["items"][0]["area"].get<double>() - 2e-6) < 1e-12);
  CHECK(call.outputs.at("mask").asImage()->at(3, 3, 0) == 0);
  Fixture missing;
  missing.hole(4, 4);
  missing.assign(call);
  REQUIRE(call.run("depth.defects").ok);
  CHECK(call.outputs.at("defects").asRecord()->data["state"] == "unknown");
  CHECK(call.outputs.at("defects").asRecord()->data["good"].is_null());
  Fixture equality(3, 3, .001, [](int, int) { return double(1.002f); });
  equality.assign(call);
  REQUIRE(call.run("depth.defects", {{"distanceThreshold", Value::number(double(1.002f) - 1)}}).ok);
  CHECK(call.outputs.at("defects").asRecord()->data["items"].empty());
}
TEST_CASE(
    "P5 true mesh plane profiles have metric span heights identities and independent attributes") {
  auto registry = inspectionRegistry();
  test::OpCall call(registry);
  Fixture f(9, 9, .001, [](int x, int) { return 1. + x * .0002; });
  f.assign(call);
  const std::unordered_map<std::string, Value> params{{"sliceCount", Value::integer(3)},
                                                      {"aType", Value::text("left-most")},
                                                      {"bType", Value::text("right-most")},
                                                      {"aWidth", Value::number(1)},
                                                      {"bWidth", Value::number(1)}};
  REQUIRE_MESSAGE(call.run("depth.profile_slices", params).ok, call.status.message);
  const auto& features = call.outputs.at("features").asRecord()->data;
  CHECK(features["state"] == "good");
  REQUIRE(features["items"].size() == 3);
  const auto* attribute = call.outputs.at("attributes").asTensor();
  REQUIRE(attribute);
  for (float value : attribute->data)
    CHECK(std::abs(value - .008) < 1e-7);
  const auto* points = call.outputs.at("sliceCloud").asCloud();
  REQUIRE(points);
  CHECK(points->pointCount() > 20);
  CHECK(points->hasIntensity());
  CHECK(points->hasNormals());
  CHECK(points->hasRgb());
  CHECK(points->channelsConsistent());
  const auto* profiles = call.outputs.at("profiles").asTensor();
  REQUIRE(profiles);
  CHECK(profiles->shape ==
        std::vector<std::int64_t>{static_cast<std::int64_t>(points->pointCount()), 4});
  for (std::size_t i = 0; i < points->pointCount(); ++i)
    CHECK(std::abs(points->xyz[3 * i + 2] - (1 + .2 * points->xyz[3 * i])) < 1e-7);
  CHECK(call.outputs.at("supports").asTensor()->shape[0] == points->pointCount());
  std::vector<cloud_groups::Group> groups;
  std::string frame, why;
  REQUIRE(cloud_groups::parse(*call.outputs.at("groups").asRecord(), *points,
                              *call.outputs.at("indices").asIndices(), groups, frame, why));
  CHECK(groups.size() == 3);
  CHECK(points->id != f.cloud.id);
  auto changed = params;
  changed["attributeMode"] = Value::text("y-y");
  REQUIRE(call.run("depth.profile_slices", changed).ok);
  for (float value : call.outputs.at("attributes").asTensor()->data)
    CHECK(std::abs(value - .0016) < 1e-7);
}
TEST_CASE("P5 profile feature modes and both region axes operate on real linked intersections") {
  auto registry = inspectionRegistry();
  test::OpCall call(registry);
  Fixture f(9, 9, .001, [](int x, int y) { return 1. + x * .0002 + y * .0003; });
  f.assign(call);
  for (const char* type : {"top", "bottom", "left-most", "right-most", "left-right-inflection",
                           "right-left-inflection"}) {
    REQUIRE_MESSAGE(call.run("depth.profile_slices", {{"sliceCount", Value::integer(1)},
                                                      {"aType", Value::text(type)},
                                                      {"bType", Value::text("bottom")},
                                                      {"aWidth", Value::number(1)},
                                                      {"bWidth", Value::number(1)}})
                        .ok,
                    call.status.message);
    const auto& point = call.outputs.at("features").asRecord()->data["items"][0]["a"];
    CHECK(point["valid"] == true);
    const bool right = std::string(type) == "top" || std::string(type) == "right-most" ||
                       std::string(type) == "right-left-inflection";
    CHECK(std::abs(point["x"].get<double>() - (right ? .004 : -.004)) < 1e-7);
  }
  REQUIRE(call.run("depth.profile_slices", {{"sliceCount", Value::integer(1)},
                                            {"direction", Value::text("y")},
                                            {"attributeMode", Value::text("y-y")},
                                            {"aType", Value::text("top")},
                                            {"bType", Value::text("bottom")},
                                            {"aWidth", Value::number(1)},
                                            {"bWidth", Value::number(1)}})
              .ok);
  CHECK(std::abs(call.outputs.at("attributes").asTensor()->data[0] - .0024) < 1e-7);
  call.inputs["region"] =
      Data::record(poses::make2D("roi", "image", "px", {4, 4}, poses::kPi / 4, {4, 4}));
  REQUIRE(call.run("depth.profile_slices", {{"sliceCount", Value::integer(2)},
                                            {"aType", Value::text("left-most")},
                                            {"bType", Value::text("right-most")},
                                            {"aWidth", Value::number(1)},
                                            {"bWidth", Value::number(1)}})
              .ok);
  CHECK(call.outputs.at("features").asRecord()->data["validCount"] == 2);
}
TEST_CASE("P5 profile holes remain separate chains and flat missing features remain unknown") {
  auto registry = inspectionRegistry();
  test::OpCall call(registry);
  Fixture f(9, 9, .001, [](int x, int) { return x < 4 ? 1. : 1.01; });
  for (int y = 0; y < 9; ++y)
    f.hole(4, y);
  f.assign(call);
  REQUIRE(call.run("depth.profile_slices", {{"sliceCount", Value::integer(1)},
                                            {"aWidth", Value::number(1)},
                                            {"bWidth", Value::number(1)}})
              .ok);
  const auto& features = call.outputs.at("features").asRecord()->data;
  CHECK(features["state"] == "unknown");
  CHECK(features["good"].is_null());
  CHECK(features["items"][0]["chainCount"] == 2);
  CHECK(call.outputs.at("attributeValid").asTensor()->data[0] == 0);
}
TEST_CASE(
    "P5 camera calibrated conic circles retain two positive depth solutions and the true tilted circle") {
  auto registry = inspectionRegistry();
  test::OpCall call(registry);
  cv::Mat scene(480, 640, CV_8UC4, cv::Scalar(255, 255, 255, 73));
  const cv::Vec3d center(.03, -.02, 1.1), normal(.3, .4, std::sqrt(.75));
  cv::Vec3d x = cv::Vec3d(1, 0, 0) - normal * normal[0];
  x /= cv::norm(x);
  const auto y = normal.cross(x);
  std::vector<cv::Point> polygon;
  for (int i = 0; i < 720; ++i) {
    const double angle = 2 * poses::kPi * i / 720;
    const auto p = center + .1 * (x * std::cos(angle) + y * std::sin(angle));
    polygon.emplace_back(cvRound(500 * p[0] / p[2] + 320), cvRound(500 * p[1] / p[2] + 240));
  }
  cv::fillConvexPoly(scene, polygon, cv::Scalar(0, 0, 0, 73));
  const auto original = scene.clone();
  call.inputs = {{"image", imageData(scene)}, {"calib", Data::record(cameraRecord(640, 480))}};
  REQUIRE_MESSAGE(call.run("image.find_circles_3d", {{"radius", Value::number(.1)}}).ok,
                  call.status.message);
  const auto& ellipses = call.outputs.at("ellipses").asRecord()->data["items"];
  REQUIRE(ellipses.size() == 1);
  const auto& circles = call.outputs.at("circles").asRecord()->data["items"];
  REQUIRE(circles.size() == 2);
  double centerError = 1e9, normalError = 1e9;
  for (const auto& circle : circles) {
    CHECK(circle["ambiguous"] == true);
    CHECK(circle["center"][2].get<double>() > 0);
    cv::Vec3d c(circle["center"][0], circle["center"][1], circle["center"][2]),
        n(circle["normal"][0], circle["normal"][1], circle["normal"][2]);
    if (cv::norm(c - center) < centerError) {
      centerError = cv::norm(c - center);
      normalError = cv::norm(n - normal);
    }
  }
  CHECK(centerError < .02);
  CHECK(normalError < .05);
  CHECK(cv::norm(scene, original, cv::NORM_INF) == 0);
  const auto overlay = cvx::view(*call.outputs.at("image").asImage());
  REQUIRE(overlay.channels() == 4);
  cv::Mat alpha;
  cv::extractChannel(overlay, alpha, 3);
  CHECK(cv::countNonZero(alpha != 73) == 0);
  for (int depth : {CV_16U, CV_32F}) {
    cv::Mat variant;
    scene.convertTo(variant, CV_MAKETYPE(depth, 4), depth == CV_16U ? 257 : 1. / 255);
    call.inputs["image"] = imageData(variant);
    REQUIRE_MESSAGE(call.run("image.find_circles_3d", {{"radius", Value::number(.1)}}).ok,
                    call.status.message);
    CHECK(call.outputs.at("ellipses").asRecord()->data["items"].size() == 1);
    const auto* image = call.outputs.at("image").asImage();
    REQUIRE(image);
    CHECK(image->channels == 4);
    CHECK(image->depth == (depth == CV_16U ? PixelDepth::U16 : PixelDepth::F32));
    CHECK(std::abs(image->at(0, 0, 3) - (depth == CV_16U ? 73 * 257 : 73. / 255)) < 1e-6);
  }
  cv::Mat distorted(960, 1280, CV_8U, cv::Scalar(255));
  std::vector<cv::Point> distortedBoundary;
  for (int i = 0; i < 1440; ++i) {
    const double a = 2 * poses::kPi * i / 1440, x = .75 + .1 * std::cos(a), y = .1 * std::sin(a),
                 radial = 1 + x * x + y * y;
    distortedBoundary.emplace_back(cvRound(640 + 300 * x * radial),
                                   cvRound(480 + 300 * y * radial));
  }
  cv::fillConvexPoly(distorted, distortedBoundary, cv::Scalar(0));
  auto strongCamera = cameraRecord(1280, 960, 300);
  strongCamera.data["distortion"] = {1, 0, 0, 0};
  call.inputs = {{"image", imageData(distorted)}, {"calib", Data::record(strongCamera)}};
  REQUIRE_MESSAGE(call.run("image.find_circles_3d", {{"radius", Value::number(.1)}}).ok,
                  call.status.message);
  bool recovered = false;
  for (const auto& solution : call.outputs.at("circles").asRecord()->data["items"]) {
    const cv::Vec3d c(solution["center"][0], solution["center"][1], solution["center"][2]);
    recovered |= cv::norm(c - cv::Vec3d(.75, 0, 1)) < .02 &&
                 std::abs(solution["normal"][2].get<double>()) > .98;
  }
  CHECK(recovered);
  Tensor badRadii;
  badRadii.shape = {3};
  badRadii.data = {.1f, .1f, .1f};
  call.inputs["radii"] = Data::tensor(badRadii);
  CHECK_FALSE(call.run("image.find_circles_3d").ok);
  CHECK(call.outputs.empty());
}
TEST_CASE("P5 space circle blank and noncircular boundaries do not manufacture detections") {
  auto registry = inspectionRegistry();
  test::OpCall call(registry);
  for (int variant = 0; variant < 2; ++variant) {
    cv::Mat scene(160, 200, CV_8U, cv::Scalar(255));
    if (variant)
      cv::rectangle(scene, {30, 20, 120, 100}, cv::Scalar(0), -1);
    call.inputs = {{"image", imageData(scene)}, {"calib", Data::record(cameraRecord(200, 160))}};
    REQUIRE_MESSAGE(call.run("image.find_circles_3d").ok, call.status.message);
    CHECK(call.outputs.at("circles").asRecord()->data["items"].empty());
  }
  cv::Mat fragments(240, 320, CV_8U, cv::Scalar(255));
  for (int angle : {0, 90, 180, 270})
    cv::ellipse(fragments, {160, 120}, {75, 50}, 25, angle + 8, angle + 82, cv::Scalar(0), -1,
                cv::LINE_AA);
  call.inputs = {{"image", imageData(fragments)}, {"calib", Data::record(cameraRecord(320, 240))}};
  REQUIRE_MESSAGE(call.run("image.find_circles_3d").ok, call.status.message);
  INFO(call.outputs.at("ellipses").asRecord()->data.dump());
  CHECK(call.outputs.at("ellipses").asRecord()->data["items"].size() == 1);
}
TEST_CASE("P5 checked in perspective circle PNGs recover their independent physical truth") {
  auto registry = inspectionRegistry();
  test::OpCall call(registry);
  const auto examples =
      (std::filesystem::path(__FILE__).parent_path() / "../../../examples").lexically_normal();
  std::ifstream truthFile(examples / "p5-fixtures.json");
  REQUIRE(truthFile.good());
  Json truth;
  truthFile >> truth;
  std::ifstream cameraFile(examples / "p5-circle-camera.json");
  REQUIRE(cameraFile.good());
  Json camera;
  cameraFile >> camera;
  for (const char* name : {"front", "tilted"}) {
    CAPTURE(std::string(name));
    std::ifstream file(examples / (std::string("p5-circle-") + name + ".png"), std::ios::binary);
    REQUIRE(file.good());
    const std::vector<unsigned char> encoded((std::istreambuf_iterator<char>(file)), {});
    auto image = cv::imdecode(encoded, cv::IMREAD_UNCHANGED);
    REQUIRE(!image.empty());
    call.inputs = {{"image", imageData(image)},
                   {"calib", Data::record(Record{camera["type"], camera["data"]})}};
    const auto& expected = truth["circles"][name];
    REQUIRE_MESSAGE(
        call.run("image.find_circles_3d", {{"radius", Value::number(expected["radiusM"])}}).ok,
        call.status.message);
    const auto& circles = call.outputs.at("circles").asRecord()->data["items"];
    REQUIRE(!circles.empty());
    CHECK(circles.size() <= 2);
    bool found = false;
    for (const auto& item : circles) {
      cv::Vec3d center, normal, expectedCenter, expectedNormal;
      for (int j = 0; j < 3; ++j) {
        center[j] = item["center"][j];
        normal[j] = item["normal"][j];
        expectedCenter[j] = expected["centerM"][j];
        expectedNormal[j] = expected["unorientedNormal"][j];
      }
      if (cv::norm(center - expectedCenter) <= expected["centerToleranceM"].get<double>() &&
          std::abs(normal.dot(expectedNormal)) >= expected["normalCosineMinimum"].get<double>())
        found = true;
    }
    INFO(circles.dump());
    CHECK(found);
    const auto& poses = call.outputs.at("instances").asRecord()->data["items"];
    REQUIRE(poses.size() == circles.size());
    for (const auto& pose : poses) {
      CHECK(pose["attributes"]["shape"] == "circle");
      CHECK(pose["attributes"]["circleRadius"] == expected["radiusM"]);
    }
  }
}
TEST_CASE("P5 perspective hole fill and plane defects use calibrated meter cell footprints") {
  auto registry = inspectionRegistry();
  test::OpCall call(registry);
  Fixture f(7, 7, .001, [](int x, int y) { return x == 1 && y == 1 ? 1.02 : 1.; });
  f.grid.projection = "perspective";
  f.grid.gridFrame = f.grid.cloudFrame = "camera";
  f.grid.camera = {7, 7, {120, 0, 3, 0, 120, 3, 0, 0, 1}, {}, "camera"};
  for (int y = 0; y < 7; ++y)
    for (int x = 0; x < 7; ++x) {
      const auto i = y * 7 + x;
      const double z = reinterpret_cast<float*>(f.depth.mutablePixels())[i];
      f.cloud.xyz[3 * i] = static_cast<float>((x - 3) * z / 120);
      f.cloud.xyz[3 * i + 1] = static_cast<float>((y - 3) * z / 120);
    }
  f.hole(3, 3);
  f.assign(call);
  REQUIRE(call.run("cloud.fill_holes").ok);
  const auto* cloud = call.outputs.at("cloud").asCloud();
  REQUIRE(cloud);
  CHECK(std::abs(cloud->xyz[cloud->xyz.size() - 3]) < 1e-9);
  CHECK(std::abs(cloud->xyz[cloud->xyz.size() - 2]) < 1e-9);
  REQUIRE(call.run("depth.defects", {{"planeFrame", Value::text("camera")}}).ok);
  const auto& items = call.outputs.at("defects").asRecord()->data["items"];
  REQUIRE(items.size() == 1);
  const double observedArea = double(1.02f) * double(1.02f) / 14400;
  CHECK(std::abs(items[0]["area"].get<double>() - observedArea) < 1e-12);
  CHECK(std::abs(items[0]["volume"].get<double>() - (1.02f - 1) * observedArea) < 1e-12);
}
TEST_CASE(
    "P5 perspective defects project measured depth cell footprints instead of baseline rays") {
  auto registry = inspectionRegistry();
  test::OpCall call(registry);
  Fixture f(3, 3, .001, [](int, int) { return 2.; });
  f.grid.projection = "perspective";
  f.grid.gridFrame = f.grid.cloudFrame = "camera";
  f.grid.camera = {3, 3, {100, 0, 1, 0, 100, 1, 0, 0, 1}, {}, "camera"};
  for (int y = 0; y < 3; ++y)
    for (int x = 0; x < 3; ++x) {
      const auto i = y * 3 + x;
      f.cloud.xyz[3 * i] = static_cast<float>((x - 1) * 2. / 100);
      f.cloud.xyz[3 * i + 1] = static_cast<float>((y - 1) * 2. / 100);
    }
  for (bool tilted : {false, true}) {
    f.assign(call);
    if (tilted) {
      Plane plane;
      plane.normal[0] = 0;
      plane.normal[1] = 3;
      plane.normal[2] = 4;
      plane.d = -4;
      call.inputs["plane"] = Data::plane(plane);
    }
    REQUIRE_MESSAGE(call.run("depth.defects", {{"planeFrame", Value::text("camera")}}).ok,
                    call.status.message);
    const auto& result = call.outputs.at("defects").asRecord()->data;
    REQUIRE(result["items"].size() == 1);
    const double area = 9 * 4. / 10000 * (tilted ? .8 : 1.);
    CHECK(std::abs(result["items"][0]["area"].get<double>() - area) < 1e-12);
    CHECK(std::abs(result["items"][0]["volume"].get<double>() - area * (tilted ? .8 : 1.)) < 1e-12);
    CHECK(result["areaModel"] == "observed_depth_orthogonal_footprint");
  }
}
TEST_CASE(
    "P5 depth inspection rejects mismatched payload units frames limits and cancellation atomically") {
  auto registry = inspectionRegistry();
  test::OpCall call(registry);
  Fixture f;
  f.assign(call);
  CHECK_FALSE(call.run("cloud.fill_holes", {{"fillKernel", Value::integer(2)}}).ok);
  CHECK(call.outputs.empty());
  CHECK_FALSE(call.run("depth.defects", {{"planeFrame", Value::text("wrong")}}).ok);
  CHECK(call.outputs.empty());
  auto mapping = *call.inputs.at("mapping").asRecord();
  mapping.data["sourceCloudId"] = "0";
  call.inputs["mapping"] = Data::record(mapping);
  CHECK_FALSE(call.run("cloud.fill_holes").ok);
  CHECK(call.outputs.empty());
  f.assign(call);
  auto map = f.map;
  map.data[0] = .5f;
  auto malformed = ops::projection5::make(f.grid, f.cloud, f.depth, f.status, map, f.ctx);
  call.inputs["indexMap"] = Data::tensor(map);
  call.inputs["mapping"] = Data::record(malformed);
  CHECK_FALSE(call.run("depth.defects").ok);
  CHECK(call.outputs.empty());
  Fixture large(100, 100);
  large.assign(call);
  CHECK_FALSE(call.run("depth.profile_slices", {{"sliceCount", Value::integer(1024)}}).ok);
  CHECK(call.outputs.empty());
  for (const char* id :
       {"cloud.fill_holes", "depth.defects", "depth.profile_slices", "image.find_circles_3d"}) {
    if (std::string(id) == "image.find_circles_3d")
      call.inputs = {{"image", imageData(cv::Mat(48, 64, CV_8U, cv::Scalar(255)))},
                     {"calib", Data::record(cameraRecord(64, 48))}};
    else
      f.assign(call);
    const auto* op = registry.find(id);
    REQUIRE(op);
    std::unordered_map<std::string, Value> params;
    for (const auto& item : op->params)
      params[item.name] = item.def;
    for (int limit : {1, 20}) {
      InspectionCancelled ctx(limit);
      call.outputs.clear();
      Inputs inputs(call.inputs);
      Outputs outputs(call.outputs);
      ParamView view(params, ctx.base);
      CHECK(op->compute(inputs, view, outputs, ctx).code == "cancelled");
      CHECK(ctx.calls >= limit);
      CHECK(call.outputs.empty());
    }
  }
}
