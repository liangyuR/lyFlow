#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <limits>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "helpers.h"
#include "lyflow/pose.h"
#include "lyflow_cv/adapter.h"

namespace {
using namespace lyflow;
using Json = nlohmann::json;

struct HighlightTruth {
  double x;
  double y;
  double angle;
  double length;
};

cv::Mat highlightsScene(int width, int height, const std::vector<HighlightTruth>& lines,
                        double peak = 0.95, double background = 0.05) {
  cv::Mat result(height, width, CV_32F, cv::Scalar(background));
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x) {
      double value = background;
      for (const auto& line : lines) {
        const double co = std::cos(line.angle), si = std::sin(line.angle);
        const double longitudinal = (x - line.x) * co + (y - line.y) * si;
        const double perpendicular = -(x - line.x) * si + (y - line.y) * co;
        if (std::abs(longitudinal) <= line.length / 2)
          value = std::max(value, background + (peak - background) *
                                                   std::exp(-perpendicular * perpendicular / 2.42));
      }
      result.at<float>(y, x) = static_cast<float>(value);
    }
  return result;
}

cv::Mat narrowParallelHighlights(const std::vector<int>& sides) {
  cv::Mat result(256, 256, CV_8U, cv::Scalar(0));
  const double angle = 3 * poses::kPi / 180;
  const double co = std::cos(angle), si = std::sin(angle);
  for (int y = 0; y < result.rows; ++y)
    for (int x = 0; x < result.cols; ++x) {
      const double along = (x - 128) * co + (y - 128) * si;
      const double normal = -(x - 128) * si + (y - 128) * co;
      double value = 0;
      if (std::abs(along) < 90)
        for (const int side : sides) {
          const double distance = (normal - side) / 0.38;
          value = std::max(value, std::exp(-distance * distance / 2));
        }
      result.at<std::uint8_t>(y, x) = cv::saturate_cast<std::uint8_t>(255 * value);
    }
  return result;
}

Data highlightImage(const cv::Mat& matrix) {
  Image image;
  REQUIRE(cvx::fromMat(matrix, image));
  return Data::image(std::move(image));
}

ParamMap highlightParams() {
  return {{"minBrightness", Value::number(0.35)}, {"minContrast", Value::number(0.1)},
          {"ridgeRadius", Value::number(6)},      {"smoothSigma", Value::number(0.7)},
          {"minLength", Value::number(60)},       {"minSupport", Value::integer(12)},
          {"minCoverage", Value::number(0.65)},   {"maxResidual", Value::number(0.8)},
          {"maxGap", Value::number(10)},          {"boxHeight", Value::number(8)}};
}

Status runHighlights(test::OpCall& call, const ParamMap& extra = {}) {
  auto params = highlightParams();
  for (const auto& item : extra)
    params[item.first] = item.second;
  return call.run("image.highlights", params);
}

const Json& highlightItems(const test::OpCall& call, const char* port = "segments") {
  const auto* record = call.outputs.at(port).asRecord();
  REQUIRE(record != nullptr);
  return record->data.at("items");
}

double lineAngleDistance(double a, double b) { return std::abs(std::remainder(a - b, poses::kPi)); }

const Json& closestHighlight(const test::OpCall& call, const HighlightTruth& truth) {
  const auto& instances = highlightItems(call, "instances");
  REQUIRE_FALSE(instances.empty());
  const Json* best = nullptr;
  double distance = std::numeric_limits<double>::infinity();
  for (const auto& item : instances) {
    const double candidate = std::hypot(item["position"][0].get<double>() - truth.x,
                                        item["position"][1].get<double>() - truth.y);
    if (candidate < distance) {
      best = &item;
      distance = candidate;
    }
  }
  REQUIRE(best != nullptr);
  CHECK(distance < 2.5);
  CHECK(lineAngleDistance((*best)["angle"].get<double>(), truth.angle) < 2 * poses::kPi / 180);
  CHECK(std::abs((*best)["size"][0].get<double>() - truth.length) <= 8);
  CHECK(std::abs((*best)["size"][1].get<double>() - 8) <= 1e-6);
  return *best;
}

