#include "helpers.h"

#include <cstdlib>
#include <fstream>

#include "lyflow/c_api.h"
#include "lyflow/client.hpp"
#include "lyflow/pose.h"

namespace {
using namespace lyflow;
using namespace lyflow::test;

std::filesystem::path assets() {
  const char* value = std::getenv("LYFLOW_P1_ASSETS");
  REQUIRE_MESSAGE((value && *value), "LYFLOW_P1_MODEL_TESTS requires LYFLOW_P1_ASSETS");
  const auto path = std::filesystem::u8path(value);
  for (const char* name : {"yolox_nano.onnx", "YOLOX-LICENSE", "astronaut.png", "chelsea.png"})
    REQUIRE_MESSAGE(std::filesystem::is_regular_file(path / name), name);
  std::ofstream file(path / "synthetic-background.ppm", std::ios::binary);
  file << "P6\n80 60\n255\n";
  const std::string pixels(80 * 60 * 3, '\0');
  file.write(pixels.data(), static_cast<std::streamsize>(pixels.size()));
  REQUIRE(file.good());
  return path;
}

Json detectionGraph(const std::filesystem::path& directory, const std::string& image) {
  std::ifstream file(std::filesystem::path(LYFLOW_EXAMPLES_DIR) / "p1-detection.lyflow.json");
  REQUIRE(file.good());
  Json graph = Json::parse(file);
  graph["params"]["imageFile"]["default"] = (directory / image).u8string();
  graph["params"]["modelFile"]["default"] = (directory / "yolox_nano.onnx").u8string();
  return graph;
}

Json results(const std::string& id) {
  char* raw = lyflow_run_outputs(id.c_str());
  REQUIRE(raw != nullptr);
  const Json result = Json::parse(raw);
  lyflow_string_free(raw);
  return result;
}

void checkDetections(const Json& result, const char* expectedClass) {
  const auto& record = result.at("detections").at("value");
  Record value{record.at("type"), record.at("data")};
  std::string why;
  REQUIRE_MESSAGE(poses::validate(value, why), why);
  const auto& items = value.data["items"];
  if (!expectedClass) {
    CHECK(items.empty());
  } else {
    CHECK(std::any_of(items.begin(), items.end(),
                      [&](const Json& item) { return item.at("classLabel") == expectedClass; }));
  }
  for (const auto& item : items) {
    CHECK(item["unit"] == "px");
    CHECK(item["confidence"].get<double>() >= 0.3);
    CHECK(item["pickable"] == false);
  }
}
}

TEST_CASE(
    "P1 fixed YOLOX model detects two public samples and returns empty synthetic background") {
  const auto directory = assets();
  for (const auto& sample :
       std::vector<std::pair<std::string, const char*>>{{"astronaut.png", "person"},
                                                        {"chelsea.png", "cat"},
                                                        {"synthetic-background.ppm", nullptr}}) {
    CAPTURE(sample.first);
    const Json graph = detectionGraph(directory, sample.first);
    Session session(graph, directory, {}, false, 1, true);
    const auto& log = session.wait();
    REQUIRE_MESSAGE(log.runStatus() == "ok", Json(log.events).dump());
    checkDetections(results(log.runId), sample.second);
    CHECK(log.finalState("model") == "done");
    CHECK(log.finalState("decode") == "done");
  }
}

