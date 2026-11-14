#include <doctest/doctest.h>

#include <cstring>
#include <limits>

#include <opencv2/imgproc.hpp>

#include "helpers.h"
#include "lyflow/pose.h"
#include "lyflow_cv/adapter.h"

namespace {
using namespace lyflow;
using Json = nlohmann::json;

Data segmentationImage(cv::Mat matrix, int scale = 1) {
  Image image;
  REQUIRE(cvx::fromMat(std::move(matrix), image));
  image.scale = scale;
  return Data::image(std::move(image));
}
ParamMap segmentationParameters(int width, int height) {
  return {{"modelContract", Value::text("semantic_nchw")},
          {"inputWidth", Value::integer(width)},
          {"inputHeight", Value::integer(height)},
          {"bboxDilation", Value::number(1)}};
}
Record segmentationMapping(const Data& image, int width, int height, const Record* rois = nullptr,
                           ParamMap params = {}) {
  test::OpCall call;
  call.inputs["image"] = image;
  if (rois)
    call.inputs["rois"] = Data::record(*rois);
  auto configured = segmentationParameters(width, height);
  for (auto& entry : params)
    configured[entry.first] = entry.second;
  REQUIRE_MESSAGE(call.run("image.segmentation_preprocess", configured).ok, call.status.message);
  return *call.outputs.at("mapping").asRecord();
}
Tensor segmentationOutput(int count, int height, int width, float foreground = .9f) {
  Tensor tensor;
  tensor.shape = {count, 2, height, width};
  const auto plane = static_cast<std::size_t>(height) * width;
  tensor.data.resize(count * 2 * plane);
  for (int n = 0; n < count; ++n) {
    std::fill_n(tensor.data.begin() + n * 2 * plane, plane, 1 - foreground);
    std::fill_n(tensor.data.begin() + (n * 2 + 1) * plane, plane, foreground);
  }
  return tensor;
}
struct SegmentCancelled final : ExecContext {
  std::filesystem::path base;
  bool cancelled() const override { return true; }
  void progress(float, std::string_view) override {}
  void log(LogLevel, std::string) override {}
  const std::filesystem::path& baseDir() const override { return base; }
  int threadBudget() const override { return 1; }
};
struct SegmentCancelLater final : ExecContext {
  mutable int checks = 0;
  std::filesystem::path base;
  bool cancelled() const override { return ++checks >= 6; }
  void progress(float, std::string_view) override {}
  void log(LogLevel, std::string) override {}
  const std::filesystem::path& baseDir() const override { return base; }
  int threadBudget() const override { return 1; }
};
struct ExistingSegment {
  Data image;
  Record rois, info;
  Tensor masks;
  ExistingSegment() {
    image = segmentationImage(cv::Mat(4, 4, CV_8UC4, cv::Scalar(10, 20, 30, 19)));
    auto a = poses::make2D("kept-A", "image", "px", {1, 1}, 0, {2, 2});
    a.data["classId"] = "1";
    a.data["classLabel"] = "person";
    a.data["confidence"] = .73;
    a.data["pickable"] = true;
    a.data["attributes"] = {{"hasMask", true}};
    auto b = poses::make2D("missing-B", "image", "px", {3, 3}, 0, {2, 2});
    b.data["attributes"] = {{"hasMask", false}};
    rois = poses::make2DList(Json::array({a.data, b.data}));
    info = {"vision.Masks2D.v1",
            {{"frame", "image"},
             {"unit", "px"},
             {"width", 4},
             {"height", 4},
             {"layout", "NHW"},
             {"items",
              Json::array(
                  {{{"id", "kept-A"},
                    {"index", 0},
                    {"hasMask", true},
                    {"source", "annotation_polygon"}},
                   {{"id", "missing-B"}, {"index", 1}, {"hasMask", false}, {"source", "none"}}})}}};
    masks.shape = {2, 4, 4};
    masks.data.assign(32, 0);
    masks.data[0] = masks.data[5] = 1;
  }
  void inputs(test::OpCall& call) const {
    call.inputs = {{"image", image},
                   {"rois", Data::record(rois)},
                   {"roiMasks", Data::tensor(masks)},
                   {"roiMaskInfo", Data::record(info)}};
  }
};
void retainedInputs(test::OpCall& post, const test::OpCall& pre) {
  for (const char* port : {"retainedInstances", "retainedMasks", "retainedMaskInfo"})
    post.inputs[port] = pre.outputs.at(port);
}
}

