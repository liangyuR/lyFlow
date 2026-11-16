#include "gap_upgrade_test_support.h"
#include "profile_graph_test_support.h"

#include "exec/plan.h"
#include "lyflow/graph_contract_upgrade.h"
#include "ops/profile_scene.h"

using namespace lyflow;
using Json = nlohmann::json;
using namespace lyflow::test::gap_upgrade;
using namespace lyflow::test::gap_graph_test;

#if defined(LYFLOW_GAP_TEMPLATE) && defined(LYFLOW_GAP_MODEL)
namespace {
Json legacyRuntime() {
  std::ifstream input(std::filesystem::path(__FILE__).parent_path() / "../../.." / "core" /
                      "tests/fixtures/contract-upgrade/gap-runtime-v1.json");
  REQUIRE(input.good());
  return Json::parse(input);
}
Json migratedRuntime(const Json& legacy, const RuntimeFixture& fixture) {
  auto graph = legacy.at("graph");
  const bool fine = legacy.at("kind").get<std::string>().find(":fine") != std::string::npos;
  if (graph.at("params").contains("modelPath"))
    graph["params"]["modelPath"]["default"] =
        (fixture.root / "valid-labels.onnx").generic_u8string();
  const auto request = gapRequest(graph, fine ? "fine" : "blocks");
  Json result, report;
  GraphContractUpgradeError error;
  REQUIRE_MESSAGE(upgradeGraphContracts(graph, request, result, report, error), error.message);
  REQUIRE(report.size() == 1);
  return result;
}
void validPlan(const Json& graph, const RuntimeFixture& fixture) {
  exec::RawGraph raw;
  Diagnostics diagnostics;
  const bool prepared = exec::prepareGraph(graph.dump(), raw, diagnostics);
  for (const auto& diagnostic : diagnostics.items())
    if (diagnostic.severity == Severity::Error)
      MESSAGE(diagnostic.nodeId, " ", diagnostic.status.code, " ", diagnostic.status.message);
  REQUIRE(prepared);
  exec::BuildOptions options;
  options.runId = "native-gap-upgrade-contract";
  options.baseDir = fixture.root;
  exec::Plan plan;
  REQUIRE(exec::buildPlan(ensureRegistry(), raw, options, plan, diagnostics));
  for (const auto& diagnostic : diagnostics.items())
    CHECK_MESSAGE((diagnostic.severity != Severity::Error), diagnostic.nodeId,
                  diagnostic.status.message);
  for (const auto& node : plan.nodes)
    CHECK_MESSAGE(node.valid, node.id);
}
}

TEST_CASE(
    "Six explicit legacy GAP importer graphs compile and execute against native public contracts") {
  RuntimeFixture fixture;
  fixture.model();
  const auto legacy = legacyRuntime();
  CHECK(legacy.at("version") == "1.4.4");
  CHECK(legacy.at("runtimeSha256") ==
        "0e189fae271362aefa65f550735eae5f58e4c1b44b5bfdff696c14bdc278802c");
  REQUIRE(legacy.at("cases").size() == 6);
  for (auto item = legacy.at("cases").begin(); item != legacy.at("cases").end(); ++item) {
    CAPTURE(item.key());
    REQUIRE(item.value().at("status") == "ok");
    const auto graph = migratedRuntime(item.value(), fixture);
    validPlan(graph, fixture);
    test::Session session(graph, fixture.root, {}, false, 1, true);
    const auto& log = session.wait();
    requireRun(log);
    REQUIRE(log.runStatus() == "ok");
    for (const auto* node : {"n_flush", "n_gap"})
      for (const auto* port : {"value", "segment"}) {
        const auto value = result(session, node, port);
        CHECK_MESSAGE(Json::parse(value.valueJson()) ==
                          item.value().at("measurements").at(node).at(port),
                      item.key(), node, port);
      }
    CHECK(Json::parse(result(session, "n_flush", "baseLine").valueJson()) ==
          item.value().at("measurements").at("n_flush").at("baseLine"));
    const auto bundle = result(session, "n_bundle", "bundle");
    REQUIRE(bundle.asRecord());
    CHECK(bundle.asRecord()->data.at("schemaVersion") == 3);
    const bool fine = item.key().find("fine") != std::string::npos;
    const auto sensor =
        result(session, fine ? "n_load" : "n_scan", fine ? "primary" : "sensorPrimary");
    REQUIRE(sensor.asProfile());
    CHECK(sensor.asProfile()->pointCount() == 1280);
    CHECK(sensor.asProfile()->descriptor.sourceCount == 1280);
    CHECK(sensor.asProfile()->validity[7] == 0);
    CHECK(sensor.asProfile()->sourceIndices[7] == 7);
    CHECK(sensor.asProfile()->cloud->rgb[3 * 100] == 100);
  }
}

TEST_CASE(
    "Explicit fine legacy GAP fallback migration rejects actual divergent choice dependencies") {
  RuntimeFixture fixture;
  fixture.model();
  const auto graph = migratedRuntime(legacyRuntime().at("cases").at("auto-fine"), fixture);
  test::Session original(graph, fixture.root, {}, false, 1, true);
  const auto& originalLog = original.wait();
  requireRun(originalLog);
  REQUIRE(originalLog.runStatus() == "ok");
  std::string guard, scanChoice;
  for (const auto& edge : graph.at("edges"))
    if (edge.at("to").at("port") == "scanChoice") {
      guard = edge.at("to").at("node").get<std::string>();
      scanChoice = edge.at("from").at("node").get<std::string>();
    }
  REQUIRE_FALSE(guard.empty());
  const auto scan = result(original, scanChoice.c_str(), "out");
  const auto actualChoice = result(original, scanChoice.c_str(), "choice");
  REQUIRE(actualChoice.asRecord());
  auto divergent = *actualChoice.asRecord();
  divergent.data["choice"] = divergent.data.at("choice") == "a" ? "b" : "a";
  test::Session invalid(
      graph, fixture.root, {guard}, false, 1, true,
      {{scanChoice, "out", scan}, {scanChoice, "choice", Data::record(std::move(divergent))}});
  const auto& log = invalid.wait();
  CHECK(log.runStatus() == "error");
  CHECK(log.finalState(guard) == "error");
  CHECK(log.nodeEvent(guard, "error").dump().find("location_path_mismatch") != std::string::npos);
  Data discarded;
  CHECK_FALSE(exec::ResultStore::instance().get(invalid.runId(), guard, "location", discarded));
}

