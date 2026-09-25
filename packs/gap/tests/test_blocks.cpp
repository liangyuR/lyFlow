// 积木算子（m8-plan M8a）：两种建图方式同源、逐帧相同；方向由 RoiSet 推出；加载期拦住填反的框；
// 积木与细粒度算子在同一张图里混用。数据是合成的一对剖面（左高右低、缝两侧各一段圆角），
// 仓库不进二进制数据，PCD 在临时目录里现写。
#include <doctest/doctest.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "../ops/gap_fine.h"
#include "exec/executor.h"
#include "exec/result_store.h"
#include "helpers.h"
#include "lyflow/c_api.h"
#include "lyflow/registry.h"

namespace lyflow::packs::gap {
void registerPackOps(Registry& r);
}  // namespace lyflow::packs::gap

namespace {

using namespace lyflow;
using Json = nlohmann::json;
namespace fs = std::filesystem;

const Registry& packRegistry() {
  static Registry r = [] {
    Registry reg;
    packs::gap::registerPackOps(reg);
    return reg;
  }();
  return r;
}

// ------------------------------------------------------------ 合成剖面（毫米）
// 左板顶面 y=165（测量帧里 y 越小越高），缝边是圆心 (-3, 166)、半径 1 的四分之一圆；
// 右板顶面 y=164（比左板高 1 mm），缝边圆心 (3, 165)。x 步长 0.05 mm。

struct P2 {
  double x, y;
};

std::vector<P2> profile(double shiftMm) {
  std::vector<P2> pts;
  for (double x = -20.0 + shiftMm; x < -3.0; x += 0.05) pts.push_back({x, 165.0});
  for (int k = 0; k <= 30; ++k) {
    const double t = (M_PI / 2) * k / 30.0 + shiftMm * 0.3;
    if (t > M_PI / 2) break;
    pts.push_back({-3.0 + std::sin(t), 166.0 - std::cos(t)});
  }
  for (int k = 30; k >= 0; --k) {
    const double t = (M_PI / 2) * k / 30.0 + shiftMm * 0.3;
    if (t > M_PI / 2) continue;
    pts.push_back({3.0 - std::sin(t), 165.0 - std::cos(t)});
  }
  for (double x = 3.0 + 0.05 - shiftMm; x < 20.0; x += 0.05) pts.push_back({x, 164.0});
  return pts;
}

/// 传感器帧（x=u, y=0, z=h）的 ASCII PCD，米。nanEvery>0 时每隔几个点插一个 NaN 槽。
void writeSensorPcd(const fs::path& file, const std::vector<P2>& pts, int nanEvery) {
  std::vector<std::array<double, 3>> rows;
  for (std::size_t i = 0; i < pts.size(); ++i) {
    if (nanEvery > 0 && i % static_cast<std::size_t>(nanEvery) == 0) {
      rows.push_back({std::numeric_limits<double>::quiet_NaN(), 0.0,
                      std::numeric_limits<double>::quiet_NaN()});
    }
    rows.push_back({pts[i].x / 1000.0, 0.0, pts[i].y / 1000.0});
  }
  std::ofstream out(file);
  out << "# .PCD v0.7\nVERSION 0.7\nFIELDS x y z\nSIZE 4 4 4\nTYPE F F F\nCOUNT 1 1 1\n"
      << "WIDTH " << rows.size() << "\nHEIGHT 1\nVIEWPOINT 0 0 0 1 0 0 0\nPOINTS " << rows.size()
      << "\nDATA ascii\n";
  for (const auto& r : rows) {
    for (int k = 0; k < 3; ++k) {
      if (k) out << ' ';
      if (std::isnan(r[k])) {
        out << "nan";
      } else {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.7f", r[k]);
        out << buf;
      }
    }
    out << '\n';
  }
}

/// 模板是测量帧（x, y, 0）。
void writeTemplatePcd(const fs::path& file, const std::vector<P2>& pts, bool left) {
  std::vector<P2> side;
  for (const P2& p : pts) {
    if ((p.x < 0) == left) side.push_back(p);
  }
  std::ofstream out(file);
  out << "# .PCD v0.7\nVERSION 0.7\nFIELDS x y z\nSIZE 4 4 4\nTYPE F F F\nCOUNT 1 1 1\n"
      << "WIDTH " << side.size() << "\nHEIGHT 1\nVIEWPOINT 0 0 0 1 0 0 0\nPOINTS " << side.size()
      << "\nDATA ascii\n";
  for (const P2& p : side) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.7f %.7f 0", p.x / 1000.0, p.y / 1000.0);
    out << buf << '\n';
  }
}

constexpr const char* kSyntheticConfig = R"(
template_dir: StandardGap
common_settings:
  seg_mode: ROI
  overall_roi: [-18, 150, 18, 180]
  overall_roi_mode: auto_center
  line_fit_distance: 0.1
  circle_fit_distance: 0.03
  using_camera: Both
  filter: {using_removal: true, filter_radius: 0.4, filter_neighbors: 3}
flush:
  base_side: left
  base_type: fit line
  ref_type: line end
  base_roi: [-15, 163, -5, 167]
  ref_roi: [5, 162, 15, 166]
  segment_points: 60
  offset: 0
  tolerances: {nominal: 1, up_deviation: 1, low_deviation: -1}
gap:
  left_type: circle
  right_type: circle
  definition: B
  left_roi: [-4.5, 164, -1.5, 167]
  right_roi: [1.5, 163, 4.5, 166]
  radius: {left_circle_radius_min: 0.5, left_circle_radius_max: 2.0,
           right_circle_radius_min: 0.5, right_circle_radius_max: 2.0}
  tolerances: {nominal: 4, up_deviation: 1, low_deviation: -1}
align:
  align_cloud: true
  ICP: {num_neighbor: 10, min_score: 30, max_matching_dist: 2, max_fitness_dist: 0.5,
        max_iteration_num: 50, bidirection_align: false}
  template_candidates:
    - {id: f1, left: left.pcd, right: right.pcd}
)";

struct Scene {
  explicit Scene(const std::string& tag) {
    root = fs::temp_directory_path() / ("lyflow-blocks-" + tag);
    fs::remove_all(root);
    fs::create_directories(root / "StandardGap");
    const std::vector<P2> master = profile(0.0);
    const std::vector<P2> slave = profile(0.025);
    writeSensorPcd(root / "LaserProfile_L0_Master_x.pcd", master, 97);
    writeSensorPcd(root / "LaserProfile_R1_Slave_x.pcd", slave, 0);
    writeTemplatePcd(root / "StandardGap" / "left.pcd", master, true);
    writeTemplatePcd(root / "StandardGap" / "right.pcd", master, false);
  }
  ~Scene() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
  fs::path root;
};

Json importAs(const char* kind, const std::string& yaml, const fs::path& baseDir) {
  const ImporterDesc* desc = packRegistry().findImporter(kind);
  REQUIRE(desc != nullptr);
  std::string out;
  const Status s = desc->fn(yaml, baseDir, out);
  REQUIRE_MESSAGE(s.ok, s.message);
  return Json::parse(out);
}