TEST_CASE(
    "P4 segmentation keep mode infers only missing ROIs and preserves native support and metadata") {
  ExistingSegment fixture;
  test::OpCall pre;
  fixture.inputs(pre);
  auto params = segmentationParameters(2, 2);
  params["replaceExistingMask"] = Value::boolean(false);
  REQUIRE_MESSAGE(pre.run("image.segmentation_preprocess", params).ok, pre.status.message);
  CHECK(pre.outputs.at("tensor").asTensor()->shape == std::vector<std::int64_t>{1, 3, 2, 2});
  CHECK(pre.outputs.at("mapping").asRecord()->data["rois"][0]["id"] == "missing-B");
  CHECK(pre.outputs.at("retainedMasks").asTensor()->shape == std::vector<std::int64_t>{1, 4, 4});
  test::OpCall post;
  post.inputs = {{"image", fixture.image},
                 {"mapping", pre.outputs.at("mapping")},
                 {"output", Data::tensor(segmentationOutput(1, 2, 2))}};
  retainedInputs(post, pre);
  REQUIRE_MESSAGE(post.run("image.segmentation_decode").ok, post.status.message);
  const auto& poses = post.outputs.at("instances").asRecord()->data["items"];
  const auto& info = post.outputs.at("maskInfo").asRecord()->data["items"];
  REQUIRE(poses.size() == 2);
  CHECK(poses[0]["id"] == "kept-A");
  CHECK(poses[0]["classId"] == "1");
  CHECK(poses[0]["classLabel"] == "person");
  CHECK(poses[0]["confidence"] == .73);
  CHECK(poses[0]["position"] == Json::array({1., 1.}));
  CHECK(poses[0]["size"] == Json::array({2., 2.}));
  CHECK(poses[0]["pickable"] == false);
  CHECK(poses[1]["attributes"]["roiId"] == "missing-B");
  for (int n = 0; n < 2; ++n) {
    CHECK(info[n]["id"] == poses[n]["id"]);
    CHECK(info[n]["index"] == n);
    CHECK(info[n]["hasMask"] == true);
  }
  CHECK(info[0]["source"] == "annotation_polygon");
  CHECK(info[1]["source"] == "semantic_connected_component");
  const auto& masks = *post.outputs.at("masks").asTensor();
  CHECK(masks.shape == std::vector<std::int64_t>{2, 4, 4});
  CHECK(std::equal(masks.data.begin(), masks.data.begin() + 16, fixture.masks.data.begin()));
  const auto labels = cvx::view(*post.outputs.at("labels").asImage());
  CHECK(labels.at<std::uint16_t>(0, 0) == 1);
  CHECK(labels.at<std::uint16_t>(0, 1) == 65535);
  CHECK(labels.at<std::uint16_t>(3, 3) == 1);
  CHECK(post.outputs.at("quality").asRecord()->data["retainedCount"] == 1);
  CHECK(post.outputs.at("quality").asRecord()->data["inferredInstanceCount"] == 1);
  CHECK(fixture.rois.data["items"][0]["pickable"] == true);
  CHECK(fixture.masks.data[0] == 1);
  CHECK(fixture.masks.data[5] == 1);
  CHECK(cvx::view(*post.outputs.at("overlay").asImage()).at<cv::Vec4b>(3, 3)[3] == 19);
}

