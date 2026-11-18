#include "helpers.h"
#include "lyflow/cloud_groups.h"
#include "lyflow/graph_migration.h"
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
}
TEST_CASE(
    "P5 primitive crop exact finite solids preserve channels and overlapping stable source groups") {
  using namespace lyflow;
  test::OpCall c;
  auto source = cropCloud();
  c.inputs = {{"cloud", source}, {"poses", cropPoses(true)}};
  REQUIRE(c.run("cloud.query_primitives").ok);
  const auto* ids = c.outputs.at("innerIndices").asIndices();
  REQUIRE(ids);
  CHECK(ids->sourceCloudId == source.asCloud()->id);
  CHECK(ids->values == std::vector<std::int32_t>{0, 1, 3, 4});
  test::OpCall extract;
  extract.inputs = {{"cloud", source}, {"indices", c.out("innerIndices")}};
  REQUIRE(extract.run("segment.extract_indices").ok);
  CHECK(extract.out("selected").asCloud()->rgb.size() == 12);
  CHECK(extract.out("selected").asCloud()->intensity == std::vector<float>{1, 2, 4, 5});
  CHECK(c.outputs.at("groupIndices").asIndices()->values ==
        std::vector<std::int32_t>{0, 1, 3, 4, 0, 1, 3, 4});
  test::OpCall judge;
  judge.inputs = {{"support", c.out("support")}};
  REQUIRE(judge.run("cloud.classify_primitive_hits", {{"maxCount", Value::integer(3)}}).ok);
  CHECK(judge.out("collisionPoses").asRecord()->data["items"].size() == 2);
  CHECK(judge.out("collisionPoses").asRecord()->data["items"][0]["pickable"] == false);
  CHECK(judge.out("collisionPoses").asRecord()->data["items"][0]["classLabel"] == "箱体");
  REQUIRE(judge.run("cloud.classify_primitive_hits", {{"maxCount", Value::integer(4)}}).ok);
  CHECK(judge.out("freePoses").asRecord()->data["items"].size() == 2);
  CHECK(extract.out("selected").asCloud()->intensity == std::vector<float>{1, 2, 4, 5});
  REQUIRE(c.run("cloud.query_primitives",
                {{"shape", Value::text("cylinder")}, {"includeBoundary", Value::boolean(false)}})
              .ok);
  CHECK(c.outputs.at("innerIndices").asIndices()->values == std::vector<std::int32_t>{0});
  REQUIRE(c.run("cloud.query_primitives", {{"shape", Value::text("sphere")},
                                           {"dimensions", Value::text("override")},
                                           {"size", Value::vec({1, 1, 1})}})
              .ok);
  CHECK(c.outputs.at("innerIndices").asIndices()->values == std::vector<std::int32_t>{0, 1, 3});
  CHECK_FALSE(c.run("cloud.query_primitives", {{"shape", Value::text("sphere")}}).ok);
  CHECK(c.outputs.empty());
  CHECK(source.asCloud()->pointCount() == 6);
}
TEST_CASE(
    "P5 primitive removal handles surface anchor padding and native model radius with readonly channels") {
  using namespace lyflow;
  test::OpCall c;
  const auto source = cropCloud();
  c.inputs = {{"cloud", source}, {"poses", cropPoses()}};
  REQUIRE(c.run("cloud.query_primitives", {{"anchor", Value::text("top")}}).ok);
  test::OpCall remove;
  remove.inputs = {{"cloud", source}, {"indices", c.out("innerIndices")}};
  REQUIRE(remove.run("cloud.remove_indices", {{"outputMode", Value::text("preserve_order")}}).ok);
  CHECK(remove.out("cloud").asCloud()->pointCount() == 6);
  CHECK(std::isnan(remove.out("cloud").asCloud()->xyz[0]));
  CHECK(std::isfinite(source.asCloud()->xyz[0]));
  CHECK(remove.out("cloud").asCloud()->rgb == source.asCloud()->rgb);
  CHECK(c.outputs.at("validity").asTensor()->data == std::vector<float>{0, 0, 1, 0, 1, 1});
  PointCloud model;
  model.push(0, 0, 0);
  c.inputs["model"] = Data::cloud(std::move(model));
  REQUIRE(c.run("cloud.query_primitives",
                {{"mode", Value::text("model_radius")}, {"radius", Value::number(.1)}})
              .ok);
  CHECK(c.outputs.at("innerIndices").asIndices()->values == std::vector<std::int32_t>{0});
  remove.inputs["indices"] = c.out("innerIndices");
  REQUIRE(remove.run("cloud.remove_indices").ok);
  CHECK(remove.out("cloud").asCloud()->intensity == std::vector<float>{2, 3, 4, 5, 6});
  remove.inputs["cloud"] = cropCloud();
  CHECK_FALSE(remove.run("cloud.remove_indices").ok);
  CHECK(remove.status.portName == "indices");
  CHECK_FALSE(c.run("cloud.query_primitives",
                    {{"mode", Value::text("model_radius")}, {"radius", Value::number(-1)}})
                  .ok);
  CHECK(c.outputs.empty());
}
TEST_CASE("P5 crop validates frames empty instances invalid coordinates and output bindings") {
  using namespace lyflow;
  test::OpCall c;
  c.inputs = {{"cloud", cropCloud()}, {"poses", cropPoses(false, "other")}};
  CHECK_FALSE(c.run("cloud.query_primitives").ok);
  c.inputs["posesToCloud"] = Data::transform(Transform{});
  REQUIRE(c.run("cloud.query_primitives").ok);
  const auto* cloud = c.inputs.at("cloud").asCloud();
  std::vector<cloud_groups::Group> groups;
  std::string frame, why;
  CHECK(cloud_groups::parse(*c.outputs.at("groups").asRecord(), *cloud,
                            *c.outputs.at("groupIndices").asIndices(), groups, frame, why));
  CHECK(frame == "world");
  c.inputs = {{"cloud", cropCloud()}, {"poses", Data::record(poses::make3DList(Json::array()))}};
  REQUIRE(c.run("cloud.query_primitives").ok);
  CHECK(c.out("innerIndices").asIndices()->values.empty());
  CHECK(c.out("outerIndices").asIndices()->values.size() == 6);
  CHECK(c.out("support").asRecord()->data["items"].empty());
  auto nanCloud = *c.inputs.at("cloud").asCloud();
  nanCloud.xyz[0] = std::numeric_limits<float>::quiet_NaN();
  c.inputs["cloud"] = Data::cloud(std::move(nanCloud));
  REQUIRE(c.run("cloud.query_primitives").ok);
  CHECK(c.out("support").asRecord()->data["unknownCount"] == 1);
  CHECK(c.outputs.at("invalidIndices").asIndices()->values == std::vector<std::int32_t>{0});

  SUBCASE("Migration preserves old output names and moves writable threshold and output mode") {
    auto legacy = test::makeGraph({{"source", "util.reroute"},
                                   {"poses", "util.reroute"},
                                   {"old", "cloud.remove_primitives", {{"mode", "shape"}}}},
                                  {{"source.out", "old.cloud"}, {"poses.out", "old.poses"}});
    legacy["params"] = {{"limit", {{"default", 4}, {"binds", {"old.maxCount"}}}},
                        {"order", {{"default", "preserve_order"}, {"binds", {"old.outputMode"}}}}};
    legacy["outputs"] = {{"removed", {{"node", "old"}, {"port", "inner"}}},
                         {"masked", {{"node", "old"}, {"port", "cloud"}}},
                         {"quality", {{"node", "old"}, {"port", "quality"}}},
                         {"hits", {{"node", "old"}, {"port", "groupIndices"}}}};
    const Json request{{"schemaVersion", 1},
                       {"rules", {{{"kind", "primitiveCrop.v1"}, {"source", {{"node", "old"}}}}}}};
    Json migrated;
    GraphMigrationError issue;
    REQUIRE_MESSAGE(migrateGraph(ensureRegistry(), legacy, request, migrated, issue),
                    issue.message);
    CHECK(migrated["outputs"]["removed"] == Json{{"node", "old__extract"}, {"port", "selected"}});
    CHECK(migrated["outputs"]["masked"] == Json{{"node", "old__remove"}, {"port", "cloud"}});
    CHECK(migrated["outputs"]["quality"] == Json{{"node", "old__classify"}, {"port", "quality"}});
    CHECK(migrated["outputs"]["hits"] == Json{{"node", "old"}, {"port", "groupIndices"}});
    CHECK(migrated["params"]["limit"]["binds"] == Json::array({"old__classify.maxCount"}));
    CHECK(migrated["params"]["order"]["binds"] == Json::array({"old__remove.outputMode"}));
  }
}
TEST_CASE(
    "P5 crop unknown support never certifies free geometry and bounded cancellation remains atomic") {
  using namespace lyflow;
  test::OpCall c;
  auto source = *cropCloud().asCloud();
  source.xyz[0] = std::numeric_limits<float>::quiet_NaN();
  c.inputs = {{"cloud", Data::cloud(source)}, {"poses", cropPoses()}};
  REQUIRE(c.run("cloud.query_primitives").ok);
  test::OpCall judge;
  judge.inputs = {{"support", c.out("support")}};
  REQUIRE(judge.run("cloud.classify_primitive_hits", {{"maxCount", Value::integer(100)}}).ok);
  CHECK(judge.out("freePoses").asRecord()->data["items"].empty());
  CHECK(judge.out("unknownPoses").asRecord()->data["items"].size() == 1);
  CHECK(judge.out("quality").asRecord()->data["items"][0]["state"] == "unknown");
  REQUIRE(judge.run("cloud.classify_primitive_hits").ok);
  CHECK(judge.out("quality").asRecord()->data["items"][0]["state"] == "collision");
  CHECK(std::isnan(c.inputs.at("cloud").asCloud()->xyz[0]));
  REQUIRE(c.run("cloud.query_primitives",
                {{"heightUnknown", Value::boolean(true)}, {"maxHeight", Value::number(.1)}})
              .ok);
  CHECK(c.outputs.at("innerIndices").asIndices()->values == std::vector<std::int32_t>{1, 3});
  test::OpCall empty;
  empty.inputs = {{"cloud", Data::cloud(PointCloud{})}, {"poses", cropPoses()}};
  REQUIRE(empty.run("cloud.query_primitives").ok);
  judge.inputs["support"] = empty.out("support");
  REQUIRE(judge.run("cloud.classify_primitive_hits").ok);
  CHECK(judge.out("freePoses").asRecord()->data["items"].empty());
  CHECK(judge.out("unknownPoses").asRecord()->data["items"].size() == 1);
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
  for (int stop : {1, 3}) {
    CAPTURE(stop);
    const auto* op = ensureRegistry().find("cloud.query_primitives");
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