struct Reading {
  double flush = std::numeric_limits<double>::quiet_NaN();
  double gap = std::numeric_limits<double>::quiet_NaN();
  bool ok = false;
  std::string status;
};

Reading run(const Json& doc, const fs::path& baseDir) {
  test::Session s(doc, baseDir);
  test::RunLog& log = s.wait();
  Reading r;
  r.status = log.runStatus();
  Data flush, gap;
  exec::ResultStore& store = exec::ResultStore::instance();
  if (store.get(s.runId(), "n_flush", "value", flush) && store.get(s.runId(), "n_gap", "value", gap)) {
    r.flush = flush.asMeasurement()->value;
    r.gap = gap.asMeasurement()->value;
    r.ok = flush.asMeasurement()->ok && gap.asMeasurement()->ok;
  }
  if (!r.ok) {
    for (const auto& e : log.ofKind("node_state")) {
      if (e.value("state", "") == "error") MESSAGE(e.dump());
    }
  }
  return r;
}

std::vector<Json> errorsOf(const Json& doc) {
  std::vector<Json> out;
  for (const Json& d : Json::parse(exec::validateGraphJson(doc.dump(), {}))) {
    if (d.value("severity", "") == "error") {
      MESSAGE(d.dump());
      out.push_back(d);
    }
  }
  return out;
}

Json& nodeOf(Json& doc, const char* id) {
  for (Json& n : doc["nodes"]) {
    if (n["id"] == id) return n;
  }
  FAIL("图里没有节点 " << id);
  static Json none;
  return none;
}

void addEdge(Json& doc, const std::string& from, const char* fromPort, const std::string& to,
             const char* toPort) {
  static int counter = 0;
  doc["edges"].push_back({{"id", "x" + std::to_string(counter++)},
                          {"from", {{"node", from}, {"port", fromPort}}},
                          {"to", {{"node", to}, {"port", toPort}}}});
}

void dropEdgesInto(Json& doc, const char* node, const char* port) {
  Json kept = Json::array();
  for (const Json& e : doc["edges"]) {
    if (e["to"]["node"] == node && e["to"]["port"] == port) continue;
    kept.push_back(e);
  }
  doc["edges"] = std::move(kept);
}

const std::set<std::string>& blockOps() {
  static const std::set<std::string> ops = {"gap.read_scan",     "gap.locate_template",
                                            "gap.locate_model",  "gap.role_line",
                                            "gap.ref_point",     "gap.seam_circles",
                                            "gap.datum_direction"};
  return ops;
}

}  // namespace

TEST_CASE("积木算子与细粒度算子同源：细粒度算子注册的 compute 就是积木算子调的那个包内函数") {
  namespace fine = packs::gap::fine;
  const std::pair<const char*, ComputeFn> pairs[] = {
      {"gap.load_profile_pair", &fine::loadPair},
      {"gap.to_measurement_frame", &fine::toMeasurementFrame},
      {"gap.load_template", &fine::loadTemplate},
      {"gap.overall_roi", &fine::overallRoi},
      {"gap.align_template", &fine::alignTemplate},
      {"gap.select_alignment", &fine::selectAlignment},
      {"gap.business_rois", &fine::businessRois},
      {"gap.fit_line", &fine::fitLine},
      {"gap.selected_point", &fine::selectedPoint},
      {"gap.nearest_to_line", &fine::nearestToLine},
      {"gap.fit_gap_circles", &fine::fitGapCircles},
      {"gap.datum_window", &fine::datumWindow},
      {"gap.profile_tensor", &fine::profileTensor},
      {"gap.labels_from_logits", &fine::labelsFromLogits},
      {"gap.roi_from_labels", &fine::roiFromLabels},
      {"gap.drop_non_finite", &fine::dropNonFinite},
      {"gap.roll_anchored_crop", &fine::rollAnchoredCrop},
  };
  for (const auto& [id, fn] : pairs) {
    CAPTURE(id);
    const OperatorDesc* op = packRegistry().find(id);
    REQUIRE(op != nullptr);
    CHECK(op->compute == fn);
  }
  // 校验钩子也是同一个：role_line / seam_circles / datum_direction 直接挂细粒度算子的
  CHECK(packRegistry().find("gap.role_line")->validate == packRegistry().find("gap.fit_line")->validate);
  CHECK(packRegistry().find("gap.seam_circles")->validate ==
        packRegistry().find("gap.fit_gap_circles")->validate);
  CHECK(packRegistry().find("gap.datum_direction")->validate ==
        packRegistry().find("gap.datum_window")->validate);
}

TEST_CASE("积木算子的参数里没有 side / toward / baseSide / datumSide（m8-plan L7）") {
  for (const std::string& id : blockOps()) {
    CAPTURE(id);
    const OperatorDesc* op = packRegistry().find(id);
    REQUIRE(op != nullptr);
    for (const Param& p : op->params) {
      CAPTURE(p.name);
      for (const char* banned : {"side", "toward", "baseSide", "datumSide"}) {
        CHECK(p.name != banned);
      }
    }
  }
  // 两种 Bundle 都声明了，字段按角色命名、不再有 base / ref
  const BundleDesc* rois = packRegistry().findBundle("gap.RoiSet");
  REQUIRE(rois != nullptr);
  std::vector<std::string> names;
  for (const BundleField& f : rois->fields) names.push_back(f.name);
  CHECK(names == std::vector<std::string>{"datum", "target", "seamLeft", "seamRight", "info"});
  const BundleDesc* scan = packRegistry().findBundle("gap.ScanPair");
  REQUIRE(scan != nullptr);
  CHECK(scan->fields.size() == 3);
  // 只装了 gap 的注册表没有 core 的类型表，自检用进程内那份全量的
  CHECK(ensureRegistry().validate().empty());
}

TEST_CASE("导入的模板路径积木图：10 个节点，只用积木算子与 flush / gap / judge / result_bundle") {
  Scene scene("count");
  const Json doc = importAs("StandardGap.yml:template", kSyntheticConfig, scene.root);
  CHECK(doc["nodes"].size() == 10);
  CHECK(doc["nodes"].size() <= 12);
  const std::set<std::string> allowed = {"gap.flush", "gap.gap", "gap.judge", "gap.result_bundle"};
  for (const Json& n : doc["nodes"]) {
    const std::string op = n["op"];
    CAPTURE(op);
    CHECK((blockOps().count(op) || allowed.count(op)));
    for (const char* banned : {"side", "toward", "baseSide", "datumSide"}) {
      CHECK_FALSE(n.value("params", Json::object()).contains(banned));
    }
  }
  // 「同一个框接两次」的边一条都没有：每个节点的每个输入端口只从一个上游来，而积木图里
  // 没有任何 Box2D 边（框都在 RoiSet 里走）
  for (const Json& e : doc["edges"]) {
    CHECK(e["from"]["port"] != "flushBase");
    CHECK(e["from"]["port"] != "gapLeft");
  }
  CHECK(errorsOf(doc).empty());
}

