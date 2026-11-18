#include <doctest/doctest.h>
#include <cmath>
#include <limits>

#include "helpers.h"
#include "lyflow/profile.h"
#include "lyflow_profile/context.h"

using namespace lyflow;
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
    "Profile overlap observation reports signed and absolute heights for valid shared stations") {
  struct Case {
    double a, b;
    const char* plane;
    const char* unitA;
    int minimum;
    double shift;
    bool clipped;
    int samples;
    bool evaluated;
  };
  const Case cases[] = {{170, 170.02, "XZ", "m", 20, 0, false, 20, true},
                        {170, 172.5, "XZ", "m", 20, 0, false, 20, true},
                        {172.5, 170, "XZ", "m", 20, 0, false, 20, true},
                        {170, 172.5, "XZ", "m", 21, 0, false, 20, false},
                        {170, 172.5, "XZ", "m", 20, 20, false, 0, false},
                        {170, 172.5, "XY", "m", 20, 0, false, 20, true},
                        {170, 170.02, "XZ", "m", 1, 0, true, 9, true},
                        {128, 128.5, "XY", "mm", 20, 0, false, 20, true},
                        {128, 128.5, "XZ", "mm", 20, 0, false, 20, true},
                        {128, 128.5, "YZ", "mm", 20, 0, false, 20, true}};
  for (const auto& item : cases) {
    CAPTURE(item.a);
    CAPTURE(item.b);
    CAPTURE(item.samples);
    const auto a = flat(item.a, "physical-A", item.plane, item.unitA);
    const auto b = flat(item.b, "physical-B", item.plane, "m", item.shift, item.clipped);
    test::OpCall call;
    call.inputs = {{"primary", a}, {"secondary", b}};
    if (item.clipped) {
      call.inputs["box"] = roi();
      call.inputs["boxContext"] =
          Data::record(profile_contract::geometryContext(a.asProfile()->descriptor));
    }
    REQUIRE(call.run("profile.compare_overlap", {{"minSamples", Value::integer(item.minimum)}}).ok);
    const auto& value = call.out("observation").asRecord()->data;
    CHECK(value["evaluated"] == item.evaluated);
    CHECK(value["sampleCount"] == item.samples);
    CHECK(value["unit"] == "m");
    if (item.evaluated) {
      const double expected = static_cast<double>(static_cast<float>(item.a / 1000.0)) -
                              static_cast<float>(item.b / 1000.0);
      CHECK(value["signedMedian"].get<double>() == doctest::Approx(expected).epsilon(1e-6));
      CHECK(value["absoluteMedian"].get<double>() ==
            doctest::Approx(std::fabs(expected)).epsilon(1e-6));
    } else {
      CHECK(value["reason"] == "insufficient_overlap");
      CHECK_FALSE(value.contains("absoluteMedian"));
    }
  }
}

TEST_CASE(
    "Profile overlap skips invalid slots and rejects bad spaces, collapsed sampling and cancellation") {
  auto a = flat(170, "physical-A");
  const auto b = flat(172.5, "physical-B");
  auto changed = *a.asProfile();
  auto cloud = *changed.cloud;
  cloud.xyz[3] = std::numeric_limits<float>::quiet_NaN();
  changed.cloud = std::make_shared<const PointCloud>(std::move(cloud));
  changed.validity.assign(changed.pointCount(), 0);
  changed.validity.front() = changed.validity.back() = 1;
  test::OpCall call;
  call.inputs = {{"primary", Data::profile(std::move(changed))}, {"secondary", b}};
  REQUIRE(call.run("profile.compare_overlap").ok);
  CHECK(call.out("observation").asRecord()->data["reason"] == "insufficient_overlap");
  call.inputs["primary"] = a;
  for (const double step : {0., -1.}) {
    CHECK_FALSE(call.run("profile.compare_overlap", {{"sampleStep", Value::number(step)}}).ok);
    CHECK(call.outputs.empty());
  }
  CHECK(call.run("profile.compare_overlap",
                 {{"minSamples", Value::integer(1)}, {"maxSamples", Value::integer(5)}})
            .code == "budget_exceeded");
  auto far = *a.asProfile();
  cloud = *far.cloud;
  for (std::size_t i = 0; i < far.pointCount(); ++i)
    cloud.xyz[3 * i] = i ? std::nextafter(1e20F, std::numeric_limits<float>::infinity()) : 1e20F;
  far.cloud = std::make_shared<const PointCloud>(std::move(cloud));
  const auto extreme = Data::profile(std::move(far));
  call.inputs = {{"primary", extreme}, {"secondary", extreme}};
  CHECK(call.run("profile.compare_overlap").code == "sample_step_stalled");
  auto wrong = *b.asProfile();
  wrong.descriptor.frame = "other";
  const auto otherSpace = profile_contract::geometryContext(wrong.descriptor);
  call.inputs = {{"primary", a}, {"secondary", Data::profile(std::move(wrong))}};
  CHECK(call.run("profile.compare_overlap").code == "space_mismatch");
  call.inputs["secondary"] = b;
  call.inputs["box"] = roi();
  CHECK_FALSE(call.run("profile.compare_overlap").ok);
  call.inputs["boxContext"] = Data::record(otherSpace);
  CHECK(call.run("profile.compare_overlap").code == "space_mismatch");
  call.inputs.erase("box");
  call.inputs.erase("boxContext");
  REQUIRE(call.run("profile.compare_overlap").ok);
  const auto* op = call.registry->find("profile.compare_overlap");
  Inputs in(call.inputs);
  call.outputs.clear();
  Outputs out(call.outputs);
  ParamView params(call.params, call.base);
  Poll cancelled;
  cancelled.stop = 3;
  CHECK(op->compute(in, params, out, cancelled).code == "cancelled");
  CHECK(call.outputs.empty());
}
