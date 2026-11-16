#include <doctest/doctest.h>
#include <fstream>
#include <filesystem>
#include "lyflow/graph_contract_upgrade.h"

using Json = nlohmann::json;
namespace {
Json legacyDts() {
  std::ifstream file(std::filesystem::path(__FILE__).parent_path() / "fixtures" /
                     "contract-upgrade" / "dts-profile-v1.lyflow.json");
  REQUIRE(file.good());
  return Json::parse(file);
}
Json profileRequest() {
  return {{"schemaVersion", 1},
          {"rules", Json::array({{{"kind", "dtsProfile.v1"},
                                  {"source",
                                   {{"node", "n_in"},
                                    {"op", "dts.profile_in"},
                                    {"opVersion", "1.0.0"},
                                    {"port", "profile"}}},
                                  {"profile",
                                   {{"frame", "scan-frame"},
                                    {"sourceId", "sensor-1"},
                                    {"unit", "mm"},
                                    {"plane", "XZ"},
                                    {"handedness", "right"},
                                    {"sampleOrder", "acquisition"}}},
                                  {"adapterId", "declared_profile"}}})}};
}
Json& node(Json& graph, const std::string& id) {
  for (auto& item : graph["nodes"])
    if (item["id"] == id)
      return item;
  throw std::invalid_argument("missing test node");
}
}

TEST_CASE("DTS Profile upgrade: actual legacy graph preserves metadata and atomic save contract") {
  auto graph = legacyDts();
  graph["contractUpgrade"] = profileRequest();
  graph["meta"]["unknown"] = {{"nested", {1, 2, 3}}};
  graph["params"] = {{"limit", {{"default", -120}, {"binds", {"n_metal.angleToApexDeg"}}}}};
  node(graph, "n_metal")["params"]["angleToApexDeg"] = -120;
  node(graph, "n_in")["params"]["path"] = "../原始数据/scan.csv";
  Json upgraded, report;
  lyflow::GraphContractUpgradeError error;
  REQUIRE(lyflow::upgradeGraphContracts(graph, graph["contractUpgrade"], upgraded, report, error));
  REQUIRE(report.size() == 1);
  CHECK(report[0]["graphUpdate"]["contractUpgradeReplacement"]["before"] == graph);
  CHECK(report[0]["graphUpdate"]["contractUpgradeReplacement"]["after"] == upgraded);
  CHECK(upgraded["nodes"].size() == graph["nodes"].size() + 1);
  CHECK(upgraded["edges"].size() == graph["edges"].size() + 2);
  CHECK(node(upgraded, "n_clean")["op"] == "profile.clean");
  CHECK(node(upgraded, "n_clean")["params"]["mode"] == "preserve_slots");
  CHECK(node(upgraded, "n_faces")["op"] == "profile.split_faces");
  CHECK(node(upgraded, "n_faces")["params"]["minPoints"] == 15);
  CHECK(node(upgraded, "n_metal")["opVersion"] == "2.0.0");
  CHECK(node(upgraded, "n_metal")["params"]["angleToApexDeg"] == -120);
  CHECK(node(upgraded, "n_in") == node(graph, "n_in"));
  CHECK(upgraded["outputs"] == graph["outputs"]);
  CHECK(upgraded["params"] == graph["params"]);
  CHECK(upgraded["groups"] == graph["groups"]);
  CHECK(upgraded["meta"]["unknown"] == graph["meta"]["unknown"]);
  CHECK_FALSE(upgraded.contains("contractUpgrade"));
  auto saved = Json::parse(upgraded.dump());
  Json again, repeat;
  REQUIRE(lyflow::upgradeGraphContracts(saved, graph["contractUpgrade"], again, repeat, error));
  CHECK(again == upgraded);
  CHECK(repeat.empty());
  node(saved, "n_metal")["opVersion"] = "future";
  REQUIRE(lyflow::upgradeGraphContracts(saved, graph["contractUpgrade"], again, repeat, error));
  CHECK(again == saved);
}

TEST_CASE("DTS Profile upgrade rejects guessed evidence and never publishes partial mutation") {
  const auto base = legacyDts();
  for (const auto* problem :
       {"source-version", "consumer-version", "unknown-source", "external-consumer", "mixed-source",
        "bypass", "occupied-adapter", "nested"}) {
    CAPTURE(problem);
    auto graph = base;
    auto request = profileRequest();
    if (std::string(problem) == "source-version")
      node(graph, "n_in")["opVersion"] = "1.0.1";
    if (std::string(problem) == "consumer-version")
      node(graph, "n_dome")["opVersion"] = "2.0.0";
    if (std::string(problem) == "unknown-source")
      node(graph, "n_in")["op"] = "external.cloud";
    if (std::string(problem) == "external-consumer")
      graph["edges"].push_back({{"id", "external"},
                                {"from", {{"node", "n_clean"}, {"port", "clean"}}},
                                {"to", {{"node", "external"}, {"port", "cloud"}}}});
    if (std::string(problem) == "mixed-source")
      graph["edges"][2]["from"]["node"] = "unknown";
    if (std::string(problem) == "bypass")
      node(graph, "n_faces")["bypass"] = true;
    if (std::string(problem) == "occupied-adapter")
      node(graph, "n_in")["id"] = "declared_profile";
    if (std::string(problem) == "nested")
      request["rules"][0]["source"]["node"] = "inside/n_in";
    Json upgraded = {{"unchanged", true}}, report = Json::array({"unchanged"});
    lyflow::GraphContractUpgradeError error;
    CHECK_FALSE(lyflow::upgradeGraphContracts(graph, request, upgraded, report, error));
    CHECK_FALSE(error.message.empty());
    CHECK(upgraded == Json{{"unchanged", true}});
    CHECK(report == Json::array({"unchanged"}));
  }
}

