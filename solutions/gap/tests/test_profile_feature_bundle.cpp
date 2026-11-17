#include <doctest/doctest.h>
#include <cmath>

#include "helpers.h"
#include "lyflow/profile.h"
#include "lyflow_profile/features.h"
#include "lyflow_profile/merge.h"
#include "ops/profile_circles.h"
#include "ops/profile_feature_bundle.h"
#include "ops/profile_scan.h"

using namespace lyflow;
namespace scene = lyflow::packs::gap::profile_scene;
namespace {
using Json = nlohmann::json;
Data profile(PointCloud cloud, const std::string& id, const char* frame = "measurement",
             const char* plane = "XY") {
  std::vector<std::uint8_t> validity(cloud.pointCount(), 1);
  std::vector<std::uint32_t> indices;
  for (std::size_t i = 0; i < cloud.pointCount(); ++i) {
    indices.push_back(static_cast<std::uint32_t>(i * 3 + 2));
    cloud.intensity.push_back(static_cast<float>(i));
    cloud.rgb.insert(cloud.rgb.end(), {20, 30, 40});
  }
  Profile value;
  std::string why;
  const auto count = cloud.pointCount() * 3 + 5;
  REQUIRE(makeProfile(std::make_shared<const PointCloud>(std::move(cloud)),
                      {1, frame, "m", plane, "right", "acquisition", id, count},
                      std::move(validity), std::move(indices), value, why));
  return Data::profile(std::move(value));
}
Data source(const std::string& id = "camera-A", float shift = 0) {
  PointCloud cloud;
  for (int i = 0; i < 50; ++i) {
    const float x = .0002F * i;
    cloud.push(x, .17F + .05F * x + shift, 0);
  }
  return profile(std::move(cloud), id);
}
Data context(const Data& input) {
  return Data::record(profile_contract::geometryContext(input.asProfile()->descriptor));
}
Data foreignContext() {
  ProfileDescriptor space;
  space.frame = "template-frame";
  space.plane = "XY";
  space.handedness = "right";
  return Data::record(profile_contract::geometryContext(space));
}
ParamMap defaults(const std::vector<Param>& descriptions) {
  ParamMap values;
  for (const auto& parameter : descriptions)
    values[parameter.name] = parameter.def;
  return values;
}
profile_contract::RobustLineResult line(const Data& input) {
  test::StubContext ctx;
  Box2D roi{{-.001F, .16F}, {.011F, .18F}};
  Point2D anchor;
  anchor.p[0] = .011F;
  anchor.p[1] = .17F;
  const auto box = Data::box2d(roi), toward = Data::point2d(anchor);
  profile_contract::RobustLineInputs in{input, box, context(input), toward, context(input), {}, {}};
  auto parameters = defaults(profile_contract::robustLineParams());
  parameters["segmentPoints"] = Value::integer(0);
  profile_contract::RobustLineResult out;
  REQUIRE_MESSAGE(
      profile_contract::fitRobustLine(in, ParamView(parameters, ctx.baseDir()), out, ctx).ok,
      "真实公共拟合必须成功");
  return out;
}
scene::LineFeatureFields lineFields(const profile_contract::RobustLineResult& fit) {
  return {fit.profile,      fit.line,    fit.lineContext,     fit.innerEnd,
          fit.pointContext, fit.inliers, fit.weightedInliers, fit.quality};
}
scene::ReferenceFeatureFields reference(const Data& input, const char* method) {
  test::StubContext ctx;
  Point2D anchor;
  anchor.p[0] = .006F;
  anchor.p[1] = .17F;
  const auto point = Data::point2d(anchor);
  profile_contract::NearestResult nearest;
  REQUIRE(profile_contract::nearestSample({input, point, context(input)},
                                          profile_contract::NearestReference::Point, nearest, ctx)
              .ok);
  scene::ReferenceFeatureFields partial{
      nearest.profile, {}, {}, nearest.point, nearest.pointContext, nearest.selection, {}},
      full;
  std::string why;
  REQUIRE_MESSAGE(scene::completeReferenceFeature(method, partial, full, why), why);
  return full;
}
scene::CirclePairFeatureFields circles() {
  auto row = [](const char* id) {
    PointCloud cloud;
    for (const auto& center : {std::array<double, 3>{-.0035, -.0005, .0015},
                               std::array<double, 3>{.0035, -.0005, .0009}})
      for (int i = 0; i < 40; ++i) {
        const double angle = (200. + 140. * i / 39) * 3.14159265358979323846 / 180;
        cloud.push(static_cast<float>(center[0] + center[2] * std::cos(angle)),
                   static_cast<float>(center[1] + center[2] * std::sin(angle)), 0);
      }
    return profile(std::move(cloud), id);
  };
  scene::CirclePairInputs input;
  input.primary = row("camera-A");
  input.secondary = row("camera-B");
  profile_contract::MergeResult merged;
  std::string why;
  REQUIRE(profile_contract::mergeProfiles(input.secondary, input.primary, "require_equal", merged,
                                          why));
  input.merged = merged.profile;
  for (int side = 0; side < 2; ++side) {
    input.boxes[side] =
        Data::box2d(Box2D{{side ? .001F : -.006F, -.004F}, {side ? .006F : -.001F, .002F}});
    input.boxContexts[side] = context(input.merged);
  }
  const auto parameters = defaults(scene::circlePairParams());
  test::StubContext ctx;
  scene::CirclePairResult result;
  REQUIRE_MESSAGE(
      scene::fitProfileGapCircles(input, ParamView(parameters, ctx.baseDir()), result, ctx).ok,
      "真实场景双圆拟合必须成功");
  return {result.profiles, result.circles, result.contexts, result.inliers, result.quality};
}
Data sensor(const std::string& id, float shift) {
  PointCloud cloud;
  for (int i = 0; i < 40; ++i)
    cloud.push(.0002F * i, 0, .17F + .00001F * i + shift);
  return profile(std::move(cloud), id, "sensor", "XZ");
}
scene::LocationFields location(float shift = 0) {
  test::StubContext ctx;
  scene::ReadScanOptions options;
  options.measurementFrame = "measurement";
  scene::LocationFields result;
  REQUIRE(scene::buildReadScan(sensor("physical-A", shift), sensor("physical-B", shift), options,
                               result.scan, ctx)
              .ok);
  const auto& merged = *result.scan.asBundle()->field("merged");
  scene::RoiFields rois;
  for (std::size_t i = 0; i < 4; ++i) {
    rois.boxes[i] = Data::box2d(Box2D{{.001F * static_cast<float>(i), .16F},
                                      {.003F + .001F * static_cast<float>(i), .18F}});
    rois.contexts[i] = context(merged);
  }
  rois.info = {scene::kRoiInfoType,
               {{"schemaVersion", 3},
                {"source", "config"},
                {"datumSide", "left"},
                {"alignment", nullptr},
                {"overallMm", nullptr},
                {"cropStatus", nullptr},
                {"processing",
                 {{"method", "assemble"}, {"status", "complete"}, {"details", Json::object()}}}}};
  std::string why;
  REQUIRE(scene::makeRoiSet(rois, result.rois, why));
  return result;
}
class Cancel final : public ExecContext {
public:
  bool cancelled() const override { return true; }
  void progress(float, std::string_view) override {}
  void log(LogLevel, std::string) override {}
  const std::filesystem::path& baseDir() const override { return path; }
  int threadBudget() const override { return 1; }

private:
  std::filesystem::path path;
};
}

