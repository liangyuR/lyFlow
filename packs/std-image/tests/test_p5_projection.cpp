#include <doctest/doctest.h>
#include <fstream>

#include "helpers.h"
#include "lyflow/cloud_groups.h"
#include "lyflow/pose.h"
#include "../ops/p5_projection_common.h"

namespace lyflow::ops {
void registerP5Projection(Registry&);
}
namespace {
using namespace lyflow;
using namespace lyflow::test;
namespace pr = lyflow::ops::projection5;
using Json = nlohmann::json;
Registry projectionRegistry() {
  Registry result;
  ops::registerP5Projection(result);
  return result;
}
Data camera(int width = 9, int height = 9) {
  return Data::record({calibration::kCameraType,
                       {{"imageSize", {width, height}},
                        {"cameraMatrix", {100, 0, 4, 0, 100, 4, 0, 0, 1}},
                        {"distortion", Json::array()},
                        {"frame", "camera"},
                        {"unit", "m"}}});
}
Data rgb(int width = 9, int height = 9, int scale = 1) {
  auto result = Image::allocate(width, height, 4, PixelDepth::U8);
  result.scale = scale;
  for (std::size_t i = 0; i < result.pixelCount(); ++i) {
    result.mutablePixels()[4 * i] = 10;
    result.mutablePixels()[4 * i + 1] = 20;
    result.mutablePixels()[4 * i + 2] = 30;
    result.mutablePixels()[4 * i + 3] = 37;
  }
  return Data::image(std::move(result));
}
Data depth(int width, int height, float value) {
  auto result = Image::allocate(width, height, 1, PixelDepth::F32);
  auto* data = reinterpret_cast<float*>(result.mutablePixels());
  std::fill(data, data + result.pixelCount(), value);
  return Data::image(std::move(result));
}
PointCloud sampleCloud() {
  PointCloud cloud;
  cloud.push(-.008f, .003f, 1);
  cloud.push(-.016f, .006f, 2);
  cloud.push(0, 0, -1);
  cloud.push(100, 0, 1);
  cloud.intensity = {11, 22, 33, 44};
  cloud.rgb = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
  cloud.normals = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
  return cloud;
}
void connectMap(OpCall& target, const OpCall& source) {
  for (const auto* name : {"cloud", "depth", "status", "indexMap", "mapping"})
    target.inputs[name] = source.outputs.at(name);
}
void checkMap(const OpCall& call) {
  StubContext context;
  pr::Grid grid;
  auto status =
      pr::parse(*call.outputs.at("cloud").asCloud(), *call.outputs.at("depth").asImage(),
                *call.outputs.at("status").asImage(), *call.outputs.at("indexMap").asTensor(),
                *call.outputs.at("mapping").asRecord(), grid, context);
  CHECK_MESSAGE(status.ok, status.message);
}

TEST_CASE(
    "P5 calibrated camera projection separates pixel and 3D frames and preserves source association") {
  auto registry = projectionRegistry();
  OpCall call(registry);
  call.inputs = {{"cloud", Data::cloud(sampleCloud())}, {"image", rgb()}, {"calib", camera()}};
  const auto parent = call.inputs["cloud"].asCloud()->id;
  REQUIRE(call.run("cloud.project_camera", {{"colorSource", Value::text("cloud")}}).ok);
  checkMap(call);
  const auto* cloud = call.out("cloud").asCloud();
  REQUIRE(cloud);
  REQUIRE(cloud->pointCount() == 1);
  CHECK(cloud->xyz == std::vector<float>{-.008f, .003f, 1});
  CHECK(cloud->intensity == std::vector<float>{11});
  CHECK(cloud->rgb == std::vector<std::uint8_t>{1, 2, 3});
  CHECK(cloud->normals == std::vector<float>{0, 0, 1});
  CHECK(call.out("pointState").asTensor()->data == std::vector<float>{1, 2, 0, 3});
  const auto pixel = 4 * 9 + 3;
  CHECK(call.out("sourceIndexMap").asTensor()->data[pixel] == 0);
  CHECK(call.out("indexMap").asTensor()->data[pixel] == 0);
  CHECK(call.out("status").asImage()->at(3, 4, 0) == 9);
  const auto& mapping = call.out("mapping").asRecord()->data;
  CHECK(mapping["imageFrame"] == "image");
  CHECK(mapping["gridFrame"] == "camera");
  CHECK(mapping["cloudFrame"] == "camera");
  CHECK(mapping["parentCloudId"] == std::to_string(parent));
  CHECK(call.out("image").asImage()->pixels == call.inputs["image"].asImage()->pixels);
  CHECK(call.out("image").asImage()->at(3, 4, 3) == 37);
  CHECK_FALSE(call.run("cloud.project_camera", {{"cloudFrame", Value::text("world")}}).ok);
  CHECK(call.outputs.empty());
  call.inputs["cloudToCamera"] = Data::transform(Transform{});
  CHECK(call.run("cloud.project_camera", {{"cloudFrame", Value::text("world")}}).ok);
  call.inputs.erase("cloudToCamera");
  for (auto pixelDepth : {PixelDepth::U16, PixelDepth::F32}) {
    auto color = Image::allocate(9, 9, 4, pixelDepth);
    for (std::size_t i = 0; i < color.pixelCount(); ++i) {
      if (pixelDepth == PixelDepth::U16) {
        auto* bytes = reinterpret_cast<std::uint16_t*>(color.mutablePixels());
        bytes[4 * i] = 65535;
        bytes[4 * i + 1] = 32768;
        bytes[4 * i + 2] = 0;
        bytes[4 * i + 3] = 12345;
      } else {
        auto* values = reinterpret_cast<float*>(color.mutablePixels());
        values[4 * i] = 1;
        values[4 * i + 1] = .5f;
        values[4 * i + 2] = 0;
        values[4 * i + 3] = .25f;
      }
    }
    call.inputs["image"] = Data::image(color);
    REQUIRE(call.run("cloud.project_camera").ok);
    CHECK(call.out("cloud").asCloud()->rgb == std::vector<std::uint8_t>{255, 128, 0});
    CHECK(call.out("image").asImage()->pixels == color.pixels);
    CHECK(call.out("image").asImage()->at(0, 0, 3) ==
          (pixelDepth == PixelDepth::U16 ? 12345 : .25));
    if (pixelDepth == PixelDepth::F32) {
      reinterpret_cast<float*>(color.mutablePixels())[3] = std::numeric_limits<float>::quiet_NaN();
      CHECK_FALSE(call.run("cloud.project_camera").ok);
      CHECK(call.outputs.empty());
    }
  }
}

TEST_CASE(
    "P5 signed orthographic heights retain zero negative max winners and measured normal angles") {
  auto registry = projectionRegistry();
  PointCloud cloud;
  for (int y = 0; y < 5; ++y)
    for (int x = 0; x < 5; ++x) {
      cloud.push(.01f * x, .01f * y, -.002f + .001f * x);
      cloud.intensity.push_back(static_cast<float>(x + 5 * y));
    }
  OpCall call(registry);
  call.inputs["cloud"] = Data::cloud(std::move(cloud));
  const ParamMap params = {{"width", Value::integer(5)},
                           {"height", Value::integer(5)},
                           {"resolution", Value::number(.01)},
                           {"normalResolution", Value::number(.01)},
                           {"originX", Value::number(0)},
                           {"originY", Value::number(0)},
                           {"outputMode", Value::text("normal_angle")}};
  REQUIRE(call.run("cloud.orthographic_map", params).ok);
  checkMap(call);
  CHECK(call.out("cloud").asCloud()->pointCount() == 25);
  CHECK(call.out("depth").asImage()->at(0, 0, 0) == doctest::Approx(-.002));
  CHECK(call.out("depth").asImage()->at(2, 2, 0) == doctest::Approx(0).epsilon(.001));
  CHECK(call.out("status").asImage()->at(2, 2, 0) == 1);
  CHECK(call.out("normalValid").asImage()->at(2, 2, 0) == 255);
  CHECK(call.out("normalAngle").asImage()->at(2, 2, 0) ==
        doctest::Approx(std::atan(.1)).epsilon(1e-4));
  auto elevated = *call.inputs["cloud"].asCloud();
  elevated.push(.02f, .02f, .03f);
  elevated.intensity.push_back(99);
  call.inputs["cloud"] = Data::cloud(std::move(elevated));
  REQUIRE(call.run("cloud.orthographic_map", params).ok);
  CHECK(call.out("depth").asImage()->at(2, 2, 0) == doctest::Approx(.03));
  CHECK(call.out("sourceIndexMap").asTensor()->data[12] == 25);
  CHECK(call.out("cloud").asCloud()->intensity[12] == 99);
  auto imageParams = params;
  imageParams["outputMode"] = Value::text("height");
  REQUIRE(call.run("cloud.orthographic_map", imageParams).ok);
  CHECK(call.out("image").asImage()->depth == PixelDepth::F32);
  CHECK(call.out("image").asImage()->at(2, 2, 0) == doctest::Approx(.03));
}

TEST_CASE("P5 depth no-op preserves off-centre measured XYZ and all channels while compacting") {
  auto registry = projectionRegistry();
  OpCall project(registry);
  project.inputs = {{"cloud", Data::cloud(sampleCloud())}, {"image", rgb()}, {"calib", camera()}};
  REQUIRE(project.run("cloud.project_camera", {{"colorSource", Value::text("cloud")}}).ok);
  OpCall filter(registry);
  connectMap(filter, project);
  REQUIRE(filter.run("image.depth_filter", {{"kernelSize", Value::integer(1)}}).ok);
  checkMap(filter);
  const auto* result = filter.out("cloud").asCloud();
  const auto* original = project.out("cloud").asCloud();
  CHECK(result->id != original->id);
  CHECK(result->xyz == original->xyz);
  CHECK(result->normals == original->normals);
  CHECK(result->intensity == original->intensity);
  CHECK(result->rgb == original->rgb);
  CHECK(filter.out("quality").asRecord()->data["changedPixels"] == 0);
}

TEST_CASE("P5 depth filters implement normalized mean median Gaussian and edge preservation") {
  auto registry = projectionRegistry();
  for (const auto* mode : {"mean", "median", "gaussian"}) {
    OpCall call(registry);
    auto source = depth(9, 9, 1);
    auto edited = *source.asImage();
    reinterpret_cast<float*>(edited.mutablePixels())[40] = 2;
    call.inputs = {{"depth", Data::image(edited)}, {"calib", camera()}};
    REQUIRE(call.run("image.depth_filter",
                     {{"mode", Value::text(mode)}, {"maxDepthJump", Value::number(0)}})
                .ok);
    checkMap(call);
    const auto center = call.out("depth").asImage()->at(4, 4, 0);
    if (std::string(mode) == "mean")
      CHECK(center == doctest::Approx(10.0 / 9));
    if (std::string(mode) == "median")
      CHECK(center == 1);
    if (std::string(mode) == "gaussian") {
      CHECK(center > 1);
      CHECK(center < 1.5);
    }
    CHECK(call.out("status").asImage()->at(4, 4, 0) == 5);
    CHECK(edited.at(4, 4, 0) == 2);
    REQUIRE(call.run("image.depth_filter",
                     {{"mode", Value::text(mode)}, {"maxDepthJump", Value::number(.01)}})
                .ok);
    CHECK(call.out("depth").asImage()->at(4, 4, 0) == 2);
  }
}

TEST_CASE(
    "P5 invalid fill modes distinguish estimates and remove protrusions depressions without old points") {
  auto registry = projectionRegistry();
  for (const auto* method : {"idw", "min", "max"}) {
    OpCall call(registry);
    auto source = *depth(9, 9, 1).asImage();
    reinterpret_cast<float*>(source.mutablePixels())[40] = 0;
    call.inputs = {{"depth", Data::image(source)}, {"calib", camera()}};
    REQUIRE(call.run("image.depth_filter",
                     {{"mode", Value::text("fill")}, {"fillMethod", Value::text(method)}})
                .ok);
    checkMap(call);
    CHECK(call.out("cloud").asCloud()->pointCount() == 81);
    CHECK(call.out("depth").asImage()->at(4, 4, 0) == 1);
    CHECK(call.out("status").asImage()->at(4, 4, 0) == 2);
    CHECK(call.out("sourceIndexMap").asTensor()->data[40] == -1);
    CHECK(call.out("quality").asRecord()->data["filledPixels"] == 1);
  }
  for (const auto* type : {"protrusion", "depression", "both"}) {
    OpCall call(registry);
    auto source = *depth(9, 9, 1).asImage();
    reinterpret_cast<float*>(source.mutablePixels())[20] = 2;
    reinterpret_cast<float*>(source.mutablePixels())[60] = .5;
    call.inputs = {{"depth", Data::image(source)}, {"calib", camera()}};
    REQUIRE(call.run("image.depth_filter", {{"mode", Value::text("eliminate")},
                                            {"eliminateType", Value::text(type)},
                                            {"threshold", Value::number(.1)}})
                .ok);
    checkMap(call);
    const auto removed = std::string(type) == "both" ? 2 : 1;
    CHECK(call.out("cloud").asCloud()->pointCount() == 81 - removed);
    CHECK(call.out("quality").asRecord()->data["removedPixels"] == removed);
  }
}

TEST_CASE("P5 ROI consumes P4 native masks and stable groups with explicit empty results") {
  auto registry = projectionRegistry();
  PointCloud source;
  for (int y = 0; y < 9; ++y)
    for (int x = 0; x < 9; ++x) {
      source.push((x - 4) * .01f, (y - 4) * .01f, 1);
      source.intensity.push_back(static_cast<float>(y * 9 + x));
    }
  OpCall project(registry);
  project.inputs = {
      {"cloud", Data::cloud(std::move(source))}, {"image", rgb()}, {"calib", camera()}};
  REQUIRE(project.run("cloud.project_camera").ok);
  auto pose = poses::make2D("cross", "image", "px", {4, 4}, 0, {7, 7});
  pose.data["attributes"] = {{"hasMask", true}};
  auto instances = poses::make2DList(Json::array({pose.data}), "image", "px");
  Tensor mask;
  mask.shape = {1, 9, 9};
  mask.data.assign(81, 0);
  for (int y = 0; y < 9; ++y)
    for (int x = 0; x < 9; ++x)
      if (x >= 1 && x <= 7 && y >= 1 && y <= 7 && (x == 4 || y == 4) && !(x == 4 && y == 4))
        mask.data[static_cast<std::size_t>(y) * 9 + x] = 1;
  Record info{
      "vision.Masks2D.v1",
      {{"frame", "image"},
       {"unit", "px"},
       {"width", 9},
       {"height", 9},
       {"layout", "NHW"},
       {"items",
        Json::array(
            {{{"id", "cross"}, {"index", 0}, {"hasMask", true}, {"source", "test_fixture"}}})}}};
  OpCall call(registry);
  connectMap(call, project);
  call.inputs["instances"] = Data::record(instances);
  call.inputs["masks"] = Data::tensor(mask);
  call.inputs["maskInfo"] = Data::record(info);
  REQUIRE(call.run("cloud.roi_extract").ok);
  checkMap(call);
  CHECK(call.out("cloud").asCloud()->pointCount() == 12);
  CHECK(call.out("indexMap").asTensor()->data[40] == -1);
  CHECK(call.out("groups").asRecord()->data["groups"][0]["id"] == "cross");
  CHECK(call.out("indices").asIndices()->sourceCloudId == call.out("cloud").asCloud()->id);
  REQUIRE(call.run("cloud.roi_extract", {{"usingMask", Value::boolean(false)}}).ok);
  CHECK(call.out("cloud").asCloud()->pointCount() == 49);
  REQUIRE(call.run("cloud.roi_extract", {{"shrink", Value::number(.5)}}).ok);
  CHECK(call.out("cloud").asCloud()->pointCount() == 4);
  auto badInfo = info;
  badInfo.data["items"][0]["hasMask"] = false;
  badInfo.data["items"][0]["source"] = "none";
  call.inputs["maskInfo"] = Data::record(badInfo);
  CHECK_FALSE(call.run("cloud.roi_extract").ok);
  CHECK(call.outputs.empty());
  call.inputs["maskInfo"] = Data::record(info);
  call.inputs["masks"] = Data::tensor(mask).withTensorScale(2);
  CHECK_FALSE(call.run("cloud.roi_extract").ok);
  CHECK(call.outputs.empty());
  auto zero = mask;
  std::fill(zero.data.begin(), zero.data.end(), 0);
  call.inputs["masks"] = Data::tensor(zero);
  CHECK_FALSE(call.run("cloud.roi_extract").ok);
  CHECK(call.outputs.empty());
  auto narrow = mask;
  narrow.shape = {1, 1, 1};
  narrow.data = {1};
  badInfo = info;
  badInfo.data["width"] = 1;
  badInfo.data["height"] = 1;
  call.inputs["masks"] = Data::tensor(narrow);
  call.inputs["maskInfo"] = Data::record(badInfo);
  CHECK_FALSE(call.run("cloud.roi_extract").ok);
  CHECK(call.outputs.empty());
  auto absentPose = pose;
  absentPose.data["attributes"]["hasMask"] = false;
  badInfo = info;
  badInfo.data["items"][0]["hasMask"] = false;
  badInfo.data["items"][0]["source"] = "none";
  call.inputs["instances"] =
      Data::record(poses::make2DList(Json::array({absentPose.data}), "image", "px"));
  call.inputs["masks"] = Data::tensor(zero);
  call.inputs["maskInfo"] = Data::record(badInfo);
  REQUIRE(call.run("cloud.roi_extract").ok);
  CHECK(call.out("cloud").asCloud()->pointCount() == 0);
  CHECK(call.out("quality").asRecord()->data["missingMaskRois"] == Json::array({"cross"}));
  call.inputs.erase("masks");
  call.inputs.erase("maskInfo");
  call.inputs["instances"] = Data::record(poses::make2DList(Json::array(), "image", "px"));
  REQUIRE(call.run("cloud.roi_extract").ok);
  checkMap(call);
  CHECK(call.out("cloud").asCloud()->pointCount() == 0);
  pose = poses::make2D("diagonal", "image", "px", {4, 4}, poses::kPi / 4, {6, 2});
  call.inputs["instances"] =
      Data::record(poses::make2DList(Json::array({pose.data}), "image", "px"));
  REQUIRE(call.run("cloud.roi_extract", {{"usingMask", Value::boolean(false)}}).ok);
  const auto& kept = call.out("sourceIndices").asIndices()->values;
  CHECK(std::find(kept.begin(), kept.end(), 5 * 9 + 5) != kept.end());
  CHECK(std::find(kept.begin(), kept.end(), 6 * 9 + 6) != kept.end());
  CHECK(std::find(kept.begin(), kept.end(), 7 * 9 + 7) == kept.end());
  CHECK(std::find(kept.begin(), kept.end(), 5 * 9 + 3) == kept.end());
  call.inputs["masks"] = Data::tensor(mask);
  call.inputs["maskInfo"] = Data::record(info);
  CHECK_FALSE(call.run("cloud.roi_extract").ok);
  CHECK(call.outputs.empty());
  call.inputs.erase("masks");
  call.inputs.erase("maskInfo");
  pose = poses::make2D("tiny", "image", "px", {0, 0}, poses::kPi / 4, {1e-308, 1e-308});
  call.inputs["instances"] =
      Data::record(poses::make2DList(Json::array({pose.data}), "image", "px"));
  REQUIRE(call.run("cloud.roi_extract", {{"usingMask", Value::boolean(false)}}).ok);
  CHECK(call.out("cloud").asCloud()->pointCount() == 1);
}

TEST_CASE("P5 unorganized ROI distinguishes visible from occluded and validates mask bindings") {
  auto registry = projectionRegistry();
  OpCall call(registry);
  auto mask = Image::allocate(9, 9, 1, PixelDepth::U8);
  std::fill(mask.mutablePixels(), mask.mutablePixels() + 81, 255);
  call.inputs = {
      {"cloud", Data::cloud(sampleCloud())}, {"calib", camera()}, {"mask", Data::image(mask)}};
  REQUIRE(call.run("cloud.roi_extract").ok);
  CHECK(call.out("cloud").asCloud()->pointCount() == 1);
  checkMap(call);
  REQUIRE(call.run("cloud.roi_extract", {{"visibility", Value::text("all")}}).ok);
  CHECK(call.out("cloud").asCloud()->pointCount() == 2);
  checkMap(call);
  call.inputs.erase("mask");
  CHECK_FALSE(call.run("cloud.roi_extract").ok);
  CHECK(call.outputs.empty());
}

TEST_CASE("P5 geometry contracts reject bad input and budgets atomically") {
  auto registry = projectionRegistry();
  OpCall call(registry);
  call.inputs["cloud"] = Data::cloud(sampleCloud());
  CHECK_FALSE(call.run("cloud.height_map",
                       {{"width", Value::integer(32768)}, {"height", Value::integer(32768)}})
                  .ok);
  CHECK(call.outputs.empty());
  call.inputs["image"] = rgb();
  call.inputs["calib"] = camera();
  REQUIRE(call.run("cloud.project_camera").ok);
  OpCall bad(registry);
  connectMap(bad, call);
  auto record = *bad.inputs["mapping"].asRecord();
  record.data["sourceCloudId"] = "wrong";
  bad.inputs["mapping"] = Data::record(record);
  CHECK_FALSE(bad.run("image.depth_filter").ok);
  CHECK(bad.outputs.empty());
  connectMap(bad, call);
  auto index = *bad.inputs["indexMap"].asTensor();
  index.data[0] = std::numeric_limits<float>::quiet_NaN();
  bad.inputs["indexMap"] = Data::tensor(std::move(index));
  CHECK_FALSE(bad.run("image.depth_filter").ok);
  CHECK(bad.outputs.empty());
  pr::Grid grid;
  grid.width = grid.height = grid.fullWidth = grid.fullHeight = 1;
  grid.gridFrame = "workspace";
  grid.cloudFrame = "world";
  grid.imageFrame = "grid";
  grid.projection = "orthographic";
  grid.gridToCloud(0, 3) = 1e6;
  grid.gridToCloud(1, 3) = -1e6;
  cv::Vec3d value;
  REQUIRE(pr::backProject(grid, 0, 0, -.2, value));
  CHECK(value[0] == 1e6);
  CHECK(value[1] == -1e6);
  CHECK(value[2] == -.2);
  auto inverse = pr::inverse(grid.gridToCloud);
  auto recovered = inverse * cv::Vec4d(value[0], value[1], value[2], 1);
  CHECK(recovered[0] == 0);
  CHECK(recovered[1] == 0);
  CHECK(recovered[2] == -.2);
}

PointCloud fixtureCloud(const std::filesystem::path& path) {
  std::ifstream stream(path);
  REQUIRE(stream.good());
  std::string line;
  while (std::getline(stream, line) && line.find("DATA ascii") == std::string::npos) {
  }
  REQUIRE(stream.good());
  PointCloud result;
  float x, y, z, intensity, nx, ny, nz;
  std::uint32_t color;
  while (stream >> x >> y >> z >> intensity >> nx >> ny >> nz >> color) {
    result.push(x, y, z);
    result.intensity.push_back(intensity);
    result.normals.insert(result.normals.end(), {nx, ny, nz});
    result.rgb.insert(result.rgb.end(),
                      {static_cast<std::uint8_t>(color >> 16),
                       static_cast<std::uint8_t>(color >> 8), static_cast<std::uint8_t>(color)});
  }
  return result;
}
TEST_CASE(
    "P5 repository PCD truth retains every selected optional channel and real occlusion index") {
  auto registry = projectionRegistry();
  const auto directory = std::filesystem::path(LYFLOW_EXAMPLES_DIR);
  PointCloud source = fixtureCloud(directory / "p5-scene.pcd");
  REQUIRE(source.pointCount() == 3066);
  std::ifstream file(directory / "p5-camera.json");
  Json calibration;
  file >> calibration;
  OpCall call(registry);
  call.inputs = {
      {"cloud", Data::cloud(std::move(source))},
      {"image", rgb(64, 48)},
      {"calib", Data::record({calibration["type"].get<std::string>(), calibration["data"]})}};
  REQUIRE(call.run("cloud.project_camera", {{"colorSource", Value::text("cloud")}}).ok);
  checkMap(call);
  const auto& input = *call.inputs["cloud"].asCloud();
  const auto& output = *call.out("cloud").asCloud();
  REQUIRE(output.pointCount() == 3063);
  const auto& original = call.out("sourceIndexMap").asTensor()->data;
  const auto& index = call.out("indexMap").asTensor()->data;
  std::size_t seen = 0;
  for (std::size_t i = 0; i < index.size(); ++i) {
    if (index[i] < 0) {
      CHECK(original[i] == -1);
      continue;
    }
    const auto current = static_cast<std::size_t>(index[i]),
               parent = static_cast<std::size_t>(original[i]);
    REQUIRE(parent < input.pointCount());
    CHECK(output.intensity[current] == input.intensity[parent]);
    for (int axis = 0; axis < 3; ++axis) {
      CHECK(output.xyz[3 * current + axis] == input.xyz[3 * parent + axis]);
      CHECK(output.normals[3 * current + axis] == input.normals[3 * parent + axis]);
      CHECK(output.rgb[3 * current + axis] == input.rgb[3 * parent + axis]);
    }
    ++seen;
  }
  CHECK(seen == 3063);
  const auto& states = call.out("pointState").asTensor()->data;
  CHECK(states[3063] == 2);
  CHECK(states[3064] == 3);
  CHECK(states[3065] == 0);
  OpCall filter(registry);
  connectMap(filter, call);
  REQUIRE(filter.run("image.depth_filter", {{"kernelSize", Value::integer(1)}}).ok);
  CHECK(filter.out("cloud").asCloud()->xyz == output.xyz);
  CHECK(filter.out("cloud").asCloud()->intensity == output.intensity);
  CHECK(filter.out("cloud").asCloud()->normals == output.normals);
  CHECK(filter.out("cloud").asCloud()->rgb == output.rgb);
  auto pose = poses::make2D("patch-up", "image", "px", {47.5, 33}, 0, {7, 6});
  pose.data["attributes"] = {{"hasMask", true}, {"maskIndex", 0}};
  Tensor mask;
  mask.shape = {1, 48, 64};
  mask.data.assign(48 * 64, 0.f);
  for (int y = 30; y < 36; ++y)
    for (int x = 44; x < 51; ++x)
      mask.data[y * 64 + x] = 1;
  Record info{"vision.Masks2D.v1",
              {{"frame", "image"},
               {"unit", "px"},
               {"width", 64},
               {"height", 48},
               {"layout", "NHW"},
               {"items", Json::array({{{"id", "patch-up"},
                                       {"index", 0},
                                       {"hasMask", true},
                                       {"source", "annotation_polygon"}}})}}};
  OpCall roi(registry);
  connectMap(roi, call);
  roi.inputs["instances"] =
      Data::record(poses::make2DList(Json::array({pose.data}), "image", "px"));
  roi.inputs["masks"] = Data::tensor(mask);
  roi.inputs["maskInfo"] = Data::record(info);
  REQUIRE(roi.run("cloud.roi_extract").ok);
  CHECK(roi.out("cloud").asCloud()->pointCount() == 42);
  const auto& kept = roi.out("sourceIndices").asIndices()->values;
  for (int x = 44; x < 51; ++x)
    CHECK(std::find(kept.begin(), kept.end(), static_cast<int>(index[30 * 64 + x])) != kept.end());
}

TEST_CASE("P5 camera distortion families rigid normals and scale-two grids round trip physically") {
  auto registry = projectionRegistry();
  for (int count : {0, 4, 5, 8, 12, 14}) {
    std::vector<double> distortion(static_cast<std::size_t>(count), 0);
    if (count) {
      distortion[0] = .1;
      distortion[1] = -.005;
      distortion[2] = .001;
      distortion[3] = -.001;
    }
    if (count >= 8) {
      distortion[5] = .002;
      distortion[6] = -.001;
    }
    if (count >= 12) {
      distortion[8] = .0002;
      distortion[10] = -.0002;
    }
    if (count == 14) {
      distortion[12] = .01;
      distortion[13] = -.008;
    }
    Json data = {{"imageSize", {64, 48}},
                 {"cameraMatrix", {60, 0, 31.5, 0, 60, 23.5, 0, 0, 1}},
                 {"distortion", distortion},
                 {"frame", "camera"},
                 {"unit", "m"}};
    PointCloud source;
    source.push(.3f, .15f, 1.2f);
    OpCall call(registry);
    call.inputs = {{"cloud", Data::cloud(std::move(source))},
                   {"image", rgb(32, 24, 2)},
                   {"calib", Data::record({calibration::kCameraType, data})}};
    REQUIRE(call.run("cloud.project_camera").ok);
    checkMap(call);
    CHECK(call.out("depth").asImage()->width == 32);
    CHECK(call.out("depth").asImage()->scale == 2);
    CHECK(call.out("indexMap").tensorScale() == 2);
    pr::Grid grid;
    StubContext context;
    REQUIRE(pr::parse(*call.out("cloud").asCloud(), *call.out("depth").asImage(),
                      *call.out("status").asImage(), *call.out("indexMap").asTensor(),
                      *call.out("mapping").asRecord(), grid, context)
                .ok);
    std::vector<cv::Point3d> points{{.3f, .15f, 1.2f}};
    std::vector<cv::Point2d> pixels;
    cv::projectPoints(points, cv::Vec3d::all(0), cv::Vec3d::all(0),
                      cv::Matx33d(grid.camera.matrix.data()), cv::Mat(distortion), pixels);
    cv::Vec3d recovered;
    REQUIRE(pr::backProjectFull(grid, pixels[0].x, pixels[0].y, 1.2f, recovered));
    CHECK(cv::norm(recovered - cv::Vec3d(.3f, .15f, 1.2f)) < 3e-6);
  }
  PointCloud cloud;
  cloud.push(.02f, .04f, 1);
  cloud.normals = {1, 0, 0};
  Transform toCamera;
  toCamera.m[0] = 0;
  toCamera.m[1] = -1;
  toCamera.m[4] = 1;
  toCamera.m[5] = 0;
  toCamera.m[3] = .01f;
  OpCall call(registry);
  call.inputs = {{"cloud", Data::cloud(std::move(cloud))},
                 {"image", rgb()},
                 {"calib", camera()},
                 {"cloudToCamera", Data::transform(toCamera)}};
  REQUIRE(call.run("cloud.project_camera", {{"cloudFrame", Value::text("world")}}).ok);
  CHECK(call.out("cloud").asCloud()->xyz[0] == doctest::Approx(-.03));
  CHECK(call.out("cloud").asCloud()->normals == std::vector<float>{0, 1, 0});
  CHECK(call.out("mapping").asRecord()->data["cloudFrame"] == "camera");
}

struct CancelProjection : ExecContext {
  std::filesystem::path directory;
  bool cancelled() const override { return true; }
  void progress(float, std::string_view) override {}
  void log(LogLevel, std::string) override {}
  const std::filesystem::path& baseDir() const override { return directory; }
  int threadBudget() const override { return 1; }
};
TEST_CASE("P5 cancelled hash and compute fail atomically without upstream mutation") {
  auto registry = projectionRegistry();
  OpCall call(registry);
  call.inputs = {{"cloud", Data::cloud(sampleCloud())}, {"image", rgb()}, {"calib", camera()}};
  REQUIRE(call.run("cloud.project_camera").ok);
  auto metadata = *call.out("mapping").asRecord();
  auto indices = *call.out("indexMap").asTensor();
  StubContext normal;
  pr::Grid grid;
  CancelProjection cancelled;
  auto status = pr::parse(*call.out("cloud").asCloud(), *call.out("depth").asImage(),
                          *call.out("status").asImage(), indices, metadata, grid, cancelled);
  CHECK_FALSE(status.ok);
  CHECK(status.code == "cancelled");
  const auto* op = registry.find("cloud.project_camera");
  REQUIRE(op);
  ParamMap params;
  for (const auto& item : op->params)
    params[item.name] = item.def;
  const std::filesystem::path base;
  ParamView p(params, base);
  Inputs in(call.inputs);
  std::unordered_map<std::string, Data> outputs;
  Outputs out(outputs);
  status = op->compute(in, p, out, cancelled);
  CHECK_FALSE(status.ok);
  CHECK(status.code == "cancelled");
  CHECK(outputs.empty());
  auto wrongDepth = *call.out("depth").asImage();
  auto copied = Image::allocate(9, 9, 1, PixelDepth::F32);
  std::memcpy(copied.mutablePixels(), wrongDepth.pixels.get(), wrongDepth.byteSize());
  reinterpret_cast<float*>(copied.mutablePixels())[39] = 2;
  metadata = pr::rebind(metadata, *call.out("cloud").asCloud(), copied,
                        *call.out("status").asImage(), indices, normal);
  status = pr::parse(*call.out("cloud").asCloud(), copied, *call.out("status").asImage(), indices,
                     metadata, grid, normal);
  CHECK_FALSE(status.ok);
}

TEST_CASE(
    "P5 neighborhood budgets reject dense excessive work but preserve a genuinely empty grid") {
  auto registry = projectionRegistry();
  OpCall call(registry);
  call.inputs = {{"depth", depth(600, 600, 1)}, {"calib", camera(600, 600)}};
  auto status = call.run("image.depth_filter", {{"kernelSize", Value::integer(31)}});
  CHECK_FALSE(status.ok);
  CHECK(status.code == "resource_limit");
  CHECK(call.outputs.empty());
  call.inputs["depth"] = depth(600, 600, 0);
  status = call.run("image.depth_filter", {{"kernelSize", Value::integer(31)},
                                           {"mode", Value::text("fill")},
                                           {"iterations", Value::integer(10)}});
  REQUIRE(status.ok);
  CHECK(call.out("cloud").asCloud()->pointCount() == 0);
  checkMap(call);
}
} // namespace