void checkHighlightContracts(const test::OpCall& call) {
  const auto* instances = call.outputs.at("instances").asRecord();
  const auto* segments = call.outputs.at("segments").asRecord();
  REQUIRE(instances != nullptr);
  REQUIRE(segments != nullptr);
  std::string why;
  CHECK_MESSAGE(poses::validate(*instances, why), why);
  CHECK(segments->type == "image.Highlights.v1");
  CHECK(segments->data.at("frame") == "image");
  CHECK(segments->data.at("unit") == "px");
  CHECK(highlightItems(call).size() == highlightItems(call, "instances").size());
  CHECK(highlightItems(call).size() <= 256);
  for (std::size_t i = 0; i < highlightItems(call).size(); ++i) {
    const auto& item = highlightItems(call)[i];
    const auto& pose = highlightItems(call, "instances")[i];
    CHECK(item.at("id") == pose.at("id"));
    CHECK(pose.at("pickable") == false);
    const auto& points = item.at("endpoints");
    REQUIRE(points.size() == 2);
    const double x0 = points[0][0], y0 = points[0][1];
    const double x1 = points[1][0], y1 = points[1][1];
    const double length = std::hypot(x1 - x0, y1 - y0);
    CHECK(item.at("length").get<double>() == doctest::Approx(length).epsilon(1e-8));
    CHECK(std::abs(pose["position"][0].get<double>() - (x0 + x1) / 2) <= 1e-6);
    CHECK(std::abs(pose["position"][1].get<double>() - (y0 + y1) / 2) <= 1e-6);
    CHECK(pose["size"][0].get<double>() == doctest::Approx(length).epsilon(1e-8));
    CHECK(item.at("rms").get<double>() <= 0.8 + 1e-8);
    CHECK(item.at("maxError").get<double>() <= 0.8 + 1e-8);
    CHECK(item.at("coverage").get<double>() >= 0.65 - 1e-8);
    CHECK(item.at("coverage").get<double>() <= 1 + 1e-8);
    CHECK(item.at("support").get<int>() >= 12);
    CHECK(item.at("meanContrast").get<double>() >= 0.1 - 1e-6);
    CHECK(std::isfinite(item.at("score").get<double>()));
    CHECK(lineAngleDistance(item.at("angle").get<double>(), pose.at("angle").get<double>()) < 1e-8);
  }
}

class HighlightCancel final : public ExecContext {
public:
  explicit HighlightCancel(int after) : after_(after) {}
  mutable int checks = 0;
  bool cancelled() const override { return ++checks >= after_; }
  void progress(float, std::string_view) override {}
  void log(LogLevel, std::string) override {}
  const std::filesystem::path& baseDir() const override { return base_; }
  int threadBudget() const override { return 1; }

private:
  int after_;
  std::filesystem::path base_;
};

struct HighlightFile {
  std::filesystem::path path;
  HighlightFile() {
    static std::atomic<int> sequence{0};
    path =
        std::filesystem::temp_directory_path() /
        std::filesystem::u8path(
            "lyflow-高光-" + std::to_string(++sequence) + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".png");
  }
  ~HighlightFile() {
    std::error_code error;
    std::filesystem::remove(path, error);
  }
  void write(const cv::Mat& matrix) {
    std::vector<std::uint8_t> bytes;
    REQUIRE(cv::imencode(".png", matrix, bytes));
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    REQUIRE(file.good());
  }
};
}

