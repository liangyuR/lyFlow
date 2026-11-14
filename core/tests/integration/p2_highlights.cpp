#include "helpers.h"

#include <cmath>
#include <fstream>

#include "lyflow/c_api.h"
#include "lyflow/client.hpp"
#include "lyflow/pose.h"

namespace {
using namespace lyflow;
using namespace lyflow::test;

Json highlightsExample() {
  std::ifstream file(std::filesystem::path(LYFLOW_EXAMPLES_DIR) / "p2-highlights.lyflow.json");
  REQUIRE(file.good());
  return Json::parse(file);
}

Json highlightsOutputs(const std::string& id) {
  char* raw = lyflow_run_outputs(id.c_str());
  REQUIRE(raw != nullptr);
  const std::string text(raw);
  lyflow_string_free(raw);
  return Json::parse(text);
}

void checkHighlights(const Json& outputs, bool verticalOnly) {
  const auto& poses = outputs.at("highlights").at("value");
  std::string why;
  REQUIRE_MESSAGE(lyflow::poses::validate(Record{poses.at("type"), poses.at("data")}, why), why);
  const auto& items = poses.at("data").at("items");
  REQUIRE(items.size() == (verticalOnly ? 1 : 2));
  const auto& segments = outputs.at("segments").at("value");
  CHECK(segments.at("type") == "image.Highlights.v1");
  CHECK(segments.at("data").at("unit") == "px");
  CHECK(segments.at("data").at("frame") == "image");
  REQUIRE(segments.at("data").at("items").size() == items.size());
  bool sawHorizontal = false, sawVertical = false;
  for (std::size_t i = 0; i < items.size(); ++i) {
    const auto& item = items[i];
    const auto& quality = segments.at("data").at("items")[i];
    CHECK(item.at("id") == quality.at("id"));
    CHECK(item.at("pickable") == false);
    const double angle = item.at("angle");
    if (std::abs(std::sin(angle)) < 0.05) {
      sawHorizontal = true;
      CHECK(std::abs(item.at("position")[0].get<double>() - (90)) <= (3));
      CHECK(std::abs(item.at("position")[1].get<double>() - (35)) <= (1));
      CHECK(std::abs(quality.at("length").get<double>() - (140)) <= (6));
    } else {
      sawVertical = true;
      CHECK(std::abs(std::cos(angle)) < 0.05);
      CHECK(std::abs(item.at("position")[0].get<double>() - (130)) <= (1));
      CHECK(std::abs(item.at("position")[1].get<double>() - (92.5)) <= (3));
      CHECK(std::abs(quality.at("length").get<double>() - (45)) <= (6));
    }
    CHECK(quality.at("rms").get<double>() <= 1.5);
    CHECK(quality.at("coverage").get<double>() > 0.7);
    CHECK(quality.at("support").get<std::size_t>() > 20);
  }
  CHECK(sawVertical);
  CHECK(sawHorizontal == !verticalOnly);
}
}

TEST_CASE("P2 highlight example detects finite lines inside a rotated region via scheduler") {
  Session run(highlightsExample(), LYFLOW_EXAMPLES_DIR, {}, false, 1, true);
  const auto& log = run.wait();
  REQUIRE_MESSAGE(log.runStatus() == "ok", Json(log.events).dump());
  checkHighlights(highlightsOutputs(log.runId), false);
  CHECK(log.finalState("detect") == "done");
}

TEST_CASE("P2 highlight C ABI and loaded SDK agree with isolated direction overrides") {
  const auto source = highlightsExample().dump();
  const auto base = std::filesystem::path(LYFLOW_EXAMPLES_DIR).u8string();
  Client client(LYFLOW_TEST_RUNTIME);
  for (const bool verticalOnly : {true, false}) {
    const std::string id = verticalOnly ? "p2-highlights-vertical" : "p2-highlights-default";
    const std::string params = verticalOnly ? R"({"direction":90,"directionTolerance":5})" : "{}";
    lyflow_run_options options{};
    options.run_id = id.c_str();
    options.base_dir = base.c_str();
    options.params_json = params.c_str();
    options.no_reuse = 1;
    lyflow_run* run = lyflow_run_start(source.c_str(), &options, nullptr, nullptr);
    REQUIRE(run != nullptr);
    lyflow_run_join(run);
    const auto abi = highlightsOutputs(id);
    lyflow_run_free(run);
    checkHighlights(abi, verticalOnly);
    RunOptions sdkOptions;
    sdkOptions.runId = id + "-sdk";
    sdkOptions.baseDir = base;
    sdkOptions.paramsJson = params;
    sdkOptions.noReuse = true;
    const auto sdk = client.run(source, sdkOptions);
    REQUIRE_MESSAGE(sdk.ok(), sdk.summary);
    const Json sdkOutputs = Json::parse(sdk.outputs);
    checkHighlights(sdkOutputs, verticalOnly);
    for (const char* name : {"highlights", "segments"})
      CHECK(abi.at(name).at("value") == sdkOutputs.at(name).at("value"));
  }
}
