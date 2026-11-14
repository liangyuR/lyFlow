#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <opencv2/imgproc.hpp>
#include <opencv2/objdetect.hpp>

#include "helpers.h"
#include "lyflow/pose.h"
#include "lyflow_cv/adapter.h"

namespace lyflow::ops {
void registerP4Barcodes(Registry&);
}
namespace {
using namespace lyflow;
using Json = nlohmann::json;

Registry barcodeRegistry() {
  Registry registry;
  ops::registerP4Barcodes(registry);
  return registry;
}
Data barcodeImage(cv::Mat matrix, int scale = 1) {
  Image image;
  REQUIRE(cvx::fromMat(std::move(matrix), image));
  image.scale = scale;
  return Data::image(std::move(image));
}
cv::Mat qrModules(const std::string& text) {
  cv::QRCodeEncoder::Params params;
  params.version = 1;
  params.mode = cv::QRCodeEncoder::MODE_BYTE;
  params.correction_level = cv::QRCodeEncoder::CORRECT_LEVEL_L;
  cv::Mat encoded;
  cv::QRCodeEncoder::create(params)->encode(text, encoded);
  REQUIRE(encoded.rows == 25);
  REQUIRE(encoded.cols == 25);
  return encoded(cv::Rect(2, 2, 21, 21)).clone();
}
cv::Mat qrImage(const std::string& text, int pixels = 5) {
  cv::Mat enlarged, bordered;
  cv::resize(qrModules(text), enlarged, {21 * pixels, 21 * pixels}, 0, 0, cv::INTER_NEAREST);
  cv::copyMakeBorder(enlarged, bordered, 4 * pixels, 4 * pixels, 4 * pixels, 4 * pixels,
                     cv::BORDER_CONSTANT, cv::Scalar(255));
  return bordered;
}
cv::Mat code39() {
  std::vector<int> widths(10, 1);
  std::vector<bool> bars(10, false);
  for (const int symbol : {0x094, 0x109, 0x109, 0x109, 0x109, 0x094}) {
    for (int bit = 8; bit >= 0; --bit) {
      widths.push_back((symbol & (1 << bit)) ? 3 : 1);
      bars.push_back(bit % 2 == 0);
    }
    widths.push_back(1);
    bars.push_back(false);
  }
  widths.push_back(10);
  bars.push_back(false);
  int total = 0;
  for (int width : widths)
    total += width * 3;
  cv::Mat result(90, total, CV_8U, cv::Scalar(255));
  int x = 0;
  for (std::size_t i = 0; i < widths.size(); ++i) {
    if (bars[i])
      result(cv::Rect(x, 5, widths[i] * 3, 80)).setTo(0);
    x += widths[i] * 3;
  }
  return result;
}
const Json& detections(const test::OpCall& call) {
  const auto* record = call.outputs.at("detections").asRecord();
  REQUIRE(record != nullptr);
  REQUIRE(record->type == "image.Barcodes.v1");
  return record->data.at("items");
}
const Json& refined(const test::OpCall& call) {
  const auto* record = call.outputs.at("feedback").asRecord();
  REQUIRE(record != nullptr);
  REQUIRE(record->type == "image.QrRefinement.v1");
  return record->data;
}
std::vector<std::string> decodedTexts(const Json& items) {
  std::vector<std::string> result;
  for (const auto& item : items)
    if (item.at("valid") == true)
      result.push_back(item.at("text").get<std::string>());
  std::sort(result.begin(), result.end());
  return result;
}
class BarcodeCancelContext final : public ExecContext {
public:
  explicit BarcodeCancelContext(int at) : at_(at) {}
  bool cancelled() const override { return ++checks_ >= at_; }
  void progress(float, std::string_view) override {}
  void log(LogLevel, std::string) override {}
  const std::filesystem::path& baseDir() const override { return base_; }
  int threadBudget() const override { return 1; }

private:
  int at_;
  mutable int checks_ = 0;
  std::filesystem::path base_;
};
} // namespace

