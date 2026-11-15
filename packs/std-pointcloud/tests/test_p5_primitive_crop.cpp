#include "helpers.h"
#include "lyflow/cloud_groups.h"
#include "lyflow/pose.h"
#include <limits>

namespace {
using namespace lyflow;
using Json = nlohmann::json;
Data cropCloud() {
  PointCloud c;
  for (auto p :
       {std::array<float, 3>{0, 0, 0}, {.5f, 0, 0}, {1, 0, 0}, {0, .5f, 0}, {0, 0, 1}, {2, 2, 2}}) {
    c.push(p[0], p[1], p[2]);
    c.rgb.insert(c.rgb.end(), {7, 8, 9});
    c.normals.insert(c.normals.end(), {0, 0, 1});
    c.intensity.push_back(static_cast<float>(c.pointCount()));
  }
  return Data::cloud(std::move(c));
}
Data cropPoses(bool two = false, const char* frame = "world") {
  Json a = Json::array();
  for (int i = 0; i < (two ? 2 : 1); ++i) {
    auto pose = poses::make3D(i ? "B" : "A", frame, {0, 0, 0}, {0, 0, 0, 1}, {1, 1, 2});
    pose.data["classLabel"] = "箱体";
    pose.data["pickable"] = true;
    a.push_back(pose.data);
  }
  return Data::record(poses::make3DList(a, frame));
}
const Json& cropReport(const test::OpCall& c) { return c.outputs.at("quality").asRecord()->data; }
}
TEST_CASE(
    "P5 primitive crop exact finite solids preserve channels and overlapping stable source groups") {
  using namespace lyflow;
  test::OpCall c;
  auto source = cropCloud();
  c.inputs = {{"cloud", source}, {"poses", cropPoses(true)}};
  REQUIRE(c.run("cloud.crop_primitives", {{"maxCount", Value::integer(3)}}).ok);
  const auto* ids = c.outputs.at("innerIndices").asIndices();
  REQUIRE(ids);
  CHECK(ids->sourceCloudId == source.asCloud()->id);
  CHECK(ids->values == std::vector<std::int32_t>{0, 1, 3, 4});
  CHECK(c.outputs.at("inner").asCloud()->rgb.size() == 12);
  CHECK(c.outputs.at("inner").asCloud()->intensity == std::vector<float>{1, 2, 4, 5});
  CHECK(c.outputs.at("groupIndices").asIndices()->values ==
        std::vector<std::int32_t>{0, 1, 3, 4, 0, 1, 3, 4});
  CHECK(c.outputs.at("collisionPoses").asRecord()->data["items"].size() == 2);
  CHECK(c.outputs.at("collisionPoses").asRecord()->data["items"][0]["pickable"] == false);
  CHECK(c.outputs.at("collisionPoses").asRecord()->data["items"][0]["classLabel"] == "箱体");
  REQUIRE(c.run("cloud.crop_primitives",
                {{"shape", Value::text("cylinder")}, {"includeBoundary", Value::boolean(false)}})
              .ok);
  CHECK(c.outputs.at("innerIndices").asIndices()->values == std::vector<std::int32_t>{0});
  REQUIRE(c.run("cloud.crop_primitives", {{"shape", Value::text("sphere")},
                                          {"dimensions", Value::text("override")},
                                          {"size", Value::vec({1, 1, 1})}})
              .ok);
  CHECK(c.outputs.at("innerIndices").asIndices()->values == std::vector<std::int32_t>{0, 1, 3});
  CHECK_FALSE(c.run("cloud.crop_primitives", {{"shape", Value::text("sphere")}}).ok);
  CHECK(c.outputs.empty());
  CHECK(source.asCloud()->pointCount() == 6);
}
TEST_CASE(
    "P5 primitive removal handles surface anchor padding and native model radius with readonly channels") {
  using namespace lyflow;
  test::OpCall c;
  const auto source = cropCloud();
  c.inputs = {{"cloud", source}, {"poses", cropPoses()}};
  REQUIRE(c.run("cloud.remove_primitives",
                {{"anchor", Value::text("top")}, {"outputMode", Value::text("preserve_order")}})
              .ok);
  CHECK(c.outputs.at("cloud").asCloud()->pointCount() == 6);
  CHECK(std::isnan(c.outputs.at("cloud").asCloud()->xyz[0]));
  CHECK(std::isfinite(source.asCloud()->xyz[0]));
  CHECK(c.outputs.at("cloud").asCloud()->rgb == source.asCloud()->rgb);
  CHECK(c.outputs.at("validity").asTensor()->data == std::vector<float>{0, 0, 1, 0, 1, 1});
  PointCloud model;
  model.push(0, 0, 0);
  c.inputs["model"] = Data::cloud(std::move(model));
  REQUIRE(c.run("cloud.remove_primitives",
                {{"mode", Value::text("model_radius")}, {"radius", Value::number(.1)}})
              .ok);
  CHECK(c.outputs.at("innerIndices").asIndices()->values == std::vector<std::int32_t>{0});
  CHECK(c.outputs.at("cloud").asCloud()->intensity == std::vector<float>{2, 3, 4, 5, 6});
  CHECK_FALSE(c.run("cloud.remove_primitives",
                    {{"mode", Value::text("model_radius")}, {"radius", Value::number(-1)}})
                  .ok);
  CHECK(c.outputs.empty());
}
TEST_CASE("P5 crop validates frames empty instances invalid coordinates and output bindings") {
  using namespace lyflow;
  test::OpCall c;
  c.inputs = {{"cloud", cropCloud()}, {"poses", cropPoses(false, "other")}};
  CHECK_FALSE(c.run("cloud.crop_primitives").ok);
  c.inputs["posesToCloud"] = Data::transform(Transform{});
  REQUIRE(c.run("cloud.crop_primitives").ok);
  const auto* cloud = c.inputs.at("cloud").asCloud();
  std::vector<cloud_groups::Group> groups;
  std::string frame, why;
  CHECK(cloud_groups::parse(*c.outputs.at("groups").asRecord(), *cloud,
                            *c.outputs.at("groupIndices").asIndices(), groups, frame, why));
  CHECK(frame == "world");
  c.inputs = {{"cloud", cropCloud()}, {"poses", Data::record(poses::make3DList(Json::array()))}};
  REQUIRE(c.run("cloud.crop_primitives").ok);
  CHECK(c.outputs.at("inner").asCloud()->pointCount() == 0);
  CHECK(c.outputs.at("outer").asCloud()->pointCount() == 6);
  CHECK(c.outputs.at("freePoses").asRecord()->data["items"].empty());
  auto nanCloud = *c.inputs.at("cloud").asCloud();
  nanCloud.xyz[0] = std::numeric_limits<float>::quiet_NaN();
  c.inputs["cloud"] = Data::cloud(std::move(nanCloud));
  REQUIRE(c.run("cloud.crop_primitives").ok);
  CHECK(cropReport(c)["unknownCount"] == 1);
  CHECK(c.outputs.at("invalidIndices").asIndices()->values == std::vector<std::int32_t>{0});
}
TEST_CASE(
    "P5 crop unknown support never certifies free geometry and bounded cancellation remains atomic") {
  using namespace lyflow;
  test::OpCall c;
  auto source = *cropCloud().asCloud();
  source.xyz[0] = std::numeric_limits<float>::quiet_NaN();
  c.inputs = {{"cloud", Data::cloud(source)}, {"poses", cropPoses()}};
  REQUIRE(c.run("cloud.crop_primitives", {{"maxCount", Value::integer(100)}}).ok);
  CHECK(c.outputs.at("freePoses").asRecord()->data["items"].empty());
  CHECK(c.outputs.at("unknownPoses").asRecord()->data["items"].size() == 1);
  CHECK(c.outputs.at("quality").asRecord()->data["items"][0]["state"] == "unknown");
  CHECK(std::isnan(c.inputs.at("cloud").asCloud()->xyz[0]));
  REQUIRE(c.run("cloud.crop_primitives",
                {{"heightUnknown", Value::boolean(true)}, {"maxHeight", Value::number(.1)}})
              .ok);
  CHECK(c.outputs.at("innerIndices").asIndices()->values == std::vector<std::int32_t>{1, 3});
  test::OpCall empty;
  empty.inputs = {{"cloud", Data::cloud(PointCloud{})}, {"poses", cropPoses()}};
  REQUIRE(empty.run("cloud.crop_primitives").ok);
  CHECK(empty.outputs.at("freePoses").asRecord()->data["items"].empty());
  CHECK(empty.outputs.at("unknownPoses").asRecord()->data["items"].size() == 1);
  struct Cancel final : ExecContext {
    mutable int polls = 0;
    int stop;
    std::filesystem::path path;
    explicit Cancel(int n) : stop(n) {}
    bool cancelled() const override { return ++polls >= stop; }
    void progress(float, std::string_view) override {}
    void log(LogLevel, std::string) override {}
    const std::filesystem::path& baseDir() const override { return path; }
    int threadBudget() const override { return 1; }
  };
  for (const char* id : {"cloud.crop_primitives", "cloud.remove_primitives"})
    for (int stop : {1, 3}) {
      CAPTURE(id);
      CAPTURE(stop);
      const auto* op = ensureRegistry().find(id);
      REQUIRE(op);
      ParamMap params;
      for (const auto& v : op->params)
        params[v.name] = v.def;
      Inputs inputs(c.inputs);
      std::unordered_map<std::string, Data> output;
      Outputs outputs(output);
      Cancel ctx(stop);
      ParamView view(params, ctx.path);
      CHECK(op->compute(inputs, view, outputs, ctx).code == "cancelled");
      CHECK(output.empty());
    }
}
