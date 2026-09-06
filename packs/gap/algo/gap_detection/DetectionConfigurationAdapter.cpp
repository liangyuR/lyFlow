#include "gap_detection/DetectionConfigurationAdapter.hpp"
#include "gap_detection/GapDetection.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "domain/config/settings_values.hpp"

namespace detection {
namespace {

void RequireMap(const YAML::Node& node, const std::string& name) {
  if (!node || !node.IsMap()) throw std::runtime_error(name + " must be a map");
}

domain::Roi parseRoi(const YAML::Node& node, const std::string& name) {
  const auto values = node.as<std::vector<double>>();
  if (values.size() != 4) throw std::runtime_error(name + " must contain four values");
  return {{{values[0], values[1], values[2], values[3]}}};
}

domain::IcpConfiguration parseIcpConfiguration(const YAML::Node& node) {
  return {node["max_matching_dist"].as<double>(), node["max_fitness_dist"].as<double>(),
          node["max_iteration_num"].as<int>(),    node["min_score"].as<int>(),
          node["num_neighbor"].as<int>(),         node["bidirection_align"].as<bool>()};
}

domain::CircleRadiusConfiguration parseRadiusConfiguration(const YAML::Node& node,
                                                           const std::string& side) {
  return {node["fixed_" + side + "_circle_radius"].as<bool>(),
          node[side + "_circle_radius_min"].as<double>(),
          node[side + "_circle_radius_max"].as<double>(),
          node[side + "_circle_radius"].as<double>()};
}

domain::CircleCompensationConfiguration parseCircleCompensationConfiguration(
    const YAML::Node& gap, const std::string& side) {
  const auto compensation = gap["compensation"];
  if (!compensation || !compensation[side]) return {};

  const auto node = compensation[side];
  RequireMap(node, "gap.compensation." + side);
  const auto center = node["nominal_center"].as<std::vector<double>>();
  if (center.size() != 2) {
    throw std::runtime_error("gap.compensation." + side +
                             ".nominal_center must contain two values");
  }
  return {node["enabled"].as<bool>(),
          node["preferred_camera"].as<std::string>(),
          center[0],
          center[1],
          node["maximum_center_shift"].as<double>(),
          node["maximum_point_gap"].as<double>(),
          node["inlier_distance"].as<double>(),
          node["minimum_inliers"].as<int>(),
          node["minimum_arc_coverage_deg"].as<double>(),
          node["maximum_rms_residual"].as<double>(),
          node["fallback_to_nominal"].as<bool>()};
}

domain::InitialPoseMode parseInitialPoseMode(const YAML::Node& align) {
  const auto node = align["initial_pose_mode"];
  if (!node) return domain::InitialPoseMode::kEndpointMidpointTranslation;
  if (!node.IsScalar()) {
    throw std::runtime_error("align.initial_pose_mode must be a scalar");
  }
  const auto mode = node.as<std::string>();
  if (mode == "endpoint_midpoint_translation") {
    return domain::InitialPoseMode::kEndpointMidpointTranslation;
  }
  if (mode == "centroid_translation") return domain::InitialPoseMode::kCentroidTranslation;
  if (mode == "pca_rigid_centroid") return domain::InitialPoseMode::kPcaRigidCentroid;
  throw std::runtime_error(
      "align.initial_pose_mode must be endpoint_midpoint_translation, "
      "centroid_translation, or pca_rigid_centroid");
}

domain::ConsistencyMode parseConsistencyMode(const YAML::Node& node) {
  if (!node) return domain::ConsistencyMode::kShadow;
  if (!node.IsScalar()) {
    throw std::runtime_error("align.robustness.consistency.mode must be a scalar");
  }
  const auto mode = node.as<std::string>();
  if (mode == "off") return domain::ConsistencyMode::kOff;
  if (mode == "shadow") return domain::ConsistencyMode::kShadow;
  if (mode == "enforce") return domain::ConsistencyMode::kEnforce;
  throw std::runtime_error("align.robustness.consistency.mode must be off, shadow, or enforce");
}

// All keys optional; defaults keep pre-hardening configs parsing (and, for degenerate-direction
// locking, behaving) unchanged. See DetectionConfiguration.hpp for the production defaults.
domain::RobustnessConfiguration parseRobustnessConfiguration(const YAML::Node& align) {
  domain::RobustnessConfiguration configuration;
  const auto node = align["robustness"];
  if (!node) return configuration;
  RequireMap(node, "align.robustness");
  if (node["degenerate_ratio"]) {
    configuration.degenerate_ratio = node["degenerate_ratio"].as<double>();
    if (!std::isfinite(configuration.degenerate_ratio) || configuration.degenerate_ratio <= 0 ||
        configuration.degenerate_ratio >= 1) {
      throw std::runtime_error("align.robustness.degenerate_ratio must be in (0, 1)");
    }
  }
  if (node["global_coarse"]) {
    configuration.global_coarse = node["global_coarse"].as<bool>();
  }
  if (const auto trust_region = node["trust_region"]) {
    RequireMap(trust_region, "align.robustness.trust_region");
    if (trust_region["max_translation_mm"]) {
      configuration.trust_region.max_translation_mm =
          trust_region["max_translation_mm"].as<double>();
    }
    if (trust_region["max_rotation_deg"]) {
      configuration.trust_region.max_rotation_deg = trust_region["max_rotation_deg"].as<double>();
    }
    if (!std::isfinite(configuration.trust_region.max_translation_mm) ||
        configuration.trust_region.max_translation_mm <= 0 ||
        !std::isfinite(configuration.trust_region.max_rotation_deg) ||
        configuration.trust_region.max_rotation_deg <= 0) {
      throw std::runtime_error("align.robustness.trust_region values must be positive");
    }
  }
  if (const auto consistency = node["consistency"]) {
    RequireMap(consistency, "align.robustness.consistency");
    configuration.consistency.mode = parseConsistencyMode(consistency["mode"]);
    if (consistency["max_translation_mm"]) {
      configuration.consistency.max_translation_mm = consistency["max_translation_mm"].as<double>();
    }
    if (consistency["max_rotation_deg"]) {
      configuration.consistency.max_rotation_deg = consistency["max_rotation_deg"].as<double>();
    }
    if (!std::isfinite(configuration.consistency.max_translation_mm) ||
        configuration.consistency.max_translation_mm <= 0 ||
        !std::isfinite(configuration.consistency.max_rotation_deg) ||
        configuration.consistency.max_rotation_deg <= 0) {
      throw std::runtime_error("align.robustness.consistency thresholds must be positive");
    }
  }
  return configuration;
}

// All keys optional, and a missing or non-map block yields the disabled default, so existing
// configurations keep parsing byte-for-byte unchanged.
domain::RollAnchoredCropConfiguration parseRollAnchoredCropConfiguration(const YAML::Node& common) {
  domain::RollAnchoredCropConfiguration configuration;
  const auto node = common["roll_anchored_crop"];
  if (!node || !node.IsMap()) return configuration;
  if (node["enabled"]) configuration.enabled = node["enabled"].as<bool>();
  if (node["half_width_mm"]) configuration.half_width_mm = node["half_width_mm"].as<double>();
  if (node["half_height_mm"]) configuration.half_height_mm = node["half_height_mm"].as<double>();
  if (node["min_points_kept"]) {
    configuration.min_points_kept = node["min_points_kept"].as<std::size_t>();
  }
  if (node["max_roll_box_height_mm"]) {
    configuration.max_roll_box_height_mm = node["max_roll_box_height_mm"].as<double>();
  }
  if (!std::isfinite(configuration.half_width_mm) || configuration.half_width_mm <= 0 ||
      !std::isfinite(configuration.half_height_mm) || configuration.half_height_mm <= 0) {
    throw std::runtime_error("common_settings.roll_anchored_crop half extents must be positive");
  }
  if (!std::isfinite(configuration.max_roll_box_height_mm) ||
      configuration.max_roll_box_height_mm <= 0) {
    throw std::runtime_error(
        "common_settings.roll_anchored_crop.max_roll_box_height_mm must be positive");
  }
  return configuration;
}

// Same shape as parseRollAnchoredCropConfiguration: the whole block and every field is
// optional, so configurations written before the intensity gate keep parsing byte-for-byte
// unchanged and default to the gate being off. Out-of-range values throw rather than being
// clamped -- a relative_threshold >= 1 would delete more than half of every ROI and could only
// ever be blocked by the max_drop_ratio fail-safe, which makes it a configuration mistake.
domain::IntensityGateConfiguration parseIntensityGateConfiguration(const YAML::Node& common) {
  domain::IntensityGateConfiguration configuration;
  const auto node = common["intensity_gate"];
  if (!node || !node.IsMap()) return configuration;
  if (node["using_gate"]) configuration.enabled = node["using_gate"].as<bool>();
  if (node["relative_threshold"]) {
    configuration.relative_threshold = node["relative_threshold"].as<double>();
  }
  if (node["min_counts"]) configuration.min_counts = node["min_counts"].as<double>();
  if (node["max_drop_ratio"]) configuration.max_drop_ratio = node["max_drop_ratio"].as<double>();
  if (node["min_points_kept"]) {
    configuration.min_points_kept = node["min_points_kept"].as<std::size_t>();
  }
  if (node["apply_flush_base"]) {
    configuration.apply_flush_base = node["apply_flush_base"].as<bool>();
  }
  if (node["apply_flush_ref"]) {
    configuration.apply_flush_ref = node["apply_flush_ref"].as<bool>();
  }
  if (node["apply_gap_left"]) configuration.apply_gap_left = node["apply_gap_left"].as<bool>();
  if (node["apply_gap_right"]) configuration.apply_gap_right = node["apply_gap_right"].as<bool>();
  if (!std::isfinite(configuration.relative_threshold) || configuration.relative_threshold < 0 ||
      configuration.relative_threshold >= 1) {
    throw std::runtime_error("common_settings.intensity_gate.relative_threshold must be in [0, 1)");
  }
  if (!std::isfinite(configuration.min_counts) || configuration.min_counts < 0 ||
      configuration.min_counts > 255) {
    throw std::runtime_error("common_settings.intensity_gate.min_counts must be in [0, 255]");
  }
  if (!std::isfinite(configuration.max_drop_ratio) || configuration.max_drop_ratio < 0 ||
      configuration.max_drop_ratio > 1) {
    throw std::runtime_error("common_settings.intensity_gate.max_drop_ratio must be in [0, 1]");
  }
  return configuration;
}

bool isSafeBasename(const std::string& value) {
  if (value.empty() || value == "." || value == ".." || value.front() == ' ' ||
      value.back() == ' ' || value.back() == '.' ||
      value.find_first_of("<>\"|?*") != std::string::npos) {
    return false;
  }
  if (!std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return character >= 0x20 && character != 0x7f && character != '/' && character != '\\' &&
               character != ':';
      })) {
    return false;
  }
  auto device_name = value.substr(0, value.find('.'));
  std::transform(
      device_name.begin(), device_name.end(), device_name.begin(), [](unsigned char character) {
        return character >= 'a' && character <= 'z' ? static_cast<char>(character - 'a' + 'A')
                                                    : static_cast<char>(character);
      });
  static const std::unordered_set<std::string> reserved_device_names = {
      "CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
      "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
  return reserved_device_names.count(device_name) == 0;
}

void validateLegacyTemplateSelectionMode(const YAML::Node& align) {
  const auto node = align["template_selection_mode"];
  if (!node) return;
  if (!node.IsScalar()) {
    throw std::runtime_error("align.template_selection_mode must be a scalar");
  }
  const auto mode = node.as<std::string>();
  if (mode == "primary_only" || mode == "primary_then_fallback" || mode == "best_icp") return;
  throw std::runtime_error(
      "align.template_selection_mode must be primary_only, primary_then_fallback, or "
      "best_icp");
}

domain::TemplateRoiConfiguration parseTemplateRois(const YAML::Node& node,
                                                   const std::string& name) {
  RequireMap(node, name);
  const auto flush = node["flush"];
  const auto gap = node["gap"];
  RequireMap(flush, name + ".flush");
  RequireMap(gap, name + ".gap");
  return {parseRoi(flush["base_roi"], name + ".flush.base_roi"),
          parseRoi(gap["left_roi"], name + ".gap.left_roi"),
          parseRoi(flush["ref_roi"], name + ".flush.ref_roi"),
          parseRoi(gap["right_roi"], name + ".gap.right_roi")};
}

std::optional<std::string> parseLegacyPrimaryTemplateId(const YAML::Node& align) {
  const auto node = align["primary_template_id"];
  if (!node) return std::nullopt;
  if (!node.IsScalar()) {
    throw std::runtime_error("align.primary_template_id must be a scalar");
  }
  const auto id = node.as<std::string>();
  if (!isSafeBasename(id)) {
    throw std::runtime_error("align.primary_template_id must be a safe name");
  }
  return id;
}

std::vector<domain::TemplateCandidate> parseTemplateCandidates(const YAML::Node& align) {
  const auto node = align["template_candidates"];
  if (node && !node.IsSequence()) {
    throw std::runtime_error("align.template_candidates must be a sequence");
  }
  std::vector<domain::TemplateCandidate> candidates;
  std::unordered_set<std::string> ids;
  const auto legacy_primary_id = parseLegacyPrimaryTemplateId(align);
  if (legacy_primary_id || !node) {
    const auto id = legacy_primary_id.value_or("primary");
    candidates.push_back({id, "left_template.pcd", "right_template.pcd"});
    ids.insert(id);
  }
  if (!node) return candidates;
  for (const auto& item : node) {
    RequireMap(item, "align.template_candidates item");
    if (!item["id"] || !item["id"].IsScalar() || !item["left"] || !item["left"].IsScalar() ||
        !item["right"] || !item["right"].IsScalar()) {
      throw std::runtime_error(
          "align.template_candidates items require scalar id, left, and right");
    }
    domain::TemplateCandidate candidate{item["id"].as<std::string>(),
                                        item["left"].as<std::string>(),
                                        item["right"].as<std::string>()};
    if (!isSafeBasename(candidate.id)) {
      throw std::runtime_error("align.template_candidates id must be a safe unique name");
    }
    if (!isSafeBasename(candidate.left) || !isSafeBasename(candidate.right)) {
      throw std::runtime_error("align.template_candidates left and right must be safe basenames");
    }
    if (!ids.insert(candidate.id).second) {
      throw std::runtime_error("align.template_candidates ids must be unique");
    }
    if (item["rois"]) {
      candidate.rois =
          parseTemplateRois(item["rois"], "align.template_candidates." + candidate.id + ".rois");
    }
    candidates.push_back(std::move(candidate));
  }
  return candidates;
}

}  // namespace