TEST_CASE(
    "P4 segmentation all retained keeps IDs with zero inference and replace mode recomputes all") {
  ExistingSegment fixture;
  fixture.info.data["items"][1]["hasMask"] = true;
  fixture.info.data["items"][1]["source"] = "annotation_polygon";
  fixture.rois.data["items"][1]["attributes"]["hasMask"] = true;
  fixture.rois.data["items"][1]["classId"] = "private";
  fixture.rois.data["items"][1]["classLabel"] = "专有类别";
  fixture.masks.data[31] = 1;
  test::OpCall pre;
  fixture.inputs(pre);
  auto params = segmentationParameters(2, 2);
  params["replaceExistingMask"] = Value::boolean(false);
  REQUIRE_MESSAGE(pre.run("image.segmentation_preprocess", params).ok, pre.status.message);
  CHECK(pre.outputs.at("tensor").asTensor()->shape == std::vector<std::int64_t>{0, 3, 2, 2});
  CHECK(pre.outputs.at("tensor").asTensor()->data.empty());
  Tensor empty;
  empty.shape = {0};
  test::OpCall post;
  post.inputs = {{"image", fixture.image},
                 {"mapping", pre.outputs.at("mapping")},
                 {"output", Data::tensor(empty)}};
  retainedInputs(post, pre);
  REQUIRE_MESSAGE(post.run("image.segmentation_decode").ok, post.status.message);
  CHECK(post.outputs.at("masks").asTensor()->data == fixture.masks.data);
  CHECK(post.outputs.at("instances").asRecord()->data["items"][1]["classLabel"] == "专有类别");
  CHECK(post.outputs.at("quality").asRecord()->data["retainedUnmappedClasses"] == 1);
  CHECK(post.outputs.at("quality").asRecord()->data["inferredInstanceCount"] == 0);
  CHECK(cvx::view(*post.outputs.at("labels").asImage()).at<std::uint16_t>(3, 3) == 65535);
  CHECK(cvx::view(*post.outputs.at("overlay").asImage()).at<cv::Vec4b>(3, 3) !=
        cv::Vec4b(10, 20, 30, 19));
  fixture.rois.data["items"][0]["classLabel"] = "连接件";
  fixture.inputs(pre);
  REQUIRE(pre.run("image.segmentation_preprocess", params).ok);
  post.inputs = {{"image", fixture.image},
                 {"mapping", pre.outputs.at("mapping")},
                 {"output", Data::tensor(empty)}};
  retainedInputs(post, pre);
  REQUIRE(post.run("image.segmentation_decode").ok);
  CHECK(post.outputs.at("quality").asRecord()->data["retainedUnmappedClasses"] == 2);
  CHECK(cvx::view(*post.outputs.at("labels").asImage()).at<std::uint16_t>(0, 0) == 65535);
  CHECK(post.outputs.at("instances").asRecord()->data["items"][0]["classId"] == "1");
  CHECK(post.outputs.at("instances").asRecord()->data["items"][0]["classLabel"] == "连接件");
  params["replaceExistingMask"] = Value::boolean(true);
  REQUIRE(pre.run("image.segmentation_preprocess", params).ok);
  CHECK(pre.outputs.at("tensor").asTensor()->shape == std::vector<std::int64_t>{2, 3, 2, 2});
  CHECK(pre.outputs.at("retainedMasks").asTensor()->shape == std::vector<std::int64_t>{0, 4, 4});
  post.inputs = {{"image", fixture.image},
                 {"mapping", pre.outputs.at("mapping")},
                 {"output", Data::tensor(segmentationOutput(2, 2, 2))}};
  retainedInputs(post, pre);
  REQUIRE(post.run("image.segmentation_decode").ok);
  CHECK(post.outputs.at("quality").asRecord()->data["retainedCount"] == 0);
  CHECK(post.outputs.at("instances").asRecord()->data["items"][0]["id"] != "kept-A");
}

