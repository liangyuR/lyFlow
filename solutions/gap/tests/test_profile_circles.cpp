#include <doctest/doctest.h>
#include <cmath>
#include <cstring>
#include <limits>

#include "helpers.h"
#include "lyflow/profile.h"
#include "lyflow_profile/provenance.h"
#include "ops/profile_circles.h"
#include "ops/profile_measurement_ops.h"

using namespace lyflow;
namespace scene = lyflow::packs::gap::profile_scene;
namespace {
using Json = nlohmann::json;
void arc(PointCloud& cloud, double cx, double cy, double radius, int count, float identity) {
  for (int i = 0; i < count; ++i) {
    const double angle = (200.0 + 140.0 * i / (count - 1)) * 3.14159265358979323846 / 180.0;
    cloud.push(static_cast<float>(cx + radius * std::cos(angle)),
               static_cast<float>(cy + radius * std::sin(angle)), 0);
    cloud.intensity.push_back(identity + static_cast<float>(cloud.pointCount()));
    cloud.normals.insert(cloud.normals.end(), {0, 0, 1});
    cloud.rgb.insert(cloud.rgb.end(), {static_cast<std::uint8_t>(identity / 1000), 70, 90});
  }
}
Data profile(int count, const char* id, float identity, double shift = 0, double rightX = .0035,
             const char* unit = "m", const char* plane = "XY") {
  PointCloud cloud;
  cloud.push(0, .03F, 0);
  cloud.intensity.push_back(identity);
  cloud.normals = {0, 0, 1};
  cloud.rgb = {static_cast<std::uint8_t>(identity / 1000), 70, 90};
  arc(cloud, -.0035, -.0005 + shift, .0015, count, identity);
  arc(cloud, rightX, -.0005 + shift, .0009, count, identity);
  std::vector<std::uint8_t> validity(cloud.pointCount(), 1);
  std::vector<std::uint32_t> indices;
  for (std::size_t i = 0; i < cloud.pointCount(); ++i) {
    if (std::string(plane) == "XZ")
      std::swap(cloud.xyz[3 * i + 1], cloud.xyz[3 * i + 2]);
    if (std::string(unit) == "mm")
      for (int axis = 0; axis < 3; ++axis)
        cloud.xyz[3 * i + axis] *= 1000;
    indices.push_back(static_cast<std::uint32_t>(3 * i + 1));
  }
  Profile result;
  std::string why;
  const auto sourceCount = cloud.pointCount() * 3 + 5;
  REQUIRE(makeProfile(std::make_shared<const PointCloud>(std::move(cloud)),
                      {1, "measurement", unit, plane, "reflected", "acquisition", id, sourceCount},
                      std::move(validity), std::move(indices), result, why));
  return Data::profile(std::move(result));
}
scene::CirclePairInputs fixture(int pCount = 40, int sCount = 60, double shift = 0,
                                bool sparse = false, bool rightMoved = false,
                                const char* unit = "m", const char* plane = "XY") {
  scene::CirclePairInputs inputs;
  inputs.primary = profile(pCount, "physical-A", 1000, 0, .0035, unit, plane);
  inputs.secondary =
      profile(sCount, "physical-B", 2000, shift, rightMoved ? .0045 : .0035, unit, plane);
  Profile merged = *inputs.secondary.asProfile();
  PointCloud cloud = *merged.cloud;
  PointCloud identity;
  cloud.id = identity.id;
  const auto* primary = inputs.primary.asProfile();
  cloud.xyz.insert(cloud.xyz.end(), primary->cloud->xyz.begin(), primary->cloud->xyz.end());
  cloud.intensity.insert(cloud.intensity.end(), primary->cloud->intensity.begin(),
                         primary->cloud->intensity.end());
  cloud.normals.insert(cloud.normals.end(), primary->cloud->normals.begin(),
                       primary->cloud->normals.end());
  cloud.rgb.insert(cloud.rgb.end(), primary->cloud->rgb.begin(), primary->cloud->rgb.end());
  merged.cloud = std::make_shared<const PointCloud>(std::move(cloud));
  merged.validity.insert(merged.validity.end(), primary->validity.begin(), primary->validity.end());
  for (auto index : primary->sourceIndices)
    merged.sourceIndices.push_back(
        static_cast<std::uint32_t>(inputs.secondary.asProfile()->descriptor.sourceCount + index));
  Record provenance;
  std::string why;
  REQUIRE_MESSAGE(profile_contract::makeCompositeProvenance(
                      inputs.secondary, inputs.primary, nullptr, nullptr, "secondary", "primary",
                      "require_equal", merged, provenance, why),
                  why);
  inputs.merged = Data::profile(std::move(merged));
  if (sparse) {
    const auto full = inputs.merged;
    Profile subset = *full.asProfile();
    const std::vector<std::int32_t> slots{1, 2, sCount + 1, sCount + 2};
    subset.cloud = std::make_shared<const PointCloud>(subset.cloud->select(slots));
    subset.validity.assign(slots.size(), 1);
    subset.sourceIndices.clear();
    for (auto slot : slots)
      subset.sourceIndices.push_back(full.asProfile()->sourceIndices[slot]);
    inputs.merged = Data::profile(std::move(subset));
    Record rebound;
    REQUIRE(profile_contract::rebindProvenance(full, inputs.merged, &provenance, rebound, why));
    provenance = std::move(rebound);
  }
  inputs.mergedProvenance = Data::record(std::move(provenance));
  for (int side = 0; side < 2; ++side) {
    Box2D box;
    box.min[0] = side ? .001F : -.006F;
    box.max[0] = side ? .006F : -.001F;
    box.min[1] = -.004F;
    box.max[1] = .002F;
    inputs.boxes[side] = Data::box2d(box);
    inputs.boxContexts[side] = Data::record(
        Record{"profile.Geometry.v2",
               profile_contract::geometryContextV2(inputs.merged, inputs.boxes[side])});
  }
  return inputs;
}
ParamMap defaults(const std::unordered_map<std::string, Value>& overrides = {}) {
  ParamMap values;
  for (const auto& parameter : scene::circlePairParams())
    values[parameter.name] = parameter.def;
  for (const auto& entry : overrides)
    values[entry.first] = entry.second;
  return values;
}
std::unordered_map<std::string, Data> cloudInputs(const scene::CirclePairInputs& inputs) {
  std::unordered_map<std::string, Data> result;
  for (const auto& entry :
       {std::make_pair("merged", &inputs.merged), std::make_pair("primary", &inputs.primary),
        std::make_pair("secondary", &inputs.secondary)}) {
    const auto* profile = entry.second->asProfile();
    Profile metric;
    std::string why;
    REQUIRE(convertProfileUnit(*profile, "m", metric, why));
    auto cloud = *metric.cloud;
    for (std::size_t i = 0; i < profile->pointCount(); ++i) {
      const auto& raw = metric.cloud->xyz;
      const auto a = profile->descriptor.plane == "YZ" ? 1 : 0;
      const auto b = profile->descriptor.plane == "XY" ? 1 : 2;
      const auto nan = std::numeric_limits<float>::quiet_NaN();
      cloud.xyz[3 * i] = profile->validity[i] ? raw[3 * i + a] : nan;
      cloud.xyz[3 * i + 1] = profile->validity[i] ? raw[3 * i + b] : nan;
      cloud.xyz[3 * i + 2] = profile->validity[i] ? 0 : nan;
    }
    result[entry.first] = Data::cloud(std::move(cloud));
  }
  result["boxLeft"] = inputs.boxes[0];
  result["boxRight"] = inputs.boxes[1];
  if (inputs.refLine.asLine2D())
    result["refLine"] = inputs.refLine;
  if (inputs.refLineRight.asLine2D())
    result["refLineRight"] = inputs.refLineRight;
  return result;
}
void sameBits(const Json& actual, const Json& expected) {
  if (expected.is_object()) {
    for (const auto& field : expected.items()) {
      REQUIRE(actual.contains(field.key()));
      sameBits(actual.at(field.key()), field.value());
    }
  } else if (expected.is_number_float()) {
    REQUIRE(actual.is_number_float());
    const double a = actual.get<double>(), b = expected.get<double>();
    CHECK(std::memcmp(&a, &b, sizeof(double)) == 0);
  } else
    CHECK(actual == expected);
}
void checkOutput(const scene::CirclePairInputs& inputs, const scene::CirclePairResult& result) {
  for (std::size_t side = 0; side < 2; ++side) {
    const char* role = side ? "right" : "left";
    const auto& quality = result.quality.data[role];
    const auto selected = quality["selectedInput"].get<std::string>();
    const auto& parent = selected == "merged"    ? inputs.merged
                         : selected == "primary" ? inputs.primary
                                                 : inputs.secondary;
    const auto* native = result.profiles[side].asProfile();
    REQUIRE(native);
    CHECK(native->descriptor.sourceCount == parent.asProfile()->descriptor.sourceCount);
    CHECK(native->descriptor.sourceId == parent.asProfile()->descriptor.sourceId);
    CHECK(native->cloud->hasRgb());
    CHECK(native->cloud->hasIntensity());
    CHECK(native->cloud->hasNormals());
    const auto* indices = result.inliers[side].asIndices();
    REQUIRE(indices);
    CHECK(indices->sourceCloudId == native->cloud->id);
    for (auto index : indices->values)
      CHECK((index >= 0 && static_cast<std::size_t>(index) < native->pointCount()));
    for (std::size_t slot = 0; slot < native->pointCount(); ++slot) {
      const auto found =
          std::find(parent.asProfile()->sourceIndices.begin(),
                    parent.asProfile()->sourceIndices.end(), native->sourceIndices[slot]);
      REQUIRE(found != parent.asProfile()->sourceIndices.end());
      const auto sourceSlot =
          static_cast<std::size_t>(found - parent.asProfile()->sourceIndices.begin());
      CHECK(native->cloud->intensity[slot] == parent.asProfile()->cloud->intensity[sourceSlot]);
      for (int axis = 0; axis < 3; ++axis) {
        CHECK(native->cloud->xyz[3 * slot + axis] ==
              parent.asProfile()->cloud->xyz[3 * sourceSlot + axis]);
        CHECK(native->cloud->rgb[3 * slot + axis] ==
              parent.asProfile()->cloud->rgb[3 * sourceSlot + axis]);
        CHECK(native->cloud->normals[3 * slot + axis] ==
              parent.asProfile()->cloud->normals[3 * sourceSlot + axis]);
      }
    }
    Json sources;
    std::string why;
    CHECK(profile_contract::profileSources(result.profiles[side],
                                           result.provenances[side].asRecord(), sources, why));
    ProfileDescriptor descriptor;
    CHECK(profile_contract::geometryMatchesPhysical(
        result.circles[side], result.contexts[side].asRecord(), descriptor, why));
  }
}
struct Poll final : ExecContext {
  mutable int calls = 0;
  int stop = std::numeric_limits<int>::max();
  std::filesystem::path base;
  bool cancelled() const override { return ++calls >= stop; }
  void progress(float, std::string_view) override {}
  void log(LogLevel, std::string) override {}
  const std::filesystem::path& baseDir() const override { return base; }
  int threadBudget() const override { return 1; }
};
}
TEST_CASE(
    "Native scene circle pair preserves candidate branches numeric bits and actual ROI source slots") {
  struct Case {
    const char* name;
    int primary;
    int secondary;
    double shift;
    bool sparse;
    bool moved;
    std::unordered_map<std::string, Value> params;
    const char* left;
    const char* right;
  };
  const std::vector<Case> cases{
      {"merged", 40, 60, 0, false, false, {}, "merged", "merged"},
      {"primary",
       40,
       60,
       .001,
       false,
       false,
       {{"leftCamera", Value::text("Primary")}, {"rightCamera", Value::text("Primary")}},
       "primary",
       "primary"},
      {"secondary",
       40,
       60,
       .001,
       false,
       false,
       {{"leftCamera", Value::text("Secondary")}, {"rightCamera", Value::text("Secondary")}},
       "secondary",
       "secondary"},
      {"weak-merged",
       20,
       80,
       0,
       false,
       false,
       {{"leftCamera", Value::text("Primary")},
        {"leftMinInliers", Value::integer(50)},
        {"rightCamera", Value::text("Secondary")}},
       "merged",
       "secondary"},
      {"fallback-primary",
       40,
       60,
       .001,
       true,
       false,
       {{"selectClosestNominal", Value::boolean(false)}, {"preferredCamera", Value::text("Left")}},
       "primary",
       "primary"},
      {"fallback-secondary",
       40,
       60,
       .001,
       true,
       false,
       {{"selectClosestNominal", Value::boolean(false)}, {"preferredCamera", Value::text("Right")}},
       "secondary",
       "secondary"},
      {"fallback-consistent",
       40,
       60,
       0,
       true,
       false,
       {{"selectClosestNominal", Value::boolean(false)}},
       "secondary",
       "secondary"},
      {"closest-nominal",
       40,
       40,
       0,
       true,
       true,
       {{"nominal", Value::number(5.6)}},
       "primary",
       "secondary"},
      {"fixed",
       40,
       60,
       0,
       false,
       false,
       {{"leftRadiusFixed", Value::boolean(true)},
        {"leftRadiusValue", Value::number(1.5)},
        {"rightRadiusFixed", Value::boolean(true)},
        {"rightRadiusValue", Value::number(.9)}},
       "merged",
       "merged"},
      {"prior",
       40,
       60,
       0,
       false,
       false,
       {{"leftRadiusPrior", Value::number(1.5)}, {"leftRadiusPriorSigma", Value::number(.2)}},
       "merged",
       "merged"},
      {"retry",
       40,
       60,
       0,
       false,
       false,
       {{"distThresh", Value::number(1.401298464324817e-42)},
        {"retryDistance", Value::number(.03)}},
       "merged",
       "merged"}};
  for (const auto& item : cases) {
    CAPTURE(item.name);
    auto inputs = fixture(item.primary, item.secondary, item.shift, item.sparse, item.moved);
    const auto before = profile_contract::context(inputs.merged);
    auto parameters = defaults(item.params);
    test::StubContext ctx;
    scene::CirclePairResult result;
    REQUIRE_MESSAGE(
        scene::fitProfileGapCircles(inputs, ParamView(parameters, ctx.baseDir()), result, ctx).ok,
        item.name);
    CHECK(result.quality.data["left"]["selectedInput"] == item.left);
    CHECK(result.quality.data["right"]["selectedInput"] == item.right);
    checkOutput(inputs, result);
    const auto clouds = cloudInputs(inputs);
    std::unordered_map<std::string, Data> legacy;
    Outputs out(legacy);
    REQUIRE(scene::detail::fitCirclePairClouds(Inputs(clouds), ParamView(parameters, ctx.baseDir()),
                                               out, ctx)
                .ok);
    sameBits(result.quality.data, legacy.at("quality").asRecord()->data);
    for (std::size_t side = 0; side < 2; ++side) {
      const auto* a = result.circles[side].asCircle2D();
      const auto* b = legacy.at(side ? "right" : "left").asCircle2D();
      CHECK(std::memcmp(a->center, b->center, sizeof(a->center)) == 0);
      CHECK(std::memcmp(&a->radius, &b->radius, sizeof(float)) == 0);
    }
    CHECK(profile_contract::context(inputs.merged) == before);
  }
}
TEST_CASE("Native scene circle pair carries masks metric views and constraint reference contexts") {
  for (const char* unit : {"m", "mm"})
    for (const char* plane : {"XY", "XZ"}) {
      auto inputs = fixture(40, 60, 0, false, false, unit, plane);
      auto primary = *inputs.primary.asProfile();
      auto cloud = *primary.cloud;
      cloud.xyz[3] = cloud.xyz[4] = cloud.xyz[5] = std::numeric_limits<float>::quiet_NaN();
      primary.cloud = std::make_shared<const PointCloud>(std::move(cloud));
      primary.validity[1] = 0;
      primary.validity[2] = 0;
      inputs.primary = Data::profile(std::move(primary));
      Line2D line;
      inputs.refLine = Data::line2d(line);
      inputs.refLineContext =
          Data::record(Record{"profile.Geometry.v2",
                              profile_contract::geometryContextV2(inputs.merged, inputs.refLine)});
      auto parameters = defaults({{"leftCamera", Value::text("Primary")},
                                  {"leftCenterAbove", Value::number(.5)},
                                  {"leftCenterTol", Value::number(.3)},
                                  {"leftCenterMode", Value::text("guard")},
                                  {"rightTangentAbove", Value::number(1.4)},
                                  {"rightTangentTol", Value::number(.2)},
                                  {"rightCenterMode", Value::text("always")}});
      test::StubContext ctx;
      scene::CirclePairResult result;
      REQUIRE_MESSAGE(
          scene::fitProfileGapCircles(inputs, ParamView(parameters, ctx.baseDir()), result, ctx).ok,
          unit);
      checkOutput(inputs, result);
      CHECK(result.profiles[0].asProfile()->descriptor.unit == unit);
      for (auto sourceIndex : result.profiles[0].asProfile()->sourceIndices) {
        CHECK(sourceIndex != inputs.primary.asProfile()->sourceIndices[1]);
        CHECK(sourceIndex != inputs.primary.asProfile()->sourceIndices[2]);
      }
      CHECK(result.quality.data["right"]["model"] == "circle-tangent-band");
    }
}
TEST_CASE(
    "Native scene circle pair rejects incomplete contexts and failed fits without changing outputs") {
  auto inputs = fixture();
  test::StubContext ctx;
  auto parameters = defaults();
  scene::CirclePairResult result;
  result.quality.type = "sentinel";
  auto wrong = inputs;
  wrong.boxContexts[0] = Data{};
  CHECK_FALSE(
      scene::fitProfileGapCircles(wrong, ParamView(parameters, ctx.baseDir()), result, ctx).ok);
  CHECK(result.quality.type == "sentinel");
  wrong = inputs;
  auto forged = *wrong.mergedProvenance.asRecord();
  forged.data["profileContext"]["profileHash"] = std::string(32, '0');
  wrong.mergedProvenance = Data::record(forged);
  CHECK_FALSE(
      scene::fitProfileGapCircles(wrong, ParamView(parameters, ctx.baseDir()), result, ctx).ok);
  wrong = inputs;
  wrong.refLine = Data::line2d(Line2D{});
  CHECK_FALSE(
      scene::fitProfileGapCircles(wrong, ParamView(parameters, ctx.baseDir()), result, ctx).ok);
  parameters["distThresh"] = Value::number(1.401298464324817e-42);
  parameters["cameraFallback"] = Value::boolean(false);
  CHECK_FALSE(
      scene::fitProfileGapCircles(inputs, ParamView(parameters, ctx.baseDir()), result, ctx).ok);
  CHECK(result.quality.type == "sentinel");
}
TEST_CASE("Native scene circle pair and shared legacy helper publish nothing on cancellation") {
  const auto inputs = fixture();
  auto parameters = defaults();
  scene::CirclePairResult result;
  Poll completed;
  REQUIRE(
      scene::fitProfileGapCircles(inputs, ParamView(parameters, completed.base), result, completed)
          .ok);
  result.quality.type = "sentinel";
  Poll stopped;
  stopped.stop = completed.calls;
  CHECK_FALSE(
      scene::fitProfileGapCircles(inputs, ParamView(parameters, stopped.base), result, stopped).ok);
  CHECK(result.quality.type == "sentinel");
  const auto clouds = cloudInputs(inputs);
  std::unordered_map<std::string, Data> values;
  Outputs out(values);
  stopped.calls = 0;
  stopped.stop = 1;
  CHECK_FALSE(scene::detail::fitCirclePairClouds(Inputs(clouds),
                                                 ParamView(parameters, stopped.base), out, stopped)
                  .ok);
  CHECK(values.empty());
}

