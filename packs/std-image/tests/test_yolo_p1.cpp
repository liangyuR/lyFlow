#include <doctest/doctest.h>

#include <cmath>
#include <limits>

#include "helpers.h"
#include "lyflow/pose.h"

namespace {
using namespace lyflow;
using Json = nlohmann::json;

Record prepare(const std::string& family, const std::string& format = "auto") {
  test::OpCall call;
  Image image = Image::allocate(32, 16, 3, PixelDepth::U8);
  image.scale = 2;
  call.inputs = {{"image", Data::image(std::move(image))}};
  REQUIRE(
      call.run("image.yolo_preprocess", {{"inputWidth", Value::integer(32)},
                                         {"inputHeight", Value::integer(32)},
                                         {"modelFamily", Value::text(family)},
                                         {"outputFormat", Value::text(format)},
                                         {"classNames", Value::text("[\"零件甲\",\"零件乙\"]")}})
          .ok);
  return *call.outputs.at("mapping").asRecord();
}

Tensor raw() {
  Tensor tensor;
  tensor.shape = {1, 6, 21};
  tensor.data.resize(6 * 21);
  return tensor;
}

void box(Tensor& tensor, int row, int cls, float score) {
  tensor.data[row] = 16;
  tensor.data[21 + row] = 16;
  tensor.data[42 + row] = 16;
  tensor.data[63 + row] = 8;
  tensor.data[(4 + cls) * 21 + row] = score;
}

const Json& items(const test::OpCall& call) {
  const Record* record = call.outputs.at("detections").asRecord();
  REQUIRE(record != nullptr);
  std::string why;
  REQUIRE_MESSAGE(poses::validate(*record, why), why);
  return record->data["items"];
}
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

TEST_CASE(
    "YOLO raw heads decode custom classes without objectness and restore preview coordinates") {
  for (const auto& family : {"yolov8", "yolo11", "yolo26"}) {
    test::OpCall call;
    Tensor tensor = raw();
    box(tensor, 0, 1, 0.8f);
    box(tensor, 1, 1, 0.7f);
    box(tensor, 2, 0, 0.9f);
    call.inputs = {{"mapping", Data::record(prepare(family, "raw"))},
                   {"prediction", Data::tensor(tensor)}};
    REQUIRE(call.run("image.yolo_decode").ok);
    REQUIRE(items(call).size() == 2);
    CHECK(items(call)[0]["classLabel"] == "零件甲");
    CHECK(items(call)[1]["classLabel"] == "零件乙");
    CHECK(items(call)[0]["confidence"].get<double>() == doctest::Approx(0.9));
    CHECK(items(call)[0]["position"] == Json::array({32.0, 16.0}));
    CHECK(items(call)[0]["size"] == Json::array({32.0, 16.0}));
    REQUIRE(call.run("image.yolo_decode", {{"classAgnostic", Value::boolean(true)}}).ok);
    CHECK(items(call).size() == 1);
    REQUIRE(call.run("image.yolo_decode",
                     {{"scoreThreshold", Value::number(static_cast<double>(0.9f))}})
                .ok);
    CHECK(items(call).size() == 1);
    tensor = raw();
    call.inputs["prediction"] = Data::tensor(tensor);
    REQUIRE(call.run("image.yolo_decode").ok);
    CHECK(items(call).empty());
  }
}

TEST_CASE("YOLO26 end-to-end heads retain model-selected overlaps and validate class indices") {
  test::OpCall call;
  Tensor tensor;
  tensor.shape = {1, 3, 6};
  tensor.data = {8, 12, 24, 20, 0.9f, 0, 8, 12, 24, 20, 0.8f, 0, 0, 0, 0, 0, 0, 0};
  call.inputs = {{"mapping", Data::record(prepare("yolo26"))},
                 {"prediction", Data::tensor(tensor)}};
  REQUIRE(call.run("image.yolo_decode", {{"classAgnostic", Value::boolean(true)}}).ok);
  REQUIRE(items(call).size() == 2);
  CHECK(items(call)[0]["position"] == Json::array({32.0, 16.0}));
  CHECK(items(call)[0]["size"] == Json::array({32.0, 16.0}));
  for (float bad : {-1.0f, 0.5f, 2.0f, std::numeric_limits<float>::infinity()}) {
    tensor.data[5] = bad;
    call.inputs["prediction"] = Data::tensor(tensor);
    CHECK_FALSE(call.run("image.yolo_decode").ok);
    CHECK(call.outputs.empty());
  }
  tensor.data[5] = 0;
  tensor.data[4] = 1.1f;
  call.inputs["prediction"] = Data::tensor(tensor);
  CHECK_FALSE(call.run("image.yolo_decode").ok);
}

TEST_CASE("YOLO raw NMS preserves model-space IoU before clipping preview results") {
  for (const auto& family : {"yolov8", "yolo11", "yolo26"}) {
    test::OpCall call;
    Tensor tensor = raw();
    for (int row = 0; row < 4; ++row) {
      box(tensor, row, 0, 0.9f - row * 0.1f);
      tensor.data[21 + row] = 13;
      tensor.data[63 + row] = 10;
    }
    tensor.data[0] = -2.5f;
    tensor.data[42] = 25;
    tensor.data[1] = 5;
    tensor.data[43] = 10;
    tensor.data[2] = -25;
    tensor.data[44] = 10;
    tensor.data[3] = 5;
    tensor.data[45] = 0;
    call.inputs = {{"mapping", Data::record(prepare(family, "raw"))},
                   {"prediction", Data::tensor(tensor)}};
    REQUIRE(call.run("image.yolo_decode", {{"iouThreshold", Value::number(0.45)}}).ok);
    REQUIRE(items(call).size() == 2);
    CHECK(items(call)[0]["id"] == "det-0-0");
    CHECK(items(call)[1]["id"] == "det-1-0");
    for (const auto& item : items(call)) {
      CHECK(item["position"] == Json::array({10.0, 10.0}));
      CHECK(item["size"] == Json::array({20.0, 20.0}));
    }
    REQUIRE(call.run("image.yolo_decode", {{"iouThreshold", Value::number(0.3)}}).ok);
    REQUIRE(items(call).size() == 1);
    CHECK(items(call)[0]["id"] == "det-0-0");
  }
}

TEST_CASE("YOLO excludes padding-only boxes before candidate and detection budgets") {
  for (const auto& family : {"yolov8", "yolo11", "yolo26"}) {
    for (const auto& format : {"raw", "end2end"}) {
      test::OpCall call;
      Tensor tensor;
      if (std::string(format) == "raw") {
        tensor = raw();
        box(tensor, 0, 0, 0.9f);
        box(tensor, 1, 0, 0.8f);
        tensor.data[0] = tensor.data[1] = 5;
        tensor.data[21] = 2;
        tensor.data[22] = 15;
        tensor.data[42] = tensor.data[43] = 10;
        tensor.data[63] = 4;
        tensor.data[64] = 10;
      } else {
        tensor.shape = {1, 2, 6};
        tensor.data = {0, 0, 10, 4, 0.9f, 0, 0, 10, 10, 20, 0.8f, 0};
      }
      call.inputs = {{"mapping", Data::record(prepare(family, format))},
                     {"prediction", Data::tensor(tensor)}};
      for (const auto& budget : {"maxCandidates", "maxDetections"}) {
        REQUIRE(call.run("image.yolo_decode", {{budget, Value::integer(1)}}).ok);
        REQUIRE(items(call).size() == 1);
        CHECK(items(call)[0]["id"] == "det-1-0");
        CHECK(items(call)[0]["position"] == Json::array({10.0, 14.0}));
        CHECK(items(call)[0]["size"] == Json::array({20.0, 20.0}));
      }
    }
  }
}

TEST_CASE("YOLO rejects malformed mapping labels dimensions and unsupported tensor layouts") {
  test::OpCall call;
  const Record correct = prepare("yolov8");
  call.inputs = {{"mapping", Data::record(correct)}, {"prediction", Data::tensor(raw())}};
  for (const auto& change : std::vector<Json>{{{"padding", {0, 0}}},
                                              {{"modelFamily", "yolo27"}},
                                              {{"inputSize", {31, 32}}},
                                              {{"classNames", {""}}},
                                              {{"frame", 2}},
                                              {{"imageScale", 0}},
                                              {{"crop", {-1, 0, 32, 16}}}}) {
    Record bad = correct;
    bad.data.update(change);
    call.inputs["mapping"] = Data::record(bad);
    CHECK_FALSE(call.run("image.yolo_decode").ok);
    CHECK(call.outputs.empty());
  }
  call.inputs["mapping"] = Data::record(correct);
  Tensor wrong = raw();
  wrong.shape = {1, 21, 6};
  call.inputs["prediction"] = Data::tensor(wrong);
  CHECK_FALSE(call.run("image.yolo_decode").ok);
  wrong = raw();
  wrong.data[0] = std::numeric_limits<float>::quiet_NaN();
  call.inputs["prediction"] = Data::tensor(wrong);
  CHECK_FALSE(call.run("image.yolo_decode").ok);
}
