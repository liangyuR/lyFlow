#include "p3_example_helpers.h"

namespace {
using namespace lyflow;
using namespace lyflow::test;

void marker(const Json& outputs, bool changed, bool stag) {
  p3::poseRecord(outputs.at("cameraPoses"));
  p3::poseRecord(outputs.at("workspacePoses"));
  const auto& camera = p3::data(outputs, "cameraPoses").at("items");
  const auto& local = p3::data(outputs, "workspacePoses").at("items");
  REQUIRE(camera.size() == 1);
  REQUIRE(local.size() == 1);
  const auto& expected = p3::truth().at(stag ? "stag" : "marker");
  const double scale = changed ? .8 : 1;
  auto position = expected.at("cameraPositionM");
  for (auto& coordinate : position)
    coordinate = coordinate.get<double>() * scale;
  p3::positionNear(camera[0].at("position"), position, expected.at("translationToleranceM"));
  position[0] = position[0].get<double>() + .1;
  position[1] = position[1].get<double>() - .3;
  position[2] = position[2].get<double>() + .3;
  p3::positionNear(local[0].at("position"), position, expected.at("translationToleranceM"));
  CHECK(camera[0].at("classId") == (stag ? "0" : "7"));
  CHECK(camera[0].at("frame") == "camera");
  CHECK(local[0].at("frame") == "bin_frame");
  CHECK(local[0].at("id") == camera[0].at("id"));
  CHECK(local[0].at("rotation") == camera[0].at("rotation"));
  CHECK(camera[0].at("size")[0].get<double>() == doctest::Approx(.1 * scale));
  CHECK(camera[0].at("pickable") == false);
  const auto& detections = p3::data(outputs, "detections").at("items");
  REQUIRE(detections.size() == 1);
  CHECK(detections[0].at("accepted") == true);
  CHECK(detections[0].at("ambiguous") == false);
  CHECK(detections[0].at("decodedId") == (stag ? 0 : 7));
  CHECK(detections[0].at("rms").get<double>() < 2);
  CHECK(p3::data(outputs, "workspace").at("workspaceFrame") == "bin_frame");
}

void workspace(const Json& outputs, bool changed, bool legacy) {
  const auto& expected = p3::truth().at(legacy ? "workspaceLegacy" : "workspaceNative");
  const auto& center = expected.at(legacy ? (changed ? "cameraCenterM" : "worldCenterM")
                                          : (changed ? "worldCenterM" : "cameraCenterM"));
  for (const char* name : {"region", "localRegion", "roundtripRegion"})
    p3::poseRecord(outputs.at(name));
  const auto& original = p3::data(outputs, "region");
  const auto& local = p3::data(outputs, "localRegion").at("items");
  const auto& restored = p3::data(outputs, "roundtripRegion").at("items");
  REQUIRE(local.size() == 1);
  REQUIRE(restored.size() == 1);
  p3::positionNear(original.at("position"), center, 1e-5);
  p3::positionNear(local[0].at("position"), expected.at("localCenterM"), 1e-5);
  p3::positionNear(restored[0].at("position"), center, 1e-5);
  CHECK(original.at("size") == expected.at("sizeM"));
  CHECK(local[0].at("size") == expected.at("sizeM"));
  CHECK(restored[0].at("size") == expected.at("sizeM"));
  CHECK(original.at("id") == restored[0].at("id"));
  CHECK(original.at("frame") ==
        (legacy ? (changed ? "Camera7" : "World") : (changed ? "world" : "camera")));
  CHECK(p3::data(outputs, "workspace").at("nativeCornerError").get<double>() <= 1e-5);
  CHECK(p3::data(outputs, "workspace").at("format") ==
        (legacy ? "xyz-unversioned-yaml" : "lyflow-json-v1"));
}
} // namespace

TEST_CASE("P3 ArUco workspace example replays saved assets through scheduler C ABI and SDK") {
  p3::crossInterfaces("p3-marker-workspace", {{"markerSize", .08}},
                      [](const Json& result, bool changed) { marker(result, changed, false); });
}
TEST_CASE("P3 STag workspace example preserves decoded identity across interfaces and overrides") {
  p3::crossInterfaces("p3-stag-workspace", {{"markerSize", .08}},
                      [](const Json& result, bool changed) { marker(result, changed, true); });
}
TEST_CASE("P3 native workspace example persists frame selection and native roundtrip") {
  p3::crossInterfaces("p3-workspace-native", {{"targetFrame", "world"}},
                      [](const Json& result, bool changed) { workspace(result, changed, false); });
}
TEST_CASE("P3 explicit legacy workspace example preserves units and parent frame after reload") {
  p3::crossInterfaces("p3-workspace-legacy", {{"targetFrame", "Camera7"}},
                      [](const Json& result, bool changed) { workspace(result, changed, true); });
}
