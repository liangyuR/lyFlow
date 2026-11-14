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

struct RawTensor {
  lyflow_tensor_view view{};
  ~RawTensor() { lyflow_tensor_view_free(&view); }
};
void nativeFeatures(const Json& outputs, const std::string& runId, Client* client, bool parallel,
                    std::map<std::string, std::vector<float>>& previous) {
  for (int branch = 0; branch < (parallel ? 2 : 1); ++branch) {
    const std::string node = branch ? "template_h" : "template_l";
    const auto& model = p3::data(outputs, parallel ? (branch ? "modelH" : "modelL") : "model");
    const auto count = model.at("featureCount").get<std::uint32_t>();
    std::vector<float> values;
    if (client) {
      auto tensor = client->tensor(runId, node, "features");
      REQUIRE(tensor.valid());
      REQUIRE(tensor.rank() == 2);
      CHECK(tensor.shape()[0] == count);
      CHECK(tensor.shape()[1] == 4);
      REQUIRE(tensor.count() == count * 4);
      values.assign(tensor.data(), tensor.data() + tensor.count());
    } else {
      RawTensor tensor;
      REQUIRE(lyflow_output_tensor(runId.c_str(), node.c_str(), "features", 0, 0, &tensor.view) ==
              0);
      REQUIRE(tensor.view.rank == 2);
      CHECK(tensor.view.shape[0] == count);
      CHECK(tensor.view.shape[1] == 4);
      REQUIRE(tensor.view.count == count * 4);
      values.assign(tensor.view.data, tensor.view.data + tensor.view.count);
    }
    for (std::size_t i = 0; i < values.size(); i += 4) {
      CHECK(std::isfinite(values[i]));
      CHECK(std::isfinite(values[i + 1]));
      CHECK(std::hypot(values[i + 2], values[i + 3]) == doctest::Approx(1).epsilon(1e-5));
    }
    const auto key = model.at("id").get<std::string>();
    if (previous.count(key))
      CHECK(values == previous.at(key));
    else
      previous.emplace(key, std::move(values));
  }
}

void replay(bool parallel) {
  std::map<std::string, std::vector<float>> features;
  const Json overrides =
      parallel ? Json{{"leftModelId", "cad-L-override"}, {"rightModelId", "cad-H-override"}}
               : Json{{"modelId", "cad-L-override"}};
  p3::crossInterfaces(
      parallel ? "p3-shape-parallel" : "p3-shape-cad", overrides,
      [parallel](const Json& outputs, bool changed) { shape(outputs, changed, parallel); },
      [&](const Json& outputs, const std::string& runId, Client* client) {
        nativeFeatures(outputs, runId, client, parallel, features);
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
