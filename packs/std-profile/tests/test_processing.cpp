#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <numeric>

#include "helpers.h"
#include "lyflow/profile.h"

namespace lyflow::packs::std_profile {
void registerProfileProcessing(Registry&);
}

namespace {
using namespace lyflow;

const Registry& processingRegistry() {
  static const Registry registry = [] {
    Registry result;
    packs::std_profile::registerProfileProcessing(result);
    return result;
  }();
  return registry;
}

Data profileOf(const std::vector<float>& u, const std::vector<float>& v,
               std::vector<std::uint8_t> validity = {}, const char* unit = "mm",
               const char* plane = "XZ", const char* handedness = "right", bool channels = true) {
  REQUIRE(u.size() == v.size());
  PointCloud cloud;
  const int first = std::string(plane) == "YZ" ? 1 : 0;
  const int second = std::string(plane) == "XY" ? 1 : 2;
  const double scale = std::string(unit) == "m" ? .001 : 1;
  for (std::size_t i = 0; i < u.size(); ++i) {
    float coordinates[3]{41.25f, 41.25f, 41.25f};
    coordinates[first] = static_cast<float>(u[i] * scale);
    coordinates[second] = static_cast<float>(v[i] * scale);
    cloud.push(coordinates[0], coordinates[1], coordinates[2]);
    if (channels) {
      cloud.intensity.push_back(1);
      for (int component = 0; component < 3; ++component) {
        cloud.normals.push_back(static_cast<float>(i * 3 + component));
        cloud.rgb.push_back(static_cast<std::uint8_t>((i * 3 + component) % 256));
      }
    }
  }
  ProfileDescriptor descriptor;
  descriptor.frame = "scan-frame";
  descriptor.unit = unit;
  descriptor.plane = plane;
  descriptor.handedness = handedness;
  descriptor.sampleOrder = "acquisition";
  descriptor.sourceId = "sample-identity";
  descriptor.sourceCount = u.size() * 2 + 20;
  if (validity.empty())
    validity.assign(u.size(), 1);
  std::vector<std::uint32_t> sources;
  for (std::size_t i = 0; i < u.size(); ++i)
    sources.push_back(static_cast<std::uint32_t>(10 + i * 2));
  Profile profile;
  std::string why;
  REQUIRE(makeProfile(std::make_shared<const PointCloud>(std::move(cloud)), descriptor,
                      std::move(validity), std::move(sources), profile, why));
  return Data::profile(std::move(profile));
}

class CountingContext : public ExecContext {
public:
  explicit CountingContext(int limit) : limit_(limit) {}
  bool cancelled() const override { return ++checks_ >= limit_; }
  void progress(float, std::string_view) override {}
  void log(LogLevel, std::string) override {}
  const std::filesystem::path& baseDir() const override { return base_; }
  int threadBudget() const override { return 1; }

private:
  int limit_;
  mutable int checks_ = 0;
  std::filesystem::path base_;
};
} // namespace

TEST_CASE("Profile processing: slot median preserves finite rejected payloads and every channel") {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  Data source = profileOf({0, 1, 2, 3, 4, 5, 6}, {9, 1, nan, 5, 3, 99, 4}, {1, 1, 0, 1, 1, 0, 1});
  Profile profile = *source.asProfile();
  PointCloud cloud = *profile.cloud;
  cloud.intensity[4] = 0;
  profile.cloud = std::make_shared<const PointCloud>(std::move(cloud));
  source = Data::profile(std::move(profile));
  test::OpCall call(processingRegistry());
  call.inputs["profile"] = source;
  REQUIRE(call.run("profile.clean",
                   {{"smoothPoints", Value::integer(3)}, {"minValidPoints", Value::integer(4)}})
              .ok);
  const auto* result = call.out("clean").asProfile();
  REQUIRE(result);
  CHECK(result->pointCount() == 7);
  CHECK(result->validity == std::vector<std::uint8_t>{1, 1, 0, 1, 0, 0, 1});
  CHECK(result->cloud->xyz[2] == 5);
  CHECK(result->cloud->xyz[5] == 5);
  CHECK(std::isnan(result->cloud->xyz[8]));
  CHECK(result->cloud->xyz[11] == 5);
  CHECK(result->cloud->xyz[14] == 3);
  CHECK(result->cloud->xyz[17] == 99);
  CHECK(result->cloud->xyz[20] == 4);
  CHECK(result->cloud->id != source.asProfile()->cloud->id);
  CHECK(result->cloud->intensity == source.asProfile()->cloud->intensity);
  CHECK(result->cloud->normals == source.asProfile()->cloud->normals);
  CHECK(result->cloud->rgb == source.asProfile()->cloud->rgb);
  CHECK(result->sourceIndices == source.asProfile()->sourceIndices);
  CHECK(profileDescriptorJson(result->descriptor) ==
        profileDescriptorJson(source.asProfile()->descriptor));
  CHECK(source.asProfile()->cloud->xyz[2] == 9);
  CHECK(source.asProfile()->validity[4] == 1);
  CHECK(source.asProfile()->cloud->xyz[17] == 99);

  REQUIRE(call.run("profile.clean", {{"smoothPoints", Value::integer(3)},
                                     {"minValidPoints", Value::integer(4)},
                                     {"mode", Value::text("compact")}})
              .ok);
  result = call.out("clean").asProfile();
  REQUIRE(result);
  CHECK(result->pointCount() == 4);
  CHECK(result->validity == std::vector<std::uint8_t>{1, 1, 1, 1});
  CHECK(result->sourceIndices == std::vector<std::uint32_t>{10, 12, 16, 22});
  CHECK(result->descriptor.sourceCount == source.asProfile()->descriptor.sourceCount);
  const auto expected = source.asProfile()->cloud->select({0, 1, 3, 6});
  CHECK(result->cloud->normals == expected.normals);
  CHECK(result->cloud->rgb == expected.rgb);
  CHECK(result->cloud->intensity == expected.intensity);
}

