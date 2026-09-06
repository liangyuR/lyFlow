/*
 * Copyright (c) XYZ Robotics Inc. - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 * Author: jianming huang <jianming.huang@xyzrobotics.ai>, 2023/02/02
 */
#include "gap_detection/GapUtils.hpp"

#include <cmath>
#include <limits>
#include <utility>
#include <algorithm>
#include <vector>
#include <pcl/search/kdtree.h>
#include <pcl/kdtree/kdtree_flann.h>
#include <pcl/common/io.h>
#include <pcl/common/eigen.h>
#include <pcl/common/point_tests.h>
#include <pcl/filters/extract_indices.h>
#include <pcl/sample_consensus/ransac.h>
#include <pcl/sample_consensus/sac_model_line.h>
#include <pcl/segmentation/region_growing.h>
#include <pcl/registration/correspondence_estimation.h>
#include <pcl/sample_consensus/method_types.h>
#include <pcl/sample_consensus/model_types.h>
#include <pcl/segmentation/sac_segmentation.h>
#include <pcl/filters/radius_outlier_removal.h>
#include <pcl/io/pcd_io.h>
#include <pcl/io/ply_io.h>
#include <boost/shared_ptr.hpp>
#include <Eigen/Eigenvalues>
#include <Eigen/Geometry>

namespace detection::utils {
namespace {

constexpr std::size_t kMinimumInitialPosePoints = 3;
constexpr double kMinimumPcaEigenvalueRatio = 20.0;
constexpr double kMaximumInitialRotationRad = 5.0 * 3.14159265358979323846 / 180.0;
constexpr double kMaximumCentroidShiftM = 0.0035;
constexpr double kMaximumInitialTranslationM = 0.012;

// Plausible outer x span of the two roll boxes taken together. Golden real samples span
// 7.6-16.2 mm; anything outside these bounds means at least one roll label is not a roll.
constexpr double kMinRollPairSpanMm = 2.0;
constexpr double kMaxRollPairSpanMm = 40.0;
// Copies src into dst unless dst already aliases src, so every intensity-gate fail-safe can
// hand the caller an untouched cloud without the caller having to branch.
void passThroughCloud(const PointCloud &src, PointCloud *dst) {
  if (dst != &src) *dst = src;
}

struct PlanarMoments {
  std::size_t point_count = 0;
  double sum_x = 0;
  double sum_y = 0;
  double sum_xx = 0;
  double sum_xy = 0;
  double sum_yy = 0;
};

bool accumulatePlanarMoments(const PointCloud &cloud, PlanarMoments *moments) {
  if (cloud.size() < kMinimumInitialPosePoints) return false;
  for (const auto &point : cloud) {
    if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z)) {
      return false;
    }
    ++moments->point_count;
    moments->sum_x += point.x;
    moments->sum_y += point.y;
    moments->sum_xx += static_cast<double>(point.x) * point.x;
    moments->sum_xy += static_cast<double>(point.x) * point.y;
    moments->sum_yy += static_cast<double>(point.y) * point.y;
  }
  return true;
}

bool computePlanarCentroid(const PlanarMoments &moments, Eigen::Vector2d *centroid) {
  if (moments.point_count < kMinimumInitialPosePoints) return false;
  const auto point_count = static_cast<double>(moments.point_count);
  (*centroid) << moments.sum_x / point_count, moments.sum_y / point_count;
  return centroid->allFinite();
}

bool computePrincipalAxis(const PlanarMoments &moments, const Eigen::Vector2d &centroid,
                          double *angle) {
  const auto point_count = static_cast<double>(moments.point_count);
  Eigen::Matrix2d covariance;
  covariance << moments.sum_xx / point_count - centroid.x() * centroid.x(),
      moments.sum_xy / point_count - centroid.x() * centroid.y(),
      moments.sum_xy / point_count - centroid.x() * centroid.y(),
      moments.sum_yy / point_count - centroid.y() * centroid.y();
  if (!covariance.allFinite()) return false;

  const Eigen::SelfAdjointEigenSolver<Eigen::Matrix2d> solver(covariance);
  if (solver.info() != Eigen::Success) return false;
  const auto eigenvalues = solver.eigenvalues();
  if (!eigenvalues.allFinite() || eigenvalues[0] <= 0 ||
      eigenvalues[1] / eigenvalues[0] < kMinimumPcaEigenvalueRatio) {
    return false;
  }
  const auto principal_axis = solver.eigenvectors().col(1);
  *angle = std::atan2(principal_axis.y(), principal_axis.x());
  return std::isfinite(*angle);
}

