#include "helpers.h"
#include "lyflow/calibration.h"

#include <fstream>

using namespace lyflow;
using namespace lyflow::test;

TEST_CASE(
    "PlaneCalib graph upgrade: plan and run share explicit units frames and future rejection") {
  std::ifstream file(std::filesystem::path(LYFLOW_EXAMPLES_DIR).parent_path() / "core" / "tests" /
                     "fixtures" / "contract-upgrade" / "plane-calib.lyflow.json");
  REQUIRE(file.good());
  const auto graph = Json::parse(file);
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
