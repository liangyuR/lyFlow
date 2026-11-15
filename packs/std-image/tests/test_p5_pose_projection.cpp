#include "helpers.h"
#include "lyflow/calibration.h"
#include "lyflow/pose.h"
#include "lyflow_cv/compat.h"
#include <numeric>
#include <limits>

namespace {
using namespace lyflow;
using Json = nlohmann::json;
Data projectionCamera(int width = 32, int height = 24, Json distortion = Json::array()) {
  return Data::record({calibration::kCameraType,
                       {{"imageSize", {width, height}},
                        {"cameraMatrix", {40., 0., 15.5, 0., 40., 11.5, 0., 0., 1.}},
                        {"distortion", distortion},
                        {"frame", "camera"},
                        {"unit", "m"}}});
}
Data projectionPoses(Json centers = Json::array({Json::array({0, 0, 2})}),
                     const char* frame = "camera") {
  Json items = Json::array();
  for (std::size_t i = 0; i < centers.size(); ++i) {
    auto r = poses::make3D("part" + std::to_string(i), frame,
                           centers[i].get<std::array<double, 3>>(), {0, 0, 0, 1}, {.4, .4, .2});
    r.data["classLabel"] = "零件";
    r.data["pickable"] = true;
    items.push_back(r.data);
  }
  return Data::record(poses::make3DList(items, frame));
}
const Json& projectionData(const test::OpCall& c, const char* port) {
  return c.outputs.at(port).asRecord()->data;
}
}
TEST_CASE(
    "P5 finite primitive projection keeps silhouettes union gaps frames metadata and mask order") {
  using namespace lyflow;
  test::OpCall c;
  c.inputs = {{"poses",
               projectionPoses(Json::array({Json::array({-.45, 0, 2}), Json::array({.45, 0, 2})}))},
              {"calib", projectionCamera()}};
  REQUIRE(c.run("pose.project_3d").ok);
  CHECK(projectionData(c, "poses")["frame"] == "image");
  CHECK(projectionData(c, "poses")["items"].size() == 2);
  CHECK(projectionData(c, "poses")["items"][0]["classLabel"] == "零件");
  CHECK(projectionData(c, "poses")["items"][0]["pickable"] == false);
  CHECK(c.outputs.at("masks").asTensor()->shape == std::vector<std::int64_t>{2, 24, 32});
  CHECK(projectionData(c, "maskInfo")["items"][1]["id"] == "part1");
  REQUIRE(c.run("pose.project_3d", {{"mode", Value::text("union")}}).ok);
  const auto* mask = c.outputs.at("masks").asTensor();
  CHECK(mask->shape[0] == 1);
  CHECK(mask->data[12 * 32 + 15] == 0);
  CHECK(projectionData(c, "poses")["items"][0]["attributes"]["sourcePoses3D"].size() == 2);
  c.inputs["rois"] = Data::record(poses::make2DList(Json::array()));
  REQUIRE(c.run("pose.project_3d").ok);
  CHECK(c.outputs.at("masks").asTensor()->shape[0] == 0);
  CHECK(projectionData(c, "quality")["items"][0]["status"] == "empty_roi");
}
TEST_CASE(
    "P5 circle projection uses actual disk and nearest volume mode suppresses occluded support") {
  using namespace lyflow;
  test::OpCall c;
  auto disk = projectionPoses();
  auto record = *disk.asRecord();
  record.data["items"][0]["attributes"] = {
      {"shape", "circle"}, {"radius", .2}, {"normal", {0, 0, 1}}};
  c.inputs = {{"poses", Data::record(record)}, {"calib", projectionCamera()}};
  REQUIRE(c.run("pose.project_3d").ok);
  auto masks = c.outputs.at("masks");
  auto count = std::count(masks.asTensor()->data.begin(), masks.asTensor()->data.end(), 1.f);
  CHECK(count > 0);
  record.data["items"][0]["attributes"]["shape"] = "box";
  c.inputs["poses"] = Data::record(record);
  REQUIRE(c.run("pose.project_3d").ok);
  CHECK(std::count(c.outputs.at("masks").asTensor()->data.begin(),
                   c.outputs.at("masks").asTensor()->data.end(), 1.f) > count);
  c.inputs["poses"] =
      projectionPoses(Json::array({Json::array({0, 0, 2}), Json::array({0, 0, 3})}));
  REQUIRE(c.run("pose.project_3d", {{"occlusion", Value::text("nearest")}}).ok);
  CHECK(projectionData(c, "poses")["items"].size() == 1);
  CHECK(projectionData(c, "quality")["items"][1]["pixels"] == 0);
  CHECK_FALSE(c.run("pose.project_3d",
                    {{"occlusion", Value::text("nearest")}, {"dilate", Value::integer(1)}})
                  .ok);
  CHECK(c.outputs.empty());
}
TEST_CASE("P5 projection cross frame distortion clipping and negative depth are explicit") {
  using namespace lyflow;
  test::OpCall c;
  c.inputs = {{"poses", projectionPoses(Json::array({Json::array({0, 0, 2})}), "world")},
              {"calib", projectionCamera(32, 24, Json::array({.1, -.01, .001, -.002, 0}))}};
  CHECK_FALSE(c.run("pose.project_3d").ok);
  c.inputs["posesToCamera"] = Data::transform(Transform{});
  REQUIRE(c.run("pose.project_3d").ok);
  CHECK_FALSE(projectionData(c, "poses")["items"].empty());
  c.inputs["poses"] = projectionPoses(Json::array({Json::array({0, 0, -2})}));
  c.inputs.erase("posesToCamera");
  REQUIRE(c.run("pose.project_3d").ok);
  CHECK(projectionData(c, "poses")["items"].empty());
  CHECK(c.outputs.at("masks").asTensor()->data.empty());
}
TEST_CASE("P5 arbitrary plane lift fits real mask support and keeps source to camera semantics") {
  using namespace lyflow;
  test::OpCall project;
  project.inputs = {{"poses", projectionPoses()}, {"calib", projectionCamera()}};
  REQUIRE(project.run("pose.project_3d", {{"surface", Value::text("top")}}).ok);
  Plane plane;
  plane.d = -2.1f;
  test::OpCall lift;
  lift.inputs = {{"poses", project.outputs.at("poses")},
                 {"calib", projectionCamera()},
                 {"plane", Data::plane(plane)},
                 {"masks", project.outputs.at("masks")},
                 {"maskInfo", project.outputs.at("maskInfo")}};
  REQUIRE(lift.run("pose.project_to_plane", {{"mode", Value::text("dimensions")}}).ok);
  const auto& pose = projectionData(lift, "poses")["items"][0];
  CHECK(pose["position"][2].get<double>() == doctest::Approx(2.1).epsilon(1e-6));
  CHECK(pose["size"][0].get<double>() > .3);
  CHECK(pose["size"][0].get<double>() < .5);
  CHECK(pose["classLabel"] == "零件");
  CHECK(pose["pickable"] == false);
  lift.inputs["masks"] = lift.inputs.at("masks").withTensorScale(2);
  CHECK_FALSE(lift.run("pose.project_to_plane").ok);
  CHECK(lift.outputs.empty());
  lift.inputs["masks"] = project.outputs.at("masks");
  plane.d = 2;
  lift.inputs["plane"] = Data::plane(plane);
  CHECK_FALSE(lift.run("pose.project_to_plane").ok);
  CHECK(lift.outputs.empty());
}
TEST_CASE("P5 plane sources support center known dimensions workspace faces and ID planes") {
  using namespace lyflow;
  test::OpCall c;
  auto pose = poses::make2D("A", "image", "px", {15.5, 11.5}, 0, {8, 6});
  c.inputs = {{"poses", Data::record(pose)},
              {"calib", projectionCamera()},
              {"workspace", projectionPoses()}};
  REQUIRE(c.run("pose.project_to_plane", {{"planeSource", Value::text("workspace_bottom")}}).ok);
  CHECK(projectionData(c, "poses")["items"][0]["position"][2].get<double>() ==
        doctest::Approx(1.9));
  REQUIRE(c.run("pose.project_to_plane", {{"planeSource", Value::text("workspace_top")}}).ok);
  CHECK(projectionData(c, "poses")["items"][0]["position"][2].get<double>() ==
        doctest::Approx(2.1));
  c.inputs.erase("workspace");
  c.inputs["planes"] =
      Data::record({"geometry.Planes3D.v1",
                    {{"frame", "camera"},
                     {"unit", "m"},
                     {"items", Json::array({{{"id", "A"}, {"normal", {0, 0, 1}}, {"d", -2}}})}}});
  REQUIRE(c.run("pose.project_to_plane", {{"mode", Value::text("dimensions")}}).ok);
  const auto& result = projectionData(c, "poses")["items"][0];
  CHECK(result["size"][0].get<double>() == doctest::Approx(.3).epsilon(1e-5));
  CHECK(result["size"][1].get<double>() == doctest::Approx(.4).epsilon(1e-5));
}
TEST_CASE("P5 mask associated removal preserves optional channels OOB unknown and top expansion") {
  using namespace lyflow;
  test::OpCall project;
  project.inputs = {{"poses", projectionPoses()}, {"calib", projectionCamera()}};
  REQUIRE(project.run("pose.project_3d").ok);
  auto sourcePose = *projectionPoses().asRecord();
  sourcePose.data["items"][0]["attributes"] = {{"primitive2DId", "part0"}};
  PointCloud cloud;
  for (auto p : {std::array<float, 3>{0, 0, 2}, {0, -.4f, 2}, {100, 0, 2}, {0, 0, -1}}) {
    cloud.push(p[0], p[1], p[2]);
    cloud.rgb.insert(cloud.rgb.end(), {1, 2, 3});
    cloud.intensity.push_back(cloud.pointCount());
  }
  auto source = Data::cloud(std::move(cloud));
  test::OpCall c;
  c.inputs = {{"cloud", source},
              {"poses", Data::record(sourcePose)},
              {"detections", project.outputs.at("poses")},
              {"masks", project.outputs.at("masks")},
              {"maskInfo", project.outputs.at("maskInfo")},
              {"calib", projectionCamera()}};
  REQUIRE(c.run("cloud.remove_mask_primitives").ok);
  CHECK(c.outputs.at("removedIndices").asIndices()->values == std::vector<std::int32_t>{0});
  CHECK(c.outputs.at("cloud").asCloud()->intensity == std::vector<float>{2, 3, 4});
  CHECK(c.outputs.at("quality").asRecord()->data["outOfBoundsKept"] == 1);
  CHECK(c.outputs.at("unknownIndices").asIndices()->values == std::vector<std::int32_t>{3});
  REQUIRE(c.run("cloud.remove_mask_primitives", {{"topExpand", Value::integer(8)}}).ok);
  CHECK(c.outputs.at("removedIndices").asIndices()->values == std::vector<std::int32_t>{0, 1});
  CHECK(source.asCloud()->pointCount() == 4);
  CHECK(source.asCloud()->rgb.size() == 12);
  sourcePose.data["items"][0]["attributes"]["primitive2DId"] = "missing";
  c.inputs["poses"] = Data::record(sourcePose);
  CHECK_FALSE(c.run("cloud.remove_mask_primitives").ok);
  CHECK(c.outputs.empty());
}
TEST_CASE(
    "P5 projection and plane binding reject malformed masks overflow and cancelled partial results") {
  using namespace lyflow;
  test::OpCall project;
  project.inputs = {{"poses", projectionPoses()}, {"calib", projectionCamera()}};
  REQUIRE(project.run("pose.project_3d").ok);
  const auto cleanMasks = project.outputs.at("masks"), cleanInfo = project.outputs.at("maskInfo");
  Plane plane;
  plane.d = -2;
  test::OpCall lift;
  lift.inputs = {{"poses", project.outputs.at("poses")},
                 {"calib", projectionCamera()},
                 {"plane", Data::plane(plane)},
                 {"masks", cleanMasks},
                 {"maskInfo", cleanInfo}};
  auto altered = *cleanInfo.asRecord();
  altered.data["items"][0]["id"] = "other";
  lift.inputs["maskInfo"] = Data::record(altered);
  CHECK_FALSE(lift.run("pose.project_to_plane").ok);
  CHECK(lift.outputs.empty());
  lift.inputs["maskInfo"] = cleanInfo;
  auto tensor = *cleanMasks.asTensor();
  tensor.data[0] = 1;
  lift.inputs["masks"] = Data::tensor(tensor);
  CHECK_FALSE(lift.run("pose.project_to_plane").ok);
  CHECK(lift.outputs.empty());
  tensor = *cleanMasks.asTensor();
  tensor.data[0] = std::numeric_limits<float>::quiet_NaN();
  lift.inputs["masks"] = Data::tensor(tensor);
  CHECK_FALSE(lift.run("pose.project_to_plane").ok);
  lift.inputs["masks"] = cleanMasks;
  auto huge = *projectionPoses().asRecord();
  huge.data["items"][0]["position"][0] = 1e300;
  project.inputs["poses"] = Data::record(huge);
  CHECK_FALSE(project.run("pose.project_3d").ok);
  CHECK(project.outputs.empty());
  project.inputs["poses"] = projectionPoses();
  project.inputs["calib"] = projectionCamera(4097, 1024);
  CHECK(project.run("pose.project_3d").code == "resource_limit");
  CHECK(project.outputs.empty());
  project.inputs["calib"] = projectionCamera();
  struct Cancel final : ExecContext {
    mutable int polls = 0;
    int stop;
    std::filesystem::path path;
    explicit Cancel(int n) : stop(n) {}
    bool cancelled() const override { return ++polls >= stop; }
    void progress(float, std::string_view) override {}
    void log(LogLevel, std::string) override {}
    const std::filesystem::path& baseDir() const override { return path; }
    int threadBudget() const override { return 1; }
  };
  for (const char* id : {"pose.project_3d", "pose.project_to_plane"})
    for (int stop : {1, 4}) {
      CAPTURE(id);
      CAPTURE(stop);
      const auto* op = ensureRegistry().find(id);
      REQUIRE(op);
      ParamMap params;
      for (const auto& v : op->params)
        params[v.name] = v.def;
      const auto& source = std::string(id) == "pose.project_3d" ? project.inputs : lift.inputs;
      Inputs inputs(source);
      std::unordered_map<std::string, Data> output;
      Outputs outputs(output);
      Cancel ctx(stop);
      ParamView p(params, ctx.path);
      CHECK(op->compute(inputs, p, outputs, ctx).code == "cancelled");
      CHECK(output.empty());
    }
}