TEST_CASE("Native scene circle registration exposes complete typed profiles contexts and lineage") {
  const auto inputs = fixture();
  Registry registry;
  scene::registerProfileGapCircles(registry);
  test::OpCall call(registry);
  call.inputs = {{"merged", inputs.merged},       {"primary", inputs.primary},
                 {"secondary", inputs.secondary}, {"mergedProvenance", inputs.mergedProvenance},
                 {"boxLeft", inputs.boxes[0]},    {"boxLeftContext", inputs.boxContexts[0]},
                 {"boxRight", inputs.boxes[1]},   {"boxRightContext", inputs.boxContexts[1]}};
  REQUIRE_MESSAGE(call.run("gap.fit_gap_circles").ok, call.status.message);
  CHECK(call.outputs.size() == 11);
  REQUIRE(call.out("leftProfile").asProfile());
  REQUIRE(call.out("rightProfile").asProfile());
  CHECK(call.out("quality").asRecord()->type == "gap.CircleFitPair.v2");
  CHECK(call.out("leftInliers").asIndices()->sourceCloudId ==
        call.out("leftProfile").asProfile()->cloud->id);
  CHECK(call.out("rightInliers").asIndices()->sourceCloudId ==
        call.out("rightProfile").asProfile()->cloud->id);
}

