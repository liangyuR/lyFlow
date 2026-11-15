#include <doctest/doctest.h>

#include "lyflow_cv/compat.h"

#include "helpers.h"
#include "lyflow/calibration.h"
#include "lyflow/pose.h"
#include "lyflow_cv/adapter.h"

namespace {
using namespace lyflow;
using Json = nlohmann::json;

Record camera() {
  return {calibration::kCameraType,
          {{"imageSize", {16, 12}},
           {"cameraMatrix", {10., 0., 8., 0., 10., 6., 0., 0., 1.}},
           {"distortion", Json::array()},
           {"frame", "camera"},
           {"unit", "m"}}};
}

Data nativeImage(const cv::Mat& matrix, int scale = 1) {
  Image image;
  REQUIRE(cvx::fromMat(matrix, image));
  image.scale = scale;
  return Data::image(std::move(image));
}

PointCloud cloud() {
  PointCloud cloud;
  for (const auto& point : {cv::Vec3f(0, 0, 1), cv::Vec3f(0, 0, 2), cv::Vec3f(.2f, 0, 1),
                            cv::Vec3f(0, 0, -1), cv::Vec3f(100, 0, 1)}) {
    cloud.push(point[0], point[1], point[2]);
    cloud.intensity.push_back(static_cast<float>(cloud.pointCount()));
    cloud.normals.insert(cloud.normals.end(), {0, 0, 1});
    cloud.rgb.insert(cloud.rgb.end(), {11, 22, 33});
  }
  return cloud;
}

Record instances(const std::string& frame = "camera") {
  return poses::make3DList(
      Json::array({poses::make3D("part", frame, {0, 0, 1}, {0, 0, 0, 1}, {.4, .2, .1}).data}),
      frame);
}

void checkEmpty(const test::OpCall& call) {
  CHECK(call.outputs.at("indices").asIndices()->values.empty());
  CHECK(call.outputs.at("cloud").asCloud()->pointCount() == 0);
  CHECK(call.outputs.at("quality").asRecord()->data.at("selected") == 0);
}

class CancelContext final : public ExecContext {
public:
  bool cancelled() const override { return true; }
  void progress(float, std::string_view) override {}
  void log(LogLevel, std::string) override {}
  const std::filesystem::path& baseDir() const override { return base_; }
  int threadBudget() const override { return 1; }

private:
  std::filesystem::path base_;
};
}

TEST_CASE("P2 calibrated image ROI retains original cloud indices channels and visible depth") {
  const auto original = cloud();
  cv::Mat mask(12, 16, CV_8UC1, cv::Scalar(0));
  mask.at<std::uint8_t>(6, 8) = 255;
  test::OpCall call;
  call.inputs = {{"cloud", Data::cloud(original)},
                 {"mask", nativeImage(mask)},
                 {"calib", Data::record(camera())}};
  REQUIRE(call.run("cloud.image_roi").ok);
  const auto* selected = call.outputs.at("cloud").asCloud();
  const auto* indices = call.outputs.at("indices").asIndices();
  CHECK(indices->sourceCloudId == original.id);
  CHECK(indices->values == std::vector<std::int32_t>{0});
  CHECK(selected->xyz == std::vector<float>{0, 0, 1});
  CHECK(selected->intensity == std::vector<float>{1});
  CHECK(selected->normals == std::vector<float>{0, 0, 1});
  CHECK(selected->rgb == std::vector<std::uint8_t>{11, 22, 33});
  REQUIRE(call.run("cloud.image_roi", {{"visibleOnly", Value::boolean(false)}}).ok);
  CHECK(call.outputs.at("indices").asIndices()->values == std::vector<std::int32_t>{0, 1});
  REQUIRE(call.run("cloud.image_roi", {{"occlusionTolerance", Value::number(1)}}).ok);
  CHECK(call.outputs.at("indices").asIndices()->values == std::vector<std::int32_t>{0, 1});
  CHECK(call.inputs.at("cloud").asCloud()->xyz == original.xyz);
  CHECK(cv::countNonZero(mask) == 1);
  call.inputs["mask"] = nativeImage(cv::Mat(12, 16, CV_8U, cv::Scalar(0)));
  REQUIRE(call.run("cloud.image_roi").ok);
  checkEmpty(call);
  call.inputs["cloud"] = Data::cloud(PointCloud{});
  REQUIRE(call.run("cloud.image_roi").ok);
  checkEmpty(call);
}

