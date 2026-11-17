#include <doctest/doctest.h>
#include <cstring>
#include <limits>

#include "helpers.h"
#include "lyflow/profile.h"
#include "lyflow_profile/context.h"
#include "ops/profile_guard.h"

using namespace lyflow;
namespace scene = lyflow::packs::gap::profile_scene;
namespace {
Data flat(double heightMm, const char* id, const char* plane = "XZ", const char* unit = "m",
          double shiftMm = 0, bool rightStep = false) {
  const std::string axes = plane;
  const bool mm = std::string(unit) == "mm";
  PointCloud cloud;
  std::vector<std::uint8_t> validity;
  std::vector<std::uint32_t> sourceIndices;
  for (int i = 0; i <= 100; ++i) {
    const double xMm = .1 * i + shiftMm;
    const double hMm = rightStep && xMm >= 5 ? 174 : heightMm;
    const float x = static_cast<float>(mm ? xMm : xMm / 1000.0);
    const float h = static_cast<float>(mm ? hMm : hMm / 1000.0);
    if (axes == "XY")
      cloud.push(x, h, 0);
    else if (axes == "YZ")
      cloud.push(0, x, h);
    else
      cloud.push(x, 0, h);
    cloud.intensity.push_back(static_cast<float>(i));
    cloud.normals.insert(cloud.normals.end(), {0, 1, 0});
    cloud.rgb.insert(cloud.rgb.end(), {70, 80, 90});
    validity.push_back(1);
    sourceIndices.push_back(static_cast<std::uint32_t>(i * 2));
  }
  Profile result;
  std::string why;
  REQUIRE(makeProfile(std::make_shared<const PointCloud>(std::move(cloud)),
                      {1, "sensor", unit, plane, "right", "acquisition", id, 256},
                      std::move(validity), std::move(sourceIndices), result, why));
  return Data::profile(std::move(result));
}
bool bitEqual(double a, double b) { return std::memcmp(&a, &b, sizeof(double)) == 0; }
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
Data roi() {
  Box2D box;
  box.min[0] = 0;
  box.min[1] = .16F;
  box.max[0] = .0045F;
  box.max[1] = .18F;
  return Data::box2d(box);
}
}
TEST_CASE(
    "Native camera guard retains legacy overlap statistics and float bits on declared sensor planes") {
  struct Case {
    const char* name;
    const char* action;
    double a;
    double b;
    bool xy;
    int minimum;
    bool shifted;
    bool cropped;
  };
  const Case cases[] = {{"close", "fail", 170, 170.02, false, 20, false, false},
                        {"record", "record", 170, 172.5, false, 20, false, false},
                        {"keep-primary", "keepPrimary", 170, 172.5, false, 20, false, false},
                        {"keep-secondary", "keepSecondary", 170, 172.5, false, 20, false, false},
                        {"fail", "fail", 170, 172.5, false, 20, false, false},
                        {"off", "off", 170, 175, false, 20, false, false},
                        {"reverse", "record", 172.5, 170, false, 20, false, false},
                        {"insufficient", "record", 170, 172.5, false, 21, false, false},
                        {"no-overlap", "fail", 170, 172.5, false, 20, true, false},
                        {"measurement-XY", "record", 170, 172.5, true, 20, false, false},
                        {"ROI-left", "record", 170, 170.02, false, 1, false, true}};
  for (const auto& item : cases) {
    CAPTURE(item.name);
    const auto a = flat(item.a, "physical-A", item.xy ? "XY" : "XZ");
    const auto b =
        flat(item.b, "physical-B", item.xy ? "XY" : "XZ", "m", item.shifted ? 20 : 0, item.cropped);
    scene::CameraGuardOptions options;
    options.onDisagree = item.action;
    options.minSamples = item.minimum;
    scene::GuardResult result;
    result.quality.type = "sentinel";
    test::StubContext ctx;
    const auto box = roi();
    const auto context = profile_contract::geometryContext(a.asProfile()->descriptor);
    const auto status =
        scene::guardProfiles(a, b, options, result, ctx, item.cropped ? &box : nullptr,
                             item.cropped ? &context : nullptr);
    if (std::string(item.name) == "fail") {
      CHECK_FALSE(status.ok);
      CHECK(status.code == "camera_disagree");
      CHECK(result.quality.type == "sentinel");
      continue;
    }
    REQUIRE_MESSAGE(status.ok, status.message);
    REQUIRE(result.quality.type == "gap.CameraGuard.v3");
    const auto& quality = result.quality.data;
    CHECK(quality["onDisagree"] == item.action);
    CHECK(bitEqual(quality["maxDeltaMm"].get<double>(), .5));
    const bool off = std::string(item.action) == "off";
    const bool insufficient = item.shifted || item.minimum == 21;
    CHECK(quality["evaluated"] == (!off && !insufficient));
    if (off || insufficient) {
      CHECK(quality["exceeded"] == false);
      CHECK_FALSE(quality.contains("deltaMedianMm"));
      CHECK_FALSE(quality.contains("signedMedianMm"));
      CHECK(quality["taken"] == "both");
      if (off)
        CHECK_FALSE(quality.contains("sampleCount"));
      else {
        CHECK(quality["sampleCount"] == (item.shifted ? 0 : 20));
        CHECK(quality["reason"] == "insufficient_overlap");
      }
    } else {
      const bool close = item.a == 170 && item.b == 170.02;
      const double expected = close ? .019997358322143555 : 2.499997615814209;
      CHECK(bitEqual(quality["deltaMedianMm"].get<double>(), expected));
      CHECK(bitEqual(quality["signedMedianMm"].get<double>(),
                     item.a > item.b ? expected : -expected));
      CHECK(quality["sampleCount"] == (item.cropped ? 9 : 20));
      CHECK(quality["exceeded"] == !close);
    }
    const auto* selectedA = std::string(item.action) == "keepSecondary" ? &b : &a;
    const auto* selectedB = std::string(item.action) == "keepPrimary" ? &a : &b;
    CHECK(result.primary.profilePtr() == selectedA->profilePtr());
    CHECK(result.secondary.profilePtr() == selectedB->profilePtr());
    CHECK(quality["primarySourceId"] == selectedA->asProfile()->descriptor.sourceId);
    CHECK(quality["secondarySourceId"] == selectedB->asProfile()->descriptor.sourceId);
  }
}
TEST_CASE("Native camera guard uses declared planes and units and ignores finite-invalid payload") {
  for (const char* plane : {"XY", "XZ", "YZ"}) {
    const auto a = flat(128, "physical-A", plane, "mm"), b = flat(128.5, "physical-B", plane);
    scene::GuardResult result;
    test::StubContext ctx;
    scene::CameraGuardOptions options;
    REQUIRE(scene::guardProfiles(a, b, options, result, ctx).ok);
    CHECK(result.quality.data["evaluated"] == true);
    const double expected =
        std::fabs(static_cast<double>(static_cast<float>(.128)) - static_cast<float>(.1285)) *
        1000.0;
    CHECK(bitEqual(result.quality.data["deltaMedianMm"].get<double>(), expected));
    CHECK(result.primary.profilePtr() == a.profilePtr());
    CHECK(result.primary.asProfile()->descriptor.unit == "mm");
  }
  auto a = flat(170, "physical-A");
  auto changed = *a.asProfile();
  auto cloud = *changed.cloud;
  for (std::size_t i = 0; i < changed.pointCount(); ++i)
    cloud.xyz[3 * i + 1] = 5;
  cloud.xyz[3] = std::numeric_limits<float>::quiet_NaN();
  changed.cloud = std::make_shared<const PointCloud>(std::move(cloud));
  changed.validity.assign(changed.pointCount(), 0);
  changed.validity.front() = changed.validity.back() = 1;
  a = Data::profile(std::move(changed));
  const auto b = flat(172.5, "physical-B");
  scene::GuardResult result;
  scene::CameraGuardOptions options;
  test::StubContext ctx;
  REQUIRE(scene::guardProfiles(a, b, options, result, ctx).ok);
  CHECK(result.quality.data["evaluated"] == false);
  CHECK(result.quality.data["reason"] == "insufficient_overlap");
  CHECK(result.primary.asProfile()->pointCount() == 101);
  CHECK(result.primary.asProfile()->cloud == a.asProfile()->cloud);
}
TEST_CASE(
    "Native camera guard rejects invalid geometry parameters and stagnant or over-budget sampling atomically") {
  const auto a = flat(170, "physical-A"), b = flat(172.5, "physical-B");
  test::StubContext ctx;
  scene::GuardResult result;
  result.quality.type = "sentinel";
  scene::CameraGuardOptions options;
  for (const double value : {0.0, -1.0}) {
    options.sampleStepMm = value;
    CHECK_FALSE(scene::guardProfiles(a, b, options, result, ctx).ok);
    CHECK(result.quality.type == "sentinel");
  }
  options = {};
  options.minSamples = 1;
  options.maxSamples = 5;
  CHECK(scene::guardProfiles(a, b, options, result, ctx).code == "budget_exceeded");
  CHECK(result.quality.type == "sentinel");
  auto far = *a.asProfile();
  auto cloud = *far.cloud;
  for (std::size_t i = 0; i < far.pointCount(); ++i)
    cloud.xyz[3 * i] = i ? std::nextafter(1e20F, std::numeric_limits<float>::infinity()) : 1e20F;
  far.cloud = std::make_shared<const PointCloud>(std::move(cloud));
  const auto extreme = Data::profile(std::move(far));
  options = {};
  CHECK(scene::guardProfiles(extreme, extreme, options, result, ctx).code == "sample_step_stalled");
  CHECK(result.quality.type == "sentinel");
  auto wrong = *b.asProfile();
  wrong.descriptor.frame = "other";
  const auto otherSpace = profile_contract::geometryContext(wrong.descriptor);
  const auto mismatch = Data::profile(std::move(wrong));
  CHECK_FALSE(scene::guardProfiles(a, mismatch, options, result, ctx).ok);
  const auto box = roi();
  CHECK_FALSE(scene::guardProfiles(a, b, options, result, ctx, &box).ok);
  CHECK_FALSE(scene::guardProfiles(a, b, options, result, ctx, &box, &otherSpace).ok);
  CHECK(result.quality.type == "sentinel");
}
TEST_CASE(
    "Native camera guard node and typed helper cancel before publishing raw profiles or quality") {
  const auto a = flat(170, "physical-A"), b = flat(170.02, "physical-B");
  scene::CameraGuardOptions options;
  scene::GuardResult result;
  Poll completed;
  REQUIRE(scene::guardProfiles(a, b, options, result, completed).ok);
  result.quality.type = "sentinel";
  Poll cancelled;
  cancelled.stop = completed.calls;
  CHECK(scene::guardProfiles(a, b, options, result, cancelled).code == "cancelled");
  CHECK(result.quality.type == "sentinel");
  Registry registry;
  scene::registerProfileCameraGuard(registry);
  test::OpCall call(registry);
  call.inputs = {{"primary", a}, {"secondary", b}};
  REQUIRE(call.run("gap.camera_guard").ok);
  CHECK(call.out("primary").profilePtr() == a.profilePtr());
  CHECK(call.out("quality").asRecord()->type == "gap.CameraGuard.v3");
  call.inputs["box"] = roi();
  CHECK_FALSE(call.run("gap.camera_guard").ok);
  CHECK(call.outputs.empty());
}
