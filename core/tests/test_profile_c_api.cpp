#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "lyflow/c_api.h"
#include <nlohmann/json.hpp>

namespace {
const char* kGraph =
    R"({"schemaVersion":1,"id":"m1-profile","name":"profile","nodes":[{"id":"p","op":"util.reroute","params":{}}],"edges":[]})";
const char* kDescriptor =
    R"({"schemaVersion":1,"frame":"sensor","unit":"mm","plane":"XZ","handedness":"right","sampleOrder":"acquisition","sourceId":"scan-1","sourceCount":4})";

struct Input {
  std::vector<float> xyz{0, 0, 1, NAN, NAN, NAN, 3, 0, 4};
  std::vector<float> intensity{1, 0, 2};
  std::vector<std::uint8_t> rgb{255, 0, 0, 0, 255, 0, 0, 0, 255};
  std::vector<std::uint8_t> validity{1, 0, 1};
  std::vector<std::uint32_t> indices{0, 2, 3};
  lyflow_run_profile_input raw{};
  Input() {
    raw.node_id = "p";
    raw.port = "out";
    raw.count = 3;
    raw.xyz = xyz.data();
    raw.intensity = intensity.data();
    raw.rgb = rgb.data();
    raw.descriptor_json = kDescriptor;
    raw.validity = validity.data();
    raw.source_indices = indices.data();
  }
};

std::string take(char* value) {
  if (!value)
    return {};
  std::string result(value);
  lyflow_string_free(value);
  return result;
}
}

TEST_CASE("Profile C ABI keeps slots, source indices and view lifetime") {
  Input in;
  lyflow_run_options options{};
  options.run_id = "m1-profile-c-api";
  options.profile_inputs = &in.raw;
  options.profile_input_count = 1;
  std::unique_ptr<lyflow_run, decltype(&lyflow_run_free)> run(
      lyflow_run_start(kGraph, &options, nullptr, nullptr), lyflow_run_free);
  REQUIRE(run != nullptr);
  in.xyz[0] = 99;
  in.validity[0] = 0;
  lyflow_run_join(run.get());
  lyflow_profile_view whole{};
  REQUIRE(lyflow_output_profile(options.run_id, "p", "out", 0, 0, &whole) == 0);
  CHECK(whole.total_points == 3);
  CHECK(whole.point_count == 3);
  CHECK(whole.xyz[0] == 0);
  CHECK(std::isnan(whole.xyz[3]));
  CHECK(whole.validity[0] == 1);
  CHECK(whole.validity[1] == 0);
  CHECK(whole.source_indices[1] == 2);
  CHECK(whole.rgb[8] == 255);
  CHECK(std::string(whole.descriptor_json).find("\"unit\":\"mm\"") != std::string::npos);
  lyflow_profile_view slice{};
  REQUIRE(lyflow_output_profile(options.run_id, "p", "out", 1, 1, &slice) == 0);
  CHECK(slice.point_count == 1);
  CHECK(slice.offset == 1);
  CHECK(slice.xyz == whole.xyz + 3);
  CHECK(slice.validity[0] == 0);
  CHECK(slice.source_indices[0] == 2);
  run.reset();
  lyflow_cache_clear();
  CHECK(whole.xyz[0] == 0);
  CHECK(slice.source_indices[0] == 2);
  lyflow_profile_view_free(&slice);
  lyflow_profile_view_free(&whole);
}

TEST_CASE("Profile C ABI rejects semantic invalidity consistently across inputs APIs") {
  Input in;
  in.indices[1] = in.indices[0];
  const auto plan = take(lyflow_plan_inputs_ex(kGraph, nullptr, nullptr, 0, nullptr, nullptr, 0,
                                               nullptr, 0, &in.raw, 1));
  const auto effective = take(lyflow_effective_params_inputs_ex(kGraph, nullptr, nullptr, nullptr,
                                                                0, nullptr, 0, &in.raw, 1));
  const auto evict = take(lyflow_cache_evict_inputs_ex(kGraph, nullptr, nullptr, 0, 0, nullptr,
                                                       nullptr, 0, nullptr, 0, &in.raw, 1));
  for (const auto* text : {&plan, &effective, &evict}) {
    CHECK(text->find("bad_input") != std::string::npos);
    const auto doc = nlohmann::json::parse(*text);
    REQUIRE(doc.is_array());
    CHECK(doc[0]["nodeId"] == "p");
  }
  in.raw.descriptor_json =
      R"({"schemaVersion":1,"frame":"sensor","unit":"px","plane":"XZ","handedness":"right","sampleOrder":"acquisition","sourceId":"scan-1","sourceCount":4})";
  const auto badUnit = take(lyflow_plan_inputs_ex(kGraph, nullptr, nullptr, 0, nullptr, nullptr, 0,
                                                  nullptr, 0, &in.raw, 1));
  CHECK(badUnit.find("bad_input") != std::string::npos);
}

