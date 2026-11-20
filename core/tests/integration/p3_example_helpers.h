#pragma once

#include "helpers.h"

#include <array>
#include <cmath>
#include <fstream>
#include <functional>

#include "lyflow/c_api.h"
#include "lyflow/client.hpp"
#include "lyflow/pose.h"

namespace lyflow::test::p3 {
inline Json read(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  REQUIRE(file.good());
  return Json::parse(file);
}
inline const Json& truth() {
  static const Json result = read(std::filesystem::path(LYFLOW_EXAMPLES_DIR) / "p3-fixtures.json");
  return result;
}
inline Json take(char* value) {
  REQUIRE(value != nullptr);
  const std::unique_ptr<char, decltype(&lyflow_string_free)> owned(value, lyflow_string_free);
  return Json::parse(owned.get());
}
inline const Json& data(const Json& outputs, const char* name) {
  return outputs.at(name).at("value").at("data");
}
inline void positionNear(const Json& actual, const Json& expected, double tolerance) {
  REQUIRE(actual.size() == expected.size());
  double squared = 0;
  for (std::size_t i = 0; i < expected.size(); ++i) {
    const double delta = actual[i].get<double>() - expected[i].get<double>();
    REQUIRE(std::isfinite(actual[i].get<double>()));
    squared += delta * delta;
  }
  CHECK(std::sqrt(squared) <= tolerance);
}
inline void poseRecord(const Json& output) {
  const auto& record = output.at("value");
  std::string why;
  REQUIRE_MESSAGE(poses::validate(Record{record.at("type"), record.at("data")}, why), why);
}
inline void agree(const Json& first, const Json& second) {
  REQUIRE(first.size() == second.size());
  for (const auto& item : first.items()) {
    CAPTURE(item.key());
    const auto& other = second.at(item.key());
    CHECK(item.value().at("type") == other.at("type"));
    CHECK(item.value().at("elementCount") == other.at("elementCount"));
    CHECK_FALSE(item.value().value("missing", false));
    CHECK_FALSE(other.value("missing", false));
    if (item.value().at("type") == "Record")
      CHECK(item.value().at("value") == other.at("value"));
    if (item.value().at("type") == "Transform")
      CHECK(item.value().at("value").at("m") == other.at("value").at("m"));
    if (item.value().at("type") == "Image")
      for (const char* field : {"width", "height", "channels", "depth", "min", "max"}) {
        CAPTURE(field);
        CHECK(item.value().at("value").at(field) == other.at("value").at(field));
      }
  }
}

struct TensorPayload {
  std::vector<std::int64_t> shape;
  std::vector<float> values;
};
inline TensorPayload tensorPayload(const std::string& runId, const std::string& node,
                                   const std::string& port, Client* client) {
  const auto copy = [](const std::int64_t* shape, std::uint32_t rank, const float* values,
                       std::uint32_t count) {
    TensorPayload result;
    if (rank)
      result.shape.assign(shape, shape + rank);
    if (count)
      result.values.assign(values, values + count);
    return result;
  };
  if (client) {
    const auto view = client->tensor(runId, node, port);
    REQUIRE(view.valid());
    CHECK(view.total() == view.count());
    return copy(view.shape(), view.rank(), view.data(), view.count());
  }
  struct RawTensor {
    lyflow_tensor_view view{};
    ~RawTensor() { lyflow_tensor_view_free(&view); }
  } tensor;
  REQUIRE(lyflow_output_tensor(runId.c_str(), node.c_str(), port.c_str(), 0, 0, &tensor.view) == 0);
  const auto& view = tensor.view;
  CHECK(view.total == view.count);
  return copy(view.shape, view.rank, view.data, view.count);
}

class SavedWorkflow {
public:
  explicit SavedWorkflow(const std::string& name) {
    static std::atomic<unsigned> counter{0};
    const auto original =
        read(std::filesystem::path(LYFLOW_EXAMPLES_DIR) / (name + ".lyflow.json"));
    base = std::filesystem::temp_directory_path() /
           std::filesystem::u8path(
               "lyflow-p3-图-" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
               std::to_string(counter.fetch_add(1)));
    REQUIRE(std::filesystem::create_directory(base));
    for (const auto& node : original.at("nodes")) {
      if (!node.at("params").contains("path"))
        continue;
      const auto relative =
          std::filesystem::u8path(node.at("params").at("path").get<std::string>());
      REQUIRE_FALSE(relative.is_absolute());
      REQUIRE(relative.parent_path().empty());
      std::filesystem::copy_file(std::filesystem::path(LYFLOW_EXAMPLES_DIR) / relative,
                                 base / relative,
                                 std::filesystem::copy_options::overwrite_existing);
    }
    path = base / "saved.lyflow.json";
    {
      std::ofstream file(path, std::ios::binary);
      file << original.dump(2);
      REQUIRE(file.good());
    }
    graph = read(path);
    REQUIRE(graph == original);
    REQUIRE(graph.dump().size() < 20000);
  }
  SavedWorkflow(const SavedWorkflow&) = delete;
  SavedWorkflow& operator=(const SavedWorkflow&) = delete;
  ~SavedWorkflow() {
    std::error_code error;
    std::filesystem::remove_all(base, error);
  }
  std::filesystem::path base, path;
  Json graph;
};

using Check = std::function<void(const Json&, bool)>;
using NativeCheck =
    std::function<void(const Json&, const std::string&, Client*, bool changed, bool baseline)>;

inline void crossInterfaces(const std::string& name, const Json& graph,
                            const std::filesystem::path& savedPath, const Json& overrides,
                            const Check& check, const NativeCheck& native = {}) {
  const auto baseDir = savedPath.parent_path();
  const auto base = baseDir.u8string(), source = graph.dump();
  Client client(LYFLOW_TEST_RUNTIME);
  std::array<Json, 2> baselines;
  for (int attempt = 0; attempt < 3; ++attempt) {
    const bool changed = attempt == 1;
    CAPTURE(name);
    CAPTURE(attempt);
    const Json params = changed ? overrides : Json::object();
    auto& expected = baselines[changed ? 1 : 0];
    if (attempt < 2) {
      auto effective = graph;
      for (const auto& parameter : params.items())
        effective.at("params").at(parameter.key())["default"] = parameter.value();
      Session scheduler(effective, baseDir, {}, false, 2, true);
      const auto& log = scheduler.wait();
      REQUIRE_MESSAGE(log.runStatus() == "ok", Json(log.events).dump());
      CHECK(log.seqIsDense());
      for (const auto& node : effective.at("nodes"))
        CHECK(log.finalState(node.at("id")) == "done");
      expected = take(lyflow_run_outputs(log.runId.c_str()));
      check(expected, changed);
      if (native)
        native(expected, log.runId, nullptr, changed, true);
    }

    const auto id = name + "-abi-" + std::to_string(attempt), parameters = params.dump();
    REQUIRE(take(lyflow_validate_params(source.c_str(), base.c_str(), parameters.c_str())).empty());
    lyflow_run_options options{};
    options.run_id = id.c_str();
    options.base_dir = base.c_str();
    options.params_json = parameters.c_str();
    options.max_parallel = 2;
    options.no_reuse = 1;
    const std::unique_ptr<lyflow_run, decltype(&lyflow_run_free)> run(
        lyflow_run_start(source.c_str(), &options, nullptr, nullptr), lyflow_run_free);
    REQUIRE(run != nullptr);
    lyflow_run_join(run.get());
    const auto abi = take(lyflow_run_outputs(id.c_str()));
    CHECK(take(lyflow_run_summary(id.c_str())).at("status") == "ok");
    agree(expected, abi);
    if (native)
      native(abi, id, nullptr, changed, false);

    RunOptions sdkOptions;
    sdkOptions.runId = id + "-sdk";
    sdkOptions.baseDir = base;
    sdkOptions.paramsJson = parameters;
    sdkOptions.maxParallel = 2;
    sdkOptions.noReuse = true;
    const auto result = client.run(source, sdkOptions);
    REQUIRE_MESSAGE(result.ok(), result.summary);
    CHECK(Json::parse(result.summary).at("status") == "ok");
    const auto sdk = Json::parse(result.outputs);
    agree(expected, sdk);
    if (native)
      native(sdk, sdkOptions.runId, &client, changed, false);
    CHECK(read(savedPath) == graph);
  }
}

inline void crossInterfaces(const std::string& name, const Json& overrides, const Check& check,
                            const NativeCheck& native = {}) {
  const SavedWorkflow saved(name);
  crossInterfaces(name, saved.graph, saved.path, overrides, check, native);
}
} // namespace lyflow::test::p3