TEST_CASE("夹具上积木图与 --fine 细粒度图的 flush、gap 逐位相同（M8a 验收 1）") {
  Scene scene("same");
  const Json blocks = importAs("StandardGap.yml:template", kSyntheticConfig, scene.root);
  const Json fine = importAs("StandardGap.yml:template:fine", kSyntheticConfig, scene.root);
  CHECK(errorsOf(blocks).empty());
  CHECK(errorsOf(fine).empty());
  const Reading a = run(blocks, scene.root);
  const Reading b = run(fine, scene.root);
  REQUIRE(a.ok);
  REQUIRE(b.ok);
  CHECK(a.flush == b.flush);
  CHECK(a.gap == b.gap);
  // 合成剖面：参考面比基准面高 1 mm（y 小 = 高 = 正），两圆心距 √37、各减一个半径
  CHECK(a.flush == doctest::Approx(1.0).epsilon(0.05));
  CHECK(a.gap == doctest::Approx(std::sqrt(37.0) - 2.0).epsilon(0.05));
  MESSAGE("flush=" << a.flush << " gap=" << a.gap);
}

TEST_CASE("gap 的两种 Bundle：scan.merged 取得到点云、图输出指向字段、ScanPair 接 RoiSet 报错（M8a 验收 3）") {
  Scene scene("bundle");
  Json doc = importAs("StandardGap.yml:template", kSyntheticConfig, scene.root);
  doc["outputs"]["datum"] = {{"node", "n_locate"}, {"port", "rois.datum"}};
  doc["outputs"]["side"] = {{"node", "n_locate"}, {"port", "rois.info"}};
  CHECK(errorsOf(doc).empty());
  test::Session s(doc, scene.root);
  test::RunLog& log = s.wait();
  REQUIRE(log.finalState("n_flush") == "done");

  lyflow_cloud_view view{};
  REQUIRE(lyflow_output_cloud(s.runId().c_str(), "n_scan", "scan.merged", 0, &view) == 0);
  const std::uint32_t full = view.total_points;
  CHECK(full > 1000u);
  lyflow_cloud_view_free(&view);
  // 定位之后的合并云是整体框裁过的：点不会比读剖面的那一份多
  REQUIRE(lyflow_output_cloud(s.runId().c_str(), "n_locate", "scan.merged", 0, &view) == 0);
  CHECK(view.total_points <= full);
  lyflow_cloud_view_free(&view);

  const Json outputs = Json::parse(exec::runOutputsJson(s.runId()));
  CHECK(outputs["datum"]["type"] == "Box2D");
  CHECK(outputs["datum"]["value"]["min"][0].get<double>() == doctest::Approx(-0.015).epsilon(0.01));
  CHECK(outputs["side"]["value"]["data"]["datumSide"] == "left");
  CHECK(outputs["side"]["value"]["data"]["source"] == "template");

  Json wrong = importAs("StandardGap.yml:template", kSyntheticConfig, scene.root);
  dropEdgesInto(wrong, "n_line", "rois");
  addEdge(wrong, "n_scan", "scan", "n_line", "rois");
  bool mismatch = false;
  for (const Json& e : errorsOf(wrong)) {
    if (e["code"] == "type_mismatch" && e["nodeId"] == "n_line") {
      mismatch = true;
      CHECK(e["message"].get<std::string>().find("Bundle<gap.ScanPair> → Bundle<gap.RoiSet>") !=
            std::string::npos);
    }
  }
  CHECK(mismatch);
}

TEST_CASE("同一张图里混用积木与细粒度算子能跑，结果与纯积木图相同（M8a 验收 5）") {
  Scene scene("mixed");
  const Json blocks = importAs("StandardGap.yml:template", kSyntheticConfig, scene.root);
  const Reading pure = run(blocks, scene.root);
  REQUIRE(pure.ok);

  SUBCASE("积木定位 → split_* → 细粒度的裁剪 + 拟合 → gap.flush") {
    Json doc = blocks;
    doc["nodes"].push_back({{"id", "x_rois"}, {"op", "gap.split_roi_set"}});
    doc["nodes"].push_back({{"id", "x_scan"}, {"op", "gap.split_scan_pair"}});
    doc["nodes"].push_back({{"id", "x_crop"}, {"op", "filter.crop_box2d"}, {"params", {{"bounds", "open"}}}});
    doc["nodes"].push_back(
        {{"id", "x_fit"}, {"op", "gap.fit_line"},
         {"params", {{"distThresh", 0.1}, {"segmentPoints", 60}, {"endpoints", "roi_intersection"}}}});
    addEdge(doc, "n_locate", "rois", "x_rois", "rois");
    addEdge(doc, "n_locate", "scan", "x_scan", "scan");
    addEdge(doc, "x_scan", "merged", "x_crop", "cloud");
    addEdge(doc, "x_rois", "datum", "x_crop", "box");
    addEdge(doc, "x_crop", "cloud", "x_fit", "cloud");
    addEdge(doc, "x_rois", "datum", "x_fit", "box");
    addEdge(doc, "x_rois", "seam", "x_fit", "toward");
    dropEdgesInto(doc, "n_flush", "baseLine");
    addEdge(doc, "x_fit", "line", "n_flush", "baseLine");
    CHECK(errorsOf(doc).empty());
    const Reading mixed = run(doc, scene.root);
    REQUIRE(mixed.ok);
    CHECK(mixed.flush == pure.flush);
    CHECK(mixed.gap == pure.gap);
  }
  SUBCASE("细粒度的 business_rois → make_roi_set → 积木的 role_line") {
    Json fine = importAs("StandardGap.yml:template:fine", kSyntheticConfig, scene.root);
    fine["nodes"].push_back({{"id", "x_line"}, {"op", "gap.role_line"},
                             {"params", {{"distThresh", 0.1}, {"segmentPoints", 60}}}});
    addEdge(fine, "n_scan_set", "scan", "x_line", "scan");
    addEdge(fine, "n_roi_set", "rois", "x_line", "rois");
    dropEdgesInto(fine, "n_flush", "baseLine");
    addEdge(fine, "x_line", "line", "n_flush", "baseLine");
    CHECK(errorsOf(fine).empty());
    const Reading mixed = run(fine, scene.root);
    REQUIRE(mixed.ok);
    CHECK(mixed.flush == pure.flush);
    CHECK(mixed.gap == pure.gap);
  }
}

