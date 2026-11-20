#include "p3_example_helpers.h"

#include <map>

namespace {
using namespace lyflow;
using namespace lyflow::test;

struct ImagePayload {
  std::vector<std::uint32_t> format;
  std::vector<std::uint8_t> values;
};
struct Payloads {
  std::map<std::string, p3::TensorPayload> tensors;
  std::map<std::string, ImagePayload> images;
};
struct RawImage {
  lyflow_image_view view{};
  ~RawImage() { lyflow_image_view_free(&view); }
};

Payloads payloads(const Json& graph, const Json& outputs, const std::string& id, Client* client) {
  Payloads result;
  for (const auto& output : graph.at("outputs").items()) {
    const auto name = output.key();
    const auto node = output.value().at("node").get<std::string>();
    const auto port = output.value().at("port").get<std::string>();
    const auto& metadata = outputs.at(name);
    REQUIRE_FALSE(metadata.value("missing", false));
    if (metadata.at("type") == "Tensor") {
      result.tensors.emplace(name, p3::tensorPayload(id, node, port, client));
    } else if (metadata.at("type") == "Image") {
      ImagePayload value;
      if (client) {
        auto view = client->image(id, node, port);
        REQUIRE(view.valid());
        value.format = {view.width(), view.height(), view.channels(), view.depth()};
        REQUIRE(view.rowCount() == view.height());
        value.values.assign(view.pixels(), view.pixels() + view.rowBytes() * view.rowCount());
      } else {
        RawImage image;
        REQUIRE(lyflow_output_image(id.c_str(), node.c_str(), port.c_str(), 0, 0, 0, &image.view) ==
                0);
        const auto& view = image.view;
        value.format = {view.width, view.height, view.channels, view.depth};
        REQUIRE(view.row_count == view.height);
        value.values.assign(view.pixels, view.pixels + view.row_bytes * view.row_count);
      }
      CHECK_FALSE(value.values.empty());
      result.images.emplace(name, std::move(value));
    }
  }
  return result;
}

void agree(const Payloads& expected, const Payloads& actual) {
  REQUIRE(expected.tensors.size() == actual.tensors.size());
  REQUIRE(expected.images.size() == actual.images.size());
  for (const auto& item : expected.tensors) {
    CHECK(item.second.shape == actual.tensors.at(item.first).shape);
    CHECK(item.second.values == actual.tensors.at(item.first).values);
  }
  for (const auto& item : expected.images) {
    CHECK(item.second.format == actual.images.at(item.first).format);
    CHECK(item.second.values == actual.images.at(item.first).values);
  }
}

class Saved {
public:
  explicit Saved(const std::string& name, bool models) {
    static std::atomic<unsigned> counter{0};
    base = std::filesystem::temp_directory_path() /
           std::filesystem::u8path(
               "lyflow-p4-保存-" + std::to_string(counter.fetch_add(1)) + "-" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    REQUIRE(std::filesystem::create_directory(base));
    const std::filesystem::path examples(LYFLOW_EXAMPLES_DIR);
    for (const auto& entry : std::filesystem::directory_iterator(examples)) {
      if (!entry.is_regular_file())
        continue;
      const auto filename = entry.path().filename().u8string();
      if (filename.rfind("p4-", 0) == 0 || filename == "1700000000000-工件,样例.png")
        std::filesystem::copy_file(entry.path(), base / entry.path().filename());
    }
    graph = p3::read(examples / (name + ".lyflow.json"));
    if (models) {
      const char* configured = std::getenv("LYFLOW_P4_ASSETS");
      REQUIRE(configured != nullptr);
      const auto directory = std::filesystem::absolute(std::filesystem::u8path(configured));
      const auto rewrite = [&](auto&& self, Json& value) -> void {
        if (value.is_string()) {
          const auto text = value.get<std::string>();
          if (text.rfind("p4-assets/", 0) == 0) {
            const auto path = directory / std::filesystem::u8path(text.substr(10));
            REQUIRE(std::filesystem::is_regular_file(path));
            value = path.u8string();
          }
        } else if (value.is_structured()) {
          for (auto& item : value)
            self(self, item);
        }
      };
      rewrite(rewrite, graph);
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
};

void cross(const std::string& name, bool models, const Json& overrides, const p3::Check& check) {
  const Saved saved(name, models);
  std::array<Payloads, 2> baselines;
  p3::crossInterfaces(name, saved.graph, saved.path, overrides, check,
                      [&](const Json& outputs, const std::string& runId, Client* client,
                          bool changed, bool baseline) {
                        auto actual = payloads(saved.graph, outputs, runId, client);
                        auto& expected = baselines[changed ? 1 : 0];
                        if (baseline) {
                          for (const auto& entry : actual.tensors) {
                            CAPTURE(entry.first);
                            const auto& values = entry.second.values;
                            CHECK(std::all_of(values.begin(), values.end(),
                                              [](float number) { return std::isfinite(number); }));
                          }
                          expected = std::move(actual);
                        } else {
                          agree(expected, actual);
                        }
                      });
}

void annotations(const Json& outputs, bool changed, bool legacy) {
  const auto& instances = p3::data(outputs, "instances").at("items");
  REQUIRE(instances.size() == (changed ? 1 : 3));
  CHECK(instances.back().at("classLabel") == "缺掩膜");
  const auto& info = p3::data(outputs, "maskInfo").at("items");
  REQUIRE(info.size() == instances.size());
  CHECK_FALSE(info.back().at("hasMask").get<bool>());
  for (const auto& instance : instances)
    CHECK_FALSE(instance.at("pickable").get<bool>());
  CHECK(outputs.at("image").at("value").at("channels") == 4);
  CHECK(outputs.at("masks").at("type") == "Tensor");
  CHECK(instances[0].at("id") == (changed ? (legacy ? "annotation-2" : "candidate")
                                          : (legacy ? "annotation-0" : "plate-A")));
}
} // namespace

TEST_CASE(
    "P4 native annotations preserve image and overlapping mask payloads across saved interfaces") {
  cross("p4-annotations-native", false, {{"pickableOnly", true}, {"includeOccluded", false}},
        [](const Json& outputs, bool changed) { annotations(outputs, changed, false); });
}
TEST_CASE("P4 legacy annotation timestamp and filtered stable ids survive saved interfaces") {
  cross("p4-annotations-legacy", false, {{"pickableOnly", true}, {"includeOccluded", false}},
        [](const Json& outputs, bool changed) { annotations(outputs, changed, true); });
}

TEST_CASE(
    "P4 barcode formats and original pixel overlays agree through saved workflow interfaces") {
  cross("p4-barcodes", false, {{"formats", "QRCodeModel2"}}, [](const Json& outputs, bool changed) {
    const auto& items = p3::data(outputs, "detections").at("items");
    REQUIRE(items.size() == (changed ? 1 : 2));
    CHECK(std::any_of(items.begin(), items.end(),
                      [](const Json& item) { return item.at("text") == "LyFlow-P4"; }));
    if (!changed)
      CHECK(std::any_of(items.begin(), items.end(),
                        [](const Json& item) { return item.at("text") == "AAAA"; }));
    CHECK(p3::data(outputs, "instances").at("items").size() == items.size());
  });
}
TEST_CASE("P4 QR refinement module size and before after feedback survive isolated overrides") {
  cross("p4-qr-refine", false, {{"modulePixels", 4}}, [](const Json& outputs, bool changed) {
    CHECK(outputs.at("image").at("value").at("width") == (changed ? 116 : 232));
    CHECK(outputs.at("image").at("value").at("height") == (changed ? 116 : 232));
    CHECK_FALSE(p3::data(outputs, "feedback").at("improved").get<bool>());
    const auto& items = p3::data(outputs, "after").at("items");
    REQUIRE(items.size() == 1);
    CHECK(items[0].at("text") == "LyFlow-P4");
  });
}
#if defined(LYFLOW_P4_MODEL_TESTS)
TEST_CASE(
    "P4 real semantic graph restores native masks after min area overrides across interfaces") {
  cross("p4-segmentation", true, {{"minArea", 1048576}}, [](const Json& outputs, bool changed) {
    const auto& items = p3::data(outputs, "instances").at("items");
    CHECK(changed ? items.empty() : !items.empty());
    CHECK(p3::data(outputs, "maskInfo").at("items").size() == items.size());
    CHECK(p3::data(outputs, "quality").at("roiCount") == 1);
    CHECK(p3::data(outputs, "quality").at("retainedCount") == 0);
  });
}
TEST_CASE("P4 annotation masks are retained or replaced per ROI across saved workflow interfaces") {
  cross("p4-segmentation-retained", true, {{"replaceExistingMask", true}},
        [](const Json& outputs, bool changed) {
          const auto& quality = p3::data(outputs, "quality");
          CHECK(quality.at("retainedCount") == (changed ? 0 : 2));
          CHECK(quality.at("roiCount") == (changed ? 3 : 1));
          CHECK(p3::data(outputs, "annotationInstances").at("items").size() == 3);
          if (!changed) {
            const auto& items = p3::data(outputs, "instances").at("items");
            REQUIRE(items.size() >= 2);
            CHECK(items[0].at("id") == "plate-A");
            CHECK(items[1].at("id") == "plate-B");
          }
        });
}
TEST_CASE("P4 complete real OCR graph preserves Chinese geometry direction and character filters") {
  cross("p4-ocr", true, {{"characterSet", "digits"}}, [](const Json& outputs, bool changed) {
    auto text = p3::data(outputs, "text").at("value").get<std::string>();
    text.erase(std::remove(text.begin(), text.end(), ' '), text.end());
    CHECK(text == (changed ? "12345\n12345\n2026"
                           : "LYFLOW12345\nINDUSTRIALOCR\n工业视觉12345\nHELLO2026"));
    const auto& items = p3::data(outputs, "results").at("items");
    REQUIRE(items.size() == 4);
    CHECK(items[3].at("orientation") == 180);
    CHECK(items[2].at("rawText").get<std::string>().find("工业视觉") != std::string::npos);
    CHECK(outputs.at("quads").at("type") == "Tensor");
  });
}
#endif
