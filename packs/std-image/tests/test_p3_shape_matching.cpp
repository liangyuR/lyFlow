#include <doctest/doctest.h>

#include <limits>
#include <cstring>
#include <iomanip>
#include <sstream>

#include <opencv2/imgproc.hpp>

#include "helpers.h"
#include "lyflow/pose.h"
#include "lyflow_cv/adapter.h"
#include "lyflow_cv/compat.h"

namespace {
using namespace lyflow;
using Json = nlohmann::json;

Data shapeImage(cv::Mat matrix, int scale = 1) {
  Image image;
  REQUIRE(cvx::fromMat(std::move(matrix), image));
  image.scale = scale;
  return Data::image(std::move(image));
}

cv::Mat shapeTemplate() {
  cv::Mat image(80, 96, CV_8U, cv::Scalar(0));
  const std::vector<cv::Point> polygon{{18, 14}, {70, 14}, {70, 28}, {45, 28}, {45, 62}, {18, 62}};
  cv::fillPoly(image, std::vector<std::vector<cv::Point>>{polygon}, cv::Scalar(230));
  cv::circle(image, {30, 29}, 5, cv::Scalar(0), -1);
  return image;
}

cv::Mat transformedShape(const cv::Mat& source, cv::Size size, cv::Point2d originalCenter,
                         cv::Point2d center, double degrees, double scale) {
  auto matrix = cv::getRotationMatrix2D(originalCenter, -degrees, scale);
  matrix.at<double>(0, 2) += center.x - originalCenter.x;
  matrix.at<double>(1, 2) += center.y - originalCenter.y;
  cv::Mat output;
  cv::warpAffine(source, output, matrix, size, cv::INTER_LINEAR, cv::BORDER_CONSTANT);
  return output;
}

struct ShapeModel {
  Data features, model;
  cv::Point2d center;
};

std::string shapeFeatureHash(const Tensor& tensor) {
  std::uint64_t hash = 14695981039346656037ull;
  for (const auto value : tensor.data) {
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    for (int byte = 0; byte < 4; ++byte) {
      hash ^= (bits >> (byte * 8)) & 255;
      hash *= 1099511628211ull;
    }
  }
  std::ostringstream output;
  output << std::hex << std::setw(16) << std::setfill('0') << hash;
  return output.str();
}

ShapeModel trainShape(const cv::Mat& image = shapeTemplate()) {
  test::OpCall call;
  call.inputs = {{"image", shapeImage(image)}};
  REQUIRE_MESSAGE(call.run("image.shape_template").ok, call.status.message);
  const auto& model = call.outputs.at("model").asRecord()->data;
  return {call.outputs.at("features"),
          call.outputs.at("model"),
          {model["center"][0].get<double>(), model["center"][1].get<double>()}};
}

ParamMap shapeParams() {
  return {{"angleMin", Value::number(-30)}, {"angleMax", Value::number(30)},
          {"angleStep", Value::number(10)}, {"scaleMin", Value::number(.8)},
          {"scaleMax", Value::number(1.2)}, {"scaleStep", Value::number(.2)},
          {"minScore", Value::number(.4)},  {"maxCandidates", Value::integer(16384)}};
}

Status matchShape(test::OpCall& call, const ParamMap& override = {}) {
  auto params = shapeParams();
  for (const auto& item : override)
    params[item.first] = item.second;
  return call.run("image.shape_match", params);
}

const Json& shapeItems(const test::OpCall& call, const char* port = "instances") {
  return call.outputs.at(port).asRecord()->data.at("items");
}

const Json& nearestShape(const Json& items, cv::Point2d point) {
  REQUIRE_FALSE(items.empty());
  const Json* nearest = &items[0];
  double best = std::numeric_limits<double>::infinity();
  for (const auto& item : items) {
    const double distance = std::hypot(item["position"][0].get<double>() - point.x,
                                       item["position"][1].get<double>() - point.y);
    if (distance < best) {
      best = distance;
      nearest = &item;
    }
  }
  CHECK(best < 2);
  return *nearest;
}

class ShapeCancelled : public ExecContext {
public:
  bool cancelled() const override { return true; }
  void progress(float, std::string_view) override {}
  void log(LogLevel, std::string) override {}
  const std::filesystem::path& baseDir() const override {
    static const std::filesystem::path base;
    return base;
  }
  int threadBudget() const override { return 1; }
};
} // namespace

