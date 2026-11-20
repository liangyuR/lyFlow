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

class Example {
public:
  Example(const std::string& name, const std::string& assetPrefix,
          const std::string& extraAsset = {}) {
    static std::atomic<unsigned> counter{0};
    const std::filesystem::path examples(LYFLOW_EXAMPLES_DIR);
    base = std::filesystem::temp_directory_path() /
           std::filesystem::u8path(
               "lyflow-示例-" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
               std::to_string(counter.fetch_add(1)));
    REQUIRE(std::filesystem::create_directory(base));
    for (const auto& entry : std::filesystem::directory_iterator(examples)) {
      if (!entry.is_regular_file())
        continue;
      const auto filename = entry.path().filename().u8string();
      if (filename.rfind(assetPrefix, 0) == 0 || filename == extraAsset)
        std::filesystem::copy_file(entry.path(), base / entry.path().filename());
    }
    graph = read(base / (name + ".lyflow.json"));
  }
  Example(const Example&) = delete;
  Example& operator=(const Example&) = delete;
  ~Example() {
    std::error_code error;
    std::filesystem::remove_all(base, error);
  }
  std::filesystem::path base;
  Json graph;
};

enum class Coverage { Scheduler, Interfaces };
using Check = std::function<void(const Json&, bool)>;
using Observe =
    std::function<void(const Json&, const std::string&, Client*, bool changed, bool baseline)>;

inline void replay(const std::string& name, const Example& example, const Json& overrides,
                   Coverage coverage, const Observe& observe) {
  CAPTURE(name);
  for (const bool changed : {false, true}) {
    CAPTURE(changed);
    auto effective = example.graph;
    if (changed)
      for (const auto& parameter : overrides.items())
        effective.at("params").at(parameter.key())["default"] = parameter.value();
    Session scheduler(effective, example.base, {}, false, 2, true);
    const auto& log = scheduler.wait();
    REQUIRE_MESSAGE(log.runStatus() == "ok", Json(log.events).dump());
    observe(take(lyflow_run_outputs(log.runId.c_str())), log.runId, nullptr, changed, true);
  }
  if (coverage == Coverage::Scheduler)
    return;

  const auto base = example.base.u8string(), source = example.graph.dump();
  Client client(LYFLOW_TEST_RUNTIME);
  for (int attempt = 0; attempt < 3; ++attempt) {
    CAPTURE(attempt);
    const bool changed = attempt == 1;
    const auto parameters = (changed ? overrides : Json::object()).dump();
    const auto id = name + "-abi-" + std::to_string(attempt);
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
    REQUIRE(take(lyflow_run_summary(id.c_str())).at("status") == "ok");
    observe(take(lyflow_run_outputs(id.c_str())), id, nullptr, changed, false);

    RunOptions sdkOptions;
    sdkOptions.runId = id + "-sdk";
    sdkOptions.baseDir = base;
    sdkOptions.paramsJson = parameters;
    sdkOptions.maxParallel = 2;
    sdkOptions.noReuse = true;
    const auto result = client.run(source, sdkOptions);
    REQUIRE_MESSAGE(result.ok(), result.summary);
    observe(Json::parse(result.outputs), sdkOptions.runId, &client, changed, false);
  }
}

inline void checkExample(const std::string& name, const Example& example, const Json& overrides,
                         Coverage coverage, const Check& check, const Observe& native = {}) {
  std::array<Json, 2> baselines;
  replay(name, example, overrides, coverage,
         [&](const Json& outputs, const std::string& runId, Client* client, bool changed,
             bool baseline) {
           auto& expected = baselines[changed];
           if (baseline) {
             check(outputs, changed);
             if (coverage == Coverage::Interfaces)
               expected = outputs;
           } else {
             agree(expected, outputs);
           }
           if (native)
             native(outputs, runId, client, changed, baseline);
         });
}
} // namespace lyflow::test::p3
