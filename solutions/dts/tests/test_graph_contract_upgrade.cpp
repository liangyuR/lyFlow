#include "helpers.h"
#include "lyflow/graph_contract_upgrade.h"

#include <algorithm>
#include <fstream>

using namespace lyflow;
using namespace lyflow::test;

TEST_CASE(
    "DTS graph upgrade: selected legacy metric boundary and retired scene execution remain explicit") {
  std::ifstream file(std::filesystem::path(LYFLOW_EXAMPLES_DIR).parent_path() / "core" / "tests" /
                     "fixtures" / "contract-upgrade" / "dts-mm.lyflow.json");
  REQUIRE(file.good());
  const auto graph = Json::parse(file);
  Point2D legacy;
  legacy.p[0] = 1000;
  legacy.p[1] = 2000;
  const std::vector<exec::InjectedInput> inputs{
      {"root", "point", Data::point2d(legacy)},
      {"root", "root", Data::record({"DtsRoot", Json::object()})}};
  Json upgraded, changes;
  GraphContractUpgradeError error;
  REQUIRE(upgradeGraphContracts(graph, graph["contractUpgrade"], upgraded, changes, error));
  REQUIRE(upgraded["nodes"].size() == 4);
  CHECK(upgraded["nodes"].back()["id"] == "root_metric");
  CHECK(upgraded["nodes"].back()["params"]["sourceContract"] == "dts.geometry.v1");
  CHECK_FALSE(upgraded.contains("contractUpgrade"));
  const auto edge = [&](const Json& doc, const char* fromNode, const char* fromPort,
                        const char* toNode, const char* toPort) {
    return std::any_of(doc["edges"].begin(), doc["edges"].end(), [&](const Json& value) {
      return value["from"] == Json{{"node", fromNode}, {"port", fromPort}} &&
             value["to"] == Json{{"node", toNode}, {"port", toPort}};
    });
  };
  CHECK(edge(upgraded, "root", "point", "root_metric", "geometry"));
  CHECK(edge(upgraded, "root_metric", "geometry", "metric", "in"));
  CHECK(edge(upgraded, "root", "point", "legacy", "in"));
  CHECK_FALSE(edge(upgraded, "root", "point", "metric", "in"));
  CHECK_FALSE(graph.contains("meta"));
  const auto requireSceneUpgrade = [&](const Json& document,
                                       const std::vector<exec::InjectedInput>& provided) {
    const auto plan = Json::parse(exec::planGraphJson(document.dump(), {}, {}, "", provided));
    INFO(plan.dump());
    const auto text = plan.dump();
    CHECK(text.find("migration_required") != std::string::npos);
    CHECK(text.find("dtsProfile.v1") != std::string::npos);
    CHECK(text.find("unit=mm") != std::string::npos);
    CHECK(text.find("--profile-frame") != std::string::npos);
    CHECK(text.find("--profile-source-id") != std::string::npos);
    Session run(document, {}, {}, false, 1, false, provided);
    REQUIRE(run.wait().runStatus() == "error");
    Data absent;
    CHECK_FALSE(exec::ResultStore::instance().get(run.runId(), "metric", "out", absent));
    CHECK_FALSE(exec::ResultStore::instance().get(run.runId(), "legacy", "out", absent));
  };
  requireSceneUpgrade(graph, inputs);
  CHECK(legacy.p[0] == 1000);
  CHECK(legacy.p[1] == 2000);

  auto pendingBypass = upgraded;
  pendingBypass["contractUpgrade"] = graph["contractUpgrade"];
  pendingBypass["meta"] = {{"owner", "keep"}};
  pendingBypass["nodes"].back()["bypass"] = true;
  Json rejected, rejectedChanges;
  REQUIRE_FALSE(upgradeGraphContracts(pendingBypass, pendingBypass["contractUpgrade"], rejected,
                                      rejectedChanges, error));
  CHECK(error.nodeId == "metric");
  const auto rejectedPlan =
      Json::parse(exec::planGraphJson(pendingBypass.dump(), {}, {}, "", inputs));
  REQUIRE(rejectedPlan.size() == 1);
  CHECK(rejectedPlan[0]["code"] == "bad_input");
  CHECK(rejectedPlan[0]["nodeId"] == "metric");
  Session bypassRun(pendingBypass, {}, {}, false, 1, false, inputs);
  CHECK(bypassRun.wait().runStatus() == "error");
  Data bypassOutput;
  CHECK_FALSE(exec::ResultStore::instance().get(bypassRun.runId(), "metric", "out", bypassOutput));
  CHECK_FALSE(pendingBypass["meta"].contains("contractUpgradeHistory"));
  pendingBypass["nodes"].back()["bypass"] = false;
  REQUIRE(upgradeGraphContracts(pendingBypass, pendingBypass["contractUpgrade"], rejected,
                                rejectedChanges, error));
  CHECK_FALSE(rejected.contains("contractUpgrade"));
  CHECK(rejected["meta"]["owner"] == "keep");
  CHECK(edge(rejected, "root", "point", "root_metric", "geometry"));
  requireSceneUpgrade(pendingBypass, inputs);

  auto saved = Json::parse(upgraded.dump());
  const auto history = saved["meta"]["contractUpgradeHistory"];
  REQUIRE(history.is_array());
  saved["nodes"].erase(std::remove_if(saved["nodes"].begin(), saved["nodes"].end(),
                                      [](const Json& n) { return n["id"] == "metric"; }),
                       saved["nodes"].end());
  saved["edges"].erase(std::remove_if(saved["edges"].begin(), saved["edges"].end(),
                                      [](const Json& e) {
                                        return e["from"]["node"] == "metric" ||
                                               e["to"]["node"] == "metric";
                                      }),
                       saved["edges"].end());
  CHECK_FALSE(saved.contains("contractUpgrade"));
  CHECK(saved["meta"]["contractUpgradeHistory"] == history);
  requireSceneUpgrade(saved, inputs);
  saved["subgraphs"]["inside"] = {{"nodes", saved["nodes"]},
                                  {"edges", saved["edges"]},
                                  {"inputs", Json::array()},
                                  {"outputs", Json::array()},
                                  {"params", Json::array()}};
  saved["nodes"] =
      Json::array({{{"id", "group"}, {"op", "sub:inside"}, {"params", Json::object()}}});
  saved["edges"] = Json::array();
  auto groupedInputs = inputs;
  for (auto& input : groupedInputs)
    input.nodeId = "group/" + input.nodeId;
  requireSceneUpgrade(saved, groupedInputs);
  CHECK(saved["meta"]["contractUpgradeHistory"] == history);
  CHECK(saved["subgraphs"]["inside"]["nodes"].size() == 3);

  auto unknown = graph;
  unknown["nodes"][0]["opVersion"] = "2.0.0";
  REQUIRE_FALSE(
      upgradeGraphContracts(unknown, unknown["contractUpgrade"], rejected, rejectedChanges, error));
  CHECK(error.nodeId == "metric");
  const auto invalidPlan = Json::parse(exec::planGraphJson(unknown.dump(), {}, {}, "", inputs));
  CHECK(invalidPlan[0]["code"] == "bad_input");
  CHECK(invalidPlan[0]["nodeId"] == "metric");
  Session invalid(unknown, {}, {}, false, 1, false, inputs);
  CHECK(invalid.wait().runStatus() == "error");
}