TEST_CASE(
    "P3 shape gradient modeling binds finite original-pixel features and rejects degeneracy") {
  const auto model = trainShape();
  const auto* tensor = model.features.asTensor();
  REQUIRE(tensor != nullptr);
  REQUIRE(tensor->consistent());
  CHECK(tensor->shape[1] == 4);
  CHECK(tensor->shape[0] >= 12);
  CHECK(tensor->shape[0] <= 256);
  for (std::size_t i = 0; i < tensor->data.size(); i += 4)
    CHECK(std::abs(tensor->data[i + 2] * tensor->data[i + 2] +
                   tensor->data[i + 3] * tensor->data[i + 3] - 1) < 1e-5);
  const auto again = trainShape();
  CHECK(again.features.asTensor()->data == tensor->data);
  CHECK(again.model.asRecord()->data == model.model.asRecord()->data);
  test::OpCall call;
  call.inputs = {{"image", shapeImage(cv::Mat(80, 96, CV_8U, cv::Scalar(240)))}};
  CHECK(call.run("image.shape_template").code == "degenerate_template");
  CHECK(call.outputs.empty());
  call.inputs = {{"image", shapeImage(shapeTemplate())},
                 {"mask", shapeImage(cv::Mat(80, 96, CV_8U, cv::Scalar(0)))}};
  CHECK(call.run("image.shape_template").code == "degenerate_template");
  CHECK(call.outputs.empty());
}

TEST_CASE(
    "P3 shape angle scale search recovers two instances with rotated NMS and real residuals") {
  const auto model = trainShape();
  const auto pattern = shapeTemplate();
  auto scene = transformedShape(pattern, {300, 200}, model.center, {80, 90}, 20, 1.2);
  const auto second = transformedShape(pattern, scene.size(), model.center, {220, 110}, -20, .8);
  cv::max(scene, second, scene);
  test::OpCall call;
  call.inputs = {
      {"image", shapeImage(scene)}, {"features", model.features}, {"model", model.model}};
  REQUIRE_MESSAGE(matchShape(call).ok, call.status.message);
  const auto& items = shapeItems(call);
  REQUIRE(items.size() == 2);
  const auto& first = nearestShape(items, {80, 90});
  const auto& other = nearestShape(items, {220, 110});
  CHECK(std::abs(first["angle"].get<double>() * 180 / poses::kPi - 20) < 3);
  CHECK(std::abs(other["angle"].get<double>() * 180 / poses::kPi + 20) < 3);
  CHECK(first["pickable"] == false);
  const auto& quality = shapeItems(call, "quality");
  REQUIRE(quality.size() == 2);
  for (const auto& item : quality) {
    CHECK(item["score"].get<double>() >= .4);
    CHECK(item["coverage"].get<double>() >= .6);
    CHECK(item["rmse"].get<double>() <= 1.5);
    CHECK(item["support"].get<std::size_t>() <= item["featureCount"].get<std::size_t>());
  }
  CHECK(call.outputs.at("image").asImage()->width == 300);
  CHECK(call.outputs.at("edges").asImage()->depth == PixelDepth::U8);
  CHECK(scene.at<std::uint8_t>(0, 0) == 0);
  REQUIRE_MESSAGE(matchShape(call, {{"maxDetections", Value::integer(1)}}).ok, call.status.message);
  CHECK(shapeItems(call).size() == 1);
  CHECK(call.outputs.at("quality").asRecord()->data["limited"] == true);
  call.inputs["image"] =
      shapeImage(transformedShape(pattern, {220, 180}, model.center, {112.5, 91.5}, 17.5, 1.13));
  REQUIRE_MESSAGE(matchShape(call).ok, call.status.message);
  REQUIRE(shapeItems(call).size() == 1);
  const auto& refined = nearestShape(shapeItems(call), {112.5, 91.5});
  CHECK(std::abs(refined["angle"].get<double>() * 180 / poses::kPi - 17.5) < 3);
  CHECK(std::abs(shapeItems(call, "quality")[0]["scale"].get<double>() - 1.13) < .06);
}