TEST_CASE("barcode decoding preserves Chinese payload empty content and real linear codes") {
  const auto registry = barcodeRegistry();
  for (const std::string text :
       {std::string("LyFlow-QR"), std::string(u8"你好-LyFlow"), std::string()}) {
    CAPTURE(text);
    test::OpCall call(registry);
    call.inputs = {{"image", barcodeImage(qrImage(text))}};
    REQUIRE_MESSAGE(call.run("image.barcode_decode").ok, call.status.message);
    CHECK(decodedTexts(detections(call)) == std::vector<std::string>{text});
    const auto& items = detections(call);
    REQUIRE(items.size() == 1);
    CHECK(items[0].at("format") == "QRCode");
    CHECK(items[0].at("bytesHex").get<std::string>().size() == 2 * text.size());
    const auto* poses = call.outputs.at("instances").asRecord();
    REQUIRE(poses != nullptr);
    REQUIRE(poses->data.at("items").size() == 1);
    CHECK(poses->data.at("items")[0].at("pickable") == false);
  }
  test::OpCall linear(registry);
  linear.inputs = {{"image", barcodeImage(code39())}};
  REQUIRE(linear.run("image.barcode_decode", {{"formats", Value::text("Linear")}}).ok);
  CHECK(decodedTexts(detections(linear)) == std::vector<std::string>{"AAAA"});
  CHECK(detections(linear)[0].at("format") == "Code39");
  REQUIRE(linear.run("image.barcode_decode", {{"formats", Value::text("QRCode")}}).ok);
  CHECK(decodedTexts(detections(linear)).empty());
}

TEST_CASE("barcode multi target rotation ROI and overflow keep explicit coordinate semantics") {
  const auto registry = barcodeRegistry();
  cv::Mat first = qrImage("FIRST"), second;
  cv::rotate(qrImage("SECOND"), second, cv::ROTATE_90_CLOCKWISE);
  cv::Mat canvas(230, 400, CV_8U, cv::Scalar(255));
  first.copyTo(canvas(cv::Rect(10, 20, first.cols, first.rows)));
  second.copyTo(canvas(cv::Rect(220, 45, second.cols, second.rows)));
  test::OpCall call(registry);
  call.inputs = {{"image", barcodeImage(canvas.clone(), 2)}};
  REQUIRE(call.run("image.barcode_decode").ok);
  CHECK(decodedTexts(detections(call)) == std::vector<std::string>{"FIRST", "SECOND"});
  const auto& firstCorners = detections(call)[0].at("corners");
  CHECK(firstCorners[0][0].get<double>() >= 50);
  CHECK(firstCorners[0][0].get<double>() <= 70);
  REQUIRE_FALSE(call.run("image.barcode_decode", {{"maxSymbols", Value::integer(1)}}).ok);
  CHECK(call.status.code == "resource_limit");
  CHECK(call.outputs.empty());
  Box2D roi;
  roi.unit = Unit2D::Pixel;
  roi.min[0] = roi.min[1] = 0;
  roi.max[0] = 180;
  roi.max[1] = 200;
  call.inputs["roi"] = Data::box2d(roi);
  REQUIRE(call.run("image.barcode_decode").ok);
  CHECK(decodedTexts(detections(call)) == std::vector<std::string>{"FIRST"});
  call.inputs.erase("roi");
  auto region = poses::make2D("region", "image", "px", {165, 185}, .04, {340, 400});
  call.inputs["region"] = Data::record(region);
  REQUIRE(call.run("image.barcode_decode").ok);
  CHECK(decodedTexts(detections(call)) == std::vector<std::string>{"FIRST"});
  region.data["frame"] = "wrong";
  call.inputs["region"] = Data::record(region);
  CHECK_FALSE(call.run("image.barcode_decode").ok);
  CHECK(call.outputs.empty());
}

TEST_CASE("barcode result budget probes 128 and rejects the 129th real symbol") {
  const auto registry = barcodeRegistry();
  const auto tile = qrImage("CAP", 3);
  for (const int count : {128, 129}) {
    CAPTURE(count);
    cv::Mat canvas(tile.rows * 9, tile.cols * 16, CV_8U, cv::Scalar(255));
    for (int i = 0; i < count; ++i)
      tile.copyTo(
          canvas(cv::Rect((i % 16) * tile.cols, (i / 16) * tile.rows, tile.cols, tile.rows)));
    test::OpCall call(registry);
    call.inputs = {{"image", barcodeImage(canvas)}};
    const auto status =
        call.run("image.barcode_decode", {{"formats", Value::text("QRCodeModel2")}});
    if (count == 128) {
      REQUIRE_MESSAGE(status.ok, status.message);
      CHECK(decodedTexts(detections(call)).size() == 128);
    } else {
      CHECK_FALSE(status.ok);
      CHECK(status.code == "resource_limit");
      CHECK(call.outputs.empty());
    }
  }
}

