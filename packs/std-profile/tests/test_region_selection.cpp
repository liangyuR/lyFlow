#include <doctest/doctest.h>
#include <limits>

#include "helpers.h"
#include "lyflow/profile.h"
#include "lyflow_profile/context.h"
#include "lyflow_profile/provenance.h"

namespace lyflow::packs::std_profile {
void registerProfileRegionSelection(Registry&);
void registerProfileSelection(Registry&);
}
using namespace lyflow;
namespace {
const Registry& regionRegistry() {
  static const Registry registry = [] {
    Registry r;
    packs::std_profile::registerProfileRegionSelection(r);
    packs::std_profile::registerProfileSelection(r);
    return r;
  }();
  return registry;
}
Data source(const char* plane = "XZ", const char* unit = "mm") {
  const bool xy = std::string(plane) == "XY";
  const float scale = std::string(unit) == "mm" ? 1000.0F : 1.0F;
  PointCloud cloud;
  for (int i = 0; i < 5; ++i) {
    const float x = static_cast<float>(i) * .001F * scale;
    cloud.push(x, xy ? x : 9.0F, xy ? 9.0F : x);
    cloud.intensity.push_back(static_cast<float>(i));
    cloud.normals.insert(cloud.normals.end(), {0, 1, 0});
    cloud.rgb.insert(cloud.rgb.end(), {static_cast<std::uint8_t>(i), 20, 30});
  }
  const float nan = std::numeric_limits<float>::quiet_NaN();
  cloud.xyz[6] = cloud.xyz[7] = cloud.xyz[8] = nan;
  Profile p;
  std::string why;
  REQUIRE(makeProfile(std::make_shared<const PointCloud>(std::move(cloud)),
                      {1, "measurement", unit, plane, "reflected", "acquisition", "physical-A", 50},
                      {1, 0, 0, 1, 1}, {2, 5, 11, 20, 31}, p, why));
  return Data::profile(std::move(p));
}
Data box() {
  Box2D b;
  b.min[0] = b.min[1] = 0;
  b.max[0] = b.max[1] = .004F;
  return Data::box2d(b);
}
Data context(const Data& p, const Data& geometry) {
  return Data::record(
      Record{"profile.Geometry.v2", profile_contract::geometryContextV2(p, geometry)});
}
struct Poll final : ExecContext {
  mutable int count = 0;
  int stop = std::numeric_limits<int>::max();
  std::filesystem::path base;
  bool cancelled() const override { return ++count >= stop; }
  void progress(float, std::string_view) override {}
  void log(LogLevel, std::string) override {}
  const std::filesystem::path& baseDir() const override { return base; }
  int threadBudget() const override { return 1; }
};
}
TEST_CASE("Profile ROI keeps source slots and all channels with explicit metric boundaries") {
  for (const char* plane : {"XY", "XZ", "YZ"}) {
    for (const char* unit : {"m", "mm"}) {
      auto p = source(plane, unit);
      if (std::string(plane) == "YZ") {
        auto value = *p.asProfile();
        auto cloud = *value.cloud;
        for (std::size_t i = 0; i < value.pointCount(); ++i)
          std::swap(cloud.xyz[3 * i], cloud.xyz[3 * i + 1]);
        value.cloud = std::make_shared<const PointCloud>(std::move(cloud));
        p = Data::profile(std::move(value));
      }
      const auto b = box();
      const auto before = profile_contract::context(p);
      test::OpCall call(regionRegistry());
      call.inputs = {{"profile", p}, {"box", b}, {"boxContext", context(p, b)}};
      REQUIRE_MESSAGE(call.run("profile.crop_box").ok, call.status.message);
      CHECK(call.out("selected").asIndices()->values == std::vector<std::int32_t>{0, 3, 4});
      CHECK(call.out("selected").asIndices()->sourceCloudId == p.asProfile()->cloud->id);
      const auto* selected = call.out("profile").asProfile();
      REQUIRE(selected);
      CHECK(selected->sourceIndices == std::vector<std::uint32_t>{2, 20, 31});
      CHECK(selected->descriptor.sourceCount == 50);
      CHECK(selected->cloud->intensity == std::vector<float>{0, 3, 4});
      CHECK(selected->cloud->rgb == std::vector<std::uint8_t>{0, 20, 30, 3, 20, 30, 4, 20, 30});
      CHECK(selected->cloud->normals.size() == 9);
      REQUIRE(call.out("provenance").asRecord());
      nlohmann::json sources;
      std::string why;
      CHECK(profile_contract::profileSources(call.out("profile"), call.out("provenance").asRecord(),
                                             sources, why));
      REQUIRE(call.run("profile.crop_box", {{"bounds", Value::text("open")}}).ok);
      CHECK(call.out("selected").asIndices()->values == std::vector<std::int32_t>{3});
      REQUIRE(call.run("profile.crop_box",
                       {{"bounds", Value::text("open")}, {"mode", Value::text("preserve_slots")}})
                  .ok);
      CHECK(call.out("profile").asProfile()->cloud == p.asProfile()->cloud);
      CHECK(call.out("profile").asProfile()->validity == std::vector<std::uint8_t>{0, 0, 0, 1, 0});
      CHECK(profile_contract::context(p) == before);
    }
  }
}
TEST_CASE(
    "Profile index selection requires exact identity and stable unique slots with explicit invalid policy") {
  const auto p = source();
  test::OpCall call(regionRegistry());
  call.inputs = {{"profile", p},
                 {"indices", Data::indices(Indices{p.asProfile()->cloud->id, {0, 3}})}};
  REQUIRE(call.run("profile.select_indices").ok);
  CHECK(call.out("profile").asProfile()->sourceIndices == std::vector<std::uint32_t>{2, 20});
  for (const auto& indices : {std::vector<std::int32_t>{3, 0}, {0, 0}, {-1}, {5}, {1}}) {
    call.inputs["indices"] = Data::indices(Indices{p.asProfile()->cloud->id, indices});
    CHECK_FALSE(call.run("profile.select_indices").ok);
    CHECK(call.outputs.empty());
  }
  call.inputs["indices"] = Data::indices(Indices{p.asProfile()->cloud->id + 1, {0}});
  CHECK_FALSE(call.run("profile.select_indices").ok);
  call.inputs["indices"] = Data::indices(Indices{p.asProfile()->cloud->id, {1, 2}});
  REQUIRE(call.run("profile.select_indices", {{"invalidPolicy", Value::text("preserve")}}).ok);
  CHECK(call.out("profile").asProfile()->validity == std::vector<std::uint8_t>{0, 0});
  CHECK(call.out("profile").asProfile()->sourceIndices == std::vector<std::uint32_t>{5, 11});
  CHECK(std::isnan(call.out("profile").asProfile()->cloud->xyz[3]));
  call.inputs["indices"] = Data::indices(Indices{p.asProfile()->cloud->id, {}});
  CHECK_FALSE(call.run("profile.select_indices").ok);
  REQUIRE(call.run("profile.select_indices", {{"emptyPolicy", Value::text("allow")}}).ok);
  CHECK(call.out("profile").asProfile()->pointCount() == 0);
  CHECK(call.out("profile").asProfile()->descriptor.sourceCount == 50);
}
TEST_CASE(
    "Profile regions reject wrong geometry and provenance and publish nothing on cancellation") {
  const auto p = source(), b = box();
  test::OpCall call(regionRegistry());
  call.inputs = {{"profile", p}, {"box", b}, {"boxContext", context(p, b)}};
  REQUIRE(call.run("profile.crop_box").ok);
  const auto validProvenance = call.out("provenance");
  call.inputs["provenance"] = validProvenance;
  CHECK_FALSE(call.run("profile.crop_box").ok);
  CHECK(call.outputs.empty());
  call.inputs.erase("provenance");
  auto wrong = *b.asBox2D();
  wrong.max[0] = 3;
  call.inputs["box"] = Data::box2d(wrong);
  CHECK_FALSE(call.run("profile.crop_box").ok);
  CHECK(call.outputs.empty());
  call.inputs["box"] = b;
  const auto* op = call.registry->find("profile.crop_box");
  ParamMap parameters;
  for (const auto& parameter : op->params)
    parameters[parameter.name] = parameter.def;
  Poll completed;
  std::unordered_map<std::string, Data> values;
  Outputs out(values);
  REQUIRE(
      op->compute(Inputs(call.inputs), ParamView(parameters, completed.base), out, completed).ok);
  values.clear();
  Poll stopped;
  stopped.stop = completed.count;
  CHECK_FALSE(
      op->compute(Inputs(call.inputs), ParamView(parameters, stopped.base), out, stopped).ok);
  CHECK(values.empty());
}

