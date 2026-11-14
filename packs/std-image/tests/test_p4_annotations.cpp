#include <algorithm>
#include <atomic>
#include <fstream>
#include <limits>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "helpers.h"
#include "lyflow/pose.h"
#include "lyflow_cv/adapter.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#endif

namespace lyflow::ops {
void registerP4Annotations(Registry&);
void registerP4Segmentation(Registry&);
}

namespace {
using namespace lyflow;
using Json = nlohmann::json;
namespace fs = std::filesystem;

const Registry& annotationRegistry() {
  static Registry registry;
  static const bool registered = [] {
    ops::registerP4Annotations(registry);
    ops::registerP4Segmentation(registry);
    return true;
  }();
  (void)registered;
  return registry;
}
struct AnnotationFiles {
  fs::path directory, parent;
  AnnotationFiles() {
    static std::atomic<unsigned> sequence{0};
    parent = fs::weakly_canonical(fs::temp_directory_path());
    directory =
        parent /
        fs::u8path("lyflow-p4-标注-" + std::to_string(++sequence) + "-" +
                   std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    REQUIRE(fs::create_directory(directory));
  }
  ~AnnotationFiles() {
    std::error_code ec;
    if (directory.is_absolute() && directory.parent_path() == parent &&
        directory.filename().u8string().find("lyflow-p4-标注-") == 0)
      fs::remove_all(directory, ec);
  }
  fs::path path(const std::string& name) const { return directory / fs::u8path(name); }
  void write(const std::string& name, const std::string& data) const {
    fs::create_directories(path(name).parent_path());
    std::ofstream stream(path(name), std::ios::binary | std::ios::trunc);
    stream.write(data.data(), static_cast<std::streamsize>(data.size()));
    REQUIRE(stream.good());
  }
  void json(const std::string& name, const Json& value) const { write(name, value.dump()); }
  void image(const std::string& name, cv::Mat matrix) const {
    std::vector<unsigned char> encoded;
    REQUIRE(cv::imencode(".png", matrix, encoded));
    write(name, std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size()));
  }
  void native(const Json& annotation, int width = 12, int height = 10) const {
    json("索引.json", {{"schemaVersion", 1},
                       {"samples", Json::array({{{"timestamp", "0001700000000"},
                                                 {"image", "图像/工件.png"},
                                                 {"annotation", "标注/工件.json"}}})}});
    json("标注/工件.json", annotation);
    image("图像/工件.png", cv::Mat(height, width, CV_8UC4, cv::Scalar(30, 20, 10, 47)));
  }
};
Json rectangle(double x0, double y0, double x1, double y1) {
  return Json::array(
      {Json::array({x0, y0}), Json::array({x1, y0}), Json::array({x1, y1}), Json::array({x0, y1})});
}
Json annotationInstance(const std::string& id, const std::string& label,
                        Json parts = Json::array()) {
  return {{"id", id},         {"label", label},   {"classId", 7},      {"bbox", {1, 1, 8, 7}},
          {"confidence", .8}, {"pickable", true}, {"occluded", false}, {"parts", std::move(parts)}};
}
Json annotation(Json instances = Json::array(), int width = 12, int height = 10) {
  return {{"schemaVersion", 1},
          {"width", width},
          {"height", height},
          {"frame", "相机"},
          {"instances", std::move(instances)}};
}
Status load(test::OpCall& call, const AnnotationFiles& files, ParamMap params = {}) {
  params["path"] = Value::text(files.path("索引.json").u8string());
  if (!params.count("timestamp"))
    params["timestamp"] = Value::text("0001700000000");
  if (!params.count("frame"))
    params["frame"] = Value::text("相机");
  return call.run("image.load_annotations", params);
}
const Json& record(const test::OpCall& call, const char* port) {
  REQUIRE(call.outputs.count(port) == 1);
  const auto* value = call.outputs.at(port).asRecord();
  REQUIRE(value);
  return value->data;
}
float mask(const Tensor& value, int instance, int y, int x) {
  return value.data[(static_cast<std::size_t>(instance) * value.shape[1] + y) * value.shape[2] + x];
}
struct AnnotationCancel final : ExecContext {
  mutable int calls = 0;
  int after = 1;
  fs::path root;
  bool cancelled() const override { return ++calls >= after; }
  void progress(float, std::string_view) override {}
  void log(LogLevel, std::string) override {}
  const fs::path& baseDir() const override { return root; }
  int threadBudget() const override { return 1; }
};
bool directoryLink(const fs::path& target, const fs::path& link) {
#if defined(_WIN32)
  if (!CreateDirectoryW(link.c_str(), nullptr))
    return false;
  const auto destination = L"\\??\\" + fs::absolute(target).wstring();
  struct Reparse {
    DWORD tag;
    WORD bytes, reserved, substituteOffset, substituteLength, printOffset, printLength;
    wchar_t path[4096];
  } value{};
  if (destination.size() + 2 > 4096)
    return false;
  value.tag = IO_REPARSE_TAG_MOUNT_POINT;
  value.substituteLength = static_cast<WORD>(destination.size() * sizeof(wchar_t));
  value.printOffset = value.substituteLength + sizeof(wchar_t);
  value.bytes = static_cast<WORD>(8 + value.printOffset + sizeof(wchar_t));
  std::copy(destination.begin(), destination.end(), value.path);
  const auto handle =
      CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                  FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (handle == INVALID_HANDLE_VALUE)
    return false;
  DWORD returned = 0;
  const bool result = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &value, value.bytes + 8,
                                      nullptr, 0, &returned, nullptr) != 0;
  CloseHandle(handle);
  return result;
#else
  std::error_code error;
  fs::create_directory_symlink(target, link, error);
  return !error;
#endif
}
}

