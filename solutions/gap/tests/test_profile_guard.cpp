#include <doctest/doctest.h>

#include "helpers.h"
#include "lyflow/profile.h"

using namespace lyflow;
namespace {
Data camera(const char* id) {
  PointCloud cloud;
  cloud.push(0, 0, 0);
  Profile result;
  std::string why;
  REQUIRE(makeProfile(std::make_shared<const PointCloud>(std::move(cloud)),
                      {1, "sensor", "m", "XZ", "right", "acquisition", id, 1}, {1}, {0}, result,
                      why));
  return Data::profile(std::move(result));
}
}

TEST_CASE(
    "Gap camera policy uses absolute median strictly above the threshold and keeps physical sources") {
  const auto a = camera("physical-A"), b = camera("physical-B");
  Record observation{"profile.OverlapComparison.v1",
                     {{"unit", "m"},
                      {"evaluated", true},
                      {"sampleCount", 20},
                      {"primarySourceId", "physical-A"},
                      {"secondarySourceId", "physical-B"},
                      {"signedMedian", -.0002},
                      {"absoluteMedian", .0025}}};
  test::OpCall call;
  call.inputs = {{"primary", a}, {"secondary", b}, {"observation", Data::record(observation)}};
  REQUIRE(call.run("gap.select_camera").ok);
  CHECK(call.out("quality").asRecord()->data["exceeded"] == true);
  CHECK(call.out("primary").profilePtr() == a.profilePtr());
  CHECK(call.out("secondary").profilePtr() == b.profilePtr());
  for (const auto* mode : {"keepPrimary", "keepSecondary"}) {
    CAPTURE(std::string(mode));
    REQUIRE(call.run("gap.select_camera", {{"onDisagree", Value::text(mode)}}).ok);
    const auto& chosen = std::string(mode) == "keepPrimary" ? a : b;
    CHECK(call.out("primary").profilePtr() == chosen.profilePtr());
    CHECK(call.out("secondary").profilePtr() == chosen.profilePtr());
    const auto& quality = call.out("quality").asRecord()->data;
    CHECK(quality["secondarySourceId"] == chosen.asProfile()->descriptor.sourceId);
  }
  CHECK(call.run("gap.select_camera", {{"onDisagree", Value::text("fail")}}).code ==
        "camera_disagree");
  CHECK(call.outputs.empty());
  REQUIRE(call.run("gap.select_camera",
                   {{"onDisagree", Value::text("fail")}, {"maxDeltaMm", Value::number(2.5)}})
              .ok);
  CHECK(call.out("quality").asRecord()->data["exceeded"] == false);
  CHECK(call.out("secondary").profilePtr() == b.profilePtr());

  observation.data["evaluated"] = false;
  observation.data["sampleCount"] = 1;
  observation.data.erase("signedMedian");
  observation.data.erase("absoluteMedian");
  call.inputs["observation"] = Data::record(observation);
  REQUIRE(call.run("gap.select_camera", {{"onDisagree", Value::text("keepPrimary")}}).ok);
  CHECK(call.out("quality").asRecord()->data["reason"] == "insufficient_overlap");
  CHECK(call.out("secondary").profilePtr() == b.profilePtr());
  call.inputs["secondary"] = a;
  CHECK_FALSE(call.run("gap.select_camera").ok);
  CHECK(call.outputs.empty());
  call.inputs["secondary"] = b;
  call.inputs.erase("observation");
  REQUIRE(call.run("gap.select_camera", {{"onDisagree", Value::text("off")}}).ok);
  CHECK(call.out("quality").asRecord()->data["evaluated"] == false);
  CHECK(call.out("secondary").profilePtr() == b.profilePtr());
}