TEST_CASE("P4 segmentation rejects falsely claimed or incorrectly bound existing mask triplets") {
  ExistingSegment fixture;
  auto params = segmentationParameters(2, 2);
  params["replaceExistingMask"] = Value::boolean(false);
  test::OpCall pre;
  const auto rejected = [&](const ExistingSegment& value) {
    value.inputs(pre);
    CHECK_FALSE(pre.run("image.segmentation_preprocess", params).ok);
    CHECK(pre.outputs.empty());
  };
  auto changed = fixture;
  changed.masks.data[0] = changed.masks.data[5] = 0;
  rejected(changed);
  changed = fixture;
  changed.masks.data[1] = std::numeric_limits<float>::quiet_NaN();
  rejected(changed);
  changed = fixture;
  changed.masks.data[15] = 1;
  rejected(changed);
  changed = fixture;
  changed.masks.data[31] = 1;
  rejected(changed);
  changed = fixture;
  changed.masks.shape = {1, 8, 4};
  rejected(changed);
  changed = fixture;
  changed.info.data["items"][0]["id"] = "wrong";
  rejected(changed);
  changed = fixture;
  changed.info.data["items"][0]["index"] = 1;
  rejected(changed);
  changed = fixture;
  changed.info.data["items"][0]["source"] = "none";
  rejected(changed);
  changed = fixture;
  changed.info.data["width"] = 5;
  rejected(changed);
  changed = fixture;
  changed.info.data["frame"] = "other";
  rejected(changed);
  changed = fixture;
  changed.rois.data["items"][0]["attributes"]["hasMask"] = false;
  rejected(changed);
  changed = fixture;
  changed.rois.data["items"][0]["attributes"]["maskIndex"] = 1;
  rejected(changed);
  fixture.inputs(pre);
  pre.inputs.erase("roiMasks");
  CHECK_FALSE(pre.run("image.segmentation_preprocess", params).ok);
  CHECK(pre.outputs.empty());
  fixture.inputs(pre);
  pre.inputs.erase("rois");
  CHECK_FALSE(pre.run("image.segmentation_preprocess", params).ok);
  CHECK(pre.outputs.empty());
  fixture.inputs(pre);
  REQUIRE(pre.run("image.segmentation_preprocess", params).ok);
  test::OpCall post;
  post.inputs = {{"image", fixture.image},
                 {"mapping", pre.outputs.at("mapping")},
                 {"output", Data::tensor(segmentationOutput(1, 2, 2))}};
  CHECK_FALSE(post.run("image.segmentation_decode").ok);
  CHECK(post.outputs.empty());
  retainedInputs(post, pre);
  auto masks = *post.inputs.at("retainedMasks").asTensor();
  masks.data[0] = 0;
  masks.data[1] = 1;
  post.inputs["retainedMasks"] = Data::tensor(masks);
  CHECK_FALSE(post.run("image.segmentation_decode").ok);
  CHECK(post.outputs.empty());
  retainedInputs(post, pre);
  auto instances = *post.inputs.at("retainedInstances").asRecord();
  instances.data["items"][0]["confidence"] = .72;
  post.inputs["retainedInstances"] = Data::record(instances);
  CHECK_FALSE(post.run("image.segmentation_decode").ok);
  CHECK(post.outputs.empty());
  retainedInputs(post, pre);
  post.inputs.erase("retainedMaskInfo");
  CHECK_FALSE(post.run("image.segmentation_decode").ok);
  CHECK(post.outputs.empty());
  params["replaceExistingMask"] = Value::boolean(true);
  rejected(changed);
}

TEST_CASE(
    "P4 segmentation retained ID collision is rejected and pose flags alone never skip inference") {
  ExistingSegment fixture;
  fixture.rois.data["items"][0]["id"] = "semantic-0-1-1";
  fixture.info.data["items"][0]["id"] = "semantic-0-1-1";
  test::OpCall pre;
  fixture.inputs(pre);
  auto params = segmentationParameters(2, 2);
  params["replaceExistingMask"] = Value::boolean(false);
  REQUIRE(pre.run("image.segmentation_preprocess", params).ok);
  test::OpCall post;
  post.inputs = {{"image", fixture.image},
                 {"mapping", pre.outputs.at("mapping")},
                 {"output", Data::tensor(segmentationOutput(1, 2, 2))}};
  retainedInputs(post, pre);
  CHECK_FALSE(post.run("image.segmentation_decode").ok);
  CHECK(post.outputs.empty());
  REQUIRE(post.run("image.segmentation_decode", {{"idPrefix", Value::text("other")}}).ok);
  CHECK(post.outputs.at("instances").asRecord()->data["items"].size() == 2);
  fixture.inputs(pre);
  pre.inputs.erase("roiMasks");
  pre.inputs.erase("roiMaskInfo");
  REQUIRE(pre.run("image.segmentation_preprocess", params).ok);
  CHECK(pre.outputs.at("tensor").asTensor()->shape == std::vector<std::int64_t>{2, 3, 2, 2});
  CHECK(pre.outputs.at("retainedMasks").asTensor()->data.empty());
}

TEST_CASE("P4 segmentation preprocessing preserves readonly RGBA and explicit RGB normalization") {
  cv::Mat matrix(2, 2, CV_8UC4, cv::Scalar(255, 0, 128, 17));
  const auto input = segmentationImage(matrix);
  test::OpCall call;
  call.inputs["image"] = input;
  auto params = segmentationParameters(2, 2);
  params["frame"] = Value::text("相机");
  params["classNames"] = Value::text("[\"背景\",\"人员\"]");
  REQUIRE_MESSAGE(call.run("image.segmentation_preprocess", params).ok, call.status.message);
  const auto& tensor = *call.outputs.at("tensor").asTensor();
  CHECK(tensor.shape == std::vector<std::int64_t>{1, 3, 2, 2});
  CHECK(tensor.data[0] == 1);
  CHECK(tensor.data[4] == -1);
  CHECK(tensor.data[8] == doctest::Approx(128 / 127.5 - 1));
  CHECK(cvx::view(*input.asImage()).at<cv::Vec4b>(0, 0) == cv::Vec4b(255, 0, 128, 17));
  const auto& mapping = call.outputs.at("mapping").asRecord()->data;
  CHECK(mapping["frame"] == "相机");
  CHECK(mapping["classNames"][1] == "人员");
  CHECK(mapping["layout"] == "NCHW");
}

