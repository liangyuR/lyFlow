#include "gap_migration_test_support.h"
#include "profile_graph_test_support.h"

#include <map>
#include <set>

#include "exec/plan.h"
#include "ops/profile_scene.h"

using namespace lyflow;
using Json = nlohmann::json;
using namespace lyflow::test::gap_migration;
using namespace lyflow::test::gap_graph_test;

TEST_CASE("GAP whole-graph migration preserves known block producers and fallback closures") {
  const auto fixtures = legacyGaps();
  for (const auto* name : {"template", "auto"}) {
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
    Json upgraded;
    GraphMigrationError error;
    REQUIRE_MESSAGE(migrate(graph, request, upgraded, error), error.message);
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
    CHECK(upgraded["outputs"] == graph["outputs"]);
    CHECK_FALSE(upgraded["meta"].contains("contractUpgradeHistory"));
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
    Json repeated;
    CHECK_FALSE(migrate(upgraded, request, repeated, error));
    CHECK(error.nodeId == "renamed_n_scan");
  }
}

TEST_CASE(
    "GAP whole-graph migration rejects unproven source slots versions and partial components") {
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
    Json result;
    GraphMigrationError error;
    CHECK_FALSE(migrate(value.first, value.second, result, error));
    CHECK(error.nodeId == "n_scan");
    CHECK_FALSE(error.message.empty());
  }
}

TEST_CASE(
    "GAP explicit shared-subgraph migration preserves promoted parameters and honest projection rules") {
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
  Json result;
  GraphMigrationError error;
  REQUIRE_MESSAGE(migrate(graph, request, result, error), error.message);
  CHECK(result["subgraphs"]["outside"] == graph["subgraphs"]["outside"]);
  CHECK(result["params"] == graph["params"]);
  CHECK(result["subgraphs"]["inside"]["params"].back()["binds"][0]["param"] == "offsetMm");
  CHECK(result["subgraphs"]["inside"]["outputs"] == inner["outputs"]);
  CHECK(result["nodes"] == graph["nodes"]);
  Json again;
  CHECK_FALSE(migrate(result, request, again, error));
  auto invalid = graph;
  invalid["params"]["offset"]["binds"] = {"a/n_flush.offset"};
  CHECK_FALSE(migrate(invalid, request, again, error));
  CHECK(error.message.find("提升参数") != std::string::npos);
  auto outsideBinding = graph;
  outsideBinding["nodes"].push_back(
      {{"id", "n_flush"}, {"op", "value.boolean"}, {"params", Json::object()}});
  outsideBinding["params"]["external"] = {
      {"type", "bool"},
      {"default", false},
      {"binds", Json::array({{{"node", "n_flush"}, {"param", "signed"}}})}};
  REQUIRE_MESSAGE(migrate(outsideBinding, request, again, error), error.message);
  CHECK(again["params"] == outsideBinding["params"]);
  auto projectedObject = graph;
  projectedObject["params"]["offset"]["binds"] =
      Json::array({{{"node", "a/n_flush"}, {"param", "offset"}}});
  CHECK_FALSE(migrate(projectedObject, request, again, error));
  CHECK(error.message.find("提升参数") != std::string::npos);
  for (bool objectBinding : {false, true}) {
    auto shadowed = graph;
    shadowed["nodes"].push_back(
        {{"id", "n_scan"}, {"op", "value.string"}, {"params", Json::object()}});
    shadowed["params"]["layout"] = {
        {"type", "string"},
        {"default", "profile"},
        {"binds", Json::array({objectBinding ? Json{{"node", "n_scan"}, {"param", "layout"}}
                                             : Json("n_scan.layout")})}};
    REQUIRE_MESSAGE(migrate(shadowed, request, again, error), error.message);
    CHECK(again["params"] == shadowed["params"]);
  }
}