TEST_CASE("locate_template 把 datum 框拖到 target 那一侧：validate 在执行前就报错（M8a 验收 4）") {
  Scene scene("dragged");
  Json doc = importAs("StandardGap.yml:template", kSyntheticConfig, scene.root);
  // 把槽 1 的 datum 挪到缝右边、target 旁边
  nodeOf(doc, "n_locate")["params"]["template1DatumRoi"] = Json::array({16, 162, 17.5, 166});
  const std::vector<Json> errors = errorsOf(doc);
  REQUIRE(errors.size() == 1);
  CHECK(errors[0]["nodeId"] == "n_locate");
  CHECK(errors[0]["code"] == "bad_param");
  CHECK(errors[0]["phase"] == "validate");
  CHECK(errors[0]["paramPath"] == "template1DatumRoi");
  CHECK(errors[0]["message"].get<std::string>().find("同一侧") != std::string::npos);

  test::Session s(doc, scene.root);
  test::RunLog& log = s.wait();
  CHECK(log.finalState("n_locate") == "error");
  bool ran = false;
  for (const auto& e : log.ofKind("node_state")) {
    if (e.value("nodeId", "") == "n_locate" && e.value("state", "") == "running") ran = true;
  }
  CHECK_FALSE(ran);

  SUBCASE("其余几条：框退化、缝框颠倒、槽之间基准件不同侧") {
    Json bad = importAs("StandardGap.yml:template", kSyntheticConfig, scene.root);
    Json& p = nodeOf(bad, "n_locate")["params"];
    p["template1SeamLeftRoi"] = Json::array({1.5, 163, 4.5, 166});
    p["template1SeamRightRoi"] = Json::array({-4.5, 164, -1.5, 167});
    CHECK(errorsOf(bad).at(0)["message"].get<std::string>().find("颠倒") != std::string::npos);
    p["template1SeamRightRoi"] = Json::array({4.5, 163, 1.5, 166});
    CHECK(errorsOf(bad).at(0)["message"].get<std::string>().find("退化") != std::string::npos);

    Json slots = importAs("StandardGap.yml:template", kSyntheticConfig, scene.root);
    Json& q = nodeOf(slots, "n_locate")["params"];
    q["template2Enabled"] = true;
    q["template2DatumRoi"] = Json::array({5, 162, 15, 166});
    q["template2TargetRoi"] = Json::array({-15, 163, -5, 167});
    q["template2SeamLeftRoi"] = Json::array({-4.5, 164, -1.5, 167});
    q["template2SeamRightRoi"] = Json::array({1.5, 163, 4.5, 166});
    const auto e = errorsOf(slots);
    REQUIRE(e.size() == 1);
    CHECK(e[0]["paramPath"] == "template2DatumRoi");
    CHECK(e[0]["message"].get<std::string>().find("不一致") != std::string::npos);
  }
}

TEST_CASE("每个模板槽各有自己的四框（m8-plan L19）：没有公共四框、没有 Override，槽 1 恒启用") {
  const OperatorDesc* op = packRegistry().find("gap.locate_template");
  REQUIRE(op != nullptr);
  CHECK(op->version == "2.0.0");
  for (const Param& p : op->params) {
    CAPTURE(p.name);
    for (const char* gone : {"datumRoi", "targetRoi", "seamLeftRoi", "seamRightRoi"}) {
      CHECK(p.name != gone);
    }
    CHECK(p.name.find("Override") == std::string::npos);
    CHECK(p.name != "template1Enabled");
  }
  for (int k = 1; k <= 4; ++k) {
    const std::string prefix = "template" + std::to_string(k);
    for (const char* role : {"Datum", "Target", "SeamLeft", "SeamRight"}) {
      const Param* roi = findParam(*op, prefix + role + "Roi");
      REQUIRE(roi != nullptr);
      // 槽 1 的四框是「人要填的」；槽 2–4 在高级里、跟着自己的 Enabled 显示
      CHECK(roi->advanced == (k > 1));
      CHECK(roi->group == "模板槽 " + std::to_string(k));
      if (k > 1) CHECK(roi->visibleWhen.param == prefix + "Enabled");
    }
  }

  // 导入器：候选自带 rois 的用自己的，没带的用配置里的全局 rois 填进该槽
  std::string yaml = kSyntheticConfig;
  const std::string from = "    - {id: f1, left: left.pcd, right: right.pcd}\n";
  yaml.replace(yaml.find(from), from.size(),
               from +
                   "    - {id: f2, left: left.pcd, right: right.pcd, rois: {flush: {base_roi: [-14, 163,"
                   " -6, 167]}, gap: {right_roi: [1.6, 163, 4.4, 166]}}}\n"
                   "    - {id: f3, left: left.pcd, right: right.pcd}\n");
  Scene scene("slots");
  Json doc = importAs("StandardGap.yml:template", yaml, scene.root);
  const Json& p = nodeOf(doc, "n_locate")["params"];
  for (auto it = p.begin(); it != p.end(); ++it) {
    CAPTURE(it.key());
    CHECK(it.key().find("Override") == std::string::npos);
    CHECK((it.key() != "datumRoi" && it.key() != "targetRoi" && it.key() != "seamLeftRoi" &&
           it.key() != "seamRightRoi"));
  }
  CHECK_FALSE(p.contains("template1Enabled"));
  CHECK(p["template2Enabled"] == true);
  CHECK(p["template3Enabled"] == true);
  CHECK_FALSE(p.contains("template4Enabled"));
  CHECK(p["template1DatumRoi"] == Json::array({-15, 163, -5, 167}));
  CHECK(p["template2DatumRoi"] == Json::array({-14, 163, -6, 167}));          // 自己的
  CHECK(p["template2TargetRoi"] == Json::array({5, 162, 15, 166}));           // 没带 → 全局
  CHECK(p["template2SeamRightRoi"] == Json::array({1.6, 163, 4.4, 166}));     // 自己的
  CHECK(p["template3SeamLeftRoi"] == Json::array({-4.5, 164, -1.5, 167}));    // 全局
  CHECK(errorsOf(doc).empty());
  const Reading three = run(doc, scene.root);
  REQUIRE(three.ok);

  // L22：槽 3 的 datum 拖到 target 同侧，诊断标明「模板 3」、指到槽 3 自己的框
  nodeOf(doc, "n_locate")["params"]["template3DatumRoi"] = Json::array({6, 162, 9, 166});
  const std::vector<Json> errors = errorsOf(doc);
  REQUIRE(errors.size() == 1);
  CHECK(errors[0]["paramPath"] == "template3DatumRoi");
  const std::string message = errors[0]["message"].get<std::string>();
  CHECK(message.rfind("模板 3 · f3：", 0) == 0);
  CHECK(message.find("同一侧") != std::string::npos);
  // 关掉的槽不查
  nodeOf(doc, "n_locate")["params"]["template3Enabled"] = false;
  CHECK(errorsOf(doc).empty());
}

