// StandardGap.yml → 测量图（ADR-0017）。默认产出积木图（m8-plan L12）：人打开导入的图，
// 看到的就是他自己也拼得出来的那十来个节点；:fine 产出细粒度图，每一步一个节点，想精细控制时用。
// 两种图的算子共用同一份实现（L6），同一帧上 flush / gap 逐帧相同 —— 这是 M8a 验收 1。
#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <yaml-cpp/yaml.h>

#include "gap_fine.h"

namespace lyflow::packs::gap {
namespace {

namespace fs = std::filesystem;

constexpr const char* kSafe =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_";

std::string safeId(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text) out.push_back(std::string(kSafe).find(c) == std::string::npos ? '_' : c);
  return out;
}

Status badInput(const std::string& message) {
  return Status::Error(Phase::Validate, "bad_input", message);
}

Status badParam(const std::string& message) {
  return Status::Error(Phase::Validate, "bad_param", message);
}

YAML::Node child(const YAML::Node& node, const char* key) {
  if (!node || !node.IsMap()) return YAML::Node(YAML::NodeType::Undefined);
  return node[key];
}

bool present(const YAML::Node& node) { return node && !node.IsNull(); }

double numberOr(const YAML::Node& node, const char* key, double def) {
  const YAML::Node v = child(node, key);
  return present(v) ? v.as<double>(def) : def;
}

std::int64_t intOr(const YAML::Node& node, const char* key, std::int64_t def) {
  const YAML::Node v = child(node, key);
  return present(v) ? v.as<std::int64_t>(def) : def;
}

bool boolOr(const YAML::Node& node, const char* key, bool def) {
  const YAML::Node v = child(node, key);
  return present(v) ? v.as<bool>(def) : def;
}

std::string textOr(const YAML::Node& node, const char* key, const std::string& def) {
  const YAML::Node v = child(node, key);
  return present(v) ? v.as<std::string>(def) : def;
}

using Roi = std::array<double, 4>;

bool roiList(const YAML::Node& node, Roi* out, std::string* problem) {
  *out = {0.0, 0.0, 0.0, 0.0};
  if (!present(node)) return true;
  if (!node.IsSequence() || node.size() != 4) {
    *problem = "ROI 必须是四个数";
    return false;
  }
  for (std::size_t i = 0; i < 4; ++i) (*out)[i] = node[i].as<double>(0.0);
  return true;
}

bool allZero(const Roi& r) { return r[0] == 0 && r[1] == 0 && r[2] == 0 && r[3] == 0; }

/// 节点存于哪个算子版本：当前注册表里那个算子的 version，与编辑器新建节点写 op.version 一致。
/// 以后算子升主版本时，导入的图也能按 opVersion 自动迁移（ADR-0008；M8c 取舍 1 暴露了缺它的后果）。
/// 导入总在注册完成之后发生，此时 ensureRegistry() 里已经有全部算子包（含别的包的 flow / filter 算子）。
std::string opVersionOf(const char* op) {
  const OperatorDesc* desc = ensureRegistry().find(op);
  return desc ? desc->version : std::string();
}

class Builder {
 public:
  std::string node(const std::string& id, const char* op, nlohmann::json params, int column,
                   int row, const std::string& title) {
    nlohmann::json entry;
    entry["id"] = id;
    entry["op"] = op;
    if (const std::string version = opVersionOf(op); !version.empty()) entry["opVersion"] = version;
    nlohmann::json ui;
    ui["position"] = nlohmann::json{{"x", column * 280}, {"y", row * 130}};
    if (!title.empty()) ui["title"] = title;
    entry["ui"] = std::move(ui);
    if (!params.is_null() && !params.empty()) entry["params"] = std::move(params);
    nodes.push_back(std::move(entry));
    return id;
  }

  void edge(const std::string& src, const char* srcPort, const std::string& dst,
            const char* dstPort) {
    char id[8];
    std::snprintf(id, sizeof(id), "e%03d", static_cast<int>(edges.size()));
    edges.push_back({{"id", id},
                     {"from", {{"node", src}, {"port", srcPort}}},
                     {"to", {{"node", dst}, {"port", dstPort}}}});
  }

  nlohmann::json nodes = nlohmann::json::array();
  nlohmann::json edges = nlohmann::json::array();
};

/// 一个输出端口的引用：(节点, 端口)。空节点 = 没有。
struct Ref {
  std::string node;
  const char* port = "";
  bool empty() const { return node.empty(); }
};

struct Candidate {
  std::string id;
  std::string left;
  std::string right;
  // 配置的 base / ref / left / right，即角色 datum / target / seamLeft / seamRight
  Roi flushBase{};
  Roi flushRef{};
  Roi gapLeft{};
  Roi gapRight{};
};

enum class Mode { Template, Model, Auto };

// 优先用文档自带的 model_roi：宿主把自己那份 setting.yml 的这一段并进来源文本，
// 就不必在 baseDir 边上放一个 setting.yml。真实的 StandardGap.yml 没有这个键，行为不变。
YAML::Node modelRoiSetting(const fs::path& baseDir, const YAML::Node& cfg) {
  const YAML::Node inline_ = child(cfg, "model_roi");
  if (present(inline_)) return inline_;
  for (const fs::path& dir : {baseDir, baseDir.parent_path()}) {
    if (dir.empty()) continue;
    const fs::path candidate = dir / "setting.yml";
    std::error_code ec;
    if (!fs::is_regular_file(candidate, ec)) continue;
    try {
      const YAML::Node root = YAML::LoadFile(candidate.string());
      const YAML::Node section = child(root, "model_roi");
      if (present(section)) return section;
    } catch (const std::exception&) {
      continue;
    }
  }
  return YAML::Node(YAML::NodeType::Undefined);
}

bool uniqueOnnx(const fs::path& baseDir, fs::path* out) {
  std::error_code ec;
  std::vector<fs::path> hits;
  for (const auto& entry : fs::directory_iterator(baseDir, ec)) {
    if (entry.path().extension() == ".onnx") hits.push_back(entry.path());
  }
  if (hits.size() != 1) return false;
  *out = hits.front();
  return true;
}

/// 一份配置解析完的样子。两种图都从它出发，所以「同一份配置 → 同一组参数」只写一遍。
struct Config {
  // YAML::Node 的赋值是「把值写进那个节点」而不是换绑，对缺席的键（zombie 节点）会抛 ——
  // 所以一律在构造时拷一次，之后只读。
  explicit Config(const YAML::Node& cfg)
      : common(child(cfg, "common_settings")),
        flush(child(cfg, "flush")),
        gap(child(cfg, "gap")),
        align(child(cfg, "align")),
        icp(child(align, "ICP")),
        robustness(child(align, "robustness")),
        trust(child(robustness, "trust_region")),
        filter(child(common, "filter")),
        guard(child(common, "camera_guard")),
        roll(child(common, "roll_anchored_crop")),
        band(child(gap, "center_band")),
        radius(child(gap, "radius")),
        weak(child(gap, "weak_fit")) {}

