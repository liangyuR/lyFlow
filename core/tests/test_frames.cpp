#include "helpers.h"
#include "runtime_frames.h"
#include "lyflow/c_api.h"
#include <fstream>
#include <limits>

using namespace lyflow;
using namespace lyflow::test;
namespace {
std::string takeFrameString(char* raw) {
  REQUIRE(raw != nullptr);
  std::string result(raw);
  lyflow_string_free(raw);
  return result;
}
struct OwnedFrame {
  lyflow_frame* value = nullptr;
  ~OwnedFrame() { lyflow_frame_free(value); }
};
struct OwnedSession {
  lyflow_session* value = nullptr;
  ~OwnedSession() { lyflow_session_free(value); }
};
struct FrameDirectory {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("lyflow-frame-test-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  FrameDirectory() { std::filesystem::create_directories(path); }
  ~FrameDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};
Json frameContext(const char* id = "f", const char* timestamp = "100") {
  return {{"frameId", id},
          {"timestampNs", timestamp},
          {"frame", "world"},
          {"calibrationVersion", "cal1"},
          {"context", {{"timestampKind", "explicit"}}}};
}
std::uint64_t seedFrame(const std::string& run) {
  auto& store = exec::ResultStore::instance();
  store.clear();
  PointCloud cloud;
  cloud.xyz = {1, 2, 3, 4, 5, 6};
  cloud.intensity = {7, 8};
  cloud.normals = {0, 0, 1, 0, 1, 0};
  cloud.rgb = {1, 2, 3, 4, 5, 6};
  const auto id = cloud.id;
  auto data = Data::cloud(std::move(cloud));
  store.put(run, "n", "cloud", run, data);
  store.put(run, "n", "alias", run, data);
  store.put(run, "n", "indices", run, Data::indices({id, {1, 0}}));
  store.put(run, "n", "groups", run,
            Data::record({"cloud.Groups.v1", {{"sourceCloudId", std::to_string(id)}}}));
  store.put(run, "n", "business", run,
            Data::record({"business.v1", {{"sourceCloudId", std::to_string(id)}}}));
  store.put(run, "n", "untyped", run, Data::record({"", {{"value", 42}}}));
  Tensor tensor;
  tensor.shape = {2};
  tensor.data = {1.5f, std::numeric_limits<float>::quiet_NaN()};
  store.put(run, "n", "tensor", run, Data::tensor(tensor).withTensorScale(4));
  Tensor empty;
  empty.shape = {1, 0, 3};
  store.put(run, "n", "empty", run, Data::tensor(empty));
  Measurement m;
  m.ok = false;
  m.value = std::numeric_limits<double>::quiet_NaN();
  m.message = "not observed";
  store.put(run, "n", "measurement", run, Data::measurement(m));
  Bundle bundle("FrameFields");
  bundle.set("point", Data::point2d({{3, 4}, Unit2D::Pixel}));
  bundle.set("line", Data::line2d({}));
  bundle.set("circle", Data::circle2d({}));
  bundle.set("box", Data::box2d({}));
  bundle.set("plane", Data::plane(Plane{}));
  bundle.set("transform", Data::transform(Transform{}));
  bundle.set("error", Data::error(Status::Error(Phase::Execute, "missing", "not observed")));
  store.put(run, "n", "bundle", run, Data::bundle(std::move(bundle)));
  for (auto depth : {PixelDepth::U8, PixelDepth::U16, PixelDepth::F32})
    for (int channels : {1, 3, 4}) {
      auto image = Image::allocate(2, 3, channels, depth);
      image.scale = 3;
      for (std::size_t i = 0; i < image.byteSize(); ++i)
        image.mutablePixels()[i] = static_cast<std::uint8_t>(i);
      store.put(run, "images",
                std::to_string(static_cast<int>(depth)) + "-" + std::to_string(channels), run,
                Data::image(image));
    }
  store.setNamedOutputs(
      run,
      {{"cloud", "n", "cloud"}, {"indices", "n", "indices"}, {"measurement", "n", "measurement"}});
  store.setRunInputs(run, {{"consumer", "model", data}});
  store.setDeclaredOutputs(run, {{"", "source", "missing"}});
  store.setSummary(run, R"({"status":"ok"})");
  return id;
}
void captureFixture(OwnedFrame& frame, const std::string& run,
                    const Json& context = frameContext()) {
  CHECK(
      takeFrameString(lyflow_frame_capture(run.c_str(), context.dump().c_str(), "{}", &frame.value))
          .empty());
  REQUIRE(frame.value != nullptr);
}
int32_t cancelled(void*) { return 1; }
}

TEST_CASE("Frame archives own native payloads preserve bits and rebind declared identities") {
  const char* run = "frame-native";
  const auto oldId = seedFrame(run);
  auto& store = exec::ResultStore::instance();
  PointCloud slice;
  slice.xyz = {2, 3, 4};
  const auto oldSliceId = slice.id;
  store.put(run, "n", "sliceCloud", run, Data::cloud(std::move(slice)));
  Bundle candidates("inspection.DefectCandidates.v1");
  candidates.set("regions", Data::record({"inspection.DefectRegions.v1",
                                          {{"sourceCloudId", std::to_string(oldId)}}}));
  store.put(run, "n", "candidates", run, Data::bundle(std::move(candidates)));
  const Json sliceReferences{{"sourceCloudId", std::to_string(oldId)},
                             {"sliceCloudId", std::to_string(oldSliceId)}};
  store.put(run, "n", "sliceInfo", run,
            Data::record({"inspection.ProfileSliceInfo.v1", sliceReferences}));
  store.put(run, "n", "features", run,
            Data::record({"inspection.ProfileFeatures.v1", sliceReferences}));
  OwnedFrame original;
  captureFixture(original, run);
  auto info = Json::parse(takeFrameString(lyflow_frame_metadata(original.value)));
  REQUIRE(info["injectedInputs"].size() == 1);
  std::size_t virtualFields = 0;
  for (const auto& entry : info["entries"])
    if (entry.value("virtual", false))
      ++virtualFields;
  CHECK(virtualFields == 8);
  CHECK(info["missing"].size() == 1);
  const auto inputNode = info["injectedInputs"][0]["sourceNode"].get<std::string>();
  FrameDirectory directory;
  auto path = (directory.path / "all.lyframe").u8string();
  CHECK(takeFrameString(lyflow_frame_save(original.value, path.c_str(), 0)).empty());
  CHECK_FALSE(takeFrameString(lyflow_frame_save(original.value, path.c_str(), 0)).empty());
  store.freeRun(run);
  store.clear();
  OwnedFrame loaded;
  CHECK(takeFrameString(lyflow_frame_load(path.c_str(), 0, &loaded.value)).empty());
  REQUIRE(loaded.value);
  const std::string view = lyflow_frame_run_id(loaded.value);
  auto get = [&](const char* node, const char* port) {
    Data result;
    REQUIRE(store.get(view, node, port, result));
    return result;
  };
  const auto cloud = get("n", "cloud");
  const auto newId = cloud.asCloud()->id;
  CHECK(newId != oldId);
  CHECK(cloud.asCloud()->xyz == std::vector<float>{1, 2, 3, 4, 5, 6});
  CHECK(cloud.asCloud()->intensity == std::vector<float>{7, 8});
  CHECK(cloud.asCloud()->normals.size() == 6);
  CHECK(cloud.asCloud()->rgb == std::vector<std::uint8_t>{1, 2, 3, 4, 5, 6});
  CHECK(get("n", "alias").asCloud()->id == newId);
  CHECK(get(inputNode.c_str(), "model").asCloud()->id == newId);
  CHECK(get("n", "indices").asIndices()->sourceCloudId == newId);
  CHECK(get("n", "groups").asRecord()->data["sourceCloudId"] == std::to_string(newId));
  CHECK(get("n", "candidates.regions").asRecord()->data["sourceCloudId"] == std::to_string(newId));
  const auto newSliceId = get("n", "sliceCloud").asCloud()->id;
  CHECK(newSliceId != oldSliceId);
  CHECK(newSliceId != newId);
  for (const char* port : {"sliceInfo", "features"}) {
    CAPTURE(std::string(port));
    const auto record = get("n", port);
    CHECK(record.asRecord()->data["sourceCloudId"] == std::to_string(newId));
    CHECK(record.asRecord()->data["sliceCloudId"] == std::to_string(newSliceId));
  }
  const auto resaved = (directory.path / "resaved.lyframe").u8string();
  CHECK(takeFrameString(lyflow_frame_save(loaded.value, resaved.c_str(), 0)).empty());
  CHECK(get("n", "business").asRecord()->data["sourceCloudId"] == std::to_string(oldId));
  CHECK(get("n", "untyped").asRecord()->type.empty());
  CHECK(get("n", "untyped").asRecord()->data["value"] == 42);
  const auto tensor = get("n", "tensor");
  CHECK(tensor.tensorScale() == 4);
  CHECK(std::isnan(tensor.asTensor()->data[1]));
  CHECK(get("n", "empty").asTensor()->shape == std::vector<std::int64_t>{1, 0, 3});
  CHECK(std::isnan(get("n", "measurement").asMeasurement()->value));
  CHECK(get("n", "bundle.point").asPoint2D()->unit == Unit2D::Pixel);
  CHECK(get("n", "bundle.error").asError()->code == "missing");
  for (int depth : {1, 2, 4})
    for (int channels : {1, 3, 4}) {
      const auto name = std::to_string(depth) + "-" + std::to_string(channels);
      const auto image = get("images", name.c_str());
      REQUIRE(image.asImage());
      CHECK(image.asImage()->scale == 3);
      CHECK(image.asImage()->byteSize() == std::size_t(6 * depth * channels));
      for (std::size_t i = 0; i < image.byteSize(); ++i)
        CHECK(image.asImage()->pixels.get()[i] == static_cast<std::uint8_t>(i));
    }
  auto bindings = Json::array({{{"sourceNode", inputNode},
                                {"sourcePort", "model"},
                                {"nodeId", "other"},
                                {"port", "cloud"}}});
  const auto inputs = runtime::frameInputs(loaded.value, bindings.dump());
  REQUIRE(inputs.size() == 1);
  CHECK(inputs[0].data.asCloud()->id == newId);
  CHECK_THROWS(runtime::frameInputs(loaded.value, Json::array({bindings[0], bindings[0]}).dump()));
  store.clear();
  CHECK(get("n", "cloud").asCloud()->id == newId);
}

TEST_CASE("Frame archive validation rejects corruption budgets and cancelled IO atomically") {
  seedFrame("frame-invalid");
  OwnedFrame frame;
  captureFixture(frame, "frame-invalid");
  FrameDirectory directory;
  auto file = (directory.path / "cancel.lyframe").u8string();
  auto error = Json::parse(
      takeFrameString(lyflow_frame_save_ex(frame.value, file.c_str(), 0, cancelled, nullptr)));
  CHECK(error["code"] == "cancelled");
  CHECK_FALSE(std::filesystem::exists(std::filesystem::u8path(file)));
  CHECK(std::filesystem::is_empty(directory.path));
  CHECK(takeFrameString(lyflow_frame_save(frame.value, file.c_str(), 0)).empty());
  OwnedFrame out;
  CHECK_FALSE(takeFrameString(lyflow_frame_load(file.c_str(), 1, &out.value)).empty());
  CHECK(out.value == nullptr);
  CHECK(Json::parse(takeFrameString(lyflow_frame_load_ex(file.c_str(), 0, cancelled, nullptr,
                                                         &out.value)))["code"] == "cancelled");
  std::ifstream input(std::filesystem::u8path(file), std::ios::binary);
  std::string raw((std::istreambuf_iterator<char>(input)), {});
  input.close();
  raw[0] = 'X';
  auto corrupt = (directory.path / "corrupt.lyframe").u8string();
  {
    std::ofstream output(std::filesystem::u8path(corrupt), std::ios::binary);
    output.write(raw.data(), raw.size());
  }
  CHECK_FALSE(takeFrameString(lyflow_frame_load(corrupt.c_str(), 0, &out.value)).empty());
  CHECK(out.value == nullptr);
  OwnedFrame bad;
  auto context = frameContext();
  context["timestampNs"] = 1;
  CHECK_FALSE(takeFrameString(
                  lyflow_frame_capture("frame-invalid", context.dump().c_str(), "{}", &bad.value))
                  .empty());
  CHECK(bad.value == nullptr);
  Json sourceNodes = Json::array();
  for (int i = 0; i < 1000; ++i)
    sourceNodes.push_back(std::string(80, 'a') + std::to_string(i));
  const auto options = Json{{"sourceNodes", sourceNodes}}.dump();
  auto overBudget = Json::parse(takeFrameString(lyflow_frame_capture(
      "frame-invalid", frameContext().dump().c_str(), options.c_str(), &bad.value)));
  CHECK(overBudget["code"] == "resource_limit");
  CHECK(bad.value == nullptr);
}

TEST_CASE("Session FIFO maintains isolation monotonic ordering capacity reset and cancellation") {
  seedFrame("frame-session");
  OwnedFrame a, b, older;
  captureFixture(a, "frame-session", frameContext("a", "100"));
  captureFixture(b, "frame-session", frameContext("b", "200"));
  captureFixture(older, "frame-session", frameContext("old", "50"));
  OwnedSession queue;
  CHECK(takeFrameString(lyflow_session_create(R"({"id":"camera","capacity":1})", &queue.value))
            .empty());
  CHECK(Json::parse(takeFrameString(lyflow_session_push(queue.value, a.value, 0)))["sequence"] ==
        "1");
  OwnedFrame retained;
  CHECK(takeFrameString(lyflow_session_front(queue.value, 0, &retained.value)).empty());
  REQUIRE(retained.value);
  CHECK(Json::parse(takeFrameString(lyflow_session_push(queue.value, b.value, 0)))["dropped"] == 1);
  CHECK(Json::parse(takeFrameString(lyflow_session_push(queue.value, older.value, 0)))["code"] ==
        "out_of_order");
  const auto before = takeFrameString(lyflow_session_info(queue.value));
  CHECK(Json::parse(takeFrameString(lyflow_session_push_ex(queue.value, b.value, 1, cancelled,
                                                           nullptr)))["code"] == "cancelled");
  CHECK(takeFrameString(lyflow_session_info(queue.value)) == before);
  OwnedFrame popped;
  CHECK(takeFrameString(lyflow_session_front(queue.value, 1, &popped.value)).empty());
  CHECK(Json::parse(takeFrameString(lyflow_session_push(queue.value, older.value, 0)))["code"] ==
        "out_of_order");
  CHECK(Json::parse(
            takeFrameString(lyflow_session_push(queue.value, older.value, 1)))["sequence"] == "3");
  CHECK(takeFrameString(lyflow_session_reset(queue.value)).empty());
  CHECK(Json::parse(takeFrameString(lyflow_session_info(queue.value)))["size"] == 0);
  CHECK(Json::parse(takeFrameString(lyflow_frame_metadata(retained.value)))["frameId"] == "a");
  CHECK_FALSE(Json::parse(takeFrameString(lyflow_frame_metadata(a.value))).contains("session"));
  OwnedSession other;
  CHECK(takeFrameString(lyflow_session_create(R"({"id":"camera","capacity":1})", &other.value))
            .empty());
  CHECK(Json::parse(takeFrameString(lyflow_session_info(other.value)))["epoch"] !=
        Json::parse(before)["epoch"]);
  for (auto policy : {"reject", "drop_newest"}) {
    OwnedSession bounded;
    Json opts{{"id", policy}, {"capacity", 1}, {"overflow", policy}};
    CHECK(takeFrameString(lyflow_session_create(opts.dump().c_str(), &bounded.value)).empty());
    CHECK(Json::parse(
              takeFrameString(lyflow_session_push(bounded.value, a.value, 0)))["accepted"] == true);
    const auto unchanged = takeFrameString(lyflow_session_info(bounded.value));
    CHECK(Json::parse(takeFrameString(
              lyflow_session_push(bounded.value, b.value, 0)))["accepted"] == false);
    CHECK(takeFrameString(lyflow_session_info(bounded.value)) == unchanged);
  }
}

TEST_CASE("Frame malformed ranges dimensions references and process reservations fail closed") {
  seedFrame("frame-preflight");
  OwnedFrame frame;
  captureFixture(frame, "frame-preflight");
  FrameDirectory directory;
  const auto path = directory.path / "original.lyframe";
  CHECK(takeFrameString(lyflow_frame_save(frame.value, path.u8string().c_str(), 0)).empty());
  std::ifstream input(path, std::ios::binary);
  std::string raw((std::istreambuf_iterator<char>(input)), {});
  input.close();
  REQUIRE(raw.size() >= 12);
  std::uint32_t headerSize = 0;
  for (int i = 0; i < 4; ++i)
    headerSize |= std::uint32_t(static_cast<unsigned char>(raw[8 + i])) << (8 * i);
  const auto manifest = Json::parse(raw.substr(12, headerSize));
  auto reject = [&](Json changed) {
    const auto header = changed.dump();
    const auto file = directory.path / "malformed.lyframe";
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    output.write(raw.data(), 8);
    const auto size = static_cast<std::uint32_t>(header.size());
    for (int i = 0; i < 4; ++i)
      output.put(static_cast<char>(size >> (8 * i)));
    output.write(header.data(), header.size());
    output.write(raw.data() + 12 + headerSize, raw.size() - 12 - headerSize);
    output.close();
    OwnedFrame bad;
    CHECK_FALSE(takeFrameString(lyflow_frame_load(file.u8string().c_str(), 0, &bad.value)).empty());
    CHECK(bad.value == nullptr);
  };
  auto changed = manifest;
  for (auto& entry : changed["entries"])
    if (entry["data"]["kind"] == "PointCloud" && entry["data"].contains("xyz"))
      entry["data"]["xyz"]["offset"] = std::uint64_t{1};
  reject(changed);
  changed = manifest;
  for (auto& entry : changed["entries"])
    if (entry["data"]["kind"] == "Image")
      entry["data"]["width"] = 2.5;
  reject(changed);
  changed = manifest;
  for (auto& entry : changed["entries"])
    if (entry["data"]["kind"] == "Indices")
      entry["data"]["cloudKey"] = "18446744073709551615";
  reject(changed);
  std::vector<std::unique_ptr<OwnedFrame>> retained;
  for (std::size_t i = 1; i < 1024; ++i) {
    auto next = std::make_unique<OwnedFrame>();
    captureFixture(*next, "frame-preflight");
    retained.push_back(std::move(next));
  }
  OwnedFrame denied;
  CHECK(Json::parse(takeFrameString(lyflow_frame_load(path.u8string().c_str(), 0,
                                                      &denied.value)))["code"] == "resource_limit");
  CHECK(denied.value == nullptr);
  retained.clear();
  CHECK(takeFrameString(lyflow_frame_load(path.u8string().c_str(), 0, &denied.value)).empty());
  REQUIRE(denied.value);
}
