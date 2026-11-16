#include <doctest/doctest.h>

#include <cmath>
#include <limits>

#include "helpers.h"
#include "lyflow/profile.h"
#include "lyflow_profile/context.h"

using namespace lyflow;
namespace {
Profile fixture(const char* unit = "mm") {
  PointCloud cloud;
  for (int i = 0; i < 4; ++i) {
    cloud.push(static_cast<float>(i), 5, static_cast<float>(2 * i + 3));
    cloud.intensity.push_back(static_cast<float>(10 + i));
    cloud.normals.insert(cloud.normals.end(), {0, 0, 1});
    cloud.rgb.insert(cloud.rgb.end(), {1, 2, 3});
  }
  Profile result;
  std::string why;
  REQUIRE(makeProfile(std::make_shared<const PointCloud>(cloud),
                      {1, "measurement", unit, "XZ", "right", "acquisition", "scan-A", 12},
                      {1, 1, 1, 1}, {1, 3, 7, 9}, result, why));
  return result;
}
}

TEST_CASE(
    "Profile context binds descriptors all channels masks and source indices across carriers") {
  const auto original = fixture();
  const auto input = Data::profile(original);
  const auto context = profile_contract::context(input);
  CHECK(profile_contract::fingerprint(context["profileHash"]));
  std::string why;
  REQUIRE(profile_contract::matches(input, context, why));
  auto copied = original;
  PointCloud copy = *original.cloud;
  PointCloud identity;
  copy.id = identity.id;
  copied.cloud = std::make_shared<const PointCloud>(copy);
  REQUIRE(profile_contract::matches(Data::profile(copied), context, why));
  copied.validity[1] = 0;
  CHECK_FALSE(profile_contract::matches(Data::profile(copied), context, why));
  copied = original;
  copied.sourceIndices[1] = 4;
  CHECK_FALSE(profile_contract::matches(Data::profile(copied), context, why));
  copied = original;
  copied.descriptor.frame = "other";
  CHECK_FALSE(profile_contract::matches(Data::profile(copied), context, why));
  for (int channel = 0; channel < 4; ++channel) {
    PointCloud changed = *original.cloud;
    if (channel == 0)
      changed.xyz[0] += 1;
    if (channel == 1)
      changed.intensity[0] += 1;
    if (channel == 2)
      changed.normals[0] += 1;
    if (channel == 3)
      changed.rgb[0] += 1;
    copied = original;
    copied.cloud = std::make_shared<const PointCloud>(changed);
    CHECK_FALSE(profile_contract::matches(Data::profile(copied), context, why));
  }
  auto malformed = context;
  malformed["extra"] = true;
  CHECK_FALSE(profile_contract::matches(input, malformed, why));
  malformed = context;
  malformed["schemaVersion"] = 2;
  CHECK_FALSE(profile_contract::matches(input, malformed, why));
  CHECK_FALSE(profile_contract::matches(Data{}, context, why));
}

TEST_CASE(
    "Native profile TLS uses declared planes SI conversion original context and delivered geometry") {
  const auto input = Data::profile(fixture());
  test::OpCall call;
  call.inputs = {{"profile", input}};
  REQUIRE_MESSAGE(call.run("profile.fit_line").ok, call.status.message);
  const auto line = call.out("line");
  const auto stats = *call.out("statistics").asRecord();
  CHECK(stats.type == "profile.LineFit.v1");
  CHECK(stats.data["count"] == 4);
  CHECK(stats.data["unit"] == "m");
  CHECK(stats.data["plane"] == "XZ");
  CHECK(stats.data["context"] == profile_contract::context(input));
  CHECK(stats.data["geometryHash"] == profile_contract::geometryHash(line));
  CHECK(call.out("lineContext").asRecord()->type == "profile.Geometry.v1");
  CHECK(call.out("lineContext").asRecord()->data == profile_contract::geometryContext(input, line));
  Profile metric;
  std::string why;
  REQUIRE(convertProfileUnit(*input.asProfile(), "m", metric, why));
  call.inputs = {{"cloud", Data::cloud(metric.cloud)}};
  REQUIRE(call.run("profile.analyze", {{"frame", Value::text("measurement")}}).ok);
  CHECK(profile_contract::geometryHash(call.out("line")) == profile_contract::geometryHash(line));
  CHECK(call.out("statistics").asRecord()->data["rms"] == stats.data["rms"]);
  call.inputs = {{"profile", input}};
  REQUIRE(call.run("profile.fit_line",
                   {{"rangeEnabled", Value::boolean(true)}, {"range", Value::vec({0, 0.0021})}})
              .ok);
  CHECK(call.out("statistics").asRecord()->data["count"] == 3);
  CHECK_FALSE(call.run("profile.fit_line", {{"rangeEnabled", Value::boolean(true)},
                                            {"range", Value::vec({0.002, 0.001})}})
                  .ok);
  CHECK(input.asProfile()->descriptor.unit == "mm");
  CHECK(input.asProfile()->sourceIndices == std::vector<std::uint32_t>{1, 3, 7, 9});
}

