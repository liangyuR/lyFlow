#include <doctest/doctest.h>

#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>

#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>

#include "helpers.h"
#include "lyflow/value.h"
#include "lyflow_cv/adapter.h"

namespace {
using namespace lyflow;
using Json = nlohmann::json;
Data ocrImage(cv::Mat matrix, int scale = 1) {
  Image image;
  REQUIRE(cvx::fromMat(std::move(matrix), image));
  image.scale = scale;
  return Data::image(std::move(image));
}
std::string quadHash(const Tensor& tensor) {
  std::uint64_t hash = 14695981039346656037ull;
  for (float value : tensor.data) {
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    for (int byte = 0; byte < 4; ++byte) {
      hash ^= (bits >> (byte * 8)) & 255;
      hash *= 1099511628211ull;
    }
  }
  std::ostringstream out;
  out << std::hex << std::setw(16) << std::setfill('0') << hash;
  return out.str();
}
struct CropsFixture {
  Data image, quads, regions, rec, cls, layout;
};
CropsFixture cropFixture(bool empty = false) {
  cv::Mat image(128, 160, CV_32FC4);
  for (int y = 0; y < image.rows; ++y)
    for (int x = 0; x < image.cols; ++x)
      image.at<cv::Vec4f>(y, x) = {x / 159.f, y / 127.f, .3f, .37f};
  Tensor quads;
  quads.shape = {empty ? 0 : 2, 4, 2};
  if (!empty)
    quads.data = {20, 20, 100, 30, 90, 70, 10, 60, 110, 15, 140, 15, 140, 105, 110, 105};
  Json items = Json::array();
  if (!empty)
    items = {{{"id", "perspective"}, {"score", .9}}, {{"id", "vertical"}, {"score", .8}}};
  auto regions = Data::record(Record{"image.TextRegions.v1",
                                     {{"frame", "camera-image"},
                                      {"unit", "px"},
                                      {"imageSize", {160, 128}},
                                      {"geometryHash", quadHash(quads)},
                                      {"items", items}}});
  test::OpCall call;
  const auto source = ocrImage(image), coordinates = Data::tensor(quads);
  call.inputs = {{"image", source}, {"quads", coordinates}, {"regions", regions}};
  REQUIRE_MESSAGE(call.run("image.ocr_crops").ok, call.status.message);
  return {source,
          coordinates,
          regions,
          call.outputs.at("recInput"),
          call.outputs.at("clsInput"),
          call.outputs.at("layout")};
}
class OcrCancelled : public ExecContext {
public:
  bool cancelled() const override { return true; }
  void progress(float, std::string_view) override {}
  void log(LogLevel, std::string) override {}
  const std::filesystem::path& baseDir() const override {
    static const std::filesystem::path path;
    return path;
  }
  int threadBudget() const override { return 1; }
};
class OcrDelayedCancel : public OcrCancelled {
public:
  bool cancelled() const override { return ++polls > 12; }
  mutable int polls = 0;
};
} // namespace