TEST_CASE(
    "P4 annotations native Unicode multipart holes and overlaps remain native with truthful bindings") {
  AnnotationFiles files;
  Json part = {{"polygon", rectangle(1, 1, 8, 7)}, {"holes", Json::array({rectangle(3, 3, 5, 5)})}};
  Json patch = {{"polygon", rectangle(4, 4, 6, 6)}, {"holes", Json::array()}};
  auto first = annotationInstance("零件-A", "连接件", Json::array({part, patch}));
  auto second =
      annotationInstance("零件-B", "同类重叠", Json::array({{{"polygon", rectangle(6, 2, 9, 6)}}}));
  auto data = annotation(Json::array({first, second}));
  data["rois"] = Json::array({{{"id", "工作区"},
                               {"polygon", rectangle(0, 0, 10, 8)},
                               {"holes", Json::array({rectangle(2, 2, 3, 3)})}}});
  files.native(data);
  test::OpCall call(annotationRegistry());
  REQUIRE_MESSAGE(load(call, files).ok, call.status.message);
  const auto& image = *call.outputs.at("image").asImage();
  CHECK(image.depth == PixelDepth::U8);
  CHECK(image.channels == 4);
  CHECK(cvx::view(image).at<cv::Vec4b>(0, 0) == cv::Vec4b(10, 20, 30, 47));
  const auto& masks = *call.outputs.at("masks").asTensor();
  CHECK(masks.shape == std::vector<std::int64_t>{2, 10, 12});
  CHECK(mask(masks, 0, 2, 2) == 1);
  CHECK(mask(masks, 0, 3, 3) == 0);
  CHECK(mask(masks, 0, 4, 4) == 1);
  CHECK(mask(masks, 0, 2, 7) == 1);
  CHECK(mask(masks, 1, 2, 7) == 1);
  for (const auto value : masks.data)
    CHECK((value == 0 || value == 1));
  const auto& poses = record(call, "instances");
  const auto& info = record(call, "maskInfo");
  CHECK(poses["unit"] == "px");
  CHECK(info["layout"] == "NHW");
  CHECK(info["frame"] == "相机");
  REQUIRE(poses["items"].size() == 2);
  for (int i = 0; i < 2; ++i) {
    CHECK(poses["items"][i]["id"] == info["items"][i]["id"]);
    CHECK(info["items"][i]["index"] == i);
    CHECK(info["items"][i]["hasMask"] == true);
    CHECK(info["items"][i]["source"] == "annotation_polygon");
    CHECK(poses["items"][i]["pickable"] == false);
  }
  CHECK(poses["items"][0]["classLabel"] == "连接件");
  CHECK(poses["items"][0]["classId"] == "7");
  CHECK(poses["items"][0]["attributes"]["annotationPickable"] == true);
  const auto overlay = cvx::view(*call.outputs.at("overlay").asImage());
  for (int y = 0; y < 10; ++y)
    for (int x = 0; x < 12; ++x)
      CHECK(overlay.at<cv::Vec4b>(y, x)[3] == 47);
  CHECK(cvx::view(image).at<cv::Vec4b>(1, 1) == cv::Vec4b(10, 20, 30, 47));
  const auto roi = cvx::view(*call.outputs.at("roiMask").asImage());
  CHECK(roi.at<unsigned char>(1, 1) == 255);
  CHECK(roi.at<unsigned char>(2, 2) == 0);
}