TEST_CASE("barcode supports image depths RGBA readonly alpha and clear empty invalid states") {
  const auto registry = barcodeRegistry();
  for (int depth : {CV_8U, CV_16U, CV_32F}) {
    CAPTURE(depth);
    cv::Mat rgba;
    cv::cvtColor(qrImage("RGBA"), rgba, cv::COLOR_GRAY2RGBA);
    rgba.convertTo(rgba, CV_MAKETYPE(depth, 4),
                   depth == CV_8U    ? 1
                   : depth == CV_16U ? 257
                                     : 1.0 / 255);
    cv::Mat alpha(rgba.rows, rgba.cols, CV_MAKETYPE(depth, 1),
                  cv::Scalar(depth == CV_8U    ? 73
                             : depth == CV_16U ? 10001
                                               : .25));
    cv::insertChannel(alpha, rgba, 3);
    const auto original = rgba.clone();
    test::OpCall call(registry);
    call.inputs = {{"image", barcodeImage(rgba)}};
    REQUIRE(call.run("image.barcode_decode").ok);
    CHECK(decodedTexts(detections(call)) == std::vector<std::string>{"RGBA"});
    CHECK(cv::norm(rgba, original, cv::NORM_INF) == 0);
    const auto* output = call.outputs.at("image").asImage();
    REQUIRE(output != nullptr);
    CHECK(output->channels == 4);
    cv::Mat actualAlpha;
    cv::extractChannel(cvx::view(*output), actualAlpha, 3);
    CHECK(cv::norm(actualAlpha, alpha, cv::NORM_INF) == 0);
  }
  test::OpCall blank(registry);
  blank.inputs = {{"image", barcodeImage(cv::Mat(100, 100, CV_8U, cv::Scalar(255)))}};
  REQUIRE(blank.run("image.barcode_decode").ok);
  CHECK(detections(blank).empty());
  cv::Mat invalid(100, 100, CV_32F, cv::Scalar(0));
  invalid.at<float>(0, 0) = std::numeric_limits<float>::quiet_NaN();
  blank.inputs["image"] = barcodeImage(invalid);
  CHECK_FALSE(blank.run("image.barcode_decode").ok);
  CHECK(blank.outputs.empty());
  blank.inputs["image"] = Data{};
  CHECK_FALSE(blank.run("image.barcode_decode").ok);
  CHECK(blank.outputs.empty());
  blank.inputs["image"] = barcodeImage(cv::Mat(2048, 4097, CV_8U, cv::Scalar(255)));
  CHECK_FALSE(blank.run("image.barcode_decode").ok);
  CHECK(blank.status.code == "resource_limit");
  CHECK(blank.outputs.empty());
  const auto* descriptor = registry.find("image.barcode_decode");
  REQUIRE(descriptor != nullptr);
  ParamMap params;
  for (const auto& param : descriptor->params)
    params[param.name] = param.def;
  for (const int at : {1, 3}) {
    BarcodeCancelContext context(at);
    std::unordered_map<std::string, Data> inputs{{"image", barcodeImage(qrImage("CANCEL"))}},
        outputs;
    Inputs inputView(inputs);
    Outputs outputView(outputs);
    const auto status = descriptor->compute(inputView, ParamView(params, {}), outputView, context);
    CHECK_FALSE(status.ok);
    CHECK(status.code == "cancelled");
    CHECK(outputs.empty());
  }
}

TEST_CASE(
    "QR reconstruction verifies standard modules quiet zone before after and readonly source") {
  const auto registry = barcodeRegistry();
  cv::Mat source;
  cv::resize(qrModules(u8"你好-LyFlow"), source, {168, 168}, 0, 0, cv::INTER_NEAREST);
  const auto original = source.clone();
  test::OpCall call(registry);
  call.inputs = {{"image", barcodeImage(source)}};
  REQUIRE_MESSAGE(call.run("image.qr_refine").ok, call.status.message);
  const auto& feedback = refined(call);
  CHECK(feedback.at("beforeDecoded") == true);
  CHECK(feedback.at("afterDecoded") == true);
  CHECK(decodedTexts(feedback.at("after")) == std::vector<std::string>{u8"你好-LyFlow"});
  CHECK(feedback.at("quietZone") == 4);
  CHECK(feedback.at("improved") == false);
  const auto* image = call.outputs.at("image").asImage();
  REQUIRE(image != nullptr);
  CHECK(image->width == 232);
  CHECK(image->height == 232);
  CHECK(image->scale == 1);
  CHECK(image->channels == 1);
  CHECK(image->depth == PixelDepth::U8);
  CHECK(cv::countNonZero(cvx::view(*image)(cv::Rect(0, 0, 232, 32)) != 255) == 0);
  CHECK(cv::norm(source, original, cv::NORM_INF) == 0);
  cv::bitwise_not(source, source);
  call.inputs["image"] = barcodeImage(source);
  REQUIRE(call.run("image.qr_refine", {{"polarity", Value::text("light")}}).ok);
  CHECK(refined(call).at("afterDecoded") == true);
  CHECK_FALSE(call.run("image.qr_refine", {{"moduleCount", Value::integer(16)}}).ok);
  CHECK(call.outputs.empty());
  call.inputs["image"] = barcodeImage(cv::Mat(168, 168, CV_8U, cv::Scalar(255)));
  REQUIRE(call.run("image.qr_refine").ok);
  CHECK(refined(call).at("afterDecoded") == false);
}