TEST_CASE(
    "GAP whole-graph fine migration preserves sensor slots inference and independent fallback guards") {
  const auto fixtures = legacyGaps();
  for (const auto* name : {"template-fine", "model-only-fine", "model-fine"}) {
    auto graph = fixtures.at(name);
    auto request = gapRequest(graph, "fine");
    renameNodes(graph, request);
    Json upgraded;
    GraphMigrationError error;
    REQUIRE_MESSAGE(migrate(graph, request, upgraded, error), name, error.message);
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
    CHECK(guarded == (std::string(name) == "model-fine" ? 1 : 0));
    const std::set<std::string> retired{
        "gap.to_measurement_frame", "gap.drop_non_finite", "gap.fit_line", "gap.selected_point",
        "gap.nearest_to_line",      "gap.flush",           "gap.gap",      "gap.point_offset"};
    for (const auto& node : upgraded["nodes"])
      CHECK_FALSE(retired.count(node["op"].get<std::string>()));
    Json repeated;
    CHECK_FALSE(migrate(upgraded, request, repeated, error));
  }
}

TEST_CASE(
    "GAP ROI migration preserves proven author side declarations and refuses conflicting overrides") {
  const auto graph = legacyGaps().at("template-fine");
  const auto request = gapRequest(graph, "fine");
  auto matching = graph;
  gapNode(matching, "n_roi_set")["params"]["datumSide"] = "left";
  gapNode(matching, "n_rois")["params"]["datumSide"] = "left";
  Json upgraded;
  GraphMigrationError error;
  REQUIRE_MESSAGE(migrate(matching, request, upgraded, error), error.message);
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
    CHECK_FALSE(migrate(value, request, upgraded, error));
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
    Json upgraded;
    GraphMigrationError error;
    CHECK_FALSE(migrate(mismatch, request, upgraded, error));
    CHECK(error.message.find(qualityPort) != std::string::npos);
    CHECK(error.message.find("同一原子特征") != std::string::npos);
  }
  auto renamed = graph;
  auto renamedRequest = request;
  renameNodes(renamed, renamedRequest);
  Json upgraded;
  GraphMigrationError error;
  REQUIRE_MESSAGE(migrate(renamed, renamedRequest, upgraded, error), error.message);
}

TEST_CASE("GAP legacy template migration retains explicitly saved initial pose modes") {
  for (const auto* mode : {"endpoint_midpoint", "centroid", "pca"}) {
    CAPTURE(mode);
    auto graph = legacyGaps().at("template");
    const auto request = gapRequest(graph);
    gapNode(graph, "n_locate")["params"]["initialPoseMode"] = mode;
    Json upgraded;
    GraphMigrationError error;
    const bool accepted = migrate(graph, request, upgraded, error);
    CHECK_MESSAGE(accepted, error.message);
    if (accepted) {
      CHECK(gapNode(upgraded, "n_locate")["params"]["initialPoseMode"] == mode);
      CHECK(gapNode(upgraded, "n_locate")["opVersion"] == "3.0.0");
    }
    gapNode(graph, "n_locate")["opVersion"] = "2.1.1";
    CHECK_FALSE(migrate(graph, request, upgraded, error));
    CHECK(error.message.find("不是已知的旧版本") != std::string::npos);
  }
}

TEST_CASE("GAP migration reconciles preserved boolean and aliased graph parameter bindings") {
  for (bool objectBinding : {false, true}) {
    auto graph = legacyGaps().at("template");
    auto& flush = gapNode(graph, "n_flush");
    for (const auto* field : {"signed", "scale", "offset"})
      flush["params"].erase(field);
    graph["params"] = Json::object();
    for (const auto& entry : {std::make_pair("signed", "signed"), std::make_pair("scale", "gain"),
                              std::make_pair("offset", "offsetMm")}) {
      const Json bind = objectBinding ? Json{{"node", "n_flush"}, {"param", entry.first}}
                                      : Json(std::string("n_flush.") + entry.first);
      graph["params"][entry.first] = {
          {"type", std::string(entry.first) == "signed" ? "bool" : "float"},
          {"default", std::string(entry.first) == "signed" ? Json(false) : Json(1.0)},
          {"binds", Json::array({bind})}};
    }
    const auto request = gapRequest(graph);
    Json upgraded;
    GraphMigrationError error;
    REQUIRE_MESSAGE(migrate(graph, request, upgraded, error), error.message);
    for (const auto* field : {"signed", "gain", "offsetMm"})
      CHECK_FALSE(gapNode(upgraded, "n_flush")["params"].contains(field));
    CHECK(upgraded["params"]["signed"] == graph["params"]["signed"]);
    auto conflicting = graph;
    gapNode(conflicting, "n_flush")["params"]["signed"] = true;
    CHECK_FALSE(migrate(conflicting, request, upgraded, error));
    CHECK(error.message.find("param_conflict") != std::string::npos);
  }
}