double wrapPcaAxisAngle(double angle) {
  constexpr double kPi = 3.14159265358979323846;
  constexpr double kHalfPi = kPi / 2.0;
  while (angle >= kHalfPi) angle -= kPi;
  while (angle < -kHalfPi) angle += kPi;
  return angle;
}

Eigen::Matrix3f legacyInitialPose(const PointCloud &left_template, const PointCloud &right_template,
                                  const PointCloud &left_target, const PointCloud &right_target) {
  const auto left_template_point = getEndPointofCloud(left_template, true);
  const auto right_template_point = getEndPointofCloud(right_template, false);
  const auto left_target_point = getEndPointofCloud(left_target, true);
  const auto right_target_point = getEndPointofCloud(right_target, false);
  Eigen::Matrix3f transform = Eigen::Matrix3f::Identity();
  transform(0, 2) = (left_target_point.x + right_target_point.x) / 2.0 -
                    (left_template_point.x + right_template_point.x) / 2.0;
  transform(1, 2) = (left_target_point.y + right_target_point.y) / 2.0 -
                    (left_template_point.y + right_template_point.y) / 2.0;
  return transform;
}

}  // namespace

InitialPoseResult computeInitialPose(const PointCloud &left_template,
                                     const PointCloud &right_template,
                                     const PointCloud &left_target, const PointCloud &right_target,
                                     domain::InitialPoseMode mode) {
  const auto legacy = legacyInitialPose(left_template, right_template, left_target, right_target);
  if (mode == domain::InitialPoseMode::kEndpointMidpointTranslation) {
    return {legacy, false};
  }

  PlanarMoments template_moments;
  PlanarMoments target_moments;
  if (!accumulatePlanarMoments(left_template, &template_moments) ||
      !accumulatePlanarMoments(right_template, &template_moments) ||
      !accumulatePlanarMoments(left_target, &target_moments) ||
      !accumulatePlanarMoments(right_target, &target_moments)) {
    return {legacy, true};
  }

  Eigen::Vector2d template_centroid;
  Eigen::Vector2d target_centroid;
  if (!computePlanarCentroid(template_moments, &template_centroid) ||
      !computePlanarCentroid(target_moments, &target_centroid) ||
      (target_centroid - template_centroid).norm() > kMaximumCentroidShiftM) {
    return {legacy, true};
  }

  Eigen::Matrix3d transform = Eigen::Matrix3d::Identity();
  if (mode == domain::InitialPoseMode::kPcaRigidCentroid) {
    double template_angle = 0;
    double target_angle = 0;
    if (!computePrincipalAxis(template_moments, template_centroid, &template_angle) ||
        !computePrincipalAxis(target_moments, target_centroid, &target_angle)) {
      return {legacy, true};
    }
    const auto angle = wrapPcaAxisAngle(target_angle - template_angle);
    if (!std::isfinite(angle) || std::abs(angle) > kMaximumInitialRotationRad) {
      return {legacy, true};
    }
    const auto cosine = std::cos(angle);
    const auto sine = std::sin(angle);
    transform(0, 0) = cosine;
    transform(0, 1) = -sine;
    transform(1, 0) = sine;
    transform(1, 1) = cosine;
  } else if (mode != domain::InitialPoseMode::kCentroidTranslation) {
    return {legacy, true};
  }

  transform.block<2, 1>(0, 2) = target_centroid - transform.block<2, 2>(0, 0) * template_centroid;
  if (!transform.allFinite() || transform.block<2, 1>(0, 2).norm() > kMaximumInitialTranslationM) {
    return {legacy, true};
  }
  return {transform.cast<float>(), false};
}