TEST_CASE("P4 segmentation overlapping ROIs keep distinct full image masks and matching pose IDs") {
  const auto input = segmentationImage(cv::Mat(4, 6, CV_8UC4, cv::Scalar(10, 20, 30, 23)));
  Json values = Json::array();
  for (const char* id : {"A", "B"})
    values.push_back(poses::make2D(id, "image", "px", {3, 2}, 0, {2, 2}).data);
  values.push_back(poses::make2D("outside", "image", "px", {-10, -10}, 0, {2, 2}).data);
  const auto rois = poses::make2DList(values);
  auto mapping =
      segmentationMapping(input, 2, 2, &rois, {{"classNames", Value::text("[\"背景\",\"人员\"]")}});
  REQUIRE(mapping.data["rois"].size() == 2);
  CHECK(mapping.data["skippedRoiIds"] == Json::array({"outside"}));
  test::OpCall call;
  call.inputs = {{"image", input},
                 {"mapping", Data::record(mapping)},
                 {"output", Data::tensor(segmentationOutput(2, 2, 2))}};
  REQUIRE_MESSAGE(call.run("image.segmentation_decode").ok, call.status.message);
  const auto& masks = *call.outputs.at("masks").asTensor();
  CHECK(masks.shape == std::vector<std::int64_t>{2, 4, 6});
  for (int n = 0; n < 2; ++n)
    for (int y = 0; y < 4; ++y)
      for (int x = 0; x < 6; ++x)
        CHECK(masks.data[n * 24 + y * 6 + x] == ((x >= 2 && x < 4 && y >= 1 && y < 3) ? 1 : 0));
  const auto labels = cvx::view(*call.outputs.at("labels").asImage());
  CHECK(labels.type() == CV_16UC1);
  CHECK(labels.at<std::uint16_t>(0, 0) == 65535);
  CHECK(labels.at<std::uint16_t>(1, 2) == 1);
  const auto& poses = call.outputs.at("instances").asRecord()->data["items"];
  const auto& ids = call.outputs.at("maskInfo").asRecord()->data["items"];
  REQUIRE(poses.size() == 2);
  for (int n = 0; n < 2; ++n) {
    CHECK(poses[n]["id"] == ids[n]["id"]);
    CHECK(ids[n]["index"] == n);
    CHECK(poses[n]["position"] == Json::array({3., 2.}));
    CHECK(poses[n]["size"] == Json::array({2., 2.}));
    CHECK(poses[n]["classLabel"] == "人员");
    CHECK(poses[n]["pickable"] == false);
    CHECK(poses[n]["attributes"]["hasMask"] == true);
  }
  const auto overlay = cvx::view(*call.outputs.at("overlay").asImage());
  CHECK(overlay.at<cv::Vec4b>(1, 2)[3] == 23);
  CHECK(cvx::view(*input.asImage()).at<cv::Vec4b>(1, 2) == cv::Vec4b(10, 20, 30, 23));
}

