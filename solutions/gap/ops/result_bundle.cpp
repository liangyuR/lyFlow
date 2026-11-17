#include "profile_migration.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <stdexcept>
#include <vector>

#include "lyflow/profile.h"
#include "lyflow/registry.h"
#include "lyflow_profile/context.h"
#include "profile_feature_bundle.h"
#include "profile_scene.h"
#ifdef LYFLOW_GAP_TEMPLATE
#include "profile_template.h"
#endif

namespace lyflow::packs::gap {
namespace {
namespace scene = profile_scene;
namespace contract = profile_contract;
using Json = nlohmann::json;
Status bad(const std::string& message, const char* port = "bundle") {
  return Status::Error(Phase::Execute, "bad_input", message, {}, port);
}
Status cancelled() { return Status::Error(Phase::Execute, "cancelled", "结果汇总已取消"); }
Json pick(const Json& value, const char* key) {
  const auto found = value.find(key);
  return found == value.end() ? Json() : *found;
}
Json native(const Record& value) { return {{"type", value.type}, {"data", value.data}}; }
Json count(const Json& value) {
  if (value.is_number_unsigned() || (value.is_number_integer() && value.get<std::int64_t>() >= 0))
    return value;
  return Json();
}
Json millimeters(const Json& value) {
  if (!value.is_number())
    return Json();
  const double result = value.get<double>() * 1000;
  if (!std::isfinite(result))
    throw std::invalid_argument("公共质量残差换算毫米后溢出");
  return Json(result);
}
Json measurement(const Data& value, bool active) {
  if (!active)
    return {{"status", "inactive"}, {"value_mm", nullptr}};
  if (const auto* failure = value.asError())
    return {{"status", "failure"},
            {"value_mm", nullptr},
            {"code", failure->code},
            {"message", failure->message}};
  const auto* input = value.asMeasurement();
  Json output{{"status", "failure"},
              {"value_mm", nullptr},
              {"unit", input->unit},
              {"message", input->message},
              {"verdict", input->verdict}};
  if (input->unit != "m" && input->unit != "mm") {
    output["code"] = "unsupported_measurement_unit";
    output["message"] = "测量单位必须为m或mm，不能标注为毫米读数";
    return output;
  }
  const double metric = input->unit == "m" ? input->value * 1000 : input->value;
  if (input->ok && std::isfinite(metric)) {
    output["status"] = "success";
    output["value_mm"] = metric;
  } else if (input->ok) {
    output["code"] = "non_finite_measurement";
  }
  return output;
}
Json fitEntry(const std::string& component, const Record& quality, const Data* line = nullptr) {
  const auto& q = quality.data;
  Json output{{"component", component},
              {"model", pick(q, "model")},
              {"radius_mode", pick(q, "radiusMode")},
              {"point_count", count(pick(q, "pointCount"))},
              {"inlier_count", count(pick(q, "inlierCount"))},
              {"inlier_ratio", pick(q, "inlierRatio")},
              {"rms_residual_mm", pick(q, "rmsResidualMm")},
              {"max_residual_mm", pick(q, "maxResidualMm")},
              {"arc_coverage_deg", pick(q, "arcCoverageDeg")},
              {"radius_mm", pick(q, "radiusMm")},
              {"center_x_mm", pick(q, "centerXMm")},
              {"center_y_mm", pick(q, "centerYMm")},
              {"line_point_x_mm", pick(q, "linePointXMm")},
              {"line_point_y_mm", pick(q, "linePointYMm")},
              {"line_dir_x", pick(q, "lineDirX")},
              {"line_dir_y", pick(q, "lineDirY")}};
  if (quality.type == "profile.LineFitRobust.v2") {
    output["model"] = "line";
    output["point_count"] = count(pick(q, "inputSlotCount"));
    output["valid_point_count"] = count(pick(q, "validSlotCount"));
    output["inlier_count"] = count(pick(q, "weightedInlierCount"));
    output["weighted_inlier_count"] = count(pick(q, "weightedInlierCount"));
    output["unique_inlier_count"] = count(pick(q, "uniqueInlierCount"));
    output["unique_inlier_ratio"] = pick(q, "uniqueInlierRatio");
    const auto valid = count(pick(q, "validSlotCount"));
    const auto weighted = count(pick(q, "weightedInlierCount"));
    output["inlier_ratio"] =
        !valid.is_null() && valid.get<std::uint64_t>() > 0 && !weighted.is_null()
            ? Json(weighted.get<double>() / valid.get<double>())
            : Json();
    output["inlier_count_semantics"] = "final_inlier_multiset";
    output["residual_aggregation"] = pick(q, "residualAggregation");
    output["rms_residual_mm"] = millimeters(pick(q, "weightedRmsResidual"));
    output["max_residual_mm"] = millimeters(pick(q, "maxResidual"));
  } else if (quality.type == "gap.ReferencePoint.v3") {
    output["line_fitted"] = false;
    output["inlier_count"] = nullptr;
    output["inlier_ratio"] = nullptr;
    output["rms_residual_mm"] = nullptr;
    output["max_residual_mm"] = nullptr;
  }
  if (line) {
    const auto* geometry = line->asLine2D();
    output["line_point_x_mm"] = static_cast<double>(geometry->point[0]) * 1000;
    output["line_point_y_mm"] = static_cast<double>(geometry->point[1]) * 1000;
    output["line_dir_x"] = geometry->dir[0];
    output["line_dir_y"] = geometry->dir[1];
    output["geometry_status"] = "bound_feature";
  } else {
    output["geometry_status"] =
        quality.type == "gap.ReferencePoint.v3" ? "unavailable" : "quality_only";
  }
  return output;
}
bool fitQuality(const Record* value) {
  return value->type == "profile.LineFitRobust.v2" || value->type == "gap.ReferencePoint.v3" ||
         value->type == "gap.FitQuality.v3" || value->type == "GapFitQuality";
}
Json boxMm(const Box2D& box) {
  return Json::array(
      {static_cast<double>(box.min[0]) * 1000, static_cast<double>(box.min[1]) * 1000,
       static_cast<double>(box.max[0]) * 1000, static_cast<double>(box.max[1]) * 1000});
}
Json icpSide(const Json& side, const char* component, const Json& templateId) {
  return {{"component", component},
          {"template_id", templateId},
          {"selected", true},
          {"score", pick(side, "score")},
          {"success", pick(side, "success")},
          {"bidirectional", pick(side, "bidirectional")},
          {"transform", pick(side, "transform")},
          {"degenerate_eigenvalue_ratio", pick(side, "degenerateRatio")},
          {"degenerate_locked", pick(side, "degenerateLocked")},
          {"trust_region_delta_translation_mm", pick(side, "trustTranslationMm")},
          {"trust_region_delta_rotation_deg", pick(side, "trustRotationDeg")},
          {"trust_region_clamped", pick(side, "trustClamped")}};
}
std::vector<std::string> packVersions() {
  std::vector<std::string> result;
  for (const auto& op : ensureRegistry().operators())
    if (!op.pack.empty() && std::find(result.begin(), result.end(), op.pack) == result.end())
      result.push_back(op.pack);
  std::sort(result.begin(), result.end());
  return result;
}
bool sameRecord(const Inputs& inputs, const char* name, const Record& expected, std::string& why) {
  if (!inputs.has(name))
    return true;
  const auto* value = inputs.get(name).asRecord();
  if (!value || value->type != expected.type || value->data != expected.data) {
    why = "独立质量端口与原子特征中的质量不一致";
    return false;
  }
  return true;
}
bool jointSpace(const ProfileDescriptor& current, ProfileDescriptor& expected, bool& haveExpected,
                std::string& why) {
  if (haveExpected && !contract::sameSpace(current, expected)) {
    why = "汇总输入的坐标帧、分析平面或手性不一致";
    return false;
  }
  expected = current;
  haveExpected = true;
  return true;
}
Status resultBundle(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                    ExecContext& ctx) {
  if (ctx.cancelled())
    return cancelled();
  if (!inputs.get("gap").asMeasurement() ||
      (inputs.has("flush") && !inputs.get("flush").asMeasurement() &&
       !inputs.get("flush").asError()))
    return bad("gap/flush必须为测量值，不能静默忽略错误端口类型", "gap");
  try {
    Json result{{"schemaVersion", 4},
                {"gap", measurement(inputs.get("gap"), true)},
                {"flush", measurement(inputs.get("flush"), inputs.has("flush"))}};
    Json nativeQuality = Json::object(), fits = Json::array(), counts = Json::object();
    std::string why;
    scene::ScanView scan;
    scene::RoiView rois;
    const bool haveScan = inputs.has("scan"), haveRois = inputs.has("rois");
    ProfileDescriptor expected;
    bool haveExpected = false;
    if (haveScan) {
      if (!scene::readScanPair(inputs.get("scan"), scan, why))
        return bad(why, "scan");
      expected = scan.primary->asProfile()->descriptor;
      haveExpected = true;
    }
    if (haveRois) {
      if (!scene::readRoiSet(inputs.get("rois"), rois, why))
        return bad(why, "rois");
      ProfileDescriptor space;
      if (!contract::geometrySpace(*rois.boxes[0], rois.contexts[0]->asRecord(), space, why) ||
          !jointSpace(space, expected, haveExpected, why))
        return bad(why, "rois");
    }
    auto featureSpace = [&](const Data& profile, const char* name) -> Status {
      if (!jointSpace(profile.asProfile()->descriptor, expected, haveExpected, why))
        return bad(why, name);
      return Status::Ok();
    };
    const char* qualityPorts[]{"fitBase", "fitRef"};
    const char* componentNames[]{"flush_base", "flush_ref"};
    const char* countNames[]{"flush_base_roi", "flush_ref_roi"};
    const char* featureNames[]{"baseFeature", "refFeature"};
    for (int i = 0; i < 2; ++i) {
      const Record* quality = nullptr;
      const Data* geometry = nullptr;
      scene::LineFeatureFields lineFeature;
      scene::ReferenceFeatureFields referenceFeature;
      if (inputs.has(featureNames[i])) {
        if (i == 0) {
          if (!scene::readLineFeature(inputs.get(featureNames[i]), lineFeature, why))
            return bad(why, featureNames[i]);
          if (auto status = featureSpace(lineFeature.profile, featureNames[i]); !status.ok)
            return status;
          quality = lineFeature.quality.asRecord();
          geometry = &lineFeature.line;
        } else {
          if (!scene::readReferenceFeature(inputs.get(featureNames[i]), referenceFeature, why))
            return bad(why, featureNames[i]);
          if (auto status = featureSpace(referenceFeature.profile, featureNames[i]); !status.ok)
            return status;
          quality = referenceFeature.quality.asRecord();
          if (referenceFeature.lineContext.asRecord()->type == "profile.Geometry.v3")
            geometry = &referenceFeature.line;
        }
        if (!sameRecord(inputs, qualityPorts[i], *quality, why))
          return bad(why, qualityPorts[i]);
      } else if (inputs.has(qualityPorts[i])) {
        quality = inputs.get(qualityPorts[i]).asRecord();
        if (!fitQuality(quality))
          return bad("fitBase/fitRef质量类型不受支持", qualityPorts[i]);
      }
      if (quality) {
        auto entry = fitEntry(componentNames[i], *quality, geometry);
        counts[countNames[i]] = entry["point_count"];
        fits.push_back(std::move(entry));
        nativeQuality[qualityPorts[i]] = native(*quality);
      }
    }
    const Record* circles = nullptr;
    scene::CirclePairFeatureFields pair;
    if (inputs.has("circleFeature")) {
      if (!scene::readCirclePairFeature(inputs.get("circleFeature"), pair, why))
        return bad(why, "circleFeature");
      for (std::size_t side = 0; side < 2; ++side)
        if (auto status = featureSpace(pair.profiles[side], "circleFeature"); !status.ok)
          return status;
      circles = &pair.quality;
      if (!sameRecord(inputs, "fits", *circles, why))
        return bad(why, "fits");
    } else if (inputs.has("fits")) {
      circles = inputs.get("fits").asRecord();
      if (circles->type != "gap.CircleFitPair.v3")
        return bad("双圆质量必须为gap.CircleFitPair.v3", "fits");
    }
    if (circles) {
      for (const auto* side : {"left", "right"}) {
        auto entry = fitEntry(std::string("gap_") + side,
                              Record{"gap.FitQuality.v3", circles->data.at(side)});
        counts[std::string("gap_") + side + "_roi"] = entry["point_count"];
        fits.push_back(std::move(entry));
      }
      nativeQuality["fits"] = native(*circles);
    }
    result["fits"] = std::move(fits);
    Json effective = Json::object();
    const auto* info = haveRois ? &rois.info->data : nullptr;
    if (inputs.has("roiOverall") != inputs.has("roiOverallContext"))
      return bad("整体ROI与其上下文必须成对提供", "roiOverallContext");
    if (inputs.has("roiOverall")) {
      ProfileDescriptor space;
      if (!contract::geometrySpace(inputs.get("roiOverall"),
                                   inputs.get("roiOverallContext").asRecord(), space, why) ||
          !jointSpace(space, expected, haveExpected, why))
        return bad(why, "roiOverallContext");
      effective["overall"] = boxMm(*inputs.get("roiOverall").asBox2D());
    } else if (info && !pick(*info, "overallMm").is_null())
      effective["overall"] = info->at("overallMm");
    if (haveRois) {
      const char* names[]{"flush_base", "flush_ref", "gap_left", "gap_right"};
      for (std::size_t i = 0; i < 4; ++i)
        effective[names[i]] = boxMm(*rois.boxes[i]->asBox2D());
    }
    result["effective_roi"] = std::move(effective);
    std::string roiSource = params.text("roiSource");
    if (info) {
      const auto actual = info->at("source").get<std::string>();
      if (!roiSource.empty() && roiSource != actual)
        return bad("roiSource参数与实际角色框来源不一致", "rois");
      roiSource = actual;
    }
    result["roi_source"] = roiSource;
    Json alignment;
#ifdef LYFLOW_GAP_TEMPLATE
    if (inputs.has("alignment")) {
      profile_template::AlignmentView view;
      if (!profile_template::readAlignment(inputs.get("alignment"), view, why))
        return bad(why, "alignment");
      if (auto status = featureSpace(*view.target, "alignment"); !status.ok)
        return status;
      alignment = view.info->data;
      if (info && !pick(*info, "alignment").is_null() && info->at("alignment") != alignment)
        return bad("独立配准输入与实际RoiSet的配准记录不一致", "alignment");
    }
#endif
    if (alignment.is_null() && info && !pick(*info, "alignment").is_null())
      alignment = info->at("alignment");
    Json icp = Json::array();
    if (!alignment.is_null()) {
      for (const auto* side : {"left", "right"})
        if (pick(alignment, side).is_object())
          icp.push_back(icpSide(alignment.at(side), side, pick(alignment, "templateId")));
      if (pick(alignment, "globalCoarse").is_boolean()) {
        const auto id = pick(alignment, "templateId");
        if (id.is_string()) {
          counts[id.get<std::string>() + "_global_coarse_attempted"] =
              alignment.at("globalCoarse").get<bool>() ? 1 : 0;
          counts[id.get<std::string>() + "_global_coarse_succeeded"] =
              pick(alignment, "globalSucceeded").is_boolean()
                  ? Json(alignment.at("globalSucceeded").get<bool>() ? 1 : 0)
                  : Json();
        }
      }
    }
    result["icp"] = std::move(icp);
    std::string cropStatus = params.text("cropStatus");
    Json crop;
    if (inputs.has("cropStatus")) {
      const auto* value = inputs.get("cropStatus").asRecord();
      if (value->type != "gap.RollCrop.v2")
        return bad("裁剪状态必须为gap.RollCrop.v2", "cropStatus");
      crop = value->data;
      if (info && !pick(*info, "cropStatus").is_null() && crop != info->at("cropStatus"))
        return bad("独立裁剪状态与实际角色框来源不一致", "cropStatus");
    } else if (info)
      crop = pick(*info, "cropStatus");
    if (!crop.is_null()) {
      cropStatus = crop.at("status").get<std::string>();
      if (pick(crop, "kind") == "gap.OverallCrop.v1") {
        counts["roll_crop_applied"] = 0;
        counts["roll_crop_reverted"] = 0;
      } else {
        if (crop.contains("method") || crop.contains("kind"))
          return bad("未知裁剪记录类型，不能按随动裁剪统计", "cropStatus");
        counts["roll_crop_applied"] = crop.at("applied").get<bool>() ? 1 : 0;
        counts["roll_crop_reverted"] = cropStatus.rfind("reverted", 0) == 0 ? 1 : 0;
      }
    }
    result["crop_status"] = cropStatus;
    Json decisions = Json::object();
    for (const auto& item :
         {std::make_pair("fallback", "location"), std::make_pair("fallbackLine", "line"),
          std::make_pair("fallbackReference", "reference"),
          std::make_pair("fallbackCircles", "circles")}) {
      if (!inputs.has(item.first))
        continue;
      const auto* value = inputs.get(item.first).asRecord();
      if (value->type != "FallbackChoice" ||
          (pick(value->data, "choice") != "a" && pick(value->data, "choice") != "b") ||
          !pick(value->data, "reason").is_string())
        return bad("回退决策必须为实际FallbackChoice", item.first);
      decisions[item.second] = value->data;
    }
    result["fallback_decisions"] = decisions;
    result["fallback"] = decisions.contains("location") ? decisions.at("location") : Json();
    result["fallback_reason"] =
        decisions.contains("location") &&
                !decisions.at("location").at("reason").get<std::string>().empty()
            ? decisions.at("location").at("reason")
            : Json();
    for (const auto* key :
         {"input_point_count", "preprocessed_point_count", "left_point_count", "right_point_count"})
      result[key] = nullptr;
    result["point_count_status"] = haveScan ? "available" : "unavailable";
    if (haveScan) {
      const auto primary = scan.primary->asProfile()->pointCount(),
                 secondary = scan.secondary->asProfile()->pointCount(),
                 merged = scan.merged->asProfile()->pointCount();
      counts["input_primary"] = primary;
      counts["input_secondary"] = secondary;
      counts["preprocess_primary"] = primary;
      counts["preprocess_secondary"] = secondary;
      counts["sensor_input_primary"] = scan.sensorPrimary->asProfile()->pointCount();
      counts["sensor_input_secondary"] = scan.sensorSecondary->asProfile()->pointCount();
      counts["filter_after_left"] = merged;
      counts["filter_after_right"] = merged;
      result["input_point_count"] = primary + secondary;
      result["preprocessed_point_count"] = primary + secondary;
      result["left_point_count"] = merged;
      result["right_point_count"] = merged;
    }
    result["point_counts"] = std::move(counts);
    result["timings"] = Json::array();
    result["timing_status"] = "unavailable";
    result["runtime_us"] = nullptr;
    result["total_runtime_us"] = nullptr;
    result["consistency_translation_delta_mm"] = nullptr;
    result["consistency_rotation_delta_deg"] = nullptr;
    result["consistency_mode"] = params.text("consistencyMode");
    result["consistency_status"] = "unavailable";
    result["consistency_retry_attempted"] = nullptr;
    result["consistency_gate_failed"] = nullptr;
    result["pack_versions"] = packVersions();
    result["graph_sha256"] = params.text("graphSha256");
    result["native_quality"] = std::move(nativeQuality);
    if (ctx.cancelled())
      return cancelled();
    outputs.set("bundle", Data::record(Record{"GapResultBundle", std::move(result)}));
    return Status::Ok();
  } catch (const std::exception& error) {
    return bad(error.what());
  }
}
Param text(const char* name, const char* initial, const char* description) {
  Param p;
  p.name = name;
  p.label = name;
  p.type = ParamType::String;
  p.def = Value::text(initial);
  p.doc = description;
  return p;
}
Port optional(const char* name, const char* type, const char* description) {
  return {name, type, name, description, false};
}
}
void registerResultBundle(Registry& registry) {
  OperatorDesc op;
  op.id = "gap.result_bundle";
  op.version = "4.0.0";
  declareGapGraphUpgrade(op, 2);
  op.migrations.push_back({3, nullptr, &contract::keepInputs});
  op.label = "原生间隙检测结果汇总";
  op.category = "间隙/测量";
  op.keywords = {"gap inspection result",
                 "native profile",
                 "quality diagnostics",
                 "检测结果",
                 "原生轮廓",
                 "质量汇总"};
  op.doc =
      "GapResultBundle schemaVersion4保留CSV业务字段；仅m/mm测量可换算value_mm。"
      "原子特征绑定数值、几何与观测，所有输入须处于同一坐标空间；独立质量缺失的几何字段保持null。"
      "当前量测槽数与原始传感器槽数分开记录；未提供scan时点数为null，未测时序/一致性保持unavailable。"
      "局部line/reference/circles回退仅记录各自决策，不重写实际ROI来源。旧major须显式gapProfile升级。";
  op.inputs = {{"gap", "Measurement", "间隙", "明确m/mm单位。", true},
               optional("flush", "Measurement", "可不接；失败仍记failure。"),
               optional("rois", "Bundle<gap.RoiSet.v3>", "实际定位角色框。"),
               optional("scan", "Bundle<gap.ScanPair.v3>", "实际量测原生轮廓对。"),
               optional("baseFeature", "Bundle<gap.LineFeature.v3>", "原子基准线特征。"),
               optional("refFeature", "Bundle<gap.ReferenceFeature.v3>", "原子参考特征。"),
               optional("circleFeature", "Bundle<gap.CirclePair.v3>", "原子双侧圆特征。"),
               optional("roiOverall", "Box2D", "实际整体框；须成对上下文。"),
               optional("roiOverallContext", "Record", "profile.Geometry.v3，同一空间。"),
               optional("fits", "Record", "gap.CircleFitPair.v3原生质量。"),
               optional("fitBase", "Record", "公共直线、参考点或原生inspection质量。"),
               optional("fitRef", "Record", "公共直线、参考点或原生inspection质量。"),
               optional("cropStatus", "Record", "gap.RollCrop.v2实际裁剪状态。"),
               optional("fallback", "Record", "定位FallbackChoice。"),
               optional("fallbackLine", "Record", "基准线FallbackChoice。"),
               optional("fallbackReference", "Record", "参考特征FallbackChoice。"),
               optional("fallbackCircles", "Record", "双圆FallbackChoice。")};
  op.inputs[1].acceptsError = true;
#ifdef LYFLOW_GAP_TEMPLATE
  op.inputs.push_back(optional("alignment", "Bundle<gap.Alignment.v3>", "原生模板配准。"));
#endif
  op.outputs = {{"bundle", "Record", "结果", "GapResultBundle schemaVersion4。", true}};
  op.params = {text("roiSource", "", "缺省取实际rois.info.source；显式值必须一致。"),
               text("cropStatus", "", "缺少实际裁剪来源时的声明文本。"),
               text("consistencyMode", "off", "只记录配置；没有实际测量来源则状态unavailable。"),
               text("graphSha256", "", "宿主提供实际图文件SHA256。")};
  op.capabilities = {false, true, true};
  op.compute = &resultBundle;
  registry.addOperator(std::move(op));
}
}
