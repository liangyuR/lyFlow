#pragma once

#include "domain/detection/DetectionConfiguration.hpp"
#include "domain/detection/DetectionTypes.hpp"

#include <Eigen/Core>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace gap::core {

enum class MeasurementStatus { kInactive, kSuccess, kFailure };

// Camera capture and saved raw PCD profiles use X-Z. GUI editors display the same
// profiles in X-Y. Every caller must state its input frame so MeasurementEngine is
// the only place that decides whether an axis conversion is required.
enum class PointCloudFrame { kSensorXZ, kMeasurementXY };
enum class NonFinitePointPolicy { kRemove, kReject };

enum class FailureStage {
  kNone,
  kInput,
  kCoordinateTransform,
  kPreprocess,
  kSegmentation,
  kFiltering,
  kTemplateLoading,
  kIcp,
  kRoi,
  kFitting,
  kMeasurement,
  kOutput,
};

enum class FailureCode {
  kNone,
  kFileMissing,
  kMalformedTuple,
  kLengthMismatch,
  kNonFiniteValue,
  kEmptyCloud,
  kSplitFailed,
  kTemplateMissing,
  kIcpScoreLow,
  kRoiEmpty,
  kInsufficientPoints,
  kLineFitFailed,
  kCircleFitFailed,
  kRadiusOutOfRange,
  kArcCoverageLow,
  kUnsupportedGeometry,
  kInvalidConfiguration,
  kInvalidGeometry,
  kIoError,
  kUnexpectedError,
  kModelRoiFailed,
};

struct Failure {
  FailureStage stage = FailureStage::kNone;
  FailureCode code = FailureCode::kNone;
  std::string component;
  std::string message;

  explicit operator bool() const { return code != FailureCode::kNone; }
};

struct MeasurementValue {
  MeasurementStatus status = MeasurementStatus::kInactive;
  std::optional<double> value_mm;
  Failure failure;
};

struct FitQuality {
  std::string component;
  std::string model;
  std::size_t point_count = 0;
  std::size_t inlier_count = 0;
  double inlier_ratio = std::numeric_limits<double>::quiet_NaN();
  double rms_residual_mm = std::numeric_limits<double>::quiet_NaN();
  double max_residual_mm = std::numeric_limits<double>::quiet_NaN();
  double arc_coverage_deg = std::numeric_limits<double>::quiet_NaN();
  double radius_mm = std::numeric_limits<double>::quiet_NaN();
  // Circle models only: free / fixed / clamped / nominal. Empty for line models and for circle
  // fits where no radius bound was available to classify against.
  std::string radius_mode;
  // Circle models: fitted centre in millimetres. NaN for line models.
  double center_x_mm = std::numeric_limits<double>::quiet_NaN();
  double center_y_mm = std::numeric_limits<double>::quiet_NaN();
  // Line models: a point on the line and its unit direction, millimetres.
  double line_point_x_mm = std::numeric_limits<double>::quiet_NaN();
  double line_point_y_mm = std::numeric_limits<double>::quiet_NaN();
  double line_dir_x = std::numeric_limits<double>::quiet_NaN();
  double line_dir_y = std::numeric_limits<double>::quiet_NaN();
  // Populated only when ML export is enabled. Same length; mask marks fit inliers.
  // Coordinates are millimetres in the cloud frame, so a consumer can match them
  // back to the raw per-camera PCD slots. Empty when export is off.
  //
  // Deliberately double, not float: the cloud stores float32 metres, and a consumer
  // recomputing millimetres in float64 lands up to ~4e-6 mm away from the same product
  // taken in float32. Points are only ~0.46 um apart at the closest, so the match
  // tolerance has to sit below that, and float32 rounding alone was enough to miss.
  std::vector<std::array<double, 2>> roi_points_mm;
  std::vector<std::uint8_t> inlier_mask;
};

struct IcpQuality {
  std::string component;
  double score = std::numeric_limits<double>::quiet_NaN();
  bool bidirectional = false;
  bool success = false;
  Eigen::Matrix3f transform = Eigen::Matrix3f::Identity();
  std::string template_id = "primary";
  bool selected = true;
  // Layer 2 (degenerate-direction locking): ratio of the smallest to largest eigenvalue of the
  // post-hoc Gauss-Newton (x, y, theta) information matrix. Low values flag near-degenerate
  // registrations (e.g. translation along a profile surface); NaN if the analysis could not run
  // (too few correspondences).
  double degenerate_eigenvalue_ratio = std::numeric_limits<double>::quiet_NaN();
  bool degenerate_locked = false;
  // Layer 1 (trust region): deviation of this side's final transform from the joint/global
  // reference pose before any clamping, and whether clamping was applied.
  double trust_region_delta_translation_mm = std::numeric_limits<double>::quiet_NaN();
  double trust_region_delta_rotation_deg = std::numeric_limits<double>::quiet_NaN();
  bool trust_region_clamped = false;
};

