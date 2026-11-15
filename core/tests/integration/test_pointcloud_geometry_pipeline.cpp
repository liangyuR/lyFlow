#include <doctest/doctest.h>

#include "helpers.h"

using namespace lyflow;
using namespace lyflow::test;

TEST_CASE("体素栅格：坐标相对叶大小过大时报错而不是静默算错") {
  const Json doc = makeGraph(
      {
          {"g", "gen.synthetic", Json{{"pointCount", 100}}},
          {"t", "transform.make", Json{{"translation", {1e30, 0.0, 0.0}}}},
          {"a", "transform.apply"},
          {"v", "filter.voxel_grid"},
      },
      {
          {"g.cloud", "a.cloud"},
          {"t.transform", "a.transform"},
          {"a.cloud", "v.cloud"},
      });
  const RunLog log = runGraph(doc);
  CHECK(log.finalState("v") == "error");
  const Json e = log.nodeEvent("v", "error");
  REQUIRE(e.contains("errors"));
  CHECK(e["errors"][0]["paramPath"] == "leafSize");
}