TEST_CASE("P1 fixed detection workflow agrees across scheduler C ABI and loaded C++ SDK") {
  const auto directory = assets();
  const Json graph = detectionGraph(directory, "astronaut.png");
  Session session(graph, directory, {}, false, 1, true);
  const auto& log = session.wait();
  REQUIRE(log.runStatus() == "ok");
  const Json expected = results(log.runId);
  const auto source = graph.dump(), base = directory.u8string();
  lyflow_run_options options{};
  options.run_id = "p1-model-c-abi";
  options.base_dir = base.c_str();
  options.no_reuse = 1;
  lyflow_run* run = lyflow_run_start(source.c_str(), &options, nullptr, nullptr);
  REQUIRE(run != nullptr);
  lyflow_run_join(run);
  const Json cResult = results(options.run_id);
  lyflow_run_free(run);
  checkDetections(cResult, "person");
  CHECK(cResult["detections"]["value"] == expected["detections"]["value"]);
  Client client(LYFLOW_TEST_RUNTIME);
  RunOptions sdkOptions;
  sdkOptions.runId = "p1-model-sdk";
  sdkOptions.baseDir = base;
  sdkOptions.noReuse = true;
  const auto sdkResult = client.run(source, sdkOptions);
  REQUIRE_MESSAGE(sdkResult.ok(), sdkResult.summary);
  const Json sdk = Json::parse(sdkResult.outputs);
  checkDetections(sdk, "person");
  CHECK(sdk["detections"]["value"] == expected["detections"]["value"]);

  OpCall call;
  Tensor wrong;
  wrong.shape = {1, 6, 1280};
  wrong.data.resize(6 * 1280);
  call.inputs = {{"input", Data::tensor(std::move(wrong))}};
  const auto status = call.run(
      "ml.onnx_run", {{"modelPath", Value::text((directory / "yolox_nano.onnx").u8string())},
                      {"modelContract", Value::text("yolox-nano-0.1.1rc0")}});
  CHECK_FALSE(status.ok);
  CHECK(status.paramPath == "modelPath");
  CHECK(call.outputs.empty());
}

TEST_CASE(
    "P1 YOLOv8 YOLO11 YOLO26 run real exported models with equivalent C ABI and SDK results") {
  const auto directory = assets();
  REQUIRE(std::filesystem::is_regular_file(directory / "Ultralytics-LICENSE"));
  std::ifstream file(std::filesystem::path(LYFLOW_EXAMPLES_DIR) / "p1-yolo-detection.lyflow.json");
  REQUIRE(file.good());
  const Json sourceGraph = Json::parse(file);
  Client client(LYFLOW_TEST_RUNTIME);
  for (const auto& profile : std::vector<std::pair<std::string, std::string>>{
           {"yolov8", "yolov8n.onnx"}, {"yolo11", "yolo11n.onnx"}, {"yolo26", "yolo26n.onnx"}}) {
    CAPTURE(profile.first);
    REQUIRE(std::filesystem::is_regular_file(directory / profile.second));
    for (const auto& sample :
         std::vector<std::pair<std::string, const char*>>{{"astronaut.png", "person"},
                                                          {"chelsea.png", "cat"},
                                                          {"synthetic-background.ppm", nullptr}}) {
      CAPTURE(sample.first);
      Json graph = sourceGraph;
      graph["params"]["modelFamily"]["default"] = profile.first;
      graph["params"]["modelFile"]["default"] = (directory / profile.second).u8string();
      graph["params"]["imageFile"]["default"] = (directory / sample.first).u8string();
      Session session(graph, directory, {}, false, 1, true);
      const auto& log = session.wait();
      REQUIRE_MESSAGE(log.runStatus() == "ok", Json(log.events).dump());
      const auto expected = results(log.runId);
      checkDetections(expected, sample.second);
      if (sample.first != "astronaut.png")
        continue;
      const auto source = graph.dump(), base = directory.u8string();
      const auto runId = "p1-" + profile.first + "-abi";
      lyflow_run_options options{};
      options.run_id = runId.c_str();
      options.base_dir = base.c_str();
      options.no_reuse = 1;
      lyflow_run* run = lyflow_run_start(source.c_str(), &options, nullptr, nullptr);
      REQUIRE(run != nullptr);
      lyflow_run_join(run);
      const auto cResult = results(runId);
      lyflow_run_free(run);
      checkDetections(cResult, sample.second);
      CHECK(cResult["detections"]["value"] == expected["detections"]["value"]);
      RunOptions sdkOptions;
      sdkOptions.runId = "p1-" + profile.first + "-sdk";
      sdkOptions.baseDir = base;
      sdkOptions.noReuse = true;
      const auto sdk = client.run(source, sdkOptions);
      REQUIRE_MESSAGE(sdk.ok(), sdk.summary);
      const Json sdkResult = Json::parse(sdk.outputs);
      checkDetections(sdkResult, sample.second);
      CHECK(sdkResult["detections"]["value"] == expected["detections"]["value"]);
    }
  }
}