TEST_CASE("P4 annotations bounding geometry excludes raster allocation padding") {
  AnnotationFiles files;
  auto value =
      annotationInstance("tight", "tight", Json::array({{{"polygon", rectangle(2, 2, 4, 4)}}}));
  value["bbox"] = {0, 0, 1, 1};
  auto data = annotation(Json::array({value}));
  data["rois"] = Json::array({{{"id", "tight-roi"}, {"polygon", rectangle(2, 2, 4, 4)}}});
  files.native(data);
  test::OpCall call(annotationRegistry());
  REQUIRE_MESSAGE(load(call, files).ok, call.status.message);
  CHECK(record(call, "instances")["items"][0]["position"] == Json::array({2.5, 2.5}));
  CHECK(record(call, "instances")["items"][0]["size"] == Json::array({5., 5.}));
  CHECK(record(call, "rois")["items"][0]["position"] == Json::array({3.5, 3.5}));
  CHECK(record(call, "rois")["items"][0]["size"] == Json::array({3., 3.}));
}

TEST_CASE(
    "P4 annotations zero raster support is missing mask and composes with segmentation keep mode") {
  AnnotationFiles files;
  const auto outer = rectangle(1, 1, 3, 3);
  const auto value =
      annotationInstance("empty-support", "person",
                         Json::array({{{"polygon", outer}, {"holes", Json::array({outer})}}}));
  files.native(annotation(Json::array({value})));
  test::OpCall call(annotationRegistry());
  REQUIRE_MESSAGE(load(call, files).ok, call.status.message);
  REQUIRE(record(call, "instances")["items"].size() == 1);
  CHECK(record(call, "instances")["items"][0]["attributes"]["hasMask"] == false);
  CHECK(record(call, "maskInfo")["items"][0]["hasMask"] == false);
  CHECK(record(call, "maskInfo")["items"][0]["source"] == "none");
  const auto& masks = *call.outputs.at("masks").asTensor();
  CHECK(masks.shape == std::vector<std::int64_t>{1, 10, 12});
  CHECK(std::all_of(masks.data.begin(), masks.data.end(), [](float value) { return value == 0; }));
  test::OpCall pre(annotationRegistry());
  pre.inputs = {{"image", call.outputs.at("image")},
                {"rois", call.outputs.at("instances")},
                {"roiMasks", call.outputs.at("masks")},
                {"roiMaskInfo", call.outputs.at("maskInfo")}};
  REQUIRE_MESSAGE(
      pre.run("image.segmentation_preprocess",
              {{"frame", Value::text("相机")}, {"replaceExistingMask", Value::boolean(false)}})
          .ok,
      pre.status.message);
  CHECK(pre.outputs.at("tensor").asTensor()->shape[0] == 1);
  CHECK(pre.outputs.at("retainedMasks").asTensor()->data.empty());
}

TEST_CASE(
    "P4 annotations validate complete hole support before clipping and preserve legal edge holes") {
  AnnotationFiles files;
  const auto outer = rectangle(1, 1, 3, 3), outside = rectangle(3.75, 1.5, 4, 2.5);
  auto value = annotationInstance(
      "outside-hole", "A", Json::array({{{"polygon", outer}, {"holes", Json::array({outside})}}}));
  files.native(annotation(Json::array({value})));
  test::OpCall call(annotationRegistry());
  CHECK(load(call, files).code == "bad_input");
  CHECK(call.outputs.empty());
  auto fractional =
      annotationInstance("edge-fractional", "A",
                         Json::array({{{"polygon", rectangle(1.25, 1.25, 3.75, 3.75)},
                                       {"holes", Json::array({rectangle(3.5, 1.5, 3.75, 2.5)})}}}));
  auto imageEdge =
      annotationInstance("edge-image", "B",
                         Json::array({{{"polygon", rectangle(8, 0, 12, 4)},
                                       {"holes", Json::array({rectangle(11.5, 1, 12, 2)})}}}));
  files.json("标注/工件.json", annotation(Json::array({fractional, imageEdge})));
  REQUIRE_MESSAGE(load(call, files).ok, call.status.message);
  const auto& masks = *call.outputs.at("masks").asTensor();
  CHECK(mask(masks, 0, 2, 4) == 0);
  CHECK(mask(masks, 0, 2, 3) == 1);
  CHECK(mask(masks, 0, 4, 4) == 1);
  CHECK(mask(masks, 1, 1, 11) == 1);
  CHECK(record(call, "maskInfo")["items"][0]["hasMask"] == true);
  CHECK(record(call, "maskInfo")["items"][1]["hasMask"] == true);
}