TEST_CASE(
    "P4 segmentation connected empty ROI remains empty and OOB cropping uses source coordinates") {
  const auto input = segmentationImage(cv::Mat(4, 6, CV_8U, cv::Scalar(50)));
  auto rois = poses::make2DList(Json::array());
  auto mapping = segmentationMapping(input, 2, 2, &rois);
  REQUIRE(mapping.data["rois"].empty());
  Tensor empty;
  empty.shape = {0};
  test::OpCall call;
  call.inputs = {
      {"image", input}, {"mapping", Data::record(mapping)}, {"output", Data::tensor(empty)}};
  REQUIRE(call.run("image.segmentation_decode").ok);
  CHECK(call.outputs.at("masks").asTensor()->shape == std::vector<std::int64_t>{0, 4, 6});
  CHECK(call.outputs.at("instances").asRecord()->data["items"].empty());
  CHECK(cvx::view(*call.outputs.at("labels").asImage()).at<std::uint16_t>(2, 2) == 65535);
  call.inputs["output"] = Data::tensor(segmentationOutput(1, 2, 2));
  CHECK_FALSE(call.run("image.segmentation_decode").ok);
  CHECK(call.outputs.empty());
  rois = poses::make2DList(
      Json::array({poses::make2D("clip", "image", "px", {0, 2}, 0, {4, 2}).data}));
  mapping = segmentationMapping(input, 2, 2, &rois);
  CHECK(mapping.data["rois"][0]["crop"] == Json::array({0, 1, 2, 2}));
  call.inputs["mapping"] = Data::record(mapping);
  REQUIRE(call.run("image.segmentation_decode").ok);
  CHECK(call.outputs.at("instances").asRecord()->data["items"][0]["position"] ==
        Json::array({1., 2.}));
}

TEST_CASE("P4 semantic binary sigmoid and explicit class mapping keep low confidence unknown") {
  const auto input = segmentationImage(cv::Mat(2, 2, CV_8U, cv::Scalar(50)));
  const auto mapping =
      segmentationMapping(input, 2, 2, nullptr, {{"headKind", Value::text("binary")}});
  Tensor output;
  output.shape = {1, 1, 2, 2};
  output.data = {1000, -1000, 0, 1000};
  test::OpCall call;
  call.inputs = {
      {"image", input}, {"mapping", Data::record(mapping)}, {"output", Data::tensor(output)}};
  REQUIRE_MESSAGE(call.run("image.segmentation_decode", {{"outputKind", Value::text("logits")},
                                                         {"maskThreshold", Value::number(.75)}})
                      .ok,
                  call.status.message);
  const auto labels = cvx::view(*call.outputs.at("labels").asImage());
  CHECK(labels.at<std::uint16_t>(0, 0) == 1);
  CHECK(labels.at<std::uint16_t>(0, 1) == 0);
  CHECK(labels.at<std::uint16_t>(1, 0) == 65535);
  CHECK(labels.at<std::uint16_t>(1, 1) == 1);
  REQUIRE(call.outputs.at("instances").asRecord()->data["items"].size() == 1);
  CHECK(call.outputs.at("instances").asRecord()->data["items"][0]["attributes"]["pixelCount"] == 2);
  CHECK(call.run("image.segmentation_decode",
                 {{"backgroundClass", Value::integer(1)}, {"outputKind", Value::text("logits")}})
            .code == "bad_param");
  CHECK(call.outputs.empty());
  output.data = {.9f, .1f, .5f, .9f};
  call.inputs["output"] = Data::tensor(output);
  REQUIRE(call.run("image.segmentation_decode", {{"maskThreshold", Value::number(.75)}}).ok);
  CHECK(cvx::view(*call.outputs.at("labels").asImage()).at<std::uint16_t>(1, 0) == 65535);
  const auto swapped = segmentationMapping(
      input, 2, 2, nullptr,
      {{"headKind", Value::text("binary")}, {"binaryForegroundClass", Value::integer(0)}});
  call.inputs["mapping"] = Data::record(swapped);
  REQUIRE(call.run("image.segmentation_decode", {{"backgroundClass", Value::integer(1)}}).ok);
  CHECK(cvx::view(*call.outputs.at("labels").asImage()).at<std::uint16_t>(0, 0) == 0);
}

TEST_CASE(
    "P4 semantic multiclass stable softmax preserves class IDs and connected component area") {
  const auto input = segmentationImage(cv::Mat(2, 2, CV_8UC3, cv::Scalar(0, 0, 0)));
  const auto mapping = segmentationMapping(input, 2, 2, nullptr,
                                           {{"classNames", Value::text("[\"背景\",\"A\",\"B\"]")}});
  Tensor output;
  output.shape = {1, 3, 2, 2};
  output.data = {0, 0, 0, 0, 1e30f, -1e30f, -1e30f, -1e30f, -1e30f, 1e30f, -1e30f, -1e30f};
  test::OpCall call;
  call.inputs = {
      {"image", input}, {"mapping", Data::record(mapping)}, {"output", Data::tensor(output)}};
  REQUIRE(call.run("image.segmentation_decode", {{"outputKind", Value::text("logits")}}).ok);
  auto labels = cvx::view(*call.outputs.at("labels").asImage());
  CHECK(labels.at<std::uint16_t>(0, 0) == 1);
  CHECK(labels.at<std::uint16_t>(0, 1) == 2);
  CHECK(labels.at<std::uint16_t>(1, 0) == 0);
  CHECK(call.outputs.at("instances").asRecord()->data["items"].size() == 2);
  REQUIRE(call.run("image.segmentation_decode",
                   {{"outputKind", Value::text("logits")}, {"minArea", Value::integer(2)}})
              .ok);
  CHECK(call.outputs.at("instances").asRecord()->data["items"].empty());
  CHECK(cvx::view(*call.outputs.at("labels").asImage()).at<std::uint16_t>(0, 0) == 1);
}