TEST_CASE("Profile processing: valid-count gate is configurable without inventing intensity") {
  std::vector<float> x(16), z(16, 5);
  std::iota(x.begin(), x.end(), 0.f);
  test::OpCall call(processingRegistry());
  call.inputs["profile"] = profileOf(x, z, {}, "mm", "XZ", "right", false);
  REQUIRE(call.run("profile.clean").ok);
  CHECK(call.out("clean").asProfile()->cloud->intensity.empty());
  CHECK(call.out("clean").asProfile()->cloud->normals.empty());
  CHECK(call.out("clean").asProfile()->cloud->rgb.empty());
  auto invalid = *call.inputs["profile"].asProfile();
  invalid.validity[0] = 0;
  call.inputs["profile"] = Data::profile(invalid);
  auto status = call.run("profile.clean");
  CHECK_FALSE(status.ok);
  CHECK(status.code == "bad_input");
  CHECK(status.message.find("15") != std::string::npos);
  CHECK(call.outputs.empty());
  REQUIRE(call.run("profile.clean", {{"minValidPoints", Value::integer(15)}}).ok);
  CHECK(call.out("clean").asProfile()->validity[0] == 0);
  call.inputs["profile"] = profileOf({}, {}, {}, "mm", "XZ", "right", false);
  CHECK_FALSE(call.run("profile.clean").ok);
  CHECK(call.outputs.empty());
}

TEST_CASE(
    "Profile processing: unit and plane adapters preserve descriptor, handedness and third axis") {
  const std::vector<float> x{0, 1, 2, 3, 4}, z{9, 1, 5, 3, 4};
  const std::vector<float> expected{5, 5, 3, 4, 3.5f};
  for (const auto* plane : {"XY", "XZ", "YZ"})
    for (const auto* unit : {"m", "mm"})
      for (const auto* handedness : {"right", "reflected"}) {
        CAPTURE(std::string(plane));
        CAPTURE(std::string(unit));
        CAPTURE(std::string(handedness));
        const Data source = profileOf(x, z, {}, unit, plane, handedness);
        test::OpCall call(processingRegistry());
        call.inputs["profile"] = source;
        REQUIRE(call.run("profile.clean", {{"smoothPoints", Value::integer(1)},
                                           {"minValidPoints", Value::integer(1)}})
                    .ok);
        CHECK(call.out("clean").asProfile()->cloud->xyz == source.asProfile()->cloud->xyz);
        REQUIRE(call.run("profile.clean", {{"smoothPoints", Value::integer(3)},
                                           {"minValidPoints", Value::integer(1)}})
                    .ok);
        const auto* output = call.out("clean").asProfile();
        REQUIRE(output);
        const int second = std::string(plane) == "XY" ? 1 : 2;
        const int unused = std::string(plane) == "XY" ? 2 : (std::string(plane) == "XZ" ? 1 : 0);
        const double scale = std::string(unit) == "m" ? 1000 : 1;
        for (std::size_t i = 0; i < expected.size(); ++i) {
          CHECK(output->cloud->xyz[3 * i + second] * scale == doctest::Approx(expected[i]));
          CHECK(output->cloud->xyz[3 * i + unused] == 41.25f);
        }
        CHECK(output->cloud->normals == source.asProfile()->cloud->normals);
        CHECK(output->cloud->rgb == source.asProfile()->cloud->rgb);
        CHECK(output->sourceIndices == source.asProfile()->sourceIndices);
        CHECK(profileDescriptorJson(output->descriptor) ==
              profileDescriptorJson(source.asProfile()->descriptor));
      }
}

