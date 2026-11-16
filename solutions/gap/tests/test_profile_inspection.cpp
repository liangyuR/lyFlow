#include <doctest/doctest.h>
#include <cmath>
#include <cstring>
#include <limits>

#include "helpers.h"
#include "lyflow/profile.h"
#include "lyflow_profile/features.h"
#include "lyflow_profile/provenance.h"
#include "ops/profile_inspection.h"

using namespace lyflow;
namespace scene = lyflow::packs::gap::profile_scene;
namespace {
using Json = nlohmann::json;
float mm(double value) { return static_cast<float>(value / 1000); }
PointCloud notchCloud(double opening = 0, bool wall = true, double raise = 0) {
  PointCloud cloud;
  const auto surface = [](double x) { return 187 + .4 * (x - 13); };
  for (int i = 0; i <= 100; ++i) {
    const double x = 3 + .1 * i;
    if (!wall && x > 12.7)
      continue;
    cloud.push(mm(x), mm(surface(x) + (x > 12 ? .6 * (x - 12) : 0)), 0);
  }
  const double corner = surface(13) + .6 - raise;
  const double xWall = 13 + opening;
  if (wall)
    for (int i = 0; i <= 12; ++i)
      cloud.push(mm(xWall), mm(corner - .1 * i), 0);
  for (int i = 1; i <= 80; ++i)
    cloud.push(mm(xWall + .1 * i), mm(corner - 1.2 - .06 * i), 0);
  return cloud;
}
PointCloud grooveCloud(double shift = 0) {
  PointCloud cloud;
  const auto surface = [](double x) { return 230 + .5 * (x + 3); };
  for (int i = 0; i <= 200; ++i) {
    const double x = -13 + .1 * i;
    if (x < -3.15)
      cloud.push(mm(x), mm(surface(x) + shift), 0);
    else if (x > -2.85)
      cloud.push(mm(x), mm(surface(x) + .4 + shift), 0);
  }
  for (int i = 1; i <= 9; ++i) {
    cloud.push(mm(-3.15), mm(surface(-3.15) + .2 * i + shift), 0);
    cloud.push(mm(-2.85), mm(surface(-2.85) + .4 + .2 * i + shift), 0);
  }
  for (int i = -2; i <= 2; ++i) {
    const double x = -3 + .05 * i;
    cloud.push(mm(x), mm(surface(x) + 2.5 + shift), 0);
  }
  return cloud;
}
Data native(PointCloud cloud, const char* identity, const char* unit = "m",
            const char* plane = "XY", bool masked = false) {
  std::vector<std::uint8_t> validity(cloud.pointCount(), 1);
  if (masked) {
    cloud.push(999, -999, 0);
    cloud.push(std::numeric_limits<float>::quiet_NaN(), 0, 0);
    validity.push_back(0);
    validity.push_back(0);
  }
  std::vector<std::uint32_t> indices;
  for (std::size_t i = 0; i < cloud.pointCount(); ++i) {
    if (std::string(plane) == "XZ")
      std::swap(cloud.xyz[3 * i + 1], cloud.xyz[3 * i + 2]);
    else if (std::string(plane) == "YZ") {
      const float x = cloud.xyz[3 * i], y = cloud.xyz[3 * i + 1];
      cloud.xyz[3 * i] = 0;
      cloud.xyz[3 * i + 1] = x;
      cloud.xyz[3 * i + 2] = y;
    }
    if (std::string(unit) == "mm")
      for (int axis = 0; axis < 3; ++axis)
        cloud.xyz[3 * i + axis] *= 1000;
    cloud.intensity.push_back(static_cast<float>(i));
    cloud.normals.insert(cloud.normals.end(), {0, 0, 1});
    cloud.rgb.insert(cloud.rgb.end(), {10, 20, 30});
    indices.push_back(static_cast<std::uint32_t>(7 * i + 3));
  }
  const auto sourceCount = cloud.pointCount() * 7 + 10;
  Profile result;
  std::string why;
  REQUIRE_MESSAGE(makeProfile(std::make_shared<const PointCloud>(std::move(cloud)),
                              {1, "measurement", unit, plane, "reflected", "acquisition", identity,
                               sourceCount},
                              std::move(validity), std::move(indices), result, why),
                  why);
  return Data::profile(std::move(result));
}
ParamMap defaults(const std::vector<Param>& descriptions, const ParamMap& overrides = {}) {
  ParamMap result;
  for (const auto& parameter : descriptions)
    result.emplace(parameter.name, parameter.def);
  for (const auto& entry : overrides)
    result[entry.first] = entry.second;
  return result;
}
Data bind(const Data& profile, const Data& geometry) {
  return Data::record(
      Record{"profile.Geometry.v2", profile_contract::geometryContextV2(profile, geometry)});
}
void checkNumeric(const Data& actual, const Data& expected) {
  CHECK(actual.typeName() == expected.typeName());
  if (const auto* a = actual.asMeasurement()) {
    const auto* b = expected.asMeasurement();
    REQUIRE(b);
    CHECK(a->ok == b->ok);
    CHECK(a->unit == b->unit);
    if (std::isnan(a->value))
      CHECK(std::isnan(b->value));
    else
      CHECK(std::memcmp(&a->value, &b->value, sizeof(double)) == 0);
  } else {
    CHECK(actual.valueJson() == expected.valueJson());
  }
}
scene::CornerInputs cornerFixture() {
  auto source = native(notchCloud(), "corner-source");
  Line2D left;
  left.point[0] = .011F;
  left.point[1] = .171F;
  left.dir[0] = .8F;
  left.dir[1] = .4F;
  left.hasSegment = true;
  left.start[0] = .008F;
  left.start[1] = .1695F;
  left.end[0] = .012F;
  left.end[1] = .1715F;
  Line2D right = left;
  right.dir[1] = -.6F;
  right.start[0] = .014F;
  right.start[1] = .16875F;
  right.end[0] = .011F;
  right.end[1] = .171F;
  Point2D origin;
  origin.p[0] = .010F;
  origin.p[1] = .170F;
  scene::CornerInputs input;
  input.lineLeft = Data::line2d(left);
  input.lineRight = Data::line2d(right);
  input.origin = Data::point2d(origin);
  input.lineLeftContext = bind(source, input.lineLeft);
  input.lineRightContext = bind(source, input.lineRight);
  input.originContext = bind(source, input.origin);
  return input;
}
class CancelContext final : public ExecContext {
public:
  bool cancelled() const override { return true; }
  void progress(float, std::string_view) override {}
  void log(LogLevel, std::string) override {}
  const std::filesystem::path& baseDir() const override { return base_; }
  int threadBudget() const override { return 1; }

private:
  std::filesystem::path base_;
};
}