TEST_CASE(
    "P4 annotations source pickable filters without permission and stable IDs survive filtering") {
  AnnotationFiles files;
  auto a = annotationInstance("unused", "A");
  a.erase("id");
  a["pickable"] = false;
  auto b = annotationInstance("unused", "B");
  b.erase("id");
  b["occluded"] = true;
  auto c = annotationInstance("unused", "C");
  c.erase("id");
  auto data = annotation(Json::array({a, b, c}));
  files.native(data);
  test::OpCall call(annotationRegistry());
  REQUIRE_MESSAGE(
      load(call, files,
           {{"pickableOnly", Value::boolean(true)}, {"includeOccluded", Value::boolean(false)}})
          .ok,
      call.status.message);
  REQUIRE(record(call, "instances")["items"].size() == 1);
  CHECK(record(call, "instances")["items"][0]["id"] == "annotation-2");
  CHECK(record(call, "instances")["items"][0]["pickable"] == false);
  CHECK(record(call, "quality")["sourceCount"] == 3);
  CHECK(record(call, "quality")["filteredCount"] == 2);
  REQUIRE(load(call, files).ok);
  CHECK(record(call, "instances")["items"][2]["id"] == "annotation-2");
  const auto& masks = *call.outputs.at("masks").asTensor();
  CHECK(masks.shape == std::vector<std::int64_t>{3, 10, 12});
  CHECK(std::all_of(masks.data.begin(), masks.data.end(), [](float value) { return value == 0; }));
  for (const auto& item : record(call, "maskInfo")["items"]) {
    CHECK(item["hasMask"] == false);
    CHECK(item["source"] == "none");
  }
  CHECK(record(call, "rois")["items"].empty());
  CHECK(record(call, "quality")["roiProvided"] == false);
  files.json("标注/工件.json", annotation());
  REQUIRE(load(call, files).ok);
  CHECK(call.outputs.at("masks").asTensor()->shape == std::vector<std::int64_t>{0, 10, 12});
  CHECK(call.outputs.at("masks").asTensor()->data.empty());
  CHECK(record(call, "instances")["items"].empty());
  auto explicitEmpty = annotation();
  explicitEmpty["rois"] = Json::array();
  files.json("标注/工件.json", explicitEmpty);
  REQUIRE(load(call, files).ok);
  CHECK(record(call, "quality")["roiProvided"] == true);
  CHECK(record(call, "rois")["items"].empty());
}

TEST_CASE(
    "P4 annotations explicit XYZ CSV honors quoted Unicode filenames multipart and integer flags") {
  AnnotationFiles files;
  files.image("rgb/1700000000000,工件.png", cv::Mat(10, 12, CV_8UC3, cv::Scalar(30, 20, 10)));
  auto value = annotationInstance(
      "legacy-id", "unused",
      Json::array({{{"polygon", rectangle(1, 1, 3, 3)}}, {{"polygon", rectangle(7, 6, 9, 8)}}}));
  value.erase("id");
  value.erase("label");
  value.erase("classId");
  value.erase("confidence");
  value.erase("occluded");
  value["instance_category"] = "旧式连接件";
  value["pickable"] = 1;
  value["isoccluded"] = 0;
  Json data = {{"width", 12},
               {"height", 10},
               {"instances", Json::array({value})},
               {"roi", rectangle(0, 0, 11, 9)}};
  files.json("labels/旧式,标注.json", data);
  files.write(
      "samples_v2.txt",
      "\"rgb/1700000000000,工件.png\",not-read.depth,\"labels/旧式,标注.json\",ignored\r\n");
  test::OpCall call(annotationRegistry());
  ParamMap params = {{"path", Value::text(files.path("samples_v2.txt").u8string())},
                     {"format", Value::text("xyz-samples-v2")},
                     {"timestamp", Value::text("1700000000000")},
                     {"frame", Value::text("image")}};
  REQUIRE_MESSAGE(call.run("image.load_annotations", params).ok, call.status.message);
  CHECK(record(call, "instances")["items"][0]["classLabel"] == "旧式连接件");
  CHECK(record(call, "instances")["items"][0]["id"] == "annotation-0");
  CHECK(mask(*call.outputs.at("masks").asTensor(), 0, 2, 2) == 1);
  CHECK(mask(*call.outputs.at("masks").asTensor(), 0, 7, 8) == 1);
  CHECK(record(call, "instances")["items"][0]["pickable"] == false);
  files.write(
      "samples_v2.txt",
      "\"rgb/1700000000000,工件.png\",x,\"labels/旧式,标注.json\"\n\"rgb/1700000000000,工件.png\",x,\"labels/旧式,标注.json\"\n");
  CHECK_FALSE(call.run("image.load_annotations", params).ok);
  CHECK(call.outputs.empty());
  files.write("samples_v2.txt", "\"rgb/1700000000000,工件.png,x,annotation.json\n");
  CHECK_FALSE(call.run("image.load_annotations", params).ok);
  CHECK(call.outputs.empty());
}