TEST_CASE("P2 calibrated image ROI checks frames distortion preview grid and atomic failures") {
  test::OpCall call;
  auto calib = camera();
  calib.data["distortion"] = {.2, -.01, .03, -.02, 0.};
  PointCloud distorted;
  distorted.push(.2f, .1f, 1);
  std::vector<cv::Point2d> truth;
  cv::projectPoints(std::vector<cv::Point3d>{{.2f, .1f, 1}}, cv::Vec3d::all(0), cv::Vec3d::all(0),
                    cv::Mat(cv::Matx33d(10, 0, 8, 0, 10, 6, 0, 0, 1)),
                    std::vector<double>{.2, -.01, .03, -.02, 0.}, truth);
  cv::Mat mask(12, 16, CV_8U, cv::Scalar(0));
  mask.at<std::uint8_t>(cvRound(truth[0].y), cvRound(truth[0].x)) = 255;
  call.inputs = {{"cloud", Data::cloud(distorted)},
                 {"calib", Data::record(calib)},
                 {"mask", nativeImage(mask)}};
  REQUIRE(call.run("cloud.image_roi").ok);
  CHECK(call.outputs.at("indices").asIndices()->values == std::vector<std::int32_t>{0});
  cv::Mat preview(6, 8, CV_8U, cv::Scalar(0));
  preview.at<std::uint8_t>(static_cast<int>(std::floor((truth[0].y + .5) / 2)),
                           static_cast<int>(std::floor((truth[0].x + .5) / 2))) = 255;
  call.inputs["mask"] = nativeImage(preview, 2);
  REQUIRE(call.run("cloud.image_roi").ok);
  CHECK(call.outputs.at("indices").asIndices()->values == std::vector<std::int32_t>{0});
  CHECK_FALSE(call.run("cloud.image_roi", {{"cloudFrame", Value::text("world")}}).ok);
  CHECK(call.outputs.empty());
  call.inputs["cloudToCamera"] = Data::transform(Transform{});
  REQUIRE(call.run("cloud.image_roi", {{"cloudFrame", Value::text("world")}}).ok);
  Transform reflection;
  reflection.m[0] = -1;
  call.inputs["cloudToCamera"] = Data::transform(reflection);
  CHECK_FALSE(call.run("cloud.image_roi").ok);
  CHECK(call.outputs.empty());
  call.inputs.erase("cloudToCamera");
  call.inputs["mask"] = nativeImage(cv::Mat(12, 15, CV_8U, cv::Scalar(0)));
  CHECK_FALSE(call.run("cloud.image_roi").ok);
  call.inputs["mask"] = nativeImage(cv::Mat(12, 16, CV_16U, cv::Scalar(0)));
  CHECK_FALSE(call.run("cloud.image_roi").ok);
}

