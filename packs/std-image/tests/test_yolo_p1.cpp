#include <doctest/doctest.h>

#include "helpers.h"

namespace {
using namespace lyflow;
using Json = nlohmann::json;

}

TEST_CASE(
    "YOLOv8 YOLO11 YOLO26 preprocessing uses RGB normalization centered padding and explicit formats") {
  test::OpCall call;
  Image image = Image::allocate(4, 2, 3, PixelDepth::U8);
  for (int i = 0; i < 8; ++i) {
    image.mutablePixels()[3 * i] = 10;
    image.mutablePixels()[3 * i + 1] = 20;
    image.mutablePixels()[3 * i + 2] = 30;
  }
  call.inputs = {{"image", Data::image(image)}};
  for (const auto& family : {"yolov8", "yolo11", "yolo26"}) {
    REQUIRE(call.run("image.yolo_preprocess", {{"inputWidth", Value::integer(64)},
                                               {"inputHeight", Value::integer(64)},
                                               {"modelFamily", Value::text(family)}})
                .ok);
    const Tensor* tensor = call.outputs.at("tensor").asTensor();
    REQUIRE(tensor != nullptr);
    CHECK(tensor->shape == std::vector<std::int64_t>({1, 3, 64, 64}));
    CHECK(tensor->data[0] == doctest::Approx(114.0 / 255));
    CHECK(tensor->data[16 * 64] == doctest::Approx(10.0 / 255));
    CHECK(tensor->data[2 * 64 * 64 + 16 * 64] == doctest::Approx(30.0 / 255));
    const auto& metadata = call.outputs.at("mapping").asRecord()->data;
    CHECK(metadata["padding"] == Json::array({0, 16}));
    CHECK(metadata["classNames"].size() == 80);
    CHECK(metadata["outputFormat"] == (std::string(family) == "yolo26" ? "end2end" : "raw"));
  }
  CHECK(image.at(0, 0, 0) == 10);
  CHECK_FALSE(call.run("image.yolo_preprocess", {{"inputWidth", Value::integer(33)}}).ok);
  for (const auto& labels : {"[]", "[1]", "[\"\"]", "null"})
    CHECK_FALSE(call.run("image.yolo_preprocess", {{"classNames", Value::text(labels)}}).ok);
  CHECK_FALSE(call.run("image.yolo_preprocess", {{"modelFamily", Value::text("unknown")}}).ok);
}