TEST_CASE("atomic line feature binds native observations indices geometry and quality") {
  const auto fit = line(source());
  const auto fields = lineFields(fit);
  Data feature;
  std::string why;
  REQUIRE_MESSAGE(scene::makeLineFeature(fields, feature, why), why);
  REQUIRE(feature.asBundle());
  CHECK(feature.asBundle()->fields.size() == 8);
  scene::LineFeatureFields read;
  REQUIRE(scene::readLineFeature(feature, read, why));
  CHECK(read.profile.profilePtr() == fit.profile.profilePtr());
  CHECK(read.line.valueJson() == fit.line.valueJson());
  CHECK(read.weightedInliers.asIndices()->values == fit.weightedInliers.asIndices()->values);
  Data unchanged = feature;
  auto mixed = fields;
  mixed.lineContext = foreignContext();
  CHECK_FALSE(scene::makeLineFeature(mixed, feature, why));
  CHECK(feature.asBundle() == unchanged.asBundle());
  mixed = fields;
  auto indices = *mixed.inliers.asIndices();
  indices.sourceCloudId++;
  mixed.inliers = Data::indices(indices);
  CHECK_FALSE(scene::makeLineFeature(mixed, feature, why));
}

TEST_CASE(
    "atomic reference feature completes fitted and nearest metadata once and rejects fake fitted geometry") {
  const auto input = source();
  const auto fit = line(input);
  scene::ReferenceFeatureFields partial{fit.profile,  fit.line,         fit.lineContext,
                                        fit.innerEnd, fit.pointContext, fit.indexMap,
                                        fit.quality},
      completed;
  Data feature;
  std::string why;
  REQUIRE_MESSAGE(scene::completeReferenceFeature("line_end", partial, completed, why), why);
  REQUIRE(scene::makeReferenceFeature(completed, feature, why));
  CHECK(feature.asBundle()->fields.size() == 7);
  CHECK(completed.selection.asRecord()->type == "gap.ReferenceSelection.v3");
  auto unfitted = partial;
  unfitted.quality = Data::record(Record{"gap.ReferencePoint.v3", Json::object()});
  CHECK_FALSE(scene::completeReferenceFeature("line_end", unfitted, completed, why));
  for (const auto* method : {"selected_point", "nearest_point"}) {
    CAPTURE(method);
    auto full = reference(input, method);
    REQUIRE_MESSAGE(scene::makeReferenceFeature(full, feature, why), why);
    scene::ReferenceFeatureFields read;
    REQUIRE(scene::readReferenceFeature(feature, read, why));
    CHECK(read.quality.asRecord()->data["lineFitted"] == false);
    CHECK(read.lineContext.asRecord()->type == "gap.GeometryUnavailable.v2");
    ProfileDescriptor descriptor;
    CHECK_FALSE(
        profile_contract::geometrySpace(read.line, read.lineContext.asRecord(), descriptor, why));
    full.pointContext = foreignContext();
    CHECK_FALSE(scene::makeReferenceFeature(full, feature, why));
  }
  const auto before = completed.profile.profilePtr();
  CHECK_FALSE(scene::completeReferenceFeature("selected_point", partial, completed, why));
  CHECK(completed.profile.profilePtr() == before);
}