TEST_CASE("P2 highlights finds horizontal vertical and slanted finite ridges with typed geometry") {
  for (const double angle : {0., poses::kPi / 2, poses::kPi / 6, -poses::kPi / 5}) {
    CAPTURE(angle);
    const HighlightTruth truth{128, 128, angle, 180};
    test::OpCall call;
    call.inputs = {{"image", highlightImage(highlightsScene(256, 256, {truth}))}};
    REQUIRE_MESSAGE(runHighlights(call).ok, call.status.message);
    REQUIRE(highlightItems(call).size() == 1);
    closestHighlight(call, truth);
    checkHighlightContracts(call);
    const auto* mask = call.outputs.at("mask").asImage();
    REQUIRE(mask != nullptr);
    CHECK(mask->width == 256);
    CHECK(mask->height == 256);
    CHECK(mask->channels == 1);
    CHECK(mask->depth == PixelDepth::U8);
  }
  test::OpCall small;
  cv::Mat tiny(64, 192, CV_8U, cv::Scalar(0));
  cv::line(tiny, {20, 31}, {22, 31}, cv::Scalar(255));
  small.inputs = {{"image", highlightImage(tiny)}};
  REQUIRE(runHighlights(small, {{"ridgeRadius", Value::number(1)},
                                {"smoothSigma", Value::number(0)},
                                {"minLength", Value::number(2)},
                                {"minSupport", Value::integer(2)},
                                {"maxGap", Value::number(0)},
                                {"maxResidual", Value::number(1)},
                                {"minCoverage", Value::number(0.5)}})
              .ok);
  REQUIRE(highlightItems(small).size() == 1);
  CHECK(highlightItems(small)[0].at("length").get<double>() == doctest::Approx(2));
  CHECK(highlightItems(small)[0].at("support") == 3);
}

TEST_CASE("P2 highlights keeps nearby parallel instances distinct and groups only bounded gaps") {
  test::OpCall call;
  cv::Mat closeLines(64, 192, CV_8U, cv::Scalar(0));
  cv::line(closeLines, {20, 31}, {170, 31}, cv::Scalar(255));
  cv::line(closeLines, {20, 33}, {170, 33}, cv::Scalar(255));
  call.inputs = {{"image", highlightImage(closeLines)}};
  REQUIRE(runHighlights(call, {{"ridgeRadius", Value::number(1)},
                               {"smoothSigma", Value::number(0)},
                               {"maxResidual", Value::number(1)}})
              .ok);
  REQUIRE(highlightItems(call).size() == 2);
  CHECK(highlightItems(call, "instances")[0].at("position")[1] == 31);
  CHECK(highlightItems(call, "instances")[1].at("position")[1] == 33);
  CHECK(highlightItems(call)[0].at("maxError") == 0);
  CHECK(highlightItems(call)[1].at("maxError") == 0);
  const ParamMap narrowParams{
      {"smoothSigma", Value::number(0)},     {"ridgeRadius", Value::number(1)},
      {"minBrightness", Value::number(0.3)}, {"minContrast", Value::number(0.1)},
      {"minLength", Value::number(100)},     {"maxGap", Value::number(4)},
      {"maxResidual", Value::number(1)},     {"boxHeight", Value::number(1)},
      {"minSupport", Value::integer(12)},    {"minCoverage", Value::number(0.65)},
      {"referenceAngle", Value::number(3)},  {"angleTolerance", Value::number(5)}};
  const double narrowAngle = 3 * poses::kPi / 180;
  for (const auto& sides : {std::vector<int>{-1}, std::vector<int>{1}, std::vector<int>{-1, 1}}) {
    CAPTURE(sides.size());
    call.inputs = {{"image", highlightImage(narrowParallelHighlights(sides))}};
    REQUIRE_MESSAGE(runHighlights(call, narrowParams).ok, call.status.message);
    REQUIRE(highlightItems(call).size() == sides.size());
    const auto& instances = highlightItems(call, "instances");
    REQUIRE(instances.size() == sides.size());
    for (const int side : sides) {
      const Json* matched = nullptr;
      double closestNormal = std::numeric_limits<double>::infinity();
      for (std::size_t i = 0; i < instances.size(); ++i) {
        const double normal =
            -(instances[i]["position"][0].get<double>() - 128) * std::sin(narrowAngle) +
            (instances[i]["position"][1].get<double>() - 128) * std::cos(narrowAngle);
        if (std::abs(normal - side) < closestNormal) {
          matched = &highlightItems(call)[i];
          closestNormal = std::abs(normal - side);
        }
      }
      REQUIRE(matched != nullptr);
      CHECK(closestNormal <= 0.3);
      CHECK(std::abs(matched->at("length").get<double>() - 180) <= 4);
      CHECK(lineAngleDistance(matched->at("angle").get<double>(), narrowAngle) <=
            0.1 * poses::kPi / 180);
      CHECK(matched->at("rms").get<double>() <= 1);
      CHECK(matched->at("maxError").get<double>() <= 1);
      CHECK(matched->at("coverage").get<double>() >= 0.65);
      CHECK(matched->at("support").get<int>() >= 12);
    }
  }
  const std::vector<HighlightTruth> truth{{128, 92, 0, 180}, {128, 104, 0, 180}};
  call.inputs = {{"image", highlightImage(highlightsScene(256, 192, truth))}};
  REQUIRE_MESSAGE(runHighlights(call).ok, call.status.message);
  REQUIRE(highlightItems(call).size() == 2);
  for (const auto& line : truth)
    closestHighlight(call, line);
  const Json first = highlightItems(call);
  REQUIRE(runHighlights(call).ok);
  CHECK(highlightItems(call) == first);
  for (const int gap : {8, 28}) {
    auto image = highlightsScene(256, 192, {{128, 96, 0, 200}});
    image.colRange(128 - gap / 2, 128 + gap / 2).setTo(0.05);
    call.inputs = {{"image", highlightImage(image)}};
    REQUIRE_MESSAGE(runHighlights(call, {{"maxGap", Value::number(14)}}).ok, call.status.message);
    if (gap == 8) {
      REQUIRE(highlightItems(call).size() == 1);
      CHECK(highlightItems(call)[0]["length"].get<double>() > 190);
    } else {
      REQUIRE(highlightItems(call).size() == 2);
      for (const auto& line : highlightItems(call))
        CHECK(line["length"].get<double>() < 110);
    }
  }
}

