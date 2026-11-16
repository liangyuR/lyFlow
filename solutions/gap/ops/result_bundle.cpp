#include "profile_migration.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <stdexcept>
#include <vector>

#include "lyflow/profile.h"
#include "lyflow/registry.h"
#include "lyflow_profile/provenance.h"
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
bool bounded(const Json& value, unsigned depth = 0) {
  if (depth > 20 || (value.is_number_float() && !std::isfinite(value.get<double>())) ||
      (value.is_string() && value.get_ref<const std::string&>().size() > 65536) ||
      (value.is_structured() && value.size() > 256))
    return false;
  if (value.is_structured())
    for (const auto& item : value)
      if (!bounded(item, depth + 1))
        return false;
  return true;
}
bool record(const Record* value, std::string& why) {
  if (!value || !value->data.is_object() || !bounded(value->data) ||
      value->data.dump().size() > 262144) {
    why = "汇总质量或来源必须是有界、有限的对象Record";
    return false;
  }
  return true;
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
  if (quality.type == "profile.LineFitRobust.v1") {
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
  } else if (quality.type == "gap.ReferencePoint.v2") {
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
        quality.type == "gap.ReferencePoint.v2" ? "unavailable" : "quality_only";
  }
  return output;
}
bool fitQuality(const Record* value, std::string& why) {
  if (!record(value, why))
    return false;
  const auto& q = value->data;
  if (value->type == "profile.LineFitRobust.v1") {
    ProfileDescriptor descriptor;
    if (pick(q, "schemaVersion") != 1 || pick(q, "unit") != "m" ||
        !contract::parse(pick(q, "context"), descriptor, why) ||
        !contract::fingerprint(pick(q, "geometryHash"))) {
      why = "公共直线质量需要实际Profile上下文与米制几何指纹";
      return false;
    }
    for (const auto* field :
         {"inputSlotCount", "validSlotCount", "uniqueInlierCount", "weightedInlierCount"})
      if (count(pick(q, field)).is_null()) {
        why = "公共直线质量缺少明确的原槽/观测/权重计数";
        return false;
      }
    for (const auto* field : {"weightedRmsResidual", "maxResidual"})
      if (!pick(q, field).is_number() || q.at(field).get<double>() < 0) {
        why = "公共直线质量缺少有限非负米制残差";
        return false;
      }
    return true;
  }
  if (value->type == "gap.ReferencePoint.v2")
    return pick(q, "schemaVersion") == 2 && pick(q, "lineFitted") == false &&
           (pick(q, "model") == "selected_point" || pick(q, "model") == "nearest_point") &&
           !count(pick(q, "pointCount")).is_null() && pick(q, "selection").is_object();
  if (value->type == "gap.FitQuality.v2" || value->type == "GapFitQuality")
    return true;
  why = "fitBase/fitRef质量类型不受支持";
  return false;
}
bool circleQuality(const Record* value, std::string& why) {
  if (!record(value, why) || value->type != "gap.CircleFitPair.v2" ||
      pick(value->data, "schemaVersion") != 2 || !pick(value->data, "left").is_object() ||
      !pick(value->data, "right").is_object()) {
    why = "双圆质量必须为gap.CircleFitPair.v2";
    return false;
  }
  for (const auto* side : {"left", "right"}) {
    const auto& q = value->data.at(side);
    ProfileDescriptor descriptor;
    if (pick(q, "logicalRole") != side ||
        !contract::parse(pick(q, "profileContext"), descriptor, why)) {
      why = "双圆质量必须保留真实左右角色的完整Profile上下文";
      return false;
    }
  }
  return true;
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
    Json result{{"schemaVersion", 3},
                {"gap", measurement(inputs.get("gap"), true)},
                {"flush", measurement(inputs.get("flush"), inputs.has("flush"))}};
    Json evidence = Json::object(), nativeQuality = Json::object(), fits = Json::array(),
         counts = Json::object();
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
      evidence["scan"] = {{"status", "bound_native_profiles"},
                          {"primary", contract::context(*scan.primary)},
                          {"secondary", contract::context(*scan.secondary)},
                          {"merged", contract::context(*scan.merged)},
                          {"sensor_primary", contract::context(*scan.sensorPrimary)},
                          {"sensor_secondary", contract::context(*scan.sensorSecondary)},
                          {"info", native(*scan.info)}};
    }
    if (haveRois) {
      if (!scene::readRoiSet(inputs.get("rois"), rois, why))
        return bad(why, "rois");
      ProfileDescriptor space;
      if (!contract::geometryMatchesPhysical(*rois.boxes[0], rois.contexts[0]->asRecord(), space,
                                             why) ||
          !jointSpace(space, expected, haveExpected, why))
        return bad(why, "rois");
      if (haveScan) {
        Data location;
        if (!scene::makeLocation(inputs.get("scan"), inputs.get("rois"), location, why))
          return bad(why, "rois");
      }
      evidence["rois"] = {{"status", haveScan ? "bound_to_scan" : "bound_geometry_only"},
                          {"info", native(*rois.info)}};
    }
    Json scanSources = Json::array();
    if (haveScan)
      for (const auto& pair : std::array<std::pair<const Data*, const Record*>, 3>{
               std::make_pair(scan.primary, &scan.primaryProvenance),
               std::make_pair(scan.secondary, &scan.secondaryProvenance),
               std::make_pair(scan.merged, scan.mergedProvenance)}) {
        Json source;
        if (!contract::profileSources(*pair.first, pair.second, source, why))
          return bad(why, "scan");
        for (const auto& value : source)
          if (std::find(scanSources.begin(), scanSources.end(), value) == scanSources.end())
            scanSources.push_back(value);
      }
    if (scanSources.size() > 64)
      return bad("scan来源联合超过64项预算", "scan");
    auto featureSpace = [&](const Data& profile, const Data& provenance,
                            const char* name) -> Status {
      if (!jointSpace(profile.asProfile()->descriptor, expected, haveExpected, why))
        return bad(why, name);
      Json sources;
      if (!contract::profileSources(profile, provenance.asRecord(), sources, why))
        return bad(why, name);
      if (haveScan && std::none_of(sources.begin(), sources.end(), [&](const Json& source) {
            return std::find(scanSources.begin(), scanSources.end(), source) != scanSources.end();
          }))
        return bad("原子特征来源未绑定本次scan的完整Profile上下文", name);
      evidence[name] = {{"status", "bound_native_feature"},
                        {"profile", contract::context(profile)},
                        {"sources", sources},
                        {"provenance", provenance.asRecord()->data}};
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
          if (auto status =
                  featureSpace(lineFeature.profile, lineFeature.provenance, featureNames[i]);
              !status.ok)
            return status;
          quality = lineFeature.quality.asRecord();
          geometry = &lineFeature.line;
        } else {
          if (!scene::readReferenceFeature(inputs.get(featureNames[i]), referenceFeature, why))
            return bad(why, featureNames[i]);
          if (auto status = featureSpace(referenceFeature.profile, referenceFeature.provenance,
                                         featureNames[i]);
              !status.ok)
            return status;
          quality = referenceFeature.quality.asRecord();
          if (referenceFeature.lineContext.asRecord()->type == "profile.Geometry.v2")
            geometry = &referenceFeature.line;
        }
        if (!sameRecord(inputs, qualityPorts[i], *quality, why))
          return bad(why, qualityPorts[i]);
      } else if (inputs.has(qualityPorts[i])) {
        quality = inputs.get(qualityPorts[i]).asRecord();
        if (!fitQuality(quality, why))
          return bad(why, qualityPorts[i]);
        evidence[featureNames[i]] = {{"status", "quality_only"}, {"geometry", "unavailable"}};
      }
      if (quality && quality->type == "profile.LineFitRobust.v1") {
        ProfileDescriptor space;
        if (!contract::parse(quality->data.at("context"), space, why) ||
            !jointSpace(space, expected, haveExpected, why))
          return bad(why, qualityPorts[i]);
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
        if (auto status = featureSpace(pair.profiles[side], pair.provenances[side],
                                       side ? "circle_right" : "circle_left");
            !status.ok)
          return status;
      circles = &pair.quality;
      if (!sameRecord(inputs, "fits", *circles, why))
        return bad(why, "fits");
    } else if (inputs.has("fits")) {
      circles = inputs.get("fits").asRecord();
      if (!circleQuality(circles, why))
        return bad(why, "fits");
    }
    if (circles) {
      for (const auto* side : {"left", "right"}) {
        ProfileDescriptor space;
        if (!contract::parse(circles->data.at(side).at("profileContext"), space, why) ||
            !jointSpace(space, expected, haveExpected, why))
          return bad(why, "fits");
        auto entry = fitEntry(std::string("gap_") + side,
                              Record{"gap.FitQuality.v2", circles->data.at(side)});
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
      const auto* binding = inputs.get("roiOverallContext").asRecord();
      ProfileDescriptor space;
      if (!inputs.get("roiOverall").asBox2D() || !binding ||
          binding->type != "profile.Geometry.v2" ||
          !contract::geometryMatchesPhysical(inputs.get("roiOverall"), binding, space, why) ||
          !jointSpace(space, expected, haveExpected, why))
        return bad(why.empty() ? "整体ROI需要严格米制几何上下文" : why, "roiOverallContext");
      effective["overall"] = boxMm(*inputs.get("roiOverall").asBox2D());
      evidence["roi_overall"] = binding->data;
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
      if (!jointSpace(view.target->asProfile()->descriptor, expected, haveExpected, why))
        return bad(why, "alignment");
      if (auto status =
              featureSpace(*view.target, Data::record(*view.targetProvenance), "alignment");
          !status.ok)
        return status;
      alignment = view.info->data;
      if (info && !pick(*info, "alignment").is_null() && info->at("alignment") != alignment)
        return bad("独立配准输入与实际RoiSet的配准记录不一致", "alignment");
      evidence["alignment"] = {{"status", "strict_native_alignment"},
                               {"target", contract::context(*view.target)}};
    }
#endif
    if (alignment.is_null() && info && !pick(*info, "alignment").is_null()) {
      alignment = info->at("alignment");
      evidence["alignment"] = {{"status", "roi_metadata"}};
    }
    Json icp = Json::array();
    if (!alignment.is_null()) {
      if (!alignment.is_object() || !bounded(alignment))
        return bad("配准元数据必须是有限对象", "alignment");
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
      if (!record(value, why) || value->type != "gap.RollCrop.v2")
        return bad("裁剪状态必须为gap.RollCrop.v2", "cropStatus");
      crop = value->data;
      if (info && !pick(*info, "cropStatus").is_null() && crop != info->at("cropStatus"))
        return bad("独立裁剪状态与实际角色框来源不一致", "cropStatus");
    } else if (info)
      crop = pick(*info, "cropStatus");
    if (!crop.is_null()) {
      if (!crop.is_object() || !pick(crop, "status").is_string() ||
          !pick(crop, "applied").is_boolean() || !bounded(crop))
        return bad("裁剪元数据缺少实际状态", "cropStatus");
      cropStatus = crop.at("status").get<std::string>();
      const bool overall = pick(crop, "kind") == "gap.OverallCrop.v1";
      if (overall) {
        if (inputs.has("cropStatus") || crop.size() != 9 ||
            !pick(crop, "schemaVersion").is_number_integer() || pick(crop, "schemaVersion") != 1 ||
            pick(crop, "method") != "overall" || cropStatus != "applied" ||
            crop.at("applied") != true || pick(crop, "bounds") != "open" || !info || !haveScan ||
            pick(pick(*info, "processing"), "method") != "template" ||
            !pick(crop, "boxMm").is_array() || crop.at("boxMm").size() != 4 ||
            pick(*info, "overallMm") != pick(crop, "boxMm") ||
            pick(scan.info->data, "crop") != crop || !pick(crop, "before").is_array() ||
            !pick(crop, "after").is_array() || crop.at("before").size() != 3 ||
            crop.at("after").size() != 3)
          return bad("整体裁剪需要版本化状态、实际窗口和同一轮廓对的完整处理记录", "cropStatus");
        const Data* profiles[] = {scan.primary, scan.secondary, scan.merged};
        for (std::size_t i = 0; i < 3; ++i) {
          const auto before = count(crop.at("before")[i]), after = count(crop.at("after")[i]);
          if (before.is_null() || after.is_null() || before < after ||
              after != profiles[i]->asProfile()->pointCount())
            return bad("整体裁剪点数与实际轮廓不一致", "cropStatus");
        }
        counts["roll_crop_applied"] = 0;
        counts["roll_crop_reverted"] = 0;
      } else {
        if (crop.contains("method") || crop.contains("kind"))
          return bad("未知裁剪记录类型，不能按随动裁剪统计", "cropStatus");
        counts["roll_crop_applied"] = crop.at("applied").get<bool>() ? 1 : 0;
        counts["roll_crop_reverted"] = cropStatus.rfind("reverted", 0) == 0 ? 1 : 0;
      }
      evidence["crop"] = crop;
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
      if (!record(value, why) || value->type != "FallbackChoice" ||
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
      evidence["point_count_semantics"] = {
          {"input_preprocess", "current_measurement_profile_slots_after_scene_processing"},
          {"sensor_input", "original_selected_sensor_profile_slots_including_invalid"},
          {"left_right", "same_current_merged_profile_slots"},
          {"fit_inliers", "legacy_final_inlier_multiset_with_separate_unique_observation_count"}};
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
    result["source_evidence"] = std::move(evidence);
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
  op.version = "3.0.0";
  declareGapGraphUpgrade(op, 2);
  op.label = "原生间隙检测结果汇总";
  op.category = "间隙/测量";
  op.keywords = {"gap inspection result",
                 "native profile",
                 "quality diagnostics",
                 "检测结果",
                 "原生轮廓",
                 "质量汇总"};
  op.doc =
      "GapResultBundle schemaVersion3保留CSV业务字段；仅m/mm测量可换算value_mm。"
      "原子特征严格绑定数值、几何、观测和来源；独立质量缺失的几何字段保持null。"
      "当前量测槽数与原始传感器槽数分开记录；未提供scan时点数为null，未测时序/一致性保持unavailable。"
      "局部line/reference/circles回退仅记录各自决策，不重写实际ROI来源。旧major须显式gapProfile升级。";
  op.inputs = {{"gap", "Measurement", "间隙", "明确m/mm单位。", true},
               optional("flush", "Measurement", "可不接；失败仍记failure。"),
               optional("rois", "Bundle<gap.RoiSet.v2>", "实际定位角色框。"),
               optional("scan", "Bundle<gap.ScanPair.v2>", "实际量测原生轮廓对。"),
               optional("baseFeature", "Bundle<gap.LineFeature.v2>", "原子基准线特征。"),
               optional("refFeature", "Bundle<gap.ReferenceFeature.v2>", "原子参考特征。"),
               optional("circleFeature", "Bundle<gap.CirclePair.v2>", "原子双侧圆特征。"),
               optional("roiOverall", "Box2D", "实际整体框；须成对上下文。"),
               optional("roiOverallContext", "Record", "profile.Geometry.v2，严格同空间。"),
               optional("fits", "Record", "gap.CircleFitPair.v2原生质量。"),
               optional("fitBase", "Record", "公共直线、参考点或原生inspection质量。"),
               optional("fitRef", "Record", "公共直线、参考点或原生inspection质量。"),
               optional("cropStatus", "Record", "gap.RollCrop.v2实际裁剪状态。"),
               optional("fallback", "Record", "定位FallbackChoice。"),
               optional("fallbackLine", "Record", "基准线FallbackChoice。"),
               optional("fallbackReference", "Record", "参考特征FallbackChoice。"),
               optional("fallbackCircles", "Record", "双圆FallbackChoice。")};
  op.inputs[1].acceptsError = true;
#ifdef LYFLOW_GAP_TEMPLATE
  op.inputs.push_back(
      optional("alignment", "Bundle<gap.Alignment.v2>", "严格验证的原生模板配准。"));
#endif
  op.outputs = {{"bundle", "Record", "结果", "GapResultBundle schemaVersion3。", true}};
  op.params = {text("roiSource", "", "缺省取实际rois.info.source；显式值必须一致。"),
               text("cropStatus", "", "缺少实际裁剪来源时的声明文本。"),
               text("consistencyMode", "off", "只记录配置；没有实际测量来源则状态unavailable。"),
               text("graphSha256", "", "宿主提供实际图文件SHA256。")};
  op.capabilities = {false, true, true};
  op.compute = &resultBundle;
  registry.addOperator(std::move(op));
}
}