TEST_CASE(
    "Native profile invalid policy selects masked samples without changing source slots or optional channels") {
  auto input = fixture();
  PointCloud cloud = *input.cloud;
  input.validity[1] = 0;
  cloud.xyz[4] = std::numeric_limits<float>::quiet_NaN();
  cloud.intensity[1] = std::numeric_limits<float>::quiet_NaN();
  input.cloud = std::make_shared<const PointCloud>(cloud);
  const auto data = Data::profile(input);
  test::OpCall call;
  call.inputs = {{"profile", data}};
  CHECK_FALSE(call.run("profile.fit_line").ok);
  REQUIRE_MESSAGE(call.run("profile.fit_line", {{"invalidPolicy", Value::text("valid_only")}}).ok,
                  call.status.message);
  CHECK(call.out("statistics").asRecord()->data["count"] == 3);
  CHECK(data.asProfile()->pointCount() == 4);
  CHECK(data.asProfile()->validity[1] == 0);
  CHECK(std::isnan(data.asProfile()->cloud->xyz[4]));
  CHECK(std::isnan(data.asProfile()->cloud->intensity[1]));
  cloud.intensity[2] = std::numeric_limits<float>::quiet_NaN();
  input.cloud = std::make_shared<const PointCloud>(cloud);
  call.inputs = {{"profile", Data::profile(input)}};
  CHECK_FALSE(call.run("profile.fit_line", {{"invalidPolicy", Value::text("valid_only")}}).ok);
  CHECK(call.status.message == "点云强度必须有限");
  Line2D geometry;
  geometry.start[0] = std::numeric_limits<float>::quiet_NaN();
  const auto unused = Data::line2d(geometry);
  geometry.start[0] = 7;
  CHECK(profile_contract::geometryHash(unused) ==
        profile_contract::geometryHash(Data::line2d(geometry)));
}