TEST_CASE(
    "GAP binding reconciliation covers unchanged aliases generated defaults and unsafe conversions") {
  struct BindingCase {
    const char* fixture;
    const char* node;
    const char* field;
    const char* target;
    Json value;
    bool supported;
  };
  for (const auto& entry : std::vector<BindingCase>{
           {"template", "n_ref_point", "method", "method", "line_end", true},
           {"template-fine", "n_crop_p", "bounds", "bounds", "open", true},
           {"template-fine", "n_filter", "radius", "radius", .0004, true},
           {"template-fine", "n_fit_base", "minInliers", "minInliers", 2, true},
           {"template-fine", "n_fit_base", "distThresh", "distanceThreshold", .1, false},
           {"template-fine", "n_filter", "keepOrganized", "keepOrganized", false, false},
           {"template-fine", "n_crop_p", "invert", "invert", false, false},
           {"template-fine", "n_load", "dropNonFinite", "dropNonFinite", true, false},
           {"model-only-fine", "n_labels_p", "row", "row", "secondary", false},
           {"template-fine", "n_tpl_f1", "layout", "layout", "sensor", false},
           {"template", "n_gap", "definition", "definition", "B", false}}) {
    CAPTURE(entry.fixture);
    CAPTURE(entry.node);
    CAPTURE(entry.field);
    auto graph = legacyGaps().at(entry.fixture);
    gapNode(graph, entry.node)["params"].erase(entry.field);
    graph["params"]["bindingMatrix"] = {{"type", entry.value.is_boolean()  ? "bool"
                                                 : entry.value.is_string() ? "string"
                                                                           : "float"},
                                        {"default", entry.value},
                                        {"binds", {std::string(entry.node) + "." + entry.field}}};
    const auto request =
        gapRequest(graph, std::string(entry.fixture) == "template" ? "blocks" : "fine");
    const auto boundGraph = graph;
    Json upgraded;
    GraphMigrationError error;
    const auto accepted = migrate(graph, request, upgraded, error);
    REQUIRE_MESSAGE(accepted == entry.supported, error.message);
    if (accepted) {
      CHECK_FALSE(gapNode(upgraded, entry.node)["params"].contains(entry.target));
      CHECK(upgraded["params"]["bindingMatrix"] == graph["params"]["bindingMatrix"]);
      gapNode(graph, entry.node)["params"][entry.field] = entry.value;
      CHECK_FALSE(migrate(graph, request, upgraded, error));
      CHECK(error.message.find("param_conflict") != std::string::npos);
    } else
      CHECK(error.message.find("动态参数") != std::string::npos);
    Json scoped{{"schemaVersion", 1},
                {"nodes", Json::array({{{"id", "instance"}, {"op", "sub:inside"}}})},
                {"edges", Json::array()},
                {"subgraphs", {{"inside", gapSubgraph(boundGraph)}}}};
    auto scopedRequest = request;
    scopedRequest["rules"][0]["scope"] = {{"subgraph", "inside"}};
    CHECK_MESSAGE(migrate(scoped, scopedRequest, upgraded, error) == entry.supported,
                  error.message);
    if (entry.supported)
      CHECK_FALSE(
          gapNode(upgraded["subgraphs"]["inside"], entry.node)["params"].contains(entry.target));
  }
  auto graph = legacyGaps().at("template");
  graph["nodes"].push_back({{"id", "point_offset"},
                            {"op", "gap.point_offset"},
                            {"opVersion", "1.0.1"},
                            {"params", Json::object()}});
  for (const auto* port : {"a", "b"})
    graph["edges"].push_back({{"id", std::string("offset_") + port},
                              {"from", {{"node", "n_ref_point"}, {"port", "point"}}},
                              {"to", {{"node", "point_offset"}, {"port", port}}}});
  for (const auto& fields :
       {std::make_pair("absDx", "absDx"), std::make_pair("absDy", "absDy"),
        std::make_pair("scaleDx", "gainDx"), std::make_pair("scaleDy", "gainDy"),
        std::make_pair("dxOffset", "dxOffsetMm"), std::make_pair("dyOffset", "dyOffsetMm")})
    graph["params"][fields.first] = {
        {"type", std::string(fields.first).rfind("abs", 0) == 0 ? "bool" : "float"},
        {"default", std::string(fields.first).rfind("abs", 0) == 0 ? Json(true) : Json(1.0)},
        {"binds", {std::string("point_offset.") + fields.first}}};
  Json upgraded;
  GraphMigrationError error;
  REQUIRE_MESSAGE(migrate(graph, gapRequest(graph), upgraded, error), error.message);
  for (const auto* field : {"absDx", "absDy", "gainDx", "gainDy", "dxOffsetMm", "dyOffsetMm"})
    CHECK_FALSE(gapNode(upgraded, "point_offset")["params"].contains(field));
  for (const auto* field : {"absDx", "absDy"}) {
    auto conflicting = graph;
    gapNode(conflicting, "point_offset")["params"][field] = false;
    CHECK_FALSE(migrate(conflicting, gapRequest(graph), upgraded, error));
    CHECK(error.message.find("param_conflict") != std::string::npos);
  }
}