TEST_CASE("P3 shape occlusion polarity and distractor gates use contour support instead of NCC") {
  const auto model = trainShape();
  auto scene = transformedShape(shapeTemplate(), {220, 180}, model.center, {108, 90}, 10, 1);
  cv::rectangle(scene, {90, 50}, {112, 76}, cv::Scalar(0), -1);
  test::OpCall call;
  call.inputs = {
      {"image", shapeImage(scene)}, {"features", model.features}, {"model", model.model}};
  REQUIRE_MESSAGE(
      matchShape(call, {{"minCoverage", Value::number(.5)}, {"minScore", Value::number(.3)}}).ok,
      call.status.message);
  REQUIRE(shapeItems(call).size() == 1);
  nearestShape(shapeItems(call), {108, 90});
  auto inverted =
      255 - transformedShape(shapeTemplate(), {220, 180}, model.center, {108, 90}, 10, 1);
  call.inputs["image"] = shapeImage(inverted);
  REQUIRE_MESSAGE(matchShape(call).ok, call.status.message);
  REQUIRE(shapeItems(call).size() == 1);
  nearestShape(shapeItems(call), {108, 90});
  REQUIRE_MESSAGE(matchShape(call, {{"ignorePolarity", Value::boolean(false)}}).ok,
                  call.status.message);
  CHECK(shapeItems(call).empty());
  cv::Mat rectangle(180, 220, CV_8U, cv::Scalar(0));
  cv::rectangle(rectangle, {80, 60}, {135, 120}, cv::Scalar(230), -1);
  call.inputs["image"] = shapeImage(rectangle);
  REQUIRE_MESSAGE(
      matchShape(call, {{"minCoverage", Value::number(.85)}, {"minScore", Value::number(.7)}}).ok,
      call.status.message);
  CHECK(shapeItems(call).empty());
  call.inputs["image"] = shapeImage(cv::Mat(180, 220, CV_8U, cv::Scalar(0)));
  REQUIRE(matchShape(call).ok);
  CHECK(shapeItems(call).empty());
}