RollAnchoredCropResult computeRollAnchoredCropMm(
    const domain::Roi &gap_left, const domain::Roi &gap_right,
    const domain::RollAnchoredCropConfiguration &config) {
  if (!config.enabled) return {std::nullopt, "disabled"};
  const double half_width = config.half_width_mm;
  const double half_height = config.half_height_mm;
  const double max_roll_box_height = config.max_roll_box_height_mm;
  // Checked before the boxes so an unusable configuration is reported as such rather than being
  // masked by whatever the labels happen to look like on this sample.
  if (!std::isfinite(half_width) || !std::isfinite(half_height) || half_width <= 0 ||
      half_height <= 0 || !std::isfinite(max_roll_box_height) || max_roll_box_height <= 0) {
    return {std::nullopt, "bad_config"};
  }
  const auto &left = gap_left.values;
  const auto &right = gap_right.values;
  for (const auto *box : {&left, &right}) {
    const bool finite = std::isfinite((*box)[0]) && std::isfinite((*box)[1]) &&
                        std::isfinite((*box)[2]) && std::isfinite((*box)[3]);
    // Degenerate box: an unlabelled or collapsed roll carries no anchor information.
    if (!finite || (*box)[2] <= (*box)[0] || (*box)[3] <= (*box)[1]) {
      return {std::nullopt, "degenerate_roll_box"};
    }
    // Implausibly tall roll box: the segment swallowed something that is not the roll.
    if ((*box)[3] - (*box)[1] > max_roll_box_height) return {std::nullopt, "roll_box_height"};
  }
  const double span = std::max(left[2], right[2]) - std::min(left[0], right[0]);
  if (span < kMinRollPairSpanMm || span > kMaxRollPairSpanMm) return {std::nullopt, "span"};
  const double left_center_x = (left[0] + left[2]) * 0.5;
  const double left_center_z = (left[1] + left[3]) * 0.5;
  const double right_center_x = (right[0] + right[2]) * 0.5;
  const double right_center_z = (right[1] + right[3]) * 0.5;
  const double center_x = (left_center_x + right_center_x) * 0.5;
  const double center_z = (left_center_z + right_center_z) * 0.5;
  return {std::array<double, 4>{center_x - half_width, center_z - half_height,
                                center_x + half_width, center_z + half_height},
          ""};
}

IntensityGateOutcome filterCloudByIntensity(const PointCloud &src, PointCloud *dst,
                                            const domain::IntensityGateConfiguration &config) {
  IntensityGateOutcome outcome;
  outcome.input_points = src.size();
  outcome.kept_points = src.size();
  // Fail-safe 1: the gate is off, so the cloud passes through untouched.
  if (!config.enabled) {
    passThroughCloud(src, dst);
    return outcome;
  }
  // Fail-safe 2: too small to reason about. A median over a handful of points is noise, and the
  // gate could only take the ROI further below the fitting minimum.
  if (src.size() <= config.min_points_kept) {
    outcome.skip_reason = "too_few_points";
    passThroughCloud(src, dst);
    return outcome;
  }

  std::vector<double> intensities;
  intensities.reserve(src.size());
  for (const auto &point : src) intensities.push_back(static_cast<double>(point.r));
  std::sort(intensities.begin(), intensities.end());
  const std::size_t middle = intensities.size() / 2;
  outcome.median_intensity = intensities.size() % 2 == 0
                                 ? (intensities[middle - 1] + intensities[middle]) * 0.5
                                 : intensities[middle];
  outcome.cutoff =
      std::max(config.min_counts, config.relative_threshold * outcome.median_intensity);

  // Fail-safe 3: both rules are switched off (or the ROI is uniformly black), so there is no
  // threshold to apply.
  if (!(outcome.cutoff > 0)) {
    outcome.skip_reason = "zero_threshold";
    passThroughCloud(src, dst);
    return outcome;
  }

  PointCloud kept;
  kept.reserve(src.size());
  for (const auto &point : src) {
    if (static_cast<double>(point.r) >= outcome.cutoff) kept.push_back(point);
  }

  // Fail-safe 4: the threshold would leave too little to fit. A bad threshold must never empty
  // an ROI, so the whole gate is abandoned for this frame.
  if (kept.size() < config.min_points_kept) {
    outcome.skip_reason = "min_points_kept";
    passThroughCloud(src, dst);
    return outcome;
  }
  // Fail-safe 5: the threshold deletes more of the ROI than the configuration allows. Strictly
  // greater, so max_drop_ratio == 1.0 never blocks anything.
  const double drop_ratio =
      static_cast<double>(src.size() - kept.size()) / static_cast<double>(src.size());
  if (drop_ratio > config.max_drop_ratio) {
    outcome.skip_reason = "max_drop_ratio";
    passThroughCloud(src, dst);
    return outcome;
  }

  outcome.kept_points = kept.size();
  outcome.applied = true;
  outcome.skip_reason = "";
  *dst = std::move(kept);
  return outcome;
}

