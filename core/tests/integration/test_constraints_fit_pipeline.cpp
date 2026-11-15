#include <array>
#include <cmath>
#include <doctest/doctest.h>

#include "helpers.h"

using namespace lyflow;

TEST_CASE("Geometry constraints constructor feeds pointcloud fitting and graph overrides") {
  using Json = nlohmann::json;
  Json items = Json::array();
  const std::array<std::array<double, 3>, 7> points{
      {{0, 0, 0}, {2, 0, 0}, {0, 3, 0}, {0, 0, 4}, {1, 2, 3}, {-2, 1, 0.5}, {3, -2, 1}}};
  const double cx = std::cos(0.08), sx = std::sin(0.08), cy = std::cos(-0.04), sy = std::sin(-0.04),
               cz = std::cos(0.12), sz = std::sin(0.12);
  std::array<double, 16> expected{cz * cy,
                                  cz * sy * sx - sz * cx,
                                  cz * sy * cx + sz * sx,
                                  0.12,
                                  sz * cy,
                                  sz * sy * sx + cz * cx,
                                  sz * sy * cx - cz * sx,
                                  -0.08,
                                  -sy,
                                  cy * sx,
                                  cy * cx,
                                  0.05,
                                  0,
                                  0,
                                  0,
                                  1};
  for (std::size_t i = 0; i < points.size(); ++i) {
    std::array<double, 3> target{};
    for (int row = 0; row < 3; ++row)
      target[row] = expected[4 * row] * points[i][0] + expected[4 * row + 1] * points[i][1] +
                    expected[4 * row + 2] * points[i][2] + expected[4 * row + 3];
    Json item{{"id", std::to_string(i)},
              {"type", "point"},
              {"source", points[i]},
              {"target", target},
              {"weight", i + 1}};
    if (i == 4) {
      item["type"] = "line";
      item["direction"] = {0, 0, 1};
    }
    if (i >= 5) {
      item["type"] = "plane";
      item["direction"] = i == 5 ? Json{1, 0, 0} : Json{0, 1, 0};
      item["offset"] = 0.025;
      target[i == 5 ? 0 : 1] -= 0.025;
      item["target"] = target;
    }
    items.push_back(std::move(item));
  }
  Json graph = test::makeGraph(
      {{"constant", "register.constraints_3d", Json::object()},
       {"fit", "register.constrained_fit_3d", {{"maxRmse", 2e-6}, {"maxError", 3e-6}}}},
      {{"constant.constraints", "fit.constraints"}});
  graph["params"] = {
      {"constraintsJson",
       {{"type", "text"}, {"default", items.dump()}, {"binds", {"constant.items"}}}}};
  REQUIRE(ensureRegistry().find("register.constraints_3d") != nullptr);
  test::Session session(graph, {}, {}, false, 1, true);
  const auto& log = session.wait();
  REQUIRE_MESSAGE(log.runStatus() == "ok", Json(log.events).dump());
  CHECK(log.finalState("constant") == "done");
  CHECK(log.finalState("fit") == "done");
  Data fitted;
  REQUIRE(exec::ResultStore::instance().get(log.runId, "fit", "transform", fitted));
  REQUIRE(fitted.asTransform() != nullptr);
  for (std::size_t i = 0; i < expected.size(); ++i)
    CHECK(std::abs(fitted.asTransform()->m[i] - expected[i]) <= 2e-6);
  Json overridden = items;
  for (auto& item : overridden)
    item["target"][0] = item["target"][0].get<double>() + 0.05;
  exec::RunOptions options;
  options.runId = log.runId + "-constraints-override";
  options.maxParallel = 1;
  options.noReuse = true;
  options.paramsJson = Json{{"constraintsJson", overridden.dump()}}.dump();
  test::RunLog overriddenLog;
  exec::Run run(graph.dump(), options, &test::detail::collect, &overriddenLog);
  run.join();
  REQUIRE_MESSAGE(overriddenLog.runStatus() == "ok", Json(overriddenLog.events).dump());
  REQUIRE(exec::ResultStore::instance().get(options.runId, "fit", "transform", fitted));
  REQUIRE(fitted.asTransform() != nullptr);
  expected[3] += 0.05;
  for (std::size_t i = 0; i < expected.size(); ++i)
    CHECK(std::abs(fitted.asTransform()->m[i] - expected[i]) <= 2e-6);
}