TEST_CASE("Profile retained input preserves identity across run plan and cache eviction") {
  lyflow_cache_clear();
  Input in;
  lyflow_profile_input* rawHandle = nullptr;
  REQUIRE(take(lyflow_profile_input_create(&in.raw, &rawHandle)).empty());
  std::unique_ptr<lyflow_profile_input, decltype(&lyflow_profile_input_free)> handle(
      rawHandle, lyflow_profile_input_free);
  REQUIRE(handle);
  lyflow_run_profile_input retained{};
  retained.node_id = "p";
  retained.port = "out";
  retained.handle = handle.get();
  in.xyz[0] = 99;
  const char* graph =
      R"({"schemaVersion":1,"id":"profile-retained","name":"profile-retained","nodes":[{"id":"p","op":"util.reroute","params":{}},{"id":"q","op":"util.reroute","params":{}}],"edges":[{"id":"e","from":{"node":"p","port":"out"},"to":{"node":"q","port":"in"}}]})";
  lyflow_run_options options{};
  options.run_id = "profile-retained";
  options.profile_inputs = &retained;
  options.profile_input_count = 1;
  std::unique_ptr<lyflow_run, decltype(&lyflow_run_free)> run(
      lyflow_run_start(graph, &options, nullptr, nullptr), lyflow_run_free);
  REQUIRE(run);
  lyflow_run_join(run.get());
  lyflow_profile_view view{};
  REQUIRE(lyflow_output_profile(options.run_id, "q", "out", 0, 0, &view) == 0);
  CHECK(view.xyz[0] == 0);
  const auto cloudId = view.cloud_id;
  lyflow_profile_view_free(&view);
  const auto plan = nlohmann::json::parse(take(lyflow_plan_inputs_ex(
      graph, nullptr, nullptr, 0, nullptr, nullptr, 0, nullptr, 0, &retained, 1)));
  REQUIRE(plan.is_array());
  bool downstreamCached = false;
  for (const auto& node : plan)
    if (node.value("nodeId", "") == "q")
      downstreamCached = node.value("cached", false);
  CHECK(downstreamCached);
  lyflow_profile_input* captured = nullptr;
  REQUIRE(take(lyflow_profile_input_capture(options.run_id, "q", "out", &captured)).empty());
  std::unique_ptr<lyflow_profile_input, decltype(&lyflow_profile_input_free)> capturedHandle(
      captured, lyflow_profile_input_free);
  retained.handle = capturedHandle.get();
  const auto capturedPlan = nlohmann::json::parse(take(lyflow_plan_inputs_ex(
      graph, nullptr, nullptr, 0, nullptr, nullptr, 0, nullptr, 0, &retained, 1)));
  CHECK(capturedPlan == plan);
  run.reset();
  const char* nodes[] = {"q"};
  const auto evict = nlohmann::json::parse(take(lyflow_cache_evict_inputs_ex(
      graph, nullptr, nodes, 1, 0, nullptr, nullptr, 0, nullptr, 0, &retained, 1)));
  CHECK(evict["removed"] == 1);
  CHECK(cloudId != 0);
}

TEST_CASE("Profile C ABI rejects empty injection bindings for raw and retained inputs") {
  Input in;
  in.raw.node_id = nullptr;
  in.raw.port = nullptr;
  lyflow_profile_input* created = nullptr;
  REQUIRE(take(lyflow_profile_input_create(&in.raw, &created)).empty());
  std::unique_ptr<lyflow_profile_input, decltype(&lyflow_profile_input_free)> handle(
      created, lyflow_profile_input_free);
  REQUIRE(handle);
  const char* graph =
      R"({"schemaVersion":1,"id":"profile-binding","name":"profile-binding","nodes":[{"id":"p","op":"value.number","params":{}}],"edges":[]})";
  const std::vector<std::pair<const char*, const char*>> bindings{
      {nullptr, "out"}, {"", "out"}, {"p", nullptr}, {"p", ""}};
  for (const auto* retained : {static_cast<const lyflow_profile_input*>(nullptr),
                               static_cast<const lyflow_profile_input*>(handle.get())}) {
    in.raw.handle = retained;
    for (const auto& binding : bindings) {
      in.raw.node_id = binding.first;
      in.raw.port = binding.second;
      for (const auto& text :
           {take(lyflow_plan_inputs_ex(graph, nullptr, nullptr, 0, nullptr, nullptr, 0, nullptr, 0,
                                       &in.raw, 1)),
            take(lyflow_effective_params_inputs_ex(graph, nullptr, nullptr, nullptr, 0, nullptr, 0,
                                                   &in.raw, 1)),
            take(lyflow_cache_evict_inputs_ex(graph, nullptr, nullptr, 0, 0, nullptr, nullptr, 0,
                                              nullptr, 0, &in.raw, 1))}) {
        const auto diagnostics = nlohmann::json::parse(text);
        REQUIRE(diagnostics.is_array());
        REQUIRE_FALSE(diagnostics.empty());
        CHECK(diagnostics[0]["code"] == "bad_input");
      }
      lyflow_run_options options{};
      options.run_id = "profile-invalid-binding";
      options.profile_inputs = &in.raw;
      options.profile_input_count = 1;
      std::vector<std::string> events;
      std::unique_ptr<lyflow_run, decltype(&lyflow_run_free)> run(
          lyflow_run_start(
              graph, &options,
              [](const char* text, void* context) {
                static_cast<std::vector<std::string>*>(context)->emplace_back(text);
              },
              &events),
          lyflow_run_free);
      REQUIRE(run);
      lyflow_run_join(run.get());
      bool failed = false;
      for (const auto& text : events) {
        const auto event = nlohmann::json::parse(text);
        if (event.value("kind", "") == "run_finished") {
          CHECK(event["status"] == "error");
          CHECK(event["error"]["code"] == "bad_input");
          failed = true;
        }
      }
      CHECK(failed);
    }
  }
}