std::optional<std::size_t> selectBestTemplateByIcp(const std::vector<TemplateIcpScore> &scores,
                                                   double minimum_score, double epsilon) {
  std::optional<std::size_t> best;
  double best_minimum = 0;
  double best_mean = 0;
  for (std::size_t index = 0; index < scores.size(); ++index) {
    const auto &score = scores[index];
    if (!std::isfinite(score.left) || !std::isfinite(score.right) || score.left < minimum_score ||
        score.right < minimum_score) {
      continue;
    }
    const auto minimum = std::min(score.left, score.right);
    const auto mean = (score.left + score.right) / 2.0;
    bool replace = !best || minimum > best_minimum + epsilon;
    if (!replace && best && std::abs(minimum - best_minimum) <= epsilon) {
      replace = mean > best_mean + epsilon;
      if (!replace && std::abs(mean - best_mean) <= epsilon) {
        const auto &current = scores[*best];
        replace =
            score.configuration_order < current.configuration_order ||
            (score.configuration_order == current.configuration_order && score.id < current.id);
      }
    }
    if (replace) {
      best = index;
      best_minimum = minimum;
      best_mean = mean;
    }
  }
  return best;
}

unsigned int computeMeanAndCovarianceMatrix(const PointCloud &cloud,
                                            const std::vector<int> &indices,
                                            Eigen::Matrix2d *covariance_matrix,
                                            Eigen::Vector3d *centroid) {
  Eigen::Matrix<double, 1, 5, Eigen::RowMajor> accu =
      Eigen::Matrix<double, 1, 5, Eigen::RowMajor>::Zero();
  std::size_t point_count = 0;
  for (const int &index : indices) {
    const auto &p = cloud[index];
    if (!pcl::isFinite(p)) continue;
    ++point_count;
    accu[0] += p.x * p.x;
    accu[1] += p.x * p.y;
    accu[2] += p.y * p.y;
    accu[3] += p.x;
    accu[4] += p.y;
  }
  accu /= static_cast<double>(point_count);
  (*centroid) << accu[3], accu[4], 0;
  covariance_matrix->coeffRef(0) = accu[0] - accu[3] * accu[3];
  covariance_matrix->coeffRef(1) = accu[1] - accu[3] * accu[4];
  covariance_matrix->coeffRef(3) = accu[2] - accu[4] * accu[4];
  covariance_matrix->coeffRef(2) = covariance_matrix->coeff(1);
  return (static_cast<unsigned int>(point_count));
}

bool EstimateNormal(const PointCloud &cloud, pcl::PointCloud<pcl::Normal> *normal,
                    const pcl::search::Search<PointT> &tree, int neighbors, bool flip_flag) {
  int num = cloud.size();
  if (neighbors < 2) {
    return false;
  }
  if (num < neighbors) {
    return false;
  }
  if (tree.getInputCloud().get() != &cloud) {
    return false;
  }

  for (int i = 0; i < num; i++) {
    // find neighbor
    std::vector<int> nn(neighbors);
    std::vector<float> nn_dists(neighbors);
    tree.nearestKSearch(i, neighbors, nn, nn_dists);
    // compute normal using pca
    pcl::Normal n;
    Eigen::Matrix2d covariance_matrix;
    Eigen::Vector3d xyz_centroid;
    if (nn.size() < 2 ||
        computeMeanAndCovarianceMatrix(cloud, nn, &covariance_matrix, &xyz_centroid) == 0) {
      float nan = std::numeric_limits<float>::quiet_NaN();
      n = pcl::Normal(nan, nan, 0, nan);
    } else {
      Eigen::Vector2d::Scalar eigen_value;
      Eigen::Vector2d eigen_vector;
      pcl::eigen22(covariance_matrix, eigen_value, eigen_vector);

      double curvature = 0;
      double eig_sum = covariance_matrix.coeff(0) + covariance_matrix.coeff(3);
      if (eig_sum != 0) curvature = std::abs(eigen_value / eig_sum);
      n = pcl::Normal(eigen_vector[0], eigen_vector[1], 0, curvature);
      if (flip_flag && n.normal_y < 0) {
        n.normal_x *= -1;
        n.normal_y *= -1;
      }
    }
    normal->push_back(n);
  }
  return true;
}

void getCloudsOverlap(const PointCloud &cloud1, const PointCloud &cloud2, PointCloud *overlap) {
  pcl::registration::CorrespondenceEstimation<PointT, PointT> core;
  core.setInputSource(cloud1.makeShared());
  core.setInputTarget(cloud2.makeShared());
  boost::shared_ptr<pcl::Correspondences> cor(new pcl::Correspondences);
  core.determineReciprocalCorrespondences(*cor, 1e-7);
  overlap->resize(cor->size());
  for (size_t i = 0; i < cor->size(); i++) {
    overlap->at(i) = cloud1[cor->at(i).index_query];
  }
}