TEST_CASE(
    "P2 highlights rejects flat low contrast noisy short and curved evidence without fake success") {
  std::vector<cv::Mat> negatives;
  negatives.emplace_back(192, 256, CV_32F, cv::Scalar(0.95));
  negatives.push_back(highlightsScene(256, 192, {{128, 96, 0, 180}}, 0.12, 0.1));
  negatives.push_back(highlightsScene(256, 192, {{128, 96, 0, 30}}));
  cv::Mat noise(192, 256, CV_32F);
  cv::RNG random(20261114);
  random.fill(noise, cv::RNG::UNIFORM, 0.05, 0.12);
  negatives.push_back(noise);
  cv::Mat curve(192, 256, CV_32F, cv::Scalar(0.05));
  cv::circle(curve, {128, 96}, 48, cv::Scalar(0.95), 3);
  negatives.push_back(curve);
  cv::Mat step(192, 256, CV_32F, cv::Scalar(0.05));
  step.rowRange(96, 192).setTo(0.95);
  negatives.push_back(step);
  for (std::size_t i = 0; i < negatives.size(); ++i) {
    CAPTURE(i);
    test::OpCall call;
    call.inputs = {{"image", highlightImage(negatives[i])}};
    REQUIRE_MESSAGE(runHighlights(call).ok, call.status.message);
    CHECK(highlightItems(call).empty());
    CHECK(highlightItems(call, "instances").empty());
  }
}