TEST_CASE("locate_template v1 → v2 迁移：公共四框抄进没开覆盖的槽，覆盖框留给自己的槽") {
  const OperatorDesc* op = packRegistry().find("gap.locate_template");
  REQUIRE(op != nullptr);
  REQUIRE(op->migrations.size() == 1);
  const MigrateFn migrate = op->migrations[0].apply;
  const Json global = Json::array({-15, 163, -5, 167});
  const Json own = Json::array({-14, 163, -6, 167});

  SUBCASE("槽 1 用公共框，槽 2 开了覆盖（试用反馈里那张图的形状）") {
    const Json v1 = {{"templateDir", "StandardGap"},
                     {"datumRoi", global},
                     {"targetRoi", Json::array({5, 162, 15, 166})},
                     {"seamLeftRoi", Json::array({-4.5, 164, -1.5, 167})},
                     {"seamRightRoi", Json::array({1.5, 163, 4.5, 166})},
                     {"template1Id", "f1"},
                     {"template2Enabled", true},
                     {"template2Override", true},
                     {"template2DatumRoi", own},
                     {"template3Id", "f3"},
                     {"minScore", 30}};
    const Json v2 = migrate(v1);
    CHECK(v2["templateDir"] == "StandardGap");
    CHECK(v2["minScore"] == 30);
    CHECK(v2["template1DatumRoi"] == global);
    CHECK(v2["template1SeamRightRoi"] == Json::array({1.5, 163, 4.5, 166}));
    CHECK(v2["template2Enabled"] == true);
    CHECK(v2["template2DatumRoi"] == own);
    CHECK_FALSE(v2.contains("template2TargetRoi"));   // 覆盖框没写 = 默认（v1 也是这个值）
    CHECK(v2["template3DatumRoi"] == global);          // 关着的槽也抄一份，打开就能用
    CHECK_FALSE(v2.contains("template3Enabled"));
    for (const char* gone : {"datumRoi", "targetRoi", "seamLeftRoi", "seamRightRoi",
                             "template2Override", "template1Enabled"}) {
      CHECK_FALSE(v2.contains(gone));
    }
  }
  SUBCASE("v1 槽 1 关着：第一个启用的槽换到槽 1，启用槽的先后不变") {
    const Json v1 = {{"datumRoi", global},
                     {"template1Enabled", false},
                     {"template3Enabled", true},
                     {"template3Id", "c"},
                     {"template4Enabled", true},
                     {"template4Id", "d"},
                     {"template4Left", "d_l.pcd"}};
    const Json v2 = migrate(v1);
    CHECK(v2["template1Id"] == "c");
    CHECK(v2["template1Left"] == "f3_left.pcd");      // 旧槽 3 的默认值显式写出来
    CHECK(v2["template1DatumRoi"] == global);
    CHECK(v2["template3Id"] == "f1");                  // 原槽 1 挪到槽 3、关着
    CHECK(v2["template3Left"] == "left_template.pcd");
    CHECK_FALSE(v2.contains("template3Enabled"));
    CHECK(v2["template4Enabled"] == true);
    CHECK(v2["template4Id"] == "d");
    CHECK(v2["template4Left"] == "d_l.pcd");
  }
  SUBCASE("已经是 v2 写法的参数原样返回（opVersion 标旧了也不会把各槽的框抹掉）") {
    const Json v2 = {{"template1DatumRoi", own}, {"template2Enabled", true}, {"template2Id", "x"}};
    CHECK(migrate(v2) == v2);
  }
  SUBCASE("存于 v1.0.0 的节点经 validate 走迁移，迁移后的图校验干净、跑出同一个读数") {
    Scene scene("migrate");
    Json doc = importAs("StandardGap.yml:template", kSyntheticConfig, scene.root);
    const Reading fresh = run(doc, scene.root);
    REQUIRE(fresh.ok);
    Json& node = nodeOf(doc, "n_locate");
    Json& p = node["params"];
    // 改回 v1 的写法：公共四框 + 槽 1 不覆盖
    for (const auto& [v1Name, v2Name] :
         {std::pair<const char*, const char*>{"datumRoi", "template1DatumRoi"},
          {"targetRoi", "template1TargetRoi"},
          {"seamLeftRoi", "template1SeamLeftRoi"},
          {"seamRightRoi", "template1SeamRightRoi"}}) {
      p[v1Name] = p[v2Name];
      p.erase(v2Name);
    }
    p["template1Enabled"] = true;
    node["opVersion"] = "1.0.0";
    bool migrated = false;
    for (const Json& d : Json::parse(exec::validateGraphJson(doc.dump(), {}))) {
      if (d.value("kind", "") == "migration" && d.value("nodeId", "") == "n_locate") migrated = true;
      CHECK(d.value("severity", "") != "error");
    }
    CHECK(migrated);
    const Reading old = run(doc, scene.root);
    REQUIRE(old.ok);
    CHECK(old.flush == fresh.flush);
    CHECK(old.gap == fresh.gap);
  }
}

TEST_CASE("基准件的侧由框推出：base_side 与几何不符时两种图都按几何，meta 里记一笔") {
  Scene scene("side");
  std::string yaml = kSyntheticConfig;
  const std::string from = "base_side: left";
  yaml.replace(yaml.find(from), from.size(), "base_side: right");
  const Json blocks = importAs("StandardGap.yml:template", yaml, scene.root);
  const Json fine = importAs("StandardGap.yml:template:fine", yaml, scene.root);
  for (const Json* doc : {&blocks, &fine}) {
    REQUIRE((*doc)["meta"].contains("importNotes"));
    CHECK((*doc)["meta"]["importNotes"][0].get<std::string>().find("base_side=right") !=
          std::string::npos);
  }
  const Reading a = run(blocks, scene.root);
  const Reading b = run(fine, scene.root);
  REQUIRE(a.ok);
  CHECK(a.flush == b.flush);
  CHECK(a.gap == b.gap);
}

TEST_CASE("四框全 0 的模板候选两种图都跳过；候选之间基准件不同侧就不导入") {
  std::string yaml = kSyntheticConfig;
  const std::string from = "    - {id: f1, left: left.pcd, right: right.pcd}\n";
  std::string zero = yaml;
  zero.replace(zero.find(from), from.size(),
               "    - {id: f0, left: a.pcd, right: b.pcd, rois: {flush: {base_roi: [0, 0, 0, 0],"
               " ref_roi: [0, 0, 0, 0]}, gap: {left_roi: [0, 0, 0, 0], right_roi: [0, 0, 0, 0]}}}\n" +
                   from);
  const Json blocks = importAs("StandardGap.yml:template", zero, fs::path());
  const Json fine = importAs("StandardGap.yml:template:fine", zero, fs::path());
  CHECK(nodeOf(const_cast<Json&>(blocks), "n_locate")["params"]["template1Id"] == "f1");
  CHECK_FALSE(nodeOf(const_cast<Json&>(blocks), "n_locate")["params"].contains("template2Enabled"));
  for (const Json& n : fine["nodes"]) CHECK(n["id"] != "n_tpl_f0");
  CHECK(blocks["meta"]["importNotes"][0].get<std::string>().find("f0") != std::string::npos);

  std::string flipped = yaml;
  flipped.replace(flipped.find(from), from.size(),
                  from + "    - {id: f2, left: a.pcd, right: b.pcd, rois: {flush: {base_roi: [5, 162,"
                         " 15, 166], ref_roi: [-15, 163, -5, 167]}}}\n");
  const ImporterDesc* desc = packRegistry().findImporter("StandardGap.yml:template");
  std::string out;
  const Status s = desc->fn(flipped, fs::path(), out);
  CHECK_FALSE(s.ok);
  CHECK(s.message.find("不一致") != std::string::npos);
}