unsigned int splitAndExtractCloud(const PointCloud &cloud, PointCloud *cloud_left,
                                  PointCloud *cloud_right, int neighbor, double cluster_angle,
                                  const std::string &save_prefix) {
  // estimate normal
  pcl::search::Search<PointT>::Ptr tree(new pcl::search::KdTree<PointT>);
  pcl::PointCloud<pcl::Normal>::Ptr normals(new pcl::PointCloud<pcl::Normal>);
  PointCloud::ConstPtr cloud_ptr(cloud.makeShared());
  tree->setInputCloud(cloud_ptr);
  EstimateNormal(*cloud_ptr, normals.get(), *tree, neighbor, true);
  if (!save_prefix.empty()) {
    pcl::PointCloud<pcl::PointXYZRGBNormal> cloud_n;
    pcl::concatenateFields(cloud, *normals, cloud_n);
    pcl::io::savePLYFile(save_prefix + "_cloud_n.ply", cloud_n);
  }
  // region grow cluster setting
  pcl::RegionGrowing<PointT, pcl::Normal> reg;
  reg.setMinClusterSize(100);
  reg.setMaxClusterSize(100000);
  reg.setSearchMethod(tree);
  reg.setNumberOfNeighbours(neighbor);
  reg.setInputCloud(cloud_ptr);
  reg.setInputNormals(normals);
  reg.setSmoothnessThreshold(cluster_angle / 180.0 * M_PI);
  reg.setCurvatureThreshold(1.0);
  // cluster and extract cloud
  std::vector<pcl::PointIndices> clusters;
  reg.extract(clusters);

  auto color = reg.getColoredCloud();
  if (!save_prefix.empty()) {
    pcl::io::savePLYFile(save_prefix + "_cloud_c.ply", *color);
  }

  int n = clusters.size();
  if (n < 2) {
    return n;
  }
  // choose the two largest cloud, and get left and right cluster according to x_avg;

  std::sort(clusters.begin(), clusters.end(),
            [&](pcl::PointIndices &x, pcl::PointIndices &y) -> bool {
              return x.indices.size() > y.indices.size();
            });
  std::vector<double> x_avg(n, 0);
  for (int i = 0; i < n; i++) {
    for (auto &p : clusters[i].indices) x_avg[i] += cloud[p].x;
    x_avg[i] /= clusters[i].indices.size();
  }
  bool left_bigger = x_avg[0] < x_avg[1];
  int left_cluster = left_bigger ? 0 : 1;
  int right_cluster = left_bigger ? 1 : 0;

  if (cloud_left != nullptr) copyPointCloud(*color, clusters[left_cluster], *cloud_left);
  if (cloud_right != nullptr) copyPointCloud(*color, clusters[right_cluster], *cloud_right);
  return n;
}

bool cloudsIdentical2D(const PointCloud &a, const PointCloud &b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i].x != b[i].x || a[i].y != b[i].y) return false;
  }
  return true;
}

void removeRadiusOutlier(const PointCloud &src_cloud, PointCloud *dst_cloud, double r,
                         int neighbor) {
  pcl::RadiusOutlierRemoval<PointT> outlier_removal;
  outlier_removal.setInputCloud(src_cloud.makeShared());
  outlier_removal.setRadiusSearch(r);
  outlier_removal.setMinNeighborsInRadius(neighbor);
  outlier_removal.filter(*dst_cloud);
}

std::string classifyRadiusMode(double radius, double minimum, double maximum, bool is_fixed) {
  if (is_fixed) return "fixed";
  const auto is_close = [](double value, double limit) {
    return std::fabs(value - limit) <= 1e-6 * std::max(1.0, std::fabs(limit));
  };
  if (is_close(radius, minimum) || is_close(radius, maximum)) return "clamped";
  return "free";
}

