#include <doctest/doctest.h>

#include <chrono>
#include <fstream>

#include "helpers.h"

namespace {
using namespace lyflow;
using Json = nlohmann::json;

struct RecognitionInput {
  Data rec, layout;
};

RecognitionInput recognitionInput(bool empty = false) {
  Tensor input;
  input.shape = {empty ? 0 : 2, 3, 48, 320};
  Json items = Json::array();
  if (!empty) {
    input.data.resize(2 * 3 * 48 * 320);
    for (int i = 0; i < 2; ++i) {
      for (int c = 0; c < 3; ++c) {
        input.data[(3 * i + c) * 48 * 320] = .2f;
        input.data[(3 * i + c) * 48 * 320 + 47 * 320 + 4] = -.8f;
      }
      items.push_back({{"id", i == 0 ? "perspective" : "vertical"},
                       {"validWidth", 5},
                       {"detectorScore", .9},
                       {"rotated90", i == 1},
                       {"orientation", 0},
                       {"classifierLabel", 0},
                       {"orientationConfidence", 0}});
    }
  }
  Record layout{"image.OCRCrops.v1",
                {{"frame", "camera-image"},
                 {"unit", "px"},
                 {"geometryHash", "sample"},
                 {"channelOrder", "bgr"},
                 {"imageSize", {160, 128}},
                 {"recHeight", 48},
                 {"recWidth", 320},
                 {"clsHeight", 48},
                 {"clsWidth", 192},
                 {"items", std::move(items)}}};
  return {Data::tensor(std::move(input)), Data::record(std::move(layout))};
}

struct DictionaryFile {
  std::filesystem::path path;
  DictionaryFile() {
    path = std::filesystem::temp_directory_path() /
           ("lyflow-ocr-" +
            std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count()) +
            ".txt");
    write("A\n中\n🙂\n");
  }
  void write(const std::string& content) {
    std::ofstream file(path, std::ios::binary);
    file << content;
  }
  ~DictionaryFile() {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
};
Tensor ctcTruth() {
  Tensor tensor;
  tensor.shape = {2, 7, 5};
  tensor.data.resize(70);
  const int truth[2][7]{{1, 1, 0, 2, 2, 3, 0}, {2, 0, 2, 4, 1, 0, 0}};
  for (int n = 0; n < 2; ++n)
    for (int t = 0; t < 7; ++t)
      tensor.data[(n * 7 + t) * 5 + truth[n][t]] = 1;
  return tensor;
}
} // namespace

TEST_CASE("P4 OCR direction flips only valid pixels and rejects repeated orientation") {
  const auto fixture = recognitionInput();
  const auto* input = fixture.rec.asTensor();
  REQUIRE(input != nullptr);
  const auto& layout = fixture.layout.asRecord()->data;
  const std::size_t plane = 48 * 320;
  const auto before = input->data;
  Tensor probabilities;
  probabilities.shape = {2, 2};
  probabilities.data = {.99f, .01f, .01f, .99f};
  test::OpCall call;
  call.inputs = {{"recInput", fixture.rec},
                 {"classification", Data::tensor(probabilities)},
                 {"layout", fixture.layout}};
  REQUIRE_MESSAGE(call.run("image.ocr_orient").ok, call.status.message);
  const auto& output = call.outputs.at("input").asTensor()->data;
  const int validWidth = layout["items"][1]["validWidth"];
  for (int c = 0; c < 3; ++c) {
    CHECK(output[c * plane] == before[c * plane]);
    CHECK(output[(3 + c) * plane] == before[(3 + c) * plane + 47 * 320 + validWidth - 1]);
    CHECK(output[(3 + c) * plane + validWidth] == 0);
  }
  CHECK(fixture.rec.asTensor()->data == before);
  CHECK(call.outputs.at("layout").asRecord()->data["items"][1]["orientation"] == 180);
  call.inputs["layout"] = call.outputs.at("layout");
  CHECK(call.run("image.ocr_orient").code == "bad_input");
  CHECK(call.outputs.empty());
}