TEST_CASE(
    "P2 centered model projection applies pose camera transform and preserves image channels") {
  PointCloud model;
  model.push(0, 0, 0);
  model.push(.2f, 0, 0);
  model.push(0, .2f, 0);
  for (const int type : {CV_8UC1, CV_16UC3, CV_32FC4}) {
    cv::Mat source(12, 16, type, cv::Scalar(0, 0, 0, .37));
    const auto before = source.clone();
    test::OpCall call;
    call.inputs = {{"image", nativeImage(source)},
                   {"model", Data::cloud(model)},
                   {"instances", Data::record(instances())},
                   {"calib", Data::record(camera())}};
    REQUIRE(call.run("image.render_model_3d").ok);
    const auto* image = call.outputs.at("image").asImage();
    CHECK(image->channels == source.channels());
    CHECK(image->at(8, 6, 0) > 0);
    CHECK(image->at(10, 6, 0) > 0);
    CHECK(image->at(8, 8, 0) > 0);
    CHECK(image->at(7, 6, 0) == 0);
    if (source.channels() == 4)
      CHECK(image->at(8, 6, 3) == doctest::Approx(.37));
    CHECK(call.outputs.at("quality").asRecord()->data.at("renderedPixels") == 3);
    CHECK(cv::norm(source, before, cv::NORM_INF) == 0);
    call.inputs["instances"] = Data::record(instances("world"));
    CHECK_FALSE(call.run("image.render_model_3d").ok);
    Transform extrinsic;
    extrinsic.m[3] = -.2f;
    call.inputs["sceneToCamera"] = Data::transform(extrinsic);
    REQUIRE(call.run("image.render_model_3d").ok);
    CHECK(call.outputs.at("image").asImage()->at(6, 6, 0) > 0);
    call.inputs["instances"] = Data::record(poses::make3DList(Json::array(), "world"));
    REQUIRE(call.run("image.render_model_3d").ok);
    CHECK(call.outputs.at("quality").asRecord()->data.at("renderedPixels") == 0);
    CHECK(cv::norm(cvx::view(*call.outputs.at("image").asImage()), source, cv::NORM_INF) == 0);
  }
  test::OpCall preview;
  preview.inputs = {{"image", nativeImage(cv::Mat(6, 8, CV_8UC3, cv::Scalar(0)), 2)},
                    {"model", Data::cloud(model)},
                    {"instances", Data::record(instances())},
                    {"calib", Data::record(camera())}};
  REQUIRE(preview.run("image.render_model_3d").ok);
  const auto* raster = preview.outputs.at("image").asImage();
  CHECK(raster->scale == 2);
  for (const auto& pixel : {cv::Point(4, 3), cv::Point(5, 3), cv::Point(4, 4)})
    CHECK(raster->at(pixel.x, pixel.y, 0) > 0);
  CHECK(preview.outputs.at("quality").asRecord()->data.at("renderedPixels") == 3);

  test::OpCall overlapping;
  const auto far = poses::make3D("far", "camera", {0, 0, 2}, {0, 0, 0, 1}, {.4, .2, .1});
  const auto near = instances().data.at("items")[0];
  overlapping.inputs = {
      {"image", nativeImage(cv::Mat(12, 16, CV_8UC3, cv::Scalar(0)))},
      {"model", Data::cloud(model)},
      {"instances", Data::record(poses::make3DList(Json::array({far.data, near}), "camera"))},
      {"calib", Data::record(camera())}};
  REQUIRE(overlapping.run("image.render_model_3d").ok);
  const auto* nearest = overlapping.outputs.at("image").asImage();
  CHECK(nearest->at(8, 6, 0) == 0);
  CHECK(nearest->at(8, 6, 1) == 255);
  CHECK(nearest->at(8, 6, 2) == 85);
}

