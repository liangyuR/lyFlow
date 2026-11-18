#include <atomic>
#include <fstream>
#include <limits>

#include "helpers.h"
#include "lyflow/pose.h"

namespace lyflow::ops {
void registerP3Workspace(Registry&);
}

namespace {
using namespace lyflow;
using Json = nlohmann::json;
using Matrix = std::array<double, 16>;
Matrix translation(double x = 0, double y = 0, double z = 0) {
  return {1, 0, 0, x, 0, 1, 0, y, 0, 0, 1, z, 0, 0, 0, 1};
}
Json workspaceFile() {
  auto camera = translation(1, 2, 3);
  camera[0] = camera[5] = 0;
  camera[1] = -1;
  camera[4] = 1;
  return {
      {"schemaVersion", 1},
      {"unit", "m"},
      {"rootFrame", "world"},
      {"frames",
       Json::array({{{"id", "ws"}, {"parent", "camera"}, {"transform", translation(.1, .2, .3)}},
                    {{"id", "camera"}, {"parent", "world"}, {"transform", camera}}})},
      {"workspaces",
       Json::array(
           {{{"id", "workspace"}, {"frame", "ws"}, {"size", {2, 4, 6}}, {"anchor", "bottom"}}})}};
}
class WorkspaceFile {
public:
  WorkspaceFile() {
    static std::atomic<unsigned> sequence{0};
    path =
        std::filesystem::temp_directory_path() /
        std::filesystem::u8path(
            "lyflow-工作空间-" + std::to_string(++sequence) + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
  }
  ~WorkspaceFile() {
    std::error_code error;
    std::filesystem::remove(path, error);
  }
  void write(const std::string& bytes) const {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(stream.good());
  }
  void write(const Json& value) const { write(value.dump()); }
  std::filesystem::path path;
};
const Registry& workspaceRegistry() {
  static Registry registry;
  static const bool initialized = [] {
    ops::registerP3Workspace(registry);
    return true;
  }();
  (void)initialized;
  return registry;
}
Status loadWorkspace(test::OpCall& call, const WorkspaceFile& file, ParamMap overrides = {}) {
  overrides["path"] = Value::text(file.path.u8string());
  return call.run("workspace.load", overrides);
}
const Json& record(const test::OpCall& call, const char* name) {
  REQUIRE(call.outputs.count(name) == 1);
  const auto* value = call.outputs.at(name).asRecord();
  REQUIRE(value != nullptr);
  return value->data;
}
std::array<double, 3> apply(const Transform& m, std::array<double, 3> point) {
  std::array<double, 3> result{};
  for (int row = 0; row < 3; ++row)
    result[row] = m.m[4 * row] * point[0] + m.m[4 * row + 1] * point[1] +
                  m.m[4 * row + 2] * point[2] + m.m[4 * row + 3];
  return result;
}
void near(const Json& actual, std::array<double, 3> expected, double tolerance = 2e-6) {
  REQUIRE(actual.is_array());
  REQUIRE(actual.size() == 3);
  for (int i = 0; i < 3; ++i)
    CHECK(std::abs(actual[i].get<double>() - expected[i]) <= tolerance);
}
std::string legacy(bool parent = true) {
  return std::string(
             "pose:\n  - [1, 0, 0, 0.1]\n  - [0, 1, 0, 0.2]\n  - [0, 0, 1, 0.3]\n  - [0, 0, 0, 1]\nlength: 2\nwidth: 4\nheight: 6\n") +
         (parent
              ? "parent_id: '7'\nparent_frame_type: Camera\nparent_pose:\n  - [0, -1, 0, 1]\n  - [1, 0, 0, 2]\n  - [0, 0, 1, 3]\n  - [0, 0, 0, 1]\n"
              : "");
}
}

TEST_CASE("P3 workspace resolves unordered frame trees and preserves bottom volume boundaries") {
  WorkspaceFile file;
  file.write(workspaceFile());
  test::OpCall call(workspaceRegistry());
  REQUIRE_MESSAGE(loadWorkspace(call, file).ok, call.status.message);
  const auto& region = record(call, "region");
  CHECK(region["frame"] == "world");
  CHECK(region["id"] == "workspace");
  CHECK(region["pickable"] == false);
  near(region["position"], {.8, 2.1, 6.3});
  near(region["size"], {2, 4, 6});
  std::string why;
  CHECK(poses::validate(*call.outputs.at("region").asRecord(), why));
  const auto* from = call.outputs.at("fromWorkspace").asTransform();
  const auto* to = call.outputs.at("toWorkspace").asTransform();
  REQUIRE(from != nullptr);
  REQUIRE(to != nullptr);
  for (const std::array<double, 3>& local :
       {std::array<double, 3>{-1, -2, 0}, {1, 2, 6}, {0, 0, 3}}) {
    const auto world = apply(*from, local), restored = apply(*to, world);
    for (int i = 0; i < 3; ++i)
      CHECK(std::abs(restored[i] - local[i]) < 2e-6);
  }
  near(Json(apply(*from, {0, 0, 0})), {.8, 2.1, 3.3});
  const auto& snapshot = record(call, "workspace");
  CHECK(snapshot["schemaVersion"] == 1);
  CHECK(snapshot["rootFrame"] == "world");
  CHECK(snapshot["workspaceFrame"] == "ws");
  CHECK(snapshot["frames"].size() == 3);
  CHECK(snapshot["nativeCornerError"].get<double>() < 1e-5);
  CHECK(snapshot["contentFingerprint"].get<std::string>().find("fnv1a64:") == 0);
  REQUIRE(loadWorkspace(call, file, {{"targetFrame", Value::text("camera")}}).ok);
  near(record(call, "region")["position"], {.1, .2, 3.3});
  REQUIRE(loadWorkspace(call, file, {{"targetFrame", Value::text("ws")}}).ok);
  near(record(call, "region")["position"], {0, 0, 3});
  auto center = workspaceFile();
  center["workspaces"][0]["anchor"] = "center";
  file.write(center);
  REQUIRE(loadWorkspace(call, file).ok);
  near(record(call, "region")["position"], {.8, 2.1, 3.3});
}

TEST_CASE("P3 workspace native transform and region are consumed by existing pose transformation") {
  WorkspaceFile file;
  file.write(workspaceFile());
  test::OpCall loaded;
  REQUIRE_MESSAGE(loadWorkspace(loaded, file).ok, loaded.status.message);
  test::OpCall transform;
  transform.inputs = {{"poses", loaded.outputs.at("region")},
                      {"transform", loaded.outputs.at("toWorkspace")}};
  REQUIRE(transform
              .run("pose.transform_3d",
                   {{"sourceFrame", Value::text("world")}, {"targetFrame", Value::text("ws")}})
              .ok);
  const auto& local = record(transform, "poses")["items"][0];
  near(local["position"], {0, 0, 3});
  near(local["size"], {2, 4, 6});
  CHECK(local["frame"] == "ws");
}

TEST_CASE("P3 workspace explicit legacy YAML handles parent transforms and bottom center anchors") {
  WorkspaceFile file;
  file.write(legacy());
  test::OpCall call(workspaceRegistry());
  const ParamMap params{{"format", Value::text("xyz-unversioned-yaml")},
                        {"legacyUnit", Value::text("m")}};
  REQUIRE_MESSAGE(loadWorkspace(call, file, params).ok, call.status.message);
  near(record(call, "region")["position"], {.8, 2.1, 6.3});
  CHECK(record(call, "region")["frame"] == "World");
  CHECK(record(call, "workspace")["frames"].size() == 3);
  file.write(legacy(false));
  REQUIRE(loadWorkspace(call, file, params).ok);
  near(record(call, "region")["position"], {.1, .2, 3.3});
  CHECK_FALSE(loadWorkspace(call, file, {{"format", Value::text("xyz-unversioned-yaml")}}).ok);
  CHECK(call.outputs.empty());
  CHECK_FALSE(loadWorkspace(call, file).ok);
  CHECK(call.outputs.empty());
  for (const auto& bytes : {legacy(false) + "version: 1\n", legacy(false) + "parent_id: 7\n",
                            legacy(false) + "height: 2\n", legacy(false) + "camera_matrix: []\n",
                            std::string("pose: &self [*self]\nlength: 2\nwidth: 4\nheight: 6\n")}) {
    file.write(bytes);
    CHECK_FALSE(loadWorkspace(call, file, params).ok);
    CHECK(call.outputs.empty());
  }
}

TEST_CASE(
    "P3 workspace overrides validate frames and keep center dimensions and transforms consistent") {
  WorkspaceFile file;
  file.write(workspaceFile());
  test::OpCall call(workspaceRegistry());
  auto override = poses::make3D("observed-tote", "world", {4, 5, 6}, {0, 0, 0, 1}, {1, 2, 4});
  call.inputs["overrideRegion"] = Data::record(override);
  REQUIRE_MESSAGE(loadWorkspace(call, file).ok, call.status.message);
  near(record(call, "region")["position"], {4, 5, 6});
  near(record(call, "region")["size"], {1, 2, 4});
  near(Json(apply(*call.outputs.at("fromWorkspace").asTransform(), {0, 0, 0})), {4, 5, 4});
  CHECK(record(call, "workspace")["overrideApplied"] == true);
  CHECK(record(call, "workspace")["overrideId"] == "observed-tote");
  CHECK_FALSE(loadWorkspace(call, file, {{"targetFrame", Value::text("ws")}}).ok);
  CHECK(call.outputs.empty());
  override.data["frame"] = "ws";
  call.inputs["overrideRegion"] = Data::record(override);
  CHECK_FALSE(loadWorkspace(call, file, {{"targetFrame", Value::text("ws")}}).ok);
  CHECK(call.outputs.empty());
  override.data["frame"] = "wrong";
  call.inputs["overrideRegion"] = Data::record(override);
  CHECK_FALSE(loadWorkspace(call, file).ok);
  CHECK(call.outputs.empty());
  override.data["frame"] = "world";
  override.data["position"][2] = 1.5e308;
  call.inputs["overrideRegion"] = Data::record(override);
  CHECK_FALSE(loadWorkspace(call, file).ok);
  CHECK(call.outputs.empty());
  file.write(workspaceFile());
}

TEST_CASE("P3 workspace rejects malformed version units trees matrix and dimensions atomically") {
  WorkspaceFile file;
  std::vector<Json> bad;
  auto add = [&](auto mutate) {
    auto value = workspaceFile();
    mutate(value);
    bad.push_back(value);
  };
  add([](Json& v) { v["schemaVersion"] = 2; });
  add([](Json& v) { v["unit"] = "mm"; });
  add([](Json& v) { v["rootFrame"] = ""; });
  add([](Json& v) { v["frames"].push_back(v["frames"][0]); });
  add([](Json& v) { v["frames"][0]["parent"] = "missing"; });
  add([](Json& v) { v["frames"][1]["parent"] = "ws"; });
  add([](Json& v) { v["frames"][0]["id"] = "world"; });
  add([](Json& v) { v["workspaces"][0]["frame"] = "unknown"; });
  add([](Json& v) { v["workspaces"][0]["size"][0] = 0; });
  add([](Json& v) { v["workspaces"][0]["anchor"] = "top"; });
  add([](Json& v) { v["frames"][0]["transform"][0] = -1; });
  add([](Json& v) { v["frames"][0]["transform"][0] = 2; });
  add([](Json& v) { v["frames"][0]["transform"][12] = 1e-12; });
  add([](Json& v) { v["frames"][0]["transform"].erase(15); });
  add([](Json& v) { v.erase("unit"); });
  add([](Json& v) { v["camera"] = Json::object(); });
  add([](Json& v) { v["workspaces"] = Json::array(); });
  add([](Json& v) { v["frames"][0]["transform"][3] = std::numeric_limits<double>::infinity(); });
  test::OpCall call(workspaceRegistry());
  for (const auto& value : bad) {
    file.write(value);
    CHECK_FALSE(loadWorkspace(call, file).ok);
    CHECK(call.outputs.empty());
  }
  file.write(std::string("{\"schemaVersion\":1,\"schemaVersion\":1}"));
  CHECK_FALSE(loadWorkspace(call, file).ok);
  CHECK(call.outputs.empty());
  file.write(workspaceFile());
  CHECK_FALSE(loadWorkspace(call, file, {{"workspaceId", Value::text("missing")}}).ok);
  CHECK_FALSE(loadWorkspace(call, file, {{"targetFrame", Value::text("missing")}}).ok);
  CHECK_FALSE(loadWorkspace(call, file, {{"maxTranslationError", Value::number(0)}}).ok);
}

TEST_CASE("P3 workspace refuses native precision loss and enforces file and frame budgets") {
  WorkspaceFile file;
  auto value = workspaceFile();
  value["frames"][1]["transform"][3] = 1000000.1;
  file.write(value);
  test::OpCall call(workspaceRegistry());
  CHECK_FALSE(loadWorkspace(call, file).ok);
  CHECK(call.outputs.empty());
  REQUIRE(loadWorkspace(call, file, {{"maxTranslationError", Value::number(.2)}}).ok);
  CHECK(record(call, "workspace")["nativeCornerError"].get<double>() > 1e-5);
  file.write(std::string(1024 * 1024 + 1, ' '));
  CHECK(loadWorkspace(call, file).code == "resource_limit");
  CHECK(call.outputs.empty());
  value = workspaceFile();
  for (int i = 0; i < 255; ++i)
    value["frames"].push_back(
        {{"id", "extra-" + std::to_string(i)}, {"parent", "world"}, {"transform", translation()}});
  file.write(value);
  CHECK(loadWorkspace(call, file).code == "resource_limit");
  CHECK(call.outputs.empty());
}

TEST_CASE("P3 workspace rereads same-size overwritten files through the real scheduler") {
  WorkspaceFile file;
  auto document = workspaceFile();
  file.write(document);
  const auto graph =
      test::makeGraph({{"ws", "workspace.load", {{"path", file.path.u8string()}}}}, {});
  std::vector<std::string> fingerprints;
  for (int run = 0; run < 2; ++run) {
    test::Session session(graph, {}, {}, true, 1);
    const auto& log = session.wait();
    REQUIRE_MESSAGE(log.runStatus() == "ok", Json(log.events).dump());
    Data result, snapshot;
    REQUIRE(exec::ResultStore::instance().get(log.runId, "ws", "region", result));
    REQUIRE(exec::ResultStore::instance().get(log.runId, "ws", "workspace", snapshot));
    REQUIRE(result.asRecord() != nullptr);
    CHECK(result.asRecord()->data["size"][0] == (run == 0 ? 2 : 3));
    fingerprints.push_back(snapshot.asRecord()->data["contentFingerprint"]);
    document["workspaces"][0]["size"][0] = 3;
    file.write(document);
  }
  CHECK(fingerprints[0] != fingerprints[1]);
}

TEST_CASE("P3 workspace cancellation never publishes a partial workspace or transform") {
  WorkspaceFile file;
  file.write(workspaceFile());
  const auto* op = workspaceRegistry().find("workspace.load");
  REQUIRE(op != nullptr);
  CHECK_FALSE(op->capabilities.deterministic);
  for (int after : {1, 4}) {
    class Cancel final : public ExecContext {
    public:
      explicit Cancel(int after) : after_(after) {}
      bool cancelled() const override { return ++polls >= after_; }
      void progress(float, std::string_view) override {}
      void log(LogLevel, std::string) override {}
      const std::filesystem::path& baseDir() const override { return base; }
      int threadBudget() const override { return 1; }
      mutable int polls = 0;
      int after_;
      std::filesystem::path base;
    } context(after);
    ParamMap params;
    for (const auto& p : op->params)
      params[p.name] = p.def;
    params["path"] = Value::text(file.path.u8string());
    std::unordered_map<std::string, Data> input, output;
    Outputs outputs(output);
    CHECK(op->compute(Inputs(input), ParamView(params, context.base), outputs, context).code ==
          "cancelled");
    CHECK(output.empty());
  }
}
