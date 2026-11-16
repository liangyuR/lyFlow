#include "helpers.h"
#include "exec/disk_cache.h"
#include "lyflow/contract.h"
#include "lyflow/profile.h"
#include "runtime_frames.h"

#include <cstring>
#include <limits>
#include <numeric>

using namespace lyflow;
using namespace lyflow::test;

namespace {
Profile fixture(std::size_t count = 1280) {
  PointCloud cloud;
  for (std::size_t i = 0; i < count; ++i) {
    cloud.push(static_cast<float>(i), 0, static_cast<float>(i) * 0.5f);
    cloud.intensity.push_back(static_cast<float>(i) / 1280);
    cloud.normals.insert(cloud.normals.end(), {0, 1, 0});
    cloud.rgb.insert(cloud.rgb.end(), {1, 2, 3});
  }
  Profile profile;
  profile.cloud = std::make_shared<const PointCloud>(std::move(cloud));
  profile.descriptor = {1, "sensor-a", "mm", "XZ", "right", "acquisition", "scan-a", count};
  profile.validity.assign(count, 1);
  profile.sourceIndices.resize(count);
  std::iota(profile.sourceIndices.begin(), profile.sourceIndices.end(), 0);
  return profile;
}

Profile holeProfile() {
  auto profile = fixture();
  PointCloud cloud = *profile.cloud;
  cloud.xyz[777 * 3] = std::numeric_limits<float>::quiet_NaN();
  profile.validity[777] = 0;
  profile.cloud = std::make_shared<const PointCloud>(std::move(cloud));
  return profile;
}

std::string owned(char* raw) {
  REQUIRE(raw != nullptr);
  std::string text(raw);
  lyflow_string_free(raw);
  return text;
}
} // namespace

TEST_CASE("Profile metadata and slots are explicit, bounded and atomic") {
  const auto profile = holeProfile();
  REQUIRE(profile.consistent());
  auto metadata = profileDescriptorJson(profile.descriptor);
  CHECK(metadata.size() == 8);
  ProfileDescriptor parsed;
  std::string why;
  REQUIRE(parseProfileDescriptor(metadata, parsed, why));
  CHECK(profileDescriptorJson(parsed) == metadata);
  for (const auto& field : {"unit", "plane", "handedness", "sampleOrder", "frame", "sourceId"}) {
    auto invalid = metadata;
    invalid[field] = "";
    CHECK_FALSE(parseProfileDescriptor(invalid, parsed, why));
    CHECK(profileDescriptorJson(parsed) == metadata);
  }
  for (const Json& count : {Json(-1), Json(1.5), Json("1280"), Json(true), Json(4294967296ull)}) {
    auto invalid = metadata;
    invalid["sourceCount"] = count;
    CHECK_FALSE(parseProfileDescriptor(invalid, parsed, why));
  }
  auto extra = metadata;
  extra["guessUnit"] = true;
  CHECK_FALSE(parseProfileDescriptor(extra, parsed, why));
  auto invalid = profile;
  invalid.validity.pop_back();
  CHECK_FALSE(invalid.consistent());
  invalid = profile;
  invalid.validity[777] = 1;
  CHECK_FALSE(invalid.consistent());
  invalid = profile;
  invalid.validity[0] = 2;
  CHECK_FALSE(invalid.consistent());
  invalid = profile;
  invalid.sourceIndices[1] = 0;
  CHECK_FALSE(invalid.consistent());
  invalid = profile;
  invalid.sourceIndices.back() = 1280;
  CHECK_FALSE(invalid.consistent());
  invalid = profile;
  std::swap(invalid.sourceIndices[0], invalid.sourceIndices[1]);
  CHECK_FALSE(invalid.consistent());
  invalid.descriptor.sampleOrder = "unknown";
  CHECK(invalid.consistent());
  invalid.sourceIndices[1] = invalid.sourceIndices[0];
  CHECK_FALSE(invalid.consistent());
  invalid = profile;
  PointCloud bad = *invalid.cloud;
  bad.xyz[777 * 3] = std::numeric_limits<float>::infinity();
  invalid.cloud = std::make_shared<const PointCloud>(std::move(bad));
  CHECK_FALSE(invalid.consistent());
  auto empty = fixture(0);
  CHECK(empty.consistent());
  empty.descriptor.sourceCount = 1280;
  CHECK(empty.consistent());
  empty.cloud.reset();
  CHECK_FALSE(empty.consistent());
}

