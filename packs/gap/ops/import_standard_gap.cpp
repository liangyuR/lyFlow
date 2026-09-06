#include <algorithm>
#include <array>
#include <cstdio>
#include <utility>
#include <filesystem>
#include <string>
#include <vector>

#include <yaml-cpp/yaml.h>

#include "gap_ops.h"

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

bool roiList(const YAML::Node& node, std::array<double, 4>* out, std::string* problem) {
  *out = {0.0, 0.0, 0.0, 0.0};
  if (!present(node)) return true;
  if (!node.IsSequence() || node.size() != 4) {
    *problem = "ROI 必须是四个数";
    return false;
  }
  for (std::size_t i = 0; i < 4; ++i) (*out)[i] = node[i].as<double>(0.0);
  return true;
}

class Builder {
 public:
  std::string node(const std::string& id, const char* op, nlohmann::json params, int column,
                   int row, const std::string& title) {
    nlohmann::json entry;
    entry["id"] = id;
    entry["op"] = op;
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

struct Candidate {
  std::string id;
  std::string left;
  std::string right;
  std::array<double, 4> flushBase{};
  std::array<double, 4> flushRef{};
  std::array<double, 4> gapLeft{};
  std::array<double, 4> gapRight{};
};

enum class Mode { Template, Model, Auto };

YAML::Node modelRoiSetting(const fs::path& baseDir) {
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

Status buildGraph(const std::string& text, const fs::path& baseDir, Mode mode,
                  std::string& graphJson) {
  YAML::Node cfg;
  try {
    cfg = YAML::Load(text);
  } catch (const std::exception& e) {
    return badInput(std::string("StandardGap.yml 解析失败: ") + e.what());
  }
  if (!cfg || !cfg.IsMap()) return badInput("StandardGap.yml 不是一个映射");

  const YAML::Node common = child(cfg, "common_settings");
  const YAML::Node flush = child(cfg, "flush");
  const YAML::Node gap = child(cfg, "gap");
  const YAML::Node align = child(cfg, "align");
  const YAML::Node icp = child(align, "ICP");

  const std::string segMode = textOr(common, "seg_mode", "ROI");
  if (segMode != "ROI") {
    return badInput("只支持 seg_mode: ROI，这份配置是 '" + segMode + "'");
  }
  if (!boolOr(align, "align_cloud", false)) {
    return badInput("align_cloud 为 false 时是 label 模式，不产生测量结果");
  }

  const std::string baseSide = textOr(flush, "base_side", "left");
  const std::string baseType = textOr(flush, "base_type", "fit line");
  const std::string refType = textOr(flush, "ref_type", "line end");
  const std::string leftType = textOr(gap, "left_type", "circle");
  const std::string rightType = textOr(gap, "right_type", "circle");
  const std::pair<const char*, const std::string&> checked[4] = {
      {"flush.base_type", baseType},
      {"flush.ref_type", refType},
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
  if (refType != "line end" && refType != "selected point" && refType != "nearest point") {
    return badInput(
        "flush.ref_type 目前只支持 line end / selected point / nearest point，拿到 '" + refType +
        "'");
  }
  if (leftType != "circle" || rightType != "circle") {
    return badInput("gap 两侧目前只支持 circle");
  }

  std::string modelPath;
  const YAML::Node setting = modelRoiSetting(baseDir);
  if (mode == Mode::Auto) {
    mode = present(setting) && boolOr(setting, "enabled", false) ? Mode::Model : Mode::Template;
  }
  if (mode == Mode::Model) {
    modelPath = textOr(setting, "model_path", "");
    if (modelPath.empty()) {
      fs::path found;
      if (uniqueOnnx(baseDir, &found)) modelPath = found.string();
    }
    if (modelPath.empty()) {
      return badParam(
          "模型模式要一个 ONNX：setting.yml 的 model_roi.model_path 没有，"
          "baseDir 下也没有唯一的 *.onnx");
    }
  }
  const bool useModel = mode == Mode::Model;
  const bool withFallback =
      useModel && present(setting) && boolOr(setting, "enabled", false);

  std::array<double, 4> flushBaseRoi{};
  std::array<double, 4> flushRefRoi{};
  std::array<double, 4> gapLeftRoi{};
  std::array<double, 4> gapRightRoi{};
  std::string problem;
  if (!roiList(child(flush, "base_roi"), &flushBaseRoi, &problem) ||
      !roiList(child(flush, "ref_roi"), &flushRefRoi, &problem) ||
      !roiList(child(gap, "left_roi"), &gapLeftRoi, &problem) ||
      !roiList(child(gap, "right_roi"), &gapRightRoi, &problem)) {
    return badInput(problem);
  }

  std::vector<Candidate> candidates;
  const YAML::Node listed = child(align, "template_candidates");
  if (present(listed) && listed.IsSequence() && listed.size() > 0) {
    for (std::size_t i = 0; i < listed.size(); ++i) {
      const YAML::Node item = listed[i];
      Candidate c;
      c.id = textOr(item, "id", "t" + std::to_string(i));
      c.left = textOr(item, "left", "left_template.pcd");
      c.right = textOr(item, "right", "right_template.pcd");
      const YAML::Node rois = child(item, "rois");
      const YAML::Node croiFlush = child(rois, "flush");
      const YAML::Node croiGap = child(rois, "gap");
      const YAML::Node base = child(croiFlush, "base_roi");
      const YAML::Node ref = child(croiFlush, "ref_roi");
      const YAML::Node left = child(croiGap, "left_roi");
      const YAML::Node right = child(croiGap, "right_roi");
      if (!roiList(base ? base : child(flush, "base_roi"), &c.flushBase, &problem) ||
          !roiList(ref ? ref : child(flush, "ref_roi"), &c.flushRef, &problem) ||
          !roiList(left ? left : child(gap, "left_roi"), &c.gapLeft, &problem) ||
          !roiList(right ? right : child(gap, "right_roi"), &c.gapRight, &problem)) {
        return badInput(problem);
      }
      candidates.push_back(std::move(c));
    }
  } else {
    Candidate c;
    c.id = "primary";
    c.left = "left_template.pcd";
    c.right = "right_template.pcd";
    c.flushBase = flushBaseRoi;
    c.flushRef = flushRefRoi;
    c.gapLeft = gapLeftRoi;
    c.gapRight = gapRightRoi;
    candidates.push_back(std::move(c));
  }
  const bool needTemplates = !useModel || withFallback;
  if (needTemplates && candidates.size() > 4) {
    return badInput("gap.select_alignment 最多接四个候选，这份配置有 " +
                    std::to_string(candidates.size()) + " 个");
  }

  Builder g;
  const YAML::Node filterCfg = child(common, "filter");
  const bool usingRemoval = boolOr(filterCfg, "using_removal", false);
  const std::string usingCamera = textOr(common, "using_camera", "Both");
  const auto minScore = intOr(icp, "min_score", 60);

  nlohmann::json loadParams;
  loadParams["dir"] = ".";
  if (useModel) loadParams["dropNonFinite"] = false;
  const std::string load = g.node("n_load", "gap.load_profile_pair", loadParams, 0, 1, "读一对剖面");

  const auto radiusOutlier = [&](const std::string& id, const std::string& source,
                                 const char* sourcePort, int column,
                                 int row) -> std::pair<std::string, const char*> {
    if (!usingRemoval) return {source, sourcePort};
    nlohmann::json p;
    p["radius"] = numberOr(filterCfg, "filter_radius", 0.0) / 1000.0;
    p["minNeighbors"] = intOr(filterCfg, "filter_neighbors", 0);
    const std::string flt = g.node(id, "filter.radius_outlier", p, column, row, "半径离群");
    g.edge(source, sourcePort, flt, "cloud");
    return {flt, "cloud"};
  };

  const auto buildTemplateBranch = [&](const std::string& prefix, const std::string& srcPrimary,
                                       const char* srcPrimaryPort, const std::string& srcSecondary,
                                       const char* srcSecondaryPort, int column) {
    struct Branch {
      std::string overall, cropP, cropS, merged, rois, select;
      const char* mergedPort = "cloud";
    } b;
    nlohmann::json overallParams;
    std::array<double, 4> overallRoi{};
    roiList(child(common, "overall_roi"), &overallRoi, &problem);
    overallParams["roi"] = overallRoi;
    overallParams["mode"] = textOr(common, "overall_roi_mode", "fixed");
    overallParams["usingCamera"] = usingCamera;
    b.overall = g.node(prefix + "n_overall", "gap.overall_roi", overallParams, column, 1,
                       "整体 ROI");
    g.edge(srcPrimary, srcPrimaryPort, b.overall, "primary");
    g.edge(srcSecondary, srcSecondaryPort, b.overall, "secondary");

    nlohmann::json cropOpen;
    cropOpen["bounds"] = "open";
    b.cropP = g.node(prefix + "n_crop_p", "filter.crop_box2d", cropOpen, column + 1, 0,
                     "裁 primary");
    b.cropS = g.node(prefix + "n_crop_s", "filter.crop_box2d", cropOpen, column + 1, 2,
                     "裁 secondary");
    g.edge(srcPrimary, srcPrimaryPort, b.cropP, "cloud");
    g.edge(b.overall, "box", b.cropP, "box");
    g.edge(srcSecondary, srcSecondaryPort, b.cropS, "cloud");
    g.edge(b.overall, "box", b.cropS, "box");

    const std::string merge = g.node(prefix + "n_merge", "util.merge", nullptr, column + 2, 1,
                                     "合并（secondary 在前）");
    g.edge(b.cropS, "cloud", merge, "a");
    g.edge(b.cropP, "cloud", merge, "b");
    const auto merged = radiusOutlier(prefix + "n_filter", merge, "cloud", column + 3, 1);
    b.merged = merged.first;
    b.mergedPort = merged.second;

    std::vector<std::string> alignNodes;
    for (std::size_t order = 0; order < candidates.size(); ++order) {
      const Candidate& c = candidates[order];
      const std::string cid = safeId(c.id);
      nlohmann::json tplParams;
      tplParams["dir"] = "StandardGap";
      tplParams["left"] = c.left;
      tplParams["right"] = c.right;
      const std::string tpl = g.node(prefix + "n_tpl_" + cid, "gap.load_template", tplParams,
                                     column + 3, 3 + static_cast<int>(order) * 2, "模板 " + cid);
      const YAML::Node robustness = child(align, "robustness");
      const YAML::Node trust = child(robustness, "trust_region");
      nlohmann::json p;
      p["templateId"] = c.id;
      p["order"] = static_cast<std::int64_t>(order);
      p["maxMatchingDist"] = numberOr(icp, "max_matching_dist", 1.0);
      p["maxFitnessDist"] = numberOr(icp, "max_fitness_dist", 10.0);
      p["maxIterations"] = intOr(icp, "max_iteration_num", 1000);
      p["normalKnn"] = intOr(icp, "num_neighbor", 10);
      p["minScore"] = minScore;
      p["bidirection"] = boolOr(icp, "bidirection_align", false);
      p["globalCoarse"] = boolOr(robustness, "global_coarse", true);
      p["successGuide"] = boolOr(align, "success_guide", false);
      p["segRoi"] = true;
      p["trustTranslation"] = numberOr(trust, "max_translation_mm", 3.0);
      p["trustRotation"] = numberOr(trust, "max_rotation_deg", 2.0);
      p["degenerateRatio"] = numberOr(robustness, "degenerate_ratio", 1.0e-3);
      p["roiFlushBase"] = c.flushBase;
      p["roiGapLeft"] = c.gapLeft;
      p["roiFlushRef"] = c.flushRef;
      p["roiGapRight"] = c.gapRight;
      const std::string node = g.node(prefix + "n_align_" + cid, "gap.align_template", p,
                                      column + 4, 3 + static_cast<int>(order) * 2, "ICP " + cid);
      g.edge(b.merged, b.mergedPort, node, "cloud");
      g.edge(tpl, "left", node, "tplLeft");
      g.edge(tpl, "right", node, "tplRight");
      alignNodes.push_back(node);
    }

    nlohmann::json selectParams;
    selectParams["minScore"] = minScore;
    b.select = g.node(prefix + "n_select", "gap.select_alignment", selectParams, column + 5, 3,
                      "选模板");
    static const char* kPorts[4] = {"a", "b", "c", "d"};
    for (std::size_t i = 0; i < alignNodes.size() && i < 4; ++i) {
      g.edge(alignNodes[i], "alignment", b.select, kPorts[i]);
    }

    nlohmann::json roiParams;
    roiParams["baseSide"] = baseSide;
    b.rois = g.node(prefix + "n_rois", "gap.business_rois", roiParams, column + 6, 3, "业务 ROI");
    g.edge(b.select, "alignment", b.rois, "alignment");
    return b;
  };

  std::string rois;
  std::string cropP, cropS;
  const char* cropPPort = "cloud";
  const char* cropSPort = "cloud";
  std::string merged;
  const char* mergedPort = "cloud";
  std::string overallBox;
  const char* overallBoxPort = "box";
  std::string cloudP, cloudS;
  const char* cloudPPort = "cloud";
  const char* cloudSPort = "cloud";
  std::string alignmentNode;
  std::string cropStatusNode;
  std::string fallbackNode;
  std::string modelFlushCloud, backupFlushCloud;
  const char* modelFlushCloudPort = "cloud";
  const char* backupFlushCloudPort = "cloud";
  std::string modelFlushRois, backupFlushRois;
  static const char* kRoiPorts[4] = {"flushBase", "flushRef", "gapLeft", "gapRight"};
  std::string roiSource[4];
  const char* roiSourcePort[4] = {kRoiPorts[0], kRoiPorts[1], kRoiPorts[2], kRoiPorts[3]};

  const std::string frameP =
      g.node("n_frame_p", "gap.to_measurement_frame", nullptr, 1, 0, "换轴 primary");
  const std::string frameS =
      g.node("n_frame_s", "gap.to_measurement_frame", nullptr, 1, 2, "换轴 secondary");
  g.edge(load, "primary", frameP, "cloud");
  g.edge(load, "secondary", frameS, "cloud");

  if (useModel) {
    const std::string tensor =
        g.node("n_tensor", "gap.profile_tensor", nullptr, 1, 4, "剖面张量");
    g.edge(load, "primary", tensor, "primary");
    g.edge(load, "secondary", tensor, "secondary");
    nlohmann::json inferParams;
    inferParams["modelPath"] = modelPath;
    const std::string infer = g.node("n_infer", "ml.onnx_run", inferParams, 1, 5, "ONNX 推理");
    g.edge(tensor, "tensor", infer, "input");
    const std::string seg =
        g.node("n_seg", "gap.labels_from_logits", nullptr, 1, 6, "逐槽 argmax");
    g.edge(infer, "output", seg, "tensor");

    nlohmann::json coloredParams;
    coloredParams["row"] = "primary";
    const std::string colored =
        g.node("n_labels_p", "gap.labels_to_cloud", coloredParams, 2, 7, "着色 primary");
    g.edge(frameP, "cloud", colored, "cloud");
    g.edge(seg, "labels", colored, "labels");

    nlohmann::json roiParams;
    roiParams["baseSide"] = baseSide;
    const std::string modelRois =
        g.node("n_rois", "gap.roi_from_labels", roiParams, 3, 4, "模型四框");
    g.edge(load, "primary", modelRois, "primary");
    g.edge(load, "secondary", modelRois, "secondary");
    g.edge(seg, "labels", modelRois, "labels");
    g.edge(colored, "cloud", modelRois, "backdrop");

    const std::string dropP =
        g.node("n_drop_p", "gap.drop_non_finite", nullptr, 2, 0, "剔 NaN primary");
    const std::string dropS =
        g.node("n_drop_s", "gap.drop_non_finite", nullptr, 2, 2, "剔 NaN secondary");
    g.edge(frameP, "cloud", dropP, "cloud");
    g.edge(frameS, "cloud", dropS, "cloud");

    const YAML::Node rollCfg = child(common, "roll_anchored_crop");
    nlohmann::json rollParams;
    rollParams["enabled"] = boolOr(rollCfg, "enabled", false);
    rollParams["halfWidth"] = numberOr(rollCfg, "half_width_mm", 35.0);
    rollParams["halfHeight"] = numberOr(rollCfg, "half_height_mm", 20.0);
    rollParams["maxRollBoxHeight"] = numberOr(rollCfg, "max_roll_box_height_mm", 8.0);
    rollParams["minPointsKept"] = intOr(rollCfg, "min_points_kept", 50);
    rollParams["usingCamera"] = usingCamera;
    const std::string roll =
        g.node("n_roll", "gap.roll_anchored_crop", rollParams, 4, 1, "跟随零件的裁剪窗");
    g.edge(dropP, "cloud", roll, "primary");
    g.edge(dropS, "cloud", roll, "secondary");
    g.edge(modelRois, "gapLeft", roll, "gapLeft");
    g.edge(modelRois, "gapRight", roll, "gapRight");

    const std::string merge =
        g.node("n_merge", "util.merge", nullptr, 5, 1, "合并（secondary 在前）");
    g.edge(roll, "secondary", merge, "a");
    g.edge(roll, "primary", merge, "b");
    const auto modelMerged = radiusOutlier("n_filter", merge, "cloud", 6, 1);

    cloudP = dropP;
    cloudS = dropS;
    cropStatusNode = roll;

    if (!withFallback) {
      rois = modelRois;
      cropP = roll;
      cropPPort = "primary";
      cropS = roll;
      cropSPort = "secondary";
      merged = modelMerged.first;
      mergedPort = modelMerged.second;
      overallBox = roll;
      overallBoxPort = "window";
      for (int i = 0; i < 4; ++i) roiSource[i] = modelRois;
    } else {
      nlohmann::json backupLoad;
      backupLoad["dir"] = ".";
      const std::string bLoad =
          g.node("b_n_load", "gap.load_profile_pair", backupLoad, 0, 9, "读一对剖面（备用）");
      const std::string bFrameP =
          g.node("b_n_frame_p", "gap.to_measurement_frame", nullptr, 1, 9, "换轴 primary（备用）");
      const std::string bFrameS =
          g.node("b_n_frame_s", "gap.to_measurement_frame", nullptr, 1, 10,
                 "换轴 secondary（备用）");
      g.edge(bLoad, "primary", bFrameP, "cloud");
      g.edge(bLoad, "secondary", bFrameS, "cloud");
      const auto branch = buildTemplateBranch("b_", bFrameP, "cloud", bFrameS, "cloud", 3);
      const auto fallback = [&](const std::string& id, const std::string& a, const char* aPort,
                                const std::string& b, const char* bPort, int row,
                                const char* title) {
        const std::string node = g.node(id, "flow.fallback", nullptr, 7, row, title);
        g.edge(a, aPort, node, "a");
        g.edge(b, bPort, node, "b");
        return node;
      };
      static const char* kFbTitles[4] = {"回退 flush_base", "回退 flush_ref", "回退 gap_left",
                                         "回退 gap_right"};
      for (int i = 0; i < 4; ++i) {
        roiSource[i] = fallback(std::string("n_fb_") + kRoiPorts[i], modelRois, kRoiPorts[i],
                                branch.rois, kRoiPorts[i], i, kFbTitles[i]);
        roiSourcePort[i] = "out";
      }
      fallbackNode = roiSource[0];
      const std::string fbMerged = fallback("n_fb_merged", modelMerged.first, modelMerged.second,
                                            branch.merged, branch.mergedPort, 4, "回退合并云");
      const std::string fbCropP =
          fallback("n_fb_crop_p", roll, "primary", branch.cropP, "cloud", 5, "回退 primary");
      const std::string fbCropS =
          fallback("n_fb_crop_s", roll, "secondary", branch.cropS, "cloud", 6, "回退 secondary");
      const std::string fbOverall =
          fallback("n_fb_overall", roll, "window", branch.overall, "box", 7, "回退整体框");
      modelFlushCloud = modelMerged.first;
      modelFlushCloudPort = modelMerged.second;
      modelFlushRois = modelRois;
      backupFlushCloud = branch.merged;
      backupFlushCloudPort = branch.mergedPort;
      backupFlushRois = branch.rois;
      merged = fbMerged;
      mergedPort = "out";
      cropP = fbCropP;
      cropPPort = "out";
      cropS = fbCropS;
      cropSPort = "out";
      overallBox = fbOverall;
      overallBoxPort = "out";
    }
  } else {
    const auto branch = buildTemplateBranch("", frameP, "cloud", frameS, "cloud", 2);
    rois = branch.rois;
    for (int i = 0; i < 4; ++i) roiSource[i] = branch.rois;
    cropP = branch.cropP;
    cropS = branch.cropS;
    merged = branch.merged;
    mergedPort = branch.mergedPort;
    overallBox = branch.overall;
    alignmentNode = branch.select;
    cloudP = frameP;
    cloudS = frameS;
  }

  const bool splitFlush = !backupFlushRois.empty();
  nlohmann::json cropOpen;
  cropOpen["bounds"] = "open";
  std::string crops[4];
  static const char* kCropTitles[4] = {"裁 flush_base", "裁 flush_ref", "裁 gap_left",
                                       "裁 gap_right"};
  for (int i = 0; i < 4; ++i) {
    const bool own = splitFlush && i < 2;
    crops[i] = g.node(std::string("n_crop_") + kRoiPorts[i], "filter.crop_box2d", cropOpen, 9, i,
                      kCropTitles[i]);
    g.edge(own ? modelFlushCloud : merged, own ? modelFlushCloudPort : mergedPort, crops[i],
           "cloud");
    g.edge(own ? modelFlushRois : roiSource[i], own ? kRoiPorts[i] : roiSourcePort[i], crops[i],
           "box");
  }
  const std::string flushBox[2] = {splitFlush ? modelFlushRois : roiSource[0],
                                   splitFlush ? modelFlushRois : roiSource[1]};
  const char* flushBoxPort[2] = {splitFlush ? kRoiPorts[0] : roiSourcePort[0],
                                 splitFlush ? kRoiPorts[1] : roiSourcePort[1]};
  const std::string flushCloud = splitFlush ? modelFlushCloud : merged;
  const char* flushCloudPort = splitFlush ? modelFlushCloudPort : mergedPort;

  const bool baseIsLeft = baseSide == "left";
  const double lineDist = numberOr(common, "line_fit_distance", 0.1);
  const auto segmentPoints = intOr(flush, "segment_points", 0);
  const char* endpoints = useModel ? "inlier_ends" : "roi_intersection";
  nlohmann::json fitBaseParams;
  fitBaseParams["side"] = baseIsLeft ? "left" : "right";
  fitBaseParams["distThresh"] = lineDist;
  fitBaseParams["segmentPoints"] = segmentPoints;
  fitBaseParams["endpoints"] = endpoints;
  const std::string fitBase =
      g.node("n_fit_base", "gap.fit_line", fitBaseParams, 10, 0, "拟合基准线");
  g.edge(crops[0], "cloud", fitBase, "cloud");
  g.edge(flushBox[0], flushBoxPort[0], fitBase, "box");

  std::string refNode;
  const char* refPort = "point";
  std::string fitRef;
  if (refType == "line end") {
    nlohmann::json p;
    p["side"] = baseIsLeft ? "right" : "left";
    p["distThresh"] = lineDist;
    p["segmentPoints"] = segmentPoints;
    p["endpoints"] = endpoints;
    fitRef = g.node("n_fit_ref", "gap.fit_line", p, 10, 1, "拟合参考线");
    g.edge(crops[1], "cloud", fitRef, "cloud");
    g.edge(flushBox[1], flushBoxPort[1], fitRef, "box");
    refNode = fitRef;
    refPort = "innerEnd";
  } else if (refType == "selected point") {
    refNode = g.node("n_sel_ref", "gap.selected_point", nullptr, 10, 1, "选参考点");
    g.edge(flushCloud, flushCloudPort, refNode, "cloud");
    g.edge(flushBox[1], flushBoxPort[1], refNode, "box");
  } else {
    refNode = g.node("n_near_ref", "gap.nearest_to_line", nullptr, 10, 1, "离基准线最近的点");
    g.edge(crops[1], "cloud", refNode, "cloud");
    g.edge(fitBase, "line", refNode, "line");
  }

  std::string lineNode = fitBase;
  const char* linePort = "line";
  std::string refOutNode = refNode;
  const char* refOutPort = refPort;
  std::string qualityBase = fitBase;
  const char* qualityBasePort = "quality";
  std::string qualityRef = fitRef;
  const char* qualityRefPort = "quality";
  if (splitFlush) {
    nlohmann::json bp;
    bp["bounds"] = "open";
    const std::string bCropBase = g.node("b_n_crop_flushBase", "filter.crop_box2d", bp, 9, 9,
                                         "裁 flush_base（备用）");
    g.edge(backupFlushCloud, backupFlushCloudPort, bCropBase, "cloud");
    g.edge(backupFlushRois, kRoiPorts[0], bCropBase, "box");
    const std::string bCropRef = g.node("b_n_crop_flushRef", "filter.crop_box2d", bp, 9, 10,
                                        "裁 flush_ref（备用）");
    g.edge(backupFlushCloud, backupFlushCloudPort, bCropRef, "cloud");
    g.edge(backupFlushRois, kRoiPorts[1], bCropRef, "box");

    nlohmann::json bBaseParams = fitBaseParams;
    bBaseParams["endpoints"] = "roi_intersection";
    const std::string bFitBase =
        g.node("b_n_fit_base", "gap.fit_line", bBaseParams, 10, 9, "拟合基准线（备用）");
    g.edge(bCropBase, "cloud", bFitBase, "cloud");
    g.edge(backupFlushRois, kRoiPorts[0], bFitBase, "box");

    std::string bRefNode;
    const char* bRefPort = "point";
    std::string bFitRef;
    if (refType == "line end") {
      nlohmann::json p;
      p["side"] = baseIsLeft ? "right" : "left";
      p["distThresh"] = lineDist;
      p["segmentPoints"] = segmentPoints;
      p["endpoints"] = "roi_intersection";
      bFitRef = g.node("b_n_fit_ref", "gap.fit_line", p, 10, 10, "拟合参考线（备用）");
      g.edge(bCropRef, "cloud", bFitRef, "cloud");
      g.edge(backupFlushRois, kRoiPorts[1], bFitRef, "box");
      bRefNode = bFitRef;
      bRefPort = "innerEnd";
    } else if (refType == "selected point") {
      bRefNode = g.node("b_n_sel_ref", "gap.selected_point", nullptr, 10, 10, "选参考点（备用）");
      g.edge(backupFlushCloud, backupFlushCloudPort, bRefNode, "cloud");
      g.edge(backupFlushRois, kRoiPorts[1], bRefNode, "box");
    } else {
      bRefNode =
          g.node("b_n_near_ref", "gap.nearest_to_line", nullptr, 10, 10, "离基准线最近的点（备用）");
      g.edge(bCropRef, "cloud", bRefNode, "cloud");
      g.edge(bFitBase, "line", bRefNode, "line");
    }

    const auto fitFallback = [&](const std::string& id, const std::string& a, const char* aPort,
                                 const std::string& b, const char* bPort, int row,
                                 const char* title) {
      const std::string node = g.node(id, "flow.fallback", nullptr, 11, row, title);
      g.edge(a, aPort, node, "a");
      g.edge(b, bPort, node, "b");
      return node;
    };
    lineNode = fitFallback("n_fb_line", fitBase, "line", bFitBase, "line", 5, "回退基准线");
    linePort = "out";
    refOutNode = fitFallback("n_fb_ref_point", refNode, refPort, bRefNode, bRefPort, 6, "回退参考点");
    refOutPort = "out";
    qualityBase = fitFallback("n_fb_quality_base", fitBase, "quality", bFitBase, "quality", 7,
                              "回退基准线质量");
    qualityBasePort = "out";
    if (!fitRef.empty()) {
      qualityRef = fitFallback("n_fb_quality_ref", fitRef, "quality", bFitRef, "quality", 8,
                               "回退参考线质量");
      qualityRefPort = "out";
    }
  }

  nlohmann::json flushParams;
  flushParams["offset"] = numberOr(flush, "offset", 0.0);
  const std::string flushNode = g.node("n_flush", "gap.flush", flushParams, 11, 0, "段差");
  g.edge(lineNode, linePort, flushNode, "baseLine");
  g.edge(refOutNode, refOutPort, flushNode, "refPoint");

  const YAML::Node radius = child(gap, "radius");
  const bool cameraFallback = boolOr(gap, "camera_separated_circle_fallback", true);
  bool selectClosest;
  if (present(child(gap, "camera_separated_select_closest_nominal"))) {
    selectClosest = boolOr(gap, "camera_separated_select_closest_nominal", true);
  } else if (present(child(gap, "camera_separated_select_closest_radius"))) {
    selectClosest = boolOr(gap, "camera_separated_select_closest_radius", true);
  } else {
    selectClosest =
        cameraFallback && textOr(gap, "camera_separated_preferred_camera", "Both") == "Both";
  }
  nlohmann::json circleParams;
  circleParams["distThresh"] = numberOr(common, "circle_fit_distance", 0.03);
  circleParams["retryDistance"] = numberOr(gap, "circle_fit_retry_distance", 0.0);
  circleParams["nominal"] = numberOr(child(gap, "tolerances"), "nominal", 0.0);
  circleParams["offset"] = numberOr(gap, "offset", 0.0);
  circleParams["leftRadiusFixed"] = boolOr(radius, "fixed_left_circle_radius", false);
  circleParams["leftRadiusValue"] = numberOr(radius, "left_circle_radius", 0.0);
  circleParams["leftRadiusMin"] = numberOr(radius, "left_circle_radius_min", 0.0);
  circleParams["leftRadiusMax"] = numberOr(radius, "left_circle_radius_max", 0.0);
  circleParams["rightRadiusFixed"] = boolOr(radius, "fixed_right_circle_radius", false);
  circleParams["rightRadiusValue"] = numberOr(radius, "right_circle_radius", 0.0);
  circleParams["rightRadiusMin"] = numberOr(radius, "right_circle_radius_min", 0.0);
  circleParams["rightRadiusMax"] = numberOr(radius, "right_circle_radius_max", 0.0);
  circleParams["cameraFallback"] = cameraFallback;
  circleParams["selectClosestNominal"] = selectClosest;
  circleParams["preferredCamera"] = textOr(gap, "camera_separated_preferred_camera", "Both");
  const std::string circles =
      g.node("n_circles", "gap.fit_gap_circles", circleParams, 10, 2, "两侧圆拟合");
  g.edge(merged, mergedPort, circles, "merged");
  g.edge(cropP, cropPPort, circles, "primary");
  g.edge(cropS, cropSPort, circles, "secondary");
  g.edge(roiSource[2], roiSourcePort[2], circles, "boxLeft");
  g.edge(roiSource[3], roiSourcePort[3], circles, "boxRight");

  const std::string definition = textOr(gap, "definition", "B");
  nlohmann::json gapParams;
  gapParams["definition"] = definition;
  gapParams["offset"] = numberOr(gap, "offset", 0.0);
  const std::string gapNode = g.node("n_gap", "gap.gap", gapParams, 11, 2, "间隙");
  g.edge(circles, "left", gapNode, "left");
  g.edge(circles, "right", gapNode, "right");
  if (definition == "A") g.edge(flushNode, "baseLine", gapNode, "baseLine");

  const YAML::Node sections[2] = {flush, gap};
  const std::string sources[2] = {flushNode, gapNode};
  static const char* kJudgeTitles[2] = {"判定段差", "判定间隙"};
  for (int row = 0; row < 2; ++row) {
    const YAML::Node tol = child(sections[row], "tolerances");
    nlohmann::json p;
    p["nominal"] = numberOr(tol, "nominal", 0.0);
    p["upper"] = numberOr(tol, "up_deviation", 0.0);
    p["lower"] = numberOr(tol, "low_deviation", 0.0);
    g.node("n_judge_" + std::to_string(row), "gap.judge", p, 12, row * 2, kJudgeTitles[row]);
    g.edge(sources[row], "value", "n_judge_" + std::to_string(row), "value");
  }

  nlohmann::json bundleParams;
  if (cropStatusNode.empty() && boolOr(child(common, "roll_anchored_crop"), "enabled", false)) {
    bundleParams["cropStatus"] = "skipped:no_model_roi";
  }
  const std::string bundle =
      g.node("n_bundle", "gap.result_bundle", bundleParams, 12, 4, "结果汇总");
  g.edge(gapNode, "value", bundle, "gap");
  g.edge(flushNode, "value", bundle, "flush");
  static const char* kBundleRoiPorts[4] = {"roiFlushBase", "roiFlushRef", "roiGapLeft",
                                           "roiGapRight"};
  for (int i = 0; i < 4; ++i) g.edge(roiSource[i], roiSourcePort[i], bundle, kBundleRoiPorts[i]);
  g.edge(overallBox, overallBoxPort, bundle, "roiOverall");
  g.edge(circles, "quality", bundle, "fits");
  g.edge(qualityBase, qualityBasePort, bundle, "fitBase");
  if (!qualityRef.empty()) g.edge(qualityRef, qualityRefPort, bundle, "fitRef");
  if (!cropStatusNode.empty()) g.edge(cropStatusNode, "status", bundle, "cropStatus");
  if (!alignmentNode.empty()) g.edge(alignmentNode, "alignment", bundle, "alignment");
  if (!fallbackNode.empty()) g.edge(fallbackNode, "choice", bundle, "fallback");
  g.edge(cloudP, cloudPPort, bundle, "cloudPrimary");
  g.edge(cloudS, cloudSPort, bundle, "cloudSecondary");
  g.edge(merged, mergedPort, bundle, "cloudMerged");

  const std::string sampleId = baseDir.filename().string();
  nlohmann::json refParams;
  refParams["configPath"] = "StandardGap.yml";
  refParams["deriveTemplateDir"] = false;
  refParams["templateDir"] = "StandardGap";
  refParams["sampleId"] = sampleId;
  if (useModel) {
    refParams["useModel"] = true;
    refParams["modelPath"] = modelPath;
  }
  const std::string ref = g.node("n_ref", "gap.measure_reference", refParams, 2, 8, "黑盒对照");
  g.edge(load, "primary", ref, "primary");
  g.edge(load, "secondary", ref, "secondary");

  nlohmann::json doc;
  doc["schemaVersion"] = 1;
  doc["id"] = safeId("gap_" + sampleId);
  doc["name"] = sampleId + " · StandardGap" + (useModel ? " · 模型" : "");
  doc["meta"] = nlohmann::json{{"app", "StandardGap.yml importer"}};
  doc["nodes"] = std::move(g.nodes);
  doc["edges"] = std::move(g.edges);
  nlohmann::json outputs = nlohmann::json::object();
  outputs["gap"] = nlohmann::json{{"node", gapNode}, {"port", "value"}, {"label", "间隙"}};
  outputs["flush"] = nlohmann::json{{"node", flushNode}, {"port", "value"}, {"label", "段差"}};
  outputs["bundle"] =
      nlohmann::json{{"node", bundle}, {"port", "bundle"}, {"label", "结果汇总"}};
  doc["outputs"] = std::move(outputs);
  graphJson = doc.dump(2);
  return Status::Ok();
}

Status importAuto(const std::string& text, const fs::path& baseDir, std::string& out) {
  return buildGraph(text, baseDir, Mode::Auto, out);
}
Status importTemplate(const std::string& text, const fs::path& baseDir, std::string& out) {
  return buildGraph(text, baseDir, Mode::Template, out);
}
Status importModel(const std::string& text, const fs::path& baseDir, std::string& out) {
  return buildGraph(text, baseDir, Mode::Model, out);
}

}  // namespace

void registerStandardGapImporter(Registry& r) {
  ImporterDesc desc;
  desc.kind = "StandardGap.yml";
  desc.label = "StandardGap.yml（自动）";
  desc.doc =
      "把一份 StandardGap.yml 转成一张测量图。模式由 baseDir 或其父目录里的 setting.yml 决定："
      "model_roi.enabled 为真走模型路径并带 flow.fallback，否则走模板/ICP 路径。";
  desc.fn = &importAuto;
  r.addImporter(desc);

  desc.kind = "StandardGap.yml:template";
  desc.label = "StandardGap.yml（模板/ICP）";
  desc.doc = "强制模板/ICP 路径，不看 setting.yml。";
  desc.fn = &importTemplate;
  r.addImporter(desc);

  desc.kind = "StandardGap.yml:model";
  desc.label = "StandardGap.yml（模型 ROI）";
  desc.doc =
      "强制模型 ROI 路径。ONNX 取自 setting.yml 的 model_roi.model_path，"
      "没有就找 baseDir 下唯一的 *.onnx。setting.yml 里 model_roi.enabled 为真时还会带上 "
      "flow.fallback 的模板备用闭包。";
  desc.fn = &importModel;
  r.addImporter(desc);
}

}  // namespace lyflow::packs::gap