namespace circles_legacy {

Data profileCloud(const PointCloud& input, const char* id) {
  Profile value;
  std::string why;
  std::vector<std::uint8_t> validity(input.pointCount(), 1);
  std::vector<std::uint32_t> indices;
  for (std::size_t i = 0; i < input.pointCount(); ++i)
    indices.push_back(static_cast<std::uint32_t>(i));
  REQUIRE(
      makeProfile(std::make_shared<const PointCloud>(input),
                  {1, "measurement", "m", "XY", "reflected", "acquisition", id, input.pointCount()},
                  std::move(validity), std::move(indices), value, why));
  return Data::profile(std::move(value));
}
struct Call {
  std::unordered_map<std::string, Data> inputs, outputs;
  Status run(const std::string&, const ParamMap& overrides = {}) {
    outputs.clear();
    scene::CirclePairInputs input;
    input.merged = profileCloud(*inputs.at("merged").asCloud(), "explicit-merged-fixture");
    input.primary = profileCloud(*inputs.at("primary").asCloud(), "explicit-primary-fixture");
    input.secondary = profileCloud(*inputs.at("secondary").asCloud(), "explicit-secondary-fixture");
    Record provenance;
    std::string why;
    REQUIRE(
        profile_contract::rebindProvenance(input.merged, input.merged, nullptr, provenance, why));
    input.mergedProvenance = Data::record(provenance);
    auto bound = [&](const Data& geometry) {
      return Data::record(Record{"profile.Geometry.v2",
                                 profile_contract::geometryContextV2(input.merged, geometry)});
    };
    input.boxes = {inputs.at("boxLeft"), inputs.at("boxRight")};
    input.boxContexts = {bound(input.boxes[0]), bound(input.boxes[1])};
    if (inputs.count("refLine")) {
      input.refLine = inputs.at("refLine");
      input.refLineContext = bound(input.refLine);
    }
    if (inputs.count("refLineRight")) {
      input.refLineRight = inputs.at("refLineRight");
      input.refLineRightContext = bound(input.refLineRight);
    }
    const auto parameters = defaults(overrides);
    test::StubContext ctx;
    scene::CirclePairResult result;
    const auto status =
        scene::fitProfileGapCircles(input, ParamView(parameters, ctx.baseDir()), result, ctx);
    if (status.ok)
      outputs = {{"left", result.circles[0]},
                 {"right", result.circles[1]},
                 {"quality", Data::record(result.quality)}};
    return status;
  }
  const Data& out(const std::string& name) { return outputs.at(name); }
  std::vector<Issue> validate(const std::string& id, const ParamMap& overrides,
                              std::set<std::string> connected) {
    Registry registry;
    scene::registerProfileGapCircles(registry);
    const auto parameters = defaults(overrides);
    test::StubContext ctx;
    for (const auto* field : {"mergedProvenance", "boxLeftContext", "boxRightContext"})
      connected.insert(field);
    if (connected.count("refLine"))
      connected.insert("refLineContext");
    if (connected.count("refLineRight"))
      connected.insert("refLineRightContext");
    const auto* op = registry.find(id);
    REQUIRE(op);
    REQUIRE(op->validate != nullptr);
    return op->validate(ParamView(parameters, ctx.baseDir()), connected);
  }
};
Box2D box(float a, float b, float c, float d) { return Box2D{{a, b}, {c, d}}; }
bool hasError(const std::vector<Issue>& issues, const std::string& code, const std::string& param) {
  for (const auto& issue : issues)
    if (issue.severity == Severity::Error && issue.status.code == code &&
        issue.status.paramPath == param)
      return true;
  return false;
}

void pushArc(PointCloud& c, double cx, double cy, double r, double from, double to, std::size_t n) {
  for (std::size_t i = 0; i < n; ++i) {
    const double t = (from + (to - from) * static_cast<double>(i) / static_cast<double>(n - 1)) *
                     3.14159265358979323846 / 180.0;
    c.push(static_cast<float>(cx + r * std::cos(t)), static_cast<float>(cy + r * std::sin(t)),
           0.0f);
  }
}
void pushLeftArc(PointCloud& c) { pushArc(c, -0.0035, -0.0005, 0.0015, 200, 340, 60); }
Line2D flatLine() {
  Line2D l;
  l.dir[0] = 1.0f;
  return l; // 过原点、水平
}
Line2D flatLineAt(double y) {
  Line2D l;
  l.dir[0] = 1.0f;
  l.point[1] = static_cast<float>(y);
  return l;
}
double rightCenterY(Call& call) { return call.out("right").asCircle2D()->center[1]; }
TEST_CASE(
    "Native circle candidates retain published independent reference bands radius and fallback fixtures") {
  SUBCASE("legacy:1221 gap.fit_gap_circles 的 rightCamera 把右圆钉到指定那台相机") {

    const auto build = [](const char* camera) {
      PointCloud primary, secondary, merged;
      pushLeftArc(primary);
      pushLeftArc(secondary);
      pushLeftArc(merged);
      pushArc(primary, 0.0035, -0.0005, 0.0009, 200, 340, 60);
      pushArc(secondary, 0.0035, -0.0015, 0.0009, 200, 340, 60);
      pushArc(merged, 0.0035, -0.0005, 0.0009, 200, 340, 60);
      pushArc(merged, 0.0035, -0.0015, 0.0009, 200, 340, 60);
      auto call = std::make_unique<Call>();
      call->inputs["merged"] = Data::cloud(merged);
      call->inputs["primary"] = Data::cloud(primary);
      call->inputs["secondary"] = Data::cloud(secondary);
      call->inputs["boxLeft"] = Data::box2d(box(-0.006f, -0.004f, -0.001f, 0.002f));
      call->inputs["boxRight"] = Data::box2d(box(0.001f, -0.004f, 0.006f, 0.002f));
      REQUIRE(call->run("gap.fit_gap_circles", {{"rightCamera", Value::text(camera)}}).ok);
      return call;
    };
    auto master = build("Primary");
    auto slave = build("Secondary");
    CHECK(rightCenterY(*master) == doctest::Approx(-0.0005).epsilon(0.02));
    CHECK(rightCenterY(*slave) == doctest::Approx(-0.0015).epsilon(0.02));
  }
  SUBCASE(
      "legacy:1247 gap.fit_gap_circles 的圆心高度带把圆心按到参考线上方，refLineRight 让右侧比另一条参考线") {

    const auto build = [](bool withRight) {
      PointCloud primary, secondary, merged;
      pushLeftArc(primary);
      pushLeftArc(secondary);
      pushLeftArc(merged);
      pushArc(merged, 0.0035, -0.0005, 0.0009, 200, 340, 40);
      pushArc(merged, 0.0035, 0.0005, 0.0009, 200, 340, 90);
      pushArc(primary, 0.0035, -0.0005, 0.0009, 200, 340, 40);
      pushArc(secondary, 0.0035, 0.0005, 0.0009, 200, 340, 90);
      auto call = std::make_unique<Call>();
      call->inputs["merged"] = Data::cloud(merged);
      call->inputs["primary"] = Data::cloud(primary);
      call->inputs["secondary"] = Data::cloud(secondary);
      call->inputs["boxLeft"] = Data::box2d(box(-0.006f, -0.004f, -0.001f, 0.002f));
      call->inputs["boxRight"] = Data::box2d(box(0.001f, -0.004f, 0.006f, 0.002f));
      call->inputs["refLine"] = Data::line2d(flatLine());
      if (withRight)
        call->inputs["refLineRight"] = Data::line2d(flatLineAt(0.001));
      return call;
    };

    auto loose = build(false);
    REQUIRE(loose->run("gap.fit_gap_circles").ok);
    CHECK(rightCenterY(*loose) == doctest::Approx(0.0005).epsilon(0.05));

    auto split = build(true);
    REQUIRE(split
                ->run("gap.fit_gap_circles", {{"rightCenterAbove", Value::number(1.5)},
                                              {"rightCenterTol", Value::number(0.3)}})
                .ok);
    CHECK(rightCenterY(*split) == doctest::Approx(-0.0005).epsilon(0.05));

    auto shared = build(false);
    REQUIRE(shared
                ->run("gap.fit_gap_circles", {{"rightCenterAbove", Value::number(0.5)},
                                              {"rightCenterTol", Value::number(0.3)}})
                .ok);
    CHECK(rightCenterY(*shared) == doctest::Approx(-0.0005).epsilon(0.05));

    auto leftBand = build(true);
    REQUIRE(leftBand
                ->run("gap.fit_gap_circles", {{"leftCenterAbove", Value::number(0.5)},
                                              {"leftCenterTol", Value::number(0.3)}})
                .ok);
    CHECK(leftBand->out("left").asCircle2D()->center[1] == doctest::Approx(-0.0005).epsilon(0.05));
  }
  SUBCASE("legacy:1303 gap.fit_gap_circles 的左圆：细化越界钉在界上、先验，quality 都说得出来") {

    const auto build = [] {
      PointCloud merged;
      for (int i = 0; i < 70; ++i) {
        const double t = (250.0 + 60.0 * i / 69.0) * 3.14159265358979323846 / 180.0;
        const double r = 0.0015 + ((i * 7) % 5 - 2) * 0.000006;
        merged.push(static_cast<float>(-0.0035 + r * std::cos(t)),
                    static_cast<float>(-0.0005 + r * std::sin(t)), 0.0f);
      }
      pushArc(merged, 0.0035, -0.0005, 0.0009, 200, 340, 60);
      auto call = std::make_unique<Call>();
      call->inputs["merged"] = Data::cloud(merged);
      call->inputs["primary"] = Data::cloud(merged);
      call->inputs["secondary"] = Data::cloud(merged);
      call->inputs["boxLeft"] = Data::box2d(box(-0.006f, -0.004f, -0.001f, 0.002f));
      call->inputs["boxRight"] = Data::box2d(box(0.001f, -0.004f, 0.006f, 0.002f));
      return call;
    };
    const auto left = [](Call& c) { return c.out("quality").asRecord()->data["left"]; };

    SUBCASE("上界卡在真半径下面：钉在 1.3，不失败，记下钉之前的半径") {
      auto call = build();
      const Status s =
          call->run("gap.fit_gap_circles", {{"leftRadiusMax", Value::number(1.3)},
                                            {"cameraFallback", Value::boolean(false)}});
      CAPTURE(s.message);
      REQUIRE(s.ok);
      CHECK(call->out("left").asCircle2D()->radius == doctest::Approx(0.0013));
      CHECK(left(*call)["radiusMode"] == "clamped");
      CHECK(left(*call)["radiusBeforeClampMm"].get<double>() > 1.3);
    }
    SUBCASE("先验：radiusMode 记成 prior，没有钉之前的半径") {
      auto call = build();
      REQUIRE(call->run("gap.fit_gap_circles", {{"leftRadiusMax", Value::number(4.0)},
                                                {"leftRadiusPrior", Value::number(1.5)},
                                                {"leftRadiusPriorSigma", Value::number(0.2)}})
                  .ok);
      CHECK(left(*call)["radiusMode"] == "prior");
      CHECK_FALSE(left(*call).contains("radiusBeforeClampMm"));
      CHECK(call->out("left").asCircle2D()->radius == doctest::Approx(0.0015).epsilon(0.05));
    }
  }
  SUBCASE(
      "legacy:1346 gap.fit_gap_circles 的相切带：圆顶压在面线上的拐角圆，压过缝里点更多的深处那段弧") {

    const auto build = [] {
      PointCloud merged;
      pushArc(merged, 0.0, 0.0016, 0.0016, 270, 360, 40);
      pushArc(merged, 0.0005, 0.0025, 0.0010, 200, 340, 70);
      pushArc(merged, 0.0040, -0.0005, 0.0009, 200, 340, 60);
      auto call = std::make_unique<Call>();
      call->inputs["merged"] = Data::cloud(merged);
      call->inputs["primary"] = Data::cloud(merged);
      call->inputs["secondary"] = Data::cloud(merged);
      call->inputs["boxLeft"] = Data::box2d(box(-0.003f, -0.001f, 0.0022f, 0.004f));
      call->inputs["boxRight"] = Data::box2d(box(0.0028f, -0.004f, 0.006f, 0.002f));
      call->inputs["refLine"] = Data::line2d(flatLine());
      return call;
    };
    const auto leftR = [](Call& c) { return c.out("left").asCircle2D()->radius; };

    auto loose = build();
    REQUIRE(loose->run("gap.fit_gap_circles", {{"leftRadiusMax", Value::number(2.0)}}).ok);
    CHECK(leftR(*loose) == doctest::Approx(0.0010).epsilon(0.05));
    const auto looseQ = loose->out("quality").asRecord()->data["left"];
    CHECK(looseQ["topAboveRefMm"].get<double>() == doctest::Approx(-1.5).epsilon(0.05));
    CHECK(looseQ["centerAboveRefMm"].get<double>() == doctest::Approx(-2.5).epsilon(0.05));

    for (const char* mode : {"always", "guard"}) {
      CAPTURE(mode);
      auto tangent = build();
      REQUIRE(tangent
                  ->run("gap.fit_gap_circles", {{"leftRadiusMax", Value::number(2.0)},
                                                {"leftTangentAbove", Value::number(0.0)},
                                                {"leftTangentTol", Value::number(0.25)},
                                                {"leftCenterMode", Value::text(mode)}})
                  .ok);
      CHECK(leftR(*tangent) == doctest::Approx(0.0016).epsilon(0.05));
      const auto q = tangent->out("quality").asRecord()->data["left"];
      CHECK(std::fabs(q["topAboveRefMm"].get<double>()) < 0.05);
      CHECK(q["model"] == "circle-tangent-band");
    }
  }
  SUBCASE("legacy:1455 gap.fit_gap_circles 的固定半径在圆心高度带那条路上也生效") {

    PointCloud cloud;
    pushLeftArc(cloud);
    pushArc(cloud, 0.0035, -0.0005, 0.0009, 200, 340, 60);
    Call call;
    call.inputs["merged"] = Data::cloud(cloud);
    call.inputs["primary"] = Data::cloud(cloud);
    call.inputs["secondary"] = Data::cloud(cloud);
    call.inputs["boxLeft"] = Data::box2d(box(-0.006f, -0.004f, -0.001f, 0.002f));
    call.inputs["boxRight"] = Data::box2d(box(0.001f, -0.004f, 0.006f, 0.002f));
    call.inputs["refLine"] = Data::line2d(flatLine());
    REQUIRE(call.run("gap.fit_gap_circles", {{"rightRadiusFixed", Value::boolean(true)},
                                             {"rightRadiusValue", Value::number(1.0)},
                                             {"rightCenterAbove", Value::number(0.5)},
                                             {"rightCenterTol", Value::number(0.4)},
                                             {"rightCenterMode", Value::text("always")},
                                             {"distThresh", Value::number(0.2)}})
                .ok);
    const Circle2D& right = *call.out("right").asCircle2D();
    CHECK(right.radius == doctest::Approx(0.001).epsilon(1e-6));
    CHECK(call.out("quality").asRecord()->data["right"]["model"] == "circle-center-band");
    CHECK(-right.center[1] == doctest::Approx(0.0005).epsilon(0.8));
  }
  SUBCASE("legacy:1481 gap.fit_gap_circles 的固定半径在相机分开的回退里也生效") {

    PointCloud primary, secondary, merged;
    pushLeftArc(primary);
    pushLeftArc(secondary);
    pushLeftArc(merged);
    pushArc(primary, 0.0035, -0.0005, 0.0009, 200, 340, 60);
    pushArc(secondary, 0.0035, -0.0005, 0.0009, 200, 340, 60);
    Call call;
    call.inputs["merged"] = Data::cloud(merged); // 右侧没有点：合并云这条路必然失败
    call.inputs["primary"] = Data::cloud(primary);
    call.inputs["secondary"] = Data::cloud(secondary);
    call.inputs["boxLeft"] = Data::box2d(box(-0.006f, -0.004f, -0.001f, 0.002f));
    call.inputs["boxRight"] = Data::box2d(box(0.001f, -0.004f, 0.006f, 0.002f));
    PointCloud stray = merged;
    stray.push(0.0035f, 0.0f, 0.0f);
    call.inputs["merged"] = Data::cloud(stray);
    REQUIRE(call.run("gap.fit_gap_circles", {{"rightRadiusFixed", Value::boolean(true)},
                                             {"rightRadiusValue", Value::number(1.0)},
                                             {"selectClosestNominal", Value::boolean(true)},
                                             {"distThresh", Value::number(0.2)}})
                .ok);
    const Circle2D& right = *call.out("right").asCircle2D();
    CHECK(right.radius == doctest::Approx(0.001).epsilon(1e-6));
    const std::string model = call.out("quality").asRecord()->data["right"]["model"];
    CHECK(model.rfind("camera-separated-circle-", 0) == 0);
  }
}
TEST_CASE("Native circle guards bearing bands and weak floors retain published boundary fixtures") {
  SUBCASE("native fine and scene hooks both reject unpaired constraint contexts") {
    Registry registry;
    scene::registerProfileGapCircles(registry);
    scene::registerProfileMeasurementOps(registry);
    const auto values = defaults();
    test::StubContext ctx;
    for (const auto* id : {"gap.fit_gap_circles", "gap.seam_circles"}) {
      const auto* op = registry.find(id);
      REQUIRE(op);
      REQUIRE(op->validate != nullptr);
      const auto diagnostics = op->validate(ParamView(values, ctx.baseDir()), {"refLine"});
      CHECK(std::any_of(diagnostics.begin(), diagnostics.end(), [](const Issue& issue) {
        return issue.severity == Severity::Error && issue.status.code == "bad_input" &&
               issue.status.portName == "refLineContext";
      }));
      CHECK(op->validate(ParamView(values, ctx.baseDir()), {"refLine", "refLineContext"}).empty());
    }
  }
  SUBCASE("legacy:1390 gap.fit_gap_circles 的参数与连线检查在 validate 里") {

    const std::set<std::string> plain = {"merged", "primary", "secondary", "boxLeft", "boxRight"};
    Call call;
    CHECK(hasError(
        call.validate("gap.fit_gap_circles", {{"rightCenterTol", Value::number(0.3)}}, plain),
        "bad_param", "rightCenterTol"));
    std::set<std::string> withRight = plain;
    withRight.insert("refLineRight");
    CHECK(call.validate("gap.fit_gap_circles", {{"rightCenterTol", Value::number(0.3)}}, withRight)
              .empty());
    CHECK(hasError(
        call.validate("gap.fit_gap_circles", {{"leftCenterTol", Value::number(0.3)}}, withRight),
        "bad_param", "leftCenterTol"));
    CHECK(hasError(call.validate("gap.fit_gap_circles",
                                 {{"leftRadiusMin", Value::number(2.0)},
                                  {"leftRadiusMax", Value::number(1.0)}},
                                 plain),
                   "bad_param", "leftRadiusMax"));
    CHECK(hasError(call.validate("gap.fit_gap_circles",
                                 {{"rightRadiusFixed", Value::boolean(true)},
                                  {"rightRadiusValue", Value::number(5.0)}},
                                 plain),
                   "bad_param", "rightRadiusValue"));
    CHECK(hasError(
        call.validate("gap.fit_gap_circles", {{"leftTangentTol", Value::number(0.25)}}, plain),
        "bad_param", "leftTangentTol"));
    CHECK(hasError(
        call.validate("gap.fit_gap_circles", {{"rightTangentTol", Value::number(0.25)}}, plain),
        "bad_param", "rightTangentTol"));
    CHECK(hasError(call.validate("gap.fit_gap_circles",
                                 {{"leftRadiusPrior", Value::number(3.0)},
                                  {"leftRadiusPriorSigma", Value::number(0.2)}},
                                 plain),
                   "bad_param", "leftRadiusPrior"));
    CHECK(call.validate("gap.fit_gap_circles", {{"leftRadiusPrior", Value::number(3.0)}}, plain)
              .empty());
    std::set<std::string> withRef = plain;
    withRef.insert("refLine");
    const auto priorWith = [&](const char* mode) {
      return call.validate("gap.fit_gap_circles",
                           {{"leftRadiusPrior", Value::number(1.0)},
                            {"leftRadiusPriorSigma", Value::number(0.1)},
                            {"leftCenterTol", Value::number(0.3)},
                            {"leftCenterMode", Value::text(mode)}},
                           withRef);
    };
    const auto warned = [](const std::vector<Issue>& issues) {
      return std::any_of(issues.begin(), issues.end(), [](const Issue& i) {
        return i.severity == Severity::Warning && i.status.paramPath == "leftRadiusPriorSigma";
      });
    };
    CHECK(warned(priorWith("always")));
    CHECK_FALSE(warned(priorWith("guard")));
    CHECK(call.validate("gap.fit_gap_circles", {}, plain).empty());
  }
  SUBCASE("legacy:1634 圆心高度带的 guard：带内结果不变，出带才重拟") {

    const auto build = [](const std::unordered_map<std::string, Value>& over) {
      PointCloud primary, secondary, merged;
      pushLeftArc(primary);
      pushLeftArc(secondary);
      pushLeftArc(merged);
      for (PointCloud* c : {&primary, &secondary, &merged}) {
        pushArc(*c, 0.0035, -0.0005, 0.0009, 200, 340, 90);
        pushArc(*c, 0.0035, 0.0005, 0.0009, 200, 340, 40);
      }
      auto call = std::make_unique<Call>();
      call->inputs["merged"] = Data::cloud(merged);
      call->inputs["primary"] = Data::cloud(primary);
      call->inputs["secondary"] = Data::cloud(secondary);
      call->inputs["boxLeft"] = Data::box2d(box(-0.006f, -0.004f, -0.001f, 0.002f));
      call->inputs["boxRight"] = Data::box2d(box(0.001f, -0.004f, 0.006f, 0.002f));
      call->inputs["refLine"] = Data::line2d(flatLine());
      REQUIRE(call->run("gap.fit_gap_circles", over).ok);
      return call;
    };
    auto plain = build({});
    const Circle2D& a = *plain->out("right").asCircle2D();
    REQUIRE(a.center[1] == doctest::Approx(-0.0005).epsilon(0.05));

    auto guarded = build({{"rightCenterAbove", Value::number(0.5)},
                          {"rightCenterTol", Value::number(0.4)},
                          {"rightCenterMode", Value::text("guard")}});
    const Circle2D& b = *guarded->out("right").asCircle2D();
    CHECK(a.center[0] == b.center[0]);
    CHECK(a.center[1] == b.center[1]);
    CHECK(a.radius == b.radius);

    auto moved = build({{"rightCenterAbove", Value::number(-0.5)},
                        {"rightCenterTol", Value::number(0.3)},
                        {"rightCenterMode", Value::text("guard")}});
    CHECK(moved->out("right").asCircle2D()->center[1] == doctest::Approx(0.0005).epsilon(0.05));
  }
  SUBCASE("legacy:1676 圆拟合的弱地板：钉死的那台弧太短就退回合并云") {

    const auto build = [] {
      PointCloud primary, secondary, merged;
      pushLeftArc(primary);
      pushLeftArc(secondary);
      pushLeftArc(merged);
      pushArc(primary, 0.0035, -0.0005, 0.0009, 200, 340, 60);
      pushArc(secondary, 0.0035, -0.0005, 0.0009, 330, 340, 6); // 只有 10°
      pushArc(merged, 0.0035, -0.0005, 0.0009, 200, 340, 60);
      auto call = std::make_unique<Call>();
      call->inputs["merged"] = Data::cloud(merged);
      call->inputs["primary"] = Data::cloud(primary);
      call->inputs["secondary"] = Data::cloud(secondary);
      call->inputs["boxLeft"] = Data::box2d(box(-0.006f, -0.004f, -0.001f, 0.002f));
      call->inputs["boxRight"] = Data::box2d(box(0.001f, -0.004f, 0.006f, 0.002f));
      return call;
    };
    auto loose = build();
    REQUIRE(loose->run("gap.fit_gap_circles", {{"rightCamera", Value::text("Secondary")}}).ok);
    const float looseR = loose->out("right").asCircle2D()->radius;

    auto floored = build();
    REQUIRE(floored
                ->run("gap.fit_gap_circles", {{"rightCamera", Value::text("Secondary")},
                                              {"rightMinArcDeg", Value::number(60.0)}})
                .ok);
    auto both = build();
    REQUIRE(both->run("gap.fit_gap_circles").ok);
    CHECK(floored->out("right").asCircle2D()->radius == both->out("right").asCircle2D()->radius);
    CHECK(floored->out("right").asCircle2D()->center[0] ==
          both->out("right").asCircle2D()->center[0]);
    CHECK(looseR != both->out("right").asCircle2D()->radius);
  }
  SUBCASE(
      "legacy:1713 gap.fit_gap_circles 的方位角带挑内点落在圆左上的那一个；guard 带内结果不变，出带才重拟") {

    SUBCASE("方位角带挑内点落在圆左上的那一个") {
      const auto build = [] {
        PointCloud cloud;
        pushLeftArc(cloud);
        pushArc(cloud, 0.0035, -0.0005, 0.0009, 180, 260, 40); // 内点在左上
        pushArc(cloud, 0.0035, 0.0015, 0.0009, 100, 180, 90);  // 内点在左下，点更多
        auto call = std::make_unique<Call>();
        call->inputs["merged"] = Data::cloud(cloud);
        call->inputs["primary"] = Data::cloud(cloud);
        call->inputs["secondary"] = Data::cloud(cloud);
        call->inputs["boxLeft"] = Data::box2d(box(-0.006f, -0.004f, -0.001f, 0.002f));
        call->inputs["boxRight"] = Data::box2d(box(0.001f, -0.004f, 0.006f, 0.003f));
        return call;
      };
      auto loose = build();
      REQUIRE(loose->run("gap.fit_gap_circles").ok);
      CHECK(rightCenterY(*loose) == doctest::Approx(0.0015).epsilon(0.05));

      auto banded = build();
      REQUIRE(banded
                  ->run("gap.fit_gap_circles", {{"rightArcBearingDeg", Value::number(140.0)},
                                                {"rightArcBearingTolDeg", Value::number(40.0)}})
                  .ok);
      CHECK(rightCenterY(*banded) == doctest::Approx(-0.0005).epsilon(0.05));
    }

    SUBCASE("guard：带内结果不变，出带才重拟") {
      const auto build = [](std::initializer_list<std::pair<const std::string, Value>> params) {
        PointCloud cloud;
        pushLeftArc(cloud);
        pushArc(cloud, 0.0035, -0.0005, 0.0009, 180, 260, 40);
        pushArc(cloud, 0.0035, 0.0015, 0.0009, 100, 180, 90);
        auto call = std::make_unique<Call>();
        call->inputs["merged"] = Data::cloud(cloud);
        call->inputs["primary"] = Data::cloud(cloud);
        call->inputs["secondary"] = Data::cloud(cloud);
        call->inputs["boxLeft"] = Data::box2d(box(-0.006f, -0.004f, -0.001f, 0.002f));
        call->inputs["boxRight"] = Data::box2d(box(0.001f, -0.004f, 0.006f, 0.003f));
        REQUIRE(call->run("gap.fit_gap_circles", params).ok);
        return call;
      };
      auto plain = build({});
      auto guarded = build({{"rightArcBearingDeg", Value::number(-140.0)},
                            {"rightArcBearingTolDeg", Value::number(60.0)},
                            {"rightCenterMode", Value::text("guard")}});
      CHECK(guarded->out("right").asCircle2D()->center[1] ==
            plain->out("right").asCircle2D()->center[1]);
      CHECK(guarded->out("right").asCircle2D()->radius == plain->out("right").asCircle2D()->radius);
      auto moved = build({{"rightArcBearingDeg", Value::number(140.0)},
                          {"rightArcBearingTolDeg", Value::number(40.0)},
                          {"rightCenterMode", Value::text("guard")}});
      CHECK(rightCenterY(*moved) == doctest::Approx(-0.0005).epsilon(0.05));
    }
  }
  SUBCASE("legacy:1776 圆拟合的弱地板：合并云也弱就报失败，不给一个错的数") {

    PointCloud cloud;
    pushLeftArc(cloud);
    pushArc(cloud, 0.0035, -0.0005, 0.0009, 330, 340, 6); // 两边都只有 10°
    Call call;
    call.inputs["merged"] = Data::cloud(cloud);
    call.inputs["primary"] = Data::cloud(cloud);
    call.inputs["secondary"] = Data::cloud(cloud);
    call.inputs["boxLeft"] = Data::box2d(box(-0.006f, -0.004f, -0.001f, 0.002f));
    call.inputs["boxRight"] = Data::box2d(box(0.001f, -0.004f, 0.006f, 0.002f));
    const Status s = call.run("gap.fit_gap_circles", {{"rightMinArcDeg", Value::number(60.0)},
                                                      {"cameraFallback", Value::boolean(false)}});
    CHECK_FALSE(s.ok);
    CHECK(s.code == "circle_fit_failed");
  }
}

}