TEST_CASE(
    "P2 highlights clips rectangular and rotated ROI without accepting excluded interference") {
  test::OpCall call;
  call.inputs = {{"image", highlightImage(highlightsScene(
                               256, 192, {{128, 64, 0, 180}, {128, 128, 0, 180}}))}};
  Box2D roi{{0, 40}, {256, 88}, Unit2D::Pixel};
  call.inputs["roi"] = Data::box2d(roi);
  REQUIRE_MESSAGE(runHighlights(call).ok, call.status.message);
  REQUIRE(highlightItems(call).size() == 1);
  closestHighlight(call, {128, 64, 0, 180});
  for (const auto& point : highlightItems(call)[0]["endpoints"])
    CHECK(point[1].get<double>() < 88);
  call.inputs.erase("roi");
  const HighlightTruth angled{128, 96, poses::kPi / 6, 160};
  call.inputs["image"] = highlightImage(highlightsScene(256, 192, {angled, {128, 24, 0, 220}}));
  call.inputs["region"] =
      Data::record(poses::make2D("region", "image", "px", {128, 96}, angled.angle, {200, 28}));
  REQUIRE_MESSAGE(runHighlights(call).ok, call.status.message);
  REQUIRE(highlightItems(call).size() == 1);
  closestHighlight(call, angled);
  const auto* mask = call.outputs.at("mask").asImage();
  REQUIRE(mask != nullptr);
  for (int x = 0; x < mask->width; ++x)
    CHECK(mask->at(x, 24, 0) == 0);
  call.inputs.erase("region");
  call.inputs["image"] = highlightImage(highlightsScene(256, 192, {{128, 2, 0, 180}}));
  call.inputs["roi"] = Data::box2d(Box2D{{-5, -5}, {261, 192}, Unit2D::Pixel});
  REQUIRE_MESSAGE(runHighlights(call).ok, call.status.message);
  CHECK(highlightItems(call).size() <= 1);
}

TEST_CASE("P2 highlights direction tolerance is undirected and uses angle boundaries") {
  test::OpCall call;
  const HighlightTruth truth{128, 128, poses::kPi / 6, 180};
  call.inputs = {{"image", highlightImage(highlightsScene(256, 256, {truth}))}};
  REQUIRE(runHighlights(
              call, {{"referenceAngle", Value::number(30)}, {"angleTolerance", Value::number(2)}})
              .ok);
  REQUIRE(highlightItems(call).size() == 1);
  REQUIRE(runHighlights(
              call, {{"referenceAngle", Value::number(210)}, {"angleTolerance", Value::number(2)}})
              .ok);
  REQUIRE(highlightItems(call).size() == 1);
  REQUIRE(runHighlights(
              call, {{"referenceAngle", Value::number(0)}, {"angleTolerance", Value::number(10)}})
              .ok);
  CHECK(highlightItems(call).empty());
  call.inputs["image"] = highlightImage(highlightsScene(256, 256, {{128, 128, 0, 180}}));
  REQUIRE(runHighlights(
              call, {{"referenceAngle", Value::number(0)}, {"angleTolerance", Value::number(0)}})
              .ok);
  CHECK(highlightItems(call).size() == 1);
  const HighlightTruth threeDegrees{112, 112, 3 * poses::kPi / 180, 160};
  cv::Mat quantized(224, 224, CV_8U);
  const double co = std::cos(threeDegrees.angle), si = std::sin(threeDegrees.angle);
  for (int y = 0; y < quantized.rows; ++y)
    for (int x = 0; x < quantized.cols; ++x) {
      const double along = (x - 112) * co + (y - 112) * si;
      const double normal = -(x - 112) * si + (y - 112) * co;
      const double value =
          0.05 + (std::abs(along) <= 80 ? 0.9 * std::exp(-normal * normal / 2.42) : 0);
      quantized.at<std::uint8_t>(y, x) = cv::saturate_cast<std::uint8_t>(255 * value);
    }
  call.inputs = {{"image", highlightImage(quantized)}};
  REQUIRE(runHighlights(call, {{"minLength", Value::number(104)},
                               {"referenceAngle", Value::number(3)},
                               {"angleTolerance", Value::number(90)}})
              .ok);
  REQUIRE(highlightItems(call).size() == 1);
  CHECK(lineAngleDistance(highlightItems(call)[0]["angle"].get<double>(), threeDegrees.angle) <
        0.1 * poses::kPi / 180);
  REQUIRE_MESSAGE(runHighlights(call, {{"minLength", Value::number(104)},
                                       {"referenceAngle", Value::number(3)},
                                       {"angleTolerance", Value::number(0.1)}})
                      .ok,
                  call.status.message);
  REQUIRE(highlightItems(call).size() == 1);
  closestHighlight(call, threeDegrees);
  checkHighlightContracts(call);
  CHECK(lineAngleDistance(highlightItems(call)[0]["angle"].get<double>(), threeDegrees.angle) <
        0.1 * poses::kPi / 180);
}