  const YAML::Node common, flush, gap, align, icp, robustness, trust, filter, guard, roll, band,
      radius, weak;
  std::string templateDir;
  std::string sampleId;
  bool useModel = false;
  bool withFallback = false;
  std::string modelPath;
  std::string refType;
  std::vector<Candidate> candidates;
  Roi top[4]{};  // flushBase / flushRef / gapLeft / gapRight
  /// 基准件在缝的哪一侧：由框推出（m8-plan L7），flush.base_side 不再参与。
  bool datumRight = false;
  bool usingRemoval = false;
  std::string usingCamera;
  std::int64_t minScore = 60;
  double lineDist = 0.1;
  std::int64_t segmentPoints = 0;
  bool wantDatum = false;
  std::string datumMode;
  std::string anchorKind;
  std::string heightKind;
  std::string datumWindowSide;
  bool wantBand = false;
  std::string bandRef;
  std::string definition;
  std::vector<std::string> notes;
};

Status parseConfig(const YAML::Node& cfg, const fs::path& baseDir, Mode mode, Config& c) {
  c.templateDir = textOr(cfg, "template_dir", "StandardGap");
  c.sampleId = baseDir.filename().string();

  const std::string segMode = textOr(c.common, "seg_mode", "ROI");
  if (segMode != "ROI") return badInput("只支持 seg_mode: ROI，这份配置是 '" + segMode + "'");
  if (!boolOr(c.align, "align_cloud", false)) {
    return badInput("align_cloud 为 false 时是 label 模式，不产生测量结果");
  }

  const std::string baseType = textOr(c.flush, "base_type", "fit line");
  c.refType = textOr(c.flush, "ref_type", "line end");
  const std::string leftType = textOr(c.gap, "left_type", "circle");
  const std::string rightType = textOr(c.gap, "right_type", "circle");
  const std::pair<const char*, const std::string&> checked[4] = {
      {"flush.base_type", baseType},
      {"flush.ref_type", c.refType},
      {"gap.left_type", leftType},
      {"gap.right_type", rightType},
  };
  for (const auto& item : checked) {
    if (item.second == "2-points line" || item.second == "circle tangent") {
      return badInput(std::string(item.first) + " = '" + item.second +
                      "' 不在这条路径的支持范围内（计划 §6）");
    }
  }
  if (baseType.find("line") == std::string::npos) {
    return badInput("flush.base_type 目前只支持直线类，拿到 '" + baseType + "'");
  }
  if (c.refType != "line end" && c.refType != "selected point" && c.refType != "nearest point") {
    return badInput(
        "flush.ref_type 目前只支持 line end / selected point / nearest point，拿到 '" + c.refType +
        "'");
  }
  if (leftType != "circle" || rightType != "circle") return badInput("gap 两侧目前只支持 circle");

  const YAML::Node setting = modelRoiSetting(baseDir, cfg);
  if (mode == Mode::Auto) {
    mode = present(setting) && boolOr(setting, "enabled", false) ? Mode::Model : Mode::Template;
  }
  if (mode == Mode::Model) {
    c.modelPath = textOr(setting, "model_path", "");
    if (c.modelPath.empty()) {
      fs::path found;
      if (uniqueOnnx(baseDir, &found)) c.modelPath = found.string();
    }
    if (c.modelPath.empty()) {
      return badParam(
          "模型模式要一个 ONNX：setting.yml 的 model_roi.model_path 没有，"
          "baseDir 下也没有唯一的 *.onnx");
    }
  }
  c.useModel = mode == Mode::Model;
  c.withFallback = c.useModel && present(setting) && boolOr(setting, "enabled", false);

  std::string problem;
  if (!roiList(child(c.flush, "base_roi"), &c.top[0], &problem) ||
      !roiList(child(c.flush, "ref_roi"), &c.top[1], &problem) ||
      !roiList(child(c.gap, "left_roi"), &c.top[2], &problem) ||
      !roiList(child(c.gap, "right_roi"), &c.top[3], &problem)) {
    return badInput(problem);
  }

  const YAML::Node listed = child(c.align, "template_candidates");
  if (present(listed) && listed.IsSequence() && listed.size() > 0) {
    for (std::size_t i = 0; i < listed.size(); ++i) {
      const YAML::Node item = listed[i];
      Candidate cand;
      cand.id = textOr(item, "id", "t" + std::to_string(i));
      cand.left = textOr(item, "left", "left_template.pcd");
      cand.right = textOr(item, "right", "right_template.pcd");
      const YAML::Node rois = child(item, "rois");
      const YAML::Node croiFlush = child(rois, "flush");
      const YAML::Node croiGap = child(rois, "gap");
      const YAML::Node base = child(croiFlush, "base_roi");
      const YAML::Node ref = child(croiFlush, "ref_roi");
      const YAML::Node left = child(croiGap, "left_roi");
      const YAML::Node right = child(croiGap, "right_roi");
      if (!roiList(base ? base : child(c.flush, "base_roi"), &cand.flushBase, &problem) ||
          !roiList(ref ? ref : child(c.flush, "ref_roi"), &cand.flushRef, &problem) ||
          !roiList(left ? left : child(c.gap, "left_roi"), &cand.gapLeft, &problem) ||
          !roiList(right ? right : child(c.gap, "right_roi"), &cand.gapRight, &problem)) {
        return badInput(problem);
      }
      // 四框全是 0 = 这个候选没配框（天幕 L4 的 f1）。选中它的那一帧只会在裁剪处 roi_empty，
      // 两种图都不给它建槽，并在 meta.importNotes 里记一笔。
      if (allZero(cand.flushBase) && allZero(cand.flushRef) && allZero(cand.gapLeft) &&
          allZero(cand.gapRight)) {
        c.notes.push_back("模板候选 " + cand.id + " 的四个框全是 0（没配），导入时跳过");
        continue;
      }
      c.candidates.push_back(std::move(cand));
    }
    if (c.candidates.empty()) return badInput("模板候选的框全是 0，没有一个能用");
  } else {
    Candidate cand;
    cand.id = "primary";
    cand.left = "left_template.pcd";
    cand.right = "right_template.pcd";
    cand.flushBase = c.top[0];
    cand.flushRef = c.top[1];
    cand.gapLeft = c.top[2];
    cand.gapRight = c.top[3];
    c.candidates.push_back(std::move(cand));
  }
  const bool needTemplates = !c.useModel || c.withFallback;
  if (needTemplates && c.candidates.size() > 4) {
    return badInput("模板最多四个槽（gap.select_alignment 的上限），这份配置有 " +
                    std::to_string(c.candidates.size()) + " 个");
  }

  // 基准件在缝的哪一侧：由模板坐标系里的框推出（L7）。各候选必须一致 ——
  // 不一致的话「哪个框是基准」本身就是歧义，积木图与细粒度图都没法给出同一个答案。
  for (std::size_t i = 0; i < c.candidates.size(); ++i) {
    const Candidate& cand = c.candidates[i];
    const bool right = datumOnRight(cand.flushBase, cand.gapLeft, cand.gapRight);
    if (i == 0) {
      c.datumRight = right;
    } else if (right != c.datumRight) {
      return badInput("模板候选 " + cand.id + " 的基准面在缝的另一侧，与 " +
                      c.candidates.front().id + " 不一致");
    }
  }
  const std::string baseSide = textOr(c.flush, "base_side", "left");
  if (present(child(c.flush, "base_side")) && (baseSide == "right") != c.datumRight) {
    c.notes.push_back("flush.base_side=" + baseSide + " 与框的几何不符（基准面在缝的" +
                      (c.datumRight ? "右" : "左") + "侧），按几何取（m8-plan L7）");
  }

  c.usingRemoval = boolOr(c.filter, "using_removal", false);
  c.usingCamera = textOr(c.common, "using_camera", "Both");
  c.minScore = intOr(c.icp, "min_score", 60);
  c.lineDist = numberOr(c.common, "line_fit_distance", 0.1);
  c.segmentPoints = intOr(c.flush, "segment_points", 0);

  // 方向基准（可选）：基准面很窄时它自己拟出来的方向不可信，改锚到旁边那张长面上。
  const YAML::Node baseDirCfg = child(c.flush, "base_direction");
  const std::string datumKind = textOr(baseDirCfg, "datum", "off");
  c.wantDatum = datumKind != "off";
  if (c.wantDatum && datumKind != "long_plane") {
    return badInput("flush.base_direction.datum 只能是 off 或 long_plane，这份写的是 '" +
                    datumKind + "'");
  }
  c.datumMode = textOr(baseDirCfg, "mode", "fixed");
  if (c.wantDatum && c.datumMode != "fixed" && c.datumMode != "band") {
    return badInput("flush.base_direction.mode 只能是 fixed 或 band，这份写的是 '" + c.datumMode +
                    "'");
  }
  // 默认：x 锚挂在基准面那一侧的缝框上，y 锚在基准面框上，窗朝背离缝的那一侧推。
  // 基准面落在长边时（R4），要保护的那条线在缝的另一侧，所以锚都可以显式写。
  c.anchorKind = textOr(baseDirCfg, "anchor", c.datumRight ? "gap_right" : "gap_left");
  c.heightKind = textOr(baseDirCfg, "height_anchor", "flush_base");
  if (c.wantDatum && c.anchorKind != "gap_left" && c.anchorKind != "gap_right") {
    return badInput("flush.base_direction.anchor 只能是 gap_left 或 gap_right，这份写的是 '" +
                    c.anchorKind + "'");
  }
  if (c.wantDatum && c.heightKind != "flush_base" && c.heightKind != "flush_ref") {
    return badInput(
        "flush.base_direction.height_anchor 只能是 flush_base 或 flush_ref，这份写的是 '" +
        c.heightKind + "'");
  }
  c.datumWindowSide = textOr(baseDirCfg, "side", c.anchorKind == "gap_left" ? "left" : "right");

  const double bandTol[2] = {numberOr(c.band, "left_tolerance", 0.0),
                             numberOr(c.band, "right_tolerance", 0.0)};
  c.wantBand = bandTol[0] > 0 || bandTol[1] > 0;
  c.bandRef = textOr(c.band, "reference", "flush_ref");
  if (c.wantBand && c.bandRef != "flush_ref" && c.bandRef != "flush_base") {
    return badInput("gap.center_band.reference 只能是 flush_ref 或 flush_base，这份写的是 '" +
                    c.bandRef + "'");
  }
  if (c.wantBand && c.bandRef == "flush_ref" && c.refType != "line end") {
    return badInput(
        "gap.center_band.reference 是 flush_ref，但 flush.ref_type 不是 'line end'，"
        "图里没有参考线可接");
  }
  for (int i = 0; i < 2; ++i) {
    const char* key = i == 0 ? "left_mode" : "right_mode";
    const std::string m = textOr(c.band, key, "always");
    if (bandTol[i] > 0 && m != "always" && m != "guard") {
      return badInput(std::string("gap.center_band.") + key + " 只能是 always 或 guard，这份写的是 '" +
                      m + "'");
    }
  }
  c.definition = textOr(c.gap, "definition", "B");
  return Status::Ok();
}

// ------------------------------------------------------------ 两种图共用的参数

nlohmann::json icpParams(const Config& c) {
  nlohmann::json p;
  p["maxMatchingDist"] = numberOr(c.icp, "max_matching_dist", 1.0);
  p["maxFitnessDist"] = numberOr(c.icp, "max_fitness_dist", 10.0);
  p["maxIterations"] = intOr(c.icp, "max_iteration_num", 1000);
  p["normalKnn"] = intOr(c.icp, "num_neighbor", 10);
  p["minScore"] = c.minScore;
  p["bidirection"] = boolOr(c.icp, "bidirection_align", false);
  p["globalCoarse"] = boolOr(c.robustness, "global_coarse", true);
  p["successGuide"] = boolOr(c.align, "success_guide", false);
  p["segRoi"] = true;
  p["trustTranslation"] = numberOr(c.trust, "max_translation_mm", 3.0);
  p["trustRotation"] = numberOr(c.trust, "max_rotation_deg", 2.0);
  p["degenerateRatio"] = numberOr(c.robustness, "degenerate_ratio", 1.0e-3);
  return p;
}

Roi overallRoiOf(const Config& c) {
  Roi overall{};
  std::string problem;
  roiList(child(c.common, "overall_roi"), &overall, &problem);
  return overall;
}

nlohmann::json circleParams(const Config& c) {
  bool selectClosest;
  const bool cameraFallback = boolOr(c.gap, "camera_separated_circle_fallback", true);
  if (present(child(c.gap, "camera_separated_select_closest_nominal"))) {
    selectClosest = boolOr(c.gap, "camera_separated_select_closest_nominal", true);
  } else if (present(child(c.gap, "camera_separated_select_closest_radius"))) {
    selectClosest = boolOr(c.gap, "camera_separated_select_closest_radius", true);
  } else {
    selectClosest =
        cameraFallback && textOr(c.gap, "camera_separated_preferred_camera", "Both") == "Both";
  }
  nlohmann::json p;
  p["distThresh"] = numberOr(c.common, "circle_fit_distance", 0.03);
  p["retryDistance"] = numberOr(c.gap, "circle_fit_retry_distance", 0.0);
  p["nominal"] = numberOr(child(c.gap, "tolerances"), "nominal", 0.0);
  p["leftRadiusFixed"] = boolOr(c.radius, "fixed_left_circle_radius", false);
  p["leftRadiusValue"] = numberOr(c.radius, "left_circle_radius", 0.0);
  p["leftRadiusMin"] = numberOr(c.radius, "left_circle_radius_min", 0.0);
  p["leftRadiusMax"] = numberOr(c.radius, "left_circle_radius_max", 0.0);
  p["rightRadiusFixed"] = boolOr(c.radius, "fixed_right_circle_radius", false);
  p["rightRadiusValue"] = numberOr(c.radius, "right_circle_radius", 0.0);
  p["rightRadiusMin"] = numberOr(c.radius, "right_circle_radius_min", 0.0);
  p["rightRadiusMax"] = numberOr(c.radius, "right_circle_radius_max", 0.0);
  p["cameraFallback"] = cameraFallback;
  p["selectClosestNominal"] = selectClosest;
  p["preferredCamera"] = textOr(c.gap, "camera_separated_preferred_camera", "Both");
  p["leftCamera"] = textOr(c.gap, "left_circle_camera", "Both");
  p["rightCamera"] = textOr(c.gap, "right_circle_camera", "Both");
  // 弱拟合的地板。钉了单相机时先退回合并云重拟，两边都弱才算这一侧没拟出来。
  p["leftMinInliers"] = intOr(c.weak, "left_min_inliers", 0);
  p["rightMinInliers"] = intOr(c.weak, "right_min_inliers", 0);
  p["leftMinArcDeg"] = numberOr(c.weak, "left_min_arc_deg", 0.0);
  p["rightMinArcDeg"] = numberOr(c.weak, "right_min_arc_deg", 0.0);
  p["leftCenterAbove"] = numberOr(c.band, "left_above", 0.0);
  p["leftCenterTol"] = numberOr(c.band, "left_tolerance", 0.0);
  p["rightCenterAbove"] = numberOr(c.band, "right_above", 0.0);
  p["rightCenterTol"] = numberOr(c.band, "right_tolerance", 0.0);
  p["leftCenterMode"] = textOr(c.band, "left_mode", "always");
  p["rightCenterMode"] = textOr(c.band, "right_mode", "always");
  return p;
}

nlohmann::json rollParams(const Config& c) {
  nlohmann::json p;
  p["enabled"] = boolOr(c.roll, "enabled", false);
  p["halfWidth"] = numberOr(c.roll, "half_width_mm", 35.0);
  p["halfHeight"] = numberOr(c.roll, "half_height_mm", 20.0);
  p["maxRollBoxHeight"] = numberOr(c.roll, "max_roll_box_height_mm", 8.0);
  p["minPointsKept"] = intOr(c.roll, "min_points_kept", 50);
  p["usingCamera"] = c.usingCamera;
  return p;
}

nlohmann::json guardParams(const Config& c) {
  nlohmann::json p;
  p["onDisagree"] = textOr(c.guard, "on_disagree", "record");
  p["maxDeltaMm"] = numberOr(c.guard, "max_delta_mm", 0.5);
  p["sampleStep"] = numberOr(c.guard, "sample_step_mm", 0.5);
  p["halfWindow"] = numberOr(c.guard, "half_window_mm", 0.25);
  p["minSamples"] = intOr(c.guard, "min_samples", 20);
  return p;
}

const char* refMethod(const std::string& refType) {
  if (refType == "line end") return "line_end";
  if (refType == "selected point") return "selected_point";
  return "nearest_point";
}

/// 两种图共用的收尾：判定、结果汇总的参数、图输出、顶层参数。
struct Tail {
  std::string flushNode, gapNode, circlesNode, bundleNode;
  std::vector<std::string> modelPathBinds;
};

void judges(Builder& g, const Config& c, const std::string& flushNode, const std::string& gapNode,
            int column) {
  const YAML::Node sections[2] = {c.flush, c.gap};
  const std::string sources[2] = {flushNode, gapNode};
  static const char* kJudgeTitles[2] = {"判定段差", "判定间隙"};
  for (int row = 0; row < 2; ++row) {
    const YAML::Node tol = child(sections[row], "tolerances");
    nlohmann::json p;
    p["nominal"] = numberOr(tol, "nominal", 0.0);
    p["upper"] = numberOr(tol, "up_deviation", 0.0);
    p["lower"] = numberOr(tol, "low_deviation", 0.0);
    g.node("n_judge_" + std::to_string(row), "gap.judge", p, column, row * 2, kJudgeTitles[row]);
    g.edge(sources[row], "value", "n_judge_" + std::to_string(row), "value");
  }
}

nlohmann::json bundleParams(const Config& c) {
  nlohmann::json p;
  // 模板路径上配了跟随裁剪窗却用不上，原算法记 skipped:no_model_roi。
  if (!c.useModel && boolOr(c.roll, "enabled", false)) p["cropStatus"] = "skipped:no_model_roi";
  return p;
}

std::string finish(Builder& g, const Config& c, const Tail& t, bool fine) {
  nlohmann::json doc;
  doc["schemaVersion"] = 1;
  doc["id"] = safeId("gap_" + c.sampleId);
  doc["name"] = c.sampleId + " · StandardGap" + (c.useModel ? " · 模型" : "") +
                (fine ? " · 细粒度" : "");
  nlohmann::json meta{{"app", "StandardGap.yml importer"}, {"style", fine ? "fine" : "blocks"}};
  if (!c.notes.empty()) meta["importNotes"] = c.notes;
  doc["meta"] = std::move(meta);
  doc["nodes"] = std::move(g.nodes);
  doc["edges"] = std::move(g.edges);
  nlohmann::json outputs = nlohmann::json::object();
  outputs["gap"] = nlohmann::json{{"node", t.gapNode}, {"port", "value"}, {"label", "间隙"}};
  outputs["flush"] = nlohmann::json{{"node", t.flushNode}, {"port", "value"}, {"label", "段差"}};
  outputs["bundle"] =
      nlohmann::json{{"node", t.bundleNode}, {"port", "bundle"}, {"label", "结果汇总"}};
  doc["outputs"] = std::move(outputs);

  // 顶层参数（m7-plan J10）：「本来就是全局」的值只有一处定义，宿主用 --param / params_json
  // 传值而不是改节点。绑定逐个列出节点，不按算子类型通配 —— 新加的节点得有人想到它。
  nlohmann::json graphParams = nlohmann::json::object();
  graphParams["gapOffset"] = nlohmann::json{
      {"type", "float"},
      {"default", numberOr(c.gap, "offset", 0.0)},
      {"binds", nlohmann::json::array({t.gapNode + ".offset", t.circlesNode + ".offset"})},
      {"doc", "间隙偏置（毫米），同时进间隙读数与相机候选的打分（gap.offset）。"}};
  if (c.useModel) {
    graphParams["modelPath"] = nlohmann::json{
        {"type", "path"},
        {"default", c.modelPath},
        {"binds", t.modelPathBinds},
        {"doc", "模型 ROI 用的 ONNX（setting.yml 的 model_roi.model_path）。"}};
  }
  doc["params"] = std::move(graphParams);
  return doc.dump(2);
}

// ================================================================ 积木图（默认）

struct Located {
  Ref rois;
  Ref scan;
};

std::string blockLocateTemplate(Builder& g, const Config& c, const std::string& id,
                                const Ref& scan, int column, int row, const char* title) {
  nlohmann::json p = icpParams(c);
  p["templateDir"] = c.templateDir;
  p["overallRoi"] = overallRoiOf(c);
  p["overallMode"] = textOr(c.common, "overall_roi_mode", "fixed");
  p["overallCamera"] = c.usingCamera;
  // 每个槽各写自己的四框（m8-plan L19）：候选自带 rois 的用自己的，没带的在解析时已经用
  // 配置里的全局 rois 填好（Candidate 的四个框总是齐的）。槽 1 恒启用，不写 Enabled。
  for (std::size_t i = 0; i < 4 && i < c.candidates.size(); ++i) {
    const std::string prefix = "template" + std::to_string(i + 1);
    const Candidate& cand = c.candidates[i];
    if (i > 0) p[prefix + "Enabled"] = true;
    p[prefix + "Id"] = cand.id;
    p[prefix + "Left"] = cand.left;
    p[prefix + "Right"] = cand.right;
    p[prefix + "DatumRoi"] = cand.flushBase;
    p[prefix + "TargetRoi"] = cand.flushRef;
    p[prefix + "SeamLeftRoi"] = cand.gapLeft;
    p[prefix + "SeamRightRoi"] = cand.gapRight;
  }
  const std::string node = g.node(id, "gap.locate_template", p, column, row, title);
  g.edge(scan.node, scan.port, node, "scan");
  return node;
}

/// 方向基准能不能写成积木：窗朝背离缝的那一侧、x 锚是同侧的缝框、y 锚是角色框。
/// 写得出来就返回 role（datum / target），否则空串。
std::string datumRole(const Config& c) {
  const std::string role = c.heightKind == "flush_ref" ? "target" : "datum";
  const bool right = role == "datum" ? c.datumRight : !c.datumRight;
  const std::string anchor = right ? "gap_right" : "gap_left";
  const std::string side = right ? "right" : "left";
  return c.anchorKind == anchor && c.datumWindowSide == side ? role : std::string();
}

/// 方向基准：写得成积木就是一个 gap.datum_direction；写不成（锚与侧显式配成了别的组合）
/// 就退回细粒度的「窗 → 裁 → 拟」，经 split_* 从 Bundle 里取框和云 —— 两种算子混用在一张图里。
Ref blockDatum(Builder& g, const Config& c, const std::string& prefix, const Located& at,
               int row) {
  const YAML::Node cfg = child(c.flush, "base_direction");
  const std::string role = datumRole(c);
  if (!role.empty()) {
    nlohmann::json p;
    p["startMm"] = numberOr(cfg, "start_mm", 0.6);
    p["lengthMm"] = numberOr(cfg, "length_mm", 13.4);
    p["heightMm"] = numberOr(cfg, "height_mm", 2.5);
    p["distThresh"] = numberOr(cfg, "fit_distance", 0.35);
    p["minInliers"] = intOr(cfg, "min_inliers", 60);
    if (role != "datum") p["role"] = role;
    const std::string node =
        g.node(prefix + "n_datum", "gap.datum_direction", p, 3, row, "方向基准");
    g.edge(at.scan.node, at.scan.port, node, "scan");
    g.edge(at.rois.node, at.rois.port, node, "rois");
    return Ref{node, "line"};
  }
  const std::string rois = g.node(prefix + "n_split_rois", "gap.split_roi_set", nullptr, 3, row,
                                  "拆角色框（方向基准）");
  g.edge(at.rois.node, at.rois.port, rois, "rois");
  const std::string scan = g.node(prefix + "n_split_scan", "gap.split_scan_pair", nullptr, 3,
                                  row + 1, "拆剖面对（方向基准）");
  g.edge(at.scan.node, at.scan.port, scan, "scan");
  const char* anchorPort = c.anchorKind == "gap_left" ? "seamLeft" : "seamRight";
  const char* heightPort = c.heightKind == "flush_base" ? "datum" : "target";
  nlohmann::json winParams;
  winParams["side"] = c.datumWindowSide;
  winParams["startMm"] = numberOr(cfg, "start_mm", 0.6);
  winParams["lengthMm"] = numberOr(cfg, "length_mm", 13.4);
  winParams["heightMm"] = numberOr(cfg, "height_mm", 2.5);
  const std::string win =
      g.node(prefix + "n_datum_box", "gap.datum_window", winParams, 4, row, "方向基准窗");
  g.edge(rois, anchorPort, win, "anchor");
  g.edge(rois, heightPort, win, "heightAnchor");
  nlohmann::json cropOpen{{"bounds", "open"}};
  const std::string crop =
      g.node(prefix + "n_crop_datum", "filter.crop_box2d", cropOpen, 5, row, "裁方向基准窗");
  g.edge(scan, "merged", crop, "cloud");
  g.edge(win, "box", crop, "box");
  nlohmann::json fitParams;
  fitParams["distThresh"] = numberOr(cfg, "fit_distance", 0.35);
  fitParams["segmentPoints"] = 0;
  fitParams["endpoints"] = "inlier_ends";
  fitParams["minInliers"] = intOr(cfg, "min_inliers", 60);
  const std::string fit =
      g.node(prefix + "n_fit_datum", "gap.fit_line", fitParams, 6, row, "拟合方向基准线");
  g.edge(crop, "cloud", fit, "cloud");
  g.edge(win, "box", fit, "box");
  g.edge(rois, anchorPort, fit, "toward");
  return Ref{fit, "line"};
}

/// 段差那一支：基准线 + 参考点（+ 可选的方向基准）。
struct FlushChain {
  Ref line, point, qualityBase, qualityRef, refLine;
};

FlushChain blockFlush(Builder& g, const Config& c, const std::string& prefix, const Located& at,
                      const char* endpoints, int row) {
  FlushChain out;
  nlohmann::json lineParams;
  lineParams["distThresh"] = c.lineDist;
  lineParams["segmentPoints"] = c.segmentPoints;
  lineParams["endpoints"] = endpoints;
  if (c.wantDatum) {
    const YAML::Node cfg = child(c.flush, "base_direction");
    lineParams["dirMode"] = c.datumMode;
    lineParams["dirNominalDeg"] = numberOr(cfg, "nominal_deg", 0.0);
    lineParams["dirTolDeg"] = numberOr(cfg, "tolerance_deg", 12.0);
  }
  const std::string line =
      g.node(prefix + "n_line", "gap.role_line", lineParams, 4, row, "拟合基准线");
  g.edge(at.scan.node, at.scan.port, line, "scan");
  g.edge(at.rois.node, at.rois.port, line, "rois");
  if (c.wantDatum) {
    const Ref datum = blockDatum(g, c, prefix, at, row + 3);
    g.edge(datum.node, datum.port, line, "refLine");
  }
  out.line = Ref{line, "line"};
  out.qualityBase = Ref{line, "quality"};

  nlohmann::json refParams;
  refParams["method"] = refMethod(c.refType);
  if (c.refType == "line end") {
    refParams["distThresh"] = c.lineDist;
    refParams["segmentPoints"] = c.segmentPoints;
    refParams["endpoints"] = endpoints;
  }
  const std::string ref =
      g.node(prefix + "n_ref_point", "gap.ref_point", refParams, 4, row + 1, "取参考点");
  g.edge(at.scan.node, at.scan.port, ref, "scan");
  g.edge(at.rois.node, at.rois.port, ref, "rois");
  if (c.refType == "nearest point") g.edge(line, "line", ref, "baseLine");
  out.point = Ref{ref, "point"};
  if (c.refType == "line end") {
    out.qualityRef = Ref{ref, "quality"};
    out.refLine = Ref{ref, "line"};
  }
  return out;
}

Ref fallbackOf(Builder& g, const std::string& id, const Ref& a, const Ref& b, int column, int row,
               const char* title) {
  const std::string node = g.node(id, "flow.fallback", nullptr, column, row, title);
  g.edge(a.node, a.port, node, "a");
  g.edge(b.node, b.port, node, "b");
  return Ref{node, "out"};
}

Status buildBlocks(const Config& c, std::string& graphJson) {
  Builder g;
  Tail tail;

  // 读剖面。配了双相机闸时闸站在读文件与换轴之间：细粒度的 load → guard，再交给 read_scan。
  nlohmann::json scanParams;
  if (c.usingRemoval) {
    scanParams["removeOutliers"] = true;
    scanParams["outlierRadiusMm"] = numberOr(c.filter, "filter_radius", 0.0);
    scanParams["outlierNeighbors"] = intOr(c.filter, "filter_neighbors", 0);
  }
  if (boolOr(c.guard, "enabled", false)) {
    nlohmann::json loadParams{{"dir", "."}};
    if (c.useModel) loadParams["dropNonFinite"] = false;
    const std::string load =
        g.node("n_load", "gap.load_profile_pair", loadParams, 0, 0, "读一对剖面");
    const std::string guard =
        g.node("n_camera_guard", "gap.camera_guard", guardParams(c), 0, 2, "双相机闸");
    g.edge(load, "primary", guard, "primary");
    g.edge(load, "secondary", guard, "secondary");
    scanParams["source"] = "inputs";
    const std::string scan = g.node("n_scan", "gap.read_scan", scanParams, 1, 1, "读剖面");
    g.edge(guard, "primary", scan, "primary");
    g.edge(guard, "secondary", scan, "secondary");
  } else {
    scanParams["dir"] = ".";
    g.node("n_scan", "gap.read_scan", scanParams, 0, 1, "读剖面");
  }
  const Ref raw{"n_scan", "scan"};

  Located at;  // 量测用的那一份（带回退时是回退之后的）
  FlushChain flush;
  if (!c.useModel) {
    const std::string locate = blockLocateTemplate(g, c, "n_locate", raw, 2, 1, "模板定位");
    at = Located{Ref{locate, "rois"}, Ref{locate, "scan"}};
    flush = blockFlush(g, c, "", at, "roi_intersection", 0);
  } else {
    const nlohmann::json roll = rollParams(c);
    nlohmann::json modelParams;
    modelParams["cropEnabled"] = roll["enabled"];
    modelParams["cropHalfWidth"] = roll["halfWidth"];
    modelParams["cropHalfHeight"] = roll["halfHeight"];
    modelParams["cropMaxRollBoxHeight"] = roll["maxRollBoxHeight"];
    modelParams["cropMinPointsKept"] = roll["minPointsKept"];
    modelParams["cropUsingCamera"] = roll["usingCamera"];
    // modelPath 不写在节点上：它由顶层参数 modelPath 绑定，宿主换模型只传一个值。
    const std::string model = g.node("n_model", "gap.locate_model", modelParams, 2, 1, "模型定位");
    g.edge(raw.node, raw.port, model, "scan");
    tail.modelPathBinds.push_back(model + ".modelPath");
    const Located byModel{Ref{model, "rois"}, Ref{model, "scan"}};
    flush = blockFlush(g, c, "", byModel, "inlier_ends", 0);
    if (!c.withFallback) {
      at = byModel;
    } else {
      // 模型 → 模板回退是图上的结构（L9）：两个定位节点并联，rois 与 scan 各一个 fallback。
      // 段差那一支与细粒度图一样整条备一份，基准线 / 参考点在拟合这一级回退。
      const std::string locate =
          blockLocateTemplate(g, c, "b_n_locate", raw, 2, 6, "模板定位（备用）");
      const Located byTemplate{Ref{locate, "rois"}, Ref{locate, "scan"}};
      at.rois = fallbackOf(g, "n_fb_rois", byModel.rois, byTemplate.rois, 3, 4, "回退角色框");
      at.scan = fallbackOf(g, "n_fb_scan", byModel.scan, byTemplate.scan, 3, 5, "回退剖面对");
      const FlushChain backup = blockFlush(g, c, "b_", byTemplate, "roi_intersection", 6);
      flush.line = fallbackOf(g, "n_fb_line", flush.line, backup.line, 5, 0, "回退基准线");
      flush.point =
          fallbackOf(g, "n_fb_ref_point", flush.point, backup.point, 5, 1, "回退参考点");
      flush.qualityBase = fallbackOf(g, "n_fb_quality_base", flush.qualityBase,
                                     backup.qualityBase, 5, 2, "回退基准线质量");
      if (!flush.qualityRef.empty()) {
        flush.qualityRef = fallbackOf(g, "n_fb_quality_ref", flush.qualityRef, backup.qualityRef,
                                      5, 3, "回退参考线质量");
        if (c.wantBand && c.bandRef == "flush_ref") {
          flush.refLine =
              fallbackOf(g, "n_fb_ref_line", flush.refLine, backup.refLine, 5, 4, "回退参考线");
        }
      }
    }
  }

  nlohmann::json flushParams;
  flushParams["offset"] = numberOr(c.flush, "offset", 0.0);
  tail.flushNode = g.node("n_flush", "gap.flush", flushParams, 6, 0, "段差");
  g.edge(flush.line.node, flush.line.port, tail.flushNode, "baseLine");
  g.edge(flush.point.node, flush.point.port, tail.flushNode, "refPoint");

  tail.circlesNode =
      g.node("n_circles", "gap.seam_circles", circleParams(c), 4, 3, "拟合缝两侧圆");
  g.edge(at.scan.node, at.scan.port, tail.circlesNode, "scan");
  g.edge(at.rois.node, at.rois.port, tail.circlesNode, "rois");
  if (c.wantBand) {
    const Ref& ref = c.bandRef == "flush_ref" ? flush.refLine : flush.line;
    g.edge(ref.node, ref.port, tail.circlesNode, "refLine");
  }

  nlohmann::json gapParams;
  gapParams["definition"] = c.definition;
  tail.gapNode = g.node("n_gap", "gap.gap", gapParams, 6, 3, "间隙");
  g.edge(tail.circlesNode, "left", tail.gapNode, "left");
  g.edge(tail.circlesNode, "right", tail.gapNode, "right");
  if (c.definition == "A") g.edge(tail.flushNode, "baseLine", tail.gapNode, "baseLine");

  judges(g, c, tail.flushNode, tail.gapNode, 7);

  tail.bundleNode = g.node("n_bundle", "gap.result_bundle", bundleParams(c), 7, 5, "结果汇总");
  g.edge(tail.gapNode, "value", tail.bundleNode, "gap");
  g.edge(tail.flushNode, "value", tail.bundleNode, "flush");
  g.edge(at.rois.node, at.rois.port, tail.bundleNode, "rois");
  g.edge(at.scan.node, at.scan.port, tail.bundleNode, "scan");
  g.edge(tail.circlesNode, "quality", tail.bundleNode, "fits");
  g.edge(flush.qualityBase.node, flush.qualityBase.port, tail.bundleNode, "fitBase");
  if (!flush.qualityRef.empty()) {
    g.edge(flush.qualityRef.node, flush.qualityRef.port, tail.bundleNode, "fitRef");
  }
  if (c.withFallback) g.edge("n_fb_rois", "choice", tail.bundleNode, "fallback");

  graphJson = finish(g, c, tail, /*fine=*/false);
  return Status::Ok();
}

// ================================================================== 细粒度图（:fine）

/// 模板分支：整体框 → 裁 → 模板 → ICP → 选 → 业务框 → 组 RoiSet / ScanPair。
/// 合并云（去噪之后的）是整片传进来的，在这里跟着整体框一起裁（与 gap.locate_template 相同）。
struct FineTemplate {
  std::string overall, cropP, cropS, cropM, select, rois, roiSet, scanSet;
};

FineTemplate fineTemplateBranch(Builder& g, const Config& c, const std::string& prefix,
                                const Ref& p, const Ref& s, const Ref& mergedFull, int column) {
  FineTemplate b;
  nlohmann::json overallParams;
  overallParams["roi"] = overallRoiOf(c);
  overallParams["mode"] = textOr(c.common, "overall_roi_mode", "fixed");
  overallParams["usingCamera"] = c.usingCamera;
  b.overall = g.node(prefix + "n_overall", "gap.overall_roi", overallParams, column, 1, "整体 ROI");
  g.edge(p.node, p.port, b.overall, "primary");
  g.edge(s.node, s.port, b.overall, "secondary");

  nlohmann::json cropOpen{{"bounds", "open"}};
  b.cropP = g.node(prefix + "n_crop_p", "filter.crop_box2d", cropOpen, column + 1, 0, "裁 primary");
  b.cropS =
      g.node(prefix + "n_crop_s", "filter.crop_box2d", cropOpen, column + 1, 2, "裁 secondary");
  b.cropM = g.node(prefix + "n_crop_m", "filter.crop_box2d", cropOpen, column + 1, 4, "裁合并云");
  g.edge(p.node, p.port, b.cropP, "cloud");
  g.edge(b.overall, "box", b.cropP, "box");
  g.edge(s.node, s.port, b.cropS, "cloud");
  g.edge(b.overall, "box", b.cropS, "box");
  g.edge(mergedFull.node, mergedFull.port, b.cropM, "cloud");
  g.edge(b.overall, "box", b.cropM, "box");

  std::vector<std::string> alignNodes;
  for (std::size_t order = 0; order < c.candidates.size(); ++order) {
    const Candidate& cand = c.candidates[order];
    const std::string cid = safeId(cand.id);
    nlohmann::json tplParams;
    tplParams["dir"] = c.templateDir;
    tplParams["left"] = cand.left;
    tplParams["right"] = cand.right;
    const std::string tpl = g.node(prefix + "n_tpl_" + cid, "gap.load_template", tplParams,
                                   column + 2, 3 + static_cast<int>(order) * 2, "模板 " + cid);
    nlohmann::json ap = icpParams(c);
    ap["templateId"] = cand.id;
    ap["order"] = static_cast<std::int64_t>(order);
    ap["roiFlushBase"] = cand.flushBase;
    ap["roiGapLeft"] = cand.gapLeft;
    ap["roiFlushRef"] = cand.flushRef;
    ap["roiGapRight"] = cand.gapRight;
    const std::string node = g.node(prefix + "n_align_" + cid, "gap.align_template", ap,
                                    column + 3, 3 + static_cast<int>(order) * 2, "ICP " + cid);
    g.edge(b.cropM, "cloud", node, "cloud");
    g.edge(tpl, "left", node, "tplLeft");
    g.edge(tpl, "right", node, "tplRight");
    alignNodes.push_back(node);
  }

  nlohmann::json selectParams;
  selectParams["minScore"] = c.minScore;
  b.select = g.node(prefix + "n_select", "gap.select_alignment", selectParams, column + 4, 3,
                    "选模板");
  static const char* kPorts[4] = {"a", "b", "c", "d"};
  for (std::size_t i = 0; i < alignNodes.size() && i < 4; ++i) {
    g.edge(alignNodes[i], "alignment", b.select, kPorts[i]);
  }

  // 基准件在哪一侧由选中模板的框推出（datumSide 默认 auto，与 gap.locate_template 同一个判据）。
  b.rois = g.node(prefix + "n_rois", "gap.business_rois", nullptr, column + 5, 3, "业务 ROI");
  g.edge(b.select, "alignment", b.rois, "alignment");

  nlohmann::json setParams{{"source", "template"}};
  b.roiSet = g.node(prefix + "n_roi_set", "gap.make_roi_set", setParams, column + 6, 3, "组角色框");
  g.edge(b.rois, "flushBase", b.roiSet, "datum");
  g.edge(b.rois, "flushRef", b.roiSet, "target");
  g.edge(b.rois, "gapLeft", b.roiSet, "seamLeft");
  g.edge(b.rois, "gapRight", b.roiSet, "seamRight");
  g.edge(b.select, "alignment", b.roiSet, "alignment");
  g.edge(b.overall, "box", b.roiSet, "overall");
  b.scanSet =
      g.node(prefix + "n_scan_set", "gap.make_scan_pair", nullptr, column + 6, 5, "组剖面对");
  g.edge(b.cropP, "cloud", b.scanSet, "primary");
  g.edge(b.cropS, "cloud", b.scanSet, "secondary");
  g.edge(b.cropM, "cloud", b.scanSet, "merged");
  return b;
}

/// 一组四个框的来源：flushBase / flushRef / gapLeft / gapRight / seam 各一个端口。
struct FineRois {
  Ref box[4];  // flushBase, flushRef, gapLeft, gapRight
  Ref seam;
};

FineRois roisFrom(const std::string& node) {
  FineRois r;
  r.box[0] = Ref{node, "flushBase"};
  r.box[1] = Ref{node, "flushRef"};
  r.box[2] = Ref{node, "gapLeft"};
  r.box[3] = Ref{node, "gapRight"};
  r.seam = Ref{node, "seam"};
  return r;
}

/// 细粒度的方向基准：「窗 → 裁 → 拟」三个节点。
Ref fineDatum(Builder& g, const Config& c, const std::string& prefix, const FineRois& rois,
              const Ref& cloud, int row) {
  const YAML::Node cfg = child(c.flush, "base_direction");
  const Ref& anchor = c.anchorKind == "gap_left" ? rois.box[2] : rois.box[3];
  const Ref& height = c.heightKind == "flush_base" ? rois.box[0] : rois.box[1];
  nlohmann::json winParams;
  winParams["side"] = c.datumWindowSide;
  winParams["startMm"] = numberOr(cfg, "start_mm", 0.6);
  winParams["lengthMm"] = numberOr(cfg, "length_mm", 13.4);
  winParams["heightMm"] = numberOr(cfg, "height_mm", 2.5);
  const std::string win =
      g.node(prefix + "n_datum_box", "gap.datum_window", winParams, 8, row, "方向基准窗");
  g.edge(anchor.node, anchor.port, win, "anchor");
  g.edge(height.node, height.port, win, "heightAnchor");
  nlohmann::json cropOpen{{"bounds", "open"}};
  const std::string crop =
      g.node(prefix + "n_crop_datum", "filter.crop_box2d", cropOpen, 9, row, "裁方向基准窗");
  g.edge(cloud.node, cloud.port, crop, "cloud");
  g.edge(win, "box", crop, "box");
  nlohmann::json fitParams;
  fitParams["distThresh"] = numberOr(cfg, "fit_distance", 0.35);
  fitParams["segmentPoints"] = 0;  // 长面整条都要，不截
  fitParams["endpoints"] = "inlier_ends";
  fitParams["minInliers"] = intOr(cfg, "min_inliers", 60);
  const std::string fit =
      g.node(prefix + "n_fit_datum", "gap.fit_line", fitParams, 10, row, "拟合方向基准线");
  g.edge(crop, "cloud", fit, "cloud");
  g.edge(win, "box", fit, "box");
  // 方向基准线的 toward 接它的锚框：长窗是从锚框推出去的，靠锚框那一头就是靠缝那一头。
  g.edge(anchor.node, anchor.port, fit, "toward");
  return Ref{fit, "line"};
}

FlushChain fineFlush(Builder& g, const Config& c, const std::string& prefix, const FineRois& rois,
                     const Ref& cloud, const char* endpoints, int row) {
  FlushChain out;
  nlohmann::json cropOpen{{"bounds", "open"}};
  const std::string cropBase = g.node(prefix + "n_crop_flushBase", "filter.crop_box2d", cropOpen,
                                      9, row, "裁 flush_base");
  g.edge(cloud.node, cloud.port, cropBase, "cloud");
  g.edge(rois.box[0].node, rois.box[0].port, cropBase, "box");
  const std::string cropRef = g.node(prefix + "n_crop_flushRef", "filter.crop_box2d", cropOpen, 9,
                                     row + 1, "裁 flush_ref");
  g.edge(cloud.node, cloud.port, cropRef, "cloud");
  g.edge(rois.box[1].node, rois.box[1].port, cropRef, "box");

  nlohmann::json baseParams;
  baseParams["distThresh"] = c.lineDist;
  baseParams["segmentPoints"] = c.segmentPoints;
  baseParams["endpoints"] = endpoints;
  if (c.wantDatum) {
    const YAML::Node cfg = child(c.flush, "base_direction");
    baseParams["dirMode"] = c.datumMode;
    baseParams["dirNominalDeg"] = numberOr(cfg, "nominal_deg", 0.0);
    baseParams["dirTolDeg"] = numberOr(cfg, "tolerance_deg", 12.0);
  }
  // 「靠缝那一端」接 seam（两个缝框中心的中点），与积木算子同一个判据（L7）。
  const std::string fitBase =
      g.node(prefix + "n_fit_base", "gap.fit_line", baseParams, 10, row, "拟合基准线");
  g.edge(cropBase, "cloud", fitBase, "cloud");
  g.edge(rois.box[0].node, rois.box[0].port, fitBase, "box");
  g.edge(rois.seam.node, rois.seam.port, fitBase, "toward");
  if (c.wantDatum) {
    const Ref datum = fineDatum(g, c, prefix, rois, cloud, row + 2);
    g.edge(datum.node, datum.port, fitBase, "refLine");
  }
  out.line = Ref{fitBase, "line"};
  out.qualityBase = Ref{fitBase, "quality"};

  if (c.refType == "line end") {
    nlohmann::json p;
    p["distThresh"] = c.lineDist;
    p["segmentPoints"] = c.segmentPoints;
    p["endpoints"] = endpoints;
    const std::string fitRef =
        g.node(prefix + "n_fit_ref", "gap.fit_line", p, 10, row + 1, "拟合参考线");
    g.edge(cropRef, "cloud", fitRef, "cloud");
    g.edge(rois.box[1].node, rois.box[1].port, fitRef, "box");
    g.edge(rois.seam.node, rois.seam.port, fitRef, "toward");
    out.point = Ref{fitRef, "innerEnd"};
    out.qualityRef = Ref{fitRef, "quality"};
    out.refLine = Ref{fitRef, "line"};
  } else if (c.refType == "selected point") {
    const std::string sel =
        g.node(prefix + "n_sel_ref", "gap.selected_point", nullptr, 10, row + 1, "选参考点");
    g.edge(cloud.node, cloud.port, sel, "cloud");
    g.edge(rois.box[1].node, rois.box[1].port, sel, "box");
    out.point = Ref{sel, "point"};
  } else {
    const std::string nearest = g.node(prefix + "n_near_ref", "gap.nearest_to_line", nullptr, 10,
                                       row + 1, "离基准线最近的点");
    g.edge(cropRef, "cloud", nearest, "cloud");
    g.edge(fitBase, "line", nearest, "line");
    out.point = Ref{nearest, "point"};
  }
  return out;
}

Status buildFine(const Config& c, std::string& graphJson) {
  Builder g;
  Tail tail;

  nlohmann::json loadParams{{"dir", "."}};
  if (c.useModel) loadParams["dropNonFinite"] = false;
  const std::string load = g.node("n_load", "gap.load_profile_pair", loadParams, 0, 1, "读一对剖面");

  // 双相机闸：两台看到的不是同一个面时（玻璃二次反射、标定漂），模型的两行输入和合并云
  // 会一起被带偏，而拟合残差照样很小。闸站在 load 之后、模型之前。配置里开了才有这个节点。
  std::string clouds = load;
  if (boolOr(c.guard, "enabled", false)) {
    const std::string guard =
        g.node("n_camera_guard", "gap.camera_guard", guardParams(c), 0, 3, "双相机闸");
    g.edge(load, "primary", guard, "primary");
    g.edge(load, "secondary", guard, "secondary");
    clouds = guard;
  }

  const std::string frameP =
      g.node("n_frame_p", "gap.to_measurement_frame", nullptr, 1, 0, "换轴 primary");
  const std::string frameS =
      g.node("n_frame_s", "gap.to_measurement_frame", nullptr, 1, 2, "换轴 secondary");
  g.edge(clouds, "primary", frameP, "cloud");
  g.edge(clouds, "secondary", frameS, "cloud");

  // 两片有限点的云：模板路径 load 时就剔了 NaN，模型路径要保留槽位，换轴之后再剔。
  Ref finiteP{frameP, "cloud"};
  Ref finiteS{frameS, "cloud"};
  if (c.useModel) {
    const std::string dropP =
        g.node("n_drop_p", "gap.drop_non_finite", nullptr, 2, 0, "剔 NaN primary");
    const std::string dropS =
        g.node("n_drop_s", "gap.drop_non_finite", nullptr, 2, 2, "剔 NaN secondary");
    g.edge(frameP, "cloud", dropP, "cloud");
    g.edge(frameS, "cloud", dropS, "cloud");
    finiteP = Ref{dropP, "cloud"};
    finiteS = Ref{dropS, "cloud"};
  }

  // 合并（secondary 在前）→ 去噪，在整片云上做，裁剪在后面（与 gap.read_scan 相同）。
  const std::string merge = g.node("n_merge", "util.merge", nullptr, 2, 4, "合并（secondary 在前）");
  g.edge(finiteS.node, finiteS.port, merge, "a");
  g.edge(finiteP.node, finiteP.port, merge, "b");
  Ref mergedFull{merge, "cloud"};
  if (c.usingRemoval) {
    nlohmann::json p;
    p["radius"] = numberOr(c.filter, "filter_radius", 0.0) / 1000.0;
    p["minNeighbors"] = intOr(c.filter, "filter_neighbors", 0);
    const std::string flt = g.node("n_filter", "filter.radius_outlier", p, 3, 4, "半径离群");
    g.edge(merge, "cloud", flt, "cloud");
    mergedFull = Ref{flt, "cloud"};
  }

  // 量测用的那一份：云、四个框、朝向框、组好的 RoiSet / ScanPair。
  Ref merged, cropP, cropS, roiSetRef, scanSetRef;
  FineRois rois;
  FlushChain flush;
  if (!c.useModel) {
    const FineTemplate b = fineTemplateBranch(g, c, "", Ref{frameP, "cloud"},
                                              Ref{frameS, "cloud"}, mergedFull, 3);
    merged = Ref{b.cropM, "cloud"};
    cropP = Ref{b.cropP, "cloud"};
    cropS = Ref{b.cropS, "cloud"};
    rois = roisFrom(b.rois);
    roiSetRef = Ref{b.roiSet, "rois"};
    scanSetRef = Ref{b.scanSet, "scan"};
    flush = fineFlush(g, c, "", rois, merged, "roi_intersection", 0);
  } else {
    const std::string tensor = g.node("n_tensor", "gap.profile_tensor", nullptr, 1, 6, "剖面张量");
    g.edge(clouds, "primary", tensor, "primary");
    g.edge(clouds, "secondary", tensor, "secondary");
    // modelPath 不写在节点上：它由顶层参数 modelPath 绑定，宿主换模型只传一个值。
    const std::string infer = g.node("n_infer", "ml.onnx_run", nullptr, 2, 6, "ONNX 推理");
    tail.modelPathBinds.push_back(infer + ".modelPath");
    g.edge(tensor, "tensor", infer, "input");
    const std::string seg =
        g.node("n_seg", "gap.labels_from_logits", nullptr, 3, 6, "逐槽 argmax");
    g.edge(infer, "output", seg, "tensor");
    nlohmann::json coloredParams{{"row", "primary"}};
    const std::string colored =
        g.node("n_labels_p", "gap.labels_to_cloud", coloredParams, 4, 7, "着色 primary");
    g.edge(frameP, "cloud", colored, "cloud");
    g.edge(seg, "labels", colored, "labels");
    const std::string modelRois =
        g.node("n_rois", "gap.roi_from_labels", nullptr, 4, 6, "模型四框");
    g.edge(clouds, "primary", modelRois, "primary");
    g.edge(clouds, "secondary", modelRois, "secondary");
    g.edge(seg, "labels", modelRois, "labels");
    g.edge(colored, "cloud", modelRois, "backdrop");

    const std::string roll =
        g.node("n_roll", "gap.roll_anchored_crop", rollParams(c), 5, 1, "跟随零件的裁剪窗");
    g.edge(finiteP.node, finiteP.port, roll, "primary");
    g.edge(finiteS.node, finiteS.port, roll, "secondary");
    g.edge(modelRois, "gapLeft", roll, "gapLeft");
    g.edge(modelRois, "gapRight", roll, "gapRight");
    g.edge(mergedFull.node, mergedFull.port, roll, "merged");

    nlohmann::json setParams{{"source", "model"}};
    const std::string modelSet =
        g.node("n_roi_set", "gap.make_roi_set", setParams, 6, 6, "组角色框");
    g.edge(modelRois, "flushBase", modelSet, "datum");
    g.edge(modelRois, "flushRef", modelSet, "target");
    g.edge(modelRois, "gapLeft", modelSet, "seamLeft");
    g.edge(modelRois, "gapRight", modelSet, "seamRight");
    g.edge(roll, "window", modelSet, "overall");
    g.edge(roll, "status", modelSet, "cropStatus");
    const std::string modelScan =
        g.node("n_scan_set", "gap.make_scan_pair", nullptr, 6, 7, "组剖面对");
    g.edge(roll, "primary", modelScan, "primary");
    g.edge(roll, "secondary", modelScan, "secondary");
    g.edge(roll, "merged", modelScan, "merged");

    const FineRois byModel = roisFrom(modelRois);
    flush = fineFlush(g, c, "", byModel, Ref{roll, "merged"}, "inlier_ends", 0);

    if (!c.withFallback) {
      merged = Ref{roll, "merged"};
      cropP = Ref{roll, "primary"};
      cropS = Ref{roll, "secondary"};
      rois = byModel;
      roiSetRef = Ref{modelSet, "rois"};
      scanSetRef = Ref{modelScan, "scan"};
    } else {
      // 备用闭包：同一份有限点的云、同一份去噪之后的合并云，走模板分支（与积木图的
      // b_n_locate 同源）。四个框、三片云、RoiSet / ScanPair 各一个 fallback。
      const FineTemplate b = fineTemplateBranch(g, c, "b_", finiteP, finiteS, mergedFull, 3);
      static const char* kRoiPorts[4] = {"flushBase", "flushRef", "gapLeft", "gapRight"};
      static const char* kFbTitles[4] = {"回退 flush_base", "回退 flush_ref", "回退 gap_left",
                                         "回退 gap_right"};
      for (int i = 0; i < 4; ++i) {
        rois.box[i] = fallbackOf(g, std::string("n_fb_") + kRoiPorts[i], byModel.box[i],
                                 Ref{b.rois, kRoiPorts[i]}, 7, i, kFbTitles[i]);
      }
      merged = fallbackOf(g, "n_fb_merged", Ref{roll, "merged"}, Ref{b.cropM, "cloud"}, 7, 4,
                          "回退合并云");
      cropP = fallbackOf(g, "n_fb_crop_p", Ref{roll, "primary"}, Ref{b.cropP, "cloud"}, 7, 5,
                         "回退 primary");
      cropS = fallbackOf(g, "n_fb_crop_s", Ref{roll, "secondary"}, Ref{b.cropS, "cloud"}, 7, 6,
                         "回退 secondary");
      roiSetRef = fallbackOf(g, "n_fb_roi_set", Ref{modelSet, "rois"}, Ref{b.roiSet, "rois"}, 7,
                             7, "回退角色框");
      scanSetRef = fallbackOf(g, "n_fb_scan_set", Ref{modelScan, "scan"},
                              Ref{b.scanSet, "scan"}, 7, 8, "回退剖面对");
      const FlushChain backup =
          fineFlush(g, c, "b_", roisFrom(b.rois), Ref{b.cropM, "cloud"}, "roi_intersection", 9);
      flush.line = fallbackOf(g, "n_fb_line", flush.line, backup.line, 11, 5, "回退基准线");
      flush.point =
          fallbackOf(g, "n_fb_ref_point", flush.point, backup.point, 11, 6, "回退参考点");
      flush.qualityBase = fallbackOf(g, "n_fb_quality_base", flush.qualityBase,
                                     backup.qualityBase, 11, 7, "回退基准线质量");
      if (!flush.qualityRef.empty()) {
        flush.qualityRef = fallbackOf(g, "n_fb_quality_ref", flush.qualityRef, backup.qualityRef,
                                      11, 8, "回退参考线质量");
        if (c.wantBand && c.bandRef == "flush_ref") {
          flush.refLine =
              fallbackOf(g, "n_fb_ref_line", flush.refLine, backup.refLine, 11, 9, "回退参考线");
        }
      }
    }
  }

  nlohmann::json flushParams;
  flushParams["offset"] = numberOr(c.flush, "offset", 0.0);
  tail.flushNode = g.node("n_flush", "gap.flush", flushParams, 11, 0, "段差");
  g.edge(flush.line.node, flush.line.port, tail.flushNode, "baseLine");
  g.edge(flush.point.node, flush.point.port, tail.flushNode, "refPoint");

  tail.circlesNode =
      g.node("n_circles", "gap.fit_gap_circles", circleParams(c), 10, 2, "两侧圆拟合");
  g.edge(merged.node, merged.port, tail.circlesNode, "merged");
  g.edge(cropP.node, cropP.port, tail.circlesNode, "primary");
  g.edge(cropS.node, cropS.port, tail.circlesNode, "secondary");
  g.edge(rois.box[2].node, rois.box[2].port, tail.circlesNode, "boxLeft");
  g.edge(rois.box[3].node, rois.box[3].port, tail.circlesNode, "boxRight");
  if (c.wantBand) {
    const Ref& ref = c.bandRef == "flush_ref" ? flush.refLine : flush.line;
    g.edge(ref.node, ref.port, tail.circlesNode, "refLine");
  }

  nlohmann::json gapParams;
  gapParams["definition"] = c.definition;
  tail.gapNode = g.node("n_gap", "gap.gap", gapParams, 11, 2, "间隙");
  g.edge(tail.circlesNode, "left", tail.gapNode, "left");
  g.edge(tail.circlesNode, "right", tail.gapNode, "right");
  if (c.definition == "A") g.edge(tail.flushNode, "baseLine", tail.gapNode, "baseLine");

  judges(g, c, tail.flushNode, tail.gapNode, 12);

  tail.bundleNode = g.node("n_bundle", "gap.result_bundle", bundleParams(c), 12, 4, "结果汇总");
  g.edge(tail.gapNode, "value", tail.bundleNode, "gap");
  g.edge(tail.flushNode, "value", tail.bundleNode, "flush");
  g.edge(roiSetRef.node, roiSetRef.port, tail.bundleNode, "rois");
  g.edge(scanSetRef.node, scanSetRef.port, tail.bundleNode, "scan");
  g.edge(tail.circlesNode, "quality", tail.bundleNode, "fits");
  g.edge(flush.qualityBase.node, flush.qualityBase.port, tail.bundleNode, "fitBase");
  if (!flush.qualityRef.empty()) {
    g.edge(flush.qualityRef.node, flush.qualityRef.port, tail.bundleNode, "fitRef");
  }
  if (c.withFallback) g.edge("n_fb_roi_set", "choice", tail.bundleNode, "fallback");

  graphJson = finish(g, c, tail, /*fine=*/true);
  return Status::Ok();
}

Status build(const std::string& text, const fs::path& baseDir, Mode mode, bool fine,
             std::string& out) {
  YAML::Node cfg;
  try {
    cfg = YAML::Load(text);
  } catch (const std::exception& e) {
    return badInput(std::string("StandardGap.yml 解析失败: ") + e.what());
  }
  if (!cfg || !cfg.IsMap()) return badInput("StandardGap.yml 不是一个映射");
  Config c(cfg);
  if (Status s = parseConfig(cfg, baseDir, mode, c); !s.ok) return s;
  return fine ? buildFine(c, out) : buildBlocks(c, out);
}

Status importAuto(const std::string& t, const fs::path& d, std::string& o) {
  return build(t, d, Mode::Auto, false, o);
}
Status importTemplate(const std::string& t, const fs::path& d, std::string& o) {
  return build(t, d, Mode::Template, false, o);
}
Status importModel(const std::string& t, const fs::path& d, std::string& o) {
  return build(t, d, Mode::Model, false, o);
}
Status importAutoFine(const std::string& t, const fs::path& d, std::string& o) {
  return build(t, d, Mode::Auto, true, o);
}
Status importTemplateFine(const std::string& t, const fs::path& d, std::string& o) {
  return build(t, d, Mode::Template, true, o);
}
Status importModelFine(const std::string& t, const fs::path& d, std::string& o) {
  return build(t, d, Mode::Model, true, o);
}

}  // namespace

