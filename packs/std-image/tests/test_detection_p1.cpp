#include <doctest/doctest.h>

#include <cmath>
#include <limits>

#include <opencv2/core.hpp>

#include "helpers.h"
#include "lyflow/pose.h"
#include "lyflow_cv/adapter.h"

namespace {
using namespace lyflow;
using Json = nlohmann::json;

Image detectionImage(int width, int height, int channels = 3, PixelDepth depth = PixelDepth::U8) {
  return Image::allocate(width, height, channels, depth);
}

Record mapping() {
  return {"vision.YOLOXPreprocess.v1",
          {{"model", "yolox-nano-0.1.1rc0"},
           {"workingSize", {416, 416}},
           {"imageScale", 1},
           {"crop", {0, 0, 416, 416}},
           {"resizedSize", {416, 416}},
           {"inputSize", {416, 416}},
           {"frame", "camera-测试"}}};
}

Tensor prediction() {
  Tensor result;
  result.shape = {1, 3549, 85};
  result.data.resize(3549 * 85);
  return result;
}

void box(Tensor& tensor, int anchor, double cx, double cy, double width, double height,
         float objectness = 0.9f, int cls = 0, float classScore = 1) {
  int stride = 8, begin = 0, side = 52;
  if (anchor >= 52 * 52) {
    stride = 16;
    begin = 52 * 52;
    side = 26;
  }
  if (anchor >= 52 * 52 + 26 * 26) {
    stride = 32;
    begin = 52 * 52 + 26 * 26;
    side = 13;
  }
  const int gx = (anchor - begin) % side, gy = (anchor - begin) / side;
  float* row = tensor.data.data() + anchor * 85;
  row[0] = static_cast<float>(cx / stride - gx);
  row[1] = static_cast<float>(cy / stride - gy);
  row[2] = static_cast<float>(std::log(width / stride));
  row[3] = static_cast<float>(std::log(height / stride));
  row[4] = objectness;
  row[5 + cls] = classScore;
}

const Json& items(const test::OpCall& call) {
  const Record* result = call.outputs.at("detections").asRecord();
  REQUIRE(result != nullptr);
  std::string why;
  REQUIRE_MESSAGE(poses::validate(*result, why), why);
  return result->data["items"];
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

TEST_CASE(
    "YOLOX decoding uses class-aware stable NMS original coordinates and valid empty results") {
  test::OpCall call;
  Tensor tensor = prediction();
  box(tensor, 0, 100, 100, 80, 40);
  box(tensor, 1, 100, 100, 80, 40, 0.8f);
  box(tensor, 2, 100, 100, 80, 40, 0.9f, 15);
  box(tensor, 2704, 300, 300, 40, 60, 0.5f, 2, 0.5f);
  call.inputs = {{"prediction", Data::tensor(tensor)}, {"mapping", Data::record(mapping())}};
  REQUIRE(call.run("image.yolox_decode").ok);
  REQUIRE(items(call).size() == 2);
  CHECK(items(call)[0]["id"] == "det-0-0");
  CHECK(items(call)[1]["classLabel"] == "cat");
  CHECK(items(call)[0]["position"][0].get<double>() == doctest::Approx(100));
  CHECK(items(call)[0]["size"][0].get<double>() == doctest::Approx(80).epsilon(1e-6));
  CHECK(items(call)[0]["pickable"] == false);
  REQUIRE(call.run("image.yolox_decode", {{"classAgnostic", Value::boolean(true)}}).ok);
  CHECK(items(call).size() == 1);
  REQUIRE(call.run("image.yolox_decode", {{"scoreThreshold", Value::number(0.25)}}).ok);
  CHECK(items(call).size() == 3);
  REQUIRE(call.run("image.yolox_decode", {{"maxCandidates", Value::integer(1)}}).ok);
  CHECK(items(call).size() == 1);
  Record meta = mapping();
  meta.data["crop"] = {40, 20, 200, 100};
  meta.data["resizedSize"] = {416, 208};
  meta.data["imageScale"] = 2;
  tensor = prediction();
  box(tensor, 3548, 208, 104, 208, 104);
  call.inputs = {{"prediction", Data::tensor(tensor)}, {"mapping", Data::record(meta)}};
  REQUIRE(call.run("image.yolox_decode").ok);
  REQUIRE(items(call).size() == 1);
  CHECK(items(call)[0]["position"][0].get<double>() == doctest::Approx(280));
  CHECK(items(call)[0]["position"][1].get<double>() == doctest::Approx(140));
  CHECK(items(call)[0]["size"][0].get<double>() == doctest::Approx(200).epsilon(1e-6));
  CHECK(items(call)[0]["size"][1].get<double>() == doctest::Approx(100).epsilon(1e-6));
  box(tensor, 3548, -1000, -1000, 5, 5);
  call.inputs["prediction"] = Data::tensor(tensor);
  REQUIRE(call.run("image.yolox_decode").ok);
  CHECK(items(call).empty());
  call.inputs["prediction"] = Data::tensor(prediction());
  REQUIRE(call.run("image.yolox_decode", {{"scoreThreshold", Value::number(0)}}).ok);
  CHECK(items(call).empty());
}

TEST_CASE("YOLOX NMS uses unclipped model boxes before ROI and preview restoration") {
  test::OpCall call;
  Record meta = mapping();
  meta.data["crop"] = {40, 20, 200, 100};
  meta.data["resizedSize"] = {416, 208};
  meta.data["imageScale"] = 2;
  Tensor tensor = prediction();
  box(tensor, 0, -5.2, 10.4, 52, 20.8, 0.9f);
  box(tensor, 1, 10.4, 10.4, 20.8, 20.8, 0.8f);
  box(tensor, 2, -52, 10.4, 20.8, 20.8, 0.7f);
  call.inputs = {{"prediction", Data::tensor(tensor)}, {"mapping", Data::record(meta)}};
  REQUIRE(call.run("image.yolox_decode", {{"iouThreshold", Value::number(0.45)}}).ok);
  REQUIRE(items(call).size() == 2);
  CHECK(items(call)[0]["id"] == "det-0-0");
  CHECK(items(call)[1]["id"] == "det-1-0");
  for (const auto& item : items(call)) {
    CHECK(item["position"][0].get<double>() == doctest::Approx(90));
    CHECK(item["position"][1].get<double>() == doctest::Approx(50));
    CHECK(item["size"][0].get<double>() == doctest::Approx(20));
    CHECK(item["size"][1].get<double>() == doctest::Approx(20));
  }
  REQUIRE(call.run("image.yolox_decode", {{"iouThreshold", Value::number(0.3)}}).ok);
  REQUIRE(items(call).size() == 1);
  CHECK(items(call)[0]["id"] == "det-0-0");
}

TEST_CASE("YOLOX decoding rejects corrupt model tensors metadata and probability contracts") {
  test::OpCall call;
  Tensor tensor = prediction();
  call.inputs = {{"prediction", Data::tensor(tensor)}, {"mapping", Data::record(mapping())}};
  for (int field : {0, 4, 84}) {
    tensor.data[field] = std::numeric_limits<float>::quiet_NaN();
    call.inputs["prediction"] = Data::tensor(tensor);
    CHECK_FALSE(call.run("image.yolox_decode").ok);
    CHECK(call.outputs.empty());
    tensor.data[field] = 0;
  }
  tensor.data[4] = 1.1f;
  call.inputs["prediction"] = Data::tensor(tensor);
  CHECK_FALSE(call.run("image.yolox_decode").ok);
  tensor = prediction();
  tensor.shape = {1, 85, 3549};
  call.inputs["prediction"] = Data::tensor(tensor);
  CHECK_FALSE(call.run("image.yolox_decode").ok);
  call.inputs["prediction"] = Data::tensor(prediction());
  for (const auto& change : std::vector<Json>{{{"resizedSize", {416, 415}}},
                                              {{"model", "arbitrary-yolo"}},
                                              {{"crop", {-1, 0, 416, 416}}},
                                              {{"imageScale", 0}},
                                              {{"frame", 1}},
                                              {{"extra", true}}}) {
    Record bad = mapping();
    bad.data.update(change);
    call.inputs["mapping"] = Data::record(bad);
    CHECK_FALSE(call.run("image.yolox_decode").ok);
    CHECK(call.outputs.empty());
  }
  call.inputs["mapping"] = Data::record(mapping());
  for (double threshold : {-0.1, 1.1, std::numeric_limits<double>::infinity()})
    CHECK_FALSE(call.run("image.yolox_decode", {{"scoreThreshold", Value::number(threshold)}}).ok);
}
