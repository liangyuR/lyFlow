#include <doctest/doctest.h>

#include <limits>

#include "helpers.h"
#include "lyflow/profile.h"

using namespace lyflow;

namespace {
Data scan(const std::string& id, bool channels = true, std::uint64_t sourceCount = 8) {
  PointCloud cloud;
  cloud.push(1, 0, 3);
  const auto nan = std::numeric_limits<float>::quiet_NaN();
  cloud.push(nan, nan, nan);
  if (channels) {
    cloud.intensity = {7, 8};
    cloud.normals = {0, 1, 0, 1, 0, 0};
    cloud.rgb = {9, 10, 11, 12, 13, 14};
  }
  Profile profile;
  std::string why;
  REQUIRE(makeProfile(std::make_shared<const PointCloud>(std::move(cloud)),
                      {1, "measurement", "m", "XZ", "right", "acquisition", id, sourceCount},
                      {1, 0}, {2, 5}, profile, why));
  return Data::profile(std::move(profile));
}
Data selected(const Data& data, const std::vector<std::int32_t>& keep) {
  const auto& original = *data.asProfile();
  Profile result;
  result.descriptor = original.descriptor;
  result.cloud = std::make_shared<const PointCloud>(original.cloud->select(keep));
  for (const auto i : keep) {
    result.validity.push_back(original.validity[i]);
    result.sourceIndices.push_back(original.sourceIndices[i]);
  }
  return Data::profile(std::move(result));
}
struct Stop : ExecContext {
  mutable int calls = 0;
  int at = 1;
  std::filesystem::path base;
  bool cancelled() const override { return ++calls >= at; }
  void progress(float, std::string_view) override {}
  void log(LogLevel, std::string) override {}
  const std::filesystem::path& baseDir() const override { return base; }
  int threadBudget() const override { return 1; }
};
} // namespace

TEST_CASE("Profile merge preserves invalid slots all channels and disjoint source index spaces") {
  const auto a = scan("file-A"), b = scan("file-B");
  test::OpCall call;
  call.inputs = {{"a", a}, {"b", b}};
  REQUIRE(call.run("profile.merge").ok);
  const auto& output = call.out("profile");
  const auto* profile = output.asProfile();
  REQUIRE(profile);
  CHECK(profile->descriptor.sourceCount == 16);
  CHECK(profile->descriptor.sourceId == "derived:composite/file-A+file-B");
  CHECK(profile->sourceIndices == std::vector<std::uint32_t>{2, 5, 10, 13});
  CHECK(profile->validity == std::vector<std::uint8_t>{1, 0, 1, 0});
  CHECK(profile->cloud->intensity == std::vector<float>{7, 8, 7, 8});
  CHECK(profile->cloud->normals.size() == 12);
  CHECK(profile->cloud->rgb ==
        std::vector<std::uint8_t>{9, 10, 11, 12, 13, 14, 9, 10, 11, 12, 13, 14});
  CHECK(std::isnan(profile->cloud->xyz[3]));
}

TEST_CASE("Profile merge makes mixed channels explicit and treats empty inputs as neutral") {
  const auto a = scan("A"), b = scan("B", false);
  test::OpCall call;
  call.inputs = {{"a", a}, {"b", b}};
  CHECK_FALSE(call.run("profile.merge").ok);
  CHECK(call.outputs.empty());
  REQUIRE(call.run("profile.merge", {{"channelPolicy", Value::text("intersection")}}).ok);
  CHECK_FALSE(call.out("profile").asProfile()->cloud->hasRgb());
  CHECK(call.out("info").asRecord()->data["droppedChannels"] ==
        nlohmann::json::array({"intensity", "normals", "rgb"}));
  call.inputs["b"] = selected(b, {});
  REQUIRE(call.run("profile.merge").ok);
  CHECK(call.out("profile").asProfile()->cloud->rgb == a.asProfile()->cloud->rgb);
  CHECK(call.out("profile").asProfile()->descriptor.sourceCount == 16);
  call.inputs["a"] = selected(a, {});
  REQUIRE(call.run("profile.merge").ok);
  CHECK(call.out("profile").asProfile()->pointCount() == 0);
  CHECK(call.out("profile").asProfile()->descriptor.sourceCount == 16);
}

TEST_CASE("Profile merge flattens composite parents into a readable source identity") {
  const auto a = scan("a"), b = scan("b"), c = scan("c");
  test::OpCall call;
  call.inputs = {{"a", b}, {"b", c}};
  REQUIRE(call.run("profile.merge").ok);
  const auto bc = call.out("profile");
  CHECK(bc.asProfile()->descriptor.sourceId == "derived:composite/b+c");
  call.inputs = {{"a", a}, {"b", bc}};
  REQUIRE(call.run("profile.merge").ok);
  const auto* nested = call.out("profile").asProfile();
  CHECK(nested->descriptor.sourceId == "derived:composite/a+b+c");
  CHECK(nested->descriptor.sourceCount == 24);
  CHECK(nested->sourceIndices == std::vector<std::uint32_t>{2, 5, 10, 13, 18, 21});
}

TEST_CASE("Profile merge rejects mismatched space huge source ranges and cancellation atomically") {
  const auto source = scan("A");
  test::OpCall call;
  call.inputs = {{"a", source}, {"b", scan("B")}};
  auto other = *source.asProfile();
  for (const auto* field : {"frame", "plane", "handedness", "unit"}) {
    other = *source.asProfile();
    if (std::string(field) == "frame")
      other.descriptor.frame = "other";
    if (std::string(field) == "plane")
      other.descriptor.plane = "XY";
    if (std::string(field) == "handedness")
      other.descriptor.handedness = "reflected";
    if (std::string(field) == "unit")
      other.descriptor.unit = "mm";
    call.inputs["b"] = Data::profile(other);
    CHECK_FALSE(call.run("profile.merge").ok);
    CHECK(call.outputs.empty());
  }
  call.inputs["b"] = scan("huge", true, std::numeric_limits<std::uint32_t>::max());
  CHECK_FALSE(call.run("profile.merge").ok);
  CHECK(call.outputs.empty());
  call.inputs["b"] = source;
  const auto* op = call.registry->find("profile.merge");
  REQUIRE(op);
  ParamMap params;
  for (const auto& p : op->params)
    params[p.name] = p.def;
  for (const auto at : {1, 3, 4}) {
    Stop ctx;
    ctx.at = at;
    Inputs inputs(call.inputs);
    Outputs outputs(call.outputs);
    ParamView view(params, ctx.base);
    CHECK_FALSE(op->compute(inputs, view, outputs, ctx).ok);
    CHECK(call.outputs.empty());
  }
}
