#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "gap_fine.h"

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

  // 四个角色框、整体框、对齐结果、裁剪状态都随 RoiSet 走（m8-plan L4/L5）；
  // roiOverall / alignment / cropStatus 三个端口接了的话以端口为准。
  RoiView rois;
  const bool haveRois = inputs.has("rois") && readRoiSet(inputs.get("rois"), &rois);
  const nlohmann::json* info = haveRois ? &rois.info->data : nullptr;
  auto infoField = [&](const char* key) -> const nlohmann::json* {
    if (!info) return nullptr;
    const auto it = info->find(key);
    return it == info->end() || it->is_null() ? nullptr : &*it;
  };

  nlohmann::json effectiveRoi = nlohmann::json::object();
  if (inputs.has("roiOverall")) {
    const lyflow::Box2D* box = inputs.get("roiOverall").asBox2D();
    if (box != nullptr) effectiveRoi["overall"] = boxMm(*box);
  } else if (const nlohmann::json* overall = infoField("overallMm")) {
    effectiveRoi["overall"] = *overall;
  }
  if (haveRois) {
    effectiveRoi["flush_base"] = boxMm(*rois.datum->asBox2D());
    effectiveRoi["gap_left"] = boxMm(*rois.seamLeft->asBox2D());
    effectiveRoi["flush_ref"] = boxMm(*rois.target->asBox2D());
    effectiveRoi["gap_right"] = boxMm(*rois.seamRight->asBox2D());
  }
  bundle["effective_roi"] = std::move(effectiveRoi);

  nlohmann::json icp = nlohmann::json::array();
  std::string roiSource = params.text("roiSource");
  if (roiSource.empty() && info) roiSource = info->value("source", std::string());
  const nlohmann::json* alignment = nullptr;
  if (inputs.has("alignment")) {
    const lyflow::Record* rec = inputs.get("alignment").asRecord();
    if (rec != nullptr) alignment = &rec->data;
  } else {
    alignment = infoField("alignment");
  }
  if (alignment != nullptr && alignment->is_object()) {
    const nlohmann::json& a = *alignment;
    const std::string templateId = a.value("templateId", std::string("primary"));
    if (a.contains("left") && a["left"].is_object()) {
      icp.push_back(icpSide(a["left"], "left", templateId, true));
    }
    if (a.contains("right") && a["right"].is_object()) {
      icp.push_back(icpSide(a["right"], "right", templateId, true));
    }
    if (a.value("globalCoarse", false)) {
      pointCounts[templateId + "_global_coarse_attempted"] = 1;
      pointCounts[templateId + "_global_coarse_succeeded"] = a.value("globalSucceeded", false) ? 1 : 0;
    }
    if (roiSource.empty()) roiSource = a.contains("rois") ? "template" : "config";
  }
  bundle["icp"] = std::move(icp);

  std::string cropStatus = params.text("cropStatus");
  const nlohmann::json* crop = nullptr;
  if (inputs.has("cropStatus")) {
    const lyflow::Record* rec = inputs.get("cropStatus").asRecord();
    if (rec != nullptr) crop = &rec->data;
  } else {
    crop = infoField("cropStatus");
  }
  if (crop != nullptr && crop->is_object()) {
    cropStatus = crop->value("status", std::string());
    pointCounts["roll_crop_applied"] = crop->value("applied", false) ? 1 : 0;
    pointCounts["roll_crop_reverted"] = cropStatus.rfind("reverted", 0) == 0 ? 1 : 0;
    if (roiSource.empty()) roiSource = "model";
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

  // 点数取自量测真正用的那一对云（定位之后的 ScanPair）：primary / secondary 是整体框或
  // 跟随裁剪窗裁过的，merged 是合并、去噪、再裁过的。
  std::size_t inputCount = 0;
  std::size_t mergedCount = 0;
  ScanView scan;
  if (inputs.has("scan") && readScanPair(inputs.get("scan"), &scan)) {
    static const char* kCloudKeys[2] = {"input_primary", "input_secondary"};
    static const char* kPreprocessKeys[2] = {"preprocess_primary", "preprocess_secondary"};
    const Data* clouds[2] = {scan.primary, scan.secondary};
    for (int i = 0; i < 2; ++i) {
      const std::size_t n = clouds[i]->asCloud()->pointCount();
      pointCounts[kCloudKeys[i]] = n;
      pointCounts[kPreprocessKeys[i]] = n;
      inputCount += n;
    }
    mergedCount = scan.merged->asCloud()->pointCount();
    pointCounts["filter_after_left"] = mergedCount;
    pointCounts["filter_after_right"] = mergedCount;
  }
  bundle["input_point_count"] = inputCount;
  bundle["preprocessed_point_count"] = inputCount;

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

/// 同上，再挂一条「这个端口只吃这种 Record」的契约（ADR-0024）。
/// 这个算子是 duck typing 的：接错一份 Record 不会报错，只会让 bundle 里对应的
/// 那几格悄悄空着 —— 而 bundle 正是业务侧写 results.csv 的唯一来源。
/// acceptsError 端口上的 Error 值不走契约检查，所以 fallback 接住的失败照样透得过去。
Port optionalRecord(const char* name, const char* label, const char* doc, const char* recordType) {
  return withContract(optional(name, "Record", label, doc), {{"recordType", recordType}});
}

}  // namespace

