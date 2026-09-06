#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace detection::domain {

enum class InitialPoseMode {
  kEndpointMidpointTranslation = 0,
  kCentroidTranslation = 1,
  kPcaRigidCentroid = 2,
};

enum class LegacyTemplateSelectionMode {
  kPrimaryOnly = 0,
  kPrimaryThenFallback = 1,
  kBestIcp = 2,
};

enum class OverallRoiMode {
  kFixed = 0,
  kAutoCenter = 1,
};

// Cross-side ROI consistency gate (Layer 3 of the alignment robustness hardening). Shadow mode
// only records the left/right deviation in quality metrics; enforce mode retries the
// worse-scoring side and, if still inconsistent, fails the measurement.
enum class ConsistencyMode {
  kOff = 0,
  kShadow = 1,
  kEnforce = 2,
};

struct Roi {
  std::array<double, 4> values{};
};

struct TemplateRoiConfiguration {
  Roi flush_base;
  Roi gap_left;
  Roi flush_ref;
  Roi gap_right;
};

struct TemplateCandidate {
  std::string id;
  std::string left;
  std::string right;
  std::optional<TemplateRoiConfiguration> rois;
};

struct IcpConfiguration {
  double max_matching_distance = 0;
  double max_fitness_distance = 0;
  int max_iteration_count = 0;
  int minimum_score = 0;
  int neighbor_count = 0;
  bool bidirectional = false;
};

// Bounds for the Layer 1 trust region, in configuration units (millimetres / degrees). A side's
// final ICP transform is clamped back to this region around the joint/global reference pose
// rather than rejected outright.
struct TrustRegionConfiguration {
  double max_translation_mm = 3.0;
  double max_rotation_deg = 2.0;
};

struct ConsistencyConfiguration {
  ConsistencyMode mode = ConsistencyMode::kShadow;
  double max_translation_mm = 2.0;
  double max_rotation_deg = 1.5;
};

// Optional `align.robustness` block. Every field has a production-safe default so configs that
// predate this feature keep parsing and behaving the same modulo the (deliberately default-on)
// global coarse registration and trust region -- see DetectionConfigurationAdapter.cpp.
struct RobustnessConfiguration {
  double degenerate_ratio = 1.0e-3;
  bool global_coarse = true;
  TrustRegionConfiguration trust_region;
  ConsistencyConfiguration consistency;
};

struct AlignmentConfiguration {
  Roi overall_roi;
  std::string using_camera;
  std::string method;
  bool save_result = false;
  IcpConfiguration icp;
  OverallRoiMode overall_roi_mode = OverallRoiMode::kFixed;
  RobustnessConfiguration robustness;
};

struct CylinderCalibrationConfiguration {
  // Internal units are metres. Configuration adapters may expose friendlier units.
  float fit_distance = 0.00003F;
  double radius = 0.0075;
  double radius_tolerance = 0.0005;
  int neighbor_count = 50;
  double cluster_angle = 50;
};

struct CalibrationConfiguration {
  AlignmentConfiguration alignment{
      {{-500.0, -500.0, 500.0, 500.0}}, "Both", "ICP", false, {1.0, 10.0, 1000, 60, 10, false}};
  CylinderCalibrationConfiguration cylinder;
};

struct FilterConfiguration {
  bool enabled = false;
  double radius = 0;
  int neighbor_count = 0;
};

// Roll-anchored overall crop. The model-ROI path short-circuits the overall-roi crop entirely
// (an override box derived from the raw profile cannot be trusted to survive a fixed window), so
// nothing bounds the cloud any more and distant ghost lines or debris reach the measurement. The
// roll pair is the most reliably labelled feature the model produces, so anchor a
// follow-the-part crop on it instead of on a fixed sensor-frame window. Defaults are sized from
// the golden samples: every real feature lies within 21 mm of the roll-pair midpoint, while the
// known debris sits at roughly 90 mm.
struct RollAnchoredCropConfiguration {
  bool enabled = false;
  double half_width_mm = 35.0;
  double half_height_mm = 20.0;
  std::size_t min_points_kept = 50;
  // Maximum plausible z height of a single roll box. This is a trust gate, not a tuning knob --
  // a roll box taller than this means the labels themselves are suspect, so the crop is refused
  // for the frame instead of being derived from a box that is not a roll. Raise it only after
  // confirming on the point's own data that the tall box really is a roll: golden samples top out
  // at 5.39 mm, and the known failure shape was 16.38 mm.
  double max_roll_box_height_mm = 8.0;
};

