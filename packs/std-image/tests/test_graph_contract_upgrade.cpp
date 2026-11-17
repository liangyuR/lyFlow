#include "helpers.h"
#include "lyflow/calibration.h"
#include "lyflow/graph_migration.h"

#include <algorithm>
#include <fstream>

using namespace lyflow;
using namespace lyflow::test;

namespace {
Json planeCalibGraph() {
  std::ifstream file(std::filesystem::path(__FILE__).parent_path() / "fixtures" /
                     "plane-calib.lyflow.json");
  REQUIRE(file.good());
  return Json::parse(file);
}

bool hasEdge(const Json& graph, const Json& from, const Json& to) {
  return std::any_of(graph["edges"].begin(), graph["edges"].end(),
                     [&](const Json& edge) { return edge["from"] == from && edge["to"] == to; });
}
} // namespace

TEST_CASE("PlaneCalib graph migration inserts the explicit adapter only on the declared edge") {
  for (const auto* producer : {"image.load_calib", "image.board_calib"}) {
    CAPTURE(producer);
    auto graph = planeCalibGraph();
    graph["nodes"][0]["op"] = producer;
    graph["contractUpgrade"]["rules"][0]["source"]["op"] = producer;
    Json migrated;
    GraphMigrationError error;
    REQUIRE_MESSAGE(
        migrateGraph(ensureRegistry(), graph, graph["contractUpgrade"], migrated, error),
        error.message);
    CHECK_FALSE(migrated.contains("contractUpgrade"));
    CHECK_FALSE(migrated.contains("meta"));
    CHECK(migrated["nodes"].size() == graph["nodes"].size() + 1);
    CHECK(migrated["edges"].size() == graph["edges"].size() + 1);
    const auto& adapter = migrated["nodes"].back();
    CHECK(adapter["id"] == "explicit_calib");
    CHECK(adapter["op"] == "image.upgrade_plane_calib");
    CHECK(adapter["opVersion"] == "1.0.0");
    CHECK(adapter["params"] ==
          Json{{"missingUnit", "mm"}, {"imageFrame", "camera"}, {"planeFrame", "table"}});
    const Json load{{"node", "load"}, {"port", "calib"}};
    const Json adapterPort{{"node", "explicit_calib"}, {"port", "calib"}};
    const Json consumer{{"node", "consumer"}, {"port", "in"}};
    CHECK(hasEdge(migrated, load, adapterPort));
    CHECK(hasEdge(migrated, adapterPort, consumer));
    CHECK_FALSE(hasEdge(migrated, load, consumer));
    CHECK(migrated["edges"].back()["id"] == "calib-edge");

    Json again;
    CHECK_FALSE(migrateGraph(ensureRegistry(), migrated, graph["contractUpgrade"], again, error));
    CHECK(error.nodeId == "load");
  }
}

TEST_CASE("PlaneCalib graph migration rejects guessed units versions ports and duplicate rules") {
  const auto graph = planeCalibGraph();
  const auto& base = graph["contractUpgrade"];
  std::vector<std::pair<Json, Json>> bad;
  for (const auto* version : {"", "0.9.0", "1.0.1", "2.0.0", "garbage"}) {
    auto changed = graph;
    changed["nodes"][0]["opVersion"] = version;
    bad.push_back({changed, base});
  }
  auto request = base;
  request["rules"][0]["missingUnit"] = "unspecified";
  bad.push_back({graph, request});
  request = base;
  request["rules"][0]["source"]["port"] = "root";
  bad.push_back({graph, request});
  request = base;
  request["rules"][0]["source"]["op"] = "image.other_calib";
  bad.push_back({graph, request});
  request = base;
  request["rules"][0]["extra"] = true;
  bad.push_back({graph, request});
  request = base;
  request["rules"].push_back(request["rules"][0]);
  request["rules"][1]["adapterId"] = "second_adapter";
  bad.push_back({graph, request});
  for (const auto& value : bad) {
    CAPTURE(value.second);
    Json migrated;
    GraphMigrationError error;
    CHECK_FALSE(migrateGraph(ensureRegistry(), value.first, value.second, migrated, error));
    CHECK(error.nodeId == "load");
    CHECK_FALSE(error.message.empty());
  }
}

TEST_CASE(
    "PlaneCalib migrated graph plans and runs with explicit units frames and future rejection") {
  const auto legacy = planeCalibGraph();
  Json graph;
  GraphMigrationError error;
  REQUIRE_MESSAGE(migrateGraph(ensureRegistry(), legacy, legacy["contractUpgrade"], graph, error),
                  error.message);
  Record old{calibration::kPlaneType, {{"H", {1, 0, 0, 0, 1, 0, 0, 0, 1}}}};
  std::vector<exec::InjectedInput> inputs{{"load", "calib", Data::record(old)}};
  const auto plan = Json::parse(exec::planGraphJson(graph.dump(), {}, {}, "", inputs));
  REQUIRE(plan.size() == 3);
  bool found = false;
  for (const auto& node : plan)
    found = found || node.value("nodeId", "") == "explicit_calib";
  CHECK(found);
  Session run(graph, {}, {}, false, 1, false, inputs);
  INFO(Json(run.wait().events).dump());
  REQUIRE(run.wait().runStatus() == "ok");
  Data result;
  REQUIRE(exec::ResultStore::instance().get(run.runId(), "consumer", "out", result));
  REQUIRE(result.asRecord());
  calibration::Plane plane;
  std::string why;
  REQUIRE(calibration::parsePlane(*result.asRecord(), plane, why));
  CHECK(plane.unit == "mm");
  CHECK(plane.imageFrame == "camera");
  CHECK(plane.planeFrame == "table");
  CHECK(result.asRecord()->data["schemaVersion"] == 1);
  CHECK_FALSE(old.data.contains("unit"));
  old.data["schemaVersion"] = 2;
  inputs[0].data = Data::record(old);
  Session future(graph, {}, {}, false, 1, false, inputs);
  CHECK(future.wait().runStatus() == "error");
  CHECK(future.wait().nodeEvent("explicit_calib", "error")["error"]["code"] == "bad_input");
}
