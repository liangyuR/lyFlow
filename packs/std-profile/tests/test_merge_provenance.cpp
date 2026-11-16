#include <doctest/doctest.h>

#include <limits>

#include "helpers.h"
#include "lyflow_profile/provenance.h"

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
  const auto beforeA = profile_contract::context(a), beforeB = profile_contract::context(b);
  test::OpCall call;
  call.inputs = {{"a", a}, {"b", b}};
  REQUIRE(call.run("profile.merge",
                   {{"roleA", Value::text("secondary")}, {"roleB", Value::text("primary")}})
              .ok);
  const auto& output = call.out("profile");
  const auto* profile = output.asProfile();
  REQUIRE(profile);
  CHECK(profile->descriptor.sourceCount == 16);
  CHECK(profile->sourceIndices == std::vector<std::uint32_t>{2, 5, 10, 13});
  CHECK(profile->validity == std::vector<std::uint8_t>{1, 0, 1, 0});
  CHECK(profile->cloud->intensity == std::vector<float>{7, 8, 7, 8});
  CHECK(profile->cloud->normals.size() == 12);
  CHECK(profile->cloud->rgb ==
        std::vector<std::uint8_t>{9, 10, 11, 12, 13, 14, 9, 10, 11, 12, 13, 14});
  CHECK(std::isnan(profile->cloud->xyz[3]));
  nlohmann::json sources, resolved;
  std::string why;
  const auto* provenance = call.out("provenance").asRecord();
  REQUIRE(profile_contract::profileSources(output, provenance, sources, why));
  CHECK(sources.size() == 3);
  CHECK(provenance->data.dump().size() < 4096);
  REQUIRE(profile_contract::resolveSourceIndex(output, provenance, 13, resolved, why));
  CHECK(resolved ==
        nlohmann::json{{"logicalRole", "primary"}, {"sourceId", "file-B"}, {"sourceIndex", 5}});
  resolved = "sentinel";
  CHECK_FALSE(profile_contract::resolveSourceIndex(output, provenance, 12, resolved, why));
  CHECK(resolved == "sentinel");
  CHECK(profile_contract::context(a) == beforeA);
  CHECK(profile_contract::context(b) == beforeB);
  const auto identity = profile->descriptor.sourceId;
  call.inputs = {{"a", scan("file-A")}, {"b", scan("file-B")}};
  REQUIRE(call.run("profile.merge",
                   {{"roleA", Value::text("secondary")}, {"roleB", Value::text("primary")}})
              .ok);
  CHECK(call.out("profile").asProfile()->descriptor.sourceId == identity);
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

TEST_CASE(
    "Profile guard duplicate inputs retain logical segment weight and nested merges flatten") {
  const auto a = scan("same-declared-source");
  test::OpCall call;
  call.inputs = {{"a", a}, {"b", a}};
  REQUIRE(call.run("profile.merge").ok);
  const auto first = call.out("profile"), provenance = call.out("provenance");
  CHECK(first.asProfile()->pointCount() == 4);
  CHECK(provenance.asRecord()->data["recipe"]["segments"].size() == 2);
  nlohmann::json sources;
  std::string why;
  REQUIRE(profile_contract::profileSources(first, provenance.asRecord(), sources, why));
  CHECK(sources.size() == 2);
  call.inputs = {{"a", first}, {"b", a}, {"aProvenance", provenance}};
  REQUIRE(call.run("profile.merge").ok);
  const auto* nested = call.out("provenance").asRecord();
  CHECK(nested->data["recipe"]["segments"].size() == 3);
  CHECK(nested->data["recipe"]["segments"][1]["logicalRole"] == "a/b");
  CHECK(nested->data["recipe"]["segments"][2]["offset"] == 16);
  CHECK(call.out("profile").asProfile()->sourceIndices ==
        std::vector<std::uint32_t>{2, 5, 10, 13, 18, 21});
  call.inputs.erase("aProvenance");
  CHECK_FALSE(call.run("profile.merge").ok);
  CHECK(call.outputs.empty());
}

