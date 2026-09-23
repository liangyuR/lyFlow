#include <doctest/doctest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "exec/executor.h"
#include "exec/result_store.h"
#include "lyflow/data.h"
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

TEST_CASE("StandardGap.yml 导入器注册了六种 kind：三种模式 × 积木 / 细粒度") {
  for (const char* kind : {"StandardGap.yml", "StandardGap.yml:template", "StandardGap.yml:model",
                           "StandardGap.yml:fine", "StandardGap.yml:template:fine",
                           "StandardGap.yml:model:fine"}) {
    const ImporterDesc* d = importer(kind);
    REQUIRE(d != nullptr);
    CHECK(d->fn != nullptr);
    CHECK(!d->label.empty());
  }
}

TEST_CASE("导入的图每个节点都写了 opVersion，等于 manifest 里该算子的版本（积木 / 细粒度、带回退）") {
  // 与编辑器新建节点（store/graph.ts 写 opVersion: op.version）一致：算子以后升主版本时导入的图能自动迁移
  std::unordered_map<std::string, std::string> versions;
  const nlohmann::json manifest = nlohmann::json::parse(ensureRegistry().toManifestJson());
  for (const auto& o : manifest["operators"]) {
    versions[o["id"].get<std::string>()] = o["version"].get<std::string>();
  }
  REQUIRE(versions.at("gap.locate_template") == "2.0.0");
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
        if (n["op"] == "ml.onnx_run" || n["op"] == "gap.locate_model") {
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

}  // namespace

TEST_CASE("带 flow.fallback 的细粒度图：备用闭包只经 b 端口流出") {
  Fixture f("fallback", "model_roi:\n  enabled: true\n  model_path: X:/m.onnx\n");
  const nlohmann::json doc = import("StandardGap.yml:model:fine", f.point);
  const std::vector<std::string> fallbacks = idsWithOp(doc, "flow.fallback");
  CHECK(fallbacks == std::vector<std::string>{
                         "n_fb_crop_p", "n_fb_crop_s", "n_fb_flushBase", "n_fb_flushRef",
                         "n_fb_gapLeft", "n_fb_gapRight", "n_fb_line", "n_fb_merged",
                         "n_fb_quality_base", "n_fb_quality_ref", "n_fb_ref_point",
                         "n_fb_roi_set", "n_fb_scan_set"});
  const std::vector<std::string> backupFits = idsWithOp(doc, "gap.fit_line");
  CHECK(backupFits == std::vector<std::string>{"b_n_fit_base", "b_n_fit_ref", "n_fit_base",
                                               "n_fit_ref"});
  for (const auto& n : doc["nodes"]) {
    const auto id = n["id"].get<std::string>();
    if (n["op"] != "gap.fit_line") continue;
    CHECK(n["params"]["endpoints"] == (id.rfind("b_", 0) == 0 ? "roi_intersection" : "inlier_ends"));
  }
  // 备用闭包不再自己读一遍文件：与模型那一支共用剔过 NaN 的两片云与去噪之后的合并云
  CHECK(idsWithOp(doc, "gap.load_profile_pair") == std::vector<std::string>{"n_load"});
  checkBackupOnlyViaB(doc);
}

TEST_CASE("带 flow.fallback 的积木图：两个定位节点并联，rois / scan 各一个 fallback") {
  Fixture f("fallback-blocks", "model_roi:\n  enabled: true\n  model_path: X:/m.onnx\n");
  const nlohmann::json doc = import("StandardGap.yml:model", f.point);
  CHECK(idsWithOp(doc, "flow.fallback") ==
        std::vector<std::string>{"n_fb_line", "n_fb_quality_base", "n_fb_quality_ref",
                                 "n_fb_ref_point", "n_fb_rois", "n_fb_scan"});
  CHECK(idsWithOp(doc, "gap.locate_model") == std::vector<std::string>{"n_model"});
  CHECK(idsWithOp(doc, "gap.locate_template") == std::vector<std::string>{"b_n_locate"});
  CHECK(idsWithOp(doc, "gap.role_line") == std::vector<std::string>{"b_n_line", "n_line"});
  for (const auto& n : doc["nodes"]) {
    const auto id = n["id"].get<std::string>();
    if (n["op"] != "gap.role_line") continue;
    CHECK(n["params"]["endpoints"] == (id.rfind("b_", 0) == 0 ? "roi_intersection" : "inlier_ends"));
  }
  // 圆拟合与结果汇总接回退之后的那一份；备用定位也吃同一个 read_scan
  CHECK(hasEdgeFrom(doc, "n_fb_rois", "n_circles", "rois"));
  CHECK(hasEdgeFrom(doc, "n_fb_scan", "n_circles", "scan"));
  CHECK(hasEdgeFrom(doc, "n_fb_rois", "n_bundle", "fallback"));
  CHECK(hasEdgeFrom(doc, "n_scan", "b_n_locate", "scan"));
  checkBackupOnlyViaB(doc);
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
  // 四个角色框随 RoiSet 进来（m8-plan L4/L5）；info 里的整体框被 roiOverall 端口顶掉
  Bundle rois("gap.RoiSet");
  rois.set("datum", Data::box2d(box(0.02f, 0.16f, 0.03f, 0.17f)));
  rois.set("target", Data::box2d(box(0.035f, 0.161f, 0.039f, 0.167f)));
  rois.set("seamLeft", Data::box2d(box(0.027f, 0.163f, 0.032f, 0.17f)));
  rois.set("seamRight", Data::box2d(box(0.034f, 0.163f, 0.038f, 0.17f)));
  Record info;
  info.type = "GapRoiInfo";
  info.data = {{"datumSide", "left"}, {"source", "template"}, {"alignment", nullptr},
               {"overallMm", {1, 2, 3, 4}}, {"cropStatus", nullptr}};
  rois.set("info", Data::record(info));
  inputs["rois"] = Data::bundle(std::move(rois));
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
  CHECK(b["effective_roi"]["flush_ref"][0].get<double>() == doctest::Approx(35.0));
  CHECK(b["effective_roi"]["gap_left"][0].get<double>() == doctest::Approx(27.0));
  CHECK(b["effective_roi"]["gap_right"][2].get<double>() == doctest::Approx(38.0));

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

TEST_CASE("圆心高度带接的是 flush_ref 的那条线，ref_type 不对就报错") {
  const std::string banded = withGapKeys(
      "  center_band: {right_above: 0.25, right_tolerance: 0.45}\n");
  const nlohmann::json doc = importText("StandardGap.yml:template:fine", banded, nullptr);
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
  importText("StandardGap.yml:template:fine", noRefLine, &s);
  CHECK_FALSE(s.ok);
}

TEST_CASE("逐侧相机从 YAML 落到 n_circles 的参数上") {
  const nlohmann::json doc = importText(
      "StandardGap.yml:template", withGapKeys("  right_circle_camera: Secondary\n"), nullptr);
  const auto& p = nodeById(doc, "n_circles")["params"];
  CHECK(p["leftCamera"] == "Both");
  CHECK(p["rightCamera"] == "Secondary");
}

namespace {

std::string withFlushKeys(const std::string& lines) {
  std::string yaml = kConfig;
  const std::size_t at = yaml.find("gap:\n");
  REQUIRE(at != std::string::npos);
  yaml.insert(at, lines);
  return yaml;
}

}  // namespace

TEST_CASE("方向基准默认不生成任何节点") {
  const nlohmann::json doc = importText("StandardGap.yml:template:fine", kConfig, nullptr);
  CHECK_FALSE(hasOp(doc, "gap.datum_window"));
  CHECK_FALSE(hasEdge(doc, "n_fit_base", "refLine"));
  // 一个方向相关的参数都不写进图，留给算子默认的 free
  CHECK_FALSE(nodeById(doc, "n_fit_base")["params"].contains("dirMode"));
}

TEST_CASE("配了 long_plane 就长出「窗 → 裁 → 拟」三个节点并接到 n_fit_base") {
  const nlohmann::json doc = importText(
      "StandardGap.yml:template:fine",
      withFlushKeys("  base_direction: {datum: long_plane, side: left, length_mm: 13,"
                    " height_mm: 2.5, mode: fixed, nominal_deg: 3.0}\n"),
      nullptr);
  REQUIRE(hasOp(doc, "gap.datum_window"));
  const auto& win = nodeById(doc, "n_datum_box")["params"];
  CHECK(win["side"] == "left");
  CHECK(win["lengthMm"] == 13.0);
  CHECK(win["heightMm"] == 2.5);
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
    if (e["to"]["node"] != "n_datum_box") continue;
    if (e["to"]["port"] == "anchor") {
      sawAnchor = true;
      CHECK(e["from"]["port"] == "gapLeft");
    }
    if (e["to"]["port"] == "heightAnchor") {
      sawHeight = true;
      CHECK(e["from"]["port"] == "flushBase");
    }
  }
  CHECK(sawAnchor);
  CHECK(sawHeight);
  CHECK(nodeById(doc, "n_fit_datum")["params"]["minInliers"] == 60);
}

TEST_CASE("datum 与 mode 写错了导入就报错") {
  for (const char* line : {"  base_direction: {datum: whatever}\n",
                           "  base_direction: {datum: long_plane, mode: whatever}\n"}) {
    Status s;
    importText("StandardGap.yml:template", withFlushKeys(line), &s);
    CHECK_FALSE(s.ok);
  }
}

TEST_CASE("方向基准的锚和高度锚可以分别指定") {
  const nlohmann::json doc = importText(
      "StandardGap.yml:template:fine",
      withFlushKeys("  base_direction: {datum: long_plane, anchor: gap_right,"
                    " height_anchor: flush_ref, mode: band, nominal_deg: 1.0}\n"),
      nullptr);
  for (const auto& e : doc["edges"]) {
    if (e["to"]["node"] != "n_datum_box") continue;
    if (e["to"]["port"] == "anchor") CHECK(e["from"]["port"] == "gapRight");
    if (e["to"]["port"] == "heightAnchor") CHECK(e["from"]["port"] == "flushRef");
  }
  // side 跟着 anchor 走：锚在右边的缝框上，窗口就往右推
  CHECK(nodeById(doc, "n_datum_box")["params"]["side"] == "right");
  CHECK(nodeById(doc, "n_fit_base")["params"]["dirMode"] == "band");
}

TEST_CASE("圆心高度带的 mode 从 YAML 落到参数上，写错就报错") {
  const nlohmann::json doc = importText(
      "StandardGap.yml:template",
      withGapKeys("  center_band: {left_above: 0.5, left_tolerance: 0.4, left_mode: guard}\n"),
      nullptr);
  const auto& p = nodeById(doc, "n_circles")["params"];
  CHECK(p["leftCenterMode"] == "guard");
  CHECK(p["rightCenterMode"] == "always");
  Status s;
  importText("StandardGap.yml:template",
             withGapKeys("  center_band: {left_above: 0.5, left_tolerance: 0.4,"
                         " left_mode: whatever}\n"),
             &s);
  CHECK_FALSE(s.ok);
}

TEST_CASE("weak_fit 的地板从 YAML 落到 n_circles 上，不写就是 0") {
  CHECK(nodeById(importText("StandardGap.yml:template", kConfig, nullptr), "n_circles")
            ["params"]["rightMinArcDeg"] == 0.0);
  const nlohmann::json doc = importText(
      "StandardGap.yml:template",
      withGapKeys("  weak_fit: {right_min_arc_deg: 55, right_min_inliers: 10}\n"), nullptr);
  const auto& p = nodeById(doc, "n_circles")["params"];
  CHECK(p["rightMinArcDeg"] == 55.0);
  CHECK(p["rightMinInliers"] == 10);
  CHECK(p["leftMinArcDeg"] == 0.0);
}

namespace {

std::vector<std::string> stringList(const nlohmann::json& a) {
  std::vector<std::string> out;
  for (const auto& e : a) out.push_back(e.get<std::string>());
  std::sort(out.begin(), out.end());
  return out;
}

/// 用进程内的全量注册表校验一张图（std 包 + gap 包都在），只留 error。
std::vector<nlohmann::json> validationErrors(const nlohmann::json& doc) {
  const nlohmann::json diags =
      nlohmann::json::parse(exec::validateGraphJson(doc.dump(), std::filesystem::path()));
  std::vector<nlohmann::json> errors;
  for (const auto& d : diags) {
    if (d["severity"] == "error") errors.push_back(d);
  }
  return errors;
}

}  // namespace

TEST_CASE("细粒度图：fit_line 接 toward、不写 side，business_rois 不写 datumSide（auto）") {
  for (const char* kind : {"StandardGap.yml:template:fine", "StandardGap.yml:model:fine"}) {
    CAPTURE(kind);
    Fixture f(std::string("m7-") + (std::strcmp(kind, "StandardGap.yml:model:fine") == 0 ? "model" : "tpl"),
              "model_roi:\n  enabled: true\n  model_path: X:/m.onnx\n");
    const nlohmann::json doc = import(kind, f.point);
    REQUIRE(doc.is_object());
    for (const auto& n : doc["nodes"]) {
      const std::string id = n["id"].get<std::string>();
      CAPTURE(id);
      if (n["op"] == "gap.fit_line") {
        CHECK_FALSE(n.value("params", nlohmann::json::object()).contains("side"));
        CHECK(hasEdge(doc, id.c_str(), "toward"));
      }
      if (n["op"] == "gap.business_rois") {
        // 基准件在哪一侧由框推出（m8-plan L7）：一个左右都不写
        const auto params = n.value("params", nlohmann::json::object());
        CHECK_FALSE(params.contains("baseSide"));
        CHECK_FALSE(params.contains("datumSide"));
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
    if (e["to"]["port"] != "toward") continue;
    if (e["to"]["node"] == "n_fit_base" || e["to"]["node"] == "n_fit_ref") {
      CHECK(e["from"]["node"] == "n_rois");
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
      CHECK(e["from"]["port"] == "gapRight");
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
        std::vector<std::string>{"n_circles.offset", "n_gap.offset"});
  CHECK(tpl["params"]["gapOffset"]["default"] == 0.0);
  CHECK_FALSE(tpl["params"].contains("modelPath"));
  // 被绑定的参数不能再显式写在节点上（一处定义）
  CHECK_FALSE(nodeById(tpl, "n_gap")["params"].contains("offset"));
  CHECK_FALSE(nodeById(tpl, "n_circles")["params"].contains("offset"));

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
        std::vector<std::string>{"n_circles.offset", "n_gap.offset"});
  }
}

TEST_CASE("导入的图过 core 的 validate：模板、模型、带 fallback 三种 × 积木 / 细粒度") {
  Fixture withFallback("m7-validate", "model_roi:\n  enabled: true\n  model_path: X:/m.onnx\n");
  Fixture plain("m7-validate-plain");
  std::ofstream(plain.point / "only.onnx") << "x";
  const struct {
    const char* kind;
    const Fixture* fixture;
  } cases[6] = {{"StandardGap.yml:template", &plain},
                {"StandardGap.yml:model", &plain},
                {"StandardGap.yml:model", &withFallback},
                {"StandardGap.yml:template:fine", &plain},
                {"StandardGap.yml:model:fine", &plain},
                {"StandardGap.yml:model:fine", &withFallback}};
  for (const auto& c : cases) {
    CAPTURE(c.kind);
    const nlohmann::json doc = import(c.kind, c.fixture->point);
    REQUIRE(doc.is_object());
    const auto errors = validationErrors(doc);
    for (const auto& e : errors) CAPTURE(e.dump());
    CHECK(errors.empty());
    if (!errors.empty()) MESSAGE(errors.front().dump());
  }
}

TEST_CASE("gap.fit_line dirMode=band 没接 refLine：validate 就报 bad_param，节点不执行") {
  nlohmann::json doc;
  doc["schemaVersion"] = 1;
  doc["id"] = "01M7FITLINEBAND";
  doc["nodes"] = nlohmann::json::array({
      {{"id", "g"}, {"op", "gen.synthetic"}, {"params", {{"pointCount", 2000}}}},
      {{"id", "roi"}, {"op", "gap.overall_roi"}},
      {{"id", "fit"}, {"op", "gap.fit_line"}, {"params", {{"dirMode", "band"}}}},
  });
  const auto edge = [](const char* id, const char* fn, const char* fp, const char* tn,
                       const char* tp) {
    return nlohmann::json{{"id", id},
                          {"from", {{"node", fn}, {"port", fp}}},
                          {"to", {{"node", tn}, {"port", tp}}}};
  };
  doc["edges"] = nlohmann::json::array({
      edge("e1", "g", "cloud", "roi", "primary"),
      edge("e2", "g", "cloud", "roi", "secondary"),
      edge("e3", "g", "cloud", "fit", "cloud"),
      edge("e4", "roi", "box", "fit", "box"),
      edge("e5", "roi", "box", "fit", "toward"),
  });
  const auto errors = validationErrors(doc);
  REQUIRE(errors.size() == 1);
  CHECK(errors[0]["nodeId"] == "fit");
  CHECK(errors[0]["code"] == "bad_param");
  CHECK(errors[0]["phase"] == "validate");
  CHECK(errors[0]["paramPath"] == "dirMode");

  // compute 里不再有这一条：执行器根本不会调到它
  exec::ResultStore::instance().clear();
  struct Log {
    std::vector<nlohmann::json> events;
  } log;
  exec::RunOptions options;
  options.runId = "m7-fit-line-band";
  {
    exec::Run run(doc.dump(), options,
                  [](const char* json, void* user) {
                    static_cast<Log*>(user)->events.push_back(nlohmann::json::parse(json));
                  },
                  &log);
    run.join();
  }
  bool fitRan = false;
  for (const auto& e : log.events) {
    if (e.value("kind", "") == "node_state" && e.value("nodeId", "") == "fit" &&
        e.value("state", "") == "running") {
      fitRan = true;
    }
  }
  CHECK_FALSE(fitRan);
}