TEST_CASE("QR detection quadrilaterals drive perspective refinement with frame validation") {
  const auto registry = barcodeRegistry();
  const auto source = qrImage("PERSPECTIVE", 6);
  const std::vector<cv::Point2f> from{{0, 0}, {173, 0}, {173, 173}, {0, 173}};
  const std::vector<cv::Point2f> to{{35, 20}, {240, 45}, {220, 250}, {18, 215}};
  cv::Mat canvas;
  cv::warpPerspective(source, canvas, cv::getPerspectiveTransform(from, to), {280, 280},
                      cv::INTER_NEAREST, cv::BORDER_CONSTANT, cv::Scalar(255));
  test::OpCall decode(registry);
  decode.inputs = {{"image", barcodeImage(canvas)}};
  REQUIRE(decode.run("image.barcode_decode").ok);
  REQUIRE(decodedTexts(detections(decode)) == std::vector<std::string>{"PERSPECTIVE"});
  test::OpCall refine(registry);
  refine.inputs = {{"image", decode.inputs.at("image")},
                   {"detections", decode.outputs.at("detections")}};
  REQUIRE_MESSAGE(refine.run("image.qr_refine").ok, refine.status.message);
  CHECK(decodedTexts(refined(refine).at("after")) == std::vector<std::string>{"PERSPECTIVE"});
  auto changed = *decode.outputs.at("detections").asRecord();
  changed.data["frame"] = "different";
  refine.inputs["detections"] = Data::record(changed);
  CHECK_FALSE(refine.run("image.qr_refine").ok);
  CHECK(refine.outputs.empty());
  changed = *decode.outputs.at("detections").asRecord();
  changed.data["items"][0]["corners"][0][0] = 1e308;
  refine.inputs["detections"] = Data::record(changed);
  CHECK_FALSE(refine.run("image.qr_refine").ok);
  CHECK(refine.outputs.empty());
}

TEST_CASE("QR cell reconstruction reports actual degraded before and improved after") {
  const auto registry = barcodeRegistry();
  cv::Mat source;
  cv::resize(qrModules("REFINE"), source, {189, 189}, 0, 0, cv::INTER_NEAREST);
  for (int y = 0; y < source.rows; ++y)
    if (y % 9 < 4)
      cv::bitwise_not(source.row(y), source.row(y));
  test::OpCall call(registry);
  call.inputs = {{"image", barcodeImage(source)}};
  REQUIRE(call.run("image.qr_refine").ok);
  CAPTURE(refined(call).dump());
  CHECK(refined(call).at("beforeDecoded") == false);
  CHECK(refined(call).at("afterDecoded") == true);
  CHECK(refined(call).at("improved") == true);
  CHECK(decodedTexts(refined(call).at("after")) == std::vector<std::string>{"REFINE"});
}

