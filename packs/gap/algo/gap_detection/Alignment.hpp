/*
 * Copyright (c) XYZ Robotics Inc. - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 * Author: jianming huang <jianming.huang@xyzrobotics.ai>, 2023/03/07
 */

#pragma once
#include "domain/detection/DetectionConfiguration.hpp"
#include "gap_core/MeasurementTypes.hpp"
#include "gap_detection/GapUtils.hpp"
#include <array>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>

namespace YAML {
class Node;
}

namespace detection {

struct AlignmentOutcome {
  Eigen::Matrix3f transform = Eigen::Matrix3f::Identity();
  double score = std::numeric_limits<double>::quiet_NaN();
  bool bidirectional = false;
  bool success = false;
  // Layer 2 (degenerate-direction locking): see IcpQuality for field semantics.
  double degenerate_eigenvalue_ratio = std::numeric_limits<double>::quiet_NaN();
  bool degenerate_locked = false;
};

// Replacement window for the model-first path's otherwise unbounded overall crop. box_mm is
// [x_min, z_min, x_max, z_max] in absolute sensor millimetres (same convention as domain::Roi);
// min_points_kept is the per-camera floor below which the crop is discarded and the unbounded
// pass redone, so a badly placed window can never starve the measurement.
struct OverallCropOverride {
  std::array<double, 4> box_mm{};
  std::size_t min_points_kept = 0;
};

class Alignment {
 public:
  Alignment();

  void setParams(const YAML::Node &params);

  void setConfiguration(const domain::AlignmentConfiguration &configuration) {
    configuration_ = configuration;
    align_times_ = 0;
  }

  virtual int64_t run(const PointCloud &left_camera_cloud,
                      const PointCloud &right_camera_cloud) = 0;

  bool no_trans_ = false;

 protected:
  AlignmentOutcome registerCloud2DICPOutcome(
      const PointCloud &src, const PointCloud &tgt,
      const Eigen::Matrix3f &init_pose_2d = Eigen::Matrix3f::Identity());

  virtual AlignmentOutcome alignOutcome(
      const PointCloud &src, const PointCloud &tgt,
      const Eigen::Matrix3f &init_pose_2d = Eigen::Matrix3f::Identity());

  Eigen::Isometry2f align(const PointCloud &src, const PointCloud &tgt,
                          const Eigen::Matrix3f &init_pose_2d = Eigen::Matrix3f::Identity());

  void saveAlignmentResult(const PointCloud &target, const Eigen::Matrix3f &transform);

  // skip_roi_crop: model-first short-circuit (per-sample roi_override present). The overall-roi
  // crop exists to feed alignment/segmentation a bounded window; an override supplies absolute
  // sensor-frame boxes derived from the raw profile, so the crop can only delete points the
  // override still needs (observed as roi_empty on displaced cars). The pass still drops
  // non-finite points (unbounded-box filter), and effective_roi["overall"] records the kept
  // clouds' bounding box instead of a configured window.
  // override_crop: only honoured when skip_roi_crop is true. Replaces the unbounded box with a
  // roll-anchored window that follows the part; reverted (and reported via rollCropReverted())
  // when it would leave fewer than min_points_kept points on a camera the configuration uses.
  void preprocess(const PointCloud &left_camera_cloud, const PointCloud &right_camera_cloud,
                  bool allow_single_camera = false, bool skip_roi_crop = false,
                  const std::optional<OverallCropOverride> &override_crop = std::nullopt);

  // Valid after preprocess(); both false unless an override_crop was supplied.
  bool rollCropApplied() const { return roll_crop_applied_; }
  bool rollCropReverted() const { return roll_crop_reverted_; }

  void setAlignmentComponent(std::string component) { alignment_component_ = std::move(component); }

  void resetAlignmentMetrics() { icp_metrics_.clear(); }

  const std::vector<gap::core::IcpQuality> &alignmentMetrics() const { return icp_metrics_; }

  domain::AlignmentConfiguration configuration_;

  PointCloud::Ptr left_camera_cloud_;
  PointCloud::Ptr right_camera_cloud_;
  PointCloud::Ptr left_cloud_;
  PointCloud::Ptr right_cloud_;
  Eigen::Matrix4f left_pose_;
  Eigen::Matrix4f right_pose_;
  std::string save_template_path_ = ".";
  double scale_;

  Eigen::Matrix4f axis_trans_;

  std::vector<gap::core::IcpQuality> icp_metrics_;
  std::string alignment_component_;

  // Millimetres, [x_min, y_min, x_max, y_max]; the overall ROI actually used by the most
  // recent preprocess() call, after auto-centering (if enabled). Exposed for ML export.
  std::array<double, 4> resolved_overall_roi_mm_{};

  // Roll-anchored crop outcome of the most recent preprocess() call; reset on every call.
  bool roll_crop_applied_ = false;
  bool roll_crop_reverted_ = false;

 private:
  int align_times_ = 0;
};

}  // namespace detection
