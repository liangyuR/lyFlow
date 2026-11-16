#include "gap_upgrade_test_support.h"
#include "lyflow/graph_contract_upgrade.h"

#include <algorithm>
#include <fstream>

using namespace lyflow;
using Json = nlohmann::json;
using namespace lyflow::test::gap_upgrade;

TEST_CASE("GAP whole-graph upgrade preserves known block producers and atomic fallback closures") {
  const auto fixtures = legacyGaps();
  for (const auto* name : {"template", "model", "auto"}) {
    CAPTURE(name);
    auto graph = fixtures.at(name);
    auto request = gapRequest(graph);
    renameNodes(graph, request);
    graph["meta"]["user"] = {{"keep", true}};
    for (auto& node : graph["nodes"])
      node["meta"] = {{"user", node["id"]}};
    auto unrelated = Json{{"id", "untouched"},
                          {"op", "value.string"},
                          {"opVersion", "1.0.0"},
                          {"params", {{"value", "keep"}}}};
    graph["nodes"].push_back(unrelated);
    const auto before = graph;
    Json upgraded, report;
    GraphContractUpgradeError error;
    REQUIRE_MESSAGE(upgradeGraphContracts(graph, request, upgraded, report, error), error.message);
    CHECK(graph == before);
    CHECK(upgraded["meta"]["user"] == graph["meta"]["user"]);
    CHECK(gapNode(upgraded, "untouched") == unrelated);
    for (const auto& node : graph["nodes"]) {
      const auto id = node.at("id").get<std::string>();
      if (id == "untouched")
        continue;
      CHECK(gapNode(upgraded, id).at("ui") == node.at("ui"));
      CHECK(gapNode(upgraded, id).at("meta") == node.at("meta"));
    }
    CHECK(gapNode(upgraded, "renamed_n_scan")["params"]["primarySourceId"] == "file-channel-A");
    CHECK(gapNode(upgraded, "renamed_n_scan")["params"]["measurementFrame"] == "measurement-frame");
    CHECK(gapNode(upgraded, "renamed_n_flush")["op"] == "metrology.flush_point_line");
    CHECK(gapNode(upgraded, "renamed_n_gap")["op"] == "metrology.circle_gap");
    CHECK(upgraded["params"]["gapOffset"]["binds"][0] == "renamed_n_gap.offsetMm");
    CHECK(upgraded["params"]["gapOffset"]["binds"][1] == "renamed_n_circles.offset");
    CHECK(upgraded["outputs"] == before["outputs"]);
    REQUIRE(report.size() == 1);
    CHECK(report[0]["source"] == Json::array({request["rules"][0]["source"]}));
    CHECK(report[0]["graphUpdate"]["contractUpgradeReplacement"]["before"] == before);
    CHECK(report[0]["graphUpdate"]["contractUpgradeReplacement"]["after"] == upgraded);
    if (name != std::string("template")) {
      CHECK(gapNode(upgraded, "renamed_n_fb_rois")["op"] == "flow.fallback");
      CHECK(gapNode(upgraded, "renamed_n_fb_scan")["op"] == "gap.split_location");
      CHECK(gapNode(upgraded, "renamed_n_fb_quality_base")["op"] == "gap.split_line_feature");
      const std::set<std::string> referenceOps{
          gapNode(upgraded, "renamed_n_fb_quality_ref")["op"].get<std::string>(),
          gapNode(upgraded, "renamed_n_fb_ref_point")["op"].get<std::string>()};
      CHECK(referenceOps == std::set<std::string>{"flow.fallback", "gap.split_reference_feature"});
      CHECK(gapNode(upgraded, "renamed_n_circles")["op"] == "gap.seam_circles");
    }
    Json repeated, repeatedReport;
    REQUIRE(upgradeGraphContracts(upgraded, request, repeated, repeatedReport, error));
    CHECK(repeated == upgraded);
    CHECK(repeatedReport.empty());
    auto active = upgraded;
    active["contractUpgrade"] = request;
    REQUIRE(upgradeGraphContracts(active, request, repeated, repeatedReport, error));
    CHECK(repeated == upgraded);
    CHECK(repeatedReport.size() == 1);
  }
}