TEST_CASE("P4 OCR preprocessing preserves RGB depth ROI and original coordinate mapping") {
  for (const auto depth : {PixelDepth::U8, PixelDepth::U16, PixelDepth::F32}) {
    for (const int channels : {1, 3, 4}) {
      const double full = depth == PixelDepth::U8 ? 255 : depth == PixelDepth::U16 ? 65535 : 1;
      cv::Mat image(96, 128,
                    CV_MAKETYPE(depth == PixelDepth::U8    ? CV_8U
                                : depth == PixelDepth::U16 ? CV_16U
                                                           : CV_32F,
                                channels),
                    cv::Scalar(full, full / 2, 0, full * .37));
      const auto original = image.clone();
      Box2D box;
      box.unit = Unit2D::Pixel;
      box.min[0] = 20;
      box.min[1] = 10;
      box.max[0] = 119;
      box.max[1] = 89;
      test::OpCall call;
      call.inputs = {{"image", ocrImage(image)}, {"roi", Data::box2d(box)}};
      REQUIRE_MESSAGE(call.run("image.ocr_preprocess").ok, call.status.message);
      const auto* tensor = call.outputs.at("input").asTensor();
      REQUIRE(tensor->shape == std::vector<std::int64_t>{1, 3, 64, 96});
      const std::size_t plane = 64 * 96;
      CHECK(std::abs(tensor->data[0] - (channels == 1 ? 1.f : -1.f)) < 1e-5);
      CHECK(std::abs(tensor->data[2 * plane] - 1) < 1e-5);
      CHECK(call.outputs.at("mapping").asRecord()->data["crop"] == Json::array({20, 10, 100, 80}));
      CHECK(cv::norm(image, original, cv::NORM_INF) == 0);
    }
  }
  test::OpCall invalid;
  invalid.inputs = {{"image", ocrImage(cv::Mat(64, 64, CV_8U), 2)}};
  CHECK(invalid.run("image.ocr_preprocess").code == "bad_input");
  CHECK(invalid.outputs.empty());
  invalid.inputs = {{"image", ocrImage(cv::Mat(64, 64, CV_8U, cv::Scalar(0)))}};
  CHECK(invalid.run("image.ocr_preprocess", {{"maxSide", Value::integer(63)}}).code == "bad_param");
  CHECK(invalid.outputs.empty());
  cv::Mat finiteImage(32, 32, CV_32FC4, cv::Scalar(0, .5, 1, .37));
  invalid.inputs = {{"image", ocrImage(finiteImage)}};
  REQUIRE(invalid.run("image.ocr_preprocess").ok);
  finiteImage.at<cv::Vec4f>(0, 0)[2] = std::nextafter(1.f, 2.f);
  CHECK(invalid.run("image.ocr_preprocess").code == "bad_input");
  CHECK(invalid.outputs.empty());
  finiteImage.at<cv::Vec4f>(0, 0)[2] = 1;
  finiteImage.at<cv::Vec4f>(0, 0)[3] = std::numeric_limits<float>::quiet_NaN();
  CHECK(invalid.run("image.ocr_preprocess").code == "bad_input");
  CHECK(invalid.outputs.empty());
}

TEST_CASE("P4 DB decode returns finite sorted regions and source alpha without outside mutation") {
  cv::Mat image(96, 128, CV_8UC4, cv::Scalar(20, 40, 80, 91));
  const auto original = image.clone();
  test::OpCall prep;
  prep.inputs = {{"image", ocrImage(image)}};
  REQUIRE(prep.run("image.ocr_preprocess").ok);
  Tensor prediction;
  prediction.shape = {1, 1, 96, 128};
  prediction.data.resize(96 * 128);
  cv::Mat probability(96, 128, CV_32F, prediction.data.data());
  probability(cv::Rect(10, 12, 40, 12)).setTo(.9);
  probability(cv::Rect(70, 14, 40, 12)).setTo(.8);
  probability(cv::Rect(20, 60, 50, 14)).setTo(.7);
  test::OpCall call;
  call.inputs = {{"image", prep.inputs.at("image")},
                 {"mapping", prep.outputs.at("mapping")},
                 {"prediction", Data::tensor(prediction)}};
  REQUIRE_MESSAGE(call.run("image.ocr_db_decode", {{"unclipRatio", Value::number(0)}}).ok,
                  call.status.message);
  const auto& regions = call.outputs.at("regions").asRecord()->data;
  REQUIRE(regions["items"].size() == 3);
  const auto geometry = call.outputs.at("quads").asTensor()->data;
  CHECK(std::abs(geometry[0] - 10) < 1e-4);
  CHECK(std::abs(geometry[8] - 70) < 1e-4);
  CHECK(std::abs(geometry[16] - 20) < 1e-4);
  for (int i = 0; i < 3; ++i)
    CHECK(regions["items"][i]["id"] == "text-" + std::to_string(i));
  const auto* overlay = call.outputs.at("image").asImage();
  REQUIRE(overlay->channels == 4);
  cv::Mat alpha;
  cv::extractChannel(cvx::view(*overlay), alpha, 3);
  CHECK(cv::countNonZero(alpha != 91) == 0);
  CHECK(cv::norm(image, original, cv::NORM_INF) == 0);
  const auto expected = call.outputs.at("regions").asRecord()->data;
  REQUIRE(call.run("image.ocr_db_decode", {{"unclipRatio", Value::number(0)}}).ok);
  CHECK(call.outputs.at("quads").asTensor()->data == geometry);
  CHECK(call.outputs.at("regions").asRecord()->data == expected);
  CHECK(call.run("image.ocr_db_decode", {{"maxRegions", Value::integer(2)}}).code ==
        "resource_limit");
  CHECK(call.outputs.empty());
  prediction.shape[1] = 2;
  prediction.data.resize(2 * 96 * 128);
  call.inputs["prediction"] = Data::tensor(prediction);
  CHECK(call.run("image.ocr_db_decode").code == "bad_input");
  CHECK(call.outputs.empty());
  const auto largeImage = ocrImage(cv::Mat(256, 256, CV_8U, cv::Scalar(0)));
  prep.inputs = {{"image", largeImage}};
  REQUIRE(prep.run("image.ocr_preprocess").ok);
  Tensor busy;
  busy.shape = {1, 1, 256, 256};
  busy.data.resize(256 * 256);
  for (int y = 0; y < 256; y += 2)
    for (int x = 0; x < 256; x += 2)
      busy.data[y * 256 + x] = .9f;
  call.inputs = {{"image", largeImage},
                 {"mapping", prep.outputs.at("mapping")},
                 {"prediction", Data::tensor(busy)}};
  CHECK(call.run("image.ocr_db_decode").code == "resource_limit");
  CHECK(call.outputs.empty());
}