void registerResultBundle(Registry& r) {
  OperatorDesc op;
  op.id = "gap.result_bundle";
  op.version = "1.1.0";
  op.label = "结果汇总";
  op.category = "间隙/测量";
  op.keywords = {"bundle", "quality", "diagnostics", "汇总", "质量"};
  op.doc =
      "把一次测量的所有结果与质量指标汇聚成一个 GapResultBundle Record，"
      "字段与旧 QualityMetrics 一一对应，业务侧的 results.csv 与 metadata.json 由它填。\n"
      "只汇总不计算：没接的端口记 inactive 或缺省，不代表那一项真的合格。"
      "四个框与 roi_source 取自 rois（RoiSet），点数取自 scan（定位之后的 ScanPair）；"
      "left_point_count / right_point_count 填的都是合并云的点数；graphSha256 由调用方填，"
      "留空的 bundle 没法溯源到具体哪张图。";
  op.inputs = {
      Port{"gap", "Measurement", "Gap", "间隙测量值。", true},
      optional("flush", "Measurement", "Flush", "段差测量值。没有面差需求的测点可以不接，记 inactive。"),
      optional("rois", "Bundle<gap.RoiSet>", "ROIs",
               "定位给出的角色框（生效的那一份）。整体框、对齐结果、裁剪状态也从它的 info 里取。"),
      optional("scan", "Bundle<gap.ScanPair>", "Scan", "量测真正用的那一对云，用来数点。"),
      optional("roiOverall", "Box2D", "ROI Overall",
               "生效的整体框。不接就用 rois.info.overallMm。"),
      optionalRecord("fits", "Fits", "gap.fit_gap_circles 的 quality（两侧圆）。",
                     "GapFitQualityPair"),
      optionalRecord("fitBase", "Fit Base", "基准线 gap.fit_line 的 quality。", "GapFitQuality"),
      optionalRecord("fitRef", "Fit Ref", "参考线 gap.fit_line 的 quality。", "GapFitQuality"),
      optionalRecord("cropStatus", "Crop Status",
                     "gap.roll_anchored_crop 的 status。不接就用 rois.info.cropStatus。",
                     "GapRollCrop"),
      optionalRecord("alignment", "Alignment",
                     "gap.select_alignment 选中的 GapAlignment。不接就用 rois.info.alignment。",
                     "GapAlignment"),
      optionalRecord("fallback", "Fallback", "flow.fallback 的 choice。", "FallbackChoice"),
  };
  op.outputs = {withExample(Port{"bundle", "Record", "Bundle", "GapResultBundle。", true},
                            examples::resultBundle())};
  op.params = {
      textParam("roiSource", "ROI Source", "",
                "留空时取 rois.info.source；没接 rois 时按接上的输入推断："
                "有 alignment 是 template，有 cropStatus 是 model。"),
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