TEST_CASE(
    "P5 inverse distortion converges with shared budgets and odd preview blocks preserve coverage") {
  auto registry = projectionRegistry();
  OpCall filter(registry);
  auto calib = camera(3, 1);
  auto record = *calib.asRecord();
  record.data["cameraMatrix"] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  record.data["distortion"] = {1, 0, 0, 0, 0};
  auto input = depth(3, 1, 0);
  auto raw = *input.asImage();
  reinterpret_cast<float*>(raw.mutablePixels())[2] = 1;
  filter.inputs = {{"depth", Data::image(raw)}, {"calib", Data::record(record)}};
  REQUIRE(filter.run("image.depth_filter", {{"kernelSize", Value::integer(1)}}).ok);
  REQUIRE(filter.out("cloud").asCloud()->pointCount() == 1);
  CHECK(std::abs(filter.out("cloud").asCloud()->xyz[0] - 1) < 1e-6);
  checkMap(filter);
  calibration::Camera parsed;
  std::string why;
  REQUIRE(calibration::parseCamera(record, parsed, why));
  cv::Vec3d ray;
  pr::RayBudget budget;
  budget.maximumEvaluations = 0;
  CHECK_FALSE(pr::normalizedRay(parsed, 2, 0, ray, &budget));
  CHECK(budget.exhausted);
  budget = {};
  REQUIRE(pr::normalizedRay(parsed, 2, 0, ray, &budget));
  CHECK(budget.evaluations > 1);
  CHECK(std::abs(ray[0] - 1) < 1e-7);
  record.data["distortion"] = {-1, 0, 0, 0, 0};
  filter.inputs["calib"] = Data::record(record);
  CHECK_FALSE(filter.run("image.depth_filter", {{"kernelSize", Value::integer(1)}}).ok);
  CHECK(filter.outputs.empty());
  PointCloud corner;
  corner.push(.04f, .04f, 1);
  OpCall project(registry);
  project.inputs = {{"cloud", Data::cloud(corner)}, {"image", rgb(5, 5, 2)}, {"calib", camera()}};
  REQUIRE(project.run("cloud.project_camera").ok);
  auto mask = Image::allocate(9, 9, 1, PixelDepth::U8);
  std::fill(mask.mutablePixels(), mask.mutablePixels() + 81, 255);
  OpCall roi(registry);
  connectMap(roi, project);
  roi.inputs["mask"] = Data::image(mask);
  REQUIRE(roi.run("cloud.roi_extract").ok);
  CHECK(roi.out("cloud").asCloud()->pointCount() == 1);
  StubContext ctx;
  pr::Grid grid;
  REQUIRE(pr::parse(*project.out("cloud").asCloud(), *project.out("depth").asImage(),
                    *project.out("status").asImage(), *project.out("indexMap").asTensor(),
                    *project.out("mapping").asRecord(), grid, ctx)
              .ok);
  REQUIRE(pr::backProject(grid, 4, 4, 1, ray));
  CHECK(std::abs(ray[0] - .04) < 1e-12);
  CHECK(std::abs(ray[1] - .04) < 1e-12);
  CHECK(pr::fullBoundaryX(grid, 4.5) == 8.5);
  CHECK(pr::fullBoundaryX(grid, 3.5) == 7.5);
  CHECK(pr::fullCoordinate(0, 2, 1) == 0);
  auto global = Image::allocate(11, 11, 1, PixelDepth::U8);
  std::fill(global.mutablePixels(), global.mutablePixels() + 121, 255);
  roi.inputs["mask"] = Data::image(global);
  CHECK_FALSE(roi.run("cloud.roi_extract").ok);
  REQUIRE(roi.run("cloud.roi_extract", {{"usingOffset", Value::boolean(true)},
                                        {"offsetX", Value::number(1)},
                                        {"offsetY", Value::number(1)}})
              .ok);
  CHECK(roi.out("cloud").asCloud()->pointCount() == 1);
  CHECK_FALSE(roi.run("cloud.roi_extract",
                      {{"usingOffset", Value::boolean(true)}, {"offsetX", Value::number(3)}})
                  .ok);
}
