// StandardGap.yml → 测量图（ADR-0017）。默认产出积木图（m8-plan L12）：人打开导入的图，
// 看到的就是他自己也拼得出来的那十来个节点；:fine 产出细粒度图，每一步一个节点，想精细控制时用。
// 两种图的算子共用同一份实现（L6），同一帧上 flush / gap 逐帧相同 —— 这是 M8a 验收 1。
#include <algorithm>
#include <array>
#include <cstdio>
#include <set>
#include <stdexcept>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <yaml-cpp/yaml.h>

#include "gap_ops.h"

namespace lyflow::packs::gap {
namespace {

namespace fs = std::filesystem;

constexpr const char* kSafe = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_";

std::string safeId(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text)
    out.push_back(std::string(kSafe).find(c) == std::string::npos ? '_' : c);
  return out;
}

Status badInput(const std::string& message) {
  return Status::Error(Phase::Validate, "bad_input", message);
}

Status badParam(const std::string& message) {
  return Status::Error(Phase::Validate, "bad_param", message);
}

YAML::Node child(const YAML::Node& node, const char* key) {
  if (!node || !node.IsMap())
    return YAML::Node(YAML::NodeType::Undefined);
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
  if (!present(node))
    return true;
  if (!node.IsSequence() || node.size() != 4) {
    *problem = "ROI 必须是四个数";
    return false;
  }
  for (std::size_t i = 0; i < 4; ++i)
    (*out)[i] = node[i].as<double>(0.0);
  return true;
}

bool allZero(const Roi& r) { return r[0] == 0 && r[1] == 0 && r[2] == 0 && r[3] == 0; }

/// 节点存于哪个算子版本：当前注册表里那个算子的 version，与编辑器新建节点写 op.version 一致。
/// 以后算子升主版本时，导入的图也能按 opVersion 自动迁移（ADR-0008；M8c 取舍 1 暴露了缺它的后果）。
/// 导入总在注册完成之后发生，此时 ensureRegistry() 里已经有全部算子包（含别的包的 flow / filter 算子）。
std::string opVersionOf(const char* op) {
  const OperatorDesc* desc = ensureRegistry().find(op);
  if (!desc)
    throw std::invalid_argument(std::string("当前运行库缺少导入所需能力或算子：") + op);
  return desc->version;
}

class Builder {
public:
  std::string node(const std::string& id, const char* op, nlohmann::json params, int column,
                   int row, const std::string& title) {
    nlohmann::json entry;
    entry["id"] = id;
    entry["op"] = op;
    if (const std::string version = opVersionOf(op); !version.empty())
      entry["opVersion"] = version;
    nlohmann::json ui;
    ui["position"] = nlohmann::json{{"x", column * 280}, {"y", row * 130}};
    if (!title.empty())
      ui["title"] = title;
    entry["ui"] = std::move(ui);
    if (!params.is_null() && !params.empty())
      entry["params"] = std::move(params);
    nodes.push_back(std::move(entry));
    return id;
  }