TEST_CASE("方向基准：写得成积木就是一个 gap.datum_direction，写不成就退回细粒度、经 split_* 混用") {
  auto with = [](const std::string& line) {
    std::string yaml = kSyntheticConfig;
    const std::size_t at = yaml.find("gap:\n");
    yaml.insert(at, line);
    return yaml;
  };
  auto edgeFrom = [](const Json& doc, const char* to, const char* port) -> std::string {
    for (const Json& e : doc["edges"]) {
      if (e["to"]["node"] == to && e["to"]["port"] == port) return e["from"]["node"];
    }
    return {};
  };
  {
    const Json doc = importAs("StandardGap.yml:template",
                              with("  base_direction: {datum: long_plane, mode: band}\n"), fs::path());
    CHECK(edgeFrom(doc, "n_line", "refLine") == "n_datum");
    const Json& p = nodeOf(const_cast<Json&>(doc), "n_datum")["params"];
    CHECK_FALSE(p.contains("role"));
    CHECK(nodeOf(const_cast<Json&>(doc), "n_line")["params"]["dirMode"] == "band");
    CHECK(doc["nodes"].size() == 11);
  }
  {
    // R4 那种：锚在缝的另一侧、高度锚在参考面 —— 就是 role=target
    const Json doc = importAs(
        "StandardGap.yml:template",
        with("  base_direction: {datum: long_plane, anchor: gap_right, height_anchor: flush_ref}\n"),
        fs::path());
    CHECK(nodeOf(const_cast<Json&>(doc), "n_datum")["params"]["role"] == "target");
  }
  {
    // 锚在左缝框、高度锚在右边的参考面：不是任何一个角色的「背离缝的那一侧」，积木写不出来
    const Json doc = importAs(
        "StandardGap.yml:template",
        with("  base_direction: {datum: long_plane, anchor: gap_left, height_anchor: flush_ref}\n"),
        fs::path());
    CHECK(edgeFrom(doc, "n_line", "refLine") == "n_fit_datum");
    CHECK(edgeFrom(doc, "n_datum_box", "anchor") == "n_split_rois");
    CHECK(edgeFrom(doc, "n_crop_datum", "cloud") == "n_split_scan");
    CHECK(errorsOf(doc).empty());
  }
}

TEST_CASE("随包的六个片段都注册了、自检干净，并进了 manifest（m8-plan L14）") {
  const Registry& r = ensureRegistry();
  std::set<std::string> ids;
  for (const SnippetDesc& s : r.snippets()) {
    CHECK(s.parseError.empty());
    ids.insert(s.id);
  }
  for (const char* want : {"gap.flush_line_end", "gap.flush_selected_point", "gap.gap_circles",
                           "gap.locate_model_template_fallback", "gap.backup_camera",
                           "gap.measure_skeleton"}) {
    CAPTURE(want);
    CHECK(ids.count(want) == 1);
  }
  CHECK(r.validate().empty());
  const Json manifest = Json::parse(r.toManifestJson());
  REQUIRE(manifest.contains("snippets"));
  bool skeleton = false;
  for (const Json& s : manifest["snippets"]) {
    if (s["id"] != "gap.measure_skeleton") continue;
    skeleton = true;
    CHECK(s["label"] == "测点骨架");
    CHECK(s["pack"].get<std::string>().rfind("gap", 0) == 0);
    CHECK(s["nodes"].size() == 8);
    CHECK(s["ports"]["inputs"].size() == 8);
  }
  CHECK(skeleton);

  // 坏片段在自检里报出来：算子不存在、端口不存在、对外输入其实已经接了边
  Registry bad;
  packs::gap::registerPackOps(bad);
  bad.addSnippet(parseSnippet(R"({"schemaVersion": 1, "id": "x.bad", "label": "坏",
    "nodes": [{"id": "a", "op": "gap.nope"}, {"id": "b", "op": "gap.flush"}],
    "edges": [{"from": {"node": "b", "port": "nope"}, "to": {"node": "b", "port": "baseLine"}}],
    "ports": {"inputs": [{"node": "b", "port": "baseLine"}]}})",
                              "bad.lyflow-snippet.json"));
  bad.addSnippet(parseSnippet("{ not json", "broken.lyflow-snippet.json"));
  std::string all;
  for (const std::string& p : bad.validate()) {
    if (p.find("snippet") != std::string::npos) all += p + "\n";
  }
  MESSAGE(all);
  CHECK(all.find("gap.nope") != std::string::npos);
  CHECK(all.find("'nope'") != std::string::npos);
  CHECK(all.find("已经接了边") != std::string::npos);
  CHECK(all.find("broken.lyflow-snippet.json") != std::string::npos);
}

TEST_CASE("roi 语义标记（m8-plan L15）：locate_template 的四个角色框画在所在模板槽的左右模板上") {
  const OperatorDesc* op = ensureRegistry().find("gap.locate_template");
  REQUIRE(op != nullptr);
  const Param* datum = findParam(*op, "template1DatumRoi");
  REQUIRE(datum != nullptr);
  CHECK(datum->semantic == "roi");
  CHECK(datum->roiBackdrop.dirParam == "templateDir");
  CHECK(datum->roiBackdrop.fileParams ==
        std::vector<std::string>{"template1Left", "template1Right"});
  // 切换条上的名字（L20）：「模板 k」+ 那个槽的 Id
  CHECK(datum->roiBackdrop.label == "模板 1");
  CHECK(datum->roiBackdrop.labelParam == "template1Id");
  const Param* slot3 = findParam(*op, "template3SeamLeftRoi");
  REQUIRE(slot3 != nullptr);
  CHECK(slot3->roiBackdrop.fileParams ==
        std::vector<std::string>{"template3Left", "template3Right"});
  CHECK(slot3->roiBackdrop.label == "模板 3");
  // 数据坐标系里的框：没有 backdrop
  const Param* overall = findParam(*op, "overallRoi");
  REQUIRE(overall != nullptr);
  CHECK(overall->semantic == "roi");
  CHECK_FALSE(overall->roiBackdrop.isSet());

  const Json manifest = Json::parse(ensureRegistry().toManifestJson());
  for (const Json& o : manifest["operators"]) {
    if (o["id"] != "gap.locate_template") continue;
    for (const Json& p : o["params"]) {
      if (p["name"] != "template2TargetRoi") continue;
      CHECK(p["semantic"] == "roi");
      CHECK(p["roiBackdrop"]["dir"] == "templateDir");
      CHECK(p["roiBackdrop"]["files"].size() == 2);
      CHECK(p["roiBackdrop"]["label"] == "模板 2");
      CHECK(p["roiBackdrop"]["labelParam"] == "template2Id");
    }
  }
}