TEST_CASE("native scene corner preserves float geometry and explicit origin with strict contexts") {
  test::StubContext ctx;
  auto input = cornerFixture();
  const auto parameters =
      defaults(scene::cornerInspectionParams(), {{"scale", Value::number(-2)},
                                                 {"gapOffset", Value::number(.15)},
                                                 {"flushOffset", Value::number(-.2)}});
  scene::CornerResult result;
  REQUIRE(scene::measureProfileCorner(input, ParamView(parameters, ctx.baseDir()), result, ctx).ok);
  auto oldParameters = parameters;
  oldParameters["originX"] = Value::number(10);
  oldParameters["originY"] = Value::number(170);
  const std::unordered_map<std::string, Data> oldInputs{{"lineLeft", input.lineLeft},
                                                        {"lineRight", input.lineRight}};
  std::unordered_map<std::string, Data> oldResults;
  Outputs output(oldResults);
  REQUIRE(scene::inspection_detail::cornerGeometry(
              Inputs(oldInputs), ParamView(oldParameters, ctx.baseDir()), output, ctx, nullptr)
              .ok);
  for (const auto& entry : std::vector<std::pair<Data, const char*>>{{result.vertex, "vertex"},
                                                                     {result.gap, "gap"},
                                                                     {result.flush, "flush"},
                                                                     {result.angle, "angle"},
                                                                     {result.segment, "segment"}})
    checkNumeric(entry.first, oldResults.at(entry.second));
  ProfileDescriptor descriptor;
  std::string why;
  CHECK(profile_contract::geometryMatchesPhysical(result.vertex, result.vertexContext.asRecord(),
                                                  descriptor, why));
  CHECK(result.quality.data.at("originMode") == "explicit_metric_point");
  CHECK(result.quality.data.at("alignmentApplied") == false);
  const auto before = result.quality.data;
  input.origin = Data::point2d(Point2D{});
  CHECK_FALSE(
      scene::measureProfileCorner(input, ParamView(parameters, ctx.baseDir()), result, ctx).ok);
  CHECK(result.quality.data == before);
  input = cornerFixture();
  input.lineRightContext = Data::record(Record{"profile.Geometry.v1", Json::object()});
  CHECK_FALSE(
      scene::measureProfileCorner(input, ParamView(parameters, ctx.baseDir()), result, ctx).ok);
}

TEST_CASE(
    "native groove and notch retain unique legacy algorithm numerical fields across scene modes") {
  test::StubContext ctx;
  for (const bool notch : {false, true})
    for (const auto& mode : {"left", "right"})
      for (const auto& camera : {"both", "primary", "secondary"}) {
        CAPTURE(notch);
        CAPTURE(mode);
        CAPTURE(camera);
        const auto a = notch ? notchCloud(0) : grooveCloud();
        const auto b = notch ? notchCloud(.6) : grooveCloud(.2);
        scene::JointInputs input{native(a, "camera-A"), native(b, "camera-B"), {}, {}};
        auto parameters =
            defaults(notch ? scene::notchInspectionParams() : scene::grooveInspectionParams(),
                     {{"baseSide", Value::text(mode)},
                      {"scale", Value::number(-1.5)},
                      {"gapOffset", Value::number(.11)},
                      {"flushOffset", Value::number(-.07)}});
        if (notch)
          parameters["camera"] = Value::text(camera);
        const std::unordered_map<std::string, Data> oldInputs{{"primary", Data::cloud(a)},
                                                              {"secondary", Data::cloud(b)}};
        std::unordered_map<std::string, Data> oldResults;
        Outputs output(oldResults);
        const auto expected =
            notch ? scene::inspection_detail::notchClouds(
                        Inputs(oldInputs), ParamView(parameters, ctx.baseDir()), output, ctx)
                  : scene::inspection_detail::grooveClouds(
                        Inputs(oldInputs), ParamView(parameters, ctx.baseDir()), output, ctx);
        scene::JointResult actual;
        const auto status = notch ? scene::measureProfileNotch(
                                        input, ParamView(parameters, ctx.baseDir()), actual, ctx)
                                  : scene::measureProfileGroove(
                                        input, ParamView(parameters, ctx.baseDir()), actual, ctx);
        REQUIRE(status.ok == expected.ok);
        if (!status.ok) {
          CHECK(status.code == expected.code);
          continue;
        }
        for (const auto& entry : std::vector<std::pair<Data, const char*>>{
                 {actual.gap, "gap"},
                 {actual.flush, "flush"},
                 {actual.baseLine, "baseLine"},
                 {actual.refLine, "refLine"},
                 {actual.gapSegment, "gapSegment"},
                 {actual.flushSegment, "flushSegment"},
                 {actual.feature, notch ? "anchor" : "groove"}})
          checkNumeric(entry.first, oldResults.at(entry.second));
        for (auto field = oldResults.at("quality").asRecord()->data.begin();
             field != oldResults.at("quality").asRecord()->data.end(); ++field)
          CHECK(actual.quality.data.at(field.key()) == field.value());
        for (auto field = oldResults.at("qualityBase").asRecord()->data.begin();
             field != oldResults.at("qualityBase").asRecord()->data.end(); ++field)
          CHECK(actual.qualityBase.data.at(field.key()) == field.value());
        CHECK(actual.quality.data.at("actualSourceIds").at("primary") == Json::array({"camera-A"}));
      }
}

