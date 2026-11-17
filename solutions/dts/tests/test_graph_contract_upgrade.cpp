#include "helpers.h"
#include "lyflow/graph_migration.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>

using namespace lyflow;
using namespace lyflow::test;

namespace {
Json fixture(const char* name) {
  std::ifstream file(std::filesystem::path(__FILE__).parent_path() / "fixtures" / name);
  REQUIRE(file.good());
  return Json::parse(file);
}
Json legacyDts() { return fixture("dts-profile-v1.lyflow.json"); }
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
bool migrate(const Json& graph, const Json& request, Json& migrated, GraphMigrationError& error) {
  return migrateGraph(ensureRegistry(), graph, request, migrated, error);
}
} // namespace

TEST_CASE(
    "DTS Profile migration rewrites the actual legacy chain and preserves document metadata") {
  auto graph = legacyDts();
  graph["contractUpgrade"] = profileRequest();
  graph["meta"]["unknown"] = {{"nested", {1, 2, 3}}};
  graph["params"] = {{"limit", {{"default", -120}, {"binds", {"n_metal.angleToApexDeg"}}}}};
  node(graph, "n_metal")["params"]["angleToApexDeg"] = -120;
  node(graph, "n_in")["params"]["path"] = "../原始数据/scan.csv";
  Json upgraded;
  GraphMigrationError error;
  REQUIRE_MESSAGE(migrate(graph, graph["contractUpgrade"], upgraded, error), error.message);
  CHECK(upgraded["nodes"].size() == graph["nodes"].size() + 1);
  CHECK(upgraded["edges"].size() == graph["edges"].size() + 2);
  CHECK(node(upgraded, "n_clean")["op"] == "profile.clean");
  CHECK(node(upgraded, "n_clean")["params"]["mode"] == "preserve_slots");
  CHECK(node(upgraded, "n_faces")["op"] == "profile.split_faces");
  CHECK(node(upgraded, "n_faces")["params"]["minPoints"] == 15);
  CHECK(node(upgraded, "n_metal")["opVersion"] == "2.0.0");
  CHECK(node(upgraded, "n_metal")["params"]["angleToApexDeg"] == -120);
  CHECK(node(upgraded, "n_in") == node(graph, "n_in"));
  CHECK(node(upgraded, "declared_profile")["op"] == "profile.from_cloud");
  CHECK(node(upgraded, "declared_profile")["params"] == profileRequest()["rules"][0]["profile"]);
  CHECK(upgraded["outputs"] == graph["outputs"]);
  CHECK(upgraded["params"] == graph["params"]);
  CHECK(upgraded["groups"] == graph["groups"]);
  CHECK(upgraded["meta"] == graph["meta"]);
  CHECK_FALSE(upgraded.contains("contractUpgrade"));
  Json again;
  CHECK_FALSE(migrate(Json::parse(upgraded.dump()), graph["contractUpgrade"], again, error));
  CHECK(error.nodeId == "n_in");
}

TEST_CASE("DTS Profile migration rejects guessed evidence") {
  const auto base = legacyDts();
  for (const auto* problem :
       {"source-version", "consumer-version", "unknown-source", "external-consumer", "mixed-source",
        "bypass", "occupied-adapter", "nested", "unit", "plane"}) {
    CAPTURE(problem);
    auto graph = base;
    auto request = profileRequest();
    const std::string p = problem;
    if (p == "source-version")
      node(graph, "n_in")["opVersion"] = "1.0.1";
    if (p == "consumer-version")
      node(graph, "n_dome")["opVersion"] = "2.0.0";
    if (p == "unknown-source")
      node(graph, "n_in")["op"] = "external.cloud";
    if (p == "external-consumer")
      graph["edges"].push_back({{"id", "external"},
                                {"from", {{"node", "n_clean"}, {"port", "clean"}}},
                                {"to", {{"node", "external"}, {"port", "cloud"}}}});
    if (p == "mixed-source")
      graph["edges"][2]["from"]["node"] = "unknown";
    if (p == "bypass")
      node(graph, "n_faces")["bypass"] = true;
    if (p == "occupied-adapter")
      node(graph, "n_in")["id"] = "declared_profile";
    if (p == "nested")
      request["rules"][0]["source"]["node"] = "inside/n_in";
    if (p == "unit")
      request["rules"][0]["profile"]["unit"] = "m";
    if (p == "plane")
      request["rules"][0]["profile"]["plane"] = "XY";
    Json upgraded;
    GraphMigrationError error;
    CHECK_FALSE(migrate(graph, request, upgraded, error));
    CHECK(error.nodeId == request["rules"][0]["source"]["node"]);
    CHECK_FALSE(error.message.empty());
  }
}

TEST_CASE("DTS Profile migration carries an earlier geometry bridge to the v2 contract") {
  auto graph = legacyDts();
  graph["nodes"].push_back(
      {{"id", "metric_consumer"}, {"op", "external.metric"}, {"params", Json::object()}});
  graph["edges"].push_back({{"id", "metric_edge"},
                            {"from", {{"node", "n_root"}, {"port", "point"}}},
                            {"to", {{"node", "metric_consumer"}, {"port", "point"}}}});
  auto request = profileRequest();
  request["rules"].insert(request["rules"].begin(),
                          Json{{"kind", "dtsGeometry.v1"},
                               {"source",
                                {{"node", "n_root"},
                                 {"op", "dts.seal_root"},
                                 {"opVersion", "1.0.0"},
                                 {"port", "point"}}},
                               {"target", {{"node", "metric_consumer"}, {"port", "point"}}},
                               {"adapterId", "root_metric"}});
  Json upgraded;
  GraphMigrationError error;
  REQUIRE_MESSAGE(migrate(graph, request, upgraded, error), error.message);
  CHECK(node(upgraded, "root_metric")["opVersion"] == "2.0.0");
  CHECK(node(upgraded, "root_metric")["params"]["sourceContract"] == "dts.geometry.v2");
  CHECK(node(upgraded, "root_metric")["params"]["sourceOp"] == "dts.seal_root");
  CHECK(node(upgraded, "n_root")["opVersion"] == "2.0.0");
  CHECK_FALSE(upgraded["meta"].contains("contractUpgradeHistory"));
}