TEST_CASE(
    "P2 highlights normalized brightness threshold includes equality without hidden tolerance") {
  test::OpCall call;
  call.inputs = {{"image", highlightImage(highlightsScene(256, 192, {{128, 96, 0, 180}}, 0.75))}};
  REQUIRE_MESSAGE(runHighlights(call, {{"smoothSigma", Value::number(0)},
                                       {"minBrightness", Value::number(0.75)}})
                      .ok,
                  call.status.message);
  CHECK(highlightItems(call).size() == 1);
  REQUIRE(runHighlights(call, {{"smoothSigma", Value::number(0)},
                               {"minBrightness", Value::number(std::nextafter(0.75, 1.0))}})
              .ok);
  CHECK(highlightItems(call).empty());
}

TEST_CASE("P2 highlights preserves source pixels depth channels alpha and RGB interpretation") {
  const cv::Mat gray = highlightsScene(256, 192, {{128, 96, 0, 180}});
  for (const int depth : {CV_8U, CV_16U, CV_32F})
    for (const int channels : {1, 3, 4}) {
      CAPTURE(depth);
      CAPTURE(channels);
      cv::Mat scalar, pixels;
      gray.convertTo(scalar, depth, depth == CV_8U ? 255 : depth == CV_16U ? 65535 : 1);
      if (channels == 1)
        pixels = scalar;
      else {
        std::vector<cv::Mat> planes(channels, scalar);
        if (channels == 4)
          planes[3] =
              cv::Mat(scalar.size(), scalar.type(), cv::Scalar(depth == CV_32F ? 0.37 : 37));
        cv::merge(planes, pixels);
      }
      const cv::Mat original = pixels.clone();
      test::OpCall call;
      call.inputs = {{"image", highlightImage(pixels)}};
      REQUIRE_MESSAGE(runHighlights(call).ok, call.status.message);
      REQUIRE(highlightItems(call).size() == 1);
      checkHighlightContracts(call);
      const auto* output = call.outputs.at("image").asImage();
      REQUIRE(output != nullptr);
      CHECK(output->channels == channels);
      CHECK(output->depth == (depth == CV_8U    ? PixelDepth::U8
                              : depth == CV_16U ? PixelDepth::U16
                                                : PixelDepth::F32));
      CHECK(cv::norm(original, pixels, cv::NORM_INF) == 0);
      if (channels == 4) {
        std::vector<cv::Mat> before, after;
        cv::split(original, before);
        cv::split(cvx::view(*output), after);
        CHECK(cv::norm(before[3], after[3], cv::NORM_INF) == 0);
      }
    }
  test::OpCall call;
  for (const bool red : {true, false}) {
    std::vector<cv::Mat> planes(3, cv::Mat::zeros(gray.size(), CV_32F));
    planes[red ? 0 : 2] = gray;
    cv::Mat rgb;
    cv::merge(planes, rgb);
    call.inputs = {{"image", highlightImage(rgb)}};
    REQUIRE(runHighlights(
                call, {{"minBrightness", Value::number(0.2)}, {"minContrast", Value::number(0.08)}})
                .ok);
    CHECK(highlightItems(call).size() == (red ? 1 : 0));
  }
}

