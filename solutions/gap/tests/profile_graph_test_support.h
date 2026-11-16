#pragma once

#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <string>
#include <vector>

#include "helpers.h"

namespace lyflow::test::gap_graph_test {
using Json = nlohmann::json;
inline constexpr const char* kRunConfig = R"(
common_settings:
  seg_mode: ROI
  overall_roi: [-30, 150, 30, 180]
  overall_roi_mode: fixed
  line_fit_distance: 0.05
  circle_fit_distance: 0.03
  using_camera: Both
  roll_anchored_crop: {enabled: false}
  filter: {using_removal: false}
flush:
  base_type: fit line
  ref_type: line end
  base_roi: [-18, 164.5, -8, 165.5]
  ref_roi: [8, 163.5, 18, 164.5]
  segment_points: 0
  offset: 0.0
  tolerances: {nominal: -1, up_deviation: 0.2, low_deviation: -0.2}
gap:
  left_type: circle
  right_type: circle
  definition: B
  left_roi: [-3.2, 164.8, -1.8, 166.2]
  right_roi: [1.8, 163.8, 3.2, 165.2]
  offset: 0
  radius: {left_circle_radius_min: 0.8, left_circle_radius_max: 1.2, right_circle_radius_min: 0.8, right_circle_radius_max: 1.2}
  tolerances: {nominal: 4, up_deviation: 0.2, low_deviation: -0.2}
align:
  align_cloud: true
  ICP: {num_neighbor: 8, min_score: 20, max_matching_dist: 2, max_fitness_dist: 0.5, max_iteration_num: 100}
  template_candidates:
    - {id: f1, left: left_template.pcd, right: right_template.pcd}
)";
inline std::string varint(std::uint64_t value) {
  std::string out;
  while (value > 127) {
    out.push_back(static_cast<char>((value & 127) | 128));
    value >>= 7;
  }
  out.push_back(static_cast<char>(value));
  return out;
}
inline std::string protoInteger(unsigned field, std::uint64_t value) {
  return varint(field << 3) + varint(value);
}
inline std::string protoBytes(unsigned field, const std::string& value) {
  return varint((field << 3) | 2) + varint(value.size()) + value;
}
inline std::string tensorInfo(const std::string& name, std::initializer_list<int> dimensions) {
  std::string shape;
  for (const auto d : dimensions)
    shape += protoBytes(1, protoInteger(1, d));
  return protoBytes(1, name) +
         protoBytes(2, protoBytes(1, protoInteger(1, 1) + protoBytes(2, shape)));
}
struct RuntimeFixture {
  std::filesystem::path root;
  std::vector<std::array<double, 2>> points;
  RuntimeFixture() {
    root = std::filesystem::temp_directory_path() / "lyflow-native-import-runtime" /
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(root / "StandardGap");
    constexpr double pi = 3.14159265358979323846;
    for (int i = 0; i < 320; ++i)
      points.push_back({-20 + 17. * i / 319, 165 + .005 * std::sin(i * .1)});
    for (int i = 0; i < 320; ++i) {
      const double theta = .5 * pi * i / 319;
      points.push_back({-3 + std::sin(theta), 166 - std::cos(theta)});
    }
    for (int i = 0; i < 320; ++i) {
      const double theta = .5 * pi * (319 - i) / 319;
      points.push_back({3 - std::sin(theta), 165 - std::cos(theta)});
    }
    for (int i = 0; i < 320; ++i)
      points.push_back({3 + 17. * i / 319, 164 + .005 * std::sin(i * .1)});
    write(root / "LaserProfile_L0_Master_x.pcd", true, 0);
    write(root / "LaserProfile_R1_Slave_x.pcd", true, 0);
    write(root / "StandardGap" / "left_template.pcd", false, -1);
    write(root / "StandardGap" / "right_template.pcd", false, 1);
  }
  ~RuntimeFixture() {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }
  void write(const std::filesystem::path& path, bool sensor, int side) {
    std::vector<std::size_t> indices;
    for (std::size_t i = 0; i < points.size(); ++i)
      if (side == 0 || (side < 0 ? points[i][0] < 0 : points[i][0] > 0))
        indices.push_back(i);
    std::ofstream output(path);
    output << "VERSION 0.7\nFIELDS x y z rgb\nSIZE 4 4 4 4\nTYPE F F F U\nCOUNT 1 1 1 1\nWIDTH "
           << indices.size() << "\nHEIGHT 1\nPOINTS " << indices.size() << "\nDATA ascii\n"
           << std::setprecision(9);
    for (const auto i : indices) {
      if (i == 7) {
        output << "nan nan nan 6579300\n";
        continue;
      }
      output << points[i][0] / 1000 << ' ' << (sensor ? 0 : points[i][1] / 1000) << ' '
             << (sensor ? points[i][1] / 1000 : 0) << " 6579300\n";
    }
  }
  std::filesystem::path model(bool missing = false) {
    std::vector<float> scores(2 * 8 * 1280, 0);
    for (int row = 0; row < 2; ++row)
      for (int i = 0; i < 1280; ++i) {
        const int cls = missing ? 0 : (i < 320 ? 2 : i < 640 ? 3 : i < 960 ? 5 : 6);
        scores[row * 10240 + cls * 1280 + i] = 1;
      }
    std::string tensor;
    for (const auto d : {2, 8, 1280})
      tensor += protoInteger(1, d);
    tensor += protoInteger(2, 1) + protoBytes(8, "constant") +
              protoBytes(9, std::string(reinterpret_cast<const char*>(scores.data()),
                                        scores.size() * sizeof(float)));
    const auto node =
        protoBytes(1, "constant") + protoBytes(2, "output") + protoBytes(4, "Identity");
    const auto graph = protoBytes(1, node) + protoBytes(2, "lyflow_import_native_fixture") +
                       protoBytes(5, tensor) + protoBytes(11, tensorInfo("input", {2, 6, 1280})) +
                       protoBytes(12, tensorInfo("output", {2, 8, 1280}));
    const auto bytes = protoInteger(1, 8) + protoBytes(2, "lyflow-synthetic-import-contract") +
                       protoBytes(7, graph) + protoBytes(8, protoInteger(2, 13));
    const auto path = root / (missing ? "empty-labels.onnx" : "valid-labels.onnx");
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), bytes.size());
    output.close();
    REQUIRE(output.good());
    return path;
  }
  Json graph(const std::string& kind, const std::string& yaml = kRunConfig) {
    const auto* entry = ensureRegistry().findImporter(kind);
    REQUIRE(entry);
    std::string out;
    const auto status = entry->fn(yaml, root, out);
    REQUIRE_MESSAGE(status.ok, status.message);
    return Json::parse(out);
  }
};
inline Data result(const test::Session& session, const char* node, const char* port) {
  Data value;
  REQUIRE_MESSAGE(exec::ResultStore::instance().get(session.runId(), node, port, value),
                  (std::string(node) + "." + port));
  return value;
}
inline void requireRun(const test::RunLog& log) {
  for (const auto& event : log.ofKind("node_state"))
    if (event.value("state", "") == "error")
      MESSAGE(event.dump());
  CHECK(log.runStatus() == "ok");
}

}
