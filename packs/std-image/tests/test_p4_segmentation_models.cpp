#if defined(LYFLOW_P4_MODEL_TESTS)
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "helpers.h"
#include "lyflow/pose.h"
#include "lyflow_cv/adapter.h"

namespace {
using namespace lyflow;
namespace fs = std::filesystem;
fs::path segmentationAssets() {
  const auto* value = std::getenv("LYFLOW_P4_ASSETS");
  REQUIRE_MESSAGE((value && *value), "LYFLOW_P4_MODEL_TESTS requires LYFLOW_P4_ASSETS");
  const auto path = fs::u8path(value);
  for (const char* file : {"human_segmentation_pphumanseg_2023mar.onnx", "astronaut.png"})
    REQUIRE_MESSAGE(fs::is_regular_file(path / file), file);
  return path;
}
Data segmentationPhoto(const fs::path& directory) {
  std::ifstream stream(directory / "astronaut.png", std::ios::binary);
  REQUIRE(stream.good());
  const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(stream)),
                                         std::istreambuf_iterator<char>());
  auto bgr = cv::imdecode(bytes, cv::IMREAD_COLOR);
  REQUIRE(!bgr.empty());
  REQUIRE(bgr.cols == 512);
  REQUIRE(bgr.rows == 512);
  cv::Mat rgba;
  cv::cvtColor(bgr, rgba, cv::COLOR_BGR2RGBA);
  rgba.forEach<cv::Vec4b>([](auto& pixel, const int*) { pixel[3] = 37; });
  Image image;
  REQUIRE(cvx::fromMat(std::move(rgba), image));
  return Data::image(std::move(image));
}
struct SegmentationRuns final : ExecContext {
  int completed = 0;
  fs::path base;
  bool cancelled() const override { return false; }
  void progress(float, std::string_view) override { ++completed; }
  void log(LogLevel, std::string) override {}
  const fs::path& baseDir() const override { return base; }
  int threadBudget() const override { return 1; }
};
Data segmentationInfer(const fs::path& directory, const Data& tensor, int expectedRuns) {
  const auto* op = ensureRegistry().find("ml.onnx_run");
  REQUIRE(op);
  ParamMap params;
  for (const auto& parameter : op->params)
    params[parameter.name] = parameter.def;
  params["modelPath"] =
      Value::text((directory / "human_segmentation_pphumanseg_2023mar.onnx").u8string());
  params["modelContract"] = Value::text("pphumanseg_2023mar");
  params["batchMode"] = Value::text("serial");
  params["intraOpThreads"] = Value::integer(1);
  const auto before = tensor.asTensor()->data;
  std::unordered_map<std::string, Data> inputs{{"input", tensor}}, outputs;
  Inputs in(inputs);
  Outputs out(outputs);
  SegmentationRuns context;
  ParamView view(params, context.base);
  const auto status = op->compute(in, view, out, context);
  REQUIRE_MESSAGE(status.ok, status.message);
  CHECK(context.completed == expectedRuns);
  CHECK(tensor.asTensor()->data == before);
  REQUIRE(outputs.count("output") == 1);
  REQUIRE(outputs.at("output").asTensor());
  return outputs.at("output");
}
test::OpCall segmentationDecode(const Data& source, const test::OpCall& pre, const Data& result) {
  test::OpCall post;
  post.inputs = {{"image", source}, {"mapping", pre.outputs.at("mapping")}, {"output", result}};
  for (const char* port : {"retainedInstances", "retainedMasks", "retainedMaskInfo"})
    post.inputs[port] = pre.outputs.at(port);
  REQUIRE_MESSAGE(post.run("image.segmentation_decode", {{"minArea", Value::integer(100)}}).ok,
                  post.status.message);
  return post;
}
void segmentationBindings(const test::OpCall& call) {
  const auto& masks = *call.outputs.at("masks").asTensor();
  const auto& poses = call.outputs.at("instances").asRecord()->data["items"];
  const auto& info = call.outputs.at("maskInfo").asRecord()->data["items"];
  REQUIRE(masks.consistent());
  REQUIRE(masks.shape ==
          std::vector<std::int64_t>{static_cast<std::int64_t>(poses.size()), 512, 512});
  CHECK(std::all_of(masks.data.begin(), masks.data.end(),
                    [](float value) { return value == 0 || value == 1; }));
  REQUIRE(info.size() == poses.size());
  for (std::size_t n = 0; n < poses.size(); ++n) {
    CHECK(info[n]["id"] == poses[n]["id"]);
    CHECK(info[n]["index"] == n);
    CHECK(info[n]["hasMask"] == true);
    CHECK(poses[n]["pickable"] == false);
    CHECK(std::count(masks.data.begin() + n * 512 * 512, masks.data.begin() + (n + 1) * 512 * 512,
                     1.f) >= 100);
  }
  const auto overlay = cvx::view(*call.outputs.at("overlay").asImage());
  REQUIRE(overlay.rows == 512);
  REQUIRE(overlay.cols == 512);
  REQUIRE(overlay.channels() == 4);
  bool alpha = true;
  for (int y = 0; y < 512; ++y)
    for (int x = 0; x < 512; ++x)
      alpha = alpha && overlay.at<cv::Vec4b>(y, x)[3] == 37;
  CHECK(alpha);
}
}