TEST_CASE("Profile processing: face pieces use active acquisition adjacency before x sorting") {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const Data source =
      profileOf({0, 1, 1.5f, 2, 2, 1, 0}, {0, 0, nan, 0, 4, 4, 4}, {1, 1, 0, 1, 1, 1, 1});
  test::OpCall call(processingRegistry());
  call.inputs["clean"] = source;
  REQUIRE(call.run("profile.split_faces",
                   {{"minPoints", Value::integer(3)}, {"jumpMm", Value::number(1)}})
              .ok);
  const auto* output = call.out("faces").asRecord();
  REQUIRE(output);
  CHECK(output->type == "profile.Faces.v2");
  const auto& result = output->data;
  CHECK(result["schemaVersion"] == 2);
  CHECK(result["workingUnit"] == "mm");
  CHECK(result["indexSpace"] == "active_samples");
  CHECK(result["slotCount"] == 7);
  CHECK(result["activeCount"] == 6);
  CHECK(result["parameters"]["minPoints"] == 3);
  REQUIRE(result["pieces"].size() == 2);
  CHECK(result["pieces"][0]["i0"] == 0);
  CHECK(result["pieces"][0]["i1"] == 2);
  CHECK(result["pieces"][0]["slotStart"] == 0);
  CHECK(result["pieces"][0]["slotEnd"] == 3);
  CHECK(result["pieces"][0]["sourceEnd"] == 16);
  CHECK(result["pieces"][1]["i0"] == 3);
  CHECK(result["pieces"][1]["i1"] == 5);
  CHECK(result["pieces"][1]["slotStart"] == 4);
  CHECK(result["pieces"][1]["slotEnd"] == 6);
  CHECK(result["pieces"][1]["sourceStart"] == 18);
  CHECK(result["pieces"][1]["x0"] == 0);
  CHECK(result["pieces"][1]["x1"] == 2);
  CHECK(result["faces"].empty());
  CHECK_FALSE(result.contains("xyz"));
  CHECK_FALSE(result.contains("sourceIndices"));
  CHECK(source.asProfile()->cloud->xyz[18] == 0);
  CHECK(source.asProfile()->sourceIndices[6] == 22);
}

TEST_CASE("Profile processing: extracted TLS faces agree across selected planes and length units") {
  std::vector<float> x, z;
  for (int i = 0; i <= 100; ++i) {
    x.push_back(static_cast<float>(i) / 10);
    z.push_back(2);
  }
  for (const auto* plane : {"XY", "XZ", "YZ"})
    for (const auto* unit : {"mm", "m"}) {
      CAPTURE(std::string(plane));
      CAPTURE(std::string(unit));
      test::OpCall call(processingRegistry());
      const Data source = profileOf(x, z, {}, unit, plane, "reflected");
      call.inputs["clean"] = source;
      REQUIRE(call.run("profile.split_faces").ok);
      const auto& result = call.out("faces").asRecord()->data;
      REQUIRE(result["faces"].size() == 1);
      const auto& face = result["faces"][0];
      CHECK(face["lenMm"].get<double>() == doctest::Approx(10));
      CHECK(face["pz"].get<double>() == doctest::Approx(2));
      CHECK(face["rmsMm"].get<double>() == doctest::Approx(0));
      CHECK(face["dx"].get<double>() == doctest::Approx(1));
      CHECK(face["dz"].get<double>() == doctest::Approx(0));
      CHECK(face["n"] == 101);
    }
}

TEST_CASE("Profile processing: unknown order and insufficient samples fail explicitly") {
  test::OpCall call(processingRegistry());
  auto source = *profileOf({0, 1, 2}, {0, 0, 0}).asProfile();
  source.descriptor.sampleOrder = "unknown";
  call.inputs["clean"] = Data::profile(source);
  auto status = call.run("profile.split_faces", {{"minPoints", Value::integer(3)}});
  CHECK_FALSE(status.ok);
  CHECK(status.code == "bad_input");
  CHECK(status.message.find("acquisition") != std::string::npos);
  CHECK(call.outputs.empty());
  source.descriptor.sampleOrder = "acquisition";
  call.inputs["clean"] = Data::profile(source);
  status = call.run("profile.split_faces");
  CHECK_FALSE(status.ok);
  CHECK(status.code == "no_segments");
  CHECK(call.outputs.empty());
}

TEST_CASE(
    "Profile processing: cancellation at entry and during work never publishes partial output") {
  std::vector<float> x(100), z(100, 2);
  std::iota(x.begin(), x.end(), 0.f);
  const Data source = profileOf(x, z);
  for (const auto* id : {"profile.clean", "profile.split_faces"})
    for (int limit : {1, 5, 25}) {
      CAPTURE(std::string(id));
      CAPTURE(limit);
      const auto* op = processingRegistry().find(id);
      REQUIRE(op);
      ParamMap params;
      for (const auto& param : op->params)
        params[param.name] = param.def;
      const std::filesystem::path base;
      ParamView view(params, base);
      std::unordered_map<std::string, Data> values{{"profile", source}, {"clean", source}};
      std::unordered_map<std::string, Data> produced;
      Inputs inputs(values);
      Outputs outputs(produced);
      CountingContext context(limit);
      const auto status = op->compute(inputs, view, outputs, context);
      CHECK_FALSE(status.ok);
      CHECK(status.code == "cancelled");
      CHECK(produced.empty());
      CHECK(source.asProfile()->validity[0] == 1);
      CHECK(source.asProfile()->cloud->xyz[2] == 2);
    }
}