TEST_CASE(
    "native scene inspection masks slots and maps declared XY XZ YZ planes and m mm without modifying input") {
  test::StubContext ctx;
  for (const bool notch : {false, true}) {
    const auto cloud = notch ? notchCloud(.2) : grooveCloud();
    const auto parameters =
        defaults(notch ? scene::notchInspectionParams() : scene::grooveInspectionParams());
    scene::JointInputs baseline{native(cloud, "a"), native(cloud, "b"), {}, {}};
    scene::JointResult expected;
    REQUIRE((notch ? scene::measureProfileNotch(baseline, ParamView(parameters, ctx.baseDir()),
                                                expected, ctx)
                   : scene::measureProfileGroove(baseline, ParamView(parameters, ctx.baseDir()),
                                                 expected, ctx))
                .ok);
    for (const auto* unit : {"m", "mm"})
      for (const auto* plane : {"XY", "XZ", "YZ"}) {
        CAPTURE(notch);
        CAPTURE(unit);
        CAPTURE(plane);
        scene::JointInputs input{
            native(cloud, "a", unit, plane, true), native(cloud, "b", unit, plane, true), {}, {}};
        const auto original = input.primary.valueJson();
        const auto* pointer = input.primary.asProfile()->cloud.get();
        scene::JointResult actual;
        REQUIRE((notch ? scene::measureProfileNotch(input, ParamView(parameters, ctx.baseDir()),
                                                    actual, ctx)
                       : scene::measureProfileGroove(input, ParamView(parameters, ctx.baseDir()),
                                                     actual, ctx))
                    .ok);
        CHECK(actual.gap.asMeasurement()->value ==
              doctest::Approx(expected.gap.asMeasurement()->value).epsilon(.002));
        CHECK(actual.flush.asMeasurement()->value ==
              doctest::Approx(expected.flush.asMeasurement()->value).epsilon(.002));
        CHECK(input.primary.asProfile()->cloud.get() == pointer);
        CHECK(input.primary.valueJson() == original);
        CHECK(input.primary.asProfile()->sourceIndices[1] == 10);
        CHECK(input.primary.asProfile()->cloud->rgb.size() ==
              3 * input.primary.asProfile()->pointCount());
        CHECK(actual.pointContext.asRecord()->data.at("sources")[0].at("descriptor").at("plane") ==
              plane);
      }
  }
}

TEST_CASE(
    "native scene inspection rejects mismatched provenance geometry and cancellation atomically") {
  test::StubContext ctx;
  auto parameters = defaults(scene::notchInspectionParams());
  scene::JointInputs input{native(notchCloud(), "a"), native(notchCloud(), "b"), {}, {}};
  scene::JointResult result;
  result.quality.type = "sentinel";
  Record wrong;
  std::string why;
  REQUIRE(
      profile_contract::rebindProvenance(input.secondary, input.secondary, nullptr, wrong, why));
  input.primaryProvenance = Data::record(wrong);
  CHECK_FALSE(
      scene::measureProfileNotch(input, ParamView(parameters, ctx.baseDir()), result, ctx).ok);
  CHECK(result.quality.type == "sentinel");
  input.primaryProvenance = {};
  Profile altered = *input.secondary.asProfile();
  altered.descriptor.frame = "another-frame";
  input.secondary = Data::profile(altered);
  CHECK_FALSE(
      scene::measureProfileNotch(input, ParamView(parameters, ctx.baseDir()), result, ctx).ok);
  input.secondary = native(notchCloud(), "b");
  parameters["confirmPoints"] = Value::number(1e30);
  CHECK(scene::measureProfileNotch(input, ParamView(parameters, ctx.baseDir()), result, ctx).code ==
        "bad_param");
  parameters = defaults(scene::notchInspectionParams());
  CancelContext cancel;
  CHECK(scene::measureProfileNotch(input, ParamView(parameters, ctx.baseDir()), result, cancel)
            .code == "cancelled");
  CHECK(result.quality.type == "sentinel");
  input.primary = native(PointCloud{}, "a");
  input.secondary = native(PointCloud{}, "b");
  CHECK_FALSE(
      scene::measureProfileNotch(input, ParamView(parameters, ctx.baseDir()), result, ctx).ok);
  CHECK(result.quality.type == "sentinel");
}