TEST_CASE(
    "P4 CTC uses Unicode dictionary classes blank repeat and explicit axis probability contracts") {
  const auto fixture = recognitionInput();
  DictionaryFile dictionary;
  ParamMap parameters{{"dictionaryPath", Value::text(dictionary.path.u8string())}};
  auto prediction = ctcTruth();
  test::OpCall call;
  call.inputs = {{"prediction", Data::tensor(prediction)}, {"layout", fixture.layout}};
  REQUIRE_MESSAGE(call.run("image.ocr_ctc_decode", parameters).ok, call.status.message);
  CHECK(call.outputs.at("text").asRecord()->data["value"] == "A中🙂\n中中 A");
  const auto& items = call.outputs.at("results").asRecord()->data["items"];
  REQUIRE(items.size() == 2);
  CHECK(items[0]["id"] == "perspective");
  CHECK(items[0]["confidence"] == 1);
  Tensor transposed;
  transposed.shape = {7, 2, 5};
  transposed.data.resize(70);
  for (int n = 0; n < 2; ++n)
    for (int t = 0; t < 7; ++t)
      for (int c = 0; c < 5; ++c)
        transposed.data[(t * 2 + n) * 5 + c] = prediction.data[(n * 7 + t) * 5 + c] * 4;
  call.inputs["prediction"] = Data::tensor(transposed);
  parameters["axisOrder"] = Value::text("TNC");
  parameters["outputMode"] = Value::text("logits");
  REQUIRE(call.run("image.ocr_ctc_decode", parameters).ok);
  CHECK(call.outputs.at("text").asRecord()->data["value"] == "A中🙂\n中中 A");
  parameters["minTextScore"] = Value::number(.99);
  REQUIRE(call.run("image.ocr_ctc_decode", parameters).ok);
  CHECK(call.outputs.at("text").asRecord()->data["value"] == "");
  CHECK(call.outputs.at("results").asRecord()->data["items"][0]["accepted"] == false);
  CHECK(call.outputs.at("results").asRecord()->data["items"][0]["rawText"] == "A中🙂");
  dictionary.write("A\nA\n");
  CHECK(call.run("image.ocr_ctc_decode", parameters).code == "bad_param");
  CHECK(call.outputs.empty());
  dictionary.write(std::string("\xc0\xaf\n"));
  CHECK(call.run("image.ocr_ctc_decode", parameters).code == "bad_param");
  CHECK(call.outputs.empty());
  dictionary.write("中Ab1🙂\n");
  Tensor mixed;
  mixed.shape = {2, 1, 3};
  mixed.data = {0, 1, 0, 0, 1, 0};
  call.inputs["prediction"] = Data::tensor(mixed);
  for (const auto& category :
       std::vector<std::pair<std::string, std::string>>{{"all", "中Ab1🙂"},
                                                        {"digits", "1"},
                                                        {"lowercase", "b1"},
                                                        {"uppercase", "A1"},
                                                        {"letters", "Ab1"}}) {
    CAPTURE(category.first);
    REQUIRE(call.run("image.ocr_ctc_decode",
                     {{"dictionaryPath", Value::text(dictionary.path.u8string())},
                      {"characterSet", Value::text(category.first)}})
                .ok);
    CHECK(call.outputs.at("text").asRecord()->data["value"] ==
          category.second + "\n" + category.second);
    CHECK(call.outputs.at("results").asRecord()->data["items"][0]["rawText"] == "中Ab1🙂");
  }
}

TEST_CASE("P4 OCR direction and CTC preserve empty batches") {
  const auto empty = recognitionInput(true);
  DictionaryFile dictionary;
  Tensor canonical;
  canonical.shape = {0};
  test::OpCall direction;
  direction.inputs = {{"recInput", empty.rec},
                      {"classification", Data::tensor(canonical)},
                      {"layout", empty.layout}};
  REQUIRE(direction.run("image.ocr_orient").ok);
  test::OpCall decode;
  decode.inputs = {{"prediction", Data::tensor(canonical)},
                   {"layout", direction.outputs.at("layout")}};
  REQUIRE(decode
              .run("image.ocr_ctc_decode",
                   {{"dictionaryPath", Value::text(dictionary.path.u8string())}})
              .ok);
  CHECK(decode.outputs.at("text").asRecord()->data["value"] == "");
  CHECK(decode.outputs.at("results").asRecord()->data["items"].empty());
}