TEST_CASE("P3 shape mask region model validation and original image channels remain explicit") {
  const auto model = trainShape();
  const auto gray = transformedShape(shapeTemplate(), {220, 180}, model.center, {108, 90}, 10, 1);
  for (const auto depth : {PixelDepth::U8, PixelDepth::U16, PixelDepth::F32}) {
    for (const int channels : {1, 3, 4}) {
      cv::Mat image;
      gray.convertTo(image,
                     depth == PixelDepth::U8    ? CV_8U
                     : depth == PixelDepth::U16 ? CV_16U
                                                : CV_32F,
                     depth == PixelDepth::U8    ? 1
                     : depth == PixelDepth::U16 ? 257
                                                : 1.0 / 255);
      if (channels != 1)
        cv::cvtColor(image, image, channels == 3 ? cv::COLOR_GRAY2RGB : cv::COLOR_GRAY2RGBA);
      if (channels == 4) {
        cv::Mat alpha(image.size(),
                      depth == PixelDepth::U8    ? CV_8U
                      : depth == PixelDepth::U16 ? CV_16U
                                                 : CV_32F,
                      cv::Scalar(depth == PixelDepth::U8    ? 91
                                 : depth == PixelDepth::U16 ? 23001
                                                            : .37));
        cv::insertChannel(alpha, image, 3);
      }
      const auto original = image.clone();
      test::OpCall call;
      call.inputs = {
          {"image", shapeImage(image)}, {"features", model.features}, {"model", model.model}};
      REQUIRE_MESSAGE(matchShape(call).ok, call.status.message);
      REQUIRE(shapeItems(call).size() == 1);
      const auto* overlay = call.outputs.at("image").asImage();
      CHECK(overlay->channels == channels);
      CHECK(overlay->depth == depth);
      CHECK(cv::norm(image, original, cv::NORM_INF) == 0);
      if (channels == 4) {
        cv::Mat before, after;
        cv::extractChannel(original, before, 3);
        cv::extractChannel(cvx::view(*overlay), after, 3);
        CHECK(cv::norm(before, after, cv::NORM_INF) == 0);
      }
    }
  }
  test::OpCall call;
  call.inputs = {{"image", shapeImage(gray)},
                 {"features", model.features},
                 {"model", model.model},
                 {"mask", shapeImage(cv::Mat(gray.size(), CV_8U, cv::Scalar(0)))}};
  REQUIRE(matchShape(call).ok);
  CHECK(shapeItems(call).empty());
  call.inputs.erase("mask");
  const auto region = poses::make2D("roi", "image", "px", {108, 90}, .2, {100, 100});
  call.inputs["region"] = Data::record(region);
  REQUIRE_MESSAGE(matchShape(call).ok, call.status.message);
  CHECK(shapeItems(call).size() == 1);
  auto wrong = region;
  wrong.data["frame"] = "wrong";
  call.inputs["region"] = Data::record(wrong);
  CHECK(matchShape(call).code == "bad_input");
  CHECK(call.outputs.empty());
  call.inputs.erase("region");
  auto altered = *model.features.asTensor();
  altered.data[0] += 1;
  call.inputs["features"] = Data::tensor(altered);
  CHECK(matchShape(call).code == "bad_input");
  CHECK(call.outputs.empty());
  call.inputs["features"] = model.features;
  auto malformed = *model.model.asRecord();
  malformed.data.erase("center");
  call.inputs["model"] = Data::record(malformed);
  CHECK(matchShape(call).code == "bad_input");
  CHECK(call.outputs.empty());
  cv::Mat faintTemplate, faintScene;
  shapeTemplate().convertTo(faintTemplate, CV_32F, .001 / 255);
  gray.convertTo(faintScene, CV_32F, .001 / 255);
  test::OpCall faint;
  faint.inputs = {{"image", shapeImage(faintTemplate)}};
  REQUIRE_MESSAGE(faint.run("image.shape_template", {{"minContrast", Value::number(.00002)}}).ok,
                  faint.status.message);
  const auto faintFeatures = faint.outputs.at("features"), faintModel = faint.outputs.at("model");
  faint.inputs = {
      {"image", shapeImage(faintScene)}, {"features", faintFeatures}, {"model", faintModel}};
  REQUIRE_MESSAGE(matchShape(faint, {{"minContrast", Value::number(.00002)}}).ok,
                  faint.status.message);
  REQUIRE(shapeItems(faint).size() == 1);
  nearestShape(shapeItems(faint), {108, 90});
  faintScene.at<float>(0, 0) = std::numeric_limits<float>::quiet_NaN();
  faint.inputs["image"] = shapeImage(faintScene);
  CHECK(matchShape(faint).code == "bad_input");
  CHECK(faint.outputs.empty());
}