domain::AlignmentConfiguration parseAlignmentConfiguration(const YAML::Node& node) {
  const auto common = node["common_settings"];
  const auto align = node["align"];
  const auto camera_selection =
      ::domain::config::ParseCameraSelection(common["using_camera"].as<std::string>());
  if (!camera_selection) throw std::runtime_error("using_camera must be Both, Left, or Right");
  domain::AlignmentConfiguration configuration{
      parseRoi(common["overall_roi"], "overall_roi"),
      std::string(::domain::config::CameraSelectionStorageValue(*camera_selection)),
      align["method"].as<std::string>(), align["save_result"].as<bool>(),
      parseIcpConfiguration(align["ICP"])};
  if (common["overall_roi_mode"]) {
    const auto mode = common["overall_roi_mode"].as<std::string>();
    if (mode == "fixed") {
      configuration.overall_roi_mode = domain::OverallRoiMode::kFixed;
    } else if (mode == "auto_center") {
      configuration.overall_roi_mode = domain::OverallRoiMode::kAutoCenter;
    } else {
      throw std::runtime_error("overall_roi_mode must be fixed or auto_center");
    }
  }
  configuration.robustness = parseRobustnessConfiguration(align);
  return configuration;
}

domain::DetectionConfiguration parseDetectionConfiguration(const YAML::Node& node) {
  const auto common = node["common_settings"];
  const auto align = node["align"];
  const auto flush = node["flush"];
  const auto gap = node["gap"];
  const auto radius = gap["radius"];
  const auto vis = node["vis"];
  const auto segmentation_mode =
      ::domain::config::ParseSegmentationMode(common["seg_mode"].as<std::string>());
  if (!segmentation_mode) {
    throw std::runtime_error("seg_mode must be Each, Together, or ROI");
  }

  domain::DetectionConfiguration configuration{
      parseAlignmentConfiguration(node),
      {common["cluster_angle"].as<double>(),
       common["num_neighbor"].as<int>(),
       common["label_mode"].as<bool>(),
       *segmentation_mode == ::domain::config::SegmentationMode::kTogether,
       *segmentation_mode == ::domain::config::SegmentationMode::kRoi,
       {common["filter"]["using_removal"].as<bool>(),
        common["filter"]["filter_radius"].as<double>(),
        common["filter"]["filter_neighbors"].as<int>()},
       common["save_template"].as<bool>(),
       common["save_template_path"].as<std::string>(),
       common["line_fit_distance"].as<double>(),
       common["circle_fit_distance"].as<double>()},
      align["success_guide"].as<bool>(),
      align["align_cloud"].as<bool>(),
      {flush["enable"].as<bool>(), flush["base_side"].as<std::string>(),
       flush["base_type"].as<std::string>(), flush["ref_type"].as<std::string>(),
       parseRoi(flush["base_roi"], "flush.base_roi"), parseRoi(flush["ref_roi"], "flush.ref_roi"),
       flush["segment_points"].as<int>(), flush["offset"].as<double>(),
       flush["tolerances"]["nominal"].as<double>()},
      {gap["enable"].as<bool>(), gap["left_type"].as<std::string>(),
       gap["right_type"].as<std::string>(), parseRoi(gap["left_roi"], "gap.left_roi"),
       parseRoi(gap["right_roi"], "gap.right_roi"), gap["segment_points"].as<int>(),
       parseRadiusConfiguration(radius, "left"), parseRadiusConfiguration(radius, "right"),
       parseCircleCompensationConfiguration(gap, "left"),
       parseCircleCompensationConfiguration(gap, "right"),
       gap["camera_separated_circle_fallback"] ? gap["camera_separated_circle_fallback"].as<bool>()
                                               : true,
       gap["circle_fit_retry_distance"] ? gap["circle_fit_retry_distance"].as<double>() : 0.0,
       gap["offset"].as<double>(), gap["tolerances"]["nominal"].as<double>()},
      {vis["show_roi"].as<bool>(), vis["show_fit_geometry"].as<bool>()}};
  configuration.common.roll_anchored_crop = parseRollAnchoredCropConfiguration(common);
  configuration.common.intensity_gate = parseIntensityGateConfiguration(common);
  configuration.initial_pose_mode = parseInitialPoseMode(align);
  validateLegacyTemplateSelectionMode(align);
  configuration.template_candidates = parseTemplateCandidates(align);
  configuration.gap.camera_separated_preferred_camera =
      gap["camera_separated_preferred_camera"]
          ? gap["camera_separated_preferred_camera"].as<std::string>()
          : "Both";
  configuration.gap.camera_separated_select_closest_nominal =
      gap["camera_separated_select_closest_nominal"]
          ? gap["camera_separated_select_closest_nominal"].as<bool>()
      : gap["camera_separated_select_closest_radius"]
          ? gap["camera_separated_select_closest_radius"].as<bool>()
          : configuration.gap.camera_separated_circle_fallback &&
                configuration.gap.camera_separated_preferred_camera == "Both";
  configuration.gap.definition = gap["definition"] ? gap["definition"].as<std::string>() : "B";
  if (!configuration.gap.left_radius.fixed) {
    configuration.gap.left_compensation.enabled = false;
  }
  if (!configuration.gap.right_radius.fixed) {
    configuration.gap.right_compensation.enabled = false;
  }
  if (configuration.template_candidates.empty()) {
    throw std::runtime_error("align.template_candidates must contain at least one template");
  }

  if (configuration.common.neighbor_count < 1) {
    throw std::runtime_error("num_neighbor must be positive");
  }
  if (configuration.flush.segment_points < 1 || configuration.gap.segment_points < 1) {
    throw std::runtime_error("segment_points must be positive");
  }
  if (configuration.flush.base_side != "left" && configuration.flush.base_side != "right") {
    throw std::runtime_error("flush.base_side must be left or right");
  }
  const auto validate_compensation = [&](const domain::CircleCompensationConfiguration& value,
                                         const domain::CircleRadiusConfiguration& circle_radius,
                                         const std::string& side) {
    if (!value.enabled) return;
    const auto prefix = "gap.compensation." + side;
    if (!circle_radius.fixed || !std::isfinite(circle_radius.value) || circle_radius.value <= 0) {
      throw std::runtime_error(prefix + " requires a positive fixed circle radius");
    }
    if (!::domain::config::ParseCameraSelection(value.preferred_camera)) {
      throw std::runtime_error(prefix + ".preferred_camera must be Left, Right, or Both");
    }
    if (!std::isfinite(value.nominal_center_x) || !std::isfinite(value.nominal_center_y)) {
      throw std::runtime_error(prefix + ".nominal_center must be finite");
    }
    if (!std::isfinite(value.maximum_center_shift) || value.maximum_center_shift <= 0 ||
        !std::isfinite(value.maximum_point_gap) || value.maximum_point_gap <= 0 ||
        !std::isfinite(value.inlier_distance) || value.inlier_distance <= 0 ||
        !std::isfinite(value.maximum_rms_residual) || value.maximum_rms_residual <= 0) {
      throw std::runtime_error(prefix + " distance thresholds must be positive");
    }
    if (value.minimum_inliers < 3) {
      throw std::runtime_error(prefix + ".minimum_inliers must be at least 3");
    }
    if (!std::isfinite(value.minimum_arc_coverage_deg) || value.minimum_arc_coverage_deg <= 0 ||
        value.minimum_arc_coverage_deg > 360) {
      throw std::runtime_error(prefix + ".minimum_arc_coverage_deg must be in (0, 360]");
    }
  };
  validate_compensation(configuration.gap.left_compensation, configuration.gap.left_radius, "left");
  validate_compensation(configuration.gap.right_compensation, configuration.gap.right_radius,
                        "right");
  if (configuration.gap.circle_fit_retry_distance != 0 &&
      (!std::isfinite(configuration.gap.circle_fit_retry_distance) ||
       configuration.gap.circle_fit_retry_distance <= configuration.common.circle_fit_distance)) {
    throw std::runtime_error(
        "gap.circle_fit_retry_distance must be greater than "
        "common_settings.circle_fit_distance");
  }
  if (!::domain::config::ParseCameraSelection(
          configuration.gap.camera_separated_preferred_camera)) {
    throw std::runtime_error("gap.camera_separated_preferred_camera must be Left, Right, or Both");
  }
  if (configuration.gap.camera_separated_preferred_camera != "Both" &&
      !configuration.gap.camera_separated_circle_fallback) {
    throw std::runtime_error(
        "gap.camera_separated_preferred_camera requires "
        "gap.camera_separated_circle_fallback");
  }
  if (configuration.gap.camera_separated_select_closest_nominal) {
    if (!configuration.gap.camera_separated_circle_fallback) {
      throw std::runtime_error(
          "gap.camera_separated_select_closest_nominal requires "
          "gap.camera_separated_circle_fallback");
    }
    if (configuration.gap.camera_separated_preferred_camera != "Both") {
      throw std::runtime_error(
          "gap.camera_separated_select_closest_nominal cannot be combined with an explicit "
          "preferred camera");
    }
  }
  return configuration;
}