TEST_CASE("P2 highlights validates parameters ROI types frames and finite normalized pixels") {
  test::OpCall call;
  const auto source = highlightImage(highlightsScene(256, 192, {{128, 96, 0, 180}}));
  call.inputs = {{"image", source}};
  const std::vector<ParamMap> invalid{
      {{"minBrightness", Value::number(-0.01)}}, {{"minContrast", Value::number(0)}},
      {{"ridgeRadius", Value::number(0.5)}},     {{"smoothSigma", Value::number(-1)}},
      {{"minLength", Value::number(1)}},         {{"maxGap", Value::number(-1)}},
      {{"maxResidual", Value::number(0.01)}},    {{"boxHeight", Value::number(0)}},
      {{"referenceAngle", Value::number(361)}},  {{"angleTolerance", Value::number(91)}},
      {{"minSupport", Value::integer(1)}},       {{"minCoverage", Value::number(1.1)}},
      {{"mergeDistance", Value::number(-1)}},    {{"mergeAngle", Value::number(91)}}};
  for (const auto& params : invalid) {
    CHECK_FALSE(runHighlights(call, params).ok);
    CHECK(call.outputs.empty());
  }
  for (const auto& roi :
       {Box2D{{0, 0}, {256, 192}, Unit2D::Meter}, Box2D{{300, 0}, {400, 100}, Unit2D::Pixel},
        Box2D{{10, 10}, {10, 10}, Unit2D::Pixel}}) {
    call.inputs["roi"] = Data::box2d(roi);
    CHECK_FALSE(runHighlights(call).ok);
    CHECK(call.outputs.empty());
  }
  call.inputs.erase("roi");
  for (const auto& pose : {poses::make2D("r", "wrong", "px", {128, 96}, 0, {200, 20}),
                           poses::make2D("r", "image", "m", {128, 96}, 0, {200, 20})}) {
    call.inputs["region"] = Data::record(pose);
    CHECK_FALSE(runHighlights(call).ok);
  }
  call.inputs["region"] = Data::record(poses::make2D("r", "image", "px", {128, 96}, 0, {200, 20}));
  call.inputs["roi"] = Data::box2d(Box2D{{0, 0}, {256, 192}, Unit2D::Pixel});
  CHECK_FALSE(runHighlights(call).ok);
  call.inputs.erase("roi");
  call.inputs.erase("region");
  for (const double value : {std::numeric_limits<double>::quiet_NaN(),
                             std::numeric_limits<double>::infinity(), -0.01, 1.01}) {
    cv::Mat bad(24, 32, CV_32F, cv::Scalar(value));
    call.inputs["image"] = highlightImage(bad);
    CHECK_FALSE(runHighlights(call).ok);
    CHECK(call.outputs.empty());
  }
}

TEST_CASE("P2 highlights enforces image and support budgets and honours cancellation atomically") {
  test::OpCall call;
  call.inputs = {{"image", highlightImage(cv::Mat(2048, 4097, CV_8U, cv::Scalar(0)))}};
  CHECK_FALSE(runHighlights(call).ok);
  CHECK(call.outputs.empty());
  cv::Mat stripes(1024, 1024, CV_8U, cv::Scalar(0));
  for (int row = 1; row < stripes.rows; row += 3)
    stripes.row(row).setTo(255);
  call.inputs = {{"image", highlightImage(stripes)}};
  const auto budget = runHighlights(call, {{"ridgeRadius", Value::number(1)},
                                           {"smoothSigma", Value::number(0)},
                                           {"minBrightness", Value::number(0.5)},
                                           {"minContrast", Value::number(0.5)}});
  CHECK_FALSE(budget.ok);
  CHECK(budget.code == "resource_limit");
  CHECK(call.outputs.empty());
  cv::Mat panorama(5, 1000000, CV_8U, cv::Scalar(0));
  cv::line(panorama, {50, 2}, {150, 2}, cv::Scalar(255));
  call.inputs = {{"image", highlightImage(panorama)}};
  const auto houghBudget = runHighlights(call, {{"ridgeRadius", Value::number(1)},
                                                {"smoothSigma", Value::number(0)},
                                                {"minLength", Value::number(30)}});
  CHECK_FALSE(houghBudget.ok);
  CHECK(houghBudget.code == "resource_limit");
  CHECK(call.outputs.empty());
  const auto* descriptor = ensureRegistry().find("image.highlights");
  REQUIRE(descriptor != nullptr);
  ParamMap params;
  for (const auto& param : descriptor->params)
    params[param.name] = param.def;
  for (const auto& param : highlightParams())
    params[param.first] = param.second;
  const std::unordered_map<std::string, Data> inputs{
      {"image", highlightImage(highlightsScene(1024, 1024, {{512, 512, 0, 800}}))}};
  for (const int after : {1, 16}) {
    HighlightCancel context(after);
    const ParamView view(params, context.baseDir());
    std::unordered_map<std::string, Data> result;
    Outputs outputs(result);
    const auto status = descriptor->compute(Inputs(inputs), view, outputs, context);
    CHECK((status.ok || status.code == "cancelled"));
    CHECK(context.checks >= after);
    CHECK(result.empty());
  }
}