TEST_CASE("P3 shape preview coordinates cancellation and explicit resource budgets stay atomic") {
  const auto model = trainShape();
  const auto scene = transformedShape(shapeTemplate(), {300, 200}, model.center, {110, 90}, 10, 1);
  const auto full = shapeImage(scene);
  test::OpCall call;
  call.inputs = {{"image", full}, {"features", model.features}, {"model", model.model}};
  REQUIRE_MESSAGE(matchShape(call).ok, call.status.message);
  REQUIRE(shapeItems(call).size() == 1);
  const auto small = shrinkImage(*full.asImage(), 1);
  call.inputs["image"] = Data::image(small);
  REQUIRE_MESSAGE(matchShape(call, {{"smoothSigma", Value::number(.4)},
                                    {"spatialStep", Value::number(1)},
                                    {"maxDistance", Value::number(1)},
                                    {"maxRmse", Value::number(.75)}})
                      .ok,
                  call.status.message);
  REQUIRE(shapeItems(call).size() == 1);
  nearestShape(shapeItems(call), {110, 90});
  CHECK(call.outputs.at("image").asImage()->scale == 2);
  CHECK(call.outputs.at("edges").asImage()->scale == 2);
  CHECK(matchShape(call, {{"angleStep", Value::number(.01)}}).code == "bad_param");
  CHECK(call.outputs.empty());
  call.inputs["image"] = full;
  CHECK(matchShape(call, {{"maxCandidates", Value::integer(1)}}).code == "resource_limit");
  CHECK(call.outputs.empty());
  call.inputs["image"] = Data::image(Image::allocate(4097, 2048, 1, PixelDepth::U8));
  CHECK(matchShape(call).code == "resource_limit");
  CHECK(call.outputs.empty());
  call.inputs["image"] = full;
  const auto* op = ensureRegistry().find("image.shape_match");
  REQUIRE(op != nullptr);
  ParamMap params;
  for (const auto& entry : op->params)
    params[entry.name] = entry.def;
  std::unordered_map<std::string, Data> output;
  Inputs inputs(call.inputs);
  Outputs outputs(output);
  const std::filesystem::path base;
  ParamView view(params, base);
  ShapeCancelled ctx;
  CHECK(op->compute(inputs, view, outputs, ctx).code == "cancelled");
  CHECK(output.empty());
}

TEST_CASE("P3 shape scheduler preserves full pixel coordinates across source preview and ROI") {
  const auto templateImage = shapeTemplate();
  const auto model = trainShape(templateImage);
  const auto scene =
      transformedShape(templateImage, {3000, 1500}, model.center, {1530, 800}, 10, 1);
  const Json parameters{{"angleMin", 0}, {"angleMax", 20}, {"angleStep", 10},
                        {"scaleMin", 1}, {"scaleMax", 1},  {"minScore", .4}};
  const auto graph =
      test::makeGraph({{"source", "io.load_image", {{"path", "injected.png"}}},
                       {"train_source", "io.load_image", {{"path", "train.png"}}},
                       {"roi_mask", "image.roi_mask", {{"roi", {1440, 720, 1620, 880}}}},
                       {"roi_source", "image.region_stats", Json::object()},
                       {"model", "image.shape_template", Json::object()},
                       {"match", "image.shape_match", parameters}},
                      {{"source.image", "match.image"},
                       {"train_source.image", "model.image"},
                       {"source.image", "roi_mask.image"},
                       {"source.image", "roi_source.image"},
                       {"roi_mask.mask", "roi_source.mask"},
                       {"roi_source.bbox", "match.roi"},
                       {"model.features", "match.features"},
                       {"model.model", "match.model"}});
  for (const auto mode : {exec::RunMode::Full, exec::RunMode::Preview}) {
    exec::RunOptions options;
    options.runId = mode == exec::RunMode::Full ? "p3-shape-full" : "p3-shape-preview";
    options.mode = mode;
    options.noReuse = true;
    options.maxParallel = 1;
    options.inputs = {{"source", "image", shapeImage(scene)},
                      {"train_source", "image", shapeImage(templateImage)}};
    test::RunLog log;
    exec::Run run(graph.dump(), options, &test::detail::collect, &log);
    run.join();
    REQUIRE_MESSAGE(log.runStatus() == "ok", Json(log.events).dump());
    Data instances, overlay, source, features;
    REQUIRE(exec::ResultStore::instance().get(options.runId, "match", "instances", instances));
    REQUIRE(exec::ResultStore::instance().get(options.runId, "match", "image", overlay));
    REQUIRE(exec::ResultStore::instance().get(options.runId, "source", "image", source));
    REQUIRE(exec::ResultStore::instance().get(options.runId, "model", "features", features));
    const auto& items = instances.asRecord()->data.at("items");
    REQUIRE(items.size() == 1);
    nearestShape(items, {1530, 800});
    CHECK(features.asTensor()->data == model.features.asTensor()->data);
    const int scale = mode == exec::RunMode::Full ? 1 : 2;
    CHECK(source.asImage()->scale == scale);
    CHECK(overlay.asImage()->scale == scale);
    CHECK(overlay.asImage()->width == 3000 / scale);
    CHECK(overlay.asImage()->height == 1500 / scale);
  }
}

