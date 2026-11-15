#include "helpers.h"
#include "lyflow/graph_contract_upgrade.h"

#include <fstream>
#include <algorithm>

using namespace lyflow;
using namespace lyflow::test;

TEST_CASE("DTS graph upgrade: plan and run convert only the selected metric boundary") {
  std::ifstream file(std::filesystem::path(LYFLOW_EXAMPLES_DIR).parent_path() / "core" / "tests" /
                     "fixtures" / "contract-upgrade" / "dts-mm.lyflow.json");
  REQUIRE(file.good());
  const auto graph = Json::parse(file);
  Point2D legacy;
  legacy.p[0] = 1000;
  legacy.p[1] = 2000;
  std::vector<exec::InjectedInput> inputs{
      {"root", "point", Data::point2d(legacy)},
      {"root", "root", Data::record({"DtsRoot", Json::object()})}};
  const auto plan = Json::parse(exec::planGraphJson(graph.dump(), {}, {}, "", inputs));
  REQUIRE(plan.size() == 4);
  bool found = false;
  for (const auto& node : plan)
    found = found || node.value("nodeId", "") == "root_metric";
  CHECK(found);
  Session run(graph, {}, {}, false, 1, false, inputs);
  const auto& log = run.wait();
  INFO(Json(log.events).dump());
  REQUIRE(log.runStatus() == "ok");
  Data metric, original;
  REQUIRE(exec::ResultStore::instance().get(run.runId(), "metric", "out", metric));
  REQUIRE(exec::ResultStore::instance().get(run.runId(), "legacy", "out", original));
  REQUIRE(metric.asPoint2D());
  REQUIRE(original.asPoint2D());
  CHECK(metric.asPoint2D()->p[0] == doctest::Approx(1));
  CHECK(metric.asPoint2D()->p[1] == doctest::Approx(2));
  CHECK(original.asPoint2D()->p[0] == 1000);
  CHECK(legacy.p[0] == 1000);
  Json upgraded, changes;
  GraphContractUpgradeError error;
  REQUIRE(upgradeGraphContracts(graph, graph["contractUpgrade"], upgraded, changes, error));
  auto pendingBypass = upgraded;
  pendingBypass["contractUpgrade"] = graph["contractUpgrade"];
  pendingBypass["meta"] = {{"owner", "keep"}};
  pendingBypass["nodes"].back()["bypass"] = true;
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
  Session enabledRun(pendingBypass, {}, {}, false, 1, false, inputs);
  REQUIRE(enabledRun.wait().runStatus() == "ok");
  Data enabledMetric;
  REQUIRE(exec::ResultStore::instance().get(enabledRun.runId(), "metric", "out", enabledMetric));
  REQUIRE(enabledMetric.asPoint2D());
  CHECK(enabledMetric.asPoint2D()->p[0] == doctest::Approx(1));
  auto saved = Json::parse(upgraded.dump());
  REQUIRE_FALSE(saved.contains("contractUpgrade"));
  const auto history = saved["meta"]["contractUpgradeHistory"];
  saved["nodes"].erase(std::remove_if(saved["nodes"].begin(), saved["nodes"].end(),
                                      [](const Json& n) { return n["id"] == "metric"; }),
                       saved["nodes"].end());
  saved["edges"].erase(std::remove_if(saved["edges"].begin(), saved["edges"].end(),
                                      [](const Json& e) {
                                        return e["from"]["node"] == "metric" ||
                                               e["to"]["node"] == "metric";
                                      }),
                       saved["edges"].end());
  const auto editedPlan = Json::parse(exec::planGraphJson(saved.dump(), {}, {}, "", inputs));
  REQUIRE(editedPlan.is_array());
  CHECK(editedPlan[0].value("kind", "") != "diagnostic");
  Session editedRun(saved, {}, {}, false, 1, false, inputs);
  CHECK(editedRun.wait().runStatus() == "ok");
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
  const auto groupedPlan =
      Json::parse(exec::planGraphJson(saved.dump(), {}, {}, "", groupedInputs));
  CHECK(groupedPlan[0].value("kind", "") != "diagnostic");
  Session groupedRun(saved, {}, {}, false, 1, false, groupedInputs);
  CHECK(groupedRun.wait().runStatus() == "ok");
  CHECK(saved["meta"]["contractUpgradeHistory"] == history);
  auto unknown = graph;
  unknown["nodes"][0]["opVersion"] = "2.0.0";
  const auto rejected = Json::parse(exec::planGraphJson(unknown.dump(), {}, {}, "", inputs));
  CHECK(rejected[0]["code"] == "bad_input");
  CHECK(rejected[0]["nodeId"] == "metric");
  Session invalid(unknown, {}, {}, false, 1, false, inputs);
  CHECK(invalid.wait().runStatus() == "error");
}