bool fitFixedRadiusCircleComponents(const PointCloud &cloud,
                                    const FixedRadiusCircleOptions &options,
                                    FixedRadiusCircleResult *result) {
  if (result == nullptr || cloud.size() < 3 || options.radius <= 0 ||
      options.maximum_center_shift <= 0 || options.maximum_point_gap <= 0 ||
      options.inlier_distance <= 0 || options.minimum_inliers < 3 ||
      options.minimum_arc_coverage_deg <= 0 || options.maximum_rms_residual <= 0) {
    return false;
  }

  FixedRadiusCircleResult best;
  bool found = false;
  auto evaluate_component = [&](std::size_t begin, std::size_t end) {
    if (end - begin < static_cast<std::size_t>(options.minimum_inliers)) return;
    for (std::size_t first = begin; first + 1 < end; ++first) {
      const Eigen::Vector2d first_point(cloud[first].x, cloud[first].y);
      for (std::size_t second = first + 1; second < end; ++second) {
        const Eigen::Vector2d second_point(cloud[second].x, cloud[second].y);
        const auto chord = second_point - first_point;
        const double chord_length = chord.norm();
        if (chord_length < options.radius * 0.2 || chord_length >= options.radius * 2.0) {
          continue;
        }
        const auto midpoint = (first_point + second_point) * 0.5;
        const double height = std::sqrt(
            std::max(0.0, options.radius * options.radius - chord_length * chord_length * 0.25));
        const Eigen::Vector2d perpendicular(-chord.y() / chord_length, chord.x() / chord_length);
        for (const double direction : {-1.0, 1.0}) {
          const Eigen::Vector2d center = midpoint + direction * height * perpendicular;
          const double center_shift = (center.cast<float>() - options.nominal_center).norm();
          if (center_shift > options.maximum_center_shift) continue;

          pcl::Indices inliers;
          std::vector<double> angles;
          double square_sum = 0;
          for (std::size_t index = begin; index < end; ++index) {
            const double delta_x = cloud[index].x - center.x();
            const double delta_y = cloud[index].y - center.y();
            const double residual = std::fabs(std::hypot(delta_x, delta_y) - options.radius);
            if (residual > options.inlier_distance) continue;
            inliers.push_back(static_cast<int>(index));
            square_sum += residual * residual;
            double angle = std::atan2(delta_y, delta_x);
            if (angle < 0) angle += 2 * M_PI;
            angles.push_back(angle);
          }
          if (inliers.size() < static_cast<std::size_t>(options.minimum_inliers)) continue;

          const double rms = std::sqrt(square_sum / inliers.size());
          if (rms > options.maximum_rms_residual) continue;
          std::sort(angles.begin(), angles.end());
          double largest_gap = angles.front() + 2 * M_PI - angles.back();
          for (std::size_t index = 1; index < angles.size(); ++index) {
            largest_gap = std::max(largest_gap, angles[index] - angles[index - 1]);
          }
          const double coverage = (2 * M_PI - largest_gap) * 180.0 / M_PI;
          if (coverage < options.minimum_arc_coverage_deg) continue;

          const bool better =
              !found || inliers.size() > best.inliers.size() ||
              (inliers.size() == best.inliers.size() &&
               (coverage > best.arc_coverage_deg + 1e-9 ||
                (std::fabs(coverage - best.arc_coverage_deg) <= 1e-9 &&
                 (rms < best.rms_residual - 1e-12 || (std::fabs(rms - best.rms_residual) <= 1e-12 &&
                                                      center_shift < best.center_shift)))));
          if (!better) continue;

          best.circle = Eigen::VectorXf(3);
          best.circle << static_cast<float>(center.x()), static_cast<float>(center.y()),
              static_cast<float>(options.radius);
          best.inliers = std::move(inliers);
          best.arc_coverage_deg = coverage;
          best.rms_residual = rms;
          best.center_shift = center_shift;
          found = true;
        }
      }
    }
  };

  std::size_t component_begin = 0;
  for (std::size_t index = 1; index <= cloud.size(); ++index) {
    const bool at_end = index == cloud.size();
    const bool discontinuity =
        !at_end && std::hypot(cloud[index].x - cloud[index - 1].x,
                              cloud[index].y - cloud[index - 1].y) > options.maximum_point_gap;
    if (at_end || discontinuity) {
      evaluate_component(component_begin, index);
      component_begin = index;
    }
  }

  if (!found) return false;
  *result = std::move(best);
  return true;
}

Eigen::Vector2f rotatePoint(const Eigen::Vector2f &pt, const Eigen::Vector2f &center,
                            double angle) {
  Eigen::Rotation2D<float> rotation(angle * M_PI / 180.0);
  return rotation * (pt - center) + center;
}

