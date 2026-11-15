#include "helpers.h"
#include "lyflow/graph_contract_upgrade.h"
#include "exec/plan.h"

#include <fstream>
#include <algorithm>

using namespace lyflow;
using namespace lyflow::test;

namespace {
Json fixture(const char* name) {
  std::ifstream file(std::filesystem::path(__FILE__).parent_path() / "fixtures" /
                     "contract-upgrade" / name);
  REQUIRE(file.good());
  return Json::parse(file);
}
} // namespace

TEST_CASE("Graph contract upgrade: exact version evidence, edge scope and idempotence") {
  for (const auto* name : {"dts-mm.lyflow.json", "plane-calib.lyflow.json"}) {
    CAPTURE(name);
    const auto graph = fixture(name);
    const auto original = graph;
    Json upgraded, report;
    GraphContractUpgradeError error;
    REQUIRE(upgradeGraphContracts(graph, graph["contractUpgrade"], upgraded, report, error));
    CHECK(graph == original);
    CHECK(upgraded["nodes"].size() == graph["nodes"].size() + 1);
    CHECK(upgraded["edges"].size() == graph["edges"].size() + 1);
    CHECK_FALSE(upgraded.contains("contractUpgrade"));
    CHECK(upgraded["meta"]["contractUpgradeHistory"][0]["request"] == graph["contractUpgrade"]);
    REQUIRE(report.size() == 1);
    CHECK(report[0]["source"]["opVersion"] == "1.0.0");
    Json again, repeated;
    REQUIRE(upgradeGraphContracts(upgraded, graph["contractUpgrade"], again, repeated, error));
    CHECK(again == upgraded);
    CHECK(repeated.empty());
    auto doubleRequest = graph["contractUpgrade"];
    doubleRequest["rules"][0]["adapterId"] = "another_adapter";
    CHECK_FALSE(upgradeGraphContracts(upgraded, doubleRequest, again, repeated, error));
    CHECK(again == upgraded);
    exec::RawGraph raw;
    Diagnostics diagnostics;
    REQUIRE(exec::parseGraph(graph.dump(), raw, diagnostics));
    CHECK(raw.nodes.size() == upgraded["nodes"].size());
    REQUIRE(diagnostics.items().size() == 1);
    CHECK(diagnostics.items()[0].migration != nullptr);
    CHECK(Json::parse(diagnostics.items()[0].migration->editsJson) == report[0]["edits"]);
    CHECK(Json::parse(diagnostics.items()[0].migration->graphUpdateJson) ==
          report[0]["graphUpdate"]);
    auto legacyApplied = upgraded;
    legacyApplied["contractUpgrade"] = graph["contractUpgrade"];
    Json retired, retirement;
    REQUIRE(
        upgradeGraphContracts(legacyApplied, graph["contractUpgrade"], retired, retirement, error));
    CHECK(retired == upgraded);
    REQUIRE(retirement.size() == 1);
    CHECK(retirement[0]["nodeId"] == "");
    CHECK(retirement[0]["edits"]["addNodes"].empty());
    exec::RawGraph already;
    Diagnostics unchanged;
    REQUIRE(exec::parseGraph(upgraded.dump(), already, unchanged));
    CHECK(unchanged.items().empty());
    auto corrupt = upgraded;
    corrupt["nodes"].back()["opVersion"] = "2.0.0";
    const auto previous = again;
    CHECK_FALSE(upgradeGraphContracts(corrupt, graph["contractUpgrade"], again, repeated, error));
    CHECK(again == previous);
    CHECK(error.nodeId == graph["contractUpgrade"]["rules"][0]["target"]["node"]);
  }
  auto graph = fixture("dts-mm.lyflow.json");
  Json upgraded, report;
  GraphContractUpgradeError error;
  REQUIRE(upgradeGraphContracts(graph, graph["contractUpgrade"], upgraded, report, error));
  CHECK(upgraded["edges"][0] == graph["edges"][1]);
  for (const auto* version : {"", "0.9.0", "1.0.1", "2.0.0", "garbage"}) {
    CAPTURE(version);
    auto changed = graph;
    changed["nodes"][0]["opVersion"] = version;
    CHECK_FALSE(upgradeGraphContracts(changed, graph["contractUpgrade"], upgraded, report, error));
  }
  auto request = graph["contractUpgrade"];
  request["schemaVersion"] = 2;
  CHECK_FALSE(upgradeGraphContracts(graph, request, upgraded, report, error));
  request = graph["contractUpgrade"];
  request["rules"][0]["source"]["port"] = "root";
  CHECK_FALSE(upgradeGraphContracts(graph, request, upgraded, report, error));
  request = graph["contractUpgrade"];
  request["rules"].push_back(request["rules"][0]);
  request["rules"][1]["adapterId"] = "second_adapter";
  const auto previous = upgraded;
  const auto previousReport = report;
  CHECK_FALSE(upgradeGraphContracts(graph, request, upgraded, report, error));
  CHECK(upgraded == previous);
  CHECK(report == previousReport);
  auto externalRequestGraph = graph;
  externalRequestGraph.erase("contractUpgrade");
  REQUIRE(upgradeGraphContracts(externalRequestGraph, graph["contractUpgrade"], upgraded, report,
                                error));
  CHECK_FALSE(upgraded.contains("contractUpgrade"));
  CHECK(upgraded["meta"]["contractUpgradeHistory"][0]["request"] == graph["contractUpgrade"]);
  auto calibration = fixture("plane-calib.lyflow.json");
  calibration["contractUpgrade"]["rules"][0]["missingUnit"] = "unspecified";
  exec::RawGraph raw;
  Diagnostics diags;
  CHECK_FALSE(exec::parseGraph(calibration.dump(), raw, diags));
  REQUIRE(diags.items().size() == 1);
  CHECK(diags.items()[0].nodeId == "consumer");
  CHECK(diags.items()[0].status.code == "bad_input");
}