TEST_CASE("Profile provenance rebinds actual subsets atomically and retains valid leaf records") {
  const auto source = scan("A"), subset = selected(source, {1});
  Record record;
  std::string why;
  REQUIRE(profile_contract::rebindProvenance(source, subset, nullptr, record, why));
  CHECK(record.type == "profile.CompositeSources.v1");
  CHECK(record.data["kind"] == "single_source");
  CHECK(record.data["recipe"]["segments"][0]["sourceContext"] == profile_contract::context(source));
  nlohmann::json sources;
  REQUIRE(profile_contract::profileSources(subset, &record, sources, why));
  CHECK(sources.size() == 2);
  Record next;
  REQUIRE(profile_contract::rebindProvenance(subset, selected(subset, {}), &record, next, why));
  CHECK(next.data["recipe"] == record.data["recipe"]);
  auto changed = *subset.asProfile();
  auto cloud = *changed.cloud;
  cloud.rgb[0] = 255;
  changed.cloud = std::make_shared<const PointCloud>(std::move(cloud));
  Record sentinel{"unchanged", {"sentinel", 1}};
  CHECK_FALSE(
      profile_contract::rebindProvenance(source, Data::profile(changed), nullptr, sentinel, why));
  CHECK(sentinel.type == "unchanged");
  Profile zero;
  REQUIRE(makeProfile(std::make_shared<const PointCloud>(),
                      {1, "measurement", "m", "XZ", "right", "acquisition", "zero", 0}, {}, {},
                      zero, why));
  const auto empty = Data::profile(std::move(zero));
  REQUIRE(profile_contract::rebindProvenance(empty, empty, nullptr, record, why));
  CHECK(record.data["recipe"]["segments"][0]["count"] == 0);
}

TEST_CASE(
    "Profile provenance strictly rejects altered content malformed recipes and forged binding") {
  const auto source = scan("A");
  Record record;
  std::string why;
  REQUIRE(profile_contract::rebindProvenance(source, source, nullptr, record, why));
  const auto bad = [&](Record altered, const Data& data) {
    nlohmann::json result = "sentinel";
    CHECK_FALSE(profile_contract::profileSources(data, &altered, result, why));
    CHECK(result == "sentinel");
  };
  auto altered = record;
  altered.data["profileContext"]["profileHash"] = std::string(32, '0');
  bad(altered, source);
  altered = record;
  altered.data["recipe"]["segments"][0]["offset"] = 1;
  altered =
      profile_contract::provenance_detail::record(source, "single_source", altered.data["recipe"]);
  bad(altered, source);
  altered = record;
  altered.data["extra"] = true;
  bad(altered, source);
  altered = record;
  altered.data["referenceSources"] = nlohmann::json::array({profile_contract::context(scan("B"))});
  bad(altered, source);
  altered = record;
  altered.data["recipe"]["segments"][0]["count"] = -1;
  bad(altered, source);
  bad(record, scan("different"));
}

TEST_CASE(
    "Profile reference provenance keeps original units and includes current context in budget") {
  const auto source = scan("A");
  Record leaf, extended;
  std::string why;
  REQUIRE(profile_contract::rebindProvenance(source, source, nullptr, leaf, why));
  auto reference = *scan("reference").asProfile();
  reference.descriptor.unit = "mm";
  const auto ref = profile_contract::context(Data::profile(std::move(reference)));
  REQUIRE(profile_contract::extendProvenanceSources(source, leaf, nlohmann::json::array({ref, ref}),
                                                    extended, why));
  nlohmann::json sources;
  REQUIRE(profile_contract::profileSources(source, &extended, sources, why));
  CHECK(sources.size() == 2);
  CHECK(extended.data["referenceSources"].size() == 1);
  nlohmann::json many = nlohmann::json::array();
  for (int i = 0; i < 63; ++i)
    many.push_back(profile_contract::context(scan("reference-" + std::to_string(i))));
  REQUIRE(profile_contract::extendProvenanceSources(source, leaf, many, extended, why));
  REQUIRE(profile_contract::profileSources(source, &extended, sources, why));
  CHECK(sources.size() == 64);
  Record sentinel{"unchanged", {}};
  CHECK_FALSE(profile_contract::extendProvenanceSources(
      source, extended, nlohmann::json::array({ref}), sentinel, why));
  CHECK(sentinel.type == "unchanged");
  test::OpCall call;
  call.inputs = {{"a", source}, {"aProvenance", Data::record(extended)}, {"b", scan("B")}};
  CHECK_FALSE(call.run("profile.merge").ok);
  CHECK(call.outputs.empty());
}

TEST_CASE(
    "Profile merge rejects mismatched space roles huge source ranges and cancellation atomically") {
  const auto source = scan("A");
  test::OpCall call;
  call.inputs = {{"a", source}, {"b", scan("B")}};
  CHECK_FALSE(call.run("profile.merge", {{"roleB", Value::text("a")}}).ok);
  CHECK(call.outputs.empty());
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