double lineCircleDistance(const Eigen::VectorXf &line, const Eigen::VectorXf &circle,
                          Eigen::Vector2f *start_pt, Eigen::Vector2f *end_pt) {
  float px = line[0];
  float py = line[1];
  float vx = line[3];
  float vy = line[4];
  float cx = circle[0];
  float cy = circle[1];
  float cr = circle[2];
  // calculate the distance
  Eigen::Vector2f n{vy, -vx};
  Eigen::Vector2f c{cx, cy};
  Eigen::Vector2f p{px, py};
  n.normalize();
  Eigen::Vector2f v = c - p;
  if (n.dot(v) < 0) n = -n;
  double dist = n.dot(v) - cr;
  *start_pt = c - n.dot(v) * n;
  *end_pt = c - cr * n;
  return dist;
}

double pointLineDistance(const Eigen::VectorXf &line, const Eigen::Vector2f &start_pt,
                         Eigen::Vector2f *end_pt) {
  float cx = line[0];
  float cy = line[1];
  float vx = line[3];
  float vy = line[4];
  // calculate the distance
  Eigen::Vector2f n{vy, -vx};
  Eigen::Vector2f c{cx, cy};
  n.normalize();
  if (n[1] > 0) n = -n;
  double dist = n.dot(start_pt - c);
  *end_pt = start_pt - dist * n;
  return dist;
}

double circleCircleDistance(const Eigen::VectorXf &circle1, const Eigen::VectorXf &circle2,
                            Eigen::Vector2f *start_pt, Eigen::Vector2f *end_pt) {
  Eigen::Vector2f c1{circle1[0], circle1[1]};
  Eigen::Vector2f c2{circle2[0], circle2[1]};
  Eigen::Vector2f v = (c2 - c1).normalized();
  auto r1 = circle1[2], r2 = circle2[2];
  *start_pt = v * r1 + c1;
  *end_pt = c2 - v * r2;
  return (c2 - c1).norm() - (r1 + r2);
}

double pointCircleDistance(const Eigen::VectorXf &circle, const Eigen::Vector2f &start_pt,
                           Eigen::Vector2f *end_pt) {
  Eigen::Vector2f c{circle[0], circle[1]};
  Eigen::Vector2f v = (c - start_pt).normalized();
  auto r = circle[2];
  *end_pt = c - v * r;
  return (c - start_pt).norm() - r;
}

double lineCloudDistance(const Eigen::VectorXf &line, const PointCloud &cloud,
                         Eigen::Vector2f *start_pt, Eigen::Vector2f *end_pt) {
  float px = line[0];
  float py = line[1];
  float vx = line[3];
  float vy = line[4];
  Eigen::Vector2f n{vy, -vx};
  n.normalize();

  int idx_near = 0;
  double min_dist = std::numeric_limits<double>::max();
  for (std::size_t i = 0; i < cloud.size(); ++i) {
    double dist = std::fabs(n[0] * (cloud[i].x - px) + n[1] * (cloud[i].y - py));
    if (min_dist > dist) {
      idx_near = i;
      min_dist = dist;
    }
  }
  *end_pt << cloud[idx_near].x, cloud[idx_near].y;
  return -pointLineDistance(line, *end_pt, start_pt);
}

double circleCloudDistance(const Eigen::VectorXf &circle, const PointCloud &cloud,
                           Eigen::Vector2f *start_pt, Eigen::Vector2f *end_pt) {
  Eigen::Vector2f c{circle[0], circle[1]};
  float r = circle[2];

  double min_dist = std::numeric_limits<double>::max();
  int idx_near = 0;
  for (std::size_t i = 0; i < cloud.size(); ++i) {
    auto dist_c = pointPointDistance(c, Eigen::Vector2f{cloud[i].x, cloud[i].y});
    auto dist = std::fabs(dist_c - r);
    if (min_dist > dist) {
      idx_near = i;
      min_dist = dist;
    }
  }
  *end_pt << cloud[idx_near].x, cloud[idx_near].y;
  return -pointCircleDistance(circle, *end_pt, start_pt);
}

double pointCloudDistance(const PointCloud &cloud, const Eigen::Vector2f &start_pt,
                          Eigen::Vector2f *end_pt) {
  double min_dist = std::numeric_limits<double>::max();
  int idx_near = 0;
  for (std::size_t i = 0; i < cloud.size(); ++i) {
    auto dist = pointPointDistance(start_pt, Eigen::Vector2f{cloud[i].x, cloud[i].y});
    if (min_dist > dist) {
      idx_near = i;
      min_dist = dist;
    }
  }
  *end_pt << cloud[idx_near].x, cloud[idx_near].y;
  return min_dist;
}

