#include "helpers.h"

#include <Eigen/Geometry>
#include <fstream>

#include "lyflow/c_api.h"
#include "lyflow/client.hpp"
#include "lyflow/pose.h"

namespace {
using namespace lyflow;
using namespace lyflow::test;

Json example(const char* name) {
  std::ifstream input(std::filesystem::path(LYFLOW_EXAMPLES_DIR) / name);
  REQUIRE(input.good());
  return Json::parse(input);
}

Json outputs(const std::string& id) {
  char* value = lyflow_run_outputs(id.c_str());
  REQUIRE(value != nullptr);
  const std::string text(value);
  lyflow_string_free(value);
  return Json::parse(text);
}

void checkHeightRules(const Json& out, bool observed) {
  const auto& height = out.at("height").at("value");
  CHECK(height.at("ok") == true);
  CHECK(height.at("unit") == "m");
  CHECK(std::abs(height.at("value").get<double>() - 1) < 1e-6);
  CHECK(out.at("top").at("elementCount") == 25);
  CHECK(out.at("rest").at("elementCount") == 443);
  const auto& relative = out.at("relativeHeight").at("value").at("data").at("items");
  REQUIRE(relative.size() == 1);
  CHECK(relative[0].at("valid") == true);
  CHECK(relative[0].at("id") == "part");
  CHECK(std::abs(relative[0].at("height").get<double>() - 1) < 1e-6);
  const auto& objects = out.at("objects").at("value");
  std::string why;
  REQUIRE_MESSAGE(poses::validate(Record{objects.at("type"), objects.at("data")}, why), why);
  CHECK(objects.at("data").at("items")[0].at("pickable") == false);
  const auto& patches = out.at("patches").at("value").at("data").at("items");
  REQUIRE(patches.size() == 2);
  CHECK(out.at("uncovered").at("elementCount") == 2);
  CHECK(out.at("toteStatus").at("value").at("data").at("state") ==
        (observed ? "occupied" : "unknown"));
}

void checkConstrained(const Json& out, double offset) {
  const Eigen::Matrix3d rotation = (Eigen::AngleAxisd(.12, Eigen::Vector3d::UnitX()) *
                                    Eigen::AngleAxisd(-.08, Eigen::Vector3d::UnitY()) *
                                    Eigen::AngleAxisd(.2, Eigen::Vector3d::UnitZ()))
                                       .toRotationMatrix();
  const auto& matrix = out.at("transform").at("value").at("m");
  REQUIRE(matrix.size() == 16);
  for (int row = 0; row < 3; ++row)
    for (int column = 0; column < 3; ++column)
      CHECK(std::abs(matrix[row * 4 + column].get<double>() - rotation(row, column)) < 1e-6);
  CHECK(std::abs(matrix[3].get<double>() - (.05 + offset)) < 1e-6);
  CHECK(std::abs(matrix[7].get<double>() + .02) < 1e-6);
  CHECK(std::abs(matrix[11].get<double>() - .01) < 1e-6);
  const auto& quality = out.at("quality").at("value").at("data");
  CHECK(quality.at("accepted") == true);
  CHECK(quality.at("weightedRmse").get<double>() < 1e-6);
  CHECK(quality.at("maxError").get<double>() < 1e-5);
  REQUIRE(quality.at("items").size() == 6);
}

Json abiRun(const Json& graph, const std::string& id, const Json& params) {
  const auto source = graph.dump(), parameters = params.dump();
  const std::string base = std::filesystem::path(LYFLOW_EXAMPLES_DIR).u8string();
  lyflow_run_options options{};
  options.run_id = id.c_str();
  options.base_dir = base.c_str();
  options.params_json = parameters.c_str();
  options.no_reuse = 1;
  lyflow_run* run = lyflow_run_start(source.c_str(), &options, nullptr, nullptr);
  REQUIRE(run != nullptr);
  lyflow_run_join(run);
  const auto result = outputs(id);
  lyflow_run_free(run);
  return result;
}
}

TEST_CASE(
    "P2 cloud rule examples preserve physical height planes unknown evidence and depth masks") {
  Session cloud(example("p2-height-rules.lyflow.json"), LYFLOW_EXAMPLES_DIR, {}, false, 1, true);
  const auto& log = cloud.wait();
  REQUIRE_MESSAGE(log.runStatus() == "ok", Json(log.events).dump());
  checkHeightRules(outputs(log.runId), false);
  Session depth(example("p2-depth-height.lyflow.json"), LYFLOW_EXAMPLES_DIR, {}, false, 1, true);
  const auto& depthLog = depth.wait();
  REQUIRE_MESSAGE(depthLog.runStatus() == "ok", Json(depthLog.events).dump());
  const auto result = outputs(depthLog.runId);
  const auto& height = result.at("height").at("value");
  CHECK(height.at("depth") == "f32");
  CHECK(std::abs(height.at("min")[0].get<double>() + .1) < 1e-6);
  CHECK(std::abs(height.at("max")[0].get<double>() - .1) < 1e-6);
  CHECK(result.at("validMask").at("value").at("min")[0] == 0);
  CHECK(result.at("validMask").at("value").at("max")[0] == 255);
  CHECK(result.at("display").at("value").at("max")[0] == 178);
}