TEST_CASE("atomic circle pair keeps both scene-selected profiles and indices in one payload") {
  auto fields = circles();
  Data feature;
  std::string why;
  REQUIRE_MESSAGE(scene::makeCirclePairFeature(fields, feature, why), why);
  CHECK(feature.asBundle()->fields.size() == 9);
  scene::CirclePairFeatureFields read;
  REQUIRE_MESSAGE(scene::readCirclePairFeature(feature, read, why), why);
  CHECK(read.profiles[0].profilePtr() == fields.profiles[0].profilePtr());
  CHECK(read.profiles[1].profilePtr() == fields.profiles[1].profilePtr());
  CHECK(read.inliers[0].asIndices()->sourceCloudId == read.profiles[0].asProfile()->cloud->id);
  const auto before = feature.asBundle();
  fields.contexts[1] = foreignContext();
  CHECK_FALSE(scene::makeCirclePairFeature(fields, feature, why));
  CHECK(feature.asBundle() == before);
  fields = read;
  std::swap(fields.inliers[0], fields.inliers[1]);
  CHECK_FALSE(scene::makeCirclePairFeature(fields, feature, why));
}

TEST_CASE("atomic location uses flat native fields and rejects role boxes from another space") {
  auto fields = location();
  Data feature;
  std::string why;
  REQUIRE_MESSAGE(scene::makeLocation(fields.scan, fields.rois, feature, why), why);
  CHECK(feature.asBundle()->fields.size() == 15);
  for (const auto& field : feature.asBundle()->fields)
    CHECK(field.second.kind() != Data::Kind::Bundle);
  scene::LocationFields read;
  REQUIRE_MESSAGE(scene::readLocation(feature, read, why), why);
  CHECK(read.scan.asBundle()->field("sensorPrimary")->profilePtr() ==
        fields.scan.asBundle()->field("sensorPrimary")->profilePtr());
  CHECK(read.rois.asBundle()->field("datum")->valueJson() ==
        fields.rois.asBundle()->field("datum")->valueJson());
  const auto before = feature.asBundle();
  Bundle wrong = *fields.rois.asBundle();
  wrong.set("datumContext", foreignContext());
  CHECK_FALSE(scene::makeLocation(fields.scan, Data::bundle(wrong), feature, why));
  CHECK(feature.asBundle() == before);
}

