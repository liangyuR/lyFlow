#include <doctest/doctest.h>

#include <cstring>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <limits>
#include <set>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "exec/executor.h"
#include "exec/result_store.h"
#include "helpers.h"
#include "profile_graph_test_support.h"
#include "lyflow/data.h"
#include "lyflow/operator.h"
#include "lyflow/registry.h"
#include "ops/profile_scene.h"
#include "ops/profile_template.h"

namespace lyflow::packs::gap {
void registerPackOps(Registry& r);
void registerStandardGapImporter(Registry& r);
} // namespace lyflow::packs::gap

namespace {

using namespace lyflow;

const Registry& packRegistry() {
  static Registry r = [] {
    Registry reg;
    packs::gap::registerPackOps(reg);
    return reg;
  }();
  return r;
}

constexpr const char* kConfig = R"(
common_settings:
  seg_mode: ROI
  overall_roi: [-10, 20, 30, 40]
  overall_roi_mode: auto_center
  line_fit_distance: 0.1
  circle_fit_distance: 0.03
  using_camera: Both
  roll_anchored_crop:
    enabled: true
    half_width_mm: 15
    half_height_mm: 5
    min_points_kept: 50
    max_roll_box_height_mm: 8
  filter:
    using_removal: true
    filter_radius: 0.4
    filter_neighbors: 5
flush:
  base_side: left
  base_type: fit line
  ref_type: line end
  base_roi: [0, 160, 5, 170]
  ref_roi: [13, 160, 18, 170]
  segment_points: 1000
  offset: 0.5
  tolerances: {nominal: 1, up_deviation: 1.3, low_deviation: -1.3}
gap:
  left_type: circle
  right_type: circle
  definition: A
  left_roi: [4, 162, 8, 170]
  right_roi: [9, 162, 14, 170]
  offset: 0
  radius: {left_circle_radius_min: 0.4, left_circle_radius_max: 1.5,
           right_circle_radius_min: 0.8, right_circle_radius_max: 2.5}
  tolerances: {nominal: 4, up_deviation: 1.3, low_deviation: -1.3}
align:
  align_cloud: true
  success_guide: true
  ICP: {num_neighbor: 30, min_score: 80, max_matching_dist: 10,
        max_fitness_dist: 0.5, max_iteration_num: 1000, bidirection_align: true}
  template_candidates:
    - {id: f1, left: left_template.pcd, right: right_template.pcd}
)";

struct Fixture {
  explicit Fixture(const std::string& tag, const char* settingBody = nullptr) {
    root = std::filesystem::temp_directory_path() / ("lyflow-import-test-" + tag);
    std::filesystem::remove_all(root);
    point = root / "R4";
    std::filesystem::create_directories(point);
    std::ofstream(point / "StandardGap.yml") << kConfig;
    if (settingBody != nullptr)
      std::ofstream(root / "setting.yml") << settingBody;
  }
  ~Fixture() {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }

  std::filesystem::path root;
  std::filesystem::path point;
};

const ImporterDesc* importer(const std::string& kind) {
  static Registry r = [] {
    Registry reg;
    packs::gap::registerStandardGapImporter(reg);
    return reg;
  }();
  return r.findImporter(kind);
}

nlohmann::json import(const std::string& kind, const std::filesystem::path& baseDir,
                      Status* status = nullptr) {
  const ImporterDesc* desc = importer(kind);
  REQUIRE(desc != nullptr);
  std::string out;
  const Status s = desc->fn(kConfig, baseDir, out);
  if (status != nullptr)
    *status = s;
  if (!s.ok)
    return nlohmann::json();
  return nlohmann::json::parse(out);
}

bool hasOp(const nlohmann::json& doc, const char* op) {
  for (const auto& n : doc["nodes"]) {
    if (n["op"] == op)
      return true;
  }
  return false;
}

