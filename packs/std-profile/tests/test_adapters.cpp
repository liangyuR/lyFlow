#include <doctest/doctest.h>

#include <cmath>
#include <limits>

#include "helpers.h"
#include "lyflow/profile.h"

using namespace lyflow;

namespace {
ParamMap explicitSource() {
  return {{"frame", Value::text("sensor")},
          {"unit", Value::text("mm")},
          {"plane", Value::text("XZ")},
          {"handedness", Value::text("right")},
          {"sampleOrder", Value::text("acquisition")},
          {"sourceId", Value::text("scan-1")}};
}
}

TEST_CASE("Profile adapters: preserve acquisition slots and shared read-only channels") {
  PointCloud cloud;
  for (int i = 0; i < 1280; ++i)
    cloud.push(static_cast<float>(i), 0, i == 120 ? std::numeric_limits<float>::quiet_NaN() : 5);
  cloud.intensity.assign(1280, 0.75f);
  cloud.normals.assign(3840, 0.0f);
  cloud.rgb.assign(3840, 23);
  test::OpCall call;
  const auto data = Data::cloud(std::move(cloud));
  call.inputs["cloud"] = data;
  REQUIRE(call.run("profile.from_cloud", explicitSource()).ok);
  const auto* profile = call.out("profile").asProfile();
  REQUIRE(profile);
  CHECK(profile->cloud == data.cloudPtr());
  CHECK(profile->pointCount() == 1280);
  CHECK(profile->sourceIndices[120] == 120);
  CHECK(profile->validity[120] == 0);
  CHECK(profile->validity[121] == 1);
  CHECK(profile->cloud->intensity.size() == 1280);
  CHECK(profile->cloud->normals.size() == 3840);
  CHECK(profile->cloud->rgb.size() == 3840);
  CHECK(std::isnan(data.asCloud()->xyz[362]));
  CHECK_FALSE(call.validate("profile.from_cloud", {}, {"cloud"}).empty());
}

TEST_CASE("Profile adapters: explicit reflection round trip preserves source mapping") {
  PointCloud cloud;
  cloud.push(1, 2, 3);
  cloud.normals = {0, 1, 0};
  test::OpCall input;
  input.inputs["cloud"] = Data::cloud(std::move(cloud));
  REQUIRE(input.run("profile.from_cloud", explicitSource()).ok);
  const Data original = input.out("profile");
  test::OpCall remap;
  remap.inputs["profile"] = original;
  REQUIRE(remap
              .run("profile.remap_axes",
                   {{"axes", Value::text("XZY")}, {"targetFrame", Value::text("measurement")}})
              .ok);
  const Data mapped = remap.out("profile");
  CHECK(mapped.asProfile()->descriptor.plane == "XY");
  CHECK(mapped.asProfile()->descriptor.handedness == "reflected");
  CHECK(mapped.asProfile()->cloud->xyz == std::vector<float>{1, 3, 2});
  CHECK(mapped.asProfile()->cloud->normals == std::vector<float>{0, 0, 1});
  remap.inputs["profile"] = mapped;
  REQUIRE(remap
              .run("profile.remap_axes",
                   {{"axes", Value::text("XZY")}, {"targetFrame", Value::text("sensor")}})
              .ok);
  CHECK(remap.out("profile").asProfile()->cloud->xyz == original.asProfile()->cloud->xyz);
  CHECK(remap.out("profile").asProfile()->descriptor.handedness == "right");
  CHECK(remap.out("profile").asProfile()->sourceIndices == original.asProfile()->sourceIndices);
  CHECK(original.asProfile()->descriptor.plane == "XZ");
  CHECK_FALSE(remap
                  .run("profile.remap_axes",
                       {{"targetFrame", Value::text("bad")}, {"signs", Value::vec({1, 2, 1})}})
                  .ok);
  CHECK(remap.outputs.empty());
  Profile direct;
  std::string why;
  REQUIRE(remapProfileAxes(*original.asProfile(), "XZY", {-1, 1, -1}, "measurement", direct, why));
  CHECK(direct.cloud->xyz == std::vector<float>{-1, 3, -2});
  CHECK(direct.cloud->normals == std::vector<float>{0, 0, -1});
  CHECK(direct.descriptor.plane == "XY");
  CHECK(direct.descriptor.handedness == "reflected");
  CHECK(direct.sourceIndices == original.asProfile()->sourceIndices);
  const auto retained = direct.cloud;
  for (const auto* axes : {"XXY", "XY", "xyz"}) {
    CHECK_FALSE(
        remapProfileAxes(*original.asProfile(), axes, {1, 1, 1}, "measurement", direct, why));
    CHECK(direct.cloud == retained);
  }
  CHECK_FALSE(
      remapProfileAxes(*original.asProfile(), "XYZ", {1, 0, 1}, "measurement", direct, why));
  CHECK_FALSE(remapProfileAxes(*original.asProfile(), "XYZ", {1, 1, 1}, "", direct, why));
  CHECK_FALSE(remapProfileAxes(*original.asProfile(), "XZY", {1, 1, 1}, "measurement", direct, why,
                               [] { return true; }));
  CHECK(direct.cloud == retained);
}

TEST_CASE(
    "Profile adapters: bare cloud masks invalid finite slots without losing original profile") {
  PointCloud cloud;
  cloud.push(1000, 2000, 3000);
  cloud.push(4000, 5000, 6000);
  cloud.normals = {1, 0, 0, 0, 1, 0};
  Profile profile;
  ProfileDescriptor d;
  d.frame = "sensor";
  d.unit = "mm";
  d.plane = "XZ";
  d.handedness = "right";
  d.sampleOrder = "acquisition";
  d.sourceId = "original";
  d.sourceCount = 30;
  std::string why;
  REQUIRE(makeProfile(std::make_shared<const PointCloud>(std::move(cloud)), d, {1, 0}, {10, 20},
                      profile, why));
  test::OpCall call;
  call.inputs["profile"] = Data::profile(profile);
  REQUIRE(call.run("profile.to_cloud").ok);
  CHECK(call.out("cloud").asCloud()->xyz[0] == doctest::Approx(1));
  CHECK(std::isnan(call.out("cloud").asCloud()->xyz[3]));
  const auto* converted = call.out("profile").asProfile();
  REQUIRE(converted);
  CHECK(converted->cloud->xyz[3] == doctest::Approx(4));
  CHECK(converted->cloud->normals == profile.cloud->normals);
  CHECK(converted->validity == profile.validity);
  CHECK(converted->sourceIndices == profile.sourceIndices);
  CHECK(converted->descriptor.sourceId == "original");
  CHECK(profile.cloud->xyz[3] == 4000);
  CHECK(call.out("descriptor").asRecord()->data.size() == 8);
}

TEST_CASE("Profile injection: provided output does not demand unused adapter input or parameters") {
  PointCloud cloud;
  cloud.push(1, 2, 3);
  test::OpCall create;
  create.inputs["cloud"] = Data::cloud(std::move(cloud));
  REQUIRE(create.run("profile.from_cloud", explicitSource()).ok);
  const auto graph = test::makeGraph({{"provided", "profile.from_cloud"}}, {});
  test::Session run(graph, {}, {}, false, 0, false,
                    {{"provided", "profile", create.out("profile")}});
  const auto result = run.wait();
  INFO(result.events);
  CHECK(result.runStatus() == "ok");
}