TEST_CASE(
    "P4 overlapping segmentation aggregation uses probability then stable ROI order without dropping masks") {
  const auto input = segmentationImage(cv::Mat(2, 2, CV_8U, cv::Scalar(50)));
  const auto rois =
      poses::make2DList(Json::array({poses::make2D("A", "image", "px", {1, 1}, 0, {2, 2}).data,
                                     poses::make2D("B", "image", "px", {1, 1}, 0, {2, 2}).data}));
  const auto mapping = segmentationMapping(
      input, 2, 2, &rois, {{"classNames", Value::text("[\"background\",\"one\",\"two\"]")}});
  Tensor output;
  output.shape = {2, 3, 2, 2};
  for (const float probability : {.1f, .8f, .1f, .05f, .05f, .9f})
    output.data.insert(output.data.end(), 4, probability);
  test::OpCall call;
  call.inputs = {
      {"image", input}, {"mapping", Data::record(mapping)}, {"output", Data::tensor(output)}};
  REQUIRE(call.run("image.segmentation_decode").ok);
  CHECK(cvx::view(*call.outputs.at("labels").asImage()).at<std::uint16_t>(0, 0) == 2);
  CHECK(call.outputs.at("masks").asTensor()->shape == std::vector<std::int64_t>{2, 2, 2});
  for (std::size_t c = 0; c < 3; ++c)
    std::fill_n(output.data.begin() + 12 + c * 4, 4, c == 2 ? .8f : .1f);
  call.inputs["output"] = Data::tensor(output);
  REQUIRE(call.run("image.segmentation_decode", {{"idPrefix", Value::text("模型A")}}).ok);
  CHECK(cvx::view(*call.outputs.at("labels").asImage()).at<std::uint16_t>(0, 0) == 1);
  CHECK(call.outputs.at("instances")
            .asRecord()
            ->data["items"][0]["id"]
            .get<std::string>()
            .find("模型A-") == 0);
  CHECK(call.outputs.at("masks").asTensor()->data == std::vector<float>(8, 1));
}

TEST_CASE(
    "P4 segmentation mapping rejects source mismatch schema corruption and incompatible heads atomically") {
  const auto input = segmentationImage(cv::Mat(2, 2, CV_8U, cv::Scalar(50)));
  const auto mapping = segmentationMapping(input, 2, 2);
  test::OpCall call;
  call.inputs = {{"image", input},
                 {"mapping", Data::record(mapping)},
                 {"output", Data::tensor(segmentationOutput(1, 2, 2))}};
  std::vector<Record> malformed;
  for (const char* key : {"frame", "width", "inputSize", "classNames", "rois", "sourceFingerprint",
                          "headKind", "binaryForegroundClass"}) {
    auto bad = mapping;
    bad.data.erase(key);
    malformed.push_back(std::move(bad));
  }
  auto bad = mapping;
  bad.data["extra"] = true;
  malformed.push_back(bad);
  bad = mapping;
  bad.data["width"] = 2.0;
  malformed.push_back(bad);
  bad = mapping;
  bad.data["rois"][0]["crop"] = {0, 0, 3, 2};
  malformed.push_back(bad);
  bad = mapping;
  bad.data["rois"].push_back(bad.data["rois"][0]);
  malformed.push_back(bad);
  bad = mapping;
  bad.data["classNames"] = {"same", "same"};
  malformed.push_back(bad);
  for (const auto& value : malformed) {
    call.inputs["mapping"] = Data::record(value);
    CHECK_FALSE(call.run("image.segmentation_decode").ok);
    CHECK(call.outputs.empty());
  }
  call.inputs["mapping"] = Data::record(mapping);
  call.inputs["image"] = segmentationImage(cv::Mat(2, 2, CV_8U, cv::Scalar(51)));
  CHECK_FALSE(call.run("image.segmentation_decode").ok);
  CHECK(call.outputs.empty());
  call.inputs["image"] = input;
  auto output = segmentationOutput(1, 2, 2);
  output.shape = {1, 2, 1, 4};
  call.inputs["output"] = Data::tensor(output);
  CHECK_FALSE(call.run("image.segmentation_decode").ok);
  CHECK(call.outputs.empty());
  output = segmentationOutput(1, 2, 2);
  output.data[0] = std::numeric_limits<float>::quiet_NaN();
  call.inputs["output"] = Data::tensor(output);
  CHECK_FALSE(call.run("image.segmentation_decode").ok);
  CHECK(call.outputs.empty());
  output.data[0] = .9f;
  call.inputs["output"] = Data::tensor(output);
  CHECK_FALSE(call.run("image.segmentation_decode").ok);
  CHECK(call.outputs.empty());
}