TEST_CASE(
    "GAP whole-graph upgrade rejects unproven source slots versions and partial components atomically") {
  const auto graph = legacyGaps().at("template");
  const auto request = gapRequest(graph);
  std::vector<std::pair<Json, Json>> bad;
  auto r = request;
  r["rules"][0]["alreadyAligned"] = false;
  bad.push_back({graph, r});
  r = request;
  r["rules"][0]["profiles"]["primary"]["unit"] = "mm";
  bad.push_back({graph, r});
  r = request;
  r["rules"][0]["component"]["style"] = "fine";
  bad.push_back({graph, r});
  r = request;
  r["rules"][0]["component"]["nodes"].erase(0);
  bad.push_back({graph, r});
  for (const auto* version : {"", "1.1.1", "2.0.0"}) {
    auto changed = graph;
    gapNode(changed, "n_scan")["opVersion"] = version;
    bad.push_back({changed, request});
  }
  auto changed = graph;
  gapNode(changed, "n_scan")["params"]["source"] = "inputs";
  bad.push_back({changed, request});
  changed = graph;
  gapNode(changed, "n_line")["bypass"] = true;
  bad.push_back({changed, request});
  changed = graph;
  changed["params"]["which"] = {
      {"type", "enum"}, {"default", "A"}, {"binds", {"n_gap.definition"}}};
  bad.push_back({changed, request});
  changed = graph;
  changed["nodes"].push_back({{"id", "observer"}, {"op", "value.string"}});
  changed["edges"].push_back({{"id", "external"},
                              {"from", {{"node", "n_line"}, {"port", "line"}}},
                              {"to", {{"node", "observer"}, {"port", "value"}}}});
  bad.push_back({changed, request});
  for (const auto& value : bad) {
    Json result{{"previous", true}}, report{{"previous", true}};
    const auto before = value.first;
    GraphContractUpgradeError error;
    CHECK_FALSE(upgradeGraphContracts(value.first, value.second, result, report, error));
    CHECK(value.first == before);
    CHECK(result == Json({{"previous", true}}));
    CHECK(report == Json({{"previous", true}}));
    CHECK(error.nodeId == "n_scan");
    CHECK_FALSE(error.message.empty());
  }
}

TEST_CASE(
    "GAP explicit shared-subgraph upgrade preserves promoted parameters and honest projection rules") {
  auto inner = gapSubgraph(legacyGaps().at("template"));
  gapNode(inner, "n_flush")["params"].erase("offset");
  inner["params"].push_back({{"name", "flushCompensation"},
                             {"type", "float"},
                             {"default", 0.0},
                             {"binds", Json::array({{{"node", "n_flush"}, {"param", "offset"}}})}});
  auto request = gapRequest(inner);
  request["rules"][0]["scope"] = {{"subgraph", "inside"}};
  Json graph{{"schemaVersion", 1},
             {"nodes", Json::array({{{"id", "a"}, {"op", "sub:inside"}},
                                    {{"id", "b"}, {"op", "sub:inside"}},
                                    {{"id", "other"}, {"op", "sub:outside"}}})},
             {"edges", Json::array()},
             {"subgraphs", {{"inside", inner}, {"outside", inner}}}};
  graph["params"]["offset"] = {
      {"type", "float"},
      {"default", 2},
      {"binds", {"a.flushCompensation", "b.flushCompensation", "other.flushCompensation"}}};
  Json result, report;
  GraphContractUpgradeError error;
  REQUIRE_MESSAGE(upgradeGraphContracts(graph, request, result, report, error), error.message);
  CHECK(result["subgraphs"]["outside"] == graph["subgraphs"]["outside"]);
  CHECK(result["params"] == graph["params"]);
  CHECK(result["subgraphs"]["inside"]["params"].back()["binds"][0]["param"] == "offsetMm");
  CHECK(result["subgraphs"]["inside"]["outputs"] == inner["outputs"]);
  CHECK(result["nodes"] == graph["nodes"]);
  Json again, repeated;
  REQUIRE(upgradeGraphContracts(result, request, again, repeated, error));
  CHECK(again == result);
  CHECK(repeated.empty());
  auto invalid = graph;
  invalid["params"]["offset"]["binds"] = {"a/n_flush.offset"};
  CHECK_FALSE(upgradeGraphContracts(invalid, request, again, repeated, error));
  CHECK(error.message.find("提升参数") != std::string::npos);
}