TEST_CASE(
    "P4 real PPHumanSeg CPU inference pastes full overlapping empty and background ROIs truthfully") {
  const auto directory = segmentationAssets();
  const auto photograph = segmentationPhoto(directory);
  const auto original = cvx::view(*photograph.asImage()).clone();
  for (int scenario = 0; scenario < 4; ++scenario) {
    CAPTURE(scenario);
    Data source = photograph;
    if (scenario == 3) {
      Image image;
      REQUIRE(cvx::fromMat(cv::Mat(512, 512, CV_8UC4, cv::Scalar(0, 0, 0, 37)), image));
      source = Data::image(std::move(image));
    }
    test::OpCall pre;
    pre.inputs["image"] = source;
    if (scenario == 1)
      pre.inputs["rois"] = Data::record(poses::make2DList(nlohmann::json::array(
          {poses::make2D("left", "image", "px", {200, 256}, 0, {400, 512}).data,
           poses::make2D("right", "image", "px", {312, 256}, 0, {400, 512}).data})));
    if (scenario == 2)
      pre.inputs["rois"] = Data::record(poses::make2DList(nlohmann::json::array()));
    REQUIRE_MESSAGE(pre.run("image.segmentation_preprocess").ok, pre.status.message);
    const int expected = scenario == 1 ? 2 : scenario == 2 ? 0 : 1;
    const auto result = segmentationInfer(directory, pre.outputs.at("tensor"), expected);
    CHECK(result.asTensor()->shape == (expected ? std::vector<std::int64_t>{expected, 2, 192, 192}
                                                : std::vector<std::int64_t>{0}));
    const auto post = segmentationDecode(source, pre, result);
    segmentationBindings(post);
    const auto labels = cvx::view(*post.outputs.at("labels").asImage());
    std::size_t foreground = 0, unknown = 0;
    for (int y = 0; y < 512; ++y)
      for (int x = 0; x < 512; ++x) {
        const auto label = labels.at<std::uint16_t>(y, x);
        foreground += label == 1;
        unknown += label == 65535;
      }
    const auto& poses = post.outputs.at("instances").asRecord()->data["items"];
    if (scenario < 2) {
      CHECK(foreground > 10000);
      CHECK(foreground < 250000);
      CHECK(!poses.empty());
    } else if (scenario == 2) {
      CHECK(poses.empty());
      CHECK(unknown == 512 * 512);
    } else {
      CHECK(poses.empty());
      CHECK(foreground == 0);
    }
  }
  CHECK(cv::norm(original, cvx::view(*photograph.asImage()), cv::NORM_INF) == 0);
}

TEST_CASE(
    "P4 real PPHumanSeg existing mask keep partial and replace execute zero one two CPU batches") {
  const auto directory = segmentationAssets();
  const auto source = segmentationPhoto(directory);
  test::OpCall initial;
  initial.inputs["image"] = source;
  REQUIRE(initial.run("image.segmentation_preprocess").ok);
  const auto baseline = segmentationDecode(
      source, initial, segmentationInfer(directory, initial.outputs.at("tensor"), 1));
  const auto originalInstances = *baseline.outputs.at("instances").asRecord(),
             originalInfo = *baseline.outputs.at("maskInfo").asRecord();
  const auto originalMasks = *baseline.outputs.at("masks").asTensor();
  REQUIRE(originalInstances.data["items"].size() == 1);
  for (int scenario = 0; scenario < 3; ++scenario) {
    CAPTURE(scenario);
    auto instances = originalInstances, info = originalInfo;
    auto masks = originalMasks;
    if (scenario > 0) {
      auto missing = poses::make2D("missing-full", "image", "px", {256, 256}, 0, {512, 512});
      missing.data["attributes"] = {{"hasMask", false}};
      instances.data["items"].push_back(missing.data);
      info.data["items"].push_back(
          {{"id", "missing-full"}, {"index", 1}, {"hasMask", false}, {"source", "none"}});
      masks.shape[0] = 2;
      masks.data.resize(2 * 512 * 512, 0);
    }
    test::OpCall pre;
    pre.inputs = {{"image", source},
                  {"rois", Data::record(instances)},
                  {"roiMasks", Data::tensor(masks)},
                  {"roiMaskInfo", Data::record(info)}};
    REQUIRE_MESSAGE(pre.run("image.segmentation_preprocess",
                            {{"replaceExistingMask", Value::boolean(scenario == 2)}})
                        .ok,
                    pre.status.message);
    CHECK(pre.outputs.at("tensor").asTensor()->shape[0] == scenario);
    const auto result = segmentationInfer(directory, pre.outputs.at("tensor"), scenario);
    const auto post = segmentationDecode(source, pre, result);
    segmentationBindings(post);
    const auto& delivered = *post.outputs.at("masks").asTensor();
    const auto& poses = post.outputs.at("instances").asRecord()->data["items"];
    if (scenario < 2)
      REQUIRE(!poses.empty());
    if (scenario == 0) {
      CHECK(delivered.data == originalMasks.data);
      CHECK(poses[0]["id"] == originalInstances.data["items"][0]["id"]);
    }
    if (scenario == 1) {
      REQUIRE(delivered.data.size() >= originalMasks.data.size());
      CHECK(
          std::equal(originalMasks.data.begin(), originalMasks.data.end(), delivered.data.begin()));
      CHECK(delivered.shape[0] >= 2);
    }
    if (scenario < 2) {
      REQUIRE(!poses.empty());
      CHECK(poses[0]["position"] == originalInstances.data["items"][0]["position"]);
      CHECK(poses[0]["size"] == originalInstances.data["items"][0]["size"]);
      CHECK(poses[0]["confidence"] == originalInstances.data["items"][0]["confidence"]);
    } else
      CHECK(post.outputs.at("quality").asRecord()->data["retainedCount"] == 0);
  }
}
#endif