// Intensity gate. Radius-outlier removal cannot delete a ghost line -- a secondary specular
// reflection has the same local density as the real surface -- and refine-v1 only cleans the
// boxes, never the points inside them. This drops points that sit inside a business ROI but are
// clearly darker than that ROI's own median return level. Evidence is deliberately thin: on the
// golden samples intensity does not separate (known bad points 9-13 vs real surface 10-14), so
// the feature ships default-off and is enabled per measurement point only after the separation
// has been measured on that point's own failures with tools/intensity_bench.html.
struct IntensityGateConfiguration {
  bool enabled = false;
  // Keep points whose intensity is at least max(min_counts, relative_threshold * median). Either
  // rule is switched off by setting it to 0; the absolute one blocks floor noise, the relative
  // one adapts to paint brightness.
  double relative_threshold = 0.5;
  double min_counts = 0.0;
  double max_drop_ratio = 0.5;
  std::size_t min_points_kept = 10;
  bool apply_flush_base = true;
  bool apply_flush_ref = true;
  // Rolls default off: intensity inside a single real roll spans 26-223 (incidence angle), so a
  // relative cutoff would cut the genuine dark end of the arc.
  bool apply_gap_left = false;
  bool apply_gap_right = false;
};

struct CommonDetectionConfiguration {
  double cluster_angle = 0;
  int neighbor_count = 0;
  bool label_mode = false;
  bool segment_together = false;
  bool roi_guided_segmentation = false;
  FilterConfiguration filter;
  bool save_template = false;
  std::string save_template_path;
  double line_fit_distance = 0;
  double circle_fit_distance = 0;
  RollAnchoredCropConfiguration roll_anchored_crop;
  IntensityGateConfiguration intensity_gate;
};

struct FlushConfiguration {
  bool enabled = false;
  std::string base_side;
  std::string base_type;
  std::string reference_type;
  Roi base_roi;
  Roi reference_roi;
  int segment_points = 0;
  double offset = 0;
  double nominal = 0;
};

struct CircleRadiusConfiguration {
  bool fixed = false;
  double minimum = 0;
  double maximum = 0;
  double value = 0;
};

struct CircleCompensationConfiguration {
  bool enabled = false;
  std::string preferred_camera = "Left";
  // Values are expressed in millimetres at the configuration boundary.
  double nominal_center_x = 0;
  double nominal_center_y = 0;
  double maximum_center_shift = 0;
  double maximum_point_gap = 0;
  double inlier_distance = 0;
  int minimum_inliers = 0;
  double minimum_arc_coverage_deg = 0;
  double maximum_rms_residual = 0;
  bool fallback_to_nominal = false;
};

struct GapConfiguration {
  bool enabled = false;
  std::string left_type;
  std::string right_type;
  Roi left_roi;
  Roi right_roi;
  int segment_points = 0;
  CircleRadiusConfiguration left_radius;
  CircleRadiusConfiguration right_radius;
  CircleCompensationConfiguration left_compensation;
  CircleCompensationConfiguration right_compensation;
  bool camera_separated_circle_fallback = true;
  double circle_fit_retry_distance = 0;
  double offset = 0;
  double nominal = 0;
  std::string camera_separated_preferred_camera = "Both";
  bool camera_separated_select_closest_nominal = true;
  // "B" (default): closest distance along the circle-center line. "A": distance between the
  // tangent lines perpendicular to the flush datum surface -- see GapDetection::detectGap.
  std::string definition{"B"};
};

struct VisualizationConfiguration {
  bool show_roi = false;
  bool show_fit_geometry = false;
};

struct DetectionConfiguration {
  AlignmentConfiguration alignment;
  CommonDetectionConfiguration common;
  bool success_guide = false;
  bool align_cloud = false;
  FlushConfiguration flush;
  GapConfiguration gap;
  VisualizationConfiguration visualization;
  InitialPoseMode initial_pose_mode = InitialPoseMode::kEndpointMidpointTranslation;
  // Retained only to keep incremental-build ABI compatibility with existing Release objects.
  // Runtime template selection does not read either legacy field.
  LegacyTemplateSelectionMode legacy_template_selection_mode =
      LegacyTemplateSelectionMode::kPrimaryOnly;
  std::string legacy_primary_template_id = "primary";
  std::vector<TemplateCandidate> template_candidates;
  // Batch-run per-sample override: absolute sensor millimetres, final override that wins over
  // template and config, and is never passed through the ICP transform. GUI paths always leave
  // this nullopt; parseDetectionConfiguration never populates it -- callers inject it directly.
  std::optional<TemplateRoiConfiguration> roi_override;
  // Echoed verbatim into quality.roi_source when roi_override is applied: "override" for a
  // manifest-supplied explicit box, "model" for a model-inferred one.
  std::string roi_override_source = "override";
};

}  // namespace detection::domain
