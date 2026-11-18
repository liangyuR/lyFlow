#include "p3_example_helpers.h"

#include <cstring>
#include <map>
#include <set>

namespace {
using namespace lyflow;
using namespace lyflow::test;

struct Blob {
  std::vector<std::int64_t> shape;
  std::vector<std::uint8_t> bytes;
  std::string sourceId;
};
struct Snapshot {
  Json outputs;
  std::map<std::string, Blob> buffers;
};
struct RawCloud {
  lyflow_cloud_view value{};
  ~RawCloud() { lyflow_cloud_view_free(&value); }
};
struct RawTensor {
  lyflow_tensor_view value{};
  ~RawTensor() { lyflow_tensor_view_free(&value); }
};
struct RawIndices {
  lyflow_indices_view value{};
  ~RawIndices() { lyflow_indices_view_free(&value); }
};
struct RawImage {
  lyflow_image_view value{};
  ~RawImage() { lyflow_image_view_free(&value); }
};
template <class T> void append(Blob& blob, const T* data, std::size_t count) {
  if (!count)
    return;
  REQUIRE(data != nullptr);
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(data);
  blob.bytes.insert(blob.bytes.end(), bytes, bytes + count * sizeof(T));
}

struct CloudRole {
  std::string node, port, identity;
  std::string name() const { return node + "." + port; }
};
class Saved {
public:
  explicit Saved(const std::string& name) {
    static std::atomic<unsigned> counter{0};
    const std::filesystem::path examples(LYFLOW_EXAMPLES_DIR);
    base = std::filesystem::temp_directory_path() /
           std::filesystem::u8path(
               "lyflow-p5-保存-" + std::to_string(counter.fetch_add(1)) + "-" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    REQUIRE(std::filesystem::create_directory(base));
    for (const auto& entry : std::filesystem::directory_iterator(examples)) {
      if (!entry.is_regular_file())
        continue;
      const auto filename = entry.path().filename().u8string();
      if (filename.rfind("p5-", 0) == 0 || filename == "1700000001000-p5-scene.png")
        std::filesystem::copy_file(entry.path(), base / entry.path().filename());
    }
    graph = p3::read(examples / (name + ".lyflow.json"));
    const auto nodes = graph.at("nodes");
    for (const auto& node : nodes) {
      const auto* op = ensureRegistry().find(node.at("op").get<std::string>());
      REQUIRE(op != nullptr);
      for (const auto& port : op->outputs) {
        if (port.type != "PointCloud")
          continue;
        const auto identity = "__p5_identity_" + std::to_string(clouds.size());
        const auto nodeId = node.at("id").get<std::string>();
        clouds.push_back({nodeId, port.name, identity});
        graph["nodes"].push_back(
            {{"id", identity}, {"op", "cloud.statistics"}, {"params", {{"frame", "contract"}}}});
        graph["edges"].push_back({{"id", identity},
                                  {"from", {{"node", nodeId}, {"port", port.name}}},
                                  {"to", {{"node", identity}, {"port", "cloud"}}}});
        graph["outputs"][identity] = {{"node", identity}, {"port", "statistics"}};
      }
    }
    path = base / "saved.lyflow.json";
    {
      std::ofstream file(path, std::ios::binary);
      file << graph.dump(2);
      REQUIRE(file.good());
    }
    REQUIRE(p3::read(path) == graph);
  }
  ~Saved() {
    std::error_code error;
    std::filesystem::remove_all(base, error);
  }
  std::filesystem::path base, path;
  Json graph;
  std::vector<CloudRole> clouds;
};

Blob cloud(const std::string& runId, const CloudRole& role, Client* client, bool direct,
           const std::string& identity) {
  Blob result;
  if (direct) {
    Data value;
    REQUIRE(exec::ResultStore::instance().get(runId, role.node, role.port, value));
    const auto* native = value.asCloud();
    REQUIRE(native != nullptr);
    REQUIRE(native->channelsConsistent());
    CHECK(std::to_string(native->id) == identity);
    result.shape = {static_cast<std::int64_t>(native->pointCount()), native->hasIntensity() ? 1 : 0,
                    native->hasNormals() ? 1 : 0, native->hasRgb() ? 1 : 0};
    append(result, native->xyz.data(), native->xyz.size());
    append(result, native->intensity.data(), native->intensity.size());
    append(result, native->normals.data(), native->normals.size());
    append(result, native->rgb.data(), native->rgb.size());
  } else if (client) {
    auto view = client->cloud(runId, role.node, role.port);
    REQUIRE(view.valid());
    const auto count = view.pointCount();
    result.shape = {count, view.intensity() ? 1 : 0, view.normals() ? 1 : 0, view.rgb() ? 1 : 0};
    append(result, view.xyz(), 3 * count);
    if (view.intensity())
      append(result, view.intensity(), count);
    if (view.normals())
      append(result, view.normals(), 3 * count);
    if (view.rgb())
      append(result, view.rgb(), 3 * count);
  } else {
    RawCloud view;
    REQUIRE(lyflow_output_cloud(runId.c_str(), role.node.c_str(), role.port.c_str(), 0,
                                &view.value) == 0);
    const auto& raw = view.value;
    REQUIRE(raw.point_count == raw.total_points);
    result.shape = {raw.point_count, (raw.flags & 1) ? 1 : 0, (raw.flags & 2) ? 1 : 0,
                    (raw.flags & 4) ? 1 : 0};
    append(result, raw.xyz, 3 * raw.point_count);
    if (raw.flags & 1)
      append(result, raw.intensity, raw.point_count);
    if (raw.flags & 2)
      append(result, raw.normals, 3 * raw.point_count);
    if (raw.flags & 4)
      append(result, raw.rgb, 3 * raw.point_count);
  }
  return result;
}

void normalizeIds(Json& value, const std::map<std::string, std::string>& identities) {
  if (value.is_object()) {
    for (auto& field : value.items()) {
      if (field.key() == "sourceCloudId" || field.key() == "parentCloudId" ||
          field.key() == "inputCloudId" || field.key() == "outputCloudId" ||
          field.key() == "resultCloudId" || field.key() == "sliceCloudId") {
        REQUIRE(field.value().is_string());
        const auto original = field.value().get<std::string>();
        REQUIRE_MESSAGE(identities.count(original),
                        (field.key() + ": unbound runtime cloud " + original));
        field.value() = identities.at(original);
      } else if (field.key() != "attributes")
        normalizeIds(field.value(), identities);
    }
  } else if (value.is_array()) {
    for (auto& element : value)
      normalizeIds(element, identities);
  }
}

Snapshot snapshot(const Saved& saved, Json outputs, const std::string& runId, Client* client,
                  bool direct = false) {
  Snapshot result;
  std::map<std::string, std::set<std::string>> roles;
  std::map<std::string, Blob> originals;
  std::map<std::string, std::size_t> counts;
  for (const auto& role : saved.clouds) {
    const auto& data = p3::data(outputs, role.identity.c_str());
    const auto identity = data.at("sourceCloudId").get<std::string>();
    REQUIRE_FALSE(identity.empty());
    auto bytes = cloud(runId, role, client, direct, identity);
    CHECK(data.at("count").get<std::size_t>() == static_cast<std::size_t>(bytes.shape[0]));
    if (originals.count(identity)) {
      CHECK(originals.at(identity).shape == bytes.shape);
      CHECK(originals.at(identity).bytes == bytes.bytes);
    } else
      originals.emplace(identity, bytes);
    counts[identity] = static_cast<std::size_t>(bytes.shape[0]);
    roles[identity].insert(role.name());
    result.buffers.emplace(role.name(), std::move(bytes));
  }
  std::map<std::string, std::string> identities;
  for (const auto& item : roles) {
    std::string token = "@cloud";
    for (const auto& name : item.second)
      token += "|" + name;
    identities[item.first] = std::move(token);
  }
  for (const auto& output : saved.graph.at("outputs").items()) {
    const auto& metadata = outputs.at(output.key());
    REQUIRE_FALSE(metadata.value("missing", false));
    const auto node = output.value().at("node").get<std::string>();
    const auto port = output.value().at("port").get<std::string>();
    const auto type = metadata.at("type").get<std::string>();
    Blob data;
    Data native;
    if (direct)
      REQUIRE(exec::ResultStore::instance().get(runId, node, port, native));
    if (type == "Tensor") {
      if (direct) {
        const auto* tensor = native.asTensor();
        REQUIRE(tensor != nullptr);
        REQUIRE(tensor->consistent());
        data.shape = tensor->shape;
        append(data, tensor->data.data(), tensor->data.size());
        CHECK(metadata.at("value").value("scale", 1) == native.tensorScale());
      } else if (client) {
        auto view = client->tensor(runId, node, port);
        REQUIRE(view.valid());
        data.shape.assign(view.shape(), view.shape() + view.rank());
        CHECK(view.count() == view.total());
        append(data, view.data(), view.count());
      } else {
        RawTensor view;
        REQUIRE(lyflow_output_tensor(runId.c_str(), node.c_str(), port.c_str(), 0, 0,
                                     &view.value) == 0);
        data.shape.assign(view.value.shape, view.value.shape + view.value.rank);
        CHECK(view.value.count == view.value.total);
        append(data, view.value.data, view.value.count);
      }
    } else if (type == "Indices") {
      std::vector<std::int32_t> values;
      if (direct) {
        const auto* indices = native.asIndices();
        REQUIRE(indices != nullptr);
        data.sourceId = std::to_string(indices->sourceCloudId);
        values = indices->values;
      } else if (client) {
        auto view = client->indices(runId, node, port);
        REQUIRE(view.valid());
        data.sourceId = std::to_string(view.sourceCloudId());
        if (view.count())
          values.assign(view.values(), view.values() + view.count());
        CHECK(view.count() == view.total());
      } else {
        RawIndices view;
        REQUIRE(lyflow_output_indices(runId.c_str(), node.c_str(), port.c_str(), 0, 0,
                                      &view.value) == 0);
        data.sourceId = std::to_string(view.value.source_cloud_id);
        if (view.value.count)
          values.assign(view.value.values, view.value.values + view.value.count);
        CHECK(view.value.count == view.value.total);
      }
      REQUIRE(identities.count(data.sourceId));
      for (const auto index : values) {
        CHECK(index >= 0);
        CHECK(static_cast<std::size_t>(index) < counts.at(data.sourceId));
      }
      data.shape = {static_cast<std::int64_t>(values.size())};
      append(data, values.data(), values.size());
      data.sourceId = identities.at(data.sourceId);
    } else if (type == "Image") {
      if (direct) {
        const auto* image = native.asImage();
        REQUIRE(image != nullptr);
        REQUIRE(image->consistent());
        data.shape = {image->width,
                      image->height,
                      image->channels,
                      static_cast<std::int64_t>(image->depth),
                      static_cast<std::int64_t>(image->rowBytes()),
                      0,
                      image->width,
                      image->height,
                      0,
                      image->height};
        append(data, image->pixels.get(), image->byteSize());
        CHECK(metadata.at("value").value("scale", 1) == image->scale);
      } else if (client) {
        auto view = client->image(runId, node, port);
        REQUIRE(view.valid());
        data.shape = {view.width(),     view.height(),  view.channels(),  view.depth(),
                      view.rowBytes(),  view.level(),   view.fullWidth(), view.fullHeight(),
                      view.rowOffset(), view.rowCount()};
        REQUIRE(view.rowCount() == view.height());
        append(data, view.pixels(), view.rowBytes() * view.rowCount());
      } else {
        RawImage view;
        REQUIRE(lyflow_output_image(runId.c_str(), node.c_str(), port.c_str(), 0, 0, 0,
                                    &view.value) == 0);
        const auto& raw = view.value;
        data.shape = {raw.width, raw.height,     raw.channels,    raw.depth,      raw.row_bytes,
                      raw.level, raw.full_width, raw.full_height, raw.row_offset, raw.row_count};
        REQUIRE(raw.row_count == raw.height);
        append(data, raw.pixels, raw.row_bytes * raw.row_count);
      }
    } else {
      if (direct && type == "Record") {
        const auto* record = native.asRecord();
        REQUIRE(record != nullptr);
        const Json expectedRecord{
            {"kind", "Record"}, {"type", record->type}, {"data", record->data}};
        CHECK(metadata.at("value") == expectedRecord);
      } else if (direct && type == "Transform") {
        const auto* transform = native.asTransform();
        REQUIRE(transform != nullptr);
        CHECK(metadata.at("value").at("m") == Json(transform->m));
      }
      continue;
    }
    result.buffers.emplace(node + "." + port, std::move(data));
  }
  normalizeIds(outputs, identities);
  result.outputs = std::move(outputs);
  return result;
}
void agree(const Snapshot& expected, const Snapshot& actual) {
  REQUIRE(expected.outputs.size() == actual.outputs.size());
  for (const auto& output : expected.outputs.items()) {
    CAPTURE(output.key());
    CHECK(output.value() == actual.outputs.at(output.key()));
  }
  REQUIRE(expected.buffers.size() == actual.buffers.size());
  for (const auto& buffer : expected.buffers) {
    CAPTURE(buffer.first);
    const auto& other = actual.buffers.at(buffer.first);
    CHECK(buffer.second.shape == other.shape);
    CHECK(buffer.second.sourceId == other.sourceId);
    CHECK(buffer.second.bytes == other.bytes);
  }
}

using Check = std::function<void(const Json&, bool)>;
void cross(const std::string& name, const Json& overrides, const Check& check) {
  Saved saved(name);
  Client client(LYFLOW_TEST_RUNTIME);
  const auto source = saved.graph.dump(), base = saved.base.u8string();
  Snapshot defaults;
  for (int attempt = 0; attempt < 3; ++attempt) {
    CAPTURE(name);
    CAPTURE(attempt);
    const bool changed = attempt == 1;
    const Json params = changed ? overrides : Json::object();
    auto effective = saved.graph;
    for (const auto& parameter : params.items()) {
      REQUIRE(effective.at("params").contains(parameter.key()));
      effective["params"][parameter.key()]["default"] = parameter.value();
    }
    Session session(effective, saved.base, {}, false, 2, true);
    const auto& log = session.wait();
    REQUIRE_MESSAGE(log.runStatus() == "ok", Json(log.events).dump());
    const auto scheduler = p3::take(lyflow_run_outputs(log.runId.c_str()));
    check(scheduler, changed);
    const auto native = snapshot(saved, scheduler, log.runId, nullptr, true);
    if (attempt == 0)
      defaults = native;
    if (attempt == 2)
      agree(defaults, native);
    const auto id = name + "-abi-" + std::to_string(attempt), parameters = params.dump();
    REQUIRE(
        p3::take(lyflow_validate_params(source.c_str(), base.c_str(), parameters.c_str())).empty());
    lyflow_run_options options{};
    options.run_id = id.c_str();
    options.base_dir = base.c_str();
    options.params_json = parameters.c_str();
    options.max_parallel = 2;
    options.no_reuse = 1;
    const std::unique_ptr<lyflow_run, decltype(&lyflow_run_free)> run(
        lyflow_run_start(source.c_str(), &options, nullptr, nullptr), lyflow_run_free);
    REQUIRE(run != nullptr);
    lyflow_run_join(run.get());
    REQUIRE_MESSAGE(p3::take(lyflow_run_summary(id.c_str())).at("status") == "ok",
                    p3::take(lyflow_run_summary(id.c_str())).dump());
    const auto abi = p3::take(lyflow_run_outputs(id.c_str()));
    check(abi, changed);
    agree(native, snapshot(saved, abi, id, nullptr));
    RunOptions sdkOptions;
    sdkOptions.runId = id + "-sdk";
    sdkOptions.baseDir = base;
    sdkOptions.paramsJson = parameters;
    sdkOptions.maxParallel = 2;
    sdkOptions.noReuse = true;
    const auto result = client.run(source, sdkOptions);
    REQUIRE_MESSAGE(result.ok(), result.summary);
    const auto sdk = Json::parse(result.outputs);
    check(sdk, changed);
    agree(native, snapshot(saved, sdk, sdkOptions.runId, &client));
    CHECK(p3::read(saved.path) == saved.graph);
  }
}
} // namespace

namespace {
void pointCount(const Json& outputs, const char* name, std::size_t count, std::size_t stride = 31) {
  const auto& output = outputs.at(name);
  CHECK(output.at("type") == "PointCloud");
  CHECK(output.at("elementCount").get<std::size_t>() == count);
  CHECK(output.at("byteSize").get<std::size_t>() == count * stride);
}
const Json& truth() {
  static const auto value =
      p3::read(std::filesystem::path(LYFLOW_EXAMPLES_DIR) / "p5-fixtures.json");
  return value;
}
void projection(const Json& outputs) {
  pointCount(outputs, "sourceCloud", 3066);
  pointCount(outputs, "projectedCloud", 3063);
  CHECK(p3::data(outputs, "projectionQuality").at("visibleCount") == 3063);
  CHECK(outputs.at("sourceImage").at("value").at("channels") == 4);
  CHECK(outputs.at("depth").at("value").at("width") == 64);
  CHECK(outputs.at("depth").at("value").at("height") == 48);
}
}

TEST_CASE(
    "P5 physical depth defects fill and profiles preserve native lineage across saved interfaces") {
  cross("p5-depth-inspection", {{"defectMode", "up"}}, [](const Json& outputs, bool changed) {
    projection(outputs);
    pointCount(outputs, "cloud", 3072);
    CHECK(p3::data(outputs, "fillQuality").at("filledCount") == 9);
    CHECK(outputs.at("filledIndices").at("elementCount") == 9);
    const auto& result = p3::data(outputs, "defects");
    CHECK(result.at("areaModel") == "observed_depth_orthogonal_footprint");
    CHECK(result.at("state") == "defect");
    CHECK(result.at("good") == false);
    CHECK(result.at("valid") == true);
    const auto& items = result.at("items");
    REQUIRE(items.size() == (changed ? 1 : 2));
    for (const auto& item : items) {
      const auto direction = item.at("direction").get<std::string>();
      CHECK((direction == "up" || (!changed && direction == "down")));
      CHECK(item.at("exceeded").is_array());
    }
    const auto& features = p3::data(outputs, "features");
    const auto shape = Json::array({features.at("sliceCount")});
    CHECK(outputs.at("attributes").at("value").at("shape") == shape);
    CHECK(outputs.at("attributeValid").at("value").at("shape") == shape);
    CHECK(outputs.at("profiles").at("value").at("shape")[1] == 4);
    CHECK(outputs.at("supports").at("value").at("shape")[1] == 3);
    CHECK(outputs.at("profilePlot").at("type") == "Image");
    CHECK(outputs.at("sliceCloud").at("elementCount").get<std::size_t>() > 0);
  });
}
TEST_CASE(
    "P5 no op and missing depth compensation restore XYZ and optional channels across interfaces") {
  cross("p5-depth-filters", {{"mode", "fill"}, {"kernelSize", 7}},
        [](const Json& outputs, bool changed) {
          projection(outputs);
          pointCount(outputs, "cloud", changed ? 3072 : 3063);
          const auto& quality = p3::data(outputs, "quality");
          CHECK(quality.at("mode") == (changed ? "fill" : "mean"));
          CHECK(quality.at("filledPixels") == (changed ? 9 : 0));
          CHECK(quality.at("changedPixels") == (changed ? 9 : 0));
        });
}
TEST_CASE(
    "P5 signed orthographic grids and native instance masks retain all ROI groups through interfaces") {
  cross("p5-orthographic-roi", {{"outputMode", "normal_angle"}},
        [](const Json& outputs, bool changed) {
          for (const auto* name : {"sourceCloud", "heightCloud", "orthographicCloud"})
            pointCount(outputs, name, 3063);
          pointCount(outputs, "cloud", 94);
          CHECK(p3::data(outputs, "quality").at("outputMode") ==
                (changed ? "normal_angle" : "height"));
          CHECK(outputs.at("depth").at("value").at("min")[0].get<double>() < 0);
          CHECK(outputs.at("depth").at("value").at("max")[0].get<double>() > 0);
          CHECK(outputs.at("image").at("value").at("channels") == (changed ? 3 : 1));
          const auto& groups = p3::data(outputs, "groups").at("groups");
          REQUIRE(groups.size() == 3);
          CHECK(groups[0].at("id") == "patch-up");
          CHECK(groups[0].at("count") == 42);
          CHECK(groups[1].at("id") == "patch-down");
          CHECK(groups[1].at("count") == 36);
          CHECK(groups[2].at("id") == "hole-region");
          CHECK(groups[2].at("count") == 16);
        });
}
TEST_CASE(
    "P5 seeded box clouds and analytic partitions agree on geometry channels and source identities") {
  cross("p5-box-crop", {{"shape", "cylinder"}}, [](const Json& outputs, bool changed) {
    pointCount(outputs, "sourceCloud", 1200, 27);
    const auto count = outputs.at("cropped").at("elementCount").get<std::size_t>();
    if (changed) {
      CHECK(count > 0);
      CHECK(count < 1200);
    } else
      CHECK(count == 1200);
    pointCount(outputs, "cropped", count, 27);
    pointCount(outputs, "removed", count, 27);
    pointCount(outputs, "outside", 1200 - count, 27);
    pointCount(outputs, "cloud", 1200 - count, 27);
    const auto& groups = p3::data(outputs, "sampleGroups").at("groups");
    REQUIRE(groups.size() == 2);
    CHECK(groups[0].at("id") == "boxA");
    CHECK(groups[1].at("id") == "boxB");
  });
}
TEST_CASE("P5 registered rigid reference and explicit symmetry survive saved non rigid overrides") {
  cross("p5-pose-rigid", {{"currentC", {.02, .12, .01}}}, [](const Json& outputs, bool changed) {
    CHECK(p3::data(outputs, "accepted").at("value") == !changed);
    CHECK(outputs.at("transform").at("type") == "Transform");
    CHECK(p3::data(outputs, "reference").at("items")[2].at("position") == Json::array({0, .1, 0}));
    CHECK(p3::data(outputs, "current").at("items")[2].at("position") ==
          (changed ? Json::array({.02, .12, .01}) : Json::array({0, .1, 0})));
    for (const auto* name : {"flipped", "rotated", "extended"})
      p3::poseRecord(outputs.at(name));
  });
}
TEST_CASE(
    "P5 full camera geometry masks and arbitrary plane inverse preserve calibrated dimensions") {
  cross("p5-pose-project-plane", {{"liftMode", "center"}}, [](const Json& outputs, bool changed) {
    const auto& items = p3::data(outputs, "poses").at("items");
    REQUIRE(items.size() == 1);
    CHECK(items[0].at("id") == "plane-box");
    CHECK(items[0].at("frame") == "camera");
    CHECK(items[0].at("pickable") == false);
    CHECK(std::abs(items[0].at("position")[2].get<double>() - 1.01) <= 1e-8);
    if (changed)
      CHECK(items[0].at("size") == Json::array({.12, .08, .02}));
    CHECK(p3::data(outputs, "maskInfo").at("frame") == "image");
  });
}
TEST_CASE(
    "P5 observed circular apertures preserve physical hypotheses and native masks across interfaces") {
  cross("p5-circles-3d", {{"imagePath", "p5-circle-tilted.png"}},
        [](const Json& outputs, bool changed) {
          const auto& expected = truth().at("circles").at(changed ? "tilted" : "front");
          const auto& items = p3::data(outputs, "circles").at("items");
          REQUIRE(items.size() >= 1);
          CHECK(items.size() <= 2);
          bool correct = false;
          for (const auto& item : items) {
            double error = 0, cosine = 0;
            for (int i = 0; i < 3; ++i) {
              const double delta =
                  item.at("center")[i].get<double>() - expected.at("centerM")[i].get<double>();
              error += delta * delta;
              cosine += item.at("normal")[i].get<double>() *
                        expected.at("unorientedNormal")[i].get<double>();
            }
            correct = correct || (std::sqrt(error) <= .004 && std::abs(cosine) >= .98);
            CHECK(item.at("center")[2].get<double>() > 0);
            CHECK(item.at("radius") == .05);
          }
          CHECK(correct);
        });
}
TEST_CASE(
    "P5 mask associated cloud removal preserves unknown outside points and all channels through interfaces") {
  cross("p5-mask-remove", {{"topExpand", 2}}, [](const Json& outputs, bool changed) {
    pointCount(outputs, "sourceCloud", 3066);
    pointCount(outputs, "cloud", 3066 - (changed ? 104 : 78));
    const auto& quality = p3::data(outputs, "quality");
    CHECK(quality.at("removed") == (changed ? 104 : 78));
    CHECK(quality.at("unknown") == 1);
    CHECK(quality.at("outOfBoundsKept") == 1);
    CHECK(outputs.at("removedIndices").at("elementCount") == (changed ? 104 : 78));
  });
}