TEST_CASE(
    "P5 large finite sphere and cylinder silhouettes do not acquire false quadratic support") {
  using namespace lyflow;
  test::OpCall c;
  auto pose = poses::make3D("far", "camera", {0, 0, 1e12}, {0, 0, 0, 1}, {2, 2, 2});
  auto calib = projectionCamera(3, 1);
  auto record = *calib.asRecord();
  record.data["cameraMatrix"] = {1e8, 0, 1, 0, 1e8, 0, 0, 0, 1};
  for (const char* shape : {"sphere", "cylinder"}) {
    pose.data["attributes"] = {{"shape", shape}};
    c.inputs = {{"poses", Data::record(pose)}, {"calib", Data::record(record)}};
    REQUIRE(c.run("pose.project_3d").ok);
    CHECK(c.outputs.at("masks").asTensor()->data == std::vector<float>{0, 1, 0});
  }
}

TEST_CASE(
    "P5 plane node uses shared stable inverse distortion for strong legal lenses and rejects failures") {
  using namespace lyflow;
  test::OpCall c;
  auto record = *projectionCamera(3, 1).asRecord();
  record.data["cameraMatrix"] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  record.data["distortion"] = {1, 0, 0, 0, 0};
  auto pose = poses::make2D("lens", "image", "px", {2, 0}, 0, {1, 1});
  Plane plane;
  plane.d = -1;
  c.inputs = {{"poses", Data::record(pose)},
              {"calib", Data::record(record)},
              {"plane", Data::plane(plane)}};
  REQUIRE(c.run("pose.project_to_plane").ok);
  const auto& result = projectionData(c, "poses")["items"][0];
  CHECK(std::abs(result["position"][0].get<double>() - 1) < 1e-7);
  CHECK(result["position"][2] == 1);
  CHECK(result["pickable"] == false);
  record.data["distortion"] = {-1, 0, 0, 0, 0};
  c.inputs["calib"] = Data::record(record);
  CHECK_FALSE(c.run("pose.project_to_plane").ok);
  CHECK(c.outputs.empty());
  record.data["distortion"] = Json::array();
  c.inputs["calib"] = Data::record(record);
  REQUIRE(c.run("pose.project_to_plane").ok);
  CHECK(projectionData(c, "poses")["items"][0]["position"][0] == 2);
  record.data["cameraMatrix"] = {10, 0, 0, 0, 10, 0, 0, 0, 1};
  record.data["distortion"] = Json::array({.03, -.005, .001, -.001, .0001, .001, -.0002, .00003,
                                           .0001, .00002, -.0001, -.00001, .01, -.02});
  c.inputs["calib"] = Data::record(record);
  REQUIRE_MESSAGE(c.run("pose.project_to_plane").ok, c.status.message);
  const auto& at = projectionData(c, "poses")["items"][0]["position"];
  std::vector<cv::Point3d> point{{at[0].get<double>(), at[1].get<double>(), at[2].get<double>()}};
  std::vector<cv::Point2d> uv;
  const auto matrix = record.data["cameraMatrix"].get<std::array<double, 9>>();
  cv::projectPoints(point, cv::Vec3d::all(0), cv::Vec3d::all(0), cv::Matx33d(matrix.data()),
                    record.data["distortion"].get<std::vector<double>>(), uv);
  CHECK(cv::norm(uv[0] - cv::Point2d(2, 0)) < 1e-7);
}