TEST_CASE(
    "native notch partial success has unavailable reference contexts rejected by a real executor consumer") {
  Registry local;
  scene::registerProfileInspection(local);
  const auto* original = local.find("gap.notch_width");
  REQUIRE(original);
  auto& registry = ensureRegistry();
  const std::string id = "test.m4c.profile_notch_partial";
  if (!registry.find(id)) {
    auto op = *original;
    op.id = id;
    op.version = "1.0.0";
    op.migrations.clear();
    registry.addOperator(std::move(op));
  }
  const auto primary = native(notchCloud(), "camera-A");
  const auto secondary = native(notchCloud(), "camera-B");
  const auto graph = test::makeGraph({{"p", "util.reroute"},
                                      {"s", "util.reroute"},
                                      {"measure", id, {{"refNear", 20.0}, {"refFar", 25.0}}},
                                      {"consume", "profile.nearest_to_line"}},
                                     {{"p.out", "measure.primary"},
                                      {"s.out", "measure.secondary"},
                                      {"p.out", "consume.profile"},
                                      {"measure.refLine", "consume.line"},
                                      {"measure.refLineContext", "consume.lineContext"}});
  test::Session session(graph, {}, {}, false, 1, true,
                        {{"p", "out", primary}, {"s", "out", secondary}});
  const auto& log = session.wait();
  CHECK(log.finalState("measure") == "done");
  CHECK(log.finalState("consume") == "error");
  Data gap, flush, unavailableContext, quality, consumed;
  REQUIRE(exec::ResultStore::instance().get(session.runId(), "measure", "gap", gap));
  REQUIRE(exec::ResultStore::instance().get(session.runId(), "measure", "flush", flush));
  REQUIRE(exec::ResultStore::instance().get(session.runId(), "measure", "refLineContext",
                                            unavailableContext));
  REQUIRE(exec::ResultStore::instance().get(session.runId(), "measure", "quality", quality));
  CHECK(gap.asMeasurement()->ok);
  CHECK_FALSE(flush.asMeasurement()->ok);
  CHECK(unavailableContext.asRecord()->type == "gap.GeometryUnavailable.v1");
  CHECK(unavailableContext.asRecord()->data.at("available") == false);
  CHECK(quality.asRecord()->data.at("refGeometryAvailable") == false);
  CHECK_FALSE(exec::ResultStore::instance().get(session.runId(), "consume", "point", consumed));
  const auto failure = log.nodeEvent("consume", "error").dump();
  CHECK(failure.find("bad_input") != std::string::npos);
  for (const auto& event : log.events) {
    CHECK(event.dump().find("output_not_written") == std::string::npos);
    CHECK(event.dump().find("internal_error") == std::string::npos);
  }
}

TEST_CASE(
    "native scene inspection zero-depth semantics preserve old core results and independent DLL golden") {
  test::StubContext ctx;
  for (const auto* name : {"grooveDepthMin", "gapDepth", "levelDepth"}) {
    CAPTURE(name);
    const bool notch = std::string(name) == "levelDepth";
    const auto cloud = notch ? notchCloud() : grooveCloud();
    const auto parameters =
        defaults(notch ? scene::notchInspectionParams() : scene::grooveInspectionParams(),
                 {{name, Value::number(0)}});
    const std::unordered_map<std::string, Data> oldInputs{{"primary", Data::cloud(cloud)},
                                                          {"secondary", Data::cloud(cloud)}};
    std::unordered_map<std::string, Data> values;
    Outputs pending(values);
    const auto expected =
        notch ? scene::inspection_detail::notchClouds(
                    Inputs(oldInputs), ParamView(parameters, ctx.baseDir()), pending, ctx)
              : scene::inspection_detail::grooveClouds(
                    Inputs(oldInputs), ParamView(parameters, ctx.baseDir()), pending, ctx);
    scene::JointResult actual;
    actual.quality.type = "sentinel";
    const scene::JointInputs input{native(cloud, "a"), native(cloud, "b"), {}, {}};
    const auto status =
        notch
            ? scene::measureProfileNotch(input, ParamView(parameters, ctx.baseDir()), actual, ctx)
            : scene::measureProfileGroove(input, ParamView(parameters, ctx.baseDir()), actual, ctx);
    REQUIRE(status.ok == expected.ok);
    if (!status.ok) {
      CHECK(status.code == expected.code);
      CHECK(actual.quality.type == "sentinel");
      continue;
    }
    checkNumeric(actual.gap, values.at("gap"));
    checkNumeric(actual.flush, values.at("flush"));
    const double legacyGap =
        std::string(name) == "gapDepth" ? 0.3500001039355993 : 0.2999999560415745;
    const double legacyFlush = -0.3577798637159791;
    CHECK(std::memcmp(&actual.gap.asMeasurement()->value, &legacyGap, sizeof(double)) == 0);
    CHECK(std::memcmp(&actual.flush.asMeasurement()->value, &legacyFlush, sizeof(double)) == 0);
  }
}