TEST_CASE("P2 highlights full and real scheduler preview retain original image geometry") {
  const HighlightTruth truth{2048, 1024, poses::kPi / 12, 1800};
  cv::Mat floating = highlightsScene(4096, 2048, {truth});
  cv::Mat pixels;
  floating.convertTo(pixels, CV_8U, 255);
  HighlightFile file;
  file.write(pixels);
  Json params{{"minBrightness", 0.3}, {"minContrast", 0.04}, {"ridgeRadius", 8}, {"smoothSigma", 1},
              {"minLength", 800},     {"maxGap", 16},        {"maxResidual", 2}, {"boxHeight", 12},
              {"minCoverage", 0.7},   {"minSupport", 40}};
  const auto graph = test::makeGraph(
      {{"source", "io.load_image", {{"path", file.path.u8string()}, {"mode", "gray"}}},
       {"highlights", "image.highlights", params}},
      {{"source.image", "highlights.image"}});
  for (const auto mode : {exec::RunMode::Full, exec::RunMode::Preview}) {
    exec::ResultStore::instance().clear();
    exec::RunOptions options;
    options.runId = mode == exec::RunMode::Full ? "p2-highlights-full" : "p2-highlights-preview";
    options.mode = mode;
    options.noReuse = true;
    options.maxParallel = 1;
    test::RunLog log;
    log.runId = options.runId;
    exec::Run run(graph.dump(), options, &test::detail::collect, &log);
    run.join();
    REQUIRE_MESSAGE(log.runStatus() == "ok", Json(log.events).dump());
    Data source, instances, segments, mask, annotated;
    REQUIRE(exec::ResultStore::instance().get(log.runId, "source", "image", source));
    REQUIRE(source.asImage() != nullptr);
    const int scale = mode == exec::RunMode::Preview ? 2 : 1;
    CHECK(source.asImage()->scale == scale);
    REQUIRE(exec::ResultStore::instance().get(log.runId, "highlights", "instances", instances));
    REQUIRE(exec::ResultStore::instance().get(log.runId, "highlights", "segments", segments));
    REQUIRE(instances.asRecord() != nullptr);
    REQUIRE(segments.asRecord() != nullptr);
    const auto& items = instances.asRecord()->data.at("items");
    REQUIRE(items.size() == 1);
    const auto& pose = items[0];
    CHECK(std::abs(pose["position"][0].get<double>() - truth.x) <= 4);
    CHECK(std::abs(pose["position"][1].get<double>() - truth.y) <= 4);
    CHECK(std::abs(pose["size"][0].get<double>() - truth.length) <= 12);
    CHECK(std::abs(pose["size"][1].get<double>() - 12) <= 1e-6);
    CHECK(lineAngleDistance(pose["angle"].get<double>(), truth.angle) < poses::kPi / 180);
    CHECK(std::abs(segments.asRecord()->data["items"][0]["length"].get<double>() - truth.length) <=
          12);
    REQUIRE(exec::ResultStore::instance().get(log.runId, "highlights", "mask", mask));
    REQUIRE(exec::ResultStore::instance().get(log.runId, "highlights", "image", annotated));
    REQUIRE(mask.asImage() != nullptr);
    REQUIRE(annotated.asImage() != nullptr);
    CHECK(mask.asImage()->scale == scale);
    CHECK(annotated.asImage()->scale == scale);
    CHECK(mask.asImage()->width == 4096 / scale);
    CHECK(annotated.asImage()->height == 2048 / scale);
  }
}