domain::CalibrationConfiguration parseCalibrationConfiguration(const YAML::Node& node) {
  RequireMap(node, "calibration configuration");
  if (node["schema_version"].as<int>() != 1) {
    throw std::runtime_error("unsupported calibration schema_version");
  }

  const auto cylinder = node["cylinder"];
  const auto alignment = node["alignment"];
  const auto icp = alignment["icp"];
  RequireMap(cylinder, "cylinder");
  RequireMap(alignment, "alignment");
  RequireMap(icp, "alignment.icp");

  const auto fit_distance_mm = cylinder["fit_distance_mm"].as<double>();
  const auto radius_mm = cylinder["radius_mm"].as<double>();
  const auto radius_tolerance_mm = cylinder["radius_tolerance_mm"].as<double>();
  const auto cylinder_neighbors = cylinder["neighbor_count"].as<int>();
  const auto cluster_angle = cylinder["cluster_angle_deg"].as<double>();

  const auto roi = parseRoi(alignment["overall_roi_mm"], "alignment.overall_roi_mm");
  const auto using_camera = alignment["using_camera"].as<std::string>();
  const auto method = alignment["method"].as<std::string>();
  const auto save_result = alignment["save_result"].as<bool>();

  domain::IcpConfiguration icp_configuration{icp["max_matching_distance_mm"].as<double>(),
                                             icp["max_fitness_distance_mm"].as<double>(),
                                             icp["max_iteration_count"].as<int>(),
                                             icp["minimum_score"].as<int>(),
                                             icp["neighbor_count"].as<int>(),
                                             icp["bidirectional"].as<bool>()};

  const auto& roi_values = roi.values;
  const bool valid_roi = std::all_of(roi_values.begin(), roi_values.end(),
                                     [](double value) { return std::isfinite(value); }) &&
                         roi_values[0] < roi_values[2] && roi_values[1] < roi_values[3];
  if (!valid_roi) throw std::runtime_error("alignment.overall_roi_mm is invalid");
  if (!::domain::config::ParseCameraSelection(using_camera)) {
    throw std::runtime_error("alignment.using_camera must be Both, Left, or Right");
  }
  if (method != "ICP") throw std::runtime_error("alignment.method must be ICP");

  if (!std::isfinite(fit_distance_mm) || fit_distance_mm <= 0) {
    throw std::runtime_error("cylinder.fit_distance_mm must be positive");
  }
  if (!std::isfinite(radius_mm) || radius_mm <= 0) {
    throw std::runtime_error("cylinder.radius_mm must be positive");
  }
  if (!std::isfinite(radius_tolerance_mm) || radius_tolerance_mm <= 0 ||
      radius_tolerance_mm >= radius_mm) {
    throw std::runtime_error(
        "cylinder.radius_tolerance_mm must be positive and smaller than radius_mm");
  }
  if (cylinder_neighbors < 2) {
    throw std::runtime_error("cylinder.neighbor_count must be at least 2");
  }
  if (!std::isfinite(cluster_angle) || cluster_angle <= 0 || cluster_angle > 180) {
    throw std::runtime_error("cylinder.cluster_angle_deg must be in (0, 180]");
  }

  if (!std::isfinite(icp_configuration.max_matching_distance) ||
      icp_configuration.max_matching_distance <= 0) {
    throw std::runtime_error("alignment.icp.max_matching_distance_mm must be positive");
  }
  if (!std::isfinite(icp_configuration.max_fitness_distance) ||
      icp_configuration.max_fitness_distance < icp_configuration.max_matching_distance) {
    throw std::runtime_error(
        "alignment.icp.max_fitness_distance_mm must be at least max_matching_distance_mm");
  }
  if (icp_configuration.max_iteration_count < 1) {
    throw std::runtime_error("alignment.icp.max_iteration_count must be positive");
  }
  if (icp_configuration.minimum_score < 0 || icp_configuration.minimum_score > 100) {
    throw std::runtime_error("alignment.icp.minimum_score must be in [0, 100]");
  }
  if (icp_configuration.neighbor_count < 2) {
    throw std::runtime_error("alignment.icp.neighbor_count must be at least 2");
  }

  domain::CalibrationConfiguration configuration;
  configuration.alignment = {roi, using_camera, method, save_result, icp_configuration};
  configuration.cylinder = {static_cast<float>(fit_distance_mm / 1000.0), radius_mm / 1000.0,
                            radius_tolerance_mm / 1000.0, cylinder_neighbors, cluster_angle};
  return configuration;
}

domain::CalibrationConfiguration loadCalibrationConfiguration(const std::string& path) {
  return parseCalibrationConfiguration(YAML::LoadFile(path));
}

void Alignment::setParams(const YAML::Node& params) {
  setConfiguration(parseAlignmentConfiguration(params));
}

void GapDetection::setParams(const YAML::Node& params) {
  setConfiguration(parseDetectionConfiguration(params));
}

}  // namespace detection