std::vector<std::string> idsWithOp(const nlohmann::json& doc, const char* op) {
  std::vector<std::string> ids;
  for (const auto& n : doc["nodes"]) {
    if (n["op"] == op)
      ids.push_back(n["id"].get<std::string>());
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

Box2D box(float xMin, float yMin, float xMax, float yMax) {
  Box2D b;
  b.min[0] = xMin;
  b.min[1] = yMin;
  b.max[0] = xMax;
  b.max[1] = yMax;
  return b;
}

using Json = nlohmann::json;
using namespace lyflow::test::gap_graph_test;

} // namespace

TEST_CASE(
    "导入器为每种模式注册积木与细粒度 kind；导入的图每个节点的 opVersion 等于 manifest 里该算子的版本") {
  for (const char* kind :
       {"StandardGap.yml", "StandardGap.yml:template", "StandardGap.yml:model",
        "StandardGap.yml:fine", "StandardGap.yml:template:fine", "StandardGap.yml:model:fine"}) {
    CAPTURE(kind);
    const ImporterDesc* d = importer(kind);
    REQUIRE(d != nullptr);
    CHECK(d->fn != nullptr);
    CHECK(!d->label.empty());
  }

  std::unordered_map<std::string, std::string> versions;
  const nlohmann::json manifest = nlohmann::json::parse(ensureRegistry().toManifestJson());
  for (const auto& o : manifest["operators"]) {
    versions[o["id"].get<std::string>()] = o["version"].get<std::string>();
  }
  Fixture f("opversion", "model_roi:\n  enabled: true\n  model_path: X:/m.onnx\n");
  for (const char* kind : {"StandardGap.yml:template", "StandardGap.yml:template:fine",
                           "StandardGap.yml:model", "StandardGap.yml:model:fine"}) {
    CAPTURE(kind);
    const nlohmann::json doc = import(kind, f.point);
    REQUIRE(doc.contains("nodes"));
    CHECK(doc["nodes"].size() >= 10);
    for (const auto& n : doc["nodes"]) {
      const std::string op = n["op"].get<std::string>();
      CAPTURE(op);
      REQUIRE(n.contains("opVersion"));
      REQUIRE(versions.count(op) == 1);
      CHECK(n["opVersion"] == versions.at(op));
    }
  }
}

TEST_CASE("auto 的模式推导只看 setting.yml 的 model_roi.enabled") {
  SUBCASE("找不到 setting.yml 就退回模板路径") {
    Fixture f("nosetting");
    const nlohmann::json doc = import("StandardGap.yml", f.point);
    CHECK(hasOp(doc, "gap.locate_template"));
    CHECK_FALSE(hasOp(doc, "gap.locate_model"));
    CHECK_FALSE(hasOp(doc, "flow.fallback"));
    const nlohmann::json fine = import("StandardGap.yml:fine", f.point);
    CHECK(hasOp(fine, "gap.business_rois"));
    CHECK_FALSE(hasOp(fine, "gap.roi_from_labels"));
  }
  SUBCASE("enabled: false 也退回模板路径") {
    Fixture f("off", "model_roi:\n  enabled: false\n  model_path: X:/m.onnx\n");
    const nlohmann::json doc = import("StandardGap.yml", f.point);
    CHECK(hasOp(doc, "gap.locate_template"));
    CHECK_FALSE(hasOp(doc, "gap.locate_model"));
  }
  SUBCASE("enabled: true 走模型路径，模型来自 model_path") {
    Fixture f("on", "model_roi:\n  enabled: true\n  model_path: X:/m.onnx\n");
    for (const char* kind : {"StandardGap.yml", "StandardGap.yml:fine"}) {
      CAPTURE(kind);
      const nlohmann::json doc = import(kind, f.point);
      CHECK((hasOp(doc, "gap.locate_model") || hasOp(doc, "gap.roi_from_labels")));
      // modelPath 由顶层参数给，节点上不再写
      CHECK(doc["params"]["modelPath"]["default"] == "X:/m.onnx");
      for (const auto& n : doc["nodes"]) {
        if (n["op"] == "gap.infer_profile" || n["op"] == "gap.locate_model") {
          CHECK_FALSE(n.value("params", nlohmann::json::object()).contains("modelPath"));
        }
      }
    }
  }
  SUBCASE("setting.yml 在测点目录里也认") {
    Fixture f("here");
    std::ofstream(f.point / "setting.yml") << "model_roi:\n  enabled: true\n  model_path: A.onnx\n";
    const nlohmann::json doc = import("StandardGap.yml", f.point);
    CHECK(hasOp(doc, "gap.locate_model"));
  }
}

TEST_CASE("模型模式的 ONNX：setting 优先，其次 baseDir 下唯一的 *.onnx") {
  SUBCASE("baseDir 下唯一的 onnx") {
    Fixture f("onnx1");
    std::ofstream(f.point / "only.onnx") << "x";
    const nlohmann::json doc = import("StandardGap.yml:model", f.point);
    REQUIRE(hasOp(doc, "gap.locate_model"));
    CHECK_FALSE(hasOp(doc, "flow.fallback"));
  }
  SUBCASE("两个 onnx 就不猜，报 bad_param") {
    Fixture f("onnx2");
    std::ofstream(f.point / "a.onnx") << "x";
    std::ofstream(f.point / "b.onnx") << "x";
    Status s = Status::Ok();
    import("StandardGap.yml:model", f.point, &s);
    CHECK_FALSE(s.ok);
    CHECK(s.code == "bad_param");
  }
}

namespace {

bool hasEdgeFrom(const nlohmann::json& doc, const char* fromNode, const char* toNode,
                 const char* toPort) {
  for (const auto& e : doc["edges"]) {
    if (e["from"]["node"] == fromNode && e["to"]["node"] == toNode && e["to"]["port"] == toPort) {
      return true;
    }
  }
  return false;
}

/// 备用闭包（b_ 开头的节点）只能经 flow.fallback 的 b 端口流进主图。
void checkBackupOnlyViaB(const nlohmann::json& doc) {
  std::unordered_map<std::string, std::string> opOf;
  for (const auto& n : doc["nodes"])
    opOf[n["id"].get<std::string>()] = n["op"].get<std::string>();
  bool sawBackup = false;
  for (const auto& e : doc["edges"]) {
    const auto from = e["from"]["node"].get<std::string>();
    const auto to = e["to"]["node"].get<std::string>();
    const auto port = e["to"]["port"].get<std::string>();
    if (from.rfind("b_", 0) != 0)
      continue;
    sawBackup = true;
    if (to.rfind("b_", 0) == 0)
      continue;
    CHECK(opOf[to] == "flow.fallback");
    CHECK(port == "b");
  }
  CHECK(sawBackup);
  CHECK(doc["outputs"].size() == 3);
  CHECK(doc["outputs"].contains("bundle"));
}

} // namespace

TEST_CASE("带 flow.fallback 的细粒度图：备用闭包只经 b 端口流出") {
  Fixture f("fallback", "model_roi:\n  enabled: true\n  model_path: X:/m.onnx\n");
  const nlohmann::json doc = import("StandardGap.yml:model:fine", f.point);
  const std::vector<std::string> fallbacks = idsWithOp(doc, "flow.fallback");
  CHECK(fallbacks == std::vector<std::string>{"n_fb_line", "n_fb_location", "n_fb_reference"});
  const std::vector<std::string> backupFits = idsWithOp(doc, "profile.fit_line_robust");
  CHECK(backupFits ==
        std::vector<std::string>{"b_n_fit_base", "b_n_fit_ref", "n_fit_base", "n_fit_ref"});
  for (const auto& n : doc["nodes"]) {
    const auto id = n["id"].get<std::string>();
    if (n["op"] != "profile.fit_line_robust")
      continue;
    CHECK(n["params"]["endpoints"] ==
          (id.rfind("b_", 0) == 0 ? "roi_intersection" : "inlier_ends"));
  }
  // 备用闭包不再自己读一遍文件：与模型那一支共用剔过 NaN 的两片云与去噪之后的合并云
  CHECK(idsWithOp(doc, "gap.load_profile_pair") == std::vector<std::string>{"n_load"});
  checkBackupOnlyViaB(doc);
}

TEST_CASE("带 flow.fallback 的积木图：定位与各完整特征分别原子回退") {
  Fixture f("fallback-blocks", "model_roi:\n  enabled: true\n  model_path: X:/m.onnx\n");
  const nlohmann::json doc = import("StandardGap.yml:model", f.point);
  CHECK(idsWithOp(doc, "flow.fallback") ==
        std::vector<std::string>{"n_fb_line", "n_fb_location", "n_fb_reference"});
  CHECK(idsWithOp(doc, "gap.locate_model") == std::vector<std::string>{"n_model"});
  CHECK(idsWithOp(doc, "gap.locate_template") == std::vector<std::string>{"b_n_locate"});
  CHECK(idsWithOp(doc, "gap.role_line") == std::vector<std::string>{"b_n_line", "n_line"});
  for (const auto& n : doc["nodes"]) {
    const auto id = n["id"].get<std::string>();
    if (n["op"] != "gap.role_line")
      continue;
    CHECK(n["params"]["endpoints"] ==
          (id.rfind("b_", 0) == 0 ? "roi_intersection" : "inlier_ends"));
  }
  CHECK(hasEdgeFrom(doc, "n_fb_location", "n_split_location", "location"));
  CHECK(hasEdgeFrom(doc, "n_split_location", "n_bundle", "rois"));
  CHECK(hasEdgeFrom(doc, "n_split_location", "n_bundle", "scan"));
  CHECK(hasEdgeFrom(doc, "n_fb_location", "n_bundle", "fallback"));
  CHECK(hasEdgeFrom(doc, "n_fb_line", "n_bundle", "fallbackLine"));
  CHECK(hasEdgeFrom(doc, "n_fb_reference", "n_bundle", "fallbackReference"));
  CHECK(hasEdgeFrom(doc, "n_scan", "b_n_locate", "scan"));
  checkBackupOnlyViaB(doc);
}

TEST_CASE("导入器把 Python 生成器的 SystemExit 换成 Validate 诊断") {
  Fixture f("bad");
  const ImporterDesc* desc = importer("StandardGap.yml:template");
  REQUIRE(desc != nullptr);
  std::string out;

  SUBCASE("seg_mode 不是 ROI") {
    const Status s =
        desc->fn("common_settings: {seg_mode: Each}\nalign: {align_cloud: true}\n", f.point, out);
    CHECK_FALSE(s.ok);
    CHECK(s.code == "bad_input");
    CHECK((s.phase == Phase::Validate));
  }
  SUBCASE("align_cloud 为假") {
    const Status s =
        desc->fn("common_settings: {seg_mode: ROI}\nalign: {align_cloud: false}\n", f.point, out);
    CHECK_FALSE(s.ok);
    CHECK(s.code == "bad_input");
  }
  SUBCASE("不支持的几何类型") {
    const Status s = desc->fn("common_settings: {seg_mode: ROI}\nalign: {align_cloud: true}\n"
                              "flush: {base_type: 2-points line}\n",
                              f.point, out);
    CHECK_FALSE(s.ok);
    CHECK(s.code == "bad_input");
  }
}

TEST_CASE("gap.result_bundle 的字段与 QualityMetrics 对齐") {
  RuntimeFixture fixture;
  const auto graph = fixture.graph("StandardGap.yml:template:fine");
  test::Session session(graph, fixture.root);
  const auto& log = session.wait();
  requireRun(log);
  const auto bundleData = result(session, "n_bundle", "bundle"),
             lineData = result(session, "n_split_line_feature", "quality"),
             circleData = result(session, "n_selected_circles", "quality"),
             alignmentData = result(session, "n_select", "alignment"),
             roiData = result(session, "n_roi_set", "rois");
  REQUIRE(bundleData.asRecord());
  const auto& bundle = bundleData.asRecord()->data;
  const auto& line = lineData.asRecord()->data;
  const auto& circles = circleData.asRecord()->data;
  lyflow::packs::gap::profile_scene::RoiView rois;
  lyflow::packs::gap::profile_template::AlignmentView alignment;
  std::string why;
  REQUIRE(lyflow::packs::gap::profile_scene::readRoiSet(roiData, rois, why));
  REQUIRE(lyflow::packs::gap::profile_template::readAlignment(alignmentData, alignment, why));
  CHECK(bundle["schemaVersion"] == 4);
  CHECK(bundle["gap"]["status"] == "success");
  CHECK(bundle["gap"]["value_mm"].get<double>() ==
        doctest::Approx(result(session, "n_gap", "value").asMeasurement()->value));
  CHECK(bundle["effective_roi"]["flush_base"][0].get<double>() ==
        doctest::Approx(rois.boxes[0]->asBox2D()->min[0] * 1000.));
  CHECK(bundle["effective_roi"]["flush_base"][3].get<double>() ==
        doctest::Approx(rois.boxes[0]->asBox2D()->max[1] * 1000.));
  CHECK(bundle["effective_roi"]["overall"][0].get<double>() == doctest::Approx(-30));
  CHECK(bundle["effective_roi"]["flush_ref"][0].get<double>() ==
        doctest::Approx(rois.boxes[1]->asBox2D()->min[0] * 1000.));
  CHECK(bundle["effective_roi"]["gap_left"][0].get<double>() ==
        doctest::Approx(rois.boxes[2]->asBox2D()->min[0] * 1000.));
  CHECK(bundle["effective_roi"]["gap_right"][2].get<double>() ==
        doctest::Approx(rois.boxes[3]->asBox2D()->max[0] * 1000.));
  REQUIRE(bundle["fits"].size() >= 3);
  CHECK(bundle["fits"][0]["component"] == "flush_base");
  CHECK(bundle["fits"][0]["inlier_count"] == line["weightedInlierCount"]);
  CHECK(bundle["fits"][0]["rms_residual_mm"].get<double>() ==
        doctest::Approx(line["weightedRmsResidual"].get<double>() * 1000));
  bool left = false, right = false;
  for (const auto& fit : bundle["fits"]) {
    if (fit["component"] == "gap_left") {
      left = true;
      CHECK(fit["radius_mm"] == circles["left"]["radiusMm"]);
    }
    if (fit["component"] == "gap_right") {
      right = true;
      CHECK(fit["radius_mm"] == circles["right"]["radiusMm"]);
    }
  }
  CHECK(left);
  CHECK(right);
  CHECK(bundle["point_counts"]["flush_base_roi"] == line["validSlotCount"]);
  CHECK(bundle["point_counts"]["gap_left_roi"] == circles["left"]["pointCount"]);
  CHECK(bundle["roi_source"] == "config");
  REQUIRE(bundle["icp"].size() == 2);
  CHECK(bundle["icp"][0]["component"] == "left");
  CHECK(bundle["icp"][1]["score"] == alignment.info->data["right"]["score"]);
  CHECK(bundle["graph_sha256"] == "");
  CHECK(bundle["pack_versions"].is_array());
}

namespace {

/// 用改过的 YAML 文本跑一次导入（上面的 import() 固定用 kConfig）。
nlohmann::json importText(const std::string& kind, const std::string& yaml, Status* status) {
  const ImporterDesc* desc = importer(kind);
  REQUIRE(desc != nullptr);
  std::string out;
  const Status s = desc->fn(yaml, std::filesystem::path(), out);
  if (status != nullptr)
    *status = s;
  if (!s.ok)
    return nlohmann::json();
  return nlohmann::json::parse(out);
}

/// 往 kConfig 的 gap: 段里插几行（直接拼在末尾会落到 align: 底下）。
std::string withGapKeys(const std::string& lines) {
  std::string yaml = kConfig;
  const std::size_t at = yaml.find("align:\n");
  REQUIRE(at != std::string::npos);
  yaml.insert(at, lines);
  return yaml;
}

const nlohmann::json& nodeById(const nlohmann::json& doc, const char* id) {
  for (const auto& n : doc["nodes"]) {
    if (n["id"] == id)
      return n;
  }
  FAIL("图里没有 " << id);
  static const nlohmann::json empty;
  return empty;
}

bool hasEdge(const nlohmann::json& doc, const char* toNode, const char* toPort) {
  for (const auto& e : doc["edges"]) {
    if (e["to"]["node"] == toNode && e["to"]["port"] == toPort)
      return true;
  }
  return false;
}

} // namespace

TEST_CASE("圆心高度带：没配时不接 refLine，配了接的是 flush_ref 的那条线，ref_type 不对就报错") {
  SUBCASE("没配时不接 refLine，配了才接参考线") {
    const nlohmann::json bare = importText("StandardGap.yml:template:fine", kConfig, nullptr);
    CHECK(nodeById(bare, "n_circles")["params"]["rightCenterTol"] == 0.0);
    CHECK_FALSE(hasEdge(bare, "n_circles", "refLine"));

    const nlohmann::json banded = importText(
        "StandardGap.yml:template:fine",
        withGapKeys("  center_band: {right_above: 0.25, right_tolerance: 0.45}\n"), nullptr);
    const auto& p = nodeById(banded, "n_circles")["params"];
    CHECK(p["rightCenterAbove"] == 0.25);
    CHECK(p["rightCenterTol"] == 0.45);
    CHECK(hasEdge(banded, "n_circles", "refLine"));
  }

  SUBCASE("接的是 flush_ref 的那条线，ref_type 不对就报错") {
    const std::string banded =
        withGapKeys("  center_band: {right_above: 0.25, right_tolerance: 0.45}\n");
    const nlohmann::json doc = importText("StandardGap.yml:template:fine", banded, nullptr);
    bool seen = false;
    for (const auto& e : doc["edges"]) {
      if (e["to"]["node"] == "n_circles" && e["to"]["port"] == "refLine") {
        seen = true;
        CHECK(e["from"]["node"] == "n_split_reference_feature");
        CHECK(e["from"]["port"] == "line");
      }
    }
    CHECK(seen);

    std::string noRefLine = banded;
    const std::size_t at = noRefLine.find("ref_type: line end");
    REQUIRE(at != std::string::npos);
    noRefLine.replace(at, std::strlen("ref_type: line end"), "ref_type: selected point");
    Status s;
    importText("StandardGap.yml:template:fine", noRefLine, &s);
    CHECK_FALSE(s.ok);
  }
}

TEST_CASE("YAML 的逐侧相机、圆心高度带 mode、weak_fit 地板落到 n_circles 的参数上") {
  struct Case {
    const char* name;
    std::string yaml;
    std::vector<std::pair<const char*, nlohmann::json>> expected;
  };
  const std::vector<Case> cases = {
      {"逐侧相机",
       withGapKeys("  right_circle_camera: Secondary\n"),
       {{"leftCamera", "Both"}, {"rightCamera", "Secondary"}}},
      {"圆心高度带的 mode",
       withGapKeys("  center_band: {left_above: 0.5, left_tolerance: 0.4, left_mode: guard}\n"),
       {{"leftCenterMode", "guard"}, {"rightCenterMode", "always"}}},
      {"weak_fit 不写就是 0", kConfig, {{"rightMinArcDeg", 0.0}}},
      {"weak_fit 的地板",
       withGapKeys("  weak_fit: {right_min_arc_deg: 55, right_min_inliers: 10}\n"),
       {{"rightMinArcDeg", 55.0}, {"rightMinInliers", 10}, {"leftMinArcDeg", 0.0}}},
  };
  for (const Case& c : cases) {
    CAPTURE(c.name);
    const nlohmann::json doc = importText("StandardGap.yml:template", c.yaml, nullptr);
    const auto& p = nodeById(doc, "n_circles")["params"];
    for (const auto& [key, value] : c.expected) {
      CAPTURE(key);
      CHECK(p[key] == value);
    }
  }

  // 圆心高度带的 mode 写错就报错
  Status s;
  importText("StandardGap.yml:template",
             withGapKeys("  center_band: {left_above: 0.5, left_tolerance: 0.4,"
                         " left_mode: whatever}\n"),
             &s);
  CHECK_FALSE(s.ok);
}

namespace {

std::string withFlushKeys(const std::string& lines) {
  std::string yaml = kConfig;
  const std::size_t at = yaml.find("gap:\n");
  REQUIRE(at != std::string::npos);
  yaml.insert(at, lines);
  return yaml;
}

} // namespace

TEST_CASE(
    "方向基准：默认不生成节点；配了 long_plane 就长出「窗 → 裁 → 拟」；锚可分别指定；写错就报错") {
  SUBCASE("默认不生成任何节点") {
    const nlohmann::json doc = importText("StandardGap.yml:template:fine", kConfig, nullptr);
    CHECK_FALSE(hasOp(doc, "geometry.anchored_window"));
    CHECK_FALSE(hasEdge(doc, "n_fit_base", "refLine"));
    // 一个方向相关的参数都不写进图，留给算子默认的 free
    CHECK(nodeById(doc, "n_fit_base")["params"]["dirMode"] == "free");
  }

  SUBCASE("配了 long_plane 就长出三个节点并接到 n_fit_base") {
    const nlohmann::json doc =
        importText("StandardGap.yml:template:fine",
                   withFlushKeys("  base_direction: {datum: long_plane, side: left, length_mm: 13,"
                                 " height_mm: 2.5, mode: fixed, nominal_deg: 3.0}\n"),
                   nullptr);
    REQUIRE(hasOp(doc, "geometry.anchored_window"));
    const auto& win = nodeById(doc, "n_datum_box")["params"];
    CHECK(win["side"] == "left");
    CHECK(win["length"] == .013);
    CHECK(win["heightPadding"] == .0025);
    const auto& base = nodeById(doc, "n_fit_base")["params"];
    CHECK(base["dirMode"] == "fixed");
    CHECK(base["dirNominalDeg"] == 3.0);
    REQUIRE(hasEdge(doc, "n_fit_base", "refLine"));
    for (const auto& e : doc["edges"]) {
      if (e["to"]["node"] == "n_fit_base" && e["to"]["port"] == "refLine") {
        CHECK(e["from"]["node"] == "n_fit_datum");
        CHECK(e["from"]["port"] == "line");
      }
    }
    // x 锚在缝的框上、y 锚在基准面框上：基准面框偶尔会整个跑偏，拿它定 x 窗口会飞出去
    bool sawAnchor = false;
    bool sawHeight = false;
    for (const auto& e : doc["edges"]) {
      if (e["to"]["node"] != "n_datum_box")
        continue;
      if (e["to"]["port"] == "anchor") {
        sawAnchor = true;
        CHECK(e["from"]["port"] == "seamLeft");
      }
      if (e["to"]["port"] == "heightAnchor") {
        sawHeight = true;
        CHECK(e["from"]["port"] == "datum");
      }
    }
    CHECK(sawAnchor);
    CHECK(sawHeight);
    CHECK(nodeById(doc, "n_fit_datum")["params"]["minInliers"] == 60);
  }

  SUBCASE("datum 与 mode 写错了导入就报错") {
    for (const char* line : {"  base_direction: {datum: whatever}\n",
                             "  base_direction: {datum: long_plane, mode: whatever}\n"}) {
      Status s;
      importText("StandardGap.yml:template", withFlushKeys(line), &s);
      CHECK_FALSE(s.ok);
    }
  }

  SUBCASE("锚和高度锚可以分别指定") {
    const nlohmann::json doc =
        importText("StandardGap.yml:template:fine",
                   withFlushKeys("  base_direction: {datum: long_plane, anchor: gap_right,"
                                 " height_anchor: flush_ref, mode: band, nominal_deg: 1.0}\n"),
                   nullptr);
    for (const auto& e : doc["edges"]) {
      if (e["to"]["node"] != "n_datum_box")
        continue;
      if (e["to"]["port"] == "anchor")
        CHECK(e["from"]["port"] == "seamRight");
      if (e["to"]["port"] == "heightAnchor")
        CHECK(e["from"]["port"] == "target");
    }
    // side 跟着 anchor 走：锚在右边的缝框上，窗口就往右推
    CHECK(nodeById(doc, "n_datum_box")["params"]["side"] == "right");
    CHECK(nodeById(doc, "n_fit_base")["params"]["dirMode"] == "band");
  }
}

namespace {

std::vector<std::string> stringList(const nlohmann::json& a) {
  std::vector<std::string> out;
  for (const auto& e : a)
    out.push_back(e.get<std::string>());
  std::sort(out.begin(), out.end());
  return out;
}

/// 用进程内的全量注册表校验一张图（std 包 + gap 包都在），只留 error。
std::vector<nlohmann::json> validationErrors(const nlohmann::json& doc) {
  const nlohmann::json diags =
      nlohmann::json::parse(exec::validateGraphJson(doc.dump(), std::filesystem::path()));
  std::vector<nlohmann::json> errors;
  for (const auto& d : diags) {
    if (d["severity"] == "error")
      errors.push_back(d);
  }
  return errors;
}

} // namespace

TEST_CASE("细粒度图：fit_line 接 toward、不写 side，business_rois 不写 datumSide（auto）") {
  for (const char* kind : {"StandardGap.yml:template:fine", "StandardGap.yml:model:fine"}) {
    CAPTURE(kind);
    Fixture f(std::string("m7-") +
                  (std::strcmp(kind, "StandardGap.yml:model:fine") == 0 ? "model" : "tpl"),
              "model_roi:\n  enabled: true\n  model_path: X:/m.onnx\n");
    const nlohmann::json doc = import(kind, f.point);
    REQUIRE(doc.is_object());
    for (const auto& n : doc["nodes"]) {
      const std::string id = n["id"].get<std::string>();
      CAPTURE(id);
      if (n["op"] == "profile.fit_line_robust") {
        CHECK_FALSE(n.value("params", nlohmann::json::object()).contains("side"));
        CHECK(hasEdge(doc, id.c_str(), "toward"));
      }
      if (n["op"] == "gap.business_rois") {
        // 基准件在哪一侧由框推出（m8-plan L7）：一个左右都不写
        const auto params = n.value("params", nlohmann::json::object());
        CHECK_FALSE(params.contains("baseSide"));
        CHECK(params["datumSide"] == "auto");
      }
      if (n["op"] == "gap.roi_from_labels") {
        CHECK_FALSE(n.value("params", nlohmann::json::object()).contains("baseSide"));
      }
    }
  }
}

TEST_CASE("细粒度图：基准线与参考线的 toward 都接 business_rois 的 seam（两缝框中心的中点）") {
  const nlohmann::json doc = importText("StandardGap.yml:template:fine", kConfig, nullptr);
  int seen = 0;
  for (const auto& e : doc["edges"]) {
    if (e["to"]["port"] != "toward")
      continue;
    if (e["to"]["node"] == "n_fit_base" || e["to"]["node"] == "n_fit_ref") {
      CHECK(e["from"]["node"] == "n_measure_rois");
      CHECK(e["from"]["port"] == "seam");
      ++seen;
    }
  }
  CHECK(seen == 2);
  // 方向基准线接它的锚框
  const nlohmann::json datum = importText(
      "StandardGap.yml:template:fine",
      withFlushKeys("  base_direction: {datum: long_plane, anchor: gap_right, mode: band}\n"),
      nullptr);
  bool saw = false;
  for (const auto& e : datum["edges"]) {
    if (e["to"]["node"] == "n_fit_datum" && e["to"]["port"] == "toward") {
      saw = true;
      CHECK(e["from"]["port"] == "seamRight");
    }
  }
  CHECK(saw);
}

TEST_CASE("导入的图带顶层参数：gapOffset 绑两处，模型模式的 modelPath 逐个列出节点") {
  Fixture f("m7-params", "model_roi:\n  enabled: true\n  model_path: X:/m.onnx\n");
  for (const char* style : {"", ":fine"}) {
    CAPTURE(style);
    const nlohmann::json tpl = import(std::string("StandardGap.yml:template") + style, f.point);
    REQUIRE(tpl["params"].contains("gapOffset"));
    CHECK(stringList(tpl["params"]["gapOffset"]["binds"]) ==
          std::vector<std::string>{"n_circles.offset", "n_gap.offsetMm"});
    CHECK(tpl["params"]["gapOffset"]["default"] == 0.0);
    CHECK_FALSE(tpl["params"].contains("modelPath"));
    // 被绑定的参数不能再显式写在节点上（一处定义）
    CHECK_FALSE(nodeById(tpl, "n_gap")["params"].contains("offsetMm"));
    CHECK_FALSE(nodeById(tpl, "n_circles")["params"].contains("offsetMm"));

    const nlohmann::json model = import(std::string("StandardGap.yml:model") + style, f.point);
    REQUIRE(model["params"].contains("modelPath"));
    CHECK(model["params"]["modelPath"]["default"] == "X:/m.onnx");
    // 黑盒对照 gap.measure_reference 两种图都不再生成（L12），modelPath 只绑推理那一处
    CHECK(stringList(model["params"]["modelPath"]["binds"]) ==
          std::vector<std::string>{std::string(*style ? "n_infer" : "n_model") + ".modelPath"});
    CHECK_FALSE(hasOp(model, "gap.measure_reference"));
    CHECK_FALSE(hasOp(tpl, "gap.measure_reference"));
    for (const auto& b : model["params"]["modelPath"]["binds"]) {
      CHECK(b.get<std::string>().find('*') == std::string::npos);
    }
    CHECK(stringList(model["params"]["gapOffset"]["binds"]) ==
          std::vector<std::string>{"n_circles.offset", "n_gap.offsetMm"});
    // 图文件的 sha256 由宿主经 params_json 传（docs/result-bundle-plan.md (a)）
    for (const nlohmann::json* doc : {&tpl, &model}) {
      REQUIRE((*doc)["params"].contains("graphSha256"));
      CHECK((*doc)["params"]["graphSha256"]["type"] == "string");
      CHECK((*doc)["params"]["graphSha256"]["default"] == "");
      CHECK(stringList((*doc)["params"]["graphSha256"]["binds"]) ==
            std::vector<std::string>{"n_bundle.graphSha256"});
    }
  }
}

TEST_CASE(
    "roisFrom：候选自带 rois 的槽记 template，没带的（回放全局框）记 config —— 积木 / 细粒度都写") {
  // kConfig 的 f1 没带 rois；再加一个带了 rois 的 f2
  std::string yaml = kConfig;
  const std::string f1 = "    - {id: f1, left: left_template.pcd, right: right_template.pcd}\n";
  const auto at = yaml.find(f1);
  REQUIRE(at != std::string::npos);
  yaml.insert(at + f1.size(), "    - {id: f2, left: f2_left.pcd, right: f2_right.pcd,\n"
                              "       rois: {gap: {left_roi: [4, 162, 8, 170]}}}\n");
  Status s;
  const nlohmann::json blocks = importText("StandardGap.yml:template", yaml, &s);
  REQUIRE(s.ok);
  const nlohmann::json& locate = nodeById(blocks, "n_locate");
  CHECK(locate["params"]["template1RoisFrom"] == "config");
  CHECK(locate["params"]["template2RoisFrom"] == "template");

  const nlohmann::json fine = importText("StandardGap.yml:template:fine", yaml, &s);
  REQUIRE(s.ok);
  CHECK(nodeById(fine, "n_align_f1")["params"]["roisFrom"] == "config");
  CHECK(nodeById(fine, "n_align_f2")["params"]["roisFrom"] == "template");
}

TEST_CASE("导入的图过 core 的 validate：模板、模型、带 fallback 三种 × 积木 / 细粒度") {
  Fixture withFallback("m7-validate", "model_roi:\n  enabled: true\n  model_path: X:/m.onnx\n");
  Fixture plain("m7-validate-plain");
  std::ofstream(plain.point / "only.onnx") << "x";
  const struct {
    const char* kind;
    const Fixture* fixture;
  } cases[6] = {
      {"StandardGap.yml:template", &plain},     {"StandardGap.yml:model", &plain},
      {"StandardGap.yml:model", &withFallback}, {"StandardGap.yml:template:fine", &plain},
      {"StandardGap.yml:model:fine", &plain},   {"StandardGap.yml:model:fine", &withFallback}};
  for (const auto& c : cases) {
    CAPTURE(c.kind);
    const nlohmann::json doc = import(c.kind, c.fixture->point);
    REQUIRE(doc.is_object());
    const auto errors = validationErrors(doc);
    for (const auto& e : errors)
      CAPTURE(e.dump());
    CHECK(errors.empty());
    if (!errors.empty())
      MESSAGE(errors.front().dump());
  }
}

TEST_CASE(
    "Native import entrypoints retain real fine stages paired contexts and fixed public units") {
  Fixture fixture("native-import-contract",
                  "model_roi:\n  enabled: true\n  model_path: X:/m.onnx\n");
  const std::set<std::string> retired{"gap.fit_line",
                                      "gap.selected_point",
                                      "gap.nearest_to_line",
                                      "gap.drop_non_finite",
                                      "gap.to_measurement_frame",
                                      "gap.flush",
                                      "gap.gap",
                                      "gap.point_offset",
                                      "gap.measure_reference",
                                      "gap.camera_guard",
                                      "gap.datum_window",
                                      "gap.judge"};
  for (const auto* kind :
       {"StandardGap.yml", "StandardGap.yml:template", "StandardGap.yml:model",
        "StandardGap.yml:fine", "StandardGap.yml:template:fine", "StandardGap.yml:model:fine"}) {
    CAPTURE(kind);
    const auto doc = import(kind, fixture.point);
    REQUIRE(doc.is_object());
    const bool fine = std::string(kind).find("fine") != std::string::npos;
    CHECK(hasEdgeFrom(doc, "n_compare_0", "n_judge_0", "value"));
    CHECK(hasEdgeFrom(doc, "n_compare_1", "n_judge_1", "value"));
    CHECK(hasEdgeFrom(doc, "n_judge_0", "n_bundle", "flush"));
    CHECK(hasEdgeFrom(doc, "n_judge_1", "n_bundle", "gap"));
    CHECK(doc["outputs"]["flush"]["node"] == "n_judge_0");
    CHECK(doc["outputs"]["gap"]["node"] == "n_judge_1");
    for (const auto& node : doc["nodes"]) {
      CHECK(retired.count(node["op"].get<std::string>()) == 0);
      if (node["op"] == "metrology.flush_point_line") {
        CHECK(node["params"]["outputUnit"] == "mm");
        CHECK(node["params"]["signConvention"] == "canonical_axis");
        CHECK(hasEdge(doc, node["id"].get<std::string>().c_str(), "baseLineContext"));
        CHECK(hasEdge(doc, node["id"].get<std::string>().c_str(), "refPointContext"));
      }
      if (node["op"] == "metrology.circle_gap") {
        CHECK(node["params"]["outputUnit"] == "mm");
        CHECK(node["params"]["definition"] == "datum_tangents");
        CHECK(
            hasEdgeFrom(doc, "n_flush", node["id"].get<std::string>().c_str(), "baseLineContext"));
      }
    }
    if (fine) {
      CHECK(hasOp(doc, "profile.remap_axes"));
      CHECK(hasOp(doc, "profile.filter_validity"));
      CHECK(hasOp(doc, "profile.merge"));
      CHECK(hasOp(doc, "profile.crop_box"));
      CHECK(hasOp(doc, "profile.fit_line_robust"));
      CHECK_FALSE(hasOp(doc, "gap.read_scan"));
      CHECK_FALSE(hasOp(doc, "gap.locate_template"));
      CHECK_FALSE(hasOp(doc, "gap.locate_model"));
      CHECK(hasEdgeFrom(doc, "n_drop_s", "n_merge", "a"));
      CHECK(hasEdgeFrom(doc, "n_drop_p", "n_merge", "b"));
    } else {
      CHECK(hasOp(doc, "gap.read_scan"));
      CHECK(hasOp(doc, "gap.role_line"));
      CHECK(hasOp(doc, "gap.ref_point"));
    }
    if (hasOp(doc, "gap.infer_profile")) {
      CHECK(hasEdgeFrom(doc, "n_raw_fields", "n_tensor", "primary"));
      CHECK(hasEdgeFrom(doc, "n_raw_fields", "n_seg", "primary"));
      CHECK(hasEdgeFrom(doc, "n_raw_fields", "n_seg", "secondary"));
    }
  }
  for (const auto* method : {"selected point", "nearest point"}) {
    std::string yaml = kConfig;
    const auto at = yaml.find("ref_type: line end");
    yaml.replace(at, std::strlen("ref_type: line end"), std::string("ref_type: ") + method);
    const auto doc = importText("StandardGap.yml:template:fine", yaml, nullptr);
    REQUIRE(doc.is_object());
    const bool selected = std::string(method) == "selected point";
    CHECK(hasOp(doc, selected ? "profile.nearest_sample" : "profile.nearest_to_line"));
    CHECK(hasEdgeFrom(doc, selected ? "n_measure_scan" : "n_crop_flushRef",
                      selected ? "n_sel_ref" : "n_near_ref", "profile"));
    CHECK(nodeById(doc, "n_reference_feature")["params"]["method"] ==
          (selected ? "selected_point" : "nearest_point"));
    if (selected) {
      CHECK_FALSE(hasOp(doc, "gap.selected_point"));
      CHECK(hasEdgeFrom(doc, "n_measure_scan", "n_reference_anchor_context", "profile"));
      for (const auto& node : doc["nodes"])
        CHECK(node["id"] != "n_crop_flushRef");
    } else {
      CHECK(hasEdgeFrom(doc, "n_fit_base", "n_near_ref", "line"));
      CHECK(hasEdgeFrom(doc, "n_fit_base", "n_near_ref", "lineContext"));
    }
    CHECK(validationErrors(doc).empty());
  }
  for (const auto* action : {"record", "off"}) {
    CAPTURE(std::string(action));
    std::string yaml = kConfig;
    const auto at = yaml.find("common_settings:\n") + std::strlen("common_settings:\n");
    yaml.insert(at, "  camera_guard: {enabled: true, on_disagree: " + std::string(action) +
                        ", sample_step_mm: 0.5, half_window_mm: 0.25}\n");
    const auto doc = importText("StandardGap.yml:template", yaml, nullptr);
    CHECK(hasOp(doc, "gap.select_camera"));
    const bool observes = std::string(action) == "record";
    CHECK(hasOp(doc, "profile.compare_overlap") == observes);
    CHECK(hasEdgeFrom(doc, "n_camera_observation", "n_camera_guard", "observation") == observes);
    if (observes) {
      const auto& observation = nodeById(doc, "n_camera_observation")["params"];
      CHECK(observation["sampleStep"] == .0005);
      CHECK(observation["halfWindow"] == .00025);
    }
    CHECK(validationErrors(doc).empty());
  }
}
TEST_CASE(
    "Native imported template and model workflows execute highlevel and true granular graphs consistently") {
  RuntimeFixture fixture;
  const auto model = fixture.model();
  for (const bool useModel : {false, true}) {
    CAPTURE(useModel);
    const auto yaml =
        std::string(kRunConfig) + (useModel ? "\nmodel_roi: {enabled: false, model_path: '" +
                                                  model.generic_u8string() + "'}\n"
                                            : "");
    double referenceFlush = 0, referenceGap = 0;
    for (const bool fine : {false, true}) {
      CAPTURE(fine);
      const auto kind = std::string("StandardGap.yml:") + (useModel ? "model" : "template") +
                        (fine ? ":fine" : "");
      const auto doc = fixture.graph(kind, yaml);
      const auto errors = validationErrors(doc);
      for (const auto& error : errors)
        CAPTURE(error.dump());
      REQUIRE(errors.empty());
      test::Session session(doc, fixture.root);
      const auto& log = session.wait();
      requireRun(log);
      const auto flush = result(session, "n_flush", "value"),
                 gap = result(session, "n_gap", "value"),
                 bundle = result(session, "n_bundle", "bundle");
      const auto flushJudged = result(session, "n_judge_0", "value"),
                 gapJudged = result(session, "n_judge_1", "value");
      REQUIRE(flush.asMeasurement());
      REQUIRE(gap.asMeasurement());
      CHECK(flush.asMeasurement()->ok);
      CHECK(gap.asMeasurement()->ok);
      CHECK(flush.asMeasurement()->unit == "mm");
      CHECK(gap.asMeasurement()->unit == "mm");
      CHECK(bundle.asRecord()->data["flush"]["verdict"] == flushJudged.asMeasurement()->verdict);
      CHECK(bundle.asRecord()->data["gap"]["verdict"] == gapJudged.asMeasurement()->verdict);
      CHECK(bundle.asRecord()->data["roi_source"] == (useModel ? "model" : "config"));
      if (!fine) {
        referenceFlush = flush.asMeasurement()->value;
        referenceGap = gap.asMeasurement()->value;
      } else {
        CHECK(flush.asMeasurement()->value == doctest::Approx(referenceFlush).epsilon(1e-6));
        CHECK(gap.asMeasurement()->value == doctest::Approx(referenceGap).epsilon(1e-6));
      }
      const auto selected = result(session, "n_selected_circles", "leftContext");
      CHECK(selected.asRecord()->type == "profile.Geometry.v3");
    }
  }
}
TEST_CASE("Imported graphs with a profile loader run from host profiles injected into it") {
  std::string guarded = kRunConfig;
  const auto at = guarded.find("common_settings:\n") + std::strlen("common_settings:\n");
  guarded.insert(at, "  camera_guard: {enabled: true, on_disagree: record}\n");
  const struct {
    const char* kind;
    std::string yaml;
  } cases[] = {{"StandardGap.yml:template", guarded},
               {"StandardGap.yml:template:fine", kRunConfig}};
  for (const auto& item : cases) {
    CAPTURE(std::string(item.kind));
    RuntimeFixture fixture;
    const auto doc = fixture.graph(item.kind, item.yaml);
    Data primary, secondary;
    double fromFiles = 0;
    {
      test::Session session(doc, fixture.root);
      requireRun(session.wait());
      primary = result(session, "n_load", "primary");
      secondary = result(session, "n_load", "secondary");
      fromFiles = result(session, "n_gap", "value").asMeasurement()->value;
    }
    std::filesystem::remove(fixture.root / "LaserProfile_L0_Master_x.pcd");
    std::filesystem::remove(fixture.root / "LaserProfile_R1_Slave_x.pcd");
    std::vector<exec::InjectedInput> inputs{exec::InjectedInput{"n_load", "primary", primary},
                                            exec::InjectedInput{"n_load", "secondary", secondary}};
    test::Session session(doc, fixture.root, {}, false, 0, false, std::move(inputs));
    const auto& log = session.wait();
    requireRun(log);
    CHECK(log.nodeEvent("n_load", "done")["stats"]["provided"] == true);
    CHECK(result(session, "n_gap", "value").asMeasurement()->value ==
          doctest::Approx(fromFiles).epsilon(1e-6));
  }
}
TEST_CASE("Native imported fallback is lazy atomic and never adds a second circle retry policy") {
  RuntimeFixture fixture;
  const auto good = fixture.model(), missing = fixture.model(true);
  for (const bool fine : {false, true})
    for (const bool modelFails : {false, true}) {
      CAPTURE(fine);
      CAPTURE(modelFails);
      const auto path = modelFails ? missing : good;
      const auto yaml = std::string(kRunConfig) + "\nmodel_roi: {enabled: true, model_path: '" +
                        path.generic_u8string() + "'}\n";
      const auto doc =
          fixture.graph(fine ? "StandardGap.yml:model:fine" : "StandardGap.yml:model", yaml);
      CHECK(idsWithOp(doc, fine ? "gap.fit_gap_circles" : "gap.seam_circles") ==
            std::vector<std::string>{"n_circles"});
      CHECK(idsWithOp(doc, "flow.fallback") ==
            std::vector<std::string>{"n_fb_line", "n_fb_location", "n_fb_reference"});
      test::Session session(doc, fixture.root);
      const auto& log = session.wait();
      requireRun(log);
      const auto chosen = result(session, "n_fb_location", "choice");
      REQUIRE(chosen.asRecord());
      CHECK(chosen.asRecord()->data["choice"] == (modelFails ? "b" : "a"));
      const auto locationScan = result(session, "n_split_location", "scan"),
                 locationRois = result(session, "n_split_location", "rois");
      lyflow::packs::gap::profile_scene::ScanView scan;
      lyflow::packs::gap::profile_scene::RoiView rois;
      std::string why;
      REQUIRE(lyflow::packs::gap::profile_scene::readScanPair(locationScan, scan, why));
      REQUIRE(lyflow::packs::gap::profile_scene::readRoiSet(locationRois, rois, why));
      CHECK(rois.info->data["source"] == (modelFails ? "config" : "model"));
      if (!modelFails) {
        for (const auto& event : log.ofKind("node_state"))
          if (event.value("nodeId", "").rfind("b_", 0) == 0)
            CHECK(event.value("state", "") != "running");
      }
      const auto bundle = result(session, "n_bundle", "bundle");
      CHECK(bundle.asRecord()->data["roi_source"] == (modelFails ? "config" : "model"));
      CHECK(result(session, "n_gap", "value").asMeasurement()->ok);
      CHECK_FALSE(hasEdge(doc, "n_bundle", "fallbackCircles"));
      checkBackupOnlyViaB(doc);
    }
}

TEST_CASE(
    "Native model fine and block preserve processed slots through disabled reverted and applied crops") {
  RuntimeFixture fixture;
  const auto model = fixture.model();
  const std::string original = "roll_anchored_crop: {enabled: false}";
  for (const auto& crop : std::vector<std::pair<std::string, std::string>>{
           {"{enabled: false}", "disabled"},
           {"{enabled: true, half_width_mm: 35, half_height_mm: 20, min_points_kept: 100000}",
            "reverted:min_points"},
           {"{enabled: true, half_width_mm: 35, half_height_mm: 20, min_points_kept: 1}",
            "applied"}}) {
    CAPTURE(crop.second);
    std::string yaml = kRunConfig;
    const auto offset = yaml.find(original);
    REQUIRE(offset != std::string::npos);
    yaml.replace(offset, original.size(), "roll_anchored_crop: " + crop.first);
    yaml += "\nmodel_roi: {enabled: false, model_path: '" + model.generic_u8string() + "'}\n";
    std::array<Data, 3> baseline;
    for (const bool fine : {false, true}) {
      CAPTURE(fine);
      const auto graph =
          fixture.graph(fine ? "StandardGap.yml:model:fine" : "StandardGap.yml:model", yaml);
      test::Session session(graph, fixture.root, {fine ? "n_roll" : "n_model"}, false, 1, true);
      const auto& log = session.wait();
      requireRun(log);
      REQUIRE(log.runStatus() == "ok");
      const auto actual = result(session, fine ? "n_roll" : "n_model", "scan");
      lyflow::packs::gap::profile_scene::ScanView scan;
      std::string why;
      REQUIRE(lyflow::packs::gap::profile_scene::readScanPair(actual, scan, why));
      CHECK(scan.info->data.at("stage") == "processed");
      Json status;
      if (fine)
        status = result(session, "n_roll", "status").asRecord()->data;
      else {
        const auto modelRois = result(session, "n_model", "rois");
        lyflow::packs::gap::profile_scene::RoiView rois;
        REQUIRE(lyflow::packs::gap::profile_scene::readRoiSet(modelRois, rois, why));
        status = rois.info->data.at("cropStatus");
      }
      CHECK(status.at("status") == crop.second);
      for (const auto* sensor : {scan.sensorPrimary, scan.sensorSecondary}) {
        REQUIRE(sensor->asProfile());
        CHECK(sensor->asProfile()->pointCount() == 1280);
        CHECK(sensor->asProfile()->validity[7] == 0);
        CHECK(sensor->asProfile()->sourceIndices[7] == 7);
        CHECK(sensor->asProfile()->cloud->rgb[3 * 100] == 100);
      }
      const Data* profiles[] = {scan.primary, scan.secondary, scan.merged};
      for (std::size_t i = 0; i < 3; ++i) {
        const auto& profile = *profiles[i]->asProfile();
        CHECK(profile.pointCount() == (i == 2 ? 2558 : 1279));
        CHECK(profile.descriptor.sourceCount == (i == 2 ? 2560 : 1280));
        CHECK(std::all_of(profile.validity.begin(), profile.validity.end(),
                          [](auto value) { return value != 0; }));
        if (!fine)
          baseline[i] = *profiles[i];
        else {
          const auto& previous = *baseline[i].asProfile();
          CHECK(profile.sourceIndices == previous.sourceIndices);
          CHECK(profile.validity == previous.validity);
          CHECK(profile.cloud->xyz == previous.cloud->xyz);
          CHECK(profile.cloud->rgb == previous.cloud->rgb);
        }
      }
    }
  }
}