TEST_CASE("P4 PPHumanSeg protocol rejects fake dimensions and duplicate softmax") {
  const auto input = segmentationImage(cv::Mat(2, 2, CV_8UC3, cv::Scalar(0, 0, 0)));
  test::OpCall call;
  call.inputs["image"] = input;
  CHECK(call.run("image.segmentation_preprocess", {{"inputWidth", Value::integer(32)}}).code ==
        "bad_param");
  CHECK(call.outputs.empty());
  REQUIRE(call.run("image.segmentation_preprocess").ok);
  auto mapping = call.outputs.at("mapping");
  call.inputs = {{"image", input},
                 {"mapping", mapping},
                 {"output", Data::tensor(segmentationOutput(1, 192, 192))}};
  CHECK(call.run("image.segmentation_decode", {{"outputKind", Value::text("logits")}}).code ==
        "bad_param");
  CHECK(call.outputs.empty());
  REQUIRE(call.run("image.segmentation_decode").ok);
  CHECK(call.outputs.at("instances").asRecord()->data["items"].size() == 1);
}

TEST_CASE(
    "P4 segmentation cancellation ROI validation and instance budgets never publish partial output") {
  const auto input = segmentationImage(cv::Mat(12, 12, CV_8U, cv::Scalar(50)));
  const auto mapping = segmentationMapping(input, 12, 12);
  auto output = segmentationOutput(1, 12, 12, .1f);
  for (int y = 0; y < 12; y += 3)
    for (int x = 0; x < 12; x += 3) {
      output.data[y * 12 + x] = .1f;
      output.data[144 + y * 12 + x] = .9f;
    }
  test::OpCall call;
  call.inputs = {
      {"image", input}, {"mapping", Data::record(mapping)}, {"output", Data::tensor(output)}};
  CHECK(call.run("image.segmentation_decode", {{"maxInstances", Value::integer(3)}}).code ==
        "resource_limit");
  CHECK(call.outputs.empty());
  SegmentCancelled cancelled;
  const auto* op = ensureRegistry().find("image.segmentation_decode");
  REQUIRE(op);
  ParamMap params;
  for (const auto& parameter : op->params)
    params[parameter.name] = parameter.def;
  std::unordered_map<std::string, Data> outputs;
  Inputs inputs(call.inputs);
  Outputs result(outputs);
  ParamView view(params, cancelled.base);
  CHECK(op->compute(inputs, view, result, cancelled).code == "cancelled");
  CHECK(outputs.empty());
  SegmentCancelLater later;
  outputs.clear();
  CHECK(op->compute(inputs, view, result, later).code == "cancelled");
  CHECK(later.checks >= 6);
  CHECK(outputs.empty());
  auto rois = poses::make2DList(
      Json::array({poses::make2D("rotated", "image", "px", {2, 2}, .1, {2, 2}).data}));
  call.inputs = {{"image", input}, {"rois", Data::record(rois)}};
  CHECK_FALSE(call.run("image.segmentation_preprocess").ok);
  CHECK(call.outputs.empty());
  call.inputs = {{"image", segmentationImage(cv::Mat(2, 2, CV_8U, cv::Scalar(50)), 2)}};
  CHECK_FALSE(call.run("image.segmentation_preprocess").ok);
  CHECK(call.outputs.empty());
}