TEST_CASE("DTS Profile migration subgraph scope preserves interfaces and bindings") {
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
  Json upgraded;
  GraphMigrationError error;
  REQUIRE_MESSAGE(migrate(graph, request, upgraded, error), error.message);
  CHECK(upgraded["nodes"] == graph["nodes"]);
  CHECK(upgraded["outputs"] == graph["outputs"]);
  auto& level = upgraded["subgraphs"]["inside"];
  CHECK(level["outputs"] == inner["outputs"]);
  CHECK(level["params"] == inner["params"]);
  CHECK_FALSE(node(level, "n_clean")["params"].contains("smoothPoints"));
  CHECK(node(level, "n_clean")["op"] == "profile.clean");
  CHECK(node(level, "n_bundle")["opVersion"] == "2.0.0");
}

TEST_CASE(
    "DTS Profile migration selects arbitrary IDs and multiple explicit sources independently") {
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
  Json upgraded;
  GraphMigrationError error;
  REQUIRE_MESSAGE(migrate(graph, request, upgraded, error), error.message);
  CHECK(node(upgraded, "n_clean")["op"] == "profile.clean");
  CHECK(node(upgraded, "other_n_clean")["op"] == "profile.clean");
  CHECK(node(upgraded, "other_profile")["params"]["sourceId"] == "other-scanner");
  CHECK(upgraded["nodes"].size() == 18);
  CHECK(upgraded["edges"].size() == 44);
}

TEST_CASE(
    "DTS geometry migration inserts the metric bridge while v1 scene nodes stay blocked until migrated") {
  const auto graph = fixture("dts-mm.lyflow.json");
  Point2D legacy;
  legacy.p[0] = 1000;
  legacy.p[1] = 2000;
  const std::vector<exec::InjectedInput> inputs{
      {"root", "point", Data::point2d(legacy)},
      {"root", "root", Data::record({"DtsRoot", Json::object()})}};
  Json upgraded;
  GraphMigrationError error;
  REQUIRE_MESSAGE(migrate(graph, graph["contractUpgrade"], upgraded, error), error.message);
  REQUIRE(upgraded["nodes"].size() == 4);
  CHECK(upgraded["nodes"].back()["id"] == "root_metric");
  CHECK(upgraded["nodes"].back()["op"] == "dts.geometry_to_metric");
  CHECK(upgraded["nodes"].back()["params"] ==
        Json{{"sourceContract", "dts.geometry.v1"}, {"sourceOp", "dts.seal_root"}});
  CHECK_FALSE(upgraded.contains("contractUpgrade"));
  CHECK_FALSE(upgraded.contains("meta"));
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

  Json again;
  CHECK_FALSE(migrate(upgraded, graph["contractUpgrade"], again, error));
  CHECK(error.nodeId == "root");
  for (const auto* problem : {"node-version", "rule-version", "port", "extra"}) {
    CAPTURE(problem);
    auto changed = graph;
    auto request = graph["contractUpgrade"];
    const std::string p = problem;
    if (p == "node-version")
      changed["nodes"][0]["opVersion"] = "2.0.0";
    if (p == "rule-version")
      request["rules"][0]["source"]["opVersion"] = "2.0.0";
    if (p == "port")
      request["rules"][0]["source"]["port"] = "root";
    if (p == "extra")
      request["rules"][0]["extra"] = true;
    CHECK_FALSE(migrate(changed, request, again, error));
    CHECK(error.nodeId == "root");
  }

  const auto pendingPlan = Json::parse(exec::planGraphJson(graph.dump(), {}, {}, "", inputs));
  REQUIRE(pendingPlan.size() == 1);
  CHECK(pendingPlan[0]["code"] == "migration_required");
  CHECK(pendingPlan[0]["nodeId"] == "");
  const auto requireSceneMigration = [&](const Json& document,
                                         const std::vector<exec::InjectedInput>& provided) {
    const auto text = exec::planGraphJson(document.dump(), {}, {}, "", provided);
    INFO(text);
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
  requireSceneMigration(upgraded, inputs);
  CHECK(legacy.p[0] == 1000);
  CHECK(legacy.p[1] == 2000);

  auto grouped = upgraded;
  grouped["subgraphs"]["inside"] = {{"nodes", grouped["nodes"]},
                                    {"edges", grouped["edges"]},
                                    {"inputs", Json::array()},
                                    {"outputs", Json::array()},
                                    {"params", Json::array()}};
  grouped["nodes"] =
      Json::array({{{"id", "group"}, {"op", "sub:inside"}, {"params", Json::object()}}});
  grouped["edges"] = Json::array();
  auto groupedInputs = inputs;
  for (auto& input : groupedInputs)
    input.nodeId = "group/" + input.nodeId;
  requireSceneMigration(grouped, groupedInputs);

  for (const auto* retired : {"dts.profile_clean", "dts.split_faces"}) {
    CAPTURE(retired);
    const auto plan =
        Json::parse(exec::planGraphJson(makeGraph({{"old", retired}}, {}).dump(), {}, {}));
    INFO(plan.dump());
    CHECK(std::any_of(plan.begin(), plan.end(), [](const Json& item) {
      return item.value("nodeId", "") == "old" && item.value("code", "") == "migration_required" &&
             item.value("message", "").find("--profile-source-id") != std::string::npos;
    }));
  }
}