TEST_CASE(
    "DTS Profile upgrade composes active M1 evidence once and retains geometry bridge identity") {
  auto graph = legacyDts();
  graph["nodes"].push_back(
      {{"id", "metric_consumer"}, {"op", "external.metric"}, {"params", Json::object()}});
  graph["edges"].push_back({{"id", "metric_edge"},
                            {"from", {{"node", "n_root"}, {"port", "point"}}},
                            {"to", {{"node", "metric_consumer"}, {"port", "point"}}}});
  auto request = profileRequest();
  request["rules"].push_back(
      {{"kind", "dtsGeometry.v1"},
       {"source",
        {{"node", "n_root"}, {"op", "dts.seal_root"}, {"opVersion", "1.0.0"}, {"port", "point"}}},
       {"target", {{"node", "metric_consumer"}, {"port", "point"}}},
       {"adapterId", "root_metric"}});
  graph["contractUpgrade"] = request;
  Json upgraded, report;
  lyflow::GraphContractUpgradeError error;
  REQUIRE(lyflow::upgradeGraphContracts(graph, request, upgraded, report, error));
  CHECK(node(upgraded, "root_metric")["opVersion"] == "2.0.0");
  CHECK(node(upgraded, "root_metric")["params"]["sourceContract"] == "dts.geometry.v2");
  CHECK(node(upgraded, "n_root")["opVersion"] == "2.0.0");
  CHECK(upgraded["meta"]["contractUpgradeHistory"].size() == 2);
  REQUIRE(report.size() == 1);
  CHECK(report[0]["graphUpdate"]["contractUpgradeReplacement"]["after"] == upgraded);
}

TEST_CASE(
    "DTS Profile upgrade explicit subgraph scope preserves interfaces bindings and library assets") {
  auto inner = legacyDts();
  inner["inputs"] = Json::array();
  inner["outputs"] = Json::array({{{"name", "result"}, {"node", "n_bundle"}, {"port", "bundle"}}});
  inner["params"] =
      Json::array({{{"name", "smooth"},
                    {"default", 9},
                    {"binds", Json::array({{{"node", "n_clean"}, {"param", "smoothPoints"}}})}}});
  inner.erase("schemaVersion");
  auto graph = Json{
      {"schemaVersion", 1},
      {"id", "scope-test"},
      {"nodes", Json::array({{{"id", "group"}, {"op", "sub:inside"}, {"params", Json::object()}}})},
      {"edges", Json::array()},
      {"subgraphs", {{"inside", inner}}},
      {"outputs", {{"bundle", {{"node", "group/n_bundle"}, {"port", "bundle"}}}}}};
  auto request = profileRequest();
  request["rules"][0]["scope"] = {{"subgraph", "inside"}};
  graph["contractUpgrade"] = request;
  Json upgraded, report;
  lyflow::GraphContractUpgradeError error;
  REQUIRE(lyflow::upgradeGraphContracts(graph, request, upgraded, report, error));
  CHECK(upgraded["nodes"] == graph["nodes"]);
  CHECK(upgraded["outputs"] == graph["outputs"]);
  auto& level = upgraded["subgraphs"]["inside"];
  CHECK(level["outputs"] == inner["outputs"]);
  CHECK(level["params"] == inner["params"]);
  CHECK_FALSE(node(level, "n_clean")["params"].contains("smoothPoints"));
  CHECK(node(level, "n_clean")["op"] == "profile.clean");
  CHECK(node(level, "n_bundle")["opVersion"] == "2.0.0");
}

TEST_CASE("DTS Profile upgrade selects arbitrary IDs and multiple explicit sources independently") {
  auto graph = legacyDts();
  const auto second = graph;
  for (auto item : second["nodes"]) {
    item["id"] = "other_" + item["id"].get<std::string>();
    graph["nodes"].push_back(item);
  }
  for (auto edge : second["edges"]) {
    edge["id"] = "other_" + edge["id"].get<std::string>();
    edge["from"]["node"] = "other_" + edge["from"]["node"].get<std::string>();
    edge["to"]["node"] = "other_" + edge["to"]["node"].get<std::string>();
    graph["edges"].push_back(edge);
  }
  auto request = profileRequest();
  auto rule = request["rules"][0];
  rule["source"]["node"] = "other_n_in";
  rule["profile"]["sourceId"] = "other-scanner";
  rule["adapterId"] = "other_profile";
  request["rules"].push_back(rule);
  Json upgraded, report;
  lyflow::GraphContractUpgradeError error;
  REQUIRE(lyflow::upgradeGraphContracts(graph, request, upgraded, report, error));
  CHECK(node(upgraded, "n_clean")["op"] == "profile.clean");
  CHECK(node(upgraded, "other_n_clean")["op"] == "profile.clean");
  CHECK(node(upgraded, "other_profile")["params"]["sourceId"] == "other-scanner");
  CHECK(upgraded["nodes"].size() == 18);
  CHECK(upgraded["edges"].size() == 44);
  REQUIRE(report.size() == 1);
  CHECK(report[0]["graphUpdate"]["contractUpgradeReplacement"]["before"] == graph);
}