TEST_CASE(
    "Composite Profile selections preserve physical segment identity and ROI reference sources") {
  const auto a = source();
  auto bProfile = *source().asProfile();
  bProfile.descriptor.sourceId = "physical-B";
  const auto b = Data::profile(std::move(bProfile));
  auto cProfile = *source("XZ", "m").asProfile();
  cProfile.descriptor.sourceId = "physical-ROI";
  const auto c = Data::profile(std::move(cProfile));
  Profile merged = *a.asProfile();
  auto cloud = *merged.cloud;
  const auto& other = *b.asProfile()->cloud;
  cloud.xyz.insert(cloud.xyz.end(), other.xyz.begin(), other.xyz.end());
  cloud.intensity.insert(cloud.intensity.end(), other.intensity.begin(), other.intensity.end());
  cloud.normals.insert(cloud.normals.end(), other.normals.begin(), other.normals.end());
  cloud.rgb.insert(cloud.rgb.end(), other.rgb.begin(), other.rgb.end());
  merged.cloud = std::make_shared<const PointCloud>(std::move(cloud));
  merged.validity.insert(merged.validity.end(), b.asProfile()->validity.begin(),
                         b.asProfile()->validity.end());
  for (auto slot : b.asProfile()->sourceIndices)
    merged.sourceIndices.push_back(slot + 50);
  Record provenance;
  std::string why;
  REQUIRE_MESSAGE(profile_contract::makeCompositeProvenance(a, b, nullptr, nullptr, "secondary",
                                                            "primary", "require_equal", merged,
                                                            provenance, why),
                  why);
  const auto paired = Data::profile(std::move(merged));
  const auto roi = box();
  test::OpCall call(regionRegistry());
  call.inputs = {{"profile", paired},
                 {"provenance", Data::record(provenance)},
                 {"box", roi},
                 {"boxContext", context(c, roi)}};
  REQUIRE_MESSAGE(call.run("profile.crop_box", {{"bounds", Value::text("open")}}).ok,
                  call.status.message);
  const auto crop = call.out("profile"), cropProvenance = call.out("provenance");
  CHECK(crop.asProfile()->sourceIndices == std::vector<std::uint32_t>{20, 70});
  nlohmann::json references;
  REQUIRE(profile_contract::profileSources(crop, cropProvenance.asRecord(), references, why));
  CHECK(std::find(references.begin(), references.end(), profile_contract::context(c)) !=
        references.end());
  call.inputs = {{"profile", crop}, {"provenance", cropProvenance}};
  REQUIRE(call.run("profile.filter_validity", {{"mode", Value::text("compact")}}).ok);
  const auto finite = call.out("profile"), finiteProvenance = call.out("provenance");
  Point2D point;
  point.p[0] = point.p[1] = .003F;
  const auto anchor = Data::point2d(point);
  call.inputs = {{"profile", finite},
                 {"provenance", finiteProvenance},
                 {"anchor", anchor},
                 {"anchorContext", context(c, anchor)}};
  REQUIRE(call.run("profile.nearest_sample").ok);
  CHECK(call.out("selection").asRecord()->data["source"]["logicalRole"] == "secondary");
  CHECK(call.out("selection").asRecord()->data["source"]["sourceId"] == "physical-A");
  CHECK(call.out("selection").asRecord()->data["source"]["sourceIndex"] == 20);
  const auto& geometrySources = call.out("pointContext").asRecord()->data["sources"];
  CHECK(std::find(geometrySources.begin(), geometrySources.end(), profile_contract::context(c)) !=
        geometrySources.end());
  call.inputs = {{"profile", finite},
                 {"provenance", finiteProvenance},
                 {"geometry", anchor},
                 {"referenceGeometry", roi},
                 {"referenceContext", context(c, roi)}};
  REQUIRE(call.run("profile.bind_geometry",
                   {{"frame", Value::text("measurement")}, {"plane", Value::text("XZ")}})
              .ok);
  CHECK(call.out("context").asRecord()->data["relation"] == "declared");
  const auto& boundSources = call.out("context").asRecord()->data["sources"];
  CHECK(std::find(boundSources.begin(), boundSources.end(), profile_contract::context(c)) !=
        boundSources.end());
  call.inputs.erase("referenceGeometry");
  CHECK_FALSE(call.run("profile.bind_geometry",
                       {{"frame", Value::text("measurement")}, {"plane", Value::text("XZ")}})
                  .ok);
  CHECK(call.outputs.empty());
}