TEST_CASE("P3 shape refinement keeps continuous centers inside ROI mask and region") {
  const auto model = trainShape();
  const auto scene =
      transformedShape(shapeTemplate(), {220, 180}, model.center, {106.75, 90}, 0, 1);
  const ParamMap parameters{{"angleMin", Value::number(0)},   {"angleMax", Value::number(0)},
                            {"scaleMin", Value::number(1)},   {"scaleMax", Value::number(1)},
                            {"minScore", Value::number(.15)}, {"minCoverage", Value::number(.3)}};
  for (const auto kind : {0, 1, 2}) {
    CAPTURE(kind);
    test::OpCall call;
    call.inputs = {
        {"image", shapeImage(scene)}, {"features", model.features}, {"model", model.model}};
    if (kind == 0) {
      Box2D box;
      box.min[0] = box.min[1] = 0;
      box.max[0] = 106;
      box.max[1] = 179;
      box.unit = Unit2D::Pixel;
      call.inputs["roi"] = Data::box2d(box);
    } else if (kind == 1) {
      cv::Mat mask(scene.size(), CV_8U, cv::Scalar(0));
      mask.colRange(0, 107).setTo(255);
      call.inputs["mask"] = shapeImage(mask);
    } else {
      call.inputs["region"] =
          Data::record(poses::make2D("search", "image", "px", {53, 89.5}, 0, {106, 179}));
    }
    REQUIRE_MESSAGE(matchShape(call, parameters).ok, call.status.message);
    REQUIRE_FALSE(shapeItems(call).empty());
    for (const auto& item : shapeItems(call)) {
      CHECK(item["position"][0].get<double>() <= 106);
      CHECK(item["position"][0].get<double>() >= 0);
      CHECK(item["position"][1].get<double>() >= 0);
      CHECK(item["position"][1].get<double>() <= 179);
    }
  }
}

TEST_CASE("P3 shape model geometry is validated independently of its matching content hash") {
  const auto trained = trainShape();
  cv::Mat scene(64, 64, CV_8U, cv::Scalar(0));
  cv::rectangle(scene, {20, 10, 20, 40}, cv::Scalar(255), -1);
  for (const auto kind : {0, 1, 2, 3, 4}) {
    CAPTURE(kind);
    Tensor tensor = *trained.features.asTensor();
    Record model = *trained.model.asRecord();
    if (kind <= 2) {
      tensor.shape = {12, 4};
      tensor.data.clear();
      for (int i = 0; i < 12; ++i) {
        const float coordinate = kind == 0 ? 0.f : kind == 1 ? i * .01f : (i - 5.5f) * 2;
        tensor.data.insert(tensor.data.end(), {coordinate, kind == 2 ? coordinate : 0.f, 1, 0});
      }
    } else if (kind == 3) {
      model.data["size"][0] = model.data["size"][0].get<double>() + 4;
    } else {
      for (std::size_t i = 0; i < tensor.data.size(); i += 4)
        tensor.data[i] += .5f;
    }
    model.data["featureCount"] = tensor.shape[0];
    model.data["featuresHash"] = shapeFeatureHash(tensor);
    test::OpCall call;
    call.inputs = {{"image", shapeImage(scene)},
                   {"features", Data::tensor(tensor)},
                   {"model", Data::record(model)}};
    CHECK(matchShape(call, {{"angleMin", Value::number(0)},
                            {"angleMax", Value::number(0)},
                            {"scaleMin", Value::number(1)},
                            {"scaleMax", Value::number(1)}})
              .code == "bad_input");
    CHECK(call.outputs.empty());
  }
}