TEST_CASE("P2 constrained example C ABI and SDK agree on target overrides without leaking state") {
  const Json graph = example("p2-constrained-fit.lyflow.json");
  Json modified =
      Json::parse(graph.at("params").at("constraintItems").at("default").get<std::string>());
  for (auto& item : modified)
    item["target"][0] = item["target"][0].get<double>() + .01;
  Client client(LYFLOW_TEST_RUNTIME);
  for (int attempt = 0; attempt < 3; ++attempt) {
    const bool changed = attempt == 1;
    const Json params = changed ? Json{{"constraintItems", modified.dump()}} : Json::object();
    const std::string id = "p2-constrained-" + std::to_string(attempt);
    const auto abi = abiRun(graph, id, params);
    checkConstrained(abi, changed ? .01 : 0);
    RunOptions options;
    options.runId = id + "-sdk";
    options.baseDir = LYFLOW_EXAMPLES_DIR;
    options.paramsJson = params.dump();
    options.noReuse = true;
    const auto sdk = client.run(graph.dump(), options);
    REQUIRE_MESSAGE(sdk.ok(), sdk.summary);
    const Json sdkOutputs = Json::parse(sdk.outputs);
    checkConstrained(sdkOutputs, changed ? .01 : 0);
    for (const char* name : {"transform", "quality"})
      CHECK(abi.at(name).at("value") == sdkOutputs.at(name).at("value"));
  }
  const auto rules = example("p2-height-rules.lyflow.json");
  for (const bool observed : {true, false})
    checkHeightRules(
        abiRun(rules, observed ? "p2-observed" : "p2-unobserved", {{"observationValid", observed}}),
        observed);
}

TEST_CASE("P2 matching examples preserve CAD anchors across save load C ABI and SDK runs") {
  const Eigen::Matrix3d expected = (Eigen::AngleAxisd(1.1, Eigen::Vector3d::UnitZ()) *
                                    Eigen::AngleAxisd(-.6, Eigen::Vector3d::UnitY()) *
                                    Eigen::AngleAxisd(.3, Eigen::Vector3d::UnitX()))
                                       .toRotationMatrix();
  const auto check = [&](const Json& out) {
    const auto& poses = out.at("instances").at("value").at("data").at("items");
    REQUIRE(poses.size() == 1);
    const auto p = poses[0].at("position").get<std::array<double, 3>>();
    CHECK((Eigen::Vector3d(p[0], p[1], p[2]) - Eigen::Vector3d(.17, -.09, .43)).norm() < .0002);
    const auto rotation =
        lyflow::poses::rotationMatrix(poses[0].at("rotation").get<lyflow::poses::Quaternion>());
    Eigen::Matrix3d actual;
    for (int row = 0; row < 3; ++row)
      for (int col = 0; col < 3; ++col)
        actual(row, col) = rotation[row * 3 + col];
    CHECK(Eigen::AngleAxisd(actual.transpose() * expected).angle() < .003);
    CHECK(poses[0].at("pickable") == false);
    const auto& quality = out.at("quality").at("value").at("data");
    REQUIRE(quality.at("items").size() == 1);
    CHECK(quality.at("items")[0].at("rmse").get<double>() < .001);
    CHECK(quality.at("items")[0].at("inlierRatio").get<double>() >= .5);
    CHECK(quality.at("backend") == "kiss-matcher-max-core-gnc");
    const auto& center = out.at("template").at("value").at("data").at("sourceCenter");
    CHECK(std::abs(center[0].get<double>() + .0025) < 1e-7);
    CHECK(std::abs(center[1].get<double>() - .001) < 1e-7);
    CHECK(std::abs(center[2].get<double>() - .009) < 1e-7);
    CHECK(out.at("visible").at("elementCount").get<int>() > 0);
  };
  Client client(LYFLOW_TEST_RUNTIME);
  const auto temporary =
      std::filesystem::temp_directory_path() /
      ("p2-cad-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
       ".pcd");
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code ec;
      std::filesystem::remove(path, ec);
    }
  } cleanup{temporary};
  OpCall loader;
  loader.base = LYFLOW_EXAMPLES_DIR;
  REQUIRE(loader.run("io.load_cloud", {{"path", Value::text("p2-model-cad.pcd")}}).ok);
  OpCall save;
  save.inputs = {{"cloud", loader.outputs.at("cloud")}};
  REQUIRE(save.run("io.save_pcd", {{"path", Value::text(temporary.u8string())}}).ok);
  for (const char* preset : {"fast", "balanced", "precise"}) {
    CAPTURE(preset);
    const std::string name = std::string("p2-matching-") + preset + ".lyflow.json";
    const auto graph = example(name.c_str());
    const std::string id = std::string("p2-match-") + preset;
    check(abiRun(graph, id, Json::object()));
    RunOptions options;
    options.runId = id + "-sdk-roundtrip";
    options.baseDir = LYFLOW_EXAMPLES_DIR;
    options.paramsJson = Json{{"modelFile", temporary.u8string()}}.dump();
    options.noReuse = true;
    const auto result = client.run(graph.dump(), options);
    REQUIRE_MESSAGE(result.ok(), result.summary);
    check(Json::parse(result.outputs));
  }
}