TEST_CASE(
    "atomic feature make split nodes preserve payloads and reject cancellation without partial publication") {
  Registry registry;
  scene::registerProfileFeatureBundles(registry);
  const auto fit = line(source());
  test::OpCall call(registry);
  const auto fields = lineFields(fit);
  call.inputs = {{"profile", fields.profile},
                 {"line", fields.line},
                 {"lineContext", fields.lineContext},
                 {"innerEnd", fields.innerEnd},
                 {"pointContext", fields.pointContext},
                 {"inliers", fields.inliers},
                 {"weightedInliers", fields.weightedInliers},
                 {"quality", fields.quality}};
  REQUIRE_MESSAGE(call.run("gap.make_line_feature").ok, call.status.message);
  const auto feature = call.out("feature");
  call.inputs = {{"feature", feature}};
  REQUIRE(call.run("gap.split_line_feature").ok);
  CHECK(call.outputs.size() == 8);
  CHECK(call.out("profile").profilePtr() == fields.profile.profilePtr());
  const auto ref = reference(source(), "selected_point");
  call.inputs = {{"profile", ref.profile},
                 {"point", ref.point},
                 {"pointContext", ref.pointContext},
                 {"selection", ref.selection}};
  REQUIRE_MESSAGE(
      call.run("gap.make_reference_feature", {{"method", Value::text("selected_point")}}).ok,
      call.status.message);
  call.inputs = {{"feature", call.out("feature")}};
  REQUIRE(call.run("gap.split_reference_feature").ok);
  CHECK(call.outputs.size() == 7);
  const auto loc = location();
  call.inputs = {{"scan", loc.scan}, {"rois", loc.rois}};
  REQUIRE_MESSAGE(call.run("gap.make_location").ok, call.status.message);
  const auto locBundle = call.out("location");
  const auto choice = [](const char* selected) {
    return Data::record(Record{"FallbackChoice", {{"choice", selected}, {"reason", "test"}}});
  };
  call.inputs["scanChoice"] = choice("a");
  call.inputs["roiChoice"] = choice("a");
  call.inputs["seamLeftChoice"] = choice("b");
  CHECK(call.run("gap.make_location").code == "location_path_mismatch");
  CHECK(call.outputs.empty());
  call.inputs["seamLeftChoice"] = choice("a");
  REQUIRE(call.run("gap.make_location").ok);
  call.inputs.erase("roiChoice");
  CHECK_FALSE(call.run("gap.make_location").ok);
  CHECK(call.outputs.empty());
  CHECK_FALSE(call.validate("gap.make_location", {}, {"scan", "rois", "scanChoice"}).empty());
  call.inputs = {{"location", locBundle}};
  REQUIRE(call.run("gap.split_location").ok);
  CHECK(call.outputs.size() == 2);
  Cancel cancel;
  const std::unordered_map<std::string, Data> input{{"feature", feature}};
  std::unordered_map<std::string, Data> output{
      {"sentinel", Data::record(Record{"sentinel", Json::object()})}};
  Outputs outputs(output);
  const ParamMap parameters;
  CHECK(registry.find("gap.split_line_feature")
            ->compute(Inputs(input), ParamView(parameters, cancel.baseDir()), outputs, cancel)
            .code == "cancelled");
  CHECK(output.size() == 1);
}

TEST_CASE("real executor fallback selects a complete native line feature before splitting") {
  const auto fit = line(source("fallback-camera"));
  Data backup;
  std::string why;
  REQUIRE(scene::makeLineFeature(lineFields(fit), backup, why));
  Registry local;
  scene::registerProfileFeatureBundles(local);
  auto& global = ensureRegistry();
  const std::string failureId = "test.m4c.feature_failure", splitId = "test.m4c.feature_split";
  if (!global.find(failureId)) {
    OperatorDesc failure;
    failure.id = failureId;
    failure.version = "1.0.0";
    failure.label = "测试失败特征";
    failure.category = "test";
    failure.outputs = {
        {"feature", bundleTypeName(scene::kLineFeatureKind), "结果", "测试失败。", true}};
    failure.compute = [](const Inputs&, const ParamView&, Outputs&, ExecContext&) {
      return Status::Error(Phase::Execute, "no_result", "主特征不可用");
    };
    global.addOperator(std::move(failure));
    const auto* declaration = local.findBundle(scene::kLineFeatureKind);
    REQUIRE(declaration);
    global.addBundle(*declaration);
    auto split = *local.find("gap.split_line_feature");
    split.id = splitId;
    split.version = "1.0.0";
    global.addOperator(std::move(split));
  }
  const auto graph = test::makeGraph({{"primary", failureId},
                                      {"backup", "util.reroute"},
                                      {"fallback", "flow.fallback"},
                                      {"split", splitId}},
                                     {{"primary.feature", "fallback.a"},
                                      {"backup.out", "fallback.b"},
                                      {"fallback.out", "split.feature"}});
  test::Session session(graph, {}, {}, false, 1, true, {{"backup", "out", backup}});
  const auto& log = session.wait();
  REQUIRE_MESSAGE(log.runStatus() == "ok", Json(log.events).dump());
  CHECK(log.finalState("primary") == "error");
  CHECK(log.finalState("split") == "done");
  Data selectedProfile, selectedQuality, selectedLine;
  REQUIRE(exec::ResultStore::instance().get(session.runId(), "split", "profile", selectedProfile));
  REQUIRE(exec::ResultStore::instance().get(session.runId(), "split", "quality", selectedQuality));
  REQUIRE(exec::ResultStore::instance().get(session.runId(), "split", "line", selectedLine));
  CHECK(selectedProfile.profilePtr() == fit.profile.profilePtr());
  CHECK(selectedQuality.valueJson() == fit.quality.valueJson());
  CHECK(selectedLine.valueJson() == fit.line.valueJson());
}