TEST_CASE("P5 symmetric primitive pixel footprint roundtrips without half pixel center bias") {
  using namespace lyflow;
  auto camera = *projectionCamera(9, 9).asRecord();
  camera.data["cameraMatrix"] = {100, 0, 4, 0, 100, 4, 0, 0, 1};
  auto source = poses::make3D("box", "camera", {0, 0, 1}, {0, 0, 0, 1}, {.02, .02, .02});
  for (const char* mode : {"individual", "union"}) {
    test::OpCall project;
    project.inputs = {{"poses", Data::record(source)}, {"calib", Data::record(camera)}};
    REQUIRE(project.run("pose.project_3d", {{"mode", Value::text(mode)}}).ok);
    const auto& pose = projectionData(project, "poses")["items"][0];
    CHECK(pose["position"] == Json::array({4, 4}));
    CHECK(pose["size"] == Json::array({3, 3}));
    Plane plane;
    plane.d = -1;
    test::OpCall back;
    back.inputs = {{"poses", project.outputs.at("poses")},
                   {"calib", Data::record(camera)},
                   {"plane", Data::plane(plane)}};
    REQUIRE(back.run("pose.project_to_plane").ok);
    const auto& center = projectionData(back, "poses")["items"][0]["position"];
    CHECK(center == Json::array({0, 0, 1}));
  }
}