void registerStandardGapImporter(Registry& r) {
  const char* kBlocks =
      "产出积木图：gap.read_scan → 定位 → gap.role_line / gap.ref_point → gap.flush，"
      "gap.seam_circles → gap.gap，再加判定与 gap.result_bundle。";
  const char* kFine = "产出细粒度图：每一步一个节点，与积木图同一份实现、同一帧上结果相同。";
  const std::string autoDoc =
      "把一份 StandardGap.yml 转成一张测量图。模式由 baseDir 或其父目录里的 setting.yml 决定："
      "model_roi.enabled 为真走模型路径并带 flow.fallback，否则走模板/ICP 路径。"
      "文档顶层的 template_dir 决定模板 PCD 所在的相对目录，缺省 StandardGap。";
  const std::string modelDoc =
      "强制模型 ROI 路径。ONNX 取自 setting.yml 的 model_roi.model_path，"
      "没有就找 baseDir 下唯一的 *.onnx。setting.yml 里 model_roi.enabled 为真时还会带上 "
      "flow.fallback 的模板备用闭包。";
  struct Kind {
    const char* kind;
    const char* label;
    std::string doc;
    ImportFn fn;
  };
  const Kind kinds[6] = {
      {"StandardGap.yml", "StandardGap.yml（自动）", autoDoc + kBlocks, &importAuto},
      {"StandardGap.yml:template", "StandardGap.yml（模板/ICP）",
       std::string("强制模板/ICP 路径，不看 setting.yml。") + kBlocks, &importTemplate},
      {"StandardGap.yml:model", "StandardGap.yml（模型 ROI）", modelDoc + kBlocks, &importModel},
      {"StandardGap.yml:fine", "StandardGap.yml（自动 · 细粒度）", autoDoc + kFine,
       &importAutoFine},
      {"StandardGap.yml:template:fine", "StandardGap.yml（模板/ICP · 细粒度）",
       std::string("强制模板/ICP 路径，不看 setting.yml。") + kFine, &importTemplateFine},
      {"StandardGap.yml:model:fine", "StandardGap.yml（模型 ROI · 细粒度）", modelDoc + kFine,
       &importModelFine},
  };
  for (const Kind& k : kinds) {
    ImporterDesc desc;
    desc.kind = k.kind;
    desc.label = k.label;
    desc.doc = k.doc;
    desc.fn = k.fn;
    r.addImporter(desc);
  }
}

}  // namespace lyflow::packs::gap