TEST_CASE(
    "GAP whole-graph fine migration preserves sensor slots inference and independent fallback guards") {
  const auto fixtures = legacyGaps();
  for (const auto* name : {"template-fine", "model-only-fine", "model-fine", "fine"}) {
    auto graph = fixtures.at(name);
    auto request = gapRequest(graph, "fine");
    renameNodes(graph, request);
    Json upgraded, report;
    GraphContractUpgradeError error;
    REQUIRE_MESSAGE(upgradeGraphContracts(graph, request, upgraded, report, error), name,
                    error.message);
    CHECK(gapNode(upgraded, "renamed_n_load")["opVersion"] == "2.0.0");
    CHECK_FALSE(gapNode(upgraded, "renamed_n_load")["params"].contains("dropNonFinite"));
    CHECK(gapNode(upgraded, "renamed_n_frame_p")["op"] == "profile.remap_axes");
    CHECK(gapNode(upgraded, "renamed_n_frame_p")["params"]["axes"] == "XZY");
    CHECK(gapNode(upgraded, "renamed_n_merge")["params"]["roleA"] == "secondary");
    CHECK(gapNode(upgraded, "renamed_n_merge")["params"]["channelPolicy"] == "intersection");
    CHECK(gapNode(upgraded, "renamed_n_filter")["op"] == "profile.radius_outlier");
    CHECK(gapNode(upgraded, "renamed_n_fit_base")["op"] == "profile.fit_line_robust");
    CHECK(gapNode(upgraded, "renamed_n_fit_base")["params"]["distanceThreshold"] == .0001);
    CHECK(gapNode(upgraded, "renamed_n_circles")["op"] == "gap.fit_gap_circles");
    CHECK(upgraded["outputs"] == graph["outputs"]);
    std::map<std::string, std::set<std::string>> ports;
    for (const auto& edge : upgraded["edges"])
      ports[edge["to"]["node"].get<std::string>()].insert(edge["to"]["port"].get<std::string>());
    if (name != std::string("template-fine")) {
      CHECK(gapNode(upgraded, "renamed_n_infer")["op"] == "gap.infer_profile");
      CHECK(ports.at("renamed_n_seg") ==
            std::set<std::string>{"tensor", "modelInput", "primary", "secondary"});
      CHECK(ports.at("renamed_n_labels_p") == std::set<std::string>{"scan", "labels"});
    }
    unsigned guarded = 0;
    for (const auto& node : upgraded["nodes"])
      if (node["op"] == "gap.make_location" && ports[node["id"]].count("scanChoice")) {
        ++guarded;
        CHECK(ports[node["id"]] == std::set<std::string>{"scan", "rois", "scanChoice", "roiChoice",
                                                         "datumChoice", "targetChoice",
                                                         "seamLeftChoice", "seamRightChoice"});
      }
    CHECK(guarded == (std::string(name) == "model-fine" || std::string(name) == "fine" ? 1 : 0));
    const std::set<std::string> retired{
        "gap.to_measurement_frame", "gap.drop_non_finite", "gap.fit_line", "gap.selected_point",
        "gap.nearest_to_line",      "gap.flush",           "gap.gap",      "gap.point_offset"};
    for (const auto& node : upgraded["nodes"])
      CHECK_FALSE(retired.count(node["op"].get<std::string>()));
    Json repeated, repeatedReport;
    REQUIRE_MESSAGE(upgradeGraphContracts(upgraded, request, repeated, repeatedReport, error),
                    error.message);
    CHECK(repeated == upgraded);
    CHECK(repeatedReport.empty());
  }
}

