#include "p3_example_helpers.h"

#include <map>

namespace {
using namespace lyflow;
using namespace lyflow::test;

void shape(const Json& outputs, bool changed, bool parallel) {
  p3::poseRecord(outputs.at("instances"));
  const auto& instances = p3::data(outputs, "instances").at("items");
  REQUIRE(instances.size() == (parallel ? 2 : 1));
  for (int branch = 0; branch < (parallel ? 2 : 1); ++branch) {
    const auto* qualityName = parallel ? (branch ? "qualityH" : "qualityL") : "quality";
    const auto* modelName = parallel ? (branch ? "modelH" : "modelL") : "model";
    const auto& model = p3::data(outputs, modelName);
    const auto& items = p3::data(outputs, qualityName).at("items");
    REQUIRE(items.size() == 1);
    const auto& quality = items[0];
    const auto modelId = std::string(branch ? "cad-H" : "cad-L") + (changed ? "-override" : "");
    CHECK(model.at("id") == modelId);
    CHECK(quality.at("modelId") == modelId);
    CHECK(quality.at("coverage").get<double>() >= .85);
    CHECK(quality.at("score").get<double>() >= .7);
    CHECK(quality.at("rmse").get<double>() <= 1.5);
    CHECK(quality.at("support").get<int>() >= 12);
    CHECK(quality.at("featureCount") == model.at("featureCount"));
    const auto found = std::find_if(instances.begin(), instances.end(), [&](const Json& pose) {
      return pose.at("id") == quality.at("id");
    });
    REQUIRE(found != instances.end());
    const auto& fixture = p3::truth().at("shape");
    p3::positionNear(model.at("center"),
                     fixture.at("projectionCenters").at(branch ? "cad-H" : "cad-L"), .1);
    auto expected = model.at("center");
    const auto& offset = parallel ? fixture.at("parallel")[branch].at("translationPixels")
                                  : fixture.at("single").at("translationPixels");
    for (int axis = 0; axis < 2; ++axis)
      expected[axis] = expected[axis].get<double>() + offset[axis].get<double>();
    p3::positionNear(found->at("position"), expected, fixture.at("positionTolerancePixels"));
    CHECK(found->at("angle").get<double>() == doctest::Approx(0).epsilon(1e-6));
    CHECK(found->at("pickable") == false);
    CHECK(found->at("frame") == "image");
  }
  if (!parallel) {
    CHECK(outputs.at("features").at("type") == "Tensor");
    CHECK(outputs.at("depth").at("value").at("depth") == "f32");
    CHECK(outputs.at("depth").at("value").at("min")[0] == 0);
    CHECK(outputs.at("depth").at("value").at("max")[0] == 1);
    CHECK(outputs.at("silhouette").at("value").at("min")[0] == 0);
    CHECK(outputs.at("silhouette").at("value").at("max")[0] == 255);
  }
}

void nativeFeatures(const Json& outputs, const std::string& runId, Client* client, bool parallel,
                    bool baseline, std::map<std::string, p3::TensorPayload>& expected) {
  for (int branch = 0; branch < (parallel ? 2 : 1); ++branch) {
    const std::string node = branch ? "template_h" : "template_l";
    CAPTURE(node);
    const auto& model = p3::data(outputs, parallel ? (branch ? "modelH" : "modelL") : "model");
    auto tensor = p3::tensorPayload(runId, node, "features", client);
    if (baseline) {
      const auto count = model.at("featureCount").get<std::uint32_t>();
      CHECK(tensor.shape == std::vector<std::int64_t>{count, 4});
      REQUIRE(tensor.values.size() == count * 4);
      for (std::size_t i = 0; i < tensor.values.size(); i += 4) {
        CHECK(std::isfinite(tensor.values[i]));
        CHECK(std::isfinite(tensor.values[i + 1]));
        CHECK(std::hypot(tensor.values[i + 2], tensor.values[i + 3]) ==
              doctest::Approx(1).epsilon(1e-5));
      }
      expected.emplace(node, std::move(tensor));
    } else {
      const auto& reference = expected.at(node);
      CHECK(tensor.shape == reference.shape);
      CHECK(tensor.values == reference.values);
    }
  }
}

void replay(bool parallel) {
  std::array<std::map<std::string, p3::TensorPayload>, 2> features;
  const Json overrides =
      parallel ? Json{{"leftModelId", "cad-L-override"}, {"rightModelId", "cad-H-override"}}
               : Json{{"modelId", "cad-L-override"}};
  p3::crossInterfaces(
      parallel ? "p3-shape-parallel" : "p3-shape-cad", overrides,
      [parallel](const Json& outputs, bool changed) { shape(outputs, changed, parallel); },
      [&](const Json& outputs, const std::string& runId, Client* client, bool changed,
          bool baseline) {
        nativeFeatures(outputs, runId, client, parallel, baseline, features[changed ? 1 : 0]);
      });
}
} // namespace

TEST_CASE("P3 CAD shape example rebuilds native features across saved workflow interfaces") {
  replay(false);
}
TEST_CASE(
    "P3 parallel CAD templates concatenate stable identities across interfaces and overrides") {
  replay(true);
}