TEST_CASE("read_scan（L18）：两个输入要么都给要么都不给；给了就用它们，与 source 无关") {
  const OperatorDesc* op = packRegistry().find("gap.read_scan");
  REQUIRE(op != nullptr);
  REQUIRE(op->validate != nullptr);
  ParamMap params;
  for (const Param& p : op->params) params[p.name] = p.def;
  const fs::path base;
  for (const char* source : {"inputs", "dir", "files"}) {
    CAPTURE(source);
    params["source"] = Value::text(source);
    CHECK(op->validate(ParamView(params, base), {"primary"}).size() == 1);
    CHECK(op->validate(ParamView(params, base), {"secondary"}).size() == 1);
    CHECK(op->validate(ParamView(params, base), {"primary", "secondary"}).empty());
    // 两个都没接：source=inputs 时可能由宿主注入，lyflow validate 看不见注入，不在这里报
    CHECK(op->validate(ParamView(params, base), {}).empty());
  }
}

namespace {

void collectEvent(const char* json, void* user) {
  static_cast<std::vector<Json>*>(user)->push_back(Json::parse(json));
}

/// 经 C ABI 跑一遍、把两片云注入 n_scan 的 primary / secondary（宿主的那条路）。
Reading runInjected(const Json& doc, const fs::path& baseDir, const lyflow::PointCloud& primary,
                    const lyflow::PointCloud& secondary, const std::string& runId) {
  lyflow_run_input in[2]{};
  in[0].node_id = "n_scan";
  in[0].port = "primary";
  in[0].kind = LYFLOW_INPUT_POINT_CLOUD;
  // rgb 也带上：gap 的强度在 R 上（模型定位的特征），宿主给的是整片云
  in[0].count = static_cast<std::uint32_t>(primary.pointCount());
  in[0].xyz = primary.xyz.data();
  in[0].rgb = primary.rgb.empty() ? nullptr : primary.rgb.data();
  in[1] = in[0];
  in[1].port = "secondary";
  in[1].count = static_cast<std::uint32_t>(secondary.pointCount());
  in[1].xyz = secondary.xyz.data();
  in[1].rgb = secondary.rgb.empty() ? nullptr : secondary.rgb.data();

  const std::string base = baseDir.u8string();
  lyflow_run_options opts{};
  opts.run_id = runId.c_str();
  opts.base_dir = base.c_str();
  opts.no_reuse = 1;
  opts.inputs = in;
  opts.input_count = 2;
  std::vector<Json> events;
  const std::string graph = doc.dump();
  lyflow_run* handle = lyflow_run_start(graph.c_str(), &opts, &collectEvent, &events);
  REQUIRE(handle != nullptr);
  lyflow_run_join(handle);

  Reading r;
  for (const Json& e : events) {
    if (e.value("kind", "") == "run_finished") r.status = e.value("status", "");
    if (e.value("kind", "") == "node_state" && e.value("state", "") == "error") MESSAGE(e.dump());
    // 注入落在 read_scan 本身：它是真跑的（不是 provided），前面没有别的读盘节点
    if (e.value("kind", "") == "node_state" && e.value("nodeId", "") == "n_scan" &&
        e.value("state", "") == "done") {
      CHECK_FALSE(e["stats"].contains("provided"));
    }
  }
  char* raw = lyflow_run_outputs(runId.c_str());
  const Json outputs = Json::parse(raw);
  lyflow_string_free(raw);
  lyflow_run_free(handle);
  const Json& flush = outputs["flush"]["value"];
  const Json& gap = outputs["gap"]["value"];
  if (flush.value("ok", false) && gap.value("ok", false)) {
    r.flush = flush["value"].get<double>();
    r.gap = gap["value"].get<double>();
    r.ok = true;
  }
  return r;
}

}  // namespace

TEST_CASE("宿主把两片云直接注入 read_scan 的 primary / secondary：与从目录读取逐位相同（L18）") {
  Scene scene("inject");
  const Json doc = importAs("StandardGap.yml:template", kSyntheticConfig, scene.root);
  const Reading fromDir = run(doc, scene.root);
  REQUIRE(fromDir.ok);

  // 宿主手里的两片云：与 read_scan 自己读到的是同一份（传感器帧、NaN 槽原样留着）
  const Json loader = test::makeGraph(
      {test::N{"load", "gap.load_profile_pair",
               Json{{"dir", scene.root.u8string()}, {"dropNonFinite", false}}}},
      {});
  test::Session ls(loader, scene.root);
  REQUIRE(ls.wait().runStatus() == "ok");
  Data primary, secondary;
  REQUIRE(exec::ResultStore::instance().get(ls.runId(), "load", "primary", primary));
  REQUIRE(exec::ResultStore::instance().get(ls.runId(), "load", "secondary", secondary));
  const lyflow::PointCloud& p = *primary.asCloud();
  const lyflow::PointCloud& s = *secondary.asCloud();

  SUBCASE("图原样（source=dir、目录也在）：注入的两片云压过目录") {
    const Reading injected = runInjected(doc, scene.root, p, s, "inject-dir");
    REQUIRE(injected.ok);
    CHECK(injected.flush == fromDir.flush);
    CHECK(injected.gap == fromDir.gap);
  }
  SUBCASE("source=inputs、不给目录：lyflow validate 照样干净，跑出来一样") {
    Json bare = doc;
    Json& params = nodeOf(bare, "n_scan")["params"];
    params["source"] = "inputs";
    params.erase("dir");
    CHECK(errorsOf(bare).empty());
    const Reading injected = runInjected(bare, scene.root, p, s, "inject-inputs");
    REQUIRE(injected.ok);
    CHECK(injected.flush == fromDir.flush);
    CHECK(injected.gap == fromDir.gap);
    // 不注入就跑：执行期报 missing_input，不会悄悄去读一个空目录
    test::Session none(bare, scene.root);
    test::RunLog& log = none.wait();
    CHECK(log.nodeEvent("n_scan", "error")["errors"][0]["code"] == "missing_input");
  }
}

// ------------------------------------------------ gap.result_bundle v1 → v2（ADR-0025）