namespace inspection_legacy {
constexpr double kGrooveXcMm = -3.0, kGrooveY0Mm = 230.0, kGrooveSlopeK = .5;
constexpr double kNotchXcMm = 13.0, kNotchY0Mm = 187.0, kNotchSlopeA = .4, kNotchEdgeDrop = .6,
                 kNotchWallHeight = 1.2;
Line2D seg(float x0, float y0, float x1, float y1) {
  Line2D l;
  l.hasSegment = true;
  l.start[0] = x0;
  l.start[1] = y0;
  l.end[0] = x1;
  l.end[1] = y1;
  l.point[0] = x0;
  l.point[1] = y0;
  const float dx = x1 - x0;
  const float dy = y1 - y0;
  const float n = std::sqrt(dx * dx + dy * dy);
  l.dir[0] = dx / n;
  l.dir[1] = dy / n;
  return l;
}
float mmf(double v) { return static_cast<float>(v / 1000.0); }
PointCloud grooveCloud(double gapHalfMm, double flushMm) {
  PointCloud c;
  const auto baseY = [](double xMm) { return kGrooveY0Mm + kGrooveSlopeK * (xMm - kGrooveXcMm); };
  for (int i = 0; i <= 200; ++i) {
    const double xMm = kGrooveXcMm - 10.0 + 0.1 * i;
    if (xMm < kGrooveXcMm - gapHalfMm) {
      c.push(mmf(xMm), mmf(baseY(xMm)), 0.0f);
    } else if (xMm > kGrooveXcMm + gapHalfMm) {
      c.push(mmf(xMm), mmf(baseY(xMm) + flushMm), 0.0f);
    }
  }
  for (int i = 1; i <= 9; ++i) {
    const double d = 0.2 * i;
    const double xl = kGrooveXcMm - gapHalfMm;
    const double xr = kGrooveXcMm + gapHalfMm;
    c.push(mmf(xl), mmf(baseY(xl) + d), 0.0f);
    c.push(mmf(xr), mmf(baseY(xr) + flushMm + d), 0.0f);
  }
  for (int j = -2; j <= 2; ++j) {
    const double xMm = kGrooveXcMm + 0.05 * j;
    c.push(mmf(xMm), mmf(baseY(xMm) + 2.5), 0.0f);
  }
  return c;
}
PointCloud flatCloud() {
  PointCloud c;
  for (int i = 0; i <= 200; ++i) {
    const double xMm = kGrooveXcMm - 10.0 + 0.1 * i;
    c.push(mmf(xMm), mmf(kGrooveY0Mm + kGrooveSlopeK * (xMm - kGrooveXcMm)), 0.0f);
  }
  return c;
}
PointCloud shiftedInY(const PointCloud& src, double dyMm) {
  PointCloud c;
  for (std::size_t i = 0; i < src.pointCount(); ++i) {
    c.push(src.xyz[3 * i], src.xyz[3 * i + 1] + mmf(dyMm), src.xyz[3 * i + 2]);
  }
  return c;
}
double nominalFlushMm(double flushMm) { return -flushMm * std::cos(std::atan(kGrooveSlopeK)); }
float notchMm(double v) { return static_cast<float>(v / 1000.0); }
PointCloud notchCloud(double openMm, bool withWall = true, double raiseMm = 0.0) {
  PointCloud c;
  const auto yA = [](double xMm) { return kNotchY0Mm + kNotchSlopeA * (xMm - kNotchXcMm); };
  for (int i = 0; i <= 100; ++i) {
    const double xMm = kNotchXcMm - 10.0 + 0.1 * i;
    const double edge = xMm > kNotchXcMm - 1.0 ? kNotchEdgeDrop * (xMm - (kNotchXcMm - 1.0)) : 0.0;
    if (!withWall && xMm > kNotchXcMm - 0.3)
      continue;
    c.push(notchMm(xMm), notchMm(yA(xMm) + edge), 0.0f);
  }
  const double yCorner = yA(kNotchXcMm) + kNotchEdgeDrop - raiseMm;
  const double xWall = kNotchXcMm + openMm;
  if (withWall) {
    for (int i = 0; i <= 12; ++i) {
      c.push(notchMm(xWall), notchMm(yCorner - 0.1 * i), 0.0f);
    }
  }
  for (int i = 1; i <= 80; ++i) {
    const double xMm = xWall + 0.1 * i;
    c.push(notchMm(xMm), notchMm(yCorner - kNotchWallHeight - 0.6 * 0.1 * i), 0.0f);
  }
  return c;
}
double notchNominalMm(double levelMm, double openMm) {
  return (1.0 - levelMm / kNotchEdgeDrop) + openMm;
}
struct Call {
  std::unordered_map<std::string, Data> inputs, outputs;
  Status run(const std::string& id, const ParamMap& overrides = {}) {
    outputs.clear();
    const bool corner = id == "gap.corner_vertex", notch = id == "gap.notch_width";
    auto parameters = defaults(corner  ? scene::cornerInspectionParams()
                               : notch ? scene::notchInspectionParams()
                                       : scene::grooveInspectionParams(),
                               overrides);
    test::StubContext ctx;
    if (corner) {
      const auto originProfile = native(PointCloud{}, "explicit-origin-fixture");
      const auto bound = [&](const Data& data) {
        return Data::record(Record{"profile.Geometry.v2",
                                   profile_contract::geometryContextV2(originProfile, data)});
      };
      Point2D origin;
      origin.p[0] = static_cast<float>(
          (overrides.count("originX") ? overrides.at("originX").floatValue() : 0) / 1000.0);
      origin.p[1] = static_cast<float>(
          (overrides.count("originY") ? overrides.at("originY").floatValue() : 0) / 1000.0);
      scene::CornerInputs input;
      input.lineLeft = inputs.at("lineLeft");
      input.lineRight = inputs.at("lineRight");
      input.lineLeftContext = bound(input.lineLeft);
      input.lineRightContext = bound(input.lineRight);
      input.origin = Data::point2d(origin);
      input.originContext = bound(input.origin);
      if (inputs.count("baseLine")) {
        input.baseLine = inputs.at("baseLine");
        input.baseLineContext = bound(input.baseLine);
      }
      scene::CornerResult result;
      const auto status =
          scene::measureProfileCorner(input, ParamView(parameters, ctx.baseDir()), result, ctx);
      if (status.ok)
        outputs = {{"vertex", result.vertex},   {"angle", result.angle},
                   {"gap", result.gap},         {"flush", result.flush},
                   {"segment", result.segment}, {"quality", Data::record(result.quality)}};
      return status;
    }
    scene::JointInputs input{native(*inputs.at("primary").asCloud(), "explicit-primary"),
                             native(*inputs.at("secondary").asCloud(), "explicit-secondary"),
                             {},
                             {}};
    scene::JointResult result;
    const auto status =
        notch
            ? scene::measureProfileNotch(input, ParamView(parameters, ctx.baseDir()), result, ctx)
            : scene::measureProfileGroove(input, ParamView(parameters, ctx.baseDir()), result, ctx);
    if (status.ok)
      outputs = {{"gap", result.gap},
                 {"flush", result.flush},
                 {"baseLine", result.baseLine},
                 {"refLine", result.refLine},
                 {"gapSegment", result.gapSegment},
                 {"flushSegment", result.flushSegment},
                 {notch ? "anchor" : "groove", result.feature},
                 {"quality", Data::record(result.quality)},
                 {"qualityBase", Data::record(result.qualityBase)},
                 {"qualityRef", Data::record(result.qualityRef)}};
    return status;
  }
  const Data& out(const std::string& name) { return outputs.at(name); }
};
TEST_CASE("Native corner retains published vertex rays angle and rejection fixtures") {
  SUBCASE("legacy:452 gap.corner_vertex 交点相对金件顶点沿基准面方向分解") {

    const Line2D left = seg(-0.005f, 0.17f, 0.005f, 0.17f);
    const Line2D right = seg(0.01f, 0.17f, 0.01f, 0.19f);
    Line2D base = left; // u = +x

    Call call;
    call.inputs["lineLeft"] = Data::line2d(left);
    call.inputs["lineRight"] = Data::line2d(right);
    call.inputs["baseLine"] = Data::line2d(base);
    REQUIRE(call.run("gap.corner_vertex", {{"originX", Value::number(8.0)},
                                           {"originY", Value::number(170.0)},
                                           {"gapOffset", Value::number(0.4)}})
                .ok);
    const Point2D* v = call.out("vertex").asPoint2D();
    REQUIRE(v != nullptr);
    CHECK(v->p[0] == doctest::Approx(0.01));
    CHECK(v->p[1] == doctest::Approx(0.17));
    CHECK(call.out("gap").asMeasurement()->value == doctest::Approx(2.4));
    CHECK(call.out("flush").asMeasurement()->value == doctest::Approx(0.0));
    CHECK(call.out("angle").asMeasurement()->value == doctest::Approx(90.0));
    CHECK(call.out("angle").asMeasurement()->unit == "deg");

    Call scaled;
    scaled.inputs = call.inputs;
    REQUIRE(scaled
                .run("gap.corner_vertex", {{"originX", Value::number(8.0)},
                                           {"originY", Value::number(170.0)},
                                           {"scale", Value::number(0.5)}})
                .ok);
    CHECK(scaled.out("gap").asMeasurement()->value == doctest::Approx(1.0));
  }
  SUBCASE("legacy:487 gap.corner_vertex 的夹角取两条翼面从顶点出发的方向，60° 不会变成 120°") {

    const Line2D left = seg(0.0f, 0.0f, -0.01f, 0.0f);
    const Line2D right = seg(0.0f, 0.0f, -0.005f, -0.0086602540f);

    Call call;
    call.inputs["lineLeft"] = Data::line2d(left);
    call.inputs["lineRight"] = Data::line2d(right);
    REQUIRE(call.run("gap.corner_vertex").ok);
    CHECK(call.out("angle").asMeasurement()->value == doctest::Approx(60.0).epsilon(0.01));
  }
  SUBCASE("legacy:499 gap.corner_vertex 近平行与夹角出界都判失败") {

    Call parallel;
    parallel.inputs["lineLeft"] = Data::line2d(seg(0.0f, 0.0f, 0.01f, 0.0f));
    parallel.inputs["lineRight"] = Data::line2d(seg(0.0f, 0.001f, 0.01f, 0.001f));
    const Status ps = parallel.run("gap.corner_vertex");
    CHECK_FALSE(ps.ok);
    CHECK(ps.code == "invalid_geometry");

    Call narrow;
    narrow.inputs["lineLeft"] = Data::line2d(seg(-0.005f, 0.17f, 0.005f, 0.17f));
    narrow.inputs["lineRight"] = Data::line2d(seg(0.01f, 0.17f, 0.01f, 0.19f));
    const Status ns = narrow.run("gap.corner_vertex", {{"maxAngle", Value::number(45.0)}});
    CHECK_FALSE(ns.ok);
    CHECK(ns.code == "invalid_geometry");
  }
}
TEST_CASE(
    "Native groove and notch retain published geometry depth occlusion and signed flush fixtures") {
  SUBCASE(
      "legacy:667 gap.groove_joint 量出槽宽与面差：换基准侧、单相机、两相机整体差、scale 与 offset") {

    SUBCASE("量出槽宽与面差") {
      const PointCloud cloud = grooveCloud(0.15, 0.4);
      Call call;
      call.inputs["primary"] = Data::cloud(cloud);
      call.inputs["secondary"] = Data::cloud(cloud);
      REQUIRE(call.run("gap.groove_joint").ok);

      CHECK(call.out("gap").asMeasurement()->value == doctest::Approx(0.3).epsilon(0.05));
      CHECK(call.out("flush").asMeasurement()->value ==
            doctest::Approx(nominalFlushMm(0.4)).epsilon(0.02));

      const Point2D* groove = call.out("groove").asPoint2D();
      REQUIRE(groove != nullptr);
      CHECK(std::fabs(static_cast<double>(groove->p[0]) - kGrooveXcMm / 1000.0) < 0.0002);

      const Record* q = call.out("quality").asRecord();
      REQUIRE(q != nullptr);
      CHECK(q->type == "gap.GrooveQuality.v2");
      CHECK(q->data["lowerSurface"].get<std::string>() == "ref");
      CHECK(q->data["cameras"]["primary"]["valid"].get<bool>());

      const Record* qb = call.out("qualityBase").asRecord();
      REQUIRE(qb != nullptr);
      CHECK(qb->type == "gap.FitQuality.v2");
      CHECK(qb->data["model"].get<std::string>() == "line");
      const Record* qr = call.out("qualityRef").asRecord();
      REQUIRE(qr != nullptr);
      CHECK(qr->type == "gap.FitQuality.v2");
    }

    SUBCASE("换基准侧只翻 flush 的符号") {
      const PointCloud cloud = grooveCloud(0.15, 0.4);
      Call call;
      call.inputs["primary"] = Data::cloud(cloud);
      call.inputs["secondary"] = Data::cloud(cloud);
      REQUIRE(call.run("gap.groove_joint", {{"baseSide", Value::text("right")}}).ok);
      CHECK(call.out("flush").asMeasurement()->value ==
            doctest::Approx(-nominalFlushMm(0.4)).epsilon(0.02));
      CHECK(call.out("gap").asMeasurement()->value == doctest::Approx(0.3).epsilon(0.05));
      CHECK(call.out("quality").asRecord()->data["lowerSurface"].get<std::string>() == "base");
    }

    SUBCASE("只有一台相机也能测，另一台标 valid=false") {
      const PointCloud cloud = grooveCloud(0.15, 0.4);
      Call call;
      call.inputs["primary"] = Data::cloud(cloud);
      call.inputs["secondary"] = Data::cloud(PointCloud{});
      REQUIRE(call.run("gap.groove_joint").ok);
      CHECK(call.out("gap").asMeasurement()->value == doctest::Approx(0.3).epsilon(0.05));
      CHECK(call.out("flush").asMeasurement()->value ==
            doctest::Approx(nominalFlushMm(0.4)).epsilon(0.02));
      const Record* q = call.out("quality").asRecord();
      REQUIRE(q != nullptr);
      CHECK(q->data["cameras"]["primary"]["valid"].get<bool>());
      CHECK_FALSE(q->data["cameras"]["secondary"]["valid"].get<bool>());
    }

    SUBCASE("两台相机整体差 0.2 mm，平均之后 flush 不动") {
      const PointCloud cloud = grooveCloud(0.15, 0.4);
      Call call;
      call.inputs["primary"] = Data::cloud(cloud);
      call.inputs["secondary"] = Data::cloud(shiftedInY(cloud, 0.2));
      REQUIRE(call.run("gap.groove_joint").ok);
      CHECK(std::fabs(call.out("flush").asMeasurement()->value - nominalFlushMm(0.4)) < 0.02);
      CHECK(call.out("gap").asMeasurement()->value == doctest::Approx(0.3).epsilon(0.05));
    }

    SUBCASE("scale 与两个 offset 是线性的") {
      const PointCloud cloud = grooveCloud(0.15, 0.4);
      Call call;
      call.inputs["primary"] = Data::cloud(cloud);
      call.inputs["secondary"] = Data::cloud(cloud);
      REQUIRE(call.run("gap.groove_joint", {{"scale", Value::number(2.0)},
                                            {"gapOffset", Value::number(0.1)},
                                            {"flushOffset", Value::number(0.05)}})
                  .ok);
      CHECK(call.out("gap").asMeasurement()->value == doctest::Approx(0.7).epsilon(0.05));
      CHECK(call.out("flush").asMeasurement()->value ==
            doctest::Approx(nominalFlushMm(0.4) + 0.05).epsilon(0.05));
    }
  }
  SUBCASE("legacy:750 gap.groove_joint 没有槽就报 groove_not_found") {

    Call call;
    call.inputs["primary"] = Data::cloud(flatCloud());
    call.inputs["secondary"] = Data::cloud(flatCloud());
    const Status s = call.run("gap.groove_joint");
    CHECK_FALSE(s.ok);
    CHECK(s.code == "groove_not_found");
  }
  SUBCASE("legacy:886 gap.notch_width 闭合缝读出圆边固有宽度，张开多少就多读多少") {

    for (const double open : {0.0, 0.5, 1.2}) {
      Call call;
      call.inputs["primary"] = Data::cloud(notchCloud(open));
      call.inputs["secondary"] = Data::cloud(notchCloud(open));
      REQUIRE(call.run("gap.notch_width").ok);
      CHECK(call.out("gap").asMeasurement()->value ==
            doctest::Approx(notchNominalMm(0.25, open)).epsilon(0.03));
      const Record* q = call.out("quality").asRecord();
      REQUIRE(q != nullptr);
      CHECK(q->type == "gap.NotchQuality.v2");
      CHECK(q->data["validCameras"].get<int>() == 2);
      CHECK(q->data["cameras"]["primary"]["cornerDepthMm"].get<double>() ==
            doctest::Approx(kNotchEdgeDrop).epsilon(0.05));
      const Point2D* anchor = call.out("anchor").asPoint2D();
      REQUIRE(anchor != nullptr);
      CHECK(std::fabs(static_cast<double>(anchor->p[0]) - kNotchXcMm / 1000.0) < 0.0002);
      CHECK(call.out("qualityBase").asRecord()->type == "gap.FitQuality.v2");
    }
  }
  SUBCASE(
      "legacy:907 gap.notch_width 的读数随 levelDepth、scale / gapOffset 变，对面翼面拟不出来时面差无效、开口照常") {

    SUBCASE("换 levelDepth 沿圆边滑动，读数按 1/坡度 变") {
      Call call;
      call.inputs["primary"] = Data::cloud(notchCloud(0.0));
      call.inputs["secondary"] = Data::cloud(notchCloud(0.0));
      REQUIRE(call.run("gap.notch_width", {{"levelDepth", Value::number(0.4)}}).ok);
      CHECK(call.out("gap").asMeasurement()->value ==
            doctest::Approx(notchNominalMm(0.4, 0.0)).epsilon(0.05));
    }

    SUBCASE("scale 与 gapOffset 是线性的") {
      Call call;
      call.inputs["primary"] = Data::cloud(notchCloud(0.0));
      call.inputs["secondary"] = Data::cloud(notchCloud(0.0));
      REQUIRE(call.run("gap.notch_width").ok);
      const double raw = call.out("gap").asMeasurement()->value;
      REQUIRE(call.run("gap.notch_width",
                       {{"scale", Value::number(2.0)}, {"gapOffset", Value::number(-0.1)}})
                  .ok);
      CHECK(call.out("gap").asMeasurement()->value ==
            doctest::Approx(raw * 2.0 - 0.1).epsilon(1e-6));
    }

    SUBCASE("对面翼面拟不出来时面差无效、开口照常") {
      Call call;
      call.inputs["primary"] = Data::cloud(notchCloud(0.0));
      call.inputs["secondary"] = Data::cloud(notchCloud(0.0));
      REQUIRE(call.run("gap.notch_width",
                       {{"refNear", Value::number(20.0)}, {"refFar", Value::number(25.0)}})
                  .ok);
      CHECK(call.out("gap").asMeasurement()->ok);
      CHECK_FALSE(call.out("flush").asMeasurement()->ok);
      CHECK(call.out("refLine").asLine2D() != nullptr);
      CHECK(call.out("flushSegment").asLine2D() != nullptr);
      CHECK(call.out("qualityRef").asRecord() != nullptr);
    }
  }
  SUBCASE(
      "legacy:945 gap.notch_width 的 camera：只取指定那一台，both 取平均；被遮挡的相机量的是自己的阴影") {

    SUBCASE("只取指定那一台，both 取有效相机的平均") {
      Call call;
      call.inputs["primary"] = Data::cloud(notchCloud(0.0));
      call.inputs["secondary"] = Data::cloud(notchCloud(0.6));
      REQUIRE(call.run("gap.notch_width", {{"camera", Value::text("secondary")}}).ok);
      CHECK(call.out("gap").asMeasurement()->value ==
            doctest::Approx(notchNominalMm(0.25, 0.6)).epsilon(0.03));
      REQUIRE(call.run("gap.notch_width", {{"camera", Value::text("primary")}}).ok);
      CHECK(call.out("gap").asMeasurement()->value ==
            doctest::Approx(notchNominalMm(0.25, 0.0)).epsilon(0.03));
      REQUIRE(call.run("gap.notch_width").ok);
      CHECK(call.out("gap").asMeasurement()->value ==
            doctest::Approx(notchNominalMm(0.25, 0.3)).epsilon(0.03));
    }

    SUBCASE("被遮挡的相机量的是自己的阴影，与看得见立边的那台不同") {
      Call call;
      call.inputs["primary"] = Data::cloud(notchCloud(0.0, /*withWall=*/false));
      call.inputs["secondary"] = Data::cloud(notchCloud(0.0));
      REQUIRE(call.run("gap.notch_width", {{"camera", Value::text("secondary")}}).ok);
      const double seen = call.out("gap").asMeasurement()->value;
      CHECK(seen == doctest::Approx(notchNominalMm(0.25, 0.0)).epsilon(0.03));
      REQUIRE(call.run("gap.notch_width", {{"camera", Value::text("primary")}}).ok);
      const double shadow = call.out("gap").asMeasurement()->value;
      CHECK(std::fabs(shadow - seen) > 0.3);
    }
  }
  SUBCASE(
      "legacy:974 gap.notch_width 对面整体抬高（面差）：开口读数不变，面差随之变、flushBase 决定符号") {

    SUBCASE("对面整体抬高不改变开口读数") {
      Call call;
      call.inputs["primary"] = Data::cloud(notchCloud(0.4, true, 0.0));
      call.inputs["secondary"] = Data::cloud(notchCloud(0.4, true, 0.0));
      REQUIRE(call.run("gap.notch_width").ok);
      const double flat = call.out("gap").asMeasurement()->value;
      call.inputs["primary"] = Data::cloud(notchCloud(0.4, true, 0.3));
      call.inputs["secondary"] = Data::cloud(notchCloud(0.4, true, 0.3));
      REQUIRE(call.run("gap.notch_width").ok);
      CHECK(call.out("gap").asMeasurement()->value == doctest::Approx(flat).epsilon(0.02));
      const Record* q = call.out("quality").asRecord();
      REQUIRE(q != nullptr);
      CHECK(q->data["cameras"]["secondary"]["levelYMm"].is_number());
    }

    SUBCASE("面差随对面抬高而变，flushBase 决定符号") {
      Call call;
      call.inputs["primary"] = Data::cloud(notchCloud(0.4, true, 0.0));
      call.inputs["secondary"] = Data::cloud(notchCloud(0.4, true, 0.0));
      REQUIRE(call.run("gap.notch_width").ok);
      REQUIRE(call.out("flush").asMeasurement()->ok);
      const double flat = call.out("flush").asMeasurement()->value;
      call.inputs["primary"] = Data::cloud(notchCloud(0.4, true, 0.3));
      call.inputs["secondary"] = Data::cloud(notchCloud(0.4, true, 0.3));
      REQUIRE(call.run("gap.notch_width").ok);
      const double raised = call.out("flush").asMeasurement()->value;
      CHECK(raised - flat ==
            doctest::Approx(0.3 * std::cos(std::atan(kNotchSlopeA))).epsilon(0.05));
      CHECK(call.out("refLine").asLine2D() != nullptr);
      CHECK(call.out("flushSegment").asLine2D() != nullptr);
      CHECK(call.out("qualityRef").asRecord()->type == "gap.FitQuality.v2");
      REQUIRE(call.run("gap.notch_width", {{"flushBase", Value::text("right")}}).ok);
      CHECK(call.out("flush").asMeasurement()->value == doctest::Approx(-raised).epsilon(0.1));
      const double gapRight = call.out("gap").asMeasurement()->value;
      REQUIRE(call.run("gap.notch_width", {{"flushBase", Value::text("left")}}).ok);
      CHECK(call.out("gap").asMeasurement()->value == doctest::Approx(gapRight).epsilon(1e-6));
    }
  }
  SUBCASE("legacy:1016 gap.notch_width 没有 V 缝就报 notch_too_shallow") {

    PointCloud flat;
    for (int i = 0; i <= 200; ++i) {
      const double xMm = kNotchXcMm - 10.0 + 0.1 * i;
      flat.push(notchMm(xMm), notchMm(kNotchY0Mm + kNotchSlopeA * (xMm - kNotchXcMm)), 0.0f);
    }
    Call call;
    call.inputs["primary"] = Data::cloud(flat);
    call.inputs["secondary"] = Data::cloud(flat);
    const Status s = call.run("gap.notch_width");
    CHECK_FALSE(s.ok);
    CHECK(s.code == "notch_too_shallow");
  }
}

}