double cloudCloudDistance(const PointCloud &cloud1, const PointCloud &cloud2,
                          Eigen::Vector2f *start_pt, Eigen::Vector2f *end_pt) {
  pcl::KdTreeFLANN<PointT> kdtree;
  kdtree.setInputCloud(cloud1.makeShared());

  std::vector<int> nn(1);
  std::vector<float> nn_dists(1);
  int idx_near_l = 0, idx_near_r = 0;
  double min_dist_sqr = std::numeric_limits<double>::max();
  for (size_t i = 0; i < cloud2.size(); i++) {
    kdtree.nearestKSearch(cloud2[i], 1, nn, nn_dists);
    if (nn_dists[0] < min_dist_sqr) {
      idx_near_l = nn[0];
      idx_near_r = i;
      min_dist_sqr = nn_dists[0];
    }
  }
  *start_pt << cloud1[idx_near_l].x, cloud1[idx_near_l].y;
  *end_pt << cloud2[idx_near_r].x, cloud2[idx_near_r].y;
  return std::sqrt(min_dist_sqr);
}

double pointPointDistance(const Eigen::Vector2f &start_pt, const Eigen::Vector2f &end_pt) {
  return (end_pt - start_pt).norm();
}

Eigen::Matrix2f getIntersectionRL(const Eigen::Matrix2f &roi, const Eigen::VectorXf &line) {
  Eigen::Matrix2f diagonal = roi;
  Eigen::Matrix2f clinodiagonal = diagonal;
  std::swap(clinodiagonal(1, 0), clinodiagonal(1, 1));

  std::vector<Eigen::Vector2f> corners{diagonal.col(0), clinodiagonal.col(0), diagonal.col(1),
                                       clinodiagonal.col(1)};
  Eigen::Matrix2f res;
  res << std::nan("nan"), std::nan("nan"), std::nan("nan"), std::nan("nan");
  Eigen::Vector2f end_point;
  for (int i = 0, j = 0; i < 4; i++) {
    double dis1 = pointLineDistance(line, corners[i], &end_point);
    double dis2 = pointLineDistance(line, corners[(i + 1) % 4], &end_point);
    if (dis1 * dis2 < 0) {
      // intersect with vertical line
      if (corners[i][0] == corners[(i + 1) % 4][0]) {
        double x = corners[i][0];
        // if intersect, line[3]!=0
        double y = (x - line[0]) / line[3] * line[4] + line[1];
        res(0, j) = x;
        res(1, j) = y;
        j++;
      } else {  // intersect with horizontal line
        double y = corners[i][1];
        double x = (y - line[1]) / line[4] * line[3] + line[0];
        res(0, j) = x;
        res(1, j) = y;
        j++;
      }
    }
  }
  return res;
}

Eigen::VectorXf getLinefrom2Points(const Eigen::Vector2f &pt1, const Eigen::Vector2f &pt2) {
  Eigen::VectorXf line(6);
  Eigen::Vector2f center = (pt2 + pt2) / 2;
  Eigen::Vector2f direction = pt2 - pt1;
  line << center[0], center[1], 0, direction[0], direction[1], 0;
  return line;
}

Eigen::Matrix2f insertPoint2Segment(const Eigen::Matrix2f &segment, const Eigen::Vector2f &point) {
  Eigen::Matrix2f res(segment);
  double len = (res.col(1) - res.col(0)).norm();
  double len1 = (point - res.col(0)).norm();
  double len2 = (point - res.col(1)).norm();
  int replace_index = len1 > len2 ? 1 : 0;
  if (len1 > len || len2 > len) res.col(replace_index) = point;
  return res;
}

Eigen::Matrix2f insertPoint2Segment(const Eigen::Matrix2f &segment, const PointT &point) {
  Eigen::Vector2f pt(point.x, point.y);
  return insertPoint2Segment(segment, pt);
}

PointT getEndPointofCloud(const PointCloud &cloud, bool is_left_end, std::vector<int> *indices) {
  if (cloud.empty()) throw std::runtime_error("Empty input cloud");
  if (!indices) return is_left_end ? cloud[0] : cloud.back();
  if (indices->empty()) throw std::runtime_error("Empty input indices");
  bool ascend = cloud[0].x <= cloud.back().x;
  std::sort(indices->begin(), indices->end());
  int index = ascend == is_left_end ? indices->at(0) : indices->back();
  return cloud[index];
}
}  // namespace detection::utils