TEST_CASE("P5 projection inverse distortion only evaluates explicitly selected ROI pixels") {
  using namespace lyflow;
  auto camera = *projectionCamera(9, 9).asRecord();
  camera.data["cameraMatrix"] = {1, 0, 4, 0, 1, 4, 0, 0, 1};
  camera.data["distortion"] = {-1, 0, 0, 0, 0};
  auto source = poses::make3D("box", "camera", {0, 0, 1}, {0, 0, 0, 1}, {.2, .2, .2});
  auto roi = poses::make2D("roi", "image", "px", {4, 4}, 0, {.5, .5});
  test::OpCall c;
  c.inputs = {{"poses", Data::record(source)},
              {"calib", Data::record(camera)},
              {"rois", Data::record(roi)}};
  REQUIRE_MESSAGE(c.run("pose.project_3d").ok, c.status.message);
  const auto* mask = c.outputs.at("masks").asTensor();
  CHECK(std::count(mask->data.begin(), mask->data.end(), 1.f) == 1);
  CHECK(mask->data[4 * 9 + 4] == 1);
  CHECK(projectionData(c, "poses")["items"][0]["position"] == Json::array({4, 4}));
  c.inputs["rois"] = Data::record(poses::make2DList(Json::array()));
  REQUIRE(c.run("pose.project_3d").ok);
  CHECK(c.outputs.at("masks").asTensor()->data.empty());
  CHECK(projectionData(c, "poses")["items"].empty());
}