TEST_CASE("Profile shares read-only payload and explicit unit conversion preserves slots") {
  auto input = holeProfile();
  const auto original = input.cloud;
  auto data = Data::profile(input);
  auto copy = data;
  CHECK(copy.profilePtr() == data.profilePtr());
  CHECK(copy.asProfile()->cloud == original);
  CHECK(copy.asCloud() == nullptr);
  CHECK(copy.typeName() == std::string("Profile"));
  CHECK(kindFromTypeName("Profile") == Data::Kind::Profile);
  CHECK(copy.elementCount() == 1280);
  CHECK(copy.byteSize() > Data::cloud(original).byteSize() + 1280 * 5);
  const auto summary = Json::parse(copy.valueJson());
  CHECK(summary["validCount"] == 1279);
  CHECK(summary["descriptor"]["unit"] == "mm");
  CHECK_FALSE(summary.contains("sourceIndices"));
  CHECK_FALSE(summary.contains("xyz"));
  CHECK(copy.valueJson().size() < 8192);
  Profile output;
  std::string why;
  REQUIRE(convertProfileUnit(input, "mm", output, why));
  CHECK(output.cloud == original);
  REQUIRE(convertProfileUnit(input, "m", output, why));
  CHECK(output.cloud != original);
  CHECK(output.cloud->id != original->id);
  CHECK(output.cloud->xyz[3] == doctest::Approx(0.001));
  CHECK(original->xyz[3] == 1);
  CHECK(std::isnan(output.cloud->xyz[777 * 3]));
  CHECK(output.cloud->intensity == original->intensity);
  CHECK(output.cloud->normals == original->normals);
  CHECK(output.cloud->rgb == original->rgb);
  CHECK(output.sourceIndices == input.sourceIndices);
  CHECK(output.validity == input.validity);
  CHECK(output.descriptor.plane == "XZ");
  Profile roundtrip;
  REQUIRE(convertProfileUnit(output, "mm", roundtrip, why));
  CHECK(roundtrip.cloud->xyz[3] == doctest::Approx(original->xyz[3]));
  const auto previous = output.cloud;
  CHECK_FALSE(convertProfileUnit(input, "px", output, why));
  CHECK(output.cloud == previous);
  const auto large = fixture(10000);
  int cancelChecks = 0;
  CHECK_FALSE(convertProfileUnit(large, "m", output, why, [&] { return ++cancelChecks == 3; }));
  CHECK(why == "Profile单位转换已取消");
  CHECK(cancelChecks == 3);
  CHECK(output.cloud == previous);
  CHECK(large.cloud->xyz[4096 * 3] == 4096);
  CHECK(large.descriptor.unit == "mm");
  Json expected, actual;
  CHECK_FALSE(checkPortContract({{"finite", true}}, data, expected, actual, why));
  CHECK(checkPortContract({{"elementCount", {{"eq", 1280}}}}, data, expected, actual, why));
}