TEST_CASE(
    "GAP migration preserves real shared-subgraph arrays projection bindings and two runtime instances") {
  RuntimeFixture fixture;
  fixture.model();
  const auto legacy = legacyRuntime().at("cases").at("auto");
  auto inner = gapSubgraph(legacy.at("graph"));
  gapNode(inner, "n_flush")["params"].erase("offset");
  inner["params"].push_back({{"name", "flushCompensation"},
                             {"type", "float"},
                             {"default", 0.0},
                             {"binds", Json::array({{{"node", "n_flush"}, {"param", "offset"}}})}});
  gapNode(inner, "n_flush")["params"].erase("signed");
  inner["params"].push_back({{"name", "signedDistance"},
                             {"type", "bool"},
                             {"default", true},
                             {"binds", Json::array({{{"node", "n_flush"}, {"param", "signed"}}})}});
  auto request = gapRequest(inner);
  request["rules"][0]["scope"] = {{"subgraph", "inside"}};
  Json graph{{"schemaVersion", 1},
             {"id", "shared-gap-migration"},
             {"nodes", Json::array({{{"id", "a"}, {"op", "sub:inside"}},
                                    {{"id", "b"}, {"op", "sub:inside"}}})},
             {"edges", Json::array()},
             {"subgraphs", {{"inside", inner}}},
             {"outputs",
              {{"aFlush", {{"node", "a/n_flush"}, {"port", "value"}}},
               {"bFlush", {{"node", "b/n_flush"}, {"port", "value"}}},
               {"aLine", {{"node", "a/n_fb_line"}, {"port", "out"}}}}},
             {"params",
              {{"offset",
                {{"type", "float"},
                 {"default", 2.0},
                 {"binds", {"a.flushCompensation", "b.flushCompensation"}}}},
               {"signedDistance",
                {{"type", "bool"},
                 {"default", false},
                 {"binds", {"a.signedDistance", "b.signedDistance"}}}}}}};
  Json upgraded, report;
  GraphContractUpgradeError error;
  REQUIRE_MESSAGE(upgradeGraphContracts(graph, request, upgraded, report, error), error.message);
  CHECK(upgraded["subgraphs"]["inside"]["params"].is_array());
  CHECK(upgraded["subgraphs"]["inside"]["outputs"] == inner["outputs"]);
  CHECK(upgraded["outputs"]["aLine"]["node"] != "a/n_fb_line");
  CHECK(upgraded["outputs"]["aLine"]["port"] == "line");
  CHECK_FALSE(gapNode(upgraded["subgraphs"]["inside"], "n_flush")["params"].contains("signed"));
  validPlan(upgraded, fixture);
  test::Session session(upgraded, fixture.root, {}, false, 1, true);
  const auto& log = session.wait();
  requireRun(log);
  REQUIRE(log.runStatus() == "ok");
  const auto baseline =
      legacy.at("measurements").at("n_flush").at("value").at("value").get<double>();
  CHECK(result(session, "a/n_flush", "value").asMeasurement()->value == baseline + 2.0);
  CHECK(result(session, "b/n_flush", "value").asMeasurement()->value == baseline + 2.0);
  CHECK(result(session, upgraded["outputs"]["aLine"]["node"].get<std::string>().c_str(), "line")
            .asLine2D() != nullptr);
  Json repeated, repeatReport;
  REQUIRE(upgradeGraphContracts(upgraded, request, repeated, repeatReport, error));
  CHECK(repeated == upgraded);
  CHECK(repeatReport.empty());
}
TEST_CASE("GAP migrated signed graph bindings compile and execute without default conflicts") {
  RuntimeFixture fixture;
  const auto legacy = legacyRuntime().at("cases").at("template");
  for (bool signedDistance : {false, true}) {
    auto graph = legacy.at("graph");
    gapNode(graph, "n_flush")["params"].erase("signed");
    graph["params"]["signedDistance"] = {
        {"type", "bool"}, {"default", signedDistance}, {"binds", {"n_flush.signed"}}};
    Json upgraded, report;
    GraphContractUpgradeError error;
    REQUIRE_MESSAGE(upgradeGraphContracts(graph, gapRequest(graph), upgraded, report, error),
                    error.message);
    validPlan(upgraded, fixture);
    test::Session session(upgraded, fixture.root, {}, false, 1, true);
    const auto& log = session.wait();
    requireRun(log);
    REQUIRE(log.runStatus() == "ok");
    const auto boundValue = result(session, "n_flush", "value").valueJson();
    auto explicitGraph = upgraded;
    explicitGraph["params"].erase("signedDistance");
    gapNode(explicitGraph, "n_flush")["params"]["signed"] = signedDistance;
    test::Session explicitSession(explicitGraph, fixture.root, {}, false, 1, true);
    const auto& explicitLog = explicitSession.wait();
    requireRun(explicitLog);
    REQUIRE(explicitLog.runStatus() == "ok");
    CHECK(boundValue == result(explicitSession, "n_flush", "value").valueJson());
  }
}
#endif