TEST_CASE(
    "Model renderer exposes binary boundaries metric Z and camera normals from nearest points") {
  PointCloud model;
  for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x) {
      model.push(x * .2f, y * .2f, 0);
      model.normals.insert(model.normals.end(), {0, 0, 2});
    }
  test::OpCall call;
  const auto near = poses::make3D("near", "camera", {0, 0, 2}, {0, 0, 0, 1}, {1, 1, 1});
  const auto far = poses::make3D("far", "camera", {0, 0, 4}, {0, 0, 0, 1}, {1, 1, 1});
  call.inputs = {
      {"image", nativeImage(cv::Mat(12, 16, CV_16UC4, cv::Scalar(101, 202, 303, 404)))},
      {"model", Data::cloud(model)},
      {"instances", Data::record(poses::make3DList(Json::array({far.data, near.data}), "camera"))},
      {"calib", Data::record(camera())}};
  for (const char* mode : {"overlay", "mask", "edge_mask", "depth", "normal"}) {
    CAPTURE(mode);
    REQUIRE(call.run("image.render_model_3d", {{"renderMode", Value::text(mode)}}).ok);
    const auto* mask = call.out("mask").asImage();
    const auto* edge = call.out("edge_mask").asImage();
    const auto* depth = call.out("depth").asImage();
    const auto* normal = call.out("normal").asImage();
    REQUIRE(normal != nullptr);
    CHECK(mask->depth == PixelDepth::U8);
    CHECK(mask->channels == 1);
    CHECK(mask->at(8, 6, 0) == 255);
    CHECK(mask->at(0, 0, 0) == 0);
    CHECK(edge->at(8, 6, 0) == 0);
    CHECK(edge->at(7, 6, 0) == 255);
    CHECK(depth->depth == PixelDepth::F32);
    CHECK(depth->channels == 1);
    CHECK(depth->at(8, 6, 0) == doctest::Approx(2));
    CHECK(depth->at(9, 6, 0) == doctest::Approx(2));
    CHECK(depth->at(0, 0, 0) == 0);
    CHECK(normal->depth == PixelDepth::F32);
    CHECK(normal->channels == 3);
    CHECK(normal->at(8, 6, 0) == 0);
    CHECK(normal->at(8, 6, 2) == doctest::Approx(1));
    CHECK(normal->at(0, 0, 2) == 0);
    const auto& quality = call.out("quality").asRecord()->data;
    CHECK(quality.at("renderMode") == mode);
    CHECK(quality.at("normalAvailable") == true);
    CHECK(quality.at("outputs").at("depth").at("encoding") == "camera-z-meters");
    CHECK(quality.at("outputs").at("normal").at("encoding") == "camera-unit-vector-xyz");
    const auto* image = call.out("image").asImage();
    if (std::string(mode) == "overlay") {
      CHECK(image->depth == PixelDepth::U16);
      CHECK(image->channels == 4);
      CHECK(image->at(8, 6, 3) == 404);
    } else {
      const auto* selected = call.out(mode).asImage();
      CHECK(cv::norm(cvx::view(*image), cvx::view(*selected), cv::NORM_INF) == 0);
    }
  }
  call.inputs["image"] = nativeImage(cv::Mat(6, 8, CV_8UC3, cv::Scalar(0)), 2);
  REQUIRE(call.run("image.render_model_3d", {{"renderMode", Value::text("depth")}}).ok);
  for (const char* name : {"image", "mask", "edge_mask", "depth", "normal"})
    CHECK(call.out(name).asImage()->scale == 2);
  CHECK(call.out("depth").asImage()->at(4, 3, 0) == doctest::Approx(2));
  CHECK(model.normals[2] == 2);
  call.inputs["image"] = nativeImage(cv::Mat(12, 16, CV_8UC3, cv::Scalar(0)));
  const auto neighbor = poses::make3D("neighbor", "camera", {.6, 0, 2}, {0, 0, 0, 1}, {1, 1, 1});
  call.inputs["instances"] =
      Data::record(poses::make3DList(Json::array({near.data, neighbor.data}), "camera"));
  REQUIRE(call.run("image.render_model_3d", {{"renderMode", Value::text("edge_mask")}}).ok);
  CHECK(call.out("mask").asImage()->at(9, 6, 0) == 255);
  CHECK(call.out("mask").asImage()->at(10, 6, 0) == 255);
  CHECK(call.out("edge_mask").asImage()->at(9, 6, 0) == 255);
  CHECK(call.out("edge_mask").asImage()->at(10, 6, 0) == 255);
}

