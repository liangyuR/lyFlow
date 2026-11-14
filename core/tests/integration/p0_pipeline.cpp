#include "helpers.h"

#include <fstream>

#include "lyflow/c_api.h"
#include "lyflow/client.hpp"

using namespace lyflow;
using namespace lyflow::test;

namespace {
Json example() {
  std::ifstream file(std::filesystem::path(LYFLOW_EXAMPLES_DIR) / "p0-inspection.lyflow.json");
  REQUIRE(file.good());
  return Json::parse(file);
}

Json named(const std::string& runId) {
  char* raw = lyflow_run_outputs(runId.c_str());
  REQUIRE(raw != nullptr);
  const auto result = Json::parse(raw);
  lyflow_string_free(raw);
  return result;
}

void checkResult(const Json& result, const char* decision) {
  CHECK(result.at("decision").at("value").at("type") == "std.Value.v1");
  CHECK(result.at("decision").at("value").at("data").at("value") == decision);
  const auto& stats = result.at("statistics").at("value").at("data");
  CHECK(stats.at("valid") == true);
  CHECK(stats.at("sampleCount") == 336);
  CHECK(stats.at("mean").get<double>() == doctest::Approx(255.0 * 8.0 / 28.0));
  const auto& regions = result.at("regions").at("value").at("data");
  CHECK(regions.at("count") == 1);
  CHECK(regions.at("regions").at(0).at("area") == 96);
  for (const char* port : {"minimumWidth", "maximumWidth"}) {
    CHECK(result.at(port).at("value").at("ok") == true);
    CHECK(result.at(port).at("value").at("unit") == "px");
    CHECK(result.at(port).at("value").at("value").get<double>() ==
          doctest::Approx(8).epsilon(0.02));
  }
}
}

TEST_CASE("P0 inspection runs through the scheduler, typed values and lazy branches") {
  auto graph = example();
  Session accepted(graph, LYFLOW_EXAMPLES_DIR);
  const auto& log = accepted.wait();
  REQUIRE_MESSAGE(log.runStatus() == "ok", log.events.back().dump());
  checkResult(named(log.runId), "通过");
  CHECK(log.finalState("rejected") == "skipped");

  graph["params"]["meanLimit"]["default"] = 100;
  Session rejected(graph, LYFLOW_EXAMPLES_DIR);
  const auto& other = rejected.wait();
  REQUIRE_MESSAGE(other.runStatus() == "ok", other.events.back().dump());
  checkResult(named(other.runId), "不通过");
  CHECK(other.finalState("accepted") == "skipped");
}

TEST_CASE("P0 embedded C ABI exposes the same versioned results") {
  const auto source = example().dump();
  const auto base = std::filesystem::path(LYFLOW_EXAMPLES_DIR).u8string();
  lyflow_run_options options{};
  options.base_dir = base.c_str();
  for (const bool overrideParams : {true, false}) {
    options.run_id = overrideParams ? "p0-c-abi-custom" : "p0-c-abi-default";
    options.params_json = overrideParams ? R"({"meanLimit":100,"failText":"本次请求"})" : nullptr;
    lyflow_run* run = lyflow_run_start(source.c_str(), &options, nullptr, nullptr);
    REQUIRE(run != nullptr);
    lyflow_run_join(run);
    const auto result = named(options.run_id);
    lyflow_run_free(run);
    checkResult(result, overrideParams ? "本次请求" : "通过");
  }
}

TEST_CASE("P0 C++ SDK loads the runtime and executes the same inspection graph") {
  Client client(LYFLOW_TEST_RUNTIME);
  RunOptions options;
  options.runId = "p0-sdk";
  options.baseDir = std::filesystem::path(LYFLOW_EXAMPLES_DIR).u8string();
  const auto result = client.run(example().dump(), options);
  REQUIRE_MESSAGE(result.ok(), result.summary);
  checkResult(Json::parse(result.outputs), "通过");
}
