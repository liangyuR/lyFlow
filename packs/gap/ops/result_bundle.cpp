#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "gap_ops.h"

namespace lyflow::packs::gap {
namespace {

nlohmann::json pick(const nlohmann::json& src, const char* key) {
  const auto it = src.find(key);
  if (it == src.end()) return nlohmann::json();
  return *it;
}

std::size_t asCount(const nlohmann::json& v) {
  return v.is_number() ? v.get<std::size_t>() : 0;
}

nlohmann::json fitEntry(const std::string& component, const nlohmann::json& q) {
  nlohmann::json out;
  out["component"] = component;
  out["model"] = q.value("model", std::string());
  out["radius_mode"] = q.value("radiusMode", std::string());
  out["point_count"] = asCount(pick(q, "pointCount"));
  out["inlier_count"] = asCount(pick(q, "inlierCount"));
  out["inlier_ratio"] = pick(q, "inlierRatio");
  out["rms_residual_mm"] = pick(q, "rmsResidualMm");
  out["max_residual_mm"] = pick(q, "maxResidualMm");
  out["arc_coverage_deg"] = pick(q, "arcCoverageDeg");
  out["radius_mm"] = pick(q, "radiusMm");
  out["center_x_mm"] = pick(q, "centerXMm");
  out["center_y_mm"] = pick(q, "centerYMm");
  out["line_point_x_mm"] = pick(q, "linePointXMm");
  out["line_point_y_mm"] = pick(q, "linePointYMm");
  out["line_dir_x"] = pick(q, "lineDirX");
  out["line_dir_y"] = pick(q, "lineDirY");
  return out;
}

nlohmann::json boxMm(const lyflow::Box2D& box) {
  return nlohmann::json::array({mToMm(box.min[0]), mToMm(box.min[1]), mToMm(box.max[0]),
                                mToMm(box.max[1])});
}

nlohmann::json measurementJson(const lyflow::Measurement* m) {
  nlohmann::json out;
  if (m == nullptr) {
    out["status"] = "inactive";
    out["value_mm"] = nlohmann::json();
    return out;
  }
  out["status"] = m->ok ? "success" : "failure";
  out["value_mm"] = m->ok && std::isfinite(m->value) ? nlohmann::json(m->value) : nlohmann::json();
  out["unit"] = m->unit;
  out["message"] = m->message;
  out["verdict"] = m->verdict;
  return out;
}

nlohmann::json icpSide(const nlohmann::json& side, const std::string& component,
                       const std::string& templateId, bool selected) {
  nlohmann::json out;
  out["component"] = component;
  out["template_id"] = templateId;
  out["selected"] = selected;
  out["score"] = side.value("score", 0.0);
  out["success"] = side.value("success", false);
  out["bidirectional"] = side.value("bidirectional", false);
  out["transform"] = pick(side, "transform");
  out["degenerate_eigenvalue_ratio"] = pick(side, "degenerateRatio");
  out["degenerate_locked"] = side.value("degenerateLocked", false);
  out["trust_region_delta_translation_mm"] = pick(side, "trustTranslationMm");
  out["trust_region_delta_rotation_deg"] = pick(side, "trustRotationDeg");
  out["trust_region_clamped"] = side.value("trustClamped", false);
  return out;
}

std::vector<std::string> packVersions() {
  std::vector<std::string> packs;
  for (const OperatorDesc& op : ensureRegistry().operators()) {
    if (op.pack.empty()) continue;
    if (std::find(packs.begin(), packs.end(), op.pack) == packs.end()) packs.push_back(op.pack);
  }
  std::sort(packs.begin(), packs.end());
  return packs;
}

Status resultBundle(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                    ExecContext&) {
  nlohmann::json bundle;
  bundle["gap"] = measurementJson(inputs.get("gap").asMeasurement());
  // 没有面差需求的测点（HUD/Audio、V 缝开口）可以不接 flush，汇总里记 inactive。
  bundle["flush"] =
      measurementJson(inputs.has("flush") ? inputs.get("flush").asMeasurement() : nullptr);

  nlohmann::json pointCounts = nlohmann::json::object();
  nlohmann::json fits = nlohmann::json::array();

  static const char* kLinePorts[2] = {"fitBase", "fitRef"};
  static const char* kLineComponents[2] = {"flush_base", "flush_ref"};
  static const char* kLineCountKeys[2] = {"flush_base_roi", "flush_ref_roi"};
  for (int i = 0; i < 2; ++i) {
    if (!inputs.has(kLinePorts[i])) continue;
    const lyflow::Record* rec = inputs.get(kLinePorts[i]).asRecord();
    if (rec == nullptr) continue;
    nlohmann::json entry = fitEntry(kLineComponents[i], rec->data);
    pointCounts[kLineCountKeys[i]] = entry["point_count"];
    fits.push_back(std::move(entry));
  }

  if (inputs.has("fits")) {
    const lyflow::Record* rec = inputs.get("fits").asRecord();
    if (rec != nullptr) {
      static const char* kSides[2] = {"left", "right"};
      static const char* kComponents[2] = {"gap_left", "gap_right"};
      static const char* kCountKeys[2] = {"gap_left_roi", "gap_right_roi"};
      for (int i = 0; i < 2; ++i) {
        const auto it = rec->data.find(kSides[i]);
        if (it == rec->data.end() || !it->is_object()) continue;
        nlohmann::json entry = fitEntry(kComponents[i], *it);
        pointCounts[kCountKeys[i]] = entry["point_count"];
        fits.push_back(std::move(entry));
      }
    }
  }
  bundle["fits"] = std::move(fits);

  nlohmann::json effectiveRoi = nlohmann::json::object();
  static const char* kRoiPorts[5] = {"roiOverall", "roiFlushBase", "roiGapLeft", "roiFlushRef",
                                     "roiGapRight"};
  static const char* kRoiKeys[5] = {"overall", "flush_base", "gap_left", "flush_ref", "gap_right"};
  for (int i = 0; i < 5; ++i) {
    if (!inputs.has(kRoiPorts[i])) continue;
    const lyflow::Box2D* box = inputs.get(kRoiPorts[i]).asBox2D();
    if (box != nullptr) effectiveRoi[kRoiKeys[i]] = boxMm(*box);
  }
  bundle["effective_roi"] = std::move(effectiveRoi);

  nlohmann::json icp = nlohmann::json::array();
  std::string roiSource = params.text("roiSource");
  if (inputs.has("alignment")) {
    const lyflow::Record* rec = inputs.get("alignment").asRecord();
    if (rec != nullptr) {
      const std::string templateId = rec->data.value("templateId", std::string("primary"));
      if (rec->data.contains("left") && rec->data["left"].is_object()) {
        icp.push_back(icpSide(rec->data["left"], "left", templateId, true));
      }
      if (rec->data.contains("right") && rec->data["right"].is_object()) {
        icp.push_back(icpSide(rec->data["right"], "right", templateId, true));
      }
      if (rec->data.value("globalCoarse", false)) {
        pointCounts[templateId + "_global_coarse_attempted"] = 1;
        pointCounts[templateId + "_global_coarse_succeeded"] =
            rec->data.value("globalSucceeded", false) ? 1 : 0;
      }
      if (roiSource.empty()) roiSource = rec->data.contains("rois") ? "template" : "config";
    }
  }
  bundle["icp"] = std::move(icp);

  std::string cropStatus = params.text("cropStatus");
  if (inputs.has("cropStatus")) {
    const lyflow::Record* rec = inputs.get("cropStatus").asRecord();
    if (rec != nullptr) {
      cropStatus = rec->data.value("status", std::string());
      pointCounts["roll_crop_applied"] = rec->data.value("applied", false) ? 1 : 0;
      pointCounts["roll_crop_reverted"] = cropStatus.rfind("reverted", 0) == 0 ? 1 : 0;
      if (roiSource.empty()) roiSource = "model";
    }
  }
  bundle["crop_status"] = cropStatus;

  nlohmann::json fallback = nlohmann::json();
  if (inputs.has("fallback")) {
    const lyflow::Record* rec = inputs.get("fallback").asRecord();
    if (rec != nullptr) {
      fallback = rec->data;
      const std::string reason = rec->data.value("reason", std::string());
      bundle["fallback_reason"] = reason.empty() ? nlohmann::json() : nlohmann::json(reason);
      if (rec->data.value("choice", std::string("a")) == "b") roiSource = "template";
    }
  }
  if (!bundle.contains("fallback_reason")) bundle["fallback_reason"] = nlohmann::json();
  bundle["fallback"] = std::move(fallback);
  bundle["roi_source"] = roiSource;

  std::size_t inputCount = 0;
  static const char* kCloudPorts[2] = {"cloudPrimary", "cloudSecondary"};
  static const char* kCloudKeys[2] = {"input_primary", "input_secondary"};
  static const char* kPreprocessKeys[2] = {"preprocess_primary", "preprocess_secondary"};
  for (int i = 0; i < 2; ++i) {
    if (!inputs.has(kCloudPorts[i])) continue;
    const lyflow::PointCloud* cloud = inputs.get(kCloudPorts[i]).asCloud();
    if (cloud == nullptr) continue;
    pointCounts[kCloudKeys[i]] = cloud->pointCount();
    pointCounts[kPreprocessKeys[i]] = cloud->pointCount();
    inputCount += cloud->pointCount();
  }
  bundle["input_point_count"] = inputCount;
  bundle["preprocessed_point_count"] = inputCount;

  std::size_t mergedCount = 0;
  if (inputs.has("cloudMerged")) {
    const lyflow::PointCloud* cloud = inputs.get("cloudMerged").asCloud();
    if (cloud != nullptr) {
      mergedCount = cloud->pointCount();
      pointCounts["filter_after_left"] = mergedCount;
      pointCounts["filter_after_right"] = mergedCount;
    }
  }
  bundle["left_point_count"] = mergedCount;
  bundle["right_point_count"] = mergedCount;
  bundle["point_counts"] = std::move(pointCounts);

  bundle["timings"] = nlohmann::json::array();
  bundle["runtime_us"] = 0;
  bundle["total_runtime_us"] = 0;
  bundle["consistency_translation_delta_mm"] = nlohmann::json();
  bundle["consistency_rotation_delta_deg"] = nlohmann::json();
  bundle["consistency_mode"] = params.text("consistencyMode");
  bundle["consistency_retry_attempted"] = false;
  bundle["consistency_gate_failed"] = false;
  bundle["pack_versions"] = packVersions();
  bundle["graph_sha256"] = params.text("graphSha256");

  lyflow::Record record;
  record.type = "GapResultBundle";
  record.data = std::move(bundle);
  outputs.set("bundle", Data::record(std::move(record)));
  return Status::Ok();
}

Param textParam(const char* name, const char* label, const char* def, const char* doc) {
  Param p;
  p.name = name;
  p.type = ParamType::String;
  p.label = label;
  p.doc = doc;
  p.def = Value::text(def);
  return p;
}

Port optional(const char* name, const char* type, const char* label, const char* doc) {
  Port p{name, type, label, doc, false};
  p.acceptsError = true;
  return p;
}

}  // namespace

void registerResultBundle(Registry& r) {
  OperatorDesc op;
  op.id = "gap.result_bundle";
  op.version = "1.0.0";
  op.label = "结果汇总";
  op.category = "间隙/测量";
  op.keywords = {"bundle", "quality", "diagnostics", "汇总", "质量"};
  op.doc =
      "把一次测量的所有结果与质量指标汇聚成一个 GapResultBundle Record，"
      "字段与旧 QualityMetrics 一一对应，业务侧的 results.csv 与 metadata.json 由它填。";
  op.inputs = {
      Port{"gap", "Measurement", "Gap", "间隙测量值。", true},
      optional("flush", "Measurement", "Flush", "段差测量值。没有面差需求的测点可以不接，记 inactive。"),
      optional("roiFlushBase", "Box2D", "ROI Flush Base", "生效的段差基准面框。"),
      optional("roiFlushRef", "Box2D", "ROI Flush Ref", "生效的段差参考面框。"),
      optional("roiGapLeft", "Box2D", "ROI Gap Left", "生效的间隙左框。"),
      optional("roiGapRight", "Box2D", "ROI Gap Right", "生效的间隙右框。"),
      optional("roiOverall", "Box2D", "ROI Overall", "生效的整体框：整体 ROI 或跟随零件的裁剪窗。"),
      optional("fits", "Record", "Fits", "gap.fit_gap_circles 的 quality（两侧圆）。"),
      optional("fitBase", "Record", "Fit Base", "基准线 gap.fit_line 的 quality。"),
      optional("fitRef", "Record", "Fit Ref", "参考线 gap.fit_line 的 quality。"),
      optional("cropStatus", "Record", "Crop Status", "gap.roll_anchored_crop 的 status。"),
      optional("alignment", "Record", "Alignment", "gap.select_alignment 选中的 GapAlignment。"),
      optional("fallback", "Record", "Fallback", "flow.fallback 的 choice。"),
      optional("cloudPrimary", "PointCloud", "Cloud Primary", "剔过 NaN 的 Master 云，用来数点。"),
      optional("cloudSecondary", "PointCloud", "Cloud Secondary", "剔过 NaN 的 Slave 云。"),
      optional("cloudMerged", "PointCloud", "Cloud Merged", "合并并滤波之后的云。"),
  };
  op.outputs = {Port{"bundle", "Record", "Bundle", "GapResultBundle。", true}};
  op.params = {
      textParam("roiSource", "ROI Source", "",
                "留空时按接上的输入推断：有 alignment 是 template，有 cropStatus 是 model。"),
      textParam("cropStatus", "Crop Status", "",
                "cropStatus 端口没接时用它。模板路径上配了跟随裁剪窗却用不上，"
                "原算法记 skipped:no_model_roi。"),
      textParam("consistencyMode", "Consistency Mode", "off", "跨侧一致性闸门的模式，只做记录。"),
      textParam("graphSha256", "Graph SHA-256", "",
                "图文件的 sha256。执行器还没有注入口子，暂时由调用方填。"),
  };
  op.capabilities = {false, true, true};
  op.compute = &resultBundle;
  r.addOperator(std::move(op));
}

}  // namespace lyflow::packs::gap