TEST_CASE(
    "Native pair transformation composes noncommuting SI extrinsics and retains independent source contracts") {
  auto master = fixture();
  master.descriptor.frame = "master-sensor";
  auto slave = fixture();
  std::string why;
  Profile metric;
  REQUIRE(convertProfileUnit(slave, "m", metric, why));
  slave = metric;
  slave.descriptor.frame = "slave-sensor";
  slave.descriptor.sourceId = "scan-B";
  slave.descriptor.sampleOrder = "unknown";
  slave.descriptor.handedness = "reflected";
  slave.sourceIndices = {9, 3, 7, 1};
  const auto masterData = Data::profile(master), slaveData = Data::profile(slave);
  Transform masterToCommon, slaveToCommon, commonToOutput;
  masterToCommon.m[3] = 1;
  slaveToCommon.m[11] = 2;
  commonToOutput.m[0] = 0;
  commonToOutput.m[2] = 1;
  commonToOutput.m[8] = -1;
  commonToOutput.m[10] = 0;
  test::OpCall call;
  call.inputs = {{"master", masterData},
                 {"slave", slaveData},
                 {"masterToCommon", Data::transform(masterToCommon)},
                 {"slaveToCommon", Data::transform(slaveToCommon)},
                 {"commonToOutput", Data::transform(commonToOutput)}};
  CHECK_FALSE(call.run("profile.transform_pair").ok);
  const std::unordered_map<std::string, Value> params{{"commonFrame", Value::text("common")},
                                                      {"outputFrame", Value::text("measurement")},
                                                      {"targetPlane", Value::text("XY")}};
  REQUIRE_MESSAGE(call.run("profile.transform_pair", params).ok, call.status.message);
  const auto* movedMaster = call.out("master").asProfile();
  const auto* movedSlave = call.out("slave").asProfile();
  REQUIRE(movedMaster);
  REQUIRE(movedSlave);
  for (std::size_t i = 0; i < master.pointCount(); ++i) {
    CHECK(movedMaster->cloud->xyz[i * 3] == doctest::Approx(master.cloud->xyz[i * 3 + 2] * 0.001));
    CHECK(movedMaster->cloud->xyz[i * 3 + 2] ==
          doctest::Approx(-master.cloud->xyz[i * 3] * 0.001 - 1));
    CHECK(movedSlave->cloud->xyz[i * 3] == doctest::Approx(slave.cloud->xyz[i * 3 + 2] + 2));
    CHECK(movedSlave->cloud->xyz[i * 3 + 2] == doctest::Approx(-slave.cloud->xyz[i * 3]));
    CHECK(movedSlave->cloud->normals[i * 3] == 1);
  }
  CHECK(movedMaster->descriptor.unit == "m");
  CHECK(movedMaster->descriptor.frame == "measurement");
  CHECK(movedMaster->descriptor.plane == "XY");
  CHECK(movedSlave->descriptor.sourceId == "scan-B");
  CHECK(movedSlave->descriptor.sourceCount == 12);
  CHECK(movedSlave->descriptor.handedness == "reflected");
  CHECK(movedSlave->descriptor.sampleOrder == "unknown");
  CHECK(movedSlave->sourceIndices == slave.sourceIndices);
  CHECK(movedSlave->cloud->intensity == slave.cloud->intensity);
  CHECK(movedMaster->cloud->rgb == master.cloud->rgb);
  CHECK(profile_contract::matches(
      call.out("master"), call.out("metadata").asRecord()->data["masterOutputContext"], why));
  CHECK(masterData.asProfile()->descriptor.unit == "mm");
  CHECK(masterData.asProfile()->cloud->xyz == master.cloud->xyz);
  call.inputs.erase("commonToOutput");
  CHECK_FALSE(call.run("profile.transform_pair", params).ok);
}

TEST_CASE(
    "Native pair valid_only preserves SI invalid payloads NaN masks and mapping without reviving slots") {
  auto input = fixture();
  PointCloud cloud = *input.cloud;
  input.validity = {1, 0, 0, 1};
  cloud.xyz[8] = std::numeric_limits<float>::quiet_NaN();
  cloud.normals[6] = std::numeric_limits<float>::quiet_NaN();
  cloud.intensity[2] = std::numeric_limits<float>::quiet_NaN();
  input.cloud = std::make_shared<const PointCloud>(cloud);
  const auto data = Data::profile(input);
  Transform moved;
  moved.m[3] = 1;
  test::OpCall call;
  call.inputs = {{"master", data},
                 {"slave", data},
                 {"masterToCommon", Data::transform(moved)},
                 {"slaveToCommon", Data::transform(moved)}};
  std::unordered_map<std::string, Value> params{{"commonFrame", Value::text("measurement")},
                                                {"outputFrame", Value::text("measurement")},
                                                {"targetPlane", Value::text("XZ")}};
  CHECK_FALSE(call.run("profile.transform_pair", params).ok);
  params["invalidPolicy"] = Value::text("valid_only");
  REQUIRE_MESSAGE(call.run("profile.transform_pair", params).ok, call.status.message);
  const auto* result = call.out("master").asProfile();
  REQUIRE(result);
  CHECK(result->pointCount() == 4);
  CHECK(result->validity == input.validity);
  CHECK(result->sourceIndices == input.sourceIndices);
  CHECK(result->cloud->xyz[0] == 1);
  CHECK(result->cloud->xyz[3] == doctest::Approx(0.001));
  CHECK(result->cloud->xyz[6] == doctest::Approx(0.002));
  CHECK(std::isnan(result->cloud->xyz[8]));
  CHECK(std::isnan(result->cloud->normals[6]));
  CHECK(std::isnan(result->cloud->intensity[2]));
  CHECK(call.out("metadata").asRecord()->data["invalidSlots"] ==
        "preserved_si_untransformed_not_observations");
  CHECK(std::isnan(data.asProfile()->cloud->xyz[8]));
  CHECK(data.asProfile()->cloud->xyz[3] == 1);
}