TEST_CASE("P4 annotations index timestamp validation and relative path escapes fail atomically") {
  AnnotationFiles files;
  files.native(annotation());
  test::OpCall call(annotationRegistry());
  CHECK(load(call, files, {{"timestamp", Value::text("missing")}}).code == "bad_param");
  CHECK(call.outputs.empty());
  CHECK(load(call, files, {{"timestamp", Value::text("1700000000001")}}).code ==
        "sample_not_found");
  CHECK(call.outputs.empty());
  const Json entry = {
      {"timestamp", "0001700000000"}, {"image", "图像/工件.png"}, {"annotation", "标注/工件.json"}};
  files.json("索引.json", {{"schemaVersion", 1}, {"samples", Json::array({entry, entry})}});
  CHECK_FALSE(load(call, files).ok);
  CHECK(call.outputs.empty());
  for (const char* path : {"../escape.png", "x/../escape.png", "/absolute.png", "C:/escape.png",
                           "C:escape.png", "file.png:stream", "x\\..\\escape.png"}) {
    auto bad = entry;
    bad["image"] = path;
    files.json("索引.json", {{"schemaVersion", 1}, {"samples", Json::array({bad})}});
    CHECK_FALSE(load(call, files).ok);
    CHECK(call.outputs.empty());
  }
  for (const char* field : {"timestamp", "image", "annotation"}) {
    auto bad = entry;
    bad.erase(field);
    files.json("索引.json", {{"schemaVersion", 1}, {"samples", Json::array({bad})}});
    CHECK_FALSE(load(call, files).ok);
    CHECK(call.outputs.empty());
  }
}

TEST_CASE("P4 annotations resolved directory links cannot read another dataset") {
  AnnotationFiles files, outside;
  files.native(annotation());
  outside.native(annotation());
  const bool linked = directoryLink(outside.directory, files.path("escaped"));
#if defined(_WIN32)
  CAPTURE(GetLastError());
#endif
  REQUIRE(linked);
  files.json("索引.json", {{"schemaVersion", 1},
                           {"samples", Json::array({{{"timestamp", "0001700000000"},
                                                     {"image", "escaped/图像/工件.png"},
                                                     {"annotation", "标注/工件.json"}}})}});
  test::OpCall call(annotationRegistry());
  CHECK_FALSE(load(call, files).ok);
  CHECK(call.outputs.empty());
  files.json("索引.json", {{"schemaVersion", 1},
                           {"samples", Json::array({{{"timestamp", "0001700000000"},
                                                     {"image", "图像/工件.png"},
                                                     {"annotation", "escaped/标注/工件.json"}}})}});
  CHECK_FALSE(load(call, files).ok);
  CHECK(call.outputs.empty());
  REQUIRE(directoryLink(files.path("图像"), files.path("linked-images")));
  files.json("索引.json", {{"schemaVersion", 1},
                           {"samples", Json::array({{{"timestamp", "0001700000000"},
                                                     {"image", "linked-images/工件.png"},
                                                     {"annotation", "标注/工件.json"}}})}});
  REQUIRE_MESSAGE(load(call, files).ok, call.status.message);
}

