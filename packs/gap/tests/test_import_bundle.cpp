#include <doctest/doctest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "lyflow/operator.h"
#include "lyflow/registry.h"

namespace lyflow::packs::gap {
void registerPackOps(Registry& r);
void registerStandardGapImporter(Registry& r);
}  // namespace lyflow::packs::gap

namespace {

using namespace lyflow;

class NullContext final : public ExecContext {
 public:
  bool cancelled() const override { return false; }
  void progress(float, std::string_view) override {}
  void log(LogLevel, std::string) override {}
  const std::filesystem::path& baseDir() const override { return baseDir_; }
  int threadBudget() const override { return 1; }

 private:
  std::filesystem::path baseDir_;
};

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
  base_roi: [1, 2, 3, 4]
  ref_roi: [5, 6, 7, 8]
  segment_points: 1000
  offset: 0.5
  tolerances: {nominal: 1, up_deviation: 1.3, low_deviation: -1.3}
gap:
  left_type: circle
  right_type: circle
  definition: A
  left_roi: [9, 10, 11, 12]
  right_roi: [13, 14, 15, 16]
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
    if (settingBody != nullptr) std::ofstream(root / "setting.yml") << settingBody;
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
  if (status != nullptr) *status = s;
  if (!s.ok) return nlohmann::json();
  return nlohmann::json::parse(out);
}

bool hasOp(const nlohmann::json& doc, const char* op) {
  for (const auto& n : doc["nodes"]) {
    if (n["op"] == op) return true;
  }
  return false;
}