TEST_CASE(
    "GAP ROI migration preserves proven author side declarations and refuses conflicting overrides") {
  const auto graph = legacyGaps().at("template-fine");
  const auto request = gapRequest(graph, "fine");
  auto matching = graph;
  gapNode(matching, "n_roi_set")["params"]["datumSide"] = "left";
  gapNode(matching, "n_rois")["params"]["datumSide"] = "left";
  Json upgraded, report;
  GraphContractUpgradeError error;
  REQUIRE_MESSAGE(upgradeGraphContracts(matching, request, upgraded, report, error), error.message);
  CHECK(gapNode(upgraded, "n_rois")["params"]["datumSide"] == "left");
  CHECK(gapNode(upgraded, "n_roi_set")["params"]["metadataMode"] == "update_metadata");
  std::vector<Json> rejected;
  for (const auto* field : {"source", "datumSide"}) {
    auto changed = graph;
    gapNode(changed, "n_roi_set")["params"][field] =
        std::string(field) == "source" ? "model" : "right";
    rejected.push_back(changed);
    changed = graph;
    gapNode(changed, "n_roi_set")["params"].erase(field);
    changed["params"]["override"] = {
        {"type", "string"}, {"default", "left"}, {"binds", {std::string("n_roi_set.") + field}}};
    rejected.push_back(changed);
  }
  auto changed = graph;
  changed["params"]["side"] = {
      {"type", "string"}, {"default", "left"}, {"binds", {"n_rois.datumSide"}}};
  rejected.push_back(changed);
  for (const auto& value : rejected) {
    const auto resultBefore = upgraded, reportBefore = report;
    CHECK_FALSE(upgradeGraphContracts(value, request, upgraded, report, error));
    CHECK(upgraded == resultBefore);
    CHECK(report == reportBefore);
    CHECK(error.nodeId == "n_load");
  }
}

TEST_CASE("GAP migration refuses historical quality fallback from a different measured feature") {
  const auto graph = legacyGaps().at("auto");
  const auto request = gapRequest(graph);
  for (const auto* qualityPort : {"fitBase", "fitRef"}) {
    auto mismatch = graph;
    const auto target =
        std::string(qualityPort) == "fitBase" ? "n_fb_quality_base" : "n_fb_quality_ref";
    const auto backup = std::string(qualityPort) == "fitBase" ? "b_n_line" : "b_n_ref_point";
    for (auto& edge : mismatch["edges"])
      if (edge["to"]["node"] == target && edge["to"]["port"] == "a")
        edge["from"]["node"] = backup;
    Json upgraded{{"previous", 1}}, report{{"previous", 2}};
    GraphContractUpgradeError error;
    CHECK_FALSE(upgradeGraphContracts(mismatch, request, upgraded, report, error));
    CHECK(upgraded == Json({{"previous", 1}}));
    CHECK(report == Json({{"previous", 2}}));
    CHECK(error.message.find(qualityPort) != std::string::npos);
    CHECK(error.message.find("同一原子特征") != std::string::npos);
  }
  auto renamed = graph;
  auto renamedRequest = request;
  renameNodes(renamed, renamedRequest);
  Json upgraded, report;
  GraphContractUpgradeError error;
  REQUIRE_MESSAGE(upgradeGraphContracts(renamed, renamedRequest, upgraded, report, error),
                  error.message);
}

TEST_CASE("GAP legacy template migration retains explicitly saved initial pose modes") {
  for (const auto* mode : {"endpoint_midpoint", "centroid", "pca"}) {
    CAPTURE(mode);
    auto graph = legacyGaps().at("template");
    const auto request = gapRequest(graph);
    gapNode(graph, "n_locate")["params"]["initialPoseMode"] = mode;
    Json upgraded, report;
    GraphContractUpgradeError error;
    const bool accepted = upgradeGraphContracts(graph, request, upgraded, report, error);
    CHECK_MESSAGE(accepted, error.message);
    if (accepted) {
      CHECK(gapNode(upgraded, "n_locate")["params"]["initialPoseMode"] == mode);
      CHECK(gapNode(upgraded, "n_locate")["opVersion"] == "3.0.0");
    }
    gapNode(graph, "n_locate")["opVersion"] = "2.1.1";
    CHECK_FALSE(upgradeGraphContracts(graph, request, upgraded, report, error));
    CHECK(error.message.find("精确版本白名单") != std::string::npos);
  }
}