  void edge(const std::string& src, const std::string& srcPort, const std::string& dst,
            const std::string& dstPort) {
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
  std::string port;
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
  // 候选自己写了 rois（哪怕只写了一部分）。没写的四框在解析时已经用配置里的全局框填好 ——
  // 原算法把后一种记 roi_source = config（docs/result-bundle-plan.md (b)）
  bool ownRois = false;
};

bool datumOnRight(const Roi& datum, const Roi& left, const Roi& right) {
  return .5 * (datum[0] + datum[2]) > .25 * (left[0] + left[2] + right[0] + right[2]);
}

enum class Mode { Template, Model, Auto };

// 优先用文档自带的 model_roi：宿主把自己那份 setting.yml 的这一段并进来源文本，
// 就不必在 baseDir 边上放一个 setting.yml。真实的 StandardGap.yml 没有这个键，行为不变。
YAML::Node modelRoiSetting(const fs::path& baseDir, const YAML::Node& cfg) {
  const YAML::Node inline_ = child(cfg, "model_roi");
  if (present(inline_))
    return inline_;
  for (const fs::path& dir : {baseDir, baseDir.parent_path()}) {
    if (dir.empty())
      continue;
    const fs::path candidate = dir / "setting.yml";
    std::error_code ec;
    if (!fs::is_regular_file(candidate, ec))
      continue;
    try {
      const YAML::Node root = YAML::LoadFile(candidate.string());
      const YAML::Node section = child(root, "model_roi");
      if (present(section))
        return section;
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
    if (entry.path().extension() == ".onnx")
      hits.push_back(entry.path());
  }
  if (hits.size() != 1)
    return false;
  *out = hits.front();
  return true;
}

/// 一份配置解析完的样子。两种图都从它出发，所以「同一份配置 → 同一组参数」只写一遍。
struct Config {
  // YAML::Node 的赋值是「把值写进那个节点」而不是换绑，对缺席的键（zombie 节点）会抛 ——
  // 所以一律在构造时拷一次，之后只读。
  explicit Config(const YAML::Node& cfg)
      : common(child(cfg, "common_settings")), flush(child(cfg, "flush")), gap(child(cfg, "gap")),
        align(child(cfg, "align")), icp(child(align, "ICP")),
        robustness(child(align, "robustness")), trust(child(robustness, "trust_region")),
        filter(child(common, "filter")), guard(child(common, "camera_guard")),
        roll(child(common, "roll_anchored_crop")), band(child(gap, "center_band")),
        radius(child(gap, "radius")), weak(child(gap, "weak_fit")) {}

  const YAML::Node common, flush, gap, align, icp, robustness, trust, filter, guard, roll, band,
      radius, weak;
  std::string templateDir;
  std::string sampleId;
  bool useModel = false;
  bool withFallback = false;
  std::string modelPath;
  std::string refType;
  std::vector<Candidate> candidates;
  Roi top[4]{}; // flushBase / flushRef / gapLeft / gapRight
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
  if (segMode != "ROI")
    return badInput("只支持 seg_mode: ROI，这份配置是 '" + segMode + "'");
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
    return badInput("flush.ref_type 目前只支持 line end / selected point / nearest point，拿到 '" +
                    c.refType + "'");
  }
  if (leftType != "circle" || rightType != "circle")
    return badInput("gap 两侧目前只支持 circle");

  const YAML::Node setting = modelRoiSetting(baseDir, cfg);
  if (mode == Mode::Auto) {
    mode = present(setting) && boolOr(setting, "enabled", false) ? Mode::Model : Mode::Template;
  }
  if (mode == Mode::Model) {
    c.modelPath = textOr(setting, "model_path", "");
    if (c.modelPath.empty()) {
      fs::path found;
      if (uniqueOnnx(baseDir, &found))
        c.modelPath = found.string();
    }
    if (c.modelPath.empty()) {
      return badParam("模型模式要一个 ONNX：setting.yml 的 model_roi.model_path 没有，"
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
      cand.ownRois = present(rois);
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
    if (c.candidates.empty())
      return badInput("模板候选的框全是 0，没有一个能用");
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
    return badInput("gap.center_band.reference 是 flush_ref，但 flush.ref_type 不是 'line end'，"
                    "图里没有参考线可接");
  }
  for (int i = 0; i < 2; ++i) {
    const char* key = i == 0 ? "left_mode" : "right_mode";
    const std::string m = textOr(c.band, key, "always");
    if (bandTol[i] > 0 && m != "always" && m != "guard") {
      return badInput(std::string("gap.center_band.") + key +
                      " 只能是 always 或 guard，这份写的是 '" + m + "'");
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
  if (refType == "line end")
    return "line_end";
  if (refType == "selected point")
    return "selected_point";
  return "nearest_point";
}

struct Tail {
  std::string flushNode, gapNode, circlesNode, bundleNode;
  std::vector<std::string> modelPathBinds, inputDirBinds, templateDirBinds, circleOffsetBinds;
};
struct BoundRef {
  Ref value, context;
};
struct ProfileRef {
  Ref value, provenance;
};
void wire(Builder& g, const Ref& source, const std::string& target, const std::string& port) {
  if (!source.empty())
    g.edge(source.node, source.port, target, port);
}
void wireBound(Builder& g, const BoundRef& source, const std::string& target,
               const std::string& value, const std::string& context) {
  wire(g, source.value, target, value);
  wire(g, source.context, target, context);
}
void wireProfile(Builder& g, const ProfileRef& source, const std::string& target,
                 const std::string& profile = "profile",
                 const std::string& provenance = "provenance") {
  wire(g, source.value, target, profile);
  wire(g, source.provenance, target, provenance);
}
Ref fallbackOf(Builder& g, const std::string& id, const Ref& a, const Ref& b, int column, int row,
               const char* title) {
  const auto node = g.node(id, "flow.fallback", nullptr, column, row, title);
  wire(g, a, node, "a");
  wire(g, b, node, "b");
  return {node, "out"};
}
std::string splitFeature(Builder& g, const Ref& feature, const std::string& id, const char* op,
                         int column, int row) {
  const auto node = g.node(id, op, nullptr, column, row, "拆选中的原子特征");
  wire(g, feature, node, "feature");
  return node;
}
Ref makeLine(Builder& g, const std::string& fit, const std::string& id, int column, int row) {
  const auto node = g.node(id, "gap.make_line_feature", nullptr, column, row, "组完整直线特征");
  for (const auto* port : {"profile", "line", "lineContext", "innerEnd", "pointContext", "inliers",
                           "weightedInliers", "quality", "provenance"})
    g.edge(fit, port, node, port);
  return {node, "feature"};
}
Ref makeReference(Builder& g, const std::string& source, const std::string& id,
                  const std::string& method, int column, int row) {
  const auto node = g.node(id, "gap.make_reference_feature", {{"method", method}}, column, row,
                           "组完整参考点特征");
  for (const auto* port : {"profile", "pointContext", "provenance"})
    g.edge(source, port, node, port);
  g.edge(source, method == "line_end" ? "innerEnd" : "point", node, "point");
  g.edge(source, method == "line_end" ? "indexMap" : "selection", node, "selection");
  if (method == "line_end")
    for (const auto* port : {"line", "lineContext", "quality"})
      g.edge(source, port, node, port);
  return {node, "feature"};
}
struct Located {
  Ref scan, rois, location;
};
Located located(Builder& g, const Ref& scan, const Ref& rois, const std::string& prefix, int column,
                int row) {
  const auto make =
      g.node(prefix + "n_location", "gap.make_location", nullptr, column, row, "组原子定位结果");
  wire(g, scan, make, "scan");
  wire(g, rois, make, "rois");
  return {scan, rois, {make, "location"}};
}
Located fallbackLocation(Builder& g, const Located& model, const Located& backup) {
  const auto selected = fallbackOf(g, "n_fb_location", model.location, backup.location, 7, 6,
                                   "整体回退定位场景与区域");
  const auto split =
      g.node("n_split_location", "gap.split_location", nullptr, 8, 6, "拆选中的定位结果");
  wire(g, selected, split, "location");
  return {{split, "scan"}, {split, "rois"}, selected};
}
struct SceneFields {
  Ref scan, sensorPrimary, sensorSecondary;
  ProfileRef primary, secondary, merged;
  Ref sourceInfo, guardQuality;
};
SceneFields splitScan(Builder& g, const Ref& scan, const std::string& id, int column, int row) {
  const auto node = g.node(id, "gap.split_scan_pair", nullptr, column, row, "拆原生场景");
  wire(g, scan, node, "scan");
  return {scan,
          {node, "sensorPrimary"},
          {node, "sensorSecondary"},
          {{node, "primary"}, {node, "primaryProvenance"}},
          {{node, "secondary"}, {node, "secondaryProvenance"}},
          {{node, "merged"}, {node, "mergedProvenance"}},
          {},
          {}};
}
struct RoiFields {
  std::array<BoundRef, 4> boxes;
  BoundRef seam;
  Ref rois, info;
};
RoiFields splitRois(Builder& g, const Ref& rois, const std::string& id, int column, int row) {
  const auto node = g.node(id, "gap.split_roi_set", nullptr, column, row, "拆原生角色框");
  wire(g, rois, node, "rois");
  RoiFields out;
  out.rois = rois;
  out.info = {node, "info"};
  const char* names[4] = {"datum", "target", "seamLeft", "seamRight"};
  for (std::size_t i = 0; i < 4; ++i)
    out.boxes[i] = {{node, names[i]}, {node, std::string(names[i]) + "Context"}};
  out.seam = {{node, "seam"}, {node, "seamContext"}};
  return out;
}
Ref updateRois(Builder& g, const RoiFields& fields, const BoundRef& overall, const Ref& cropStatus,
               const std::string& id, int column, int row) {
  const auto node = g.node(id, "gap.make_roi_set", {{"metadataMode", "update_metadata"}}, column,
                           row, "补实际窗口和裁剪状态");
  const char* names[4] = {"datum", "target", "seamLeft", "seamRight"};
  for (std::size_t i = 0; i < 4; ++i)
    wireBound(g, fields.boxes[i], node, names[i], std::string(names[i]) + "Context");
  wire(g, fields.info, node, "info");
  wireBound(g, overall, node, "overall", "overallContext");
  wire(g, cropStatus, node, "cropStatus");
  return {node, "rois"};
}
ProfileRef crop(Builder& g, const ProfileRef& profile, const BoundRef& box, const std::string& id,
                int column, int row, bool allowEmpty = false) {
  const auto node = g.node(
      id, "profile.crop_box",
      {{"bounds", "open"}, {"mode", "compact"}, {"emptyPolicy", allowEmpty ? "allow" : "error"}},
      column, row, "原生轮廓开区间裁剪");
  wireProfile(g, profile, node);
  wireBound(g, box, node, "box", "boxContext");
  return {{node, "profile"}, {node, "provenance"}};
}
std::string fitLine(Builder& g, const ProfileRef& profile, const BoundRef& box,
                    const BoundRef& toward, const BoundRef& reference, const nlohmann::json& params,
                    const std::string& id, int column, int row) {
  const auto node = g.node(id, "profile.fit_line_robust", params, column, row, "公共轮廓直线拟合");
  wireProfile(g, profile, node);
  wireBound(g, box, node, "box", "boxContext");
  wireBound(g, toward, node, "toward", "towardContext");
  wireBound(g, reference, node, "refLine", "refLineContext");
  return node;
}
nlohmann::json lineParams(const Config& c, const char* endpoints, bool direction = false) {
  nlohmann::json p{{"distanceThreshold", c.lineDist / 1000.0},
                   {"segmentPoints", c.segmentPoints},
                   {"endpoints", endpoints},
                   {"lineType", "fit"},
                   {"minInliers", 0},
                   {"dirMode", "free"}};
  if (direction && c.wantDatum) {
    const auto cfg = child(c.flush, "base_direction");
    p["dirMode"] = c.datumMode;
    p["dirNominalDeg"] = numberOr(cfg, "nominal_deg", 0);
    p["dirTolDeg"] = numberOr(cfg, "tolerance_deg", 12);
  }
  return p;
}
nlohmann::json fileParams() {
  return {{"dir", "."},
          {"layout", "sensor"},
          {"unit", "m"},
          {"fileFrame", "gap.sensor-files"},
          {"sensorFrame", "gap.sensor"},
          {"handedness", "right"}};
}
SceneFields readScene(Builder& g, const Config& c, Tail& tail, bool fine) {
  const bool guard = boolOr(c.guard, "enabled", false);
  Ref p, s, sourceInfo, guardQuality;
  if (fine || guard) {
    const auto load =
        g.node("n_load", "gap.load_profile_pair", fileParams(), 0, 1, "读取原始采集槽");
    tail.inputDirBinds.push_back(load + ".dir");
    p = {load, "primary"};
    s = {load, "secondary"};
    sourceInfo = {load, "sourceInfo"};
    if (guard) {
      const auto gate =
          g.node("n_camera_guard", "gap.camera_guard", guardParams(c), 0, 3, "双相机一致性检查");
      wire(g, p, gate, "primary");
      wire(g, s, gate, "secondary");
      p = {gate, "primary"};
      s = {gate, "secondary"};
      guardQuality = {gate, "quality"};
    }
  }
  if (!fine) {
    auto params = fileParams();
    params["measurementFrame"] = "gap.measurement";
    params["removeOutliers"] = c.usingRemoval;
    params["outlierRadiusMm"] = numberOr(c.filter, "filter_radius", .4);
    params["outlierNeighbors"] = intOr(c.filter, "filter_neighbors", 5);
    if (guard)
      params["source"] = "inputs";
    else
      tail.inputDirBinds.push_back("n_scan.dir");
    const auto node = g.node("n_scan", "gap.read_scan", params, 1, 1, "原生场景读入");
    wire(g, p, node, "primary");
    wire(g, s, node, "secondary");
    wire(g, sourceInfo, node, "sourceInfo");
    wire(g, guardQuality, node, "guardQuality");
    auto fields = splitScan(g, {node, "scan"}, "n_raw_fields", 2, 2);
    fields.sourceInfo = sourceInfo;
    fields.guardQuality = guardQuality;
    return fields;
  }
  const auto frameP =
      g.node("n_frame_p", "profile.remap_axes",
             {{"axes", "XZY"}, {"signs", {1, 1, 1}}, {"targetFrame", "gap.measurement"}}, 1, 0,
             "明确换轴primary");
  const auto frameS =
      g.node("n_frame_s", "profile.remap_axes",
             {{"axes", "XZY"}, {"signs", {1, 1, 1}}, {"targetFrame", "gap.measurement"}}, 1, 2,
             "明确换轴secondary");
  wire(g, p, frameP, "profile");
  wire(g, s, frameS, "profile");
  const auto finiteP = g.node("n_drop_p", "profile.filter_validity", {{"mode", "compact"}}, 2, 0,
                              "压缩有效primary槽");
  const auto finiteS = g.node("n_drop_s", "profile.filter_validity", {{"mode", "compact"}}, 2, 2,
                              "压缩有效secondary槽");
  g.edge(frameP, "profile", finiteP, "profile");
  g.edge(frameS, "profile", finiteS, "profile");
  const auto merge =
      g.node("n_merge", "profile.merge",
             {{"roleA", "secondary"}, {"roleB", "primary"}, {"channelPolicy", "intersection"}}, 3,
             3, "按真实双源区间合并");
  g.edge(finiteS, "profile", merge, "a");
  g.edge(finiteP, "profile", merge, "b");
  g.edge(finiteS, "provenance", merge, "aProvenance");
  g.edge(finiteP, "provenance", merge, "bProvenance");
  ProfileRef merged{{merge, "profile"}, {merge, "provenance"}};
  if (c.usingRemoval) {
    const auto node = g.node("n_filter", "profile.radius_outlier",
                             {{"radius", numberOr(c.filter, "filter_radius", .4) / 1000.0},
                              {"minNeighbors", intOr(c.filter, "filter_neighbors", 5)}},
                             4, 3, "合并轮廓原槽去噪");
    wireProfile(g, merged, node);
    merged = {{node, "profile"}, {node, "provenance"}};
  }
  const auto set =
      g.node("n_raw_scan", "gap.make_scan_pair", {{"stage", "raw"}}, 4, 1, "组原生采集与量测场景");
  wire(g, p, set, "sensorPrimary");
  wire(g, s, set, "sensorSecondary");
  g.edge(frameP, "profile", set, "primary");
  g.edge(frameS, "profile", set, "secondary");
  wireProfile(g, merged, set, "merged", "mergedProvenance");
  wire(g, sourceInfo, set, "sourceInfo");
  wire(g, guardQuality, set, "guardQuality");
  auto fields = splitScan(g, {set, "scan"}, "n_raw_fields", 5, 1);
  fields.sourceInfo = sourceInfo;
  fields.guardQuality = guardQuality;
  return fields;
}
std::string datumRole(const Config& c) {
  const auto role = c.heightKind == "flush_ref" ? "target" : "datum";
  const bool right = c.heightKind == "flush_ref" ? !c.datumRight : c.datumRight;
  return c.anchorKind == (right ? "gap_right" : "gap_left") &&
                 c.datumWindowSide == (right ? "right" : "left")
             ? role
             : "";
}
BoundRef fineDatum(Builder& g, const Config& c, const std::string& prefix, const RoiFields& rois,
                   const ProfileRef& cloud, int row) {
  const auto cfg = child(c.flush, "base_direction");
  const auto& anchor = rois.boxes[c.anchorKind == "gap_left" ? 2 : 3];
  const auto& height = rois.boxes[c.heightKind == "flush_base" ? 0 : 1];
  const auto win = g.node(prefix + "n_datum_box", "gap.datum_window",
                          {{"side", c.datumWindowSide},
                           {"startMm", numberOr(cfg, "start_mm", .6)},
                           {"lengthMm", numberOr(cfg, "length_mm", 13.4)},
                           {"heightMm", numberOr(cfg, "height_mm", 2.5)}},
                          8, row, "方向基准窗口");
  wireBound(g, anchor, win, "anchor", "anchorContext");
  wireBound(g, height, win, "heightAnchor", "heightContext");
  const BoundRef window{{win, "box"}, {win, "context"}};
  const auto selected = crop(g, cloud, window, prefix + "n_crop_datum", 9, row);
  auto params = lineParams(c, "inlier_ends");
  params["distanceThreshold"] = numberOr(cfg, "fit_distance", .35) / 1000.0;
  params["segmentPoints"] = 0;
  params["minInliers"] = intOr(cfg, "min_inliers", 60);
  const auto fit =
      fitLine(g, selected, window, anchor, {}, params, prefix + "n_fit_datum", 10, row);
  return {{fit, "line"}, {fit, "lineContext"}};
}
struct FlushChain {
  BoundRef line, point, refLine;
  Ref qualityBase, qualityRef, lineFeature, referenceFeature;
};
FlushChain splitFlush(Builder& g, const Ref& line, const Ref& reference, const std::string& prefix,
                      int row) {
  const auto l =
      splitFeature(g, line, prefix + "n_split_line_feature", "gap.split_line_feature", 10, row);
  const auto p = splitFeature(g, reference, prefix + "n_split_reference_feature",
                              "gap.split_reference_feature", 10, row + 1);
  return {{{l, "line"}, {l, "lineContext"}},
          {{p, "point"}, {p, "pointContext"}},
          {{p, "line"}, {p, "lineContext"}},
          {l, "quality"},
          {p, "quality"},
          line,
          reference};
}
FlushChain fineFlush(Builder& g, const Config& c, const std::string& prefix, const Located& at,
                     const char* endpoints, int row) {
  const auto fields = splitScan(g, at.scan, prefix + "n_measure_scan", 7, row + 2);
  const auto rois = splitRois(g, at.rois, prefix + "n_measure_rois", 7, row + 3);
  const auto base = crop(g, fields.merged, rois.boxes[0], prefix + "n_crop_flushBase", 8, row);
  ProfileRef reference;
  if (c.refType != "selected point")
    reference = crop(g, fields.merged, rois.boxes[1], prefix + "n_crop_flushRef", 8, row + 1);
  const auto datum =
      c.wantDatum ? fineDatum(g, c, prefix, rois, fields.merged, row + 4) : BoundRef{};
  const auto fitted = fitLine(g, base, rois.boxes[0], rois.seam, datum,
                              lineParams(c, endpoints, true), prefix + "n_fit_base", 9, row);
  const auto line = makeLine(g, fitted, prefix + "n_line_feature", 10, row);
  Ref point;
  if (c.refType == "line end") {
    const auto fit = fitLine(g, reference, rois.boxes[1], rois.seam, {}, lineParams(c, endpoints),
                             prefix + "n_fit_ref", 9, row + 1);
    point = makeReference(g, fit, prefix + "n_reference_feature", "line_end", 10, row + 1);
  } else if (c.refType == "selected point") {
    const auto anchor = g.node(prefix + "n_reference_anchor", "geometry.box_anchor",
                               {{"anchor", "minimum"}}, 8, row + 1, "角色框最小角");
    wire(g, rois.boxes[1].value, anchor, "box");
    const auto bind = g.node(prefix + "n_reference_anchor_context", "profile.bind_geometry",
                             {{"frame", "gap.measurement"}, {"plane", "XY"}}, 9, row + 1,
                             "锚点绑定实际轮廓和角色框来源");
    wireProfile(g, fields.merged, bind);
    g.edge(anchor, "point", bind, "geometry");
    wireBound(g, rois.boxes[1], bind, "referenceGeometry", "referenceContext");
    const auto nearest = g.node(prefix + "n_sel_ref", "profile.nearest_sample", nullptr, 10,
                                row + 1, "在完整轮廓选择最接近框最小角的原槽");
    wireProfile(g, fields.merged, nearest);
    g.edge(bind, "geometry", nearest, "anchor");
    g.edge(bind, "context", nearest, "anchorContext");
    point =
        makeReference(g, nearest, prefix + "n_reference_feature", "selected_point", 11, row + 1);
  } else {
    const auto nearest = g.node(prefix + "n_near_ref", "profile.nearest_to_line", nullptr, 10,
                                row + 1, "在参考ROI选择无限线最近样本");
    wireProfile(g, reference, nearest);
    g.edge(fitted, "line", nearest, "line");
    g.edge(fitted, "lineContext", nearest, "lineContext");
    point = makeReference(g, nearest, prefix + "n_reference_feature", "nearest_point", 11, row + 1);
  }
  if (c.withFallback)
    return {{}, {}, {}, {}, {}, line, point};
  return splitFlush(g, line, point, prefix, row);
}
BoundRef blockDatum(Builder& g, const Config& c, const std::string& prefix, const Located& at,
                    int row) {
  const auto role = datumRole(c);
  if (role.empty()) {
    const auto fields = splitScan(g, at.scan, prefix + "n_datum_scan", 6, row + 2);
    const auto rois = splitRois(g, at.rois, prefix + "n_datum_rois", 6, row + 1);
    return fineDatum(g, c, prefix, rois, fields.merged, row);
  }
  const auto cfg = child(c.flush, "base_direction");
  const auto node = g.node(prefix + "n_datum", "gap.datum_direction",
                           {{"role", role},
                            {"startMm", numberOr(cfg, "start_mm", .6)},
                            {"lengthMm", numberOr(cfg, "length_mm", 13.4)},
                            {"heightMm", numberOr(cfg, "height_mm", 2.5)},
                            {"distThresh", numberOr(cfg, "fit_distance", .35)},
                            {"minInliers", intOr(cfg, "min_inliers", 60)}},
                           5, row, "角色外长面方向基准");
  wire(g, at.scan, node, "scan");
  wire(g, at.rois, node, "rois");
  return {{node, "line"}, {node, "lineContext"}};
}
FlushChain blockFlush(Builder& g, const Config& c, const std::string& prefix, const Located& at,
                      const char* endpoints, int row) {
  auto params = lineParams(c, endpoints, true);
  params.erase("distanceThreshold");
  params["distThresh"] = c.lineDist;
  const auto line = g.node(prefix + "n_line", "gap.role_line", params, 6, row, "角色基准线");
  wire(g, at.scan, line, "scan");
  wire(g, at.rois, line, "rois");
  if (c.wantDatum)
    wireBound(g, blockDatum(g, c, prefix, at, row + 3), line, "refLine", "refLineContext");
  nlohmann::json rp{{"method", refMethod(c.refType)}};
  if (c.refType == "line end") {
    rp["distThresh"] = c.lineDist;
    rp["segmentPoints"] = c.segmentPoints;
    rp["endpoints"] = endpoints;
    rp["lineType"] = "fit";
    rp["minInliers"] = 0;
  }
  const auto point = g.node(prefix + "n_ref_point", "gap.ref_point", rp, 6, row + 1, "角色参考点");
  wire(g, at.scan, point, "scan");
  wire(g, at.rois, point, "rois");
  if (c.refType == "nearest point") {
    g.edge(line, "line", point, "baseLine");
    g.edge(line, "lineContext", point, "baseLineContext");
  }
  if (c.withFallback)
    return {{}, {}, {}, {}, {}, {line, "feature"}, {point, "feature"}};
  return splitFlush(g, {line, "feature"}, {point, "feature"}, prefix, row);
}
Located templateBranch(Builder& g, const Config& c, const SceneFields& raw, Tail& tail,
                       const std::string& prefix, bool fine, int column, int row) {
  if (!fine) {
    auto p = icpParams(c);
    p["templateDir"] = c.templateDir;
    p["overallRoi"] = overallRoiOf(c);
    p["overallMode"] = textOr(c.common, "overall_roi_mode", "fixed");
    p["overallCamera"] = c.usingCamera;
    for (std::size_t i = 0; i < c.candidates.size(); ++i) {
      const auto key = "template" + std::to_string(i + 1);
      const auto& v = c.candidates[i];
      if (i)
        p[key + "Enabled"] = true;
      p[key + "Id"] = v.id;
      p[key + "Left"] = v.left;
      p[key + "Right"] = v.right;
      p[key + "DatumRoi"] = v.flushBase;
      p[key + "TargetRoi"] = v.flushRef;
      p[key + "SeamLeftRoi"] = v.gapLeft;
      p[key + "SeamRightRoi"] = v.gapRight;
      p[key + "RoisFrom"] = v.ownRois ? "template" : "config";
    }
    const auto node =
        g.node(prefix + "n_locate", "gap.locate_template", p, column, row, "模板定位");
    wire(g, raw.scan, node, "scan");
    tail.templateDirBinds.push_back(node + ".templateDir");
    return located(g, {node, "scan"}, {node, "rois"}, prefix, column + 1, row);
  }
  const auto overall = g.node(prefix + "n_overall", "gap.overall_roi",
                              {{"roi", overallRoiOf(c)},
                               {"mode", textOr(c.common, "overall_roi_mode", "fixed")},
                               {"usingCamera", c.usingCamera}},
                              column, row, "实际整体窗口");
  wireProfile(g, raw.primary, overall, "primary", "primaryProvenance");
  wireProfile(g, raw.secondary, overall, "secondary", "secondaryProvenance");
  const BoundRef window{{overall, "box"}, {overall, "context"}};
  const auto primary = crop(g, raw.primary, window, prefix + "n_crop_p", column + 1, row - 1, true),
             secondary =
                 crop(g, raw.secondary, window, prefix + "n_crop_s", column + 1, row + 1, true),
             merged = crop(g, raw.merged, window, prefix + "n_crop_m", column + 1, row + 3, true);
  const auto scan = g.node(prefix + "n_scan_set", "gap.make_scan_pair", {{"stage", "processed"}},
                           column + 2, row, "组整体框裁剪场景");
  wire(g, raw.sensorPrimary, scan, "sensorPrimary");
  wire(g, raw.sensorSecondary, scan, "sensorSecondary");
  wireProfile(g, primary, scan, "primary", "primaryProvenance");
  wireProfile(g, secondary, scan, "secondary", "secondaryProvenance");
  wireProfile(g, merged, scan, "merged", "mergedProvenance");
  wire(g, raw.sourceInfo, scan, "sourceInfo");
  wire(g, raw.guardQuality, scan, "guardQuality");
  std::vector<std::string> alignments;
  for (std::size_t i = 0; i < c.candidates.size(); ++i) {
    const auto& v = c.candidates[i];
    const auto id = safeId(v.id);
    const auto load = g.node(prefix + "n_tpl_" + id, "gap.load_template",
                             {{"dir", c.templateDir}, {"left", v.left}, {"right", v.right}},
                             column + 3, row + 2 * static_cast<int>(i), "模板 " + v.id);
    tail.templateDirBinds.push_back(load + ".dir");
    auto p = icpParams(c);
    p["templateId"] = v.id;
    p["order"] = i;
    p["roiFlushBase"] = v.flushBase;
    p["roiFlushRef"] = v.flushRef;
    p["roiGapLeft"] = v.gapLeft;
    p["roiGapRight"] = v.gapRight;
    p["roisFrom"] = v.ownRois ? "template" : "config";
    const auto align = g.node(prefix + "n_align_" + id, "gap.align_template", p, column + 4,
                              row + 2 * static_cast<int>(i), "ICP " + v.id);
    g.edge(scan, "scan", align, "scan");
    g.edge(load, "templates", align, "templates");
    alignments.push_back(align);
  }
  const auto chosen = g.node(prefix + "n_select", "gap.select_alignment",
                             {{"minScore", c.minScore}}, column + 5, row, "优选同次扫描模板");
  const char* ports[4] = {"a", "b", "c", "d"};
  for (std::size_t i = 0; i < alignments.size(); ++i)
    g.edge(alignments[i], "alignment", chosen, ports[i]);
  const auto boxes = g.node(prefix + "n_rois", "gap.business_rois", {{"datumSide", "auto"}},
                            column + 6, row, "实际变换业务区域");
  g.edge(chosen, "alignment", boxes, "alignment");
  const auto fields =
      splitRois(g, {boxes, "rois"}, prefix + "n_template_roi_fields", column + 7, row);
  const auto rois = updateRois(g, fields, window, {}, prefix + "n_roi_set", column + 8, row);
  return located(g, {scan, "scan"}, rois, prefix, column + 9, row);
}
Located modelBranch(Builder& g, const Config& c, const SceneFields& raw, Tail& tail, bool fine) {
  if (!fine) {
    const auto roll = rollParams(c);
    nlohmann::json p{{"cropEnabled", roll["enabled"]},
                     {"cropHalfWidth", roll["halfWidth"]},
                     {"cropHalfHeight", roll["halfHeight"]},
                     {"cropMaxRollBoxHeight", roll["maxRollBoxHeight"]},
                     {"cropMinPointsKept", roll["minPointsKept"]},
                     {"cropUsingCamera", roll["usingCamera"]}};
    const auto node = g.node("n_model", "gap.locate_model", p, 3, 1, "模型定位");
    wire(g, raw.scan, node, "scan");
    tail.modelPathBinds.push_back(node + ".modelPath");
    return located(g, {node, "scan"}, {node, "rois"}, "", 4, 1);
  }
  const auto tensor =
      g.node("n_tensor", "gap.profile_tensor", nullptr, 1, 6, "保原始1280槽的特征张量");
  wire(g, raw.sensorPrimary, tensor, "primary");
  wire(g, raw.sensorSecondary, tensor, "secondary");
  const auto infer = g.node("n_infer", "gap.infer_profile", nullptr, 2, 6, "显式轮廓Tensor推理");
  g.edge(tensor, "tensor", infer, "input");
  tail.modelPathBinds.push_back(infer + ".modelPath");
  const auto labels =
      g.node("n_seg", "gap.labels_from_logits", nullptr, 3, 6, "绑定actual sensor与特征输入的标签");
  g.edge(infer, "output", labels, "tensor");
  g.edge(tensor, "tensor", labels, "modelInput");
  wire(g, raw.sensorPrimary, labels, "primary");
  wire(g, raw.sensorSecondary, labels, "secondary");
  const auto color =
      g.node("n_labels_p", "gap.labels_to_cloud", {{"row", "primary"}}, 4, 7, "原槽标签诊断着色");
  wire(g, raw.scan, color, "scan");
  g.edge(labels, "labels", color, "labels");
  const auto boxes =
      g.node("n_rois", "gap.roi_from_labels", nullptr, 4, 6, "从完整标签提取模型四框");
  wire(g, raw.scan, boxes, "scan");
  g.edge(labels, "labels", boxes, "labels");
  g.edge(color, "cloud", boxes, "backdrop");
  const auto finiteP = g.node("n_model_valid_primary", "profile.filter_validity",
                              {{"mode", "compact"}}, 4, 9, "保留primary有效原槽与来源");
  const auto finiteS = g.node("n_model_valid_secondary", "profile.filter_validity",
                              {{"mode", "compact"}}, 4, 10, "保留secondary有效原槽与来源");
  wireProfile(g, raw.primary, finiteP);
  wireProfile(g, raw.secondary, finiteS);
  const auto prepared = g.node("n_model_processed", "gap.make_scan_pair", {{"stage", "processed"}},
                               5, 9, "组模型测量前处理场景");
  wire(g, raw.sensorPrimary, prepared, "sensorPrimary");
  wire(g, raw.sensorSecondary, prepared, "sensorSecondary");
  g.edge(finiteP, "profile", prepared, "primary");
  g.edge(finiteS, "profile", prepared, "secondary");
  g.edge(finiteP, "provenance", prepared, "primaryProvenance");
  g.edge(finiteS, "provenance", prepared, "secondaryProvenance");
  wireProfile(g, raw.merged, prepared, "merged", "mergedProvenance");
  wire(g, raw.sourceInfo, prepared, "sourceInfo");
  wire(g, raw.guardQuality, prepared, "guardQuality");
  const auto roll =
      g.node("n_roll", "gap.roll_anchored_crop", rollParams(c), 5, 6, "保已有merged的随动窗口");
  g.edge(prepared, "scan", roll, "scan");
  for (const auto* name : {"gapLeft", "gapRight"}) {
    g.edge(boxes, name, roll, name);
    g.edge(boxes, std::string(name) + "Context", roll, std::string(name) + "Context");
  }
  const auto fields = splitRois(g, {boxes, "rois"}, "n_model_roi_fields", 6, 6);
  const auto rois = updateRois(g, fields, {{roll, "window"}, {roll, "windowContext"}},
                               {roll, "status"}, "n_roi_set", 7, 6);
  return located(g, {roll, "scan"}, rois, "", 8, 6);
}

Ref circleFeature(Builder& g, const Config& c, const Located& at, const FlushChain& flush,
                  const std::string& prefix, bool fine, Tail& tail, int row) {
  const auto node = g.node(prefix + "n_circles", fine ? "gap.fit_gap_circles" : "gap.seam_circles",
                           circleParams(c), 11, row, "双迹线与双相机业务圆拟合");
  tail.circleOffsetBinds.push_back(node + ".offset");
  if (fine) {
    const auto scan = splitScan(g, at.scan, prefix + "n_circle_scan", 9, row + 2);
    const auto boxes = splitRois(g, at.rois, prefix + "n_circle_rois", 9, row + 3);
    wireProfile(g, scan.primary, node, "primary", "primaryProvenance");
    wireProfile(g, scan.secondary, node, "secondary", "secondaryProvenance");
    wireProfile(g, scan.merged, node, "merged", "mergedProvenance");
    wireBound(g, boxes.boxes[2], node, "boxLeft", "boxLeftContext");
    wireBound(g, boxes.boxes[3], node, "boxRight", "boxRightContext");
  } else {
    wire(g, at.scan, node, "scan");
    wire(g, at.rois, node, "rois");
  }
  if (c.wantBand)
    wireBound(g, c.bandRef == "flush_ref" ? flush.refLine : flush.line, node, "refLine",
              "refLineContext");
  if (!fine)
    return {node, "feature"};
  const auto make = g.node(prefix + "n_circle_feature", "gap.make_circle_pair", nullptr, 12, row,
                           "组完整双圆特征");
  for (const auto* port :
       {"left", "right", "leftContext", "rightContext", "leftProfile", "rightProfile",
        "leftProvenance", "rightProvenance", "leftInliers", "rightInliers", "quality"})
    g.edge(node, port, make, port);
  return {make, "feature"};
}
void judges(Builder& g, const Config& c, const Tail& tail) {
  const YAML::Node sections[2] = {c.flush, c.gap};
  const std::string sources[2] = {tail.flushNode, tail.gapNode};
  for (int i = 0; i < 2; ++i) {
    const auto limits = child(sections[i], "tolerances");
    const auto node = g.node("n_judge_" + std::to_string(i), "gap.judge",
                             {{"nominal", numberOr(limits, "nominal", 0)},
                              {"upper", numberOr(limits, "up_deviation", 0)},
                              {"lower", numberOr(limits, "low_deviation", 0)}},
                             15, i * 2, i == 0 ? "判定段差" : "判定间隙");
    g.edge(sources[i], "value", node, "value");
  }
}
std::string finish(Builder& g, const Config& c, const Tail& tail, bool fine) {
  nlohmann::json doc{
      {"schemaVersion", 1},
      {"id", safeId("gap_" + c.sampleId)},
      {"name",
       c.sampleId + " · StandardGap" + (c.useModel ? " · 模型" : "") + (fine ? " · 细粒度" : "")},
      {"meta", {{"app", "StandardGap.yml importer"}, {"style", fine ? "fine" : "blocks"}}},
      {"nodes", g.nodes},
      {"edges", g.edges}};
  if (!c.notes.empty())
    doc["meta"]["importNotes"] = c.notes;
  doc["outputs"] = {
      {"gap", {{"node", tail.gapNode}, {"port", "value"}, {"label", "间隙"}}},
      {"flush", {{"node", tail.flushNode}, {"port", "value"}, {"label", "段差"}}},
      {"bundle", {{"node", tail.bundleNode}, {"port", "bundle"}, {"label", "结果汇总"}}}};
  auto offsets = tail.circleOffsetBinds;
  offsets.insert(offsets.begin(), tail.gapNode + ".offsetMm");
  doc["params"] = {{"gapOffset",
                    {{"type", "float"},
                     {"default", numberOr(c.gap, "offset", 0)},
                     {"binds", offsets},
                     {"doc", "间隙偏置（毫米），同步读数与实际圆候选评分。"}}},
                   {"flushOffset",
                    {{"type", "float"},
                     {"default", numberOr(c.flush, "offset", 0)},
                     {"binds", {tail.flushNode + ".offsetMm"}},
                     {"doc", "段差补偿（毫米）。"}}},
                   {"flushGain",
                    {{"type", "float"},
                     {"default", numberOr(c.flush, "scale", 1)},
                     {"binds", {tail.flushNode + ".gain"}},
                     {"doc", "无量纲段差倍率。"}}},
                   {"inputDir",
                    {{"type", "path"},
                     {"default", "."},
                     {"binds", tail.inputDirBinds},
                     {"doc", "已声明sensor XZ米制布局的双路PCD目录。"}}},
                   {"graphSha256",
                    {{"type", "string"},
                     {"default", ""},
                     {"binds", {tail.bundleNode + ".graphSha256"}},
                     {"doc", "宿主声明的图文件sha256。"}}}};
  if (!tail.templateDirBinds.empty())
    doc["params"]["templateDir"] = {{"type", "path"},
                                    {"default", c.templateDir},
                                    {"binds", tail.templateDirBinds},
                                    {"doc", "实际启用模板的公共目录。"}};
  if (c.useModel)
    doc["params"]["modelPath"] = {{"type", "path"},
                                  {"default", c.modelPath},
                                  {"binds", tail.modelPathBinds},
                                  {"doc", "实际推理节点使用的ONNX模型。"}};
  for (const auto& parameter : doc["params"])
    for (const auto& binding : parameter["binds"]) {
      const auto path = binding.get<std::string>();
      const auto dot = path.find('.');
      for (auto& node : doc["nodes"])
        if (node["id"] == path.substr(0, dot) && node.contains("params"))
          node["params"].erase(path.substr(dot + 1));
    }
  return doc.dump(2);
}
Status buildPipeline(const Config& c, bool fine, std::string& graphJson) {
  Builder g;
  Tail tail;
  const auto raw = readScene(g, c, tail, fine);
  Located at, byModel, byTemplate;
  FlushChain flush, modelFlush, templateFlush;
  Ref circles;
  if (!c.useModel) {
    at = templateBranch(g, c, raw, tail, "", fine, 3, 1);
    flush = fine ? fineFlush(g, c, "", at, "roi_intersection", 0)
                 : blockFlush(g, c, "", at, "roi_intersection", 0);
    circles = circleFeature(g, c, at, flush, "", fine, tail, 3);
  } else {
    byModel = modelBranch(g, c, raw, tail, fine);
    modelFlush = fine ? fineFlush(g, c, "", byModel, "inlier_ends", 0)
                      : blockFlush(g, c, "", byModel, "inlier_ends", 0);
    if (!c.withFallback) {
      at = byModel;
      flush = modelFlush;
    } else {
      byTemplate = templateBranch(g, c, raw, tail, "b_", fine, 3, 10);
      templateFlush = fine ? fineFlush(g, c, "b_", byTemplate, "roi_intersection", 10)
                           : blockFlush(g, c, "b_", byTemplate, "roi_intersection", 10);
      at = fallbackLocation(g, byModel, byTemplate);
      const auto line = fallbackOf(g, "n_fb_line", modelFlush.lineFeature,
                                   templateFlush.lineFeature, 11, 5, "原子回退直线与来源质量");
      const auto reference =
          fallbackOf(g, "n_fb_reference", modelFlush.referenceFeature,
                     templateFlush.referenceFeature, 11, 6, "原子回退参考点与来源质量");
      flush = splitFlush(g, line, reference, "n_selected_", 5);
    }
    circles = circleFeature(g, c, at, flush, "", fine, tail, 3);
  }
  tail.circlesNode = splitFeature(g, circles, "n_selected_circles", "gap.split_circle_pair", 13, 3);
  tail.flushNode = g.node("n_flush", "metrology.flush_point_line",
                          {{"offsetMm", numberOr(c.flush, "offset", 0)},
                           {"gain", numberOr(c.flush, "scale", 1)},
                           {"signed", boolOr(c.flush, "signed", true)},
                           {"signConvention", "canonical_axis"},
                           {"outputUnit", "mm"}},
                          14, 0, "公共段差测量");
  wireBound(g, flush.line, tail.flushNode, "baseLine", "baseLineContext");
  wireBound(g, flush.point, tail.flushNode, "refPoint", "refPointContext");
  tail.gapNode = g.node("n_gap", "metrology.circle_gap",
                        {{"definition", c.definition == "A" ? "datum_tangents" : "center_line"},
                         {"offsetMm", numberOr(c.gap, "offset", 0)},
                         {"outputUnit", "mm"}},
                        14, 3, "公共间隙测量");
  for (const auto* side : {"left", "right"}) {
    g.edge(tail.circlesNode, side, tail.gapNode, side);
    g.edge(tail.circlesNode, std::string(side) + "Context", tail.gapNode,
           std::string(side) + "Context");
  }
  if (c.definition == "A") {
    g.edge(tail.flushNode, "baseLine", tail.gapNode, "baseLine");
    g.edge(tail.flushNode, "baseLineContext", tail.gapNode, "baseLineContext");
  }
  judges(g, c, tail);
  nlohmann::json bundleParams;
  if (!c.useModel && boolOr(c.roll, "enabled", false))
    bundleParams["cropStatus"] = "skipped:no_model_roi";
  tail.bundleNode =
      g.node("n_bundle", "gap.result_bundle", bundleParams, 15, 5, "原生场景结果汇总");
  g.edge(tail.gapNode, "value", tail.bundleNode, "gap");
  g.edge(tail.flushNode, "value", tail.bundleNode, "flush");
  wire(g, at.scan, tail.bundleNode, "scan");
  wire(g, at.rois, tail.bundleNode, "rois");
  g.edge(tail.circlesNode, "quality", tail.bundleNode, "fits");
  wire(g, flush.qualityBase, tail.bundleNode, "fitBase");
  if (c.refType == "line end")
    wire(g, flush.qualityRef, tail.bundleNode, "fitRef");
  wire(g, flush.lineFeature, tail.bundleNode, "baseFeature");
  wire(g, flush.referenceFeature, tail.bundleNode, "refFeature");
  wire(g, circles, tail.bundleNode, "circleFeature");
  if (c.withFallback) {
    g.edge("n_fb_location", "choice", tail.bundleNode, "fallback");
    g.edge("n_fb_line", "choice", tail.bundleNode, "fallbackLine");
    g.edge("n_fb_reference", "choice", tail.bundleNode, "fallbackReference");
  }
  graphJson = finish(g, c, tail, fine);
  return Status::Ok();
}
Status buildBlocks(const Config& c, std::string& graphJson) {
  return buildPipeline(c, false, graphJson);
}
Status buildFine(const Config& c, std::string& graphJson) {
  return buildPipeline(c, true, graphJson);
}

Status build(const std::string& text, const fs::path& baseDir, Mode mode, bool fine,
             std::string& out) {
  YAML::Node cfg;
  try {
    cfg = YAML::Load(text);
  } catch (const std::exception& e) {
    return badInput(std::string("StandardGap.yml 解析失败: ") + e.what());
  }
  if (!cfg || !cfg.IsMap())
    return badInput("StandardGap.yml 不是一个映射");
  try {
    Config c(cfg);
    if (Status status = parseConfig(cfg, baseDir, mode, c); !status.ok)
      return status;
    if (c.definition != "A" && c.definition != "B")
      return badParam("gap.definition必须为A或B");
    std::set<std::string> ids;
    for (const auto& candidate : c.candidates)
      if (!ids.insert(safeId(candidate.id)).second)
        return badParam("模板标识经过安全化后重复：" + candidate.id);
    std::string candidate;
    const auto status = fine ? buildFine(c, candidate) : buildBlocks(c, candidate);
    if (status.ok)
      out = std::move(candidate);
    return status;
  } catch (const std::exception& error) {
    return badInput(std::string("StandardGap导入失败：") + error.what());
  }
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

} // namespace

void registerStandardGapImporter(Registry& r) {
  const char* kBlocks =
      "产出积木图：gap.read_scan → 定位 → gap.role_line / gap.ref_point → metrology.flush_point_line，"
      "gap.seam_circles → metrology.circle_gap，再加判定与 gap.result_bundle。";
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

} // namespace lyflow::packs::gap