TEST_CASE("Profile cache keys include semantic metadata, validity, mapping and channels") {
  auto input = fixture(4);
  const auto digest = [&](const Profile& profile) {
    return exec::injectionKeysOf({{"n", "out", Data::profile(profile)}}).providedDigest.at("n");
  };
  const auto baseline = digest(input);
  CHECK(digest(input) == baseline);
  CHECK(digest(fixture(4)) != baseline);
  auto changed = input;
  changed.descriptor.unit = "m";
  CHECK(digest(changed) != baseline);
  changed = input;
  changed.descriptor.frame = "other";
  CHECK(digest(changed) != baseline);
  changed = input;
  changed.validity[0] = 0;
  CHECK(digest(changed) != baseline);
  changed = input;
  changed.descriptor.sampleOrder = "unknown";
  std::swap(changed.sourceIndices[0], changed.sourceIndices[1]);
  CHECK(digest(changed) != baseline);
  changed = input;
  PointCloud cloud = *input.cloud;
  cloud.rgb[0] = 10;
  changed.cloud = std::make_shared<const PointCloud>(std::move(cloud));
  CHECK(digest(changed) != baseline);
  const auto graph = makeGraph({{"n", "util.reroute"}}, {});
  const auto error = Data::error(Status::Error(Phase::Validate, "bad_input", "Profile单位无效"));
  const std::vector<exec::InjectedInput> invalid{{"n", "out", error}};
  CHECK(Json::parse(exec::planGraphJson(graph.dump(), {}, {}, {}, invalid))[0]["code"] ==
        "bad_input");
  CHECK(Json::parse(exec::effectiveParamsJson(graph.dump(), {}, {}, invalid))[0]["code"] ==
        "bad_input");
  CHECK(Json::parse(exec::evictNodesJson(graph.dump(), {}, {"n"}, false, {}, invalid))[0]["code"] ==
        "bad_input");
  Session session(graph, {}, {}, false, 1, false, invalid);
  CHECK(session.wait().runStatus() == "error");
}

TEST_CASE("Profile disk cache keeps NaN slots and rejects invalid metadata atomically") {
  const auto input = holeProfile();
  std::string bytes;
  REQUIRE(exec::encodeNode({{"profile", Data::profile(input)}}, bytes));
  exec::PortData decoded;
  REQUIRE(exec::decodeNode(bytes, decoded));
  REQUIRE(decoded.size() == 1);
  const auto* result = decoded[0].second.asProfile();
  REQUIRE(result);
  CHECK(result->consistent());
  CHECK(result->cloud->id != input.cloud->id);
  CHECK(std::memcmp(result->cloud->xyz.data(), input.cloud->xyz.data(),
                    input.cloud->xyz.size() * sizeof(float)) == 0);
  CHECK(result->validity == input.validity);
  CHECK(result->sourceIndices == input.sourceIndices);
  CHECK(profileDescriptorJson(result->descriptor) == profileDescriptorJson(input.descriptor));
  const auto offset = bytes.find("\"unit\":\"mm\"");
  REQUIRE(offset != std::string::npos);
  bytes.replace(offset, 11, "\"unit\":\"zz\"");
  const auto unchanged = decoded[0].second.profilePtr();
  CHECK_FALSE(exec::decodeNode(bytes, decoded));
  CHECK(decoded[0].second.profilePtr() == unchanged);
}

TEST_CASE("Profile native frame archive retains shared cloud identity and source mapping") {
  const auto profile = holeProfile();
  const auto graph = makeGraph({{"n", "util.reroute"}}, {});
  Session session(graph, {}, {}, false, 1, false, {{"n", "out", Data::profile(profile)}});
  const auto& log = session.wait();
  INFO(Json(log.events).dump());
  REQUIRE(log.runStatus() == "ok");
  const Json context{{"frameId", "profile-frame"}, {"timestampNs", "1"}, {"frame", "sensor-a"}};
  lyflow_frame* frame = nullptr;
  REQUIRE(owned(lyflow_frame_capture(session.runId().c_str(), context.dump().c_str(), "{}", &frame))
              .empty());
  REQUIRE(frame != nullptr);
  const auto path =
      std::filesystem::temp_directory_path() / ("lyflow-profile-" + session.runId() + ".lyframe");
  CHECK(owned(lyflow_frame_save(frame, path.u8string().c_str(), 0)).empty());
  lyflow_frame* loaded = nullptr;
  REQUIRE(owned(lyflow_frame_load(path.u8string().c_str(), 0, &loaded)).empty());
  REQUIRE(loaded != nullptr);
  const auto info = Json::parse(owned(lyflow_frame_metadata(loaded)));
  Data restored;
  REQUIRE(exec::ResultStore::instance().get(info["viewRunId"], "n", "out", restored));
  REQUIRE(restored.asProfile());
  CHECK(restored.asProfile()->consistent());
  CHECK(restored.asProfile()->validity == profile.validity);
  CHECK(restored.asProfile()->sourceIndices == profile.sourceIndices);
  CHECK(restored.asProfile()->cloud->id != profile.cloud->id);
  CHECK(std::isnan(restored.asProfile()->cloud->xyz[777 * 3]));
  const auto bindings = Json::array(
      {{{"sourceNode", "n"}, {"sourcePort", "out"}, {"nodeId", "replay"}, {"port", "out"}}});
  const auto replay = runtime::frameInputs(loaded, bindings.dump());
  REQUIRE(replay.size() == 1);
  CHECK(replay[0].data.profilePtr() == restored.profilePtr());
  lyflow_frame_free(frame);
  lyflow_frame_free(loaded);
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
}

