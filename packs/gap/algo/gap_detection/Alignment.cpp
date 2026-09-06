/*
 * Copyright (c) XYZ Robotics Inc. - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 * Author: jianming huang <jianming.huang@xyzrobotics.ai>, 2023/03/07
 */
#include "gap_detection/Alignment.hpp"
#include "gap_detection/Converter.hpp"

#include "algo/icp2d.h"
#include "algo/profile_geometry.h"
#include "std_bridge.hpp"

#include <pcl/io/pcd_io.h>
#include <pcl/kdtree/kdtree_flann.h>
#include <Eigen/Eigenvalues>
#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <vector>

namespace detection {
namespace {

struct DegeneracyAnalysis {
  bool valid = false;
  Eigen::Vector3d eigenvalues = Eigen::Vector3d::Zero();       // ascending, scaled/dimensionless
  Eigen::Matrix3d eigenvectors = Eigen::Matrix3d::Identity();  // columns match eigenvalues
  // Metres. Used to make the rotation DOF commensurate with translation: the scaled parameter is
  // (tx, ty, characteristic_length * theta_rad).
  double characteristic_length = 1.0;
};

DegeneracyAnalysis analyzeDegeneracy(const PointCloud &src, const PointCloud &tgt,
                                     const Eigen::Matrix3f &pose_2d, double max_matching_dist,
                                     int neighbor_count) {
  DegeneracyAnalysis result;
  const int k = std::max(neighbor_count, 3);
  if (src.empty() || tgt.size() < static_cast<std::size_t>(k)) return result;

  pcl::KdTreeFLANN<PointT> target_tree;
  target_tree.setInputCloud(tgt.makeShared());

  Eigen::Matrix3d information = Eigen::Matrix3d::Zero();
  double sum_sq_radius = 0;
  std::size_t used = 0;
  const double max_matching_dist_sq = max_matching_dist * max_matching_dist;
  std::vector<Eigen::Vector2d> neighborhood;
  neighborhood.reserve(static_cast<std::size_t>(k));

  for (const auto &point : src) {
    if (!std::isfinite(point.x) || !std::isfinite(point.y)) continue;
    const Eigen::Vector2f local(point.x, point.y);
    const Eigen::Vector3f homogeneous(point.x, point.y, 1.0F);
    const Eigen::Vector3f transformed = pose_2d * homogeneous;
    PointT query;
    query.x = transformed.x();
    query.y = transformed.y();
    query.z = 0;

    std::vector<int> nearest_index(1);
    std::vector<float> nearest_distance(1);
    if (target_tree.nearestKSearch(query, 1, nearest_index, nearest_distance) == 0) continue;
    if (nearest_distance[0] > max_matching_dist_sq) continue;

    std::vector<int> neighbor_indices(k);
    std::vector<float> neighbor_distances(k);
    if (target_tree.nearestKSearch(nearest_index[0], k, neighbor_indices, neighbor_distances) < k) {
      continue;
    }
    neighborhood.clear();
    for (auto index : neighbor_indices) neighborhood.emplace_back(tgt[index].x, tgt[index].y);
    // Local surface normal: eigenvector of the smallest neighbourhood covariance eigenvalue.
    const auto normal = lyflow::std_pc::estimateNormalFromNeighborhood(neighborhood);
    if (!normal) continue;

    const Eigen::Vector2f rotated = pose_2d.block<2, 2>(0, 0) * local;
    const auto terms = lyflow::std_pc::computePointToPlaneJacobian(
        Eigen::Vector2d(rotated.x(), rotated.y()), *normal);
    const Eigen::Vector3d jacobian(terms.translationX, terms.translationY, terms.rotation);
    information += jacobian * jacobian.transpose();
    sum_sq_radius += rotated.squaredNorm();
    ++used;
  }
  if (used < 3) return result;

  result.characteristic_length = std::max(1e-6, std::sqrt(sum_sq_radius / used));
  Eigen::Matrix3d scale = Eigen::Matrix3d::Identity();
  scale(2, 2) = 1.0 / result.characteristic_length;
  const Eigen::Matrix3d scaled_information = scale * information * scale;
  const Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> solver(scaled_information);
  if (solver.info() != Eigen::Success) return result;
  result.eigenvalues = solver.eigenvalues();
  result.eigenvectors = solver.eigenvectors();
  result.valid = true;
  return result;
}

// Post-hoc correction: express the ICP update (relative to the initial pose) in the degeneracy
// eigenbasis and zero out the components along near-degenerate directions, then recompose. This
// is the "compute the final transform delta ... zero the components ... recompose" fallback for
// black-box ICP implementations described in the hardening spec.
Eigen::Matrix3f lockDegenerateDirections(const Eigen::Matrix3f &init_pose_2d,
                                         const Eigen::Matrix3f &pose_2d,
                                         const DegeneracyAnalysis &analysis,
                                         double degenerate_ratio, bool *locked) {
  *locked = false;
  if (!analysis.valid) return pose_2d;
  const double max_eigenvalue = analysis.eigenvalues.maxCoeff();
  if (!(max_eigenvalue > 0)) return pose_2d;

  const Eigen::Matrix3f delta_3d = pose_2d * init_pose_2d.inverse();
  const double delta_theta =
      std::atan2(static_cast<double>(delta_3d(1, 0)), static_cast<double>(delta_3d(0, 0)));
  Eigen::Vector3d motion(delta_3d(0, 2), delta_3d(1, 2),
                         analysis.characteristic_length * delta_theta);

  for (int i = 0; i < 3; ++i) {
    if (analysis.eigenvalues[i] < degenerate_ratio * max_eigenvalue) {
      const Eigen::Vector3d direction = analysis.eigenvectors.col(i);
      motion -= motion.dot(direction) * direction;
      *locked = true;
    }
  }
  if (!*locked) return pose_2d;

  const double corrected_theta = motion.z() / analysis.characteristic_length;
  const float cosine = static_cast<float>(std::cos(corrected_theta));
  const float sine = static_cast<float>(std::sin(corrected_theta));
  Eigen::Matrix3f corrected_delta = Eigen::Matrix3f::Identity();
  corrected_delta(0, 0) = cosine;
  corrected_delta(0, 1) = -sine;
  corrected_delta(1, 0) = sine;
  corrected_delta(1, 1) = cosine;
  corrected_delta(0, 2) = static_cast<float>(motion.x());
  corrected_delta(1, 2) = static_cast<float>(motion.y());
  return corrected_delta * init_pose_2d;
}

std::optional<Eigen::Vector2f> robustCloudCenter(const PointCloud &cloud) {
  std::vector<float> xs;
  std::vector<float> ys;
  xs.reserve(cloud.size());
  ys.reserve(cloud.size());
  for (const auto &point : cloud) {
    if (!std::isfinite(point.x) || !std::isfinite(point.y)) continue;
    xs.push_back(point.x);
    ys.push_back(point.y);
  }
  if (xs.empty()) return std::nullopt;

  const auto median = [](std::vector<float> *values) {
    const auto middle = values->begin() + values->size() / 2;
    std::nth_element(values->begin(), middle, values->end());
    const float upper = *middle;
    if (values->size() % 2 != 0) return upper;
    const auto lower = std::max_element(values->begin(), middle);
    return (*lower + upper) * 0.5F;
  };
  return Eigen::Vector2f{median(&xs), median(&ys)};
}

}  // namespace

Alignment::Alignment() {
  left_camera_cloud_.reset(new PointCloud);
  right_camera_cloud_.reset(new PointCloud);
  left_cloud_.reset(new PointCloud);
  right_cloud_.reset(new PointCloud);
  scale_ = 1000;
}

AlignmentOutcome Alignment::registerCloud2DICPOutcome(const PointCloud &src, const PointCloud &tgt,
                                                      const Eigen::Matrix3f &init_pose_2d) {
  const auto &params = configuration_.icp;
  double max_matching_dist = params.max_matching_distance / 1000;
  double max_fitness_dist = params.max_fitness_distance / 1000;
  int max_iteration_num = params.max_iteration_count;
  int min_score = params.minimum_score;
  int neighbor = params.neighbor_count;
  bool bidirection = params.bidirectional;
  bool used_bidirection = false;

  Eigen::Matrix3f pose_2d = Eigen::Matrix3f::Identity();
  double fitness = 0;

  lyflow::std_pc::Icp2DOptions icp_options;
  icp_options.maxMatchingDistance = max_matching_dist;
  icp_options.fitnessDistance = max_fitness_dist;
  icp_options.maxIterationCount = max_iteration_num;
  icp_options.normalKnn = neighbor;
  lyflow::std_pc::Icp2D icp_2d(icp_options);
  icp_2d.setSource(src);
  icp_2d.setTarget(tgt);
  auto icp_outcome = icp_2d.align(init_pose_2d);
  pose_2d = icp_outcome.transform;
  fitness = static_cast<double>(icp_outcome.fitness) * 100;
  if (fitness < min_score) {
    if (bidirection) {
      used_bidirection = true;
      icp_2d.setSource(tgt);
      icp_2d.setTarget(src);
      icp_outcome = icp_2d.align(init_pose_2d.inverse());
      pose_2d = icp_outcome.transform;
      fitness = static_cast<double>(icp_outcome.fitness) * 100;
    }
    if (fitness >= min_score) pose_2d = pose_2d.inverse();
  }

  const bool success = fitness >= min_score;

  // Layer 2: degenerate-direction locking. Only meaningful for a converged result; the raw
  // pose otherwise still carries the (failed) ICP output for diagnostics upstream.
  double degenerate_eigenvalue_ratio = std::numeric_limits<double>::quiet_NaN();
  bool degenerate_locked = false;
  Eigen::Matrix3f final_pose = pose_2d;
  if (success) {
    const auto analysis = analyzeDegeneracy(src, tgt, pose_2d, max_matching_dist, neighbor);
    if (analysis.valid) {
      degenerate_eigenvalue_ratio =
          analysis.eigenvalues.minCoeff() / analysis.eigenvalues.maxCoeff();
      final_pose =
          lockDegenerateDirections(init_pose_2d, pose_2d, analysis,
                                   configuration_.robustness.degenerate_ratio, &degenerate_locked);
    }
  }

  AlignmentOutcome outcome{final_pose, fitness, used_bidirection, success};
  outcome.degenerate_eigenvalue_ratio = degenerate_eigenvalue_ratio;
  outcome.degenerate_locked = degenerate_locked;
  return outcome;
}

AlignmentOutcome Alignment::alignOutcome(const PointCloud &src, const PointCloud &tgt,
                                         const Eigen::Matrix3f &init_pose_2d) {
  const auto &params = configuration_;
  const auto &method = params.method;
  if (method == "ICP") {
    return registerCloud2DICPOutcome(src, tgt, init_pose_2d);
  }
  throw std::runtime_error("Not Implemented Method");
}

void Alignment::saveAlignmentResult(const PointCloud &target, const Eigen::Matrix3f &transform) {
  PointCloud::Ptr transformed_cloud(new PointCloud);
  const auto transform_3d = conv::EigenMatrix3fto4f(transform.inverse(), conv::kZ);
  pcl::transformPointCloud(target, *transformed_cloud, transform_3d);
  pcl::io::savePCDFile(save_template_path_ + "/reg" + std::to_string(align_times_++) + ".pcd",
                       *transformed_cloud, true);
}

Eigen::Isometry2f Alignment::align(const PointCloud &src, const PointCloud &tgt,
                                   const Eigen::Matrix3f &init_pose_2d) {
  const auto outcome = alignOutcome(src, tgt, init_pose_2d);
  gap::core::IcpQuality metric;
  metric.component = alignment_component_;
  metric.score = outcome.score;
  metric.bidirectional = outcome.bidirectional;
  metric.success = outcome.success;
  metric.transform = outcome.transform;
  metric.degenerate_eigenvalue_ratio = outcome.degenerate_eigenvalue_ratio;
  metric.degenerate_locked = outcome.degenerate_locked;
  icp_metrics_.push_back(std::move(metric));
  if (!outcome.success) {
    std::ostringstream message;
    message << "ICP Failed: score=" << std::fixed << std::setprecision(2) << outcome.score
            << ", minimum_score=" << configuration_.icp.minimum_score;
    throw std::runtime_error(message.str());
  }
  if (configuration_.save_result) saveAlignmentResult(tgt, outcome.transform);
  return Eigen::Isometry2f(outcome.transform);
}

void Alignment::preprocess(const PointCloud &left_camera_cloud,
                           const PointCloud &right_camera_cloud, bool allow_single_camera,
                           bool skip_roi_crop,
                           const std::optional<OverallCropOverride> &override_crop) {
  roll_crop_applied_ = false;
  roll_crop_reverted_ = false;
  // put cloud in x-y plane from x-z plane
  PointCloud::Ptr left_camera_cloud_xy(new PointCloud);
  PointCloud::Ptr right_camera_cloud_xy(new PointCloud);
  if (no_trans_) {
    axis_trans_ = Eigen::Matrix4f::Identity();
    left_camera_cloud_xy = left_camera_cloud.makeShared();
    right_camera_cloud_xy = right_camera_cloud.makeShared();
  } else {
    axis_trans_ =
        conv::swapCloudAxis(left_camera_cloud, left_camera_cloud_xy.get(), conv::kY, conv::kZ);
    conv::swapCloudAxis(right_camera_cloud, right_camera_cloud_xy.get(), conv::kY, conv::kZ);
  }

  // Extract cloud in ROI. Auto mode keeps the configured width and height, but follows the
  // robust centre of the currently selected camera clouds.
  const auto &roi = configuration_.overall_roi.values;
  std::array<double, 4> resolved_roi = roi;
  constexpr double kUnboundedMm = 1e7;
  const std::array<double, 4> unbounded_roi{-kUnboundedMm, -kUnboundedMm, kUnboundedMm,
                                            kUnboundedMm};
  if (skip_roi_crop) {
    // Unbounded box: keeps cropBox2D's drop-non-finite semantics without deleting any
    // finite point. resolved_overall_roi_mm_ is replaced by the kept clouds' bounding box below.
    resolved_roi = unbounded_roi;
    if (override_crop) {
      // Roll-anchored window: a real box that follows the part, so it is reported as-is and the
      // bounding-box diagnostic below is skipped.
      resolved_roi = override_crop->box_mm;
      roll_crop_applied_ = true;
    }
  } else if (configuration_.overall_roi_mode == domain::OverallRoiMode::kAutoCenter) {
    const auto left_center = robustCloudCenter(*left_camera_cloud_xy);
    const auto right_center = robustCloudCenter(*right_camera_cloud_xy);
    std::optional<Eigen::Vector2f> center;
    if (configuration_.using_camera == "Left") {
      center = left_center;
    } else if (configuration_.using_camera == "Right") {
      center = right_center;
    } else if (left_center && right_center) {
      center = (*left_center + *right_center) * 0.5F;
    } else {
      center = left_center ? left_center : right_center;
    }

    if (center) {
      const double half_width = (roi[2] - roi[0]) * 0.5;
      const double half_height = (roi[3] - roi[1]) * 0.5;
      const double center_x_mm = static_cast<double>((*center).x()) * 1000.0;
      const double center_y_mm = static_cast<double>((*center).y()) * 1000.0;
      resolved_roi = {center_x_mm - half_width, center_y_mm - half_height, center_x_mm + half_width,
                      center_y_mm + half_height};
    }
  }
  resolved_overall_roi_mm_ = resolved_roi;
  // cropBox2D appends, so the destinations are cleared here to make a second pass (the
  // roll-anchored crop's revert path) equivalent to a first one.
  const auto apply_crop = [&](const std::array<double, 4> &box_mm) {
    Eigen::Matrix2f roi_matrix;
    roi_matrix << box_mm[0], box_mm[2], box_mm[1], box_mm[3];
    roi_matrix /= 1000;
    left_camera_cloud_->clear();
    right_camera_cloud_->clear();
    gap_std::roiCrop2D(*left_camera_cloud_xy, left_camera_cloud_.get(), roi_matrix);
    gap_std::roiCrop2D(*right_camera_cloud_xy, right_camera_cloud_.get(), roi_matrix);
  };
  apply_crop(resolved_roi);
  if (roll_crop_applied_) {
    // Point-count failsafe: a crop derived from mislabelled rolls must never starve the
    // measurement, so anything below the floor discards the window and redoes the unbounded
    // pass. Only the cameras the configuration actually uses are checked -- a "Left"/"Right"
    // configuration legitimately leaves the other side empty until the copy step below.
    const std::size_t kept_left = left_camera_cloud_->size();
    const std::size_t kept_right = right_camera_cloud_->size();
    const std::size_t minimum = override_crop->min_points_kept;
    bool insufficient = false;
    if (configuration_.using_camera == "Left") {
      insufficient = kept_left < minimum;
    } else if (configuration_.using_camera == "Right") {
      insufficient = kept_right < minimum;
    } else if (allow_single_camera) {
      insufficient = kept_left + kept_right < minimum;
    } else {
      insufficient = kept_left < minimum || kept_right < minimum;
    }
    if (insufficient) {
      roll_crop_applied_ = false;
      roll_crop_reverted_ = true;
      resolved_roi = unbounded_roi;
      resolved_overall_roi_mm_ = resolved_roi;
      apply_crop(resolved_roi);
    }
  }
  if (skip_roi_crop && !roll_crop_applied_) {
    // Honest diagnostics: no window was applied, so record what actually survived.
    double min_x = std::numeric_limits<double>::infinity(), min_y = min_x;
    double max_x = -min_x, max_y = -min_y;
    for (const auto *cloud : {left_camera_cloud_.get(), right_camera_cloud_.get()}) {
      for (const auto &point : *cloud) {
        min_x = std::min(min_x, static_cast<double>(point.x));
        min_y = std::min(min_y, static_cast<double>(point.y));
        max_x = std::max(max_x, static_cast<double>(point.x));
        max_y = std::max(max_y, static_cast<double>(point.y));
      }
    }
    if (min_x <= max_x)
      resolved_overall_roi_mm_ = {min_x * 1000.0, min_y * 1000.0, max_x * 1000.0, max_y * 1000.0};
  }

  // copy used camera cloud to the unused camera
  const auto &using_camera = configuration_.using_camera;
  if (using_camera == "Left") {
    pcl::copyPointCloud(*left_camera_cloud_, *right_camera_cloud_);
  } else if (using_camera == "Right") {
    pcl::copyPointCloud(*right_camera_cloud_, *left_camera_cloud_);
  }
  // smooth cloud [TODO@Jianming]

  const bool primary_empty = left_camera_cloud_->empty();
  const bool secondary_empty = right_camera_cloud_->empty();
  const bool no_usable_points = allow_single_camera && using_camera == "Both"
                                    ? primary_empty && secondary_empty
                                    : primary_empty || secondary_empty;
  if (no_usable_points) {
    std::ostringstream message;
    message << "no points after preprocessing (primary=" << left_camera_cloud_->size()
            << ", secondary=" << right_camera_cloud_->size() << ", using_camera=" << using_camera
            << ", bounds_mm=[" << resolved_roi[0] << ", " << resolved_roi[1] << ", "
            << resolved_roi[2] << ", " << resolved_roi[3] << "])";
    throw std::runtime_error(message.str());
  }
}
}  // namespace detection