namespace {

/// 把迁移诊断的 edits 按 applyMigrations 的语义写回一份 doc（前端与 lyflow migrate 各有一份同样的实现）。
Json applyMigration(Json doc, const Json& m) {
  for (Json& n : doc["nodes"]) {
    if (n["id"] != m["nodeId"]) continue;
    n["op"] = m["op"];
    n["opVersion"] = m["opVersion"];
    n["params"] = m["params"];
  }
  if (!m.contains("edits")) return doc;
  const Json& edits = m["edits"];
  Json kept = Json::array();
  for (const Json& e : doc["edges"]) {
    bool drop = false;
    for (const Json& r : edits["removeEdges"]) {
      drop = drop || (e["from"] == r["from"] && e["to"] == r["to"]);
    }
    if (!drop) kept.push_back(e);
  }
  for (const Json& n : edits["addNodes"]) {
    doc["nodes"].push_back(
        {{"id", n["id"]}, {"op", n["op"]}, {"opVersion", n["opVersion"]}, {"params", n["params"]}});
  }
  for (const Json& e : edits["addEdges"]) kept.push_back(e);
  doc["edges"] = std::move(kept);
  return doc;
}

std::vector<Json> migrationsOf(const Json& doc) {
  std::vector<Json> out;
  for (const Json& d : Json::parse(exec::validateGraphJson(doc.dump(), {}))) {
    if (d.value("kind", "") == "migration") out.push_back(d);
  }
  return out;
}

/// m8a 之前的细粒度图：make_roi_set / make_scan_pair 不存在，七根散线直接接到 result_bundle。
/// 从 --fine 导出的图反推：两个 make_* 节点的每条入边改接到 bundle 上对应的 v1 端口。
Json asV1(Json doc) {
  const std::map<std::string, std::string> roiPorts = {
      {"datum", "roiFlushBase"}, {"target", "roiFlushRef"},   {"seamLeft", "roiGapLeft"},
      {"seamRight", "roiGapRight"}, {"alignment", "alignment"}, {"overall", "roiOverall"},
      {"cropStatus", "cropStatus"}};
  const std::map<std::string, std::string> scanPorts = {
      {"primary", "cloudPrimary"}, {"secondary", "cloudSecondary"}, {"merged", "cloudMerged"}};
  Json edges = Json::array();
  for (const Json& e : doc["edges"]) {
    const std::string to = e["to"]["node"];
    const std::string port = e["to"]["port"];
    if (to == "n_bundle" && (port == "rois" || port == "scan")) continue;
    const auto* table = to == "n_roi_set" ? &roiPorts : to == "n_scan_set" ? &scanPorts : nullptr;
    if (!table) {
      edges.push_back(e);
      continue;
    }
    Json moved = e;
    moved["to"] = {{"node", "n_bundle"}, {"port", table->at(port)}};
    edges.push_back(moved);
  }
  Json nodes = Json::array();
  for (Json n : doc["nodes"]) {
    if (n["id"] == "n_roi_set" || n["id"] == "n_scan_set") continue;
    if (n["id"] == "n_bundle") n["opVersion"] = "1.1.0";
    nodes.push_back(n);
  }
  doc["nodes"] = std::move(nodes);
  doc["edges"] = std::move(edges);
  return doc;
}

Json bundleOf(const Json& doc, const fs::path& baseDir) {
  test::Session s(doc, baseDir);
  test::RunLog& log = s.wait();
  REQUIRE(log.runStatus() == "ok");
  Data bundle;
  REQUIRE(exec::ResultStore::instance().get(s.runId(), "n_bundle", "bundle", bundle));
  return bundle.asRecord()->data;
}

}  // namespace

TEST_CASE("result_bundle v1 → v2：七个散端口收进新插的 make_scan_pair / make_roi_set，汇总逐字段相同") {
  Scene scene("bundle-v1");
  const Json fine = importAs("StandardGap.yml:template:fine", kSyntheticConfig, scene.root);
  const Json v1 = asV1(fine);

  const std::vector<Json> migrations = migrationsOf(v1);
  REQUIRE(migrations.size() == 1);
  const Json& m = migrations[0];
  CHECK(m["nodeId"] == "n_bundle");
  CHECK(m["opVersion"] == "2.0.0");
  REQUIRE(m.contains("edits"));
  // alignment / roiOverall 在 v2 仍是 bundle 的端口，那两条边原样留着
  CHECK(m["edits"]["removeEdges"].size() == 7);
  REQUIRE(m["edits"]["addNodes"].size() == 2);
  std::set<std::string> added;
  for (const Json& n : m["edits"]["addNodes"]) added.insert(n["op"].get<std::string>());
  CHECK(added == std::set<std::string>{"gap.make_roi_set", "gap.make_scan_pair"});
  CHECK(m["edits"]["addEdges"].size() == 9);
  CHECK(errorsOf(v1).empty());

  // 老图当场就能跑（执行器在内存里用迁移后的拓扑），汇总与 m8a 的细粒度图逐字段相同
  const Json expected = bundleOf(fine, scene.root);
  CHECK(bundleOf(v1, scene.root) == expected);

  // 写回之后再校验：不再有迁移诊断，汇总仍相同
  const Json written = applyMigration(v1, m);
  CHECK(migrationsOf(written).empty());
  CHECK(errorsOf(written).empty());
  CHECK(bundleOf(written, scene.root) == expected);
}

TEST_CASE("result_bundle v1 → v2：凑不齐一个 Bundle 的散端口只能删边，notes 写明丢了哪几项") {
  Scene scene("bundle-partial");
  Json doc = asV1(importAs("StandardGap.yml:template:fine", kSyntheticConfig, scene.root));
  // 只留 cloudPrimary / cloudSecondary 与 roiFlushBase / roiFlushRef（KUN10 点 4 的接法），并去掉 opVersion
  Json edges = Json::array();
  for (const Json& e : doc["edges"]) {
    const std::string port = e["to"]["port"];
    if (e["to"]["node"] == "n_bundle" &&
        (port == "cloudMerged" || port == "roiGapLeft" || port == "roiGapRight")) {
      continue;
    }
    edges.push_back(e);
  }
  doc["edges"] = std::move(edges);
  nodeOf(doc, "n_bundle").erase("opVersion");

  const std::vector<Json> migrations = migrationsOf(doc);
  REQUIRE(migrations.size() == 1);
  const Json& m = migrations[0];
  CHECK(m["edits"]["removeEdges"].size() == 4);
  CHECK(m["edits"]["addNodes"].empty());
  CHECK(m["edits"]["addEdges"].empty());
  std::string notes;
  for (const Json& n : m["notes"]) notes += n.get<std::string>() + "\n";
  MESSAGE(notes);
  CHECK(notes.find("没有记 opVersion") != std::string::npos);
  CHECK(notes.find("cloudPrimary、cloudSecondary 已删，没有等价接法") != std::string::npos);
  CHECK(notes.find("roiFlushBase、roiFlushRef 已删，没有等价接法") != std::string::npos);
  CHECK(errorsOf(doc).empty());

  const Json written = applyMigration(doc, m);
  CHECK(migrationsOf(written).empty());
  CHECK(errorsOf(written).empty());
}

TEST_CASE("没打 opVersion 的 v2 接法不算迁移：迁移函数对新写法是幂等的") {
  Scene scene("bundle-v2-unversioned");
  Json doc = importAs("StandardGap.yml:template:fine", kSyntheticConfig, scene.root);
  for (Json& n : doc["nodes"]) n.erase("opVersion");
  CHECK(migrationsOf(doc).empty());
  CHECK(errorsOf(doc).empty());
}