std::vector<std::string> idsWithOp(const nlohmann::json& doc, const char* op) {
  std::vector<std::string> ids;
  for (const auto& n : doc["nodes"]) {
    if (n["op"] == op) ids.push_back(n["id"].get<std::string>());
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

}  // namespace

TEST_CASE("StandardGap.yml 导入器注册了三种 kind") {
  for (const char* kind : {"StandardGap.yml", "StandardGap.yml:template", "StandardGap.yml:model"}) {
    const ImporterDesc* d = importer(kind);
    REQUIRE(d != nullptr);
    CHECK(d->fn != nullptr);
    CHECK(!d->label.empty());
  }
}

TEST_CASE("auto 的模式推导只看 setting.yml 的 model_roi.enabled") {
  SUBCASE("找不到 setting.yml 就退回模板路径") {
    Fixture f("nosetting");
    const nlohmann::json doc = import("StandardGap.yml", f.point);
    CHECK(hasOp(doc, "gap.business_rois"));
    CHECK_FALSE(hasOp(doc, "gap.roi_from_labels"));
    CHECK_FALSE(hasOp(doc, "flow.fallback"));
  }
  SUBCASE("enabled: false 也退回模板路径") {
    Fixture f("off", "model_roi:\n  enabled: false\n  model_path: X:/m.onnx\n");
    const nlohmann::json doc = import("StandardGap.yml", f.point);
    CHECK(hasOp(doc, "gap.business_rois"));
    CHECK_FALSE(hasOp(doc, "gap.roi_from_labels"));
  }
  SUBCASE("enabled: true 走模型路径，模型来自 model_path") {
    Fixture f("on", "model_roi:\n  enabled: true\n  model_path: X:/m.onnx\n");
    const nlohmann::json doc = import("StandardGap.yml", f.point);
    REQUIRE(hasOp(doc, "gap.roi_from_labels"));
    for (const auto& n : doc["nodes"]) {
      if (n["op"] == "ml.onnx_run") CHECK(n["params"]["modelPath"] == "X:/m.onnx");
    }
  }
  SUBCASE("setting.yml 在测点目录里也认") {
    Fixture f("here");
    std::ofstream(f.point / "setting.yml") << "model_roi:\n  enabled: true\n  model_path: A.onnx\n";
    const nlohmann::json doc = import("StandardGap.yml", f.point);
    CHECK(hasOp(doc, "gap.roi_from_labels"));
  }
}

TEST_CASE("模型模式的 ONNX：setting 优先，其次 baseDir 下唯一的 *.onnx") {
  SUBCASE("baseDir 下唯一的 onnx") {
    Fixture f("onnx1");
    std::ofstream(f.point / "only.onnx") << "x";
    const nlohmann::json doc = import("StandardGap.yml:model", f.point);
    REQUIRE(hasOp(doc, "gap.roi_from_labels"));
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

TEST_CASE("带 flow.fallback 的完整图：备用闭包只经 b 端口流出") {
  Fixture f("fallback", "model_roi:\n  enabled: true\n  model_path: X:/m.onnx\n");
  const nlohmann::json doc = import("StandardGap.yml:model", f.point);
  const std::vector<std::string> fallbacks = idsWithOp(doc, "flow.fallback");
  CHECK(fallbacks == std::vector<std::string>{
                         "n_fb_crop_p", "n_fb_crop_s", "n_fb_flushBase", "n_fb_flushRef",
                         "n_fb_gapLeft", "n_fb_gapRight", "n_fb_line", "n_fb_merged",
                         "n_fb_overall", "n_fb_quality_base", "n_fb_quality_ref",
                         "n_fb_ref_point"});
  const std::vector<std::string> backupFits = idsWithOp(doc, "gap.fit_line");
  CHECK(backupFits == std::vector<std::string>{"b_n_fit_base", "b_n_fit_ref", "n_fit_base",
                                               "n_fit_ref"});
  for (const auto& n : doc["nodes"]) {
    const auto id = n["id"].get<std::string>();
    if (n["op"] != "gap.fit_line") continue;
    CHECK(n["params"]["endpoints"] == (id.rfind("b_", 0) == 0 ? "roi_intersection" : "inlier_ends"));
  }
  std::unordered_map<std::string, std::string> opOf;
  for (const auto& n : doc["nodes"]) opOf[n["id"].get<std::string>()] = n["op"].get<std::string>();
  bool sawBackup = false;
  for (const auto& e : doc["edges"]) {
    const auto from = e["from"]["node"].get<std::string>();
    const auto to = e["to"]["node"].get<std::string>();
    const auto port = e["to"]["port"].get<std::string>();
    if (from.rfind("b_", 0) != 0) continue;
    sawBackup = true;
    if (to.rfind("b_", 0) == 0) continue;
    CHECK(opOf[to] == "flow.fallback");
    CHECK(port == "b");
  }
  CHECK(sawBackup);
  CHECK(doc["outputs"].size() == 3);
  CHECK(doc["outputs"].contains("bundle"));
}

TEST_CASE("导入器把 Python 生成器的 SystemExit 换成 Validate 诊断") {
  Fixture f("bad");
  const ImporterDesc* desc = importer("StandardGap.yml:template");
  REQUIRE(desc != nullptr);
  std::string out;

  SUBCASE("seg_mode 不是 ROI") {
    const Status s = desc->fn("common_settings: {seg_mode: Each}\nalign: {align_cloud: true}\n",
                              f.point, out);
    CHECK_FALSE(s.ok);
    CHECK(s.code == "bad_input");
    CHECK((s.phase == Phase::Validate));
  }
  SUBCASE("align_cloud 为假") {
    const Status s = desc->fn("common_settings: {seg_mode: ROI}\nalign: {align_cloud: false}\n",
                              f.point, out);
    CHECK_FALSE(s.ok);
    CHECK(s.code == "bad_input");
  }
  SUBCASE("不支持的几何类型") {
    const Status s = desc->fn(
        "common_settings: {seg_mode: ROI}\nalign: {align_cloud: true}\n"
        "flush: {base_type: 2-points line}\n",
        f.point, out);
    CHECK_FALSE(s.ok);
    CHECK(s.code == "bad_input");
  }
}

TEST_CASE("gap.result_bundle 的字段与 QualityMetrics 对齐") {
  const OperatorDesc* op = packRegistry().find("gap.result_bundle");
  REQUIRE(op != nullptr);
  ParamMap params;
  for (const Param& p : op->params) params[p.name] = p.def;
  params["cropStatus"] = Value::text("skipped:no_model_roi");

  Measurement gap;
  gap.value = 4.2;
  gap.ok = true;
  Measurement flush;
  flush.value = 1.5;
  flush.ok = true;

  Record fitBase;
  fitBase.type = "GapFitQuality";
  fitBase.data = {{"model", "line"}, {"pointCount", 136}, {"inlierCount", 85},
                  {"inlierRatio", 0.625}, {"rmsResidualMm", 0.04}, {"maxResidualMm", 0.09},
                  {"linePointXMm", 31.7}, {"linePointYMm", 165.9}, {"lineDirX", 1.0},
                  {"lineDirY", 0.0}};
  Record circles;
  circles.type = "GapFitQualityPair";
  circles.data["left"] = {{"model", "circle"}, {"radiusMode", "free"}, {"pointCount", 114},
                          {"inlierCount", 20}, {"radiusMm", 1.05}, {"centerXMm", 35.3},
                          {"centerYMm", 165.9}, {"arcCoverageDeg", 199.1}};
  circles.data["right"] = {{"model", "circle"}, {"radiusMode", "free"}, {"pointCount", 94},
                           {"inlierCount", 24}, {"radiusMm", 1.9}, {"centerXMm", 43.9},
                           {"centerYMm", 167.7}, {"arcCoverageDeg", 110.8}};
  Record alignment;
  alignment.type = "GapAlignment";
  alignment.data = {{"templateId", "f1"}, {"globalCoarse", true}, {"globalSucceeded", true},
                    {"rois", nlohmann::json::object()},
                    {"left", {{"score", 88.0}, {"success", true}}},
                    {"right", {{"score", 91.0}, {"success", true}}}};

  std::unordered_map<std::string, Data> inputs;
  inputs["gap"] = Data::measurement(gap);
  inputs["flush"] = Data::measurement(flush);
  inputs["roiFlushBase"] = Data::box2d(box(0.02f, 0.16f, 0.03f, 0.17f));
  inputs["roiOverall"] = Data::box2d(box(-0.1f, 0.03f, 0.2f, 0.29f));
  inputs["fitBase"] = Data::record(fitBase);
  inputs["fits"] = Data::record(circles);
  inputs["alignment"] = Data::record(alignment);
  inputs["cropStatus"] = Data::error(Status::Error(Phase::Execute, "upstream_failed", "x"));

  std::unordered_map<std::string, Data> outputs;
  NullContext ctx;
  const std::filesystem::path base;
  ParamView view(params, base);
  Inputs in(inputs);
  Outputs out(outputs);
  const Status s = op->compute(in, view, out, ctx);
  REQUIRE(s.ok);
  const Record* bundle = outputs["bundle"].asRecord();
  REQUIRE(bundle != nullptr);
  CHECK(bundle->type == "GapResultBundle");
  const nlohmann::json& b = bundle->data;

  CHECK(b["gap"]["status"] == "success");
  CHECK(b["gap"]["value_mm"].get<double>() == doctest::Approx(4.2));
  CHECK(b["effective_roi"]["flush_base"][0].get<double>() == doctest::Approx(20.0));
  CHECK(b["effective_roi"]["flush_base"][3].get<double>() == doctest::Approx(170.0));
  CHECK(b["effective_roi"]["overall"][0].get<double>() == doctest::Approx(-100.0));
  CHECK_FALSE(b["effective_roi"].contains("flush_ref"));

  REQUIRE(b["fits"].size() == 3);
  CHECK(b["fits"][0]["component"] == "flush_base");
  CHECK(b["fits"][0]["inlier_count"] == 85);
  CHECK(b["fits"][1]["component"] == "gap_left");
  CHECK(b["fits"][1]["radius_mm"].get<double>() == doctest::Approx(1.05));
  CHECK(b["fits"][2]["component"] == "gap_right");

  CHECK(b["point_counts"]["flush_base_roi"] == 136);
  CHECK(b["point_counts"]["gap_left_roi"] == 114);
  CHECK(b["point_counts"]["f1_global_coarse_succeeded"] == 1);
  CHECK(b["roi_source"] == "template");
  CHECK(b["crop_status"] == "skipped:no_model_roi");
  REQUIRE(b["icp"].size() == 2);
  CHECK(b["icp"][0]["component"] == "left");
  CHECK(b["icp"][1]["score"].get<double>() == doctest::Approx(91.0));
  CHECK(b["graph_sha256"] == "");
  CHECK(b["pack_versions"].is_array());
}

TEST_CASE("gap.fit_line 与 gap.fit_gap_circles 各出一份 quality Record") {
  for (const char* id : {"gap.fit_line", "gap.fit_gap_circles"}) {
    const OperatorDesc* op = packRegistry().find(id);
    REQUIRE(op != nullptr);
    bool found = false;
    for (const Port& p : op->outputs) {
      if (p.name == "quality") {
        found = true;
        CHECK(p.type == "Record");
      }
    }
    CHECK(found);
  }
}

namespace {

/// 用改过的 YAML 文本跑一次导入（上面的 import() 固定用 kConfig）。
nlohmann::json importText(const std::string& kind, const std::string& yaml, Status* status) {
  const ImporterDesc* desc = importer(kind);
  REQUIRE(desc != nullptr);
  std::string out;
  const Status s = desc->fn(yaml, std::filesystem::path(), out);
  if (status != nullptr) *status = s;
  if (!s.ok) return nlohmann::json();
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
    if (n["id"] == id) return n;
  }
  FAIL("图里没有 " << id);
  static const nlohmann::json empty;
  return empty;
}

bool hasEdge(const nlohmann::json& doc, const char* toNode, const char* toPort) {
  for (const auto& e : doc["edges"]) {
    if (e["to"]["node"] == toNode && e["to"]["port"] == toPort) return true;
  }
  return false;
}

}  // namespace

TEST_CASE("圆心高度带没配时不接 refLine，配了才接参考线") {
  const nlohmann::json bare = importText("StandardGap.yml:template", kConfig, nullptr);
  CHECK(nodeById(bare, "n_circles")["params"]["rightCenterTol"] == 0.0);
  CHECK_FALSE(hasEdge(bare, "n_circles", "refLine"));

  const nlohmann::json banded = importText(
      "StandardGap.yml:template",
      withGapKeys("  center_band: {right_above: 0.25, right_tolerance: 0.45}\n"), nullptr);
  const auto& p = nodeById(banded, "n_circles")["params"];
  CHECK(p["rightCenterAbove"] == 0.25);
  CHECK(p["rightCenterTol"] == 0.45);
  CHECK(hasEdge(banded, "n_circles", "refLine"));
}

TEST_CASE("圆心高度带接的是 flush_ref 的那条线，ref_type 不对就报错") {
  const std::string banded = withGapKeys(
      "  center_band: {right_above: 0.25, right_tolerance: 0.45}\n");
  const nlohmann::json doc = importText("StandardGap.yml:template", banded, nullptr);
  bool seen = false;
  for (const auto& e : doc["edges"]) {
    if (e["to"]["node"] == "n_circles" && e["to"]["port"] == "refLine") {
      seen = true;
      CHECK(e["from"]["node"] == "n_fit_ref");
      CHECK(e["from"]["port"] == "line");
    }
  }
  CHECK(seen);

  std::string noRefLine = banded;
  const std::size_t at = noRefLine.find("ref_type: line end");
  REQUIRE(at != std::string::npos);
  noRefLine.replace(at, std::strlen("ref_type: line end"), "ref_type: selected point");
  Status s;
  importText("StandardGap.yml:template", noRefLine, &s);
  CHECK_FALSE(s.ok);
}

TEST_CASE("逐侧相机从 YAML 落到 n_circles 的参数上") {
  const nlohmann::json doc = importText(
      "StandardGap.yml:template", withGapKeys("  right_circle_camera: Secondary\n"), nullptr);
  const auto& p = nodeById(doc, "n_circles")["params"];
  CHECK(p["leftCamera"] == "Both");
  CHECK(p["rightCamera"] == "Secondary");
}