TEST_CASE("Model renderer rotates normals into camera frame and diagnoses unavailable normals") {
  PointCloud model;
  model.push(0, 0, 0);
  model.normals = {2, 0, 0};
  const double root = std::sqrt(.5);
  const auto pose = poses::make3D("part", "world", {0, 2, 0}, {0, 0, root, root}, {1, 1, 1});
  Transform extrinsic;
  extrinsic.m[5] = extrinsic.m[10] = 0;
  extrinsic.m[6] = -1;
  extrinsic.m[9] = 1;
  test::OpCall call;
  call.inputs = {{"image", nativeImage(cv::Mat(12, 16, CV_8UC3, cv::Scalar(0)))},
                 {"model", Data::cloud(model)},
                 {"instances", Data::record(poses::make3DList(Json::array({pose.data}), "world"))},
                 {"calib", Data::record(camera())},
                 {"sceneToCamera", Data::transform(extrinsic)}};
  REQUIRE(call.run("image.render_model_3d", {{"renderMode", Value::text("normal")}}).ok);
  const auto* normal = call.out("normal").asImage();
  CHECK(normal->at(8, 6, 0) == doctest::Approx(0).epsilon(1e-6));
  CHECK(normal->at(8, 6, 1) == doctest::Approx(0).epsilon(1e-6));
  CHECK(normal->at(8, 6, 2) == doctest::Approx(1));
  CHECK(call.out("depth").asImage()->at(8, 6, 0) == doctest::Approx(2));
  const auto near = poses::make3D("near", "camera", {0, 0, 2}, {0, 1, 0, 0}, {1, 1, 1});
  const auto far = poses::make3D("far", "camera", {0, 0, 4}, {0, 0, 0, 1}, {1, 1, 1});
  call.inputs.erase("sceneToCamera");
  call.inputs["instances"] =
      Data::record(poses::make3DList(Json::array({far.data, near.data}), "camera"));
  REQUIRE(call.run("image.render_model_3d", {{"renderMode", Value::text("normal")}}).ok);
  CHECK(call.out("normal").asImage()->at(8, 6, 0) == doctest::Approx(-1));
  CHECK(call.out("normal").asImage()->at(8, 6, 1) == 0);
  {
    const auto* op = ensureRegistry().find("image.render_model_3d");
    ParamMap params;
    for (const auto& param : op->params)
      params.emplace(param.name, param.def);
    std::unordered_map<std::string, Data> result;
    Outputs output(result);
    CancelContext cancelled;
    CHECK(op->compute(Inputs(call.inputs), ParamView(params, call.base), output, cancelled).code ==
          "cancelled");
    CHECK(result.empty());
  }
  for (const std::vector<float> normals : {std::vector<float>{}, std::vector<float>{0, 0, 0}}) {
    model.normals = normals;
    call.inputs["model"] = Data::cloud(model);
    REQUIRE(call.run("image.render_model_3d").ok);
    CHECK_FALSE(call.outputs.count("normal"));
    CHECK(call.out("quality").asRecord()->data.at("normalAvailable") == false);
    const auto status = call.run("image.render_model_3d", {{"renderMode", Value::text("normal")}});
    CHECK_FALSE(status.ok);
    CHECK(status.portName == "model");
    CHECK(call.outputs.empty());
  }
  CHECK_FALSE(call.run("image.render_model_3d", {{"renderMode", Value::text("invented")}}).ok);
  CHECK(call.outputs.empty());
}

TEST_CASE(
    "P2 model projection rejects nonfinite data resource overflow and cancellation without output") {
  test::OpCall call;
  auto invalid = cloud();
  invalid.xyz[0] = std::numeric_limits<float>::quiet_NaN();
  call.inputs = {{"cloud", Data::cloud(invalid)},
                 {"mask", nativeImage(cv::Mat(12, 16, CV_8U, cv::Scalar(255)))},
                 {"calib", Data::record(camera())}};
  CHECK_FALSE(call.run("cloud.image_roi").ok);
  CHECK(call.outputs.empty());
  PointCloud oversized;
  oversized.xyz.resize(3 * (2 * 1024 * 1024 + 1));
  call.inputs["cloud"] = Data::cloud(std::move(oversized));
  const auto status = call.run("cloud.image_roi");
  CHECK_FALSE(status.ok);
  CHECK(status.code == "resource_limit");
  CHECK(call.outputs.empty());
  call.inputs["cloud"] = Data::cloud(cloud());
  const auto* op = ensureRegistry().find("cloud.image_roi");
  REQUIRE(op != nullptr);
  ParamMap defaults;
  for (const auto& param : op->params)
    defaults.emplace(param.name, param.def);
  std::filesystem::path base;
  CancelContext ctx;
  Inputs in(call.inputs);
  Outputs out(call.outputs);
  const auto cancelled = op->compute(in, ParamView(defaults, base), out, ctx);
  CHECK_FALSE(cancelled.ok);
  CHECK(cancelled.code == "cancelled");
  CHECK(call.outputs.empty());
}
