#include "p3_example_helpers.h"

TEST_CASE("P0 inspection runs through the scheduler, typed values and lazy branches") {
  using namespace lyflow::test;
  const std::filesystem::path base(LYFLOW_EXAMPLES_DIR);
  auto graph = p3::read(base / "p0-inspection.lyflow.json");
  Session accepted(graph, base);
  const auto& log = accepted.wait();
  REQUIRE_MESSAGE(log.runStatus() == "ok", log.events.back().dump());
  CHECK(log.finalState("rejected") == "skipped");

  const auto result = p3::take(lyflow_run_outputs(log.runId.c_str()));
  CHECK(result.at("decision").at("value").at("type") == "std.Value.v1");
  CHECK(p3::data(result, "decision").at("value") == "通过");
  const auto& stats = p3::data(result, "statistics");
  CHECK(stats.at("valid") == true);
  CHECK(stats.at("sampleCount") == 336);
  CHECK(stats.at("mean").get<double>() == doctest::Approx(255.0 * 8.0 / 28.0));
  const auto& regions = p3::data(result, "regions");
  CHECK(regions.at("count") == 1);
  CHECK(regions.at("regions").at(0).at("area") == 96);
  for (const char* port : {"minimumWidth", "maximumWidth"}) {
    CHECK(result.at(port).at("value").at("ok") == true);
    CHECK(result.at(port).at("value").at("unit") == "px");
    CHECK(result.at(port).at("value").at("value").get<double>() ==
          doctest::Approx(8).epsilon(0.02));
  }

  graph["params"]["meanLimit"]["default"] = 100;
  Session rejected(graph, base);
  const auto& other = rejected.wait();
  REQUIRE_MESSAGE(other.runStatus() == "ok", other.events.back().dump());
  const auto rejectedResult = p3::take(lyflow_run_outputs(other.runId.c_str()));
  CHECK(p3::data(rejectedResult, "decision").at("value") == "不通过");
  CHECK(other.finalState("accepted") == "skipped");
}