struct StageTiming {
  FailureStage stage = FailureStage::kNone;
  std::string component;
  std::int64_t duration_us = 0;
};

struct QualityMetrics {
  std::size_t input_point_count = 0;
  std::size_t preprocessed_point_count = 0;
  std::size_t left_point_count = 0;
  std::size_t right_point_count = 0;
  std::unordered_map<std::string, std::size_t> point_counts;
  std::vector<FitQuality> fits;
  std::vector<IcpQuality> icp;
  std::vector<StageTiming> timings;
  std::int64_t runtime_us = 0;
  std::int64_t total_runtime_us = 0;
  // Millimetres, [x_min, y_min, x_max, y_max]; the boxes actually used after
  // auto-centering, template override and ICP alignment. Keys: overall,
  // flush_base, gap_left, flush_ref, gap_right.
  std::unordered_map<std::string, std::array<double, 4>> effective_roi;

  // Layer 3 (cross-side consistency gate). Deviation of the right side's transform from the
  // left side's, in the shared template frame: Delta = t_left^-1 * t_right, nominally identity.
  // Always populated when consistency_mode != "off" (shadow records but never gates).
  double consistency_translation_delta_mm = std::numeric_limits<double>::quiet_NaN();
  double consistency_rotation_delta_deg = std::numeric_limits<double>::quiet_NaN();
  std::string consistency_mode = "off";
  bool consistency_retry_attempted = false;
  bool consistency_gate_failed = false;

  // Actual source of the business ROIs used this run: config / template / override
  // (override is introduced by a later slice).
  std::string roi_source;

  // Outcome of the roll-anchored overall crop. Empty = crop not configured for this point;
  // otherwise "applied", "reverted:min_points" (the window would have left fewer than the
  // configured minimum points, so it was undone), "rejected:<skip_reason>" (see
  // detection::utils::RollAnchoredCropResult) or "skipped:no_model_roi" (the point enables the
  // crop but this run took the template/ICP path, which the crop does not touch).
  std::string crop_status;
  // SHA-256 of the equipment config file this measurement actually read, so a mid-shift parameter
  // edit stays attributable: results before and after the edit carry different hashes. Empty when
  // the hash could not be computed, which is never fatal to the measurement.
  std::string config_sha256;

  double minimumInlierRatio() const;
  double maximumFitResidualMm() const;
  double minimumIcpScore() const;
};

struct DebugGeometry {
  PointCloud left_cloud;
  PointCloud right_cloud;
  std::vector<Eigen::Vector2f> gap_points;
  std::vector<Eigen::Vector2f> flush_points;
  std::vector<Eigen::Matrix2f> rois;
  std::unordered_map<std::string, Eigen::Matrix2f> lines;
  std::unordered_map<std::string, Eigen::VectorXf> circles;
};

struct RunOptions {
  // PCL 1.12 uses this seed when its sample-consensus constructors receive
  // random=false. Keep the value explicit so batch metadata and future
  // backends have one stable determinism contract.
  std::uint64_t random_seed = 12345;
  PointCloudFrame input_frame = PointCloudFrame::kSensorXZ;
  NonFinitePointPolicy non_finite_point_policy = NonFinitePointPolicy::kRemove;
  bool capture_geometry = true;
  bool collect_partial_results = true;
  std::string algorithm_id = "current_cpp";
  // Off by default: populates FitQuality::roi_points_mm/inlier_mask, which scale
  // with point count. Everything else in QualityMetrics is unaffected.
  bool export_fit_points = false;
};

struct MeasurementRequest {
  std::string sample_id;
  PointCloud primary_cloud;
  PointCloud secondary_cloud;
  detection::domain::DetectionConfiguration configuration;
  RunOptions options;
};

struct MeasurementResult {
  std::string sample_id;
  std::string algorithm_id;
  bool success = false;
  MeasurementValue gap;
  MeasurementValue flush;
  Failure failure;
  QualityMetrics quality;
  DebugGeometry geometry;
};

const char* toString(MeasurementStatus status);
const char* toString(FailureStage stage);
const char* toString(FailureCode code);

// Stable two-decimal text used by the PLC, automatic page, database page and
// interactive page. Keeping it with the result contract prevents each caller
// from inventing different NAN/INACTIVE behavior.
std::string formatMeasurementValue(const MeasurementValue& value);

// Returns the radius of a fitted circle in millimetres. Circle-less measurements
// intentionally retain the existing "Disabled" label used by the GUI.
std::string formatFittedCircleRadiusMm(const DebugGeometry& geometry, const std::string& component,
                                       bool circle_enabled);

FailureCode classifyFailureCode(const std::string& message);

}  // namespace gap::core
