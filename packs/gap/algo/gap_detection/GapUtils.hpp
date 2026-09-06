/*
 * Copyright (c) XYZ Robotics Inc. - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 * Author: jianming huang <jianming.huang@xyzrobotics.ai>, 2023/02/02
 */

#pragma once

#include <array>
#include <cstddef>
#include <limits>
#include <string>
#include <stdexcept>
#include <optional>
#include <vector>
#include "domain/detection/DetectionConfiguration.hpp"
#include "domain/detection/DetectionTypes.hpp"

#define CHECK_(condition, msg)       \
  if (!(condition)) {                \
    throw std::runtime_error((msg)); \
  }

namespace detection::utils {

struct FixedRadiusCircleOptions {
  double radius = 0;
  Eigen::Vector2f nominal_center = Eigen::Vector2f::Zero();
  double maximum_center_shift = 0;
  double maximum_point_gap = 0;
  double inlier_distance = 0;
  int minimum_inliers = 0;
  double minimum_arc_coverage_deg = 0;
  double maximum_rms_residual = 0;
};

struct FixedRadiusCircleResult {
  Eigen::VectorXf circle;
  pcl::Indices inliers;
  double arc_coverage_deg = 0;
  double rms_residual = 0;
  double center_shift = 0;
};

struct InitialPoseResult {
  Eigen::Matrix3f transform = Eigen::Matrix3f::Identity();
  bool fell_back_to_legacy = false;
};

struct TemplateIcpScore {
  std::string id;
  double left = 0;
  double right = 0;
  std::size_t configuration_order = 0;
};

InitialPoseResult computeInitialPose(const PointCloud &left_template,
                                     const PointCloud &right_template,
                                     const PointCloud &left_target, const PointCloud &right_target,
                                     domain::InitialPoseMode mode);

// Outcome of one roll-anchored crop derivation. `box_mm` is empty whenever a rejection fired, in
// which case `skip_reason` names it and the caller keeps the unbounded crop.
struct RollAnchoredCropResult {
  std::optional<std::array<double, 4>> box_mm;
  // Empty when box_mm is set. Otherwise one of: "disabled", "bad_config",
  // "degenerate_roll_box", "span", "roll_box_height".
  const char *skip_reason = "";
};

// Roll-anchored overall crop: centre = midpoint of the two roll-box centres, half extents from
// config. Derives no box (caller keeps the unbounded crop) when the config is disabled
// ("disabled"), a half extent or config.max_roll_box_height_mm is non-positive or non-finite
// ("bad_config"), either roll box is degenerate -- hi <= lo on either axis or a non-finite
// coordinate ("degenerate_roll_box"), either roll box is taller in z than
// config.max_roll_box_height_mm ("roll_box_height"), or the roll-pair
// x span falls outside [kMinRollPairSpanMm, kMaxRollPairSpanMm] ("span") -- an implausible span
// or height means the labels themselves are untrustworthy, and a crop derived from bad labels
// must not delete good points.
// Output is [x_lo, z_lo, x_hi, z_hi] in absolute sensor millimetres, same convention as Roi.
RollAnchoredCropResult computeRollAnchoredCropMm(
    const domain::Roi &gap_left, const domain::Roi &gap_right,
    const domain::RollAnchoredCropConfiguration &config);

// Result of one intensity-gate evaluation. `applied` is false whenever a fail-safe fired, in
// which case `dst` received a verbatim copy of the input and `kept_points == input_points`.
// `skip_reason` names the fail-safe: "disabled", "too_few_points", "zero_threshold",
// "min_points_kept", "max_drop_ratio", or "" once the gate really ran.
struct IntensityGateOutcome {
  std::size_t input_points = 0;
  std::size_t kept_points = 0;
  double median_intensity = std::numeric_limits<double>::quiet_NaN();
  double cutoff = std::numeric_limits<double>::quiet_NaN();
  bool applied = false;
  const char *skip_reason = "disabled";
};

// Drops points that are inside a business ROI but clearly darker than that ROI itself, aimed at
// the ghost lines a secondary specular reflection leaves behind. Intensity is the laser return
// level carried in `point.r`. The cutoff is max(min_counts, relative_threshold * median), so an
// absolute floor and a paint-adaptive relative rule combine without a mode switch. Four
// fail-safes (see IntensityGateOutcome::skip_reason) make a badly calibrated threshold cost at
// most one unfiltered frame instead of an emptied ROI. `dst` may alias `src`.
IntensityGateOutcome filterCloudByIntensity(const PointCloud &src, PointCloud *dst,
                                            const domain::IntensityGateConfiguration &config);

std::optional<std::size_t> selectBestTemplateByIcp(const std::vector<TemplateIcpScore> &scores,
                                                   double minimum_score, double epsilon = 1e-6);

void getCloudsOverlap(const PointCloud &cloud1, const PointCloud &cloud2, PointCloud *overlap);

unsigned int splitAndExtractCloud(const PointCloud &cloud, PointCloud *cloud_left,
                                  PointCloud *cloud_right, int neighbor, double cluster_angle,
                                  const std::string &save_prefix = "");

// True when both clouds hold the same 2D (x, y) point sequence, e.g. a mirrored template pair
// where one merged cloud was stored to both the left and right files.
bool cloudsIdentical2D(const PointCloud &a, const PointCloud &b);

// Classifies a fitted circle's radius against the configured search bounds, for ML export/
// evaluation. is_fixed takes precedence (the radius was pinned, not fitted free); otherwise a
// radius within relative 1e-6 of either bound is "clamped" (RANSAC pushed it to the search-space
// edge), and anything else is "free". minimum/maximum/radius must share the same unit.
std::string classifyRadiusMode(double radius, double minimum, double maximum, bool is_fixed);

// Fits a known-radius circle to one ordered, spatially continuous component at a time. Candidate
// centres are constrained around the nominal centre so disconnected glass reflections cannot win
// merely by contributing more unrelated points.
bool fitFixedRadiusCircleComponents(const PointCloud &cloud,
                                    const FixedRadiusCircleOptions &options,
                                    FixedRadiusCircleResult *result);

// 直线/圆拟合与盒裁剪已经进 lyflow_std_algo（ADR-0015）：
// algo/fit2d.h 的 fitLine2D / fitAxisLine2D / fitCircle2D / fitCircleFixedRadius2D、
// algo/crop2d.h 的 cropBox2D。本包只留领域逻辑。
void removeRadiusOutlier(const PointCloud &src_cloud, PointCloud *dst_cloud, double r,
                         int neighbor);
double lineCircleDistance(const Eigen::VectorXf &line, const Eigen::VectorXf &circle,
                          Eigen::Vector2f *start_pt, Eigen::Vector2f *end_pt);

double pointLineDistance(const Eigen::VectorXf &line, const Eigen::Vector2f &start_pt,
                         Eigen::Vector2f *end_pt);

double circleCircleDistance(const Eigen::VectorXf &circle1, const Eigen::VectorXf &circle2,
                            Eigen::Vector2f *start_pt, Eigen::Vector2f *end_pt);

double pointCircleDistance(const Eigen::VectorXf &circle, const Eigen::Vector2f &start_pt,
                           Eigen::Vector2f *end_pt);

double lineCloudDistance(const Eigen::VectorXf &line, const PointCloud &cloud,
                         Eigen::Vector2f *start_pt, Eigen::Vector2f *end_pt);

double circleCloudDistance(const Eigen::VectorXf &circle, const PointCloud &cloud,
                           Eigen::Vector2f *start_pt, Eigen::Vector2f *end_pt);

// the result is always >= 0
double pointCloudDistance(const PointCloud &cloud, const Eigen::Vector2f &start_pt,
                          Eigen::Vector2f *end_pt);

// the result is always >= 0
double cloudCloudDistance(const PointCloud &cloud1, const PointCloud &cloud2,
                          Eigen::Vector2f *start_pt, Eigen::Vector2f *end_pt);

// the result is always >= 0
double pointPointDistance(const Eigen::Vector2f &start_pt, const Eigen::Vector2f &end_pt);

Eigen::Matrix2f getIntersectionRL(const Eigen::Matrix2f &roi, const Eigen::VectorXf &line);

Eigen::VectorXf getLinefrom2Points(const Eigen::Vector2f &pt1, const Eigen::Vector2f &pt2);

Eigen::Matrix2f insertPoint2Segment(const Eigen::Matrix2f &segment, const Eigen::Vector2f &point);

Eigen::Matrix2f insertPoint2Segment(const Eigen::Matrix2f &segment, const PointT &point);

// assume input cloud is monotonous about x
PointT getEndPointofCloud(const PointCloud &cloud, bool is_left_end,
                          std::vector<int> *indices = nullptr);

Eigen::Vector2f rotatePoint(const Eigen::Vector2f &pt, const Eigen::Vector2f &center, double angle);
}  // namespace detection::utils