TEST_CASE("QR refinement scheduler keeps its reconstructed grid absolute after source preview") {
  cv::Mat canvas(2200, 2200, CV_8U, cv::Scalar(255));
  const auto code = qrImage("SCALE-GRID", 72);
  code.copyTo(canvas(cv::Rect(56, 56, code.cols, code.rows)));
  const auto sourceData = barcodeImage(canvas);
  const auto graph = test::makeGraph(
      {{"source", "io.load_image", {{"path", "injected-qr.png"}}},
       {"before", "image.barcode_decode", {{"formats", "QRCodeModel2"}}},
       {"refine", "image.qr_refine", {{"outputFrame", "qr_refined"}, {"modulePixels", 8}}},
       {"after", "image.barcode_decode", {{"formats", "QRCodeModel2"}, {"frame", "qr_refined"}}}},
      {{"source.image", "before.image"},
       {"source.image", "refine.image"},
       {"before.detections", "refine.detections"},
       {"refine.image", "after.image"}});
  for (const auto mode : {exec::RunMode::Full, exec::RunMode::Preview}) {
    CAPTURE(mode == exec::RunMode::Preview);
    exec::RunOptions options;
    options.runId = mode == exec::RunMode::Full ? "p4-qr-absolute-full" : "p4-qr-absolute-preview";
    options.mode = mode;
    options.noReuse = true;
    options.maxParallel = 1;
    options.inputs = {{"source", "image", sourceData}};
    test::RunLog log;
    exec::Run run(graph.dump(), options, &test::detail::collect, &log);
    run.join();
    REQUIRE_MESSAGE(log.runStatus() == "ok", Json(log.events).dump());
    Data source, enhanced, before, feedback, after, overlay, instances;
    auto& results = exec::ResultStore::instance();
    REQUIRE(results.get(options.runId, "source", "image", source));
    REQUIRE(results.get(options.runId, "refine", "image", enhanced));
    REQUIRE(results.get(options.runId, "before", "detections", before));
    REQUIRE(results.get(options.runId, "refine", "feedback", feedback));
    REQUIRE(results.get(options.runId, "after", "detections", after));
    REQUIRE(results.get(options.runId, "after", "image", overlay));
    REQUIRE(results.get(options.runId, "after", "instances", instances));
    const int scale = mode == exec::RunMode::Full ? 1 : 2;
    CHECK(source.asImage()->scale == scale);
    CHECK(source.asImage()->width == 2200 / scale);
    CHECK(enhanced.asImage()->width == 232);
    CHECK(enhanced.asImage()->height == 232);
    CHECK(enhanced.asImage()->scale == 1);
    CHECK(overlay.asImage()->scale == 1);
    const auto& initial = before.asRecord()->data;
    const auto& mapped = feedback.asRecord()->data;
    const auto& decoded = after.asRecord()->data;
    REQUIRE(initial["items"].size() == 1);
    REQUIRE(decoded["items"].size() == 1);
    CHECK(decodedTexts(initial["items"]) == std::vector<std::string>{"SCALE-GRID"});
    CHECK(decodedTexts(decoded["items"]) == std::vector<std::string>{"SCALE-GRID"});
    CHECK(initial["imageSize"] == Json::array({2200., 2200.}));
    CHECK(decoded["imageSize"] == Json::array({232., 232.}));
    CHECK(decoded["frame"] == mapped["outputFrame"]);
    CHECK(mapped["modulePixels"] == 8);
    CHECK(mapped["sourceCorners"] == initial["items"][0]["corners"]);
    const auto matrix = mapped["outputToSource"].get<std::array<double, 9>>();
    const std::array<std::array<double, 2>, 4> grid{{{32, 32}, {199, 32}, {199, 199}, {32, 199}}};
    const auto project = [&](double x, double y) {
      const double w = matrix[6] * x + matrix[7] * y + matrix[8];
      return std::array<double, 2>{(matrix[0] * x + matrix[1] * y + matrix[2]) / w,
                                   (matrix[3] * x + matrix[4] * y + matrix[5]) / w};
    };
    for (std::size_t i = 0; i < 4; ++i) {
      const auto expected = initial["items"][0]["corners"][i].get<std::array<double, 2>>();
      const auto actual = project(grid[i][0], grid[i][1]);
      CHECK(actual[0] == doctest::Approx(expected[0]).epsilon(1e-6));
      CHECK(actual[1] == doctest::Approx(expected[1]).epsilon(1e-6));
      const auto corner = decoded["items"][0]["corners"][i].get<std::array<double, 2>>();
      CHECK(corner[0] >= 0);
      CHECK(corner[0] < 232);
      CHECK(corner[1] >= 0);
      CHECK(corner[1] < 232);
      const auto restored = project(corner[0], corner[1]);
      // ZXing返回模块外沿，允许相对中心栅格多两个重构像素。
      const double tolerance = 2.1 * (21 * 72) / (21 * 8 - 1);
      CHECK(std::abs(restored[0] - expected[0]) <= tolerance);
      CHECK(std::abs(restored[1] - expected[1]) <= tolerance);
    }
    REQUIRE(instances.asRecord()->data["items"].size() == 1);
    const auto center =
        instances.asRecord()->data["items"][0]["position"].get<std::array<double, 2>>();
    CHECK(center[0] >= 0);
    CHECK(center[0] < 232);
    CHECK(center[1] >= 0);
    CHECK(center[1] < 232);
  }
}
