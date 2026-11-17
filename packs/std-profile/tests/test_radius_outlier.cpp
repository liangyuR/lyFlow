#include <doctest/doctest.h>
#include <cmath>
#include <limits>

#include "helpers.h"
#include "lyflow/profile.h"
#include "lyflow_pcl/filters.h"

namespace lyflow::packs::std_profile {
void registerProfileRadiusOutlier(Registry&);
}
using namespace lyflow;
namespace {
const Registry& radiusRegistry() {
  static const Registry registry = [] {
    Registry value;
    packs::std_profile::registerProfileRadiusOutlier(value);
    return value;
  }();
  return registry;
}
PointCloud cloudFixture() {
  PointCloud cloud;
  for (float x : {0.0F, 0.0F, .5F, 1.0F, 4.0F}) {
    cloud.push(x, 0, 0);
    cloud.intensity.push_back(static_cast<float>(cloud.pointCount()));
    cloud.normals.insert(cloud.normals.end(), {0, 1, 0});
    cloud.rgb.insert(cloud.rgb.end(), {10, 20, 30});
  }
  return cloud;
}
Data profileFixture(bool invalid = false, const char* unit = "m") {
  auto cloud = cloudFixture();
  if (std::string(unit) == "mm")
    for (auto& coordinate : cloud.xyz)
      coordinate *= 1000;
  Profile profile;
  std::string why;
  REQUIRE(makeProfile(std::make_shared<const PointCloud>(std::move(cloud)),
                      {1, "measurement", unit, "XZ", "reflected", "acquisition", "camera-A", 50},
                      {1, static_cast<std::uint8_t>(invalid ? 0 : 1), 1, 1, 1}, {2, 5, 11, 20, 31},
                      profile, why));
  return Data::profile(std::move(profile));
}
struct Poll final : ExecContext {
  mutable int calls = 0;
  int stop = std::numeric_limits<int>::max();
  std::filesystem::path base;
  bool cancelled() const override { return ++calls >= stop; }
  void progress(float, std::string_view) override {}
  void log(LogLevel, std::string) override {}
  const std::filesystem::path& baseDir() const override { return base; }
  int threadBudget() const override { return 3; }
};
}
TEST_CASE("Shared radius kernel preserves dense threshold equality and excludes missing queries") {
  const auto cloud = cloudFixture();
  test::StubContext ctx;
  pcl_support::RadiusSelection result;
  REQUIRE(pcl_support::radiusOutlier(cloud, .5, 1, result, ctx).ok);
  CHECK(result.kept.sourceCloudId == cloud.id);
  CHECK(result.kept.values == std::vector<std::int32_t>{0, 1, 2, 3});
  CHECK(result.removed.values == std::vector<std::int32_t>{4});
  REQUIRE(pcl_support::radiusOutlier(cloud, std::nextafter(.5, 0.0), 1, result, ctx).ok);
  CHECK(result.kept.values == std::vector<std::int32_t>{0, 1});
  REQUIRE(pcl_support::radiusOutlier(cloud, .5, 4, result, ctx).ok);
  CHECK(result.kept.values.empty());
  CHECK(result.removed.values == std::vector<std::int32_t>{0, 1, 2, 3, 4});
  auto missing = cloud;
  missing.xyz[3] = std::numeric_limits<float>::quiet_NaN();
  REQUIRE(pcl_support::radiusOutlier(missing, .51, 1, result, ctx).ok);
  CHECK(result.kept.values == std::vector<std::int32_t>{0, 2, 3});
  CHECK(result.removed.values == std::vector<std::int32_t>{1, 4});
  PointCloud coincident;
  for (int i = 0; i < 3; ++i)
    coincident.push(0, 0, 0);
  REQUIRE(pcl_support::radiusOutlier(coincident, 1e-6, 2, result, ctx).ok);
  CHECK(result.kept.values == std::vector<std::int32_t>{0, 1, 2});
}
TEST_CASE(
    "Profile radius filtering matches the shared finite kernel and retains all original channels and slots") {
  for (const char* unit : {"m", "mm"}) {
    const auto source = profileFixture(false, unit);
    test::StubContext ctx;
    pcl_support::RadiusSelection expected;
    REQUIRE(pcl_support::radiusOutlier(cloudFixture(), .5, 1, expected, ctx).ok);
    test::OpCall call(radiusRegistry());
    call.inputs = {{"profile", source}};
    REQUIRE_MESSAGE(call.run("profile.radius_outlier",
                             {{"radius", Value::number(.5)}, {"minNeighbors", Value::integer(1)}})
                        .ok,
                    call.status.message);
    CHECK(call.out("kept").asIndices()->values == expected.kept.values);
    CHECK(call.out("removed").asIndices()->values == expected.removed.values);
    CHECK(call.out("kept").asIndices()->sourceCloudId == source.asProfile()->cloud->id);
    const auto* selected = call.out("profile").asProfile();
    REQUIRE(selected);
    CHECK(selected->descriptor.unit == unit);
    CHECK(selected->descriptor.sourceCount == 50);
    CHECK(selected->sourceIndices == std::vector<std::uint32_t>{2, 5, 11, 20});
    CHECK(selected->cloud->intensity == std::vector<float>{1, 2, 3, 4});
    CHECK(selected->cloud->normals.size() == 12);
    CHECK(selected->cloud->rgb.size() == 12);
  }
}
TEST_CASE("Profile radius filtering masks finite-invalid samples without fabricating neighbors") {
  auto source = profileFixture(true);
  auto value = *source.asProfile();
  auto cloud = *value.cloud;
  cloud.xyz[12] = std::numeric_limits<float>::quiet_NaN();
  value.cloud = std::make_shared<const PointCloud>(std::move(cloud));
  value.validity[4] = 0;
  source = Data::profile(std::move(value));
  test::OpCall call(radiusRegistry());
  call.inputs = {{"profile", source}};
  REQUIRE(call.run("profile.radius_outlier",
                   {{"radius", Value::number(.51)}, {"minNeighbors", Value::integer(1)}})
              .ok);
  CHECK(call.out("kept").asIndices()->values == std::vector<std::int32_t>{0, 2, 3});
  CHECK(call.out("removed").asIndices()->values == std::vector<std::int32_t>{1, 4});
  CHECK(call.out("profile").asProfile()->sourceIndices == std::vector<std::uint32_t>{2, 11, 20});
}
TEST_CASE("Shared radius kernel rejects illegal parameters atomically and honors cancellation") {
  const auto cloud = cloudFixture();
  test::StubContext ctx;
  pcl_support::RadiusSelection result{{17, {99}}, {17, {98}}};
  for (const auto radius : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                            std::numeric_limits<double>::quiet_NaN(), 1001.0}) {
    CHECK_FALSE(pcl_support::radiusOutlier(cloud, radius, 1, result, ctx).ok);
    CHECK(result.kept.values == std::vector<std::int32_t>{99});
  }
  for (const std::int64_t count : {std::int64_t(-1), std::int64_t(0), std::int64_t(10001),
                                   std::numeric_limits<std::int64_t>::max()}) {
    CHECK_FALSE(pcl_support::radiusOutlier(cloud, .5, count, result, ctx).ok);
    CHECK(result.kept.values == std::vector<std::int32_t>{99});
  }
  auto infinite = cloud;
  infinite.xyz[0] = std::numeric_limits<float>::infinity();
  CHECK_FALSE(pcl_support::radiusOutlier(infinite, .5, 1, result, ctx).ok);
  CHECK(result.kept.values == std::vector<std::int32_t>{99});
  Poll stopped;
  stopped.stop = 1;
  CHECK_FALSE(pcl_support::radiusOutlier(cloud, .5, 1, result, stopped).ok);
  CHECK(result.kept.values == std::vector<std::int32_t>{99});
  const auto source = profileFixture();
  test::OpCall call(radiusRegistry());
  call.inputs = {{"profile", source}};
  REQUIRE(call.run("profile.radius_outlier", {{"minNeighbors", Value::integer(1)}}).ok);
  const auto* op = call.registry->find("profile.radius_outlier");
  ParamMap params;
  for (const auto& p : op->params)
    params[p.name] = p.def;
  params["minNeighbors"] = Value::integer(1);
  Poll completed;
  std::unordered_map<std::string, Data> values;
  Outputs out(values);
  REQUIRE(op->compute(Inputs(call.inputs), ParamView(params, completed.base), out, completed).ok);
  values.clear();
  stopped.calls = 0;
  stopped.stop = completed.calls;
  CHECK_FALSE(op->compute(Inputs(call.inputs), ParamView(params, stopped.base), out, stopped).ok);
  CHECK(values.empty());
  PointCloud empty;
  REQUIRE(pcl_support::radiusOutlier(empty, .5, 1, result, ctx).ok);
  CHECK(result.kept.values.empty());
  CHECK(result.removed.values.empty());
  CHECK(result.kept.sourceCloudId == empty.id);
}

TEST_CASE("Shared radius kernel keeps native index order across thread budgets") {
  PointCloud cloud;
  for (int i = 0; i < 24577; ++i)
    cloud.push(static_cast<float>(i % 200) * .001F, static_cast<float>((i / 200) % 200) * .001F, 0);
  test::StubContext serial({}, 1);
  pcl_support::RadiusSelection expected;
  REQUIRE(pcl_support::radiusOutlier(cloud, .0011, 3, expected, serial).ok);
  REQUIRE_FALSE(expected.kept.values.empty());
  REQUIRE_FALSE(expected.removed.values.empty());
  for (int threads : {3, 8}) {
    test::StubContext parallel({}, threads);
    pcl_support::RadiusSelection actual;
    REQUIRE(pcl_support::radiusOutlier(cloud, .0011, 3, actual, parallel).ok);
    CHECK(actual.kept.values == expected.kept.values);
    CHECK(actual.removed.values == expected.removed.values);
    CHECK(std::is_sorted(actual.kept.values.begin(), actual.kept.values.end()));
    CHECK(std::is_sorted(actual.removed.values.begin(), actual.removed.values.end()));
  }
}