TEST_CASE("P4 OCR perspective and vertical crops retain ID and pixel orientation") {
  const auto fixture = cropFixture();
  const auto* input = fixture.rec.asTensor();
  REQUIRE(input->shape == std::vector<std::int64_t>{2, 3, 48, 320});
  const auto& layout = fixture.layout.asRecord()->data;
  CHECK(layout["items"][0]["id"] == "perspective");
  CHECK(layout["items"][1]["id"] == "vertical");
  CHECK(layout["items"][1]["rotated90"] == true);
  const std::size_t plane = 48 * 320;
  CHECK(std::abs(input->data[0] + .4f) < 1e-5);
  CHECK(std::abs(input->data[2 * plane] - (20.f / 159 * 2 - 1)) < .04);
  CHECK(std::abs(input->data[2 * plane + 47 * 320] - (10.f / 159 * 2 - 1)) < .04);
}

TEST_CASE("P4 OCR empty batches invalid geometry and cancellation never publish partial results") {
  const auto empty = cropFixture(true);
  REQUIRE(empty.rec.asTensor());
  CHECK(empty.rec.asTensor()->shape == std::vector<std::int64_t>{0, 3, 48, 320});
  CHECK(empty.layout.asRecord()->data["items"].empty());
  const auto fixture = cropFixture();
  auto corrupt = *fixture.quads.asTensor();
  corrupt.data[0] = -1;
  test::OpCall invalid;
  invalid.inputs = {
      {"image", fixture.image}, {"quads", Data::tensor(corrupt)}, {"regions", fixture.regions}};
  CHECK(invalid.run("image.ocr_crops").code == "bad_input");
  CHECK(invalid.outputs.empty());
  auto regions = *fixture.regions.asRecord();
  regions.data["geometryHash"] = quadHash(corrupt);
  invalid.inputs["regions"] = Data::record(regions);
  CHECK(invalid.run("image.ocr_crops").code == "bad_input");
  CHECK(invalid.outputs.empty());
  corrupt = *fixture.quads.asTensor();
  corrupt.data[0] = std::numeric_limits<float>::quiet_NaN();
  invalid.inputs["quads"] = Data::tensor(corrupt);
  CHECK(invalid.run("image.ocr_crops").code == "bad_input");
  CHECK(invalid.outputs.empty());
  const auto* op = ensureRegistry().find("image.ocr_crops");
  REQUIRE(op);
  ParamMap params;
  for (const auto& p : op->params)
    params[p.name] = p.def;
  const std::filesystem::path base;
  ParamView view(params, base);
  Inputs input(invalid.inputs);
  std::unordered_map<std::string, Data> storage;
  Outputs output(storage);
  OcrCancelled cancelled;
  CHECK(op->compute(input, view, output, cancelled).code == "cancelled");
  CHECK(storage.empty());
  Tensor many;
  many.shape = {128, 4, 2};
  auto repeated = *fixture.regions.asRecord();
  repeated.data["items"] = Json::array();
  for (int i = 0; i < 128; ++i) {
    many.data.insert(many.data.end(), fixture.quads.asTensor()->data.begin(),
                     fixture.quads.asTensor()->data.begin() + 8);
    repeated.data["items"].push_back({{"id", "row-" + std::to_string(i)}, {"score", .9}});
  }
  repeated.data["geometryHash"] = quadHash(many);
  invalid.inputs = {
      {"image", fixture.image}, {"quads", Data::tensor(many)}, {"regions", Data::record(repeated)}};
  Inputs repeatedInput(invalid.inputs);
  OcrDelayedCancel delayed;
  CHECK(op->compute(repeatedInput, view, output, delayed).code == "cancelled");
  CHECK(delayed.polls > 12);
  CHECK(storage.empty());
}