TEST_CASE(
    "GAP fine model migration requires uncompressed source slots and matching diagnostic cloud role and frame") {
  const auto graph = legacyGaps().at("model-only-fine");
  const auto request = gapRequest(graph, "fine");
  for (const auto* producer : {"n_load", "n_frame_s", "n_crop_p"}) {
    auto changed = graph;
    for (auto& edge : changed["edges"])
      if (edge["to"]["node"] == "n_labels_p" && edge["to"]["port"] == "cloud")
        edge["from"] = {{"node", producer},
                        {"port", std::string(producer) == "n_load" ? "primary" : "cloud"}};
    Json upgraded;
    GraphMigrationError error;
    CHECK_FALSE(migrate(changed, request, upgraded, error));
    CHECK_FALSE(error.message.empty());
  }
  for (bool omit : {false, true}) {
    auto changed = graph;
    if (omit)
      gapNode(changed, "n_load")["params"].erase("dropNonFinite");
    else
      gapNode(changed, "n_load")["params"]["dropNonFinite"] = true;
    Json upgraded;
    GraphMigrationError error;
    CHECK_FALSE(migrate(changed, request, upgraded, error));
    CHECK(error.message.find("原始槽") != std::string::npos);
  }
}

#if defined(LYFLOW_GAP_TEMPLATE) && defined(LYFLOW_GAP_MODEL)
namespace {
Json legacyRuntime() { return gapFixture("gap-runtime-v1.json"); }
Json migratedRuntime(const Json& legacy, const RuntimeFixture& fixture) {
  auto graph = legacy.at("graph");
  const bool fine = legacy.at("kind").get<std::string>().find(":fine") != std::string::npos;
  if (graph.at("params").contains("modelPath"))
    graph["params"]["modelPath"]["default"] =
        (fixture.root / "valid-labels.onnx").generic_u8string();
  const auto request = gapRequest(graph, fine ? "fine" : "blocks");
  Json result;
  GraphMigrationError error;
  REQUIRE_MESSAGE(migrate(graph, request, result, error), error.message);
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
  Json upgraded;
  GraphMigrationError error;
  REQUIRE_MESSAGE(migrate(graph, request, upgraded, error), error.message);
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
  Json repeated;
  CHECK_FALSE(migrate(upgraded, request, repeated, error));
}
TEST_CASE("GAP migrated signed graph bindings compile and execute without default conflicts") {
  RuntimeFixture fixture;
  const auto legacy = legacyRuntime().at("cases").at("template");
  for (bool signedDistance : {false, true}) {
    auto graph = legacy.at("graph");
    gapNode(graph, "n_flush")["params"].erase("signed");
    graph["params"]["signedDistance"] = {
        {"type", "bool"}, {"default", signedDistance}, {"binds", {"n_flush.signed"}}};
    Json upgraded;
    GraphMigrationError error;
    REQUIRE_MESSAGE(migrate(graph, gapRequest(graph), upgraded, error), error.message);
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