TEST_CASE("P5 plane dimensions never substitutes bbox for declared native mask") {
  using namespace lyflow;
  auto pose = poses::make2D("masked", "image", "px", {15.5, 11.5}, 0, {8, 6});
  pose.data["attributes"] = {{"hasMask", true}, {"maskIndex", 0}};
  Plane plane;
  plane.d = -1;
  test::OpCall c;
  c.inputs = {
      {"poses", Data::record(pose)}, {"calib", projectionCamera()}, {"plane", Data::plane(plane)}};
  CHECK_FALSE(c.run("pose.project_to_plane", {{"mode", Value::text("dimensions")}}).ok);
  CHECK(c.outputs.empty());
  REQUIRE(c.run("pose.project_to_plane", {{"mode", Value::text("center")}}).ok);
  pose.data["attributes"]["hasMask"] = false;
  c.inputs["poses"] = Data::record(pose);
  REQUIRE(c.run("pose.project_to_plane", {{"mode", Value::text("dimensions")}}).ok);
  CHECK(projectionData(c, "poses")["items"][0]["attributes"]["shape"] == "box");
}

TEST_CASE("P5 plane dimensions select native mask or clipped box independently per instance") {
  using namespace lyflow;
  bool mixed = false;
  SUBCASE("all bbox annotations retain valid zero mask planes") { mixed = false; }
  SUBCASE("mixed annotations retain actual mask footprints and bbox footprints") { mixed = true; }
  auto bbox = poses::make2D("bbox", "image", "px", {0, 11.5}, 0, {8, 6});
  bbox.data["attributes"] = {{"hasMask", false}, {"maskIndex", mixed ? 1 : 0}};
  auto masked = poses::make2D("masked", "image", "px", {23.5, 11.5}, 0, {8, 6});
  masked.data["attributes"] = {{"hasMask", true}, {"maskIndex", 0}};
  Json items = mixed ? Json::array({masked.data, bbox.data}) : Json::array({bbox.data});
  Tensor tensor;
  tensor.shape = {mixed ? 2 : 1, 24, 32};
  tensor.data.assign((mixed ? 2 : 1) * 24 * 32, 0);
  if (mixed)
    for (int row = 10; row <= 13; ++row)
      for (int col = 22; col <= 25; ++col)
        tensor.data[row * 32 + col] = 1;
  Json bindings = Json::array();
  if (mixed)
    bindings.push_back({{"id", "masked"}, {"index", 0}, {"hasMask", true}, {"source", "native"}});
  bindings.push_back(
      {{"id", "bbox"}, {"index", mixed ? 1 : 0}, {"hasMask", false}, {"source", "none"}});
  Record info{"vision.Masks2D.v1",
              {{"frame", "image"},
               {"unit", "px"},
               {"width", 32},
               {"height", 24},
               {"layout", "NHW"},
               {"items", bindings}}};
  Plane plane;
  plane.d = -2;
  test::OpCall c;
  c.inputs = {{"poses", Data::record(poses::make2DList(items))},
              {"calib", projectionCamera()},
              {"plane", Data::plane(plane)},
              {"masks", Data::tensor(tensor)},
              {"maskInfo", Data::record(info)}};
  REQUIRE_MESSAGE(c.run("pose.project_to_plane", {{"mode", Value::text("dimensions")}}).ok,
                  c.status.message);
  const auto& result = projectionData(c, "poses")["items"];
  REQUIRE(result.size() == items.size());
  for (const auto& pose : result) {
    const bool isMask = pose["id"] == "masked";
    const double lowX = isMask ? .3 : -.8, highX = isMask ? .5 : -.575, lowY = isMask ? -.1 : -.15,
                 highY = isMask ? .1 : .15;
    CHECK(pose["position"][0].get<double>() == doctest::Approx((lowX + highX) / 2).epsilon(1e-6));
    CHECK(pose["position"][1].get<double>() == doctest::Approx(0).epsilon(1e-6));
    CHECK(pose["position"][2].get<double>() == doctest::Approx(2).epsilon(1e-6));
    auto size = pose["size"].get<std::array<double, 3>>();
    auto wanted = std::array<double, 2>{highX - lowX, highY - lowY};
    std::sort(size.begin(), size.begin() + 2);
    std::sort(wanted.begin(), wanted.end());
    CHECK(size[0] == doctest::Approx(wanted[0]).epsilon(1e-6));
    CHECK(size[1] == doctest::Approx(wanted[1]).epsilon(1e-6));
    const auto r = poses::rotationMatrix(pose["rotation"].get<poses::Quaternion>());
    const auto actualSize = pose["size"].get<std::array<double, 3>>();
    std::array<double, 2> minimum{1e10, 1e10}, maximum{-1e10, -1e10};
    for (int sx : {-1, 1})
      for (int sy : {-1, 1})
        for (int axis = 0; axis < 2; ++axis) {
          const double corner = pose["position"][axis].get<double>() +
                                sx * r[axis * 3] * actualSize[0] / 2 +
                                sy * r[axis * 3 + 1] * actualSize[1] / 2;
          minimum[axis] = std::min(minimum[axis], corner);
          maximum[axis] = std::max(maximum[axis], corner);
        }
    CHECK(minimum[0] == doctest::Approx(lowX).epsilon(1e-6));
    CHECK(maximum[0] == doctest::Approx(highX).epsilon(1e-6));
    CHECK(minimum[1] == doctest::Approx(lowY).epsilon(1e-6));
    CHECK(maximum[1] == doctest::Approx(highY).epsilon(1e-6));
  }
  if (mixed) {
    CHECK(projectionData(c, "quality")["items"][0]["supportCount"] == 16);
    std::fill(tensor.data.begin(), tensor.data.begin() + 24 * 32, 0);
    c.inputs["masks"] = Data::tensor(tensor);
    CHECK_FALSE(c.run("pose.project_to_plane", {{"mode", Value::text("dimensions")}}).ok);
    CHECK(c.outputs.empty());
  }
}
