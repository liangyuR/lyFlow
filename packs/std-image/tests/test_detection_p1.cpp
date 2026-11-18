#include <doctest/doctest.h>

#include <cmath>
#include <limits>

#include "helpers.h"

namespace {
using namespace lyflow;
using Json = nlohmann::json;

Image detectionImage(int width, int height, int channels = 3, PixelDepth depth = PixelDepth::U8) {
  return Image::allocate(width, height, channels, depth);
}

}

TEST_CASE("YOLOX preprocessing declares exact BGR layout padding ROI and preview mapping") {
  test::OpCall call;
  Image image = detectionImage(4, 2);
  for (int i = 0; i < 8; ++i) {
    image.mutablePixels()[i * 3] = 10;
    image.mutablePixels()[i * 3 + 1] = 20;
    image.mutablePixels()[i * 3 + 2] = 30;
  }
  image.scale = 2;
  call.inputs = {{"image", Data::image(image)}};
  REQUIRE(call.run("image.yolox_preprocess").ok);
  const Tensor* result = call.outputs.at("tensor").asTensor();
  REQUIRE(result != nullptr);
  CHECK(result->shape == std::vector<std::int64_t>({1, 3, 416, 416}));
  CHECK(result->data[0] == 30);
  CHECK(result->data[416 * 416] == 20);
  CHECK(result->data[2 * 416 * 416] == 10);
  CHECK(result->data[208 * 416] == 114);
  CHECK(image.at(0, 0, 0) == 10);
  const auto& metadata = call.outputs.at("mapping").asRecord()->data;
  CHECK(metadata["resizedSize"] == Json::array({416, 208}));
  CHECK(metadata["imageScale"] == 2);
  Box2D roi;
  roi.unit = Unit2D::Pixel;
  roi.min[0] = 0.2f;
  roi.max[0] = 1.2f;
  roi.max[1] = 2;
  call.inputs["roi"] = Data::box2d(roi);
  REQUIRE(call.run("image.yolox_preprocess", {{"frame", Value::text("相机一")}}).ok);
  CHECK(call.outputs.at("mapping").asRecord()->data["crop"] == Json::array({0, 0, 2, 2}));
  roi.max[0] = roi.min[0];
  call.inputs["roi"] = Data::box2d(roi);
  CHECK_FALSE(call.run("image.yolox_preprocess").ok);
  CHECK(call.outputs.empty());
  call.inputs.erase("roi");
  for (int channels : {1, 4}) {
    call.inputs["image"] = Data::image(detectionImage(1, 1, channels));
    REQUIRE(call.run("image.yolox_preprocess").ok);
  }
  for (PixelDepth depth : {PixelDepth::U16, PixelDepth::F32}) {
    call.inputs["image"] = Data::image(detectionImage(4, 2, 3, depth));
    CHECK_FALSE(call.run("image.yolox_preprocess").ok);
  }
  call.inputs["image"] = Data::image(image);
  CHECK_FALSE(call.run("image.yolox_preprocess", {{"frame", Value::text("")}}).ok);
}