TEST_CASE("Profile Bundle frame binding validates fields and fingerprints full payloads") {
  auto& registry = ensureRegistry();
  if (!registry.findBundle("test.ProfileCarrier")) {
    BundleDesc desc;
    desc.kind = "test.ProfileCarrier";
    desc.fields = {{"profile", "Profile", ""}};
    registry.addBundle(std::move(desc));
  }
  auto wrap = [](Profile profile) {
    Bundle bundle("test.ProfileCarrier");
    bundle.set("profile", Data::profile(std::move(profile)));
    return Data::bundle(std::move(bundle));
  };
  const auto graph = makeGraph({{"p", "util.reroute"}, {"q", "util.reroute"}}, {{"p.out", "q.in"}});
  const auto input = holeProfile();
  Session source(graph, {}, {}, false, 1, false, {{"p", "out", wrap(input)}});
  REQUIRE(source.wait().runStatus() == "ok");
  lyflow_profile_view nativeView{};
  REQUIRE(lyflow_output_profile(source.runId().c_str(), "q", "out.profile", 0, 0, &nativeView) ==
          0);
  CHECK(nativeView.point_count == input.pointCount());
  CHECK(nativeView.cloud_id == input.cloud->id);
  CHECK(nativeView.xyz == input.cloud->xyz.data());
  CHECK(nativeView.validity[777] == 0);
  CHECK(nativeView.source_indices[777] == input.sourceIndices[777]);
  CHECK(Json::parse(nativeView.descriptor_json) == profileDescriptorJson(input.descriptor));
  lyflow_profile_view_free(&nativeView);
  CHECK(lyflow_output_profile(source.runId().c_str(), "q", "out.missing", 0, 0, &nativeView) == 1);
  CHECK(lyflow_output_profile(source.runId().c_str(), "q", "out.profile.nested", 0, 0,
                              &nativeView) == 1);
  lyflow_frame* rawFrame = nullptr;
  REQUIRE(owned(lyflow_frame_capture(
                    source.runId().c_str(),
                    R"({"frameId":"bundle-profile","timestampNs":"1","frame":"sensor"})", "{}",
                    &rawFrame))
              .empty());
  std::unique_ptr<lyflow_frame, decltype(&lyflow_frame_free)> frame(rawFrame, lyflow_frame_free);
  REQUIRE(frame);
  const auto bindings =
      Json::array({{{"sourceNode", "q"}, {"sourcePort", "out"}, {"nodeId", "p"}, {"port", "out"}}});
  auto inputs = runtime::frameInputs(frame.get(), bindings.dump());
  REQUIRE(inputs.size() == 1);
  REQUIRE(inputs[0].data.asBundle());
  const auto* bound = inputs[0].data.asBundle()->field("profile")->asProfile();
  REQUIRE(bound);
  CHECK(bound->cloud == input.cloud);
  CHECK(bound->validity == input.validity);
  auto downstream = [&](const std::vector<exec::InjectedInput>& values) {
    const auto plan = Json::parse(exec::planGraphJson(graph.dump(), {}, {}, "", values));
    REQUIRE(plan.is_array());
    for (const auto& node : plan)
      if (node.value("nodeId", "") == "q")
        return node;
    FAIL("缺少下游规划节点");
    return Json::object();
  };
  const auto baseline = downstream(inputs);
  CHECK(baseline["cached"] == true);
  Session replay(graph, {}, {}, true, 1, false, inputs);
  const auto& replayLog = replay.wait();
  REQUIRE(replayLog.runStatus() == "ok");
  CHECK(replayLog.nodeEvent("q", "skipped")["stats"]["cached"] == true);
  for (const auto* field : {"descriptor", "mask", "coordinates"}) {
    CAPTURE(field);
    auto changed = *bound;
    if (std::string(field) == "descriptor")
      changed.descriptor.frame = "another-sensor";
    else if (std::string(field) == "mask")
      changed.validity[0] = 0;
    else {
      PointCloud cloud = *changed.cloud;
      cloud.xyz[0] += 1;
      changed.cloud = std::make_shared<const PointCloud>(std::move(cloud));
    }
    std::vector<exec::InjectedInput> changedInputs{{"p", "out", wrap(std::move(changed))}};
    const auto changedPlan = downstream(changedInputs);
    CHECK(changedPlan["cacheKey"] != baseline["cacheKey"]);
    CHECK(changedPlan["cached"] == false);
    CHECK(Json::parse(exec::evictNodesJson(graph.dump(), {}, {"q"}, false, {},
                                           changedInputs))["removed"] == 0);
    Session changedRun(graph, {}, {}, true, 1, false, changedInputs);
    REQUIRE(changedRun.wait().runStatus() == "ok");
    CHECK_FALSE(changedRun.wait().nodeEvent("q", "done").empty());
  }
  CHECK(Json::parse(exec::evictNodesJson(graph.dump(), {}, {"q"}, false, {}, inputs))["removed"] ==
        1);
  auto invalid = *bound;
  invalid.validity[777] = 1;
  std::vector<exec::InjectedInput> invalidInputs{{"p", "out", wrap(invalid)}};
  const auto diagnostics =
      Json::parse(exec::planGraphJson(graph.dump(), {}, {}, "", invalidInputs));
  CHECK(diagnostics[0]["code"] == "bad_input");
  CHECK(diagnostics[0]["nodeId"] == "p");
  CHECK(diagnostics[0]["message"].get<std::string>().find("profile") != std::string::npos);
  CHECK(Json::parse(exec::effectiveParamsJson(graph.dump(), {}, {}, invalidInputs))[0]["code"] ==
        "bad_input");
  CHECK(Json::parse(exec::evictNodesJson(graph.dump(), {}, {"q"}, false, {},
                                         invalidInputs))[0]["code"] == "bad_input");
  Session invalidRun(graph, {}, {}, true, 1, false, invalidInputs);
  CHECK(invalidRun.wait().runStatus() == "error");
  Bundle nested("test.ProfileCarrier");
  nested.set("profile", wrap(input));
  invalidInputs[0].data = Data::bundle(std::move(nested));
  CHECK(Json::parse(exec::planGraphJson(graph.dump(), {}, {}, "", invalidInputs))[0]["code"] ==
        "bad_input");

  auto digest = [&](Data data) {
    return exec::injectionKeysOf({{"p", "out", std::move(data)}}).providedDigest.at("p");
  };
  const auto tensor = Data::tensor(Tensor{{2, 2}, {1, 2, 3, 4}});
  CHECK(digest(tensor) != digest(Data::tensor(Tensor{{2, 2}, {4, 3, 2, 1}})));
  CHECK(digest(tensor) != digest(Data::tensor(Tensor{{1, 4}, {1, 2, 3, 4}})));
  CHECK(digest(tensor) != digest(tensor.withTensorScale(2)));
  CHECK(digest(Data::indices({1, {1, 2}})) != digest(Data::indices({1, {2, 1}})));
  CHECK(digest(Data::indices({1, {1, 2}})) != digest(Data::indices({2, {1, 2}})));
  CHECK(digest(Data::record({"test.Record", {{"value", 1}}})) !=
        digest(Data::record({"test.Record", {{"value", 2}}})));
}