TEST_CASE("P4 annotations size geometry flags and duplicate IDs reject before partial output") {
  AnnotationFiles files;
  const auto good = annotation(Json::array({annotationInstance("A", "A")}));
  files.native(good);
  test::OpCall call(annotationRegistry());
  std::vector<Json> invalid;
  auto changed = good;
  changed["width"] = 0;
  invalid.push_back(changed);
  changed = good;
  changed["width"] = 16385;
  invalid.push_back(changed);
  changed = good;
  changed["width"] = 12.5;
  invalid.push_back(changed);
  changed = good;
  changed["width"] = 13;
  invalid.push_back(changed);
  changed = good;
  changed["instances"][0]["bbox"] = {0, 0, 13, 1};
  invalid.push_back(changed);
  changed = good;
  changed["instances"][0]["bbox"] = {0, 0, 0, 1};
  invalid.push_back(changed);
  changed = good;
  changed["instances"][0]["pickable"] = 1;
  invalid.push_back(changed);
  changed = good;
  changed["instances"][0]["confidence"] = 1.1;
  invalid.push_back(changed);
  changed = good;
  changed["instances"].push_back(changed["instances"][0]);
  invalid.push_back(changed);
  changed = good;
  changed["instances"][0]["parts"] = Json::array({{{"polygon", rectangle(-1, 0, 3, 3)}}});
  invalid.push_back(changed);
  changed = good;
  changed["instances"][0]["parts"] = Json::array({{{"polygon", {{1, 1}, {2, 2}, {3, 3}}}}});
  invalid.push_back(changed);
  changed = good;
  changed["instances"][0]["parts"] = Json::array(
      {{{"polygon", rectangle(1, 1, 3, 3)}, {"holes", Json::array({rectangle(7, 6, 9, 8)})}}});
  invalid.push_back(changed);
  for (const auto& value : invalid) {
    files.json("标注/工件.json", value);
    CHECK_FALSE(load(call, files).ok);
    CHECK(call.outputs.empty());
  }
  files.write("标注/工件.json", std::string(4 * 1024 * 1024 + 1, ' '));
  CHECK(load(call, files).code == "resource_limit");
  CHECK(call.outputs.empty());
  files.json("标注/工件.json", good);
  files.write("图像/工件.png", "not an image");
  CHECK_FALSE(load(call, files).ok);
  CHECK(call.outputs.empty());
}

TEST_CASE("P4 annotations hole raster budget accounts for every full extent hole before filling") {
  AnnotationFiles files;
  const auto full = rectangle(0, 0, 511, 511);
  Json holes = Json::array();
  for (int i = 0; i < 1024; ++i)
    holes.push_back(full);
  auto value = annotationInstance("many-holes", "many",
                                  Json::array({{{"polygon", full}, {"holes", std::move(holes)}}}));
  value["bbox"] = {0, 0, 512, 512};
  files.native(annotation(Json::array({value}), 512, 512), 512, 512);
  test::OpCall call(annotationRegistry());
  CHECK(load(call, files).code == "resource_limit");
  CHECK(call.outputs.empty());
  Json many = Json::array();
  for (int i = 0; i < 129; ++i)
    many.push_back(annotationInstance("instance-" + std::to_string(i), "without-mask"));
  files.json("标注/工件.json", annotation(std::move(many), 512, 512));
  CHECK(load(call, files).code == "resource_limit");
  CHECK(call.outputs.empty());
}

TEST_CASE("P4 annotations cancellation and same dimension file replacement are truthful") {
  AnnotationFiles files;
  auto data = annotation(Json::array({annotationInstance("A", "first")}));
  files.native(data);
  test::OpCall call(annotationRegistry());
  REQUIRE_MESSAGE(load(call, files).ok, call.status.message);
  CHECK(record(call, "instances")["items"][0]["classLabel"] == "first");
  data["instances"][0]["label"] = "other";
  files.json("标注/工件.json", data);
  files.image("图像/工件.png", cv::Mat(10, 12, CV_8UC4, cv::Scalar(60, 50, 40, 47)));
  REQUIRE(load(call, files).ok);
  CHECK(record(call, "instances")["items"][0]["classLabel"] == "other");
  CHECK(cvx::view(*call.outputs.at("image").asImage()).at<cv::Vec4b>(0, 0) ==
        cv::Vec4b(40, 50, 60, 47));
  const auto* op = annotationRegistry().find("image.load_annotations");
  REQUIRE(op);
  CHECK_FALSE(op->capabilities.deterministic);
  for (int count : {1, 8}) {
    AnnotationCancel ctx;
    ctx.after = count;
    ParamMap params;
    for (const auto& p : op->params)
      params[p.name] = p.def;
    params["path"] = Value::text(files.path("索引.json").u8string());
    params["timestamp"] = Value::text("0001700000000");
    params["frame"] = Value::text("相机");
    const std::unordered_map<std::string, Data> input;
    std::unordered_map<std::string, Data> output;
    Inputs in(input);
    Outputs out(output);
    ParamView view(params, ctx.root);
    CHECK(op->compute(in, view, out, ctx).code == "cancelled");
    CHECK(output.empty());
  }
}