TEST_CASE(
    "Graph contract retirement: partial fanout resumes and passive history survives editing") {
  auto graph = fixture("dts-mm.lyflow.json");
  graph["meta"] = {
      {"owner", "keep"},
      {"contractUpgradeHistory", Json::array({{{"schemaVersion", 2}, {"future", true}}})}};
  auto second = graph["contractUpgrade"]["rules"][0];
  second["target"]["node"] = "legacy";
  second["adapterId"] = "root_metric_second";
  graph["contractUpgrade"]["rules"].push_back(second);
  Json upgraded, report;
  GraphContractUpgradeError error;
  REQUIRE(upgradeGraphContracts(graph, graph["contractUpgrade"], upgraded, report, error));
  REQUIRE(report.size() == 2);
  CHECK_FALSE(report[0].contains("graphUpdate"));
  REQUIRE(report[1]["graphUpdate"]["contractUpgradeRetirement"]["requirements"].size() == 2);
  CHECK(upgraded["meta"]["owner"] == "keep");
  CHECK(upgraded["meta"]["contractUpgradeHistory"].size() == 2);
  auto partial = graph;
  const auto& edit = report[0]["edits"];
  partial["nodes"].push_back(edit["addNodes"][0]);
  const auto removed = edit["removeEdges"][0];
  auto& edges = partial["edges"];
  edges.erase(
      std::remove_if(edges.begin(), edges.end(), [&](const Json& e) { return e == removed; }),
      edges.end());
  for (const auto& edge : edit["addEdges"])
    edges.push_back(edge);
  const auto savedPartial = Json::parse(partial.dump());
  Json completed, remaining;
  REQUIRE(upgradeGraphContracts(savedPartial, savedPartial["contractUpgrade"], completed, remaining,
                                error));
  CHECK(completed == upgraded);
  REQUIRE(remaining.size() == 1);
  CHECK(remaining[0]["nodeId"] == "legacy");
  CHECK(remaining[0]["edits"]["addNodes"].size() == 1);
  auto edited = Json::parse(upgraded.dump());
  edited["nodes"].erase(std::remove_if(edited["nodes"].begin(), edited["nodes"].end(),
                                       [](const Json& n) { return n["id"] == "metric"; }),
                        edited["nodes"].end());
  edited["edges"].erase(std::remove_if(edited["edges"].begin(), edited["edges"].end(),
                                       [](const Json& e) {
                                         return e["from"]["node"] == "metric" ||
                                                e["to"]["node"] == "metric";
                                       }),
                        edited["edges"].end());
  exec::RawGraph raw;
  Diagnostics diags;
  REQUIRE(exec::parseGraph(edited.dump(), raw, diags));
  CHECK(diags.empty());
  edited["subgraphs"]["inside"] = {{"nodes", edited["nodes"]},
                                   {"edges", edited["edges"]},
                                   {"inputs", Json::array()},
                                   {"outputs", Json::array()},
                                   {"params", Json::array()}};
  edited["nodes"] =
      Json::array({{{"id", "group"}, {"op", "sub:inside"}, {"params", Json::object()}}});
  edited["edges"] = Json::array();
  exec::RawGraph grouped;
  Diagnostics groupedDiags;
  REQUIRE(exec::parseGraph(edited.dump(), grouped, groupedDiags));
  CHECK(groupedDiags.empty());
  CHECK(edited["meta"] == upgraded["meta"]);
  for (const auto& request :
       {Json(), Json::array(), Json{{"schemaVersion", 999}, {"rules", Json::array()}}}) {
    auto invalid = graph;
    invalid["contractUpgrade"] = request;
    exec::RawGraph rejected;
    Diagnostics rejectedDiags;
    CHECK_FALSE(exec::parseGraph(invalid.dump(), rejected, rejectedDiags));
    CHECK(rejectedDiags.hasErrors());
  }
}

