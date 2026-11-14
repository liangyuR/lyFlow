#include "helpers.h"

#include <fstream>

#include "lyflow/c_api.h"
#include "lyflow/client.hpp"
#include "lyflow/value.h"

namespace {
using namespace lyflow;
using namespace lyflow::test;

Json expressionExample() {
  std::ifstream file(std::filesystem::path(LYFLOW_EXAMPLES_DIR) / "p2-expression.lyflow.json");
  REQUIRE(file.good());
  return Json::parse(file);
}

Json expressionOutputs(const std::string& runId) {
  char* raw = lyflow_run_outputs(runId.c_str());
  REQUIRE(raw != nullptr);
  const std::string source(raw);
  lyflow_string_free(raw);
  return Json::parse(source);
}

void checkExpressionOutputs(const Json& result, bool overridden) {
  const Json expectedMean = {{"kind", "number"}, {"value", overridden ? 17 : 13}, {"unit", "mm"}};
  const Json expectedLimited = {
      {"kind", "number"}, {"value", overridden ? 15 : 13}, {"unit", "mm"}};
  CHECK(result.at("mean").at("value").at("data") == expectedMean);
  CHECK(result.at("withinLimit").at("value").at("data") ==
        Json{{"kind", "boolean"}, {"value", !overridden}, {"unit", ""}});
  CHECK(result.at("limited").at("value").at("data") == expectedLimited);
  for (const auto& item : result.items()) {
    const auto& record = item.value().at("value");
    std::string why;
    REQUIRE_MESSAGE(values::validate(Record{record.at("type"), record.at("data")}, why), why);
  }
}
}

TEST_CASE("P2 expression example preserves units and selects the intended branch in scheduler") {
  for (const bool overridden : {false, true}) {
    auto graph = expressionExample();
    if (overridden)
      graph["params"]["leftValue"]["default"] = 20;
    Session session(graph, LYFLOW_EXAMPLES_DIR, {}, false, 2, true);
    const auto& log = session.wait();
    REQUIRE_MESSAGE(log.runStatus() == "ok", Json(log.events).dump());
    checkExpressionOutputs(expressionOutputs(log.runId), overridden);
    CHECK(log.finalState("mean") == "done");
    CHECK(log.finalState("within") == "done");
    CHECK(log.finalState("limited") == "done");
  }
}

TEST_CASE("P2 expression graph agrees through C ABI and loaded SDK with per-run overrides") {
  const auto source = expressionExample().dump();
  const auto base = std::filesystem::path(LYFLOW_EXAMPLES_DIR).u8string();
  Client client(LYFLOW_TEST_RUNTIME);
  for (const bool overridden : {true, false}) {
    const std::string id = overridden ? "p2-expression-override" : "p2-expression-default";
    const std::string params = overridden ? R"({"leftValue":20})" : "{}";
    lyflow_run_options options{};
    options.run_id = id.c_str();
    options.base_dir = base.c_str();
    options.params_json = params.c_str();
    options.no_reuse = 1;
    lyflow_run* run = lyflow_run_start(source.c_str(), &options, nullptr, nullptr);
    REQUIRE(run != nullptr);
    lyflow_run_join(run);
    const Json abi = expressionOutputs(id);
    lyflow_run_free(run);
    checkExpressionOutputs(abi, overridden);
    RunOptions sdkOptions;
    sdkOptions.runId = id + "-sdk";
    sdkOptions.baseDir = base;
    sdkOptions.paramsJson = params;
    sdkOptions.noReuse = true;
    const auto sdk = client.run(source, sdkOptions);
    REQUIRE_MESSAGE(sdk.ok(), sdk.summary);
    const Json sdkOutputs = Json::parse(sdk.outputs);
    checkExpressionOutputs(sdkOutputs, overridden);
    for (const char* name : {"mean", "withinLimit", "limited"})
      CHECK(abi.at(name).at("value") == sdkOutputs.at(name).at("value"));
  }
}
