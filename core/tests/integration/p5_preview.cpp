#include "p3_example_helpers.h"
#include "test_ops.h"

#include "lyflow/calibration.h"

#include <opencv2/imgcodecs.hpp>

namespace {
using namespace lyflow;
using namespace lyflow::test;
class PreviewFiles {
public:
  PreviewFiles() {
    base = std::filesystem::temp_directory_path() /
           ("lyflow-p5-preview-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    REQUIRE(std::filesystem::create_directory(base));
    cv::Mat image(2201, 2201, CV_8UC4, cv::Scalar(10, 20, 30, 40));
    REQUIRE(cv::imwrite((base / "source.png").u8string(), image));
    const Json camera{{"type", calibration::kCameraType},
                      {"data",
                       {{"imageSize", {2201, 2201}},
                        {"cameraMatrix", {1000, 0, 1100, 0, 1000, 1100, 0, 0, 1}},
                        {"distortion", Json::array()},
                        {"frame", "camera"},
                        {"unit", "m"}}}};
    std::ofstream file(base / "camera.json", std::ios::binary);
    file << camera.dump();
    REQUIRE(file.good());
  }
  ~PreviewFiles() {
    std::error_code error;
    std::filesystem::remove_all(base, error);
  }
  std::filesystem::path base;
};
}

TEST_CASE(
    "P5 true odd sized source preview preserves physical cloud samples and full pixel mask coverage") {
  ensureTestOps();
  PreviewFiles files;
  PointCloud points;
  for (const double pixel : {100.25, 1100.25, 2200., -100.}) {
    points.push(static_cast<float>((pixel - 1100) / 1000),
                static_cast<float>((pixel - 1100) / 1000), 1);
    const auto n = static_cast<unsigned char>(points.pointCount());
    points.intensity.push_back(n);
    points.normals.insert(points.normals.end(), {0, 0, -1});
    points.rgb.insert(points.rgb.end(),
                      {static_cast<unsigned char>(10 * n), static_cast<unsigned char>(20 * n),
                       static_cast<unsigned char>(30 * n)});
  }
  const auto input = Data::cloud(std::move(points));
  const auto graph =
      makeGraph({{"source", "test.make_cloud"},
                 {"image", "io.load_image", {{"path", "source.png"}, {"mode", "unchanged"}}},
                 {"camera", "image.load_camera_calib", {{"path", "camera.json"}}},
                 {"project",
                  "cloud.project_camera",
                  {{"cloudFrame", "camera"}, {"imageFrame", "image"}, {"colorSource", "cloud"}}},
                 {"filter", "image.depth_filter", {{"mode", "mean"}, {"kernelSize", 1}}},
                 {"region",
                  "pose.construct_2d",
                  {{"id", "full-frame"},
                   {"frame", "image"},
                   {"position", {1100, 1100}},
                   {"size", {2201, 2201}}}},
                 {"regions", "pose.filter"},
                 {"roi", "cloud.roi_extract", {{"usingMask", false}}}},
                {{"source.cloud", "project.cloud"},
                 {"image.image", "project.image"},
                 {"camera.calib", "project.calib"},
                 {"project.cloud", "filter.cloud"},
                 {"project.depth", "filter.depth"},
                 {"project.status", "filter.status"},
                 {"project.indexMap", "filter.indexMap"},
                 {"project.mapping", "filter.mapping"},
                 {"filter.cloud", "roi.cloud"},
                 {"filter.depth", "roi.depth"},
                 {"filter.status", "roi.status"},
                 {"filter.indexMap", "roi.indexMap"},
                 {"filter.mapping", "roi.mapping"},
                 {"region.pose", "regions.poses"},
                 {"regions.matched", "roi.instances"}});
  for (const auto mode : {exec::RunMode::Full, exec::RunMode::Preview}) {
    CAPTURE(mode == exec::RunMode::Preview);
    exec::RunOptions options;
    options.runId = mode == exec::RunMode::Full ? "p5-odd-full" : "p5-odd-preview";
    options.baseDir = files.base;
    options.mode = mode;
    options.maxParallel = 1;
    options.noReuse = true;
    options.inputs = {{"source", "cloud", input}};
    RunLog log;
    exec::Run run(graph.dump(), options, &lyflow::test::detail::collect, &log);
    run.join();
    REQUIRE_MESSAGE(log.runStatus() == "ok", Json(log.events).dump());
    Data image, depth, status, index, mapping, projected, filtered, selected;
    auto& store = exec::ResultStore::instance();
    REQUIRE(store.get(options.runId, "image", "image", image));
    REQUIRE(store.get(options.runId, "project", "depth", depth));
    REQUIRE(store.get(options.runId, "project", "status", status));
    REQUIRE(store.get(options.runId, "project", "indexMap", index));
    REQUIRE(store.get(options.runId, "project", "mapping", mapping));
    REQUIRE(store.get(options.runId, "project", "cloud", projected));
    REQUIRE(store.get(options.runId, "filter", "cloud", filtered));
    REQUIRE(store.get(options.runId, "roi", "cloud", selected));
    const int scale = mode == exec::RunMode::Full ? 1 : 2, width = scale == 1 ? 2201 : 1101;
    CHECK(image.asImage()->scale == scale);
    CHECK(image.asImage()->width == width);
    CHECK(depth.asImage()->scale == scale);
    CHECK(depth.asImage()->width == width);
    CHECK(status.asImage()->scale == scale);
    CHECK(index.tensorScale() == scale);
    CHECK(index.asTensor()->shape == std::vector<std::int64_t>{width, width});
    CHECK(mapping.asRecord()->data.at("width") == width);
    CHECK(mapping.asRecord()->data.at("fullWidth") == 2201);
    CHECK(mapping.asRecord()->data.at("imageFrame") == "image");
    CHECK(mapping.asRecord()->data.at("gridFrame") == "camera");
    CHECK(mapping.asRecord()->data.at("sourceCloudId") == std::to_string(projected.asCloud()->id));
    CHECK(index.asTensor()->data.back() == 2);
    CHECK(status.asImage()->pixels.get()[static_cast<std::size_t>(width) * width - 1] != 0);
    for (const auto* cloud : {projected.asCloud(), filtered.asCloud(), selected.asCloud()}) {
      REQUIRE(cloud != nullptr);
      REQUIRE(cloud->pointCount() == 3);
      CHECK(cloud->xyz ==
            std::vector<float>(input.asCloud()->xyz.begin(), input.asCloud()->xyz.begin() + 9));
      CHECK(cloud->intensity == std::vector<float>{1, 2, 3});
      CHECK(cloud->normals == std::vector<float>(input.asCloud()->normals.begin(),
                                                 input.asCloud()->normals.begin() + 9));
      CHECK(cloud->rgb == std::vector<std::uint8_t>(input.asCloud()->rgb.begin(),
                                                    input.asCloud()->rgb.begin() + 9));
    }
  }
}

TEST_CASE(
    "P5 full camera masks and ray plane geometry stay in the same pixel frame during preview") {
  const auto graph =
      p3::read(std::filesystem::path(LYFLOW_EXAMPLES_DIR) / "p5-pose-project-plane.lyflow.json");
  Json expected;
  std::vector<float> expectedMask;
  std::vector<std::int64_t> expectedShape;
  int expectedScale = 0;
  for (const auto mode : {exec::RunMode::Full, exec::RunMode::Preview}) {
    exec::RunOptions options;
    options.runId = mode == exec::RunMode::Full ? "p5-pose-full" : "p5-pose-preview";
    options.baseDir = LYFLOW_EXAMPLES_DIR;
    options.mode = mode;
    options.maxParallel = 1;
    options.noReuse = true;
    RunLog log;
    exec::Run run(graph.dump(), options, &lyflow::test::detail::collect, &log);
    run.join();
    REQUIRE_MESSAGE(log.runStatus() == "ok", Json(log.events).dump());
    const auto outputs = p3::take(lyflow_run_outputs(options.runId.c_str()));
    if (mode == exec::RunMode::Full)
      expected = outputs;
    else
      p3::agree(expected, outputs);
    const auto source = graph.at("outputs").at("masks");
    Data mask;
    REQUIRE(exec::ResultStore::instance().get(options.runId, source.at("node"), source.at("port"),
                                              mask));
    CHECK(mask.tensorScale() == 1);
    REQUIRE(mask.asTensor() != nullptr);
    CHECK(mask.asTensor()->shape == std::vector<std::int64_t>{1, 48, 64});
    if (mode == exec::RunMode::Full) {
      expectedMask = mask.asTensor()->data;
      expectedShape = mask.asTensor()->shape;
      expectedScale = mask.tensorScale();
    } else {
      CHECK(mask.tensorScale() == expectedScale);
      CHECK(mask.asTensor()->shape == expectedShape);
      CHECK(mask.asTensor()->data == expectedMask);
    }
    CHECK(p3::data(outputs, "maskInfo").at("frame") == "image");
    const auto& pose = p3::data(outputs, "poses").at("items")[0];
    CHECK(std::abs(pose.at("position")[0].get<double>()) <= 1e-9);
    CHECK(std::abs(pose.at("position")[1].get<double>()) <= 1e-9);
    CHECK(std::abs(pose.at("position")[2].get<double>() - 1.01) <= 1e-8);
  }
}