TEST_CASE("Graph contract retirement precedes source and consumer operator migration hooks") {
  Registry registry;
  registry.addType(PortType{"Record", "#aaaaaa", {}, ""});
  auto identity = [](const Json& params) { return params; };
  auto compute = [](const Inputs&, const ParamView&, Outputs&, ExecContext&) {
    return Status::Ok();
  };
  OperatorDesc source;
  source.id = "image.load_calib";
  source.version = "2.0.0";
  source.label = "Source";
  source.category = "Test";
  source.outputs = {{"calib", "Record", "Calib", "", true}};
  source.compute = compute;
  source.migrations = {{1, identity}};
  registry.addOperator(std::move(source));
  OperatorDesc adapter;
  adapter.id = "image.upgrade_plane_calib";
  adapter.version = "1.0.0";
  adapter.label = "Adapter";
  adapter.category = "Test";
  adapter.inputs = {{"calib", "Record", "Calib", "", true}};
  adapter.outputs = adapter.inputs;
  for (const auto* name : {"missingUnit", "imageFrame", "planeFrame"}) {
    Param p;
    p.name = name;
    p.label = name;
    p.type = ParamType::String;
    p.def = Value::text("");
    adapter.params.push_back(std::move(p));
  }
  adapter.compute = compute;
  registry.addOperator(std::move(adapter));
  OperatorDesc consumer;
  consumer.id = "util.reroute";
  consumer.version = "2.0.0";
  consumer.label = "Consumer";
  consumer.category = "Test";
  consumer.inputs = {{"newIn", "Record", "Input", "", true}};
  consumer.outputs = {{"out", "Record", "Output", "", true}};
  consumer.compute = compute;
  consumer.migrations = {{1, nullptr, [](const Json&, const std::vector<MigrationInput>& inputs) {
                            TopologyEdit edits;
                            for (const auto& input : inputs) {
                              if (input.port == "in") {
                                CHECK(input.fromOp == "image.upgrade_plane_calib");
                                edits.dropInputs.push_back("in");
                                edits.addEdges.push_back(
                                    {input.fromNode, input.fromPort, "@self", "newIn"});
                              }
                            }
                            return edits;
                          }}};
  registry.addOperator(std::move(consumer));
  REQUIRE(registry.validate().empty());
  const auto graph = fixture("plane-calib.lyflow.json");
  exec::RawGraph raw;
  Diagnostics diags;
  REQUIRE(exec::parseGraph(graph.dump(), raw, diags));
  exec::Plan plan;
  exec::BuildOptions options;
  REQUIRE(exec::buildPlan(registry, raw, options, plan, diags));
  const auto diagnostics = Json::parse(diags.toJson());
  REQUIRE(diagnostics.size() == 3);
  CHECK(diagnostics[0].contains("graphUpdate"));
  CHECK(diagnostics[1]["nodeId"] == "load");
  CHECK(diagnostics[2]["edits"]["addEdges"][0]["to"]["port"] == "newIn");
  Json saved, report;
  GraphContractUpgradeError error;
  REQUIRE(upgradeGraphContracts(graph, graph["contractUpgrade"], saved, report, error));
  for (std::size_t i = 1; i < diagnostics.size(); ++i) {
    const auto& change = diagnostics[i];
    for (auto& node : saved["nodes"])
      if (node["id"] == change["nodeId"])
        for (const auto* key : {"op", "opVersion", "params"})
          node[key] = change[key];
    if (change.contains("edits")) {
      for (const auto& removed : change["edits"]["removeEdges"])
        saved["edges"].erase(std::remove_if(saved["edges"].begin(), saved["edges"].end(),
                                            [&](const Json& edge) {
                                              return edge["from"] == removed["from"] &&
                                                     edge["to"] == removed["to"];
                                            }),
                             saved["edges"].end());
      for (const auto& added : change["edits"]["addEdges"])
        saved["edges"].push_back(added);
    }
  }
  exec::RawGraph reopened;
  Diagnostics reopenedDiags;
  REQUIRE(exec::parseGraph(saved.dump(), reopened, reopenedDiags));
  exec::Plan reopenedPlan;
  REQUIRE(exec::buildPlan(registry, reopened, options, reopenedPlan, reopenedDiags));
  CHECK(reopenedDiags.empty());
}