#if defined(LYFLOW_P4_MODEL_TESTS)
TEST_CASE("P4 public PP OCRv6 and direction ONNX models execute the complete native OCR chain") {
  const auto* configured = std::getenv("LYFLOW_P4_ASSETS");
  REQUIRE(configured != nullptr);
  const std::filesystem::path directory(configured);
  cv::Mat scene = cv::imread((directory / "ocr-scene.png").u8string(), cv::IMREAD_COLOR);
  REQUIRE_FALSE(scene.empty());
  cv::cvtColor(scene, scene, cv::COLOR_BGR2RGB);
  const auto infer = [&](const Data& input, const char* file, const char* contract, bool serial) {
    test::OpCall run;
    run.inputs = {{"input", input}};
    REQUIRE_MESSAGE(
        run.run("ml.onnx_run", {{"modelPath", Value::text((directory / file).u8string())},
                                {"modelContract", Value::text(contract)},
                                {"batchMode", Value::text(serial ? "serial" : "model")}})
            .ok,
        run.status.message);
    return run.outputs.at("output");
  };
  const auto runScene = [&](const cv::Mat& image) {
    const auto original = image.clone();
    const auto source = ocrImage(image);
    test::OpCall prep;
    prep.inputs = {{"image", source}};
    REQUIRE(prep.run("image.ocr_preprocess").ok);
    test::OpCall detect;
    detect.inputs = {{"image", source},
                     {"mapping", prep.outputs.at("mapping")},
                     {"prediction", infer(prep.outputs.at("input"), "det.onnx", "ocr-db", false)}};
    REQUIRE_MESSAGE(detect.run("image.ocr_db_decode").ok, detect.status.message);
    test::OpCall crop;
    crop.inputs = {{"image", source},
                   {"quads", detect.outputs.at("quads")},
                   {"regions", detect.outputs.at("regions")}};
    REQUIRE_MESSAGE(crop.run("image.ocr_crops").ok, crop.status.message);
    test::OpCall direction;
    direction.inputs = {{"recInput", crop.outputs.at("recInput")},
                        {"layout", crop.outputs.at("layout")},
                        {"classification",
                         infer(crop.outputs.at("clsInput"), "cls.onnx", "ocr-direction-2", true)}};
    REQUIRE_MESSAGE(direction.run("image.ocr_orient").ok, direction.status.message);
    test::OpCall decode;
    decode.inputs = {
        {"prediction", infer(direction.outputs.at("input"), "rec.onnx", "ocr-ctc", true)},
        {"layout", direction.outputs.at("layout")}};
    REQUIRE_MESSAGE(
        decode
            .run("image.ocr_ctc_decode",
                 {{"dictionaryPath", Value::text((directory / "paddle-v6-dict.txt").u8string())}})
            .ok,
        decode.status.message);
    CHECK(cv::norm(image, original, cv::NORM_INF) == 0);
    return std::make_pair(decode.outputs.at("text"), decode.outputs.at("results"));
  };
  for (int variant = 0; variant < 2; ++variant) {
    CAPTURE(variant);
    cv::Mat input = scene;
    if (variant) {
      auto affine = cv::getRotationMatrix2D({600, 400}, -12, 1);
      affine.at<double>(0, 2) += 200;
      affine.at<double>(1, 2) += 200;
      cv::warpAffine(scene, input, affine, {1600, 1200}, cv::INTER_LINEAR, cv::BORDER_CONSTANT,
                     cv::Scalar(255, 255, 255));
    }
    const auto result = runScene(input);
    auto text = result.first.asRecord()->data["value"].get<std::string>();
    CAPTURE(text);
    text.erase(std::remove(text.begin(), text.end(), ' '), text.end());
    CHECK(text == "LYFLOW12345\nINDUSTRIALOCR\n工业视觉12345\nHELLO2026");
    const auto& items = result.second.asRecord()->data["items"];
    REQUIRE(items.size() == 4);
    CHECK(items[3]["orientation"] == 180);
  }
  const auto blank = runScene(cv::Mat(320, 640, CV_8UC3, cv::Scalar(255, 255, 255)));
  CHECK(blank.first.asRecord()->data["value"] == "");
  CHECK(blank.second.asRecord()->data["items"].empty());
}
#endif
