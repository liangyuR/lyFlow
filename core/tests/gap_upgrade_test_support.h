#pragma once

#include <algorithm>
#include <fstream>

#include "helpers.h"

namespace lyflow::test::gap_upgrade {
using Json = nlohmann::json;
inline Json legacyGaps() {
  std::ifstream input(std::filesystem::path(__FILE__).parent_path() / "fixtures" /
                      "contract-upgrade" / "gap-standard-v1.json");
  REQUIRE(input.good());
  return Json::parse(input);
}
inline Json gapRequest(const Json& graph, const std::string& style = "blocks") {
  Json nodes = Json::array();
  for (const auto& node : graph.at("nodes"))
    nodes.push_back(node.at("id"));
  const Json declaration{{"sourceId", "file-channel-A"},
                         {"unit", "m"},
                         {"fileFrame", "file-frame"},
                         {"sensorFrame", "sensor-frame"},
                         {"fileHandedness", "right"}};
  auto secondary = declaration;
  secondary["sourceId"] = "file-channel-B";
  Json request{
      {"schemaVersion", 1},
      {"rules", Json::array({{{"kind", "gapProfile.v1"},
                              {"source",
                               {{"node", "n_scan"},
                                {"op", "gap.read_scan"},
                                {"opVersion", "1.1.0"},
                                {"port", "scan"}}},
                              {"profiles", {{"primary", declaration}, {"secondary", secondary}}},
                              {"measurement", {{"frame", "measurement-frame"}, {"plane", "XY"}}},
                              {"alreadyAligned", true},
                              {"component", {{"style", style}, {"nodes", nodes}}},
                              {"adapterIdPrefix", "native_gap"}}})}};
  if (style == "fine")
    request["rules"][0]["source"] = {{"node", "n_load"},
                                     {"op", "gap.load_profile_pair"},
                                     {"opVersion", "1.0.0"},
                                     {"port", "primary"}};
  return request;
}
inline Json& gapNode(Json& graph, const std::string& id) {
  const auto node = std::find_if(graph["nodes"].begin(), graph["nodes"].end(),
                                 [&](const auto& value) { return value.at("id") == id; });
  REQUIRE(node != graph["nodes"].end());
  return *node;
}
inline Json gapSubgraph(Json graph) {
  graph["inputs"] = Json::array();
  Json outputs = Json::array();
  for (auto item = graph.at("outputs").begin(); item != graph.at("outputs").end(); ++item)
    outputs.push_back(
        {{"name", item.key()},
         {"type", item.key() == "bundle" ? "Record" : "Measurement"},
         {"from", {{"node", item.value().at("node")}, {"port", item.value().at("port")}}}});
  graph["outputs"] = std::move(outputs);
  Json params = Json::array();
  for (auto item = graph.at("params").begin(); item != graph.at("params").end(); ++item) {
    auto parameter = item.value();
    parameter["name"] = item.key();
    Json binds = Json::array();
    for (const auto& value : parameter.at("binds")) {
      const auto binding = value.get<std::string>();
      const auto dot = binding.rfind('.');
      binds.push_back({{"node", binding.substr(0, dot)}, {"param", binding.substr(dot + 1)}});
    }
    parameter["binds"] = std::move(binds);
    params.push_back(std::move(parameter));
  }
  graph["params"] = std::move(params);
  graph.erase("schemaVersion");
  return graph;
}
inline void renameNodes(Json& graph, Json& request) {
  for (auto& node : graph["nodes"])
    node["id"] = "renamed_" + node["id"].get<std::string>();
  for (auto& edge : graph["edges"])
    for (const auto* side : {"from", "to"})
      edge[side]["node"] = "renamed_" + edge[side]["node"].get<std::string>();
  for (auto& output : graph["outputs"])
    output["node"] = "renamed_" + output["node"].get<std::string>();
  for (auto& parameter : graph["params"])
    for (auto& bind : parameter["binds"])
      bind = "renamed_" + bind.get<std::string>();
  auto& rule = request["rules"][0];
  rule["source"]["node"] = "renamed_" + rule["source"]["node"].get<std::string>();
  for (auto& id : rule["component"]["nodes"])
    id = "renamed_" + id.get<std::string>();
}
}
