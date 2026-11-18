/*
 * Copyright (c) XYZ Robotics Inc. - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 * Author: jianming huang <jianming.huang@xyzrobotics.ai>, 2023/02/02
 */
#include "gap_detection/GapUtils.hpp"

#include <algorithm>
#include <cmath>

#include <Eigen/Eigenvalues>
#include <Eigen/Geometry>

namespace detection::utils {
namespace {

constexpr std::size_t kMinimumInitialPosePoints = 3;
constexpr double kMinimumPcaEigenvalueRatio = 20.0;
constexpr double kMaximumInitialRotationRad = 5.0 * 3.14159265358979323846 / 180.0;
constexpr double kMaximumCentroidShiftM = 0.0035;
constexpr double kMaximumInitialTranslationM = 0.012;

struct PlanarMoments {
  std::size_t point_count = 0;
  double sum_x = 0;
  double sum_y = 0;
  double sum_xx = 0;
  double sum_xy = 0;
  double sum_yy = 0;
};

bool accumulatePlanarMoments(const PointCloud& cloud, PlanarMoments* moments) {
  if (cloud.size() < kMinimumInitialPosePoints)
    return false;
  for (const auto& point : cloud) {
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

bool computePlanarCentroid(const PlanarMoments& moments, Eigen::Vector2d* centroid) {
  if (moments.point_count < kMinimumInitialPosePoints)
    return false;
  const auto point_count = static_cast<double>(moments.point_count);
  (*centroid) << moments.sum_x / point_count, moments.sum_y / point_count;
  return centroid->allFinite();
}

bool computePrincipalAxis(const PlanarMoments& moments, const Eigen::Vector2d& centroid,
                          double* angle) {
  const auto point_count = static_cast<double>(moments.point_count);
  Eigen::Matrix2d covariance;
  covariance << moments.sum_xx / point_count - centroid.x() * centroid.x(),
      moments.sum_xy / point_count - centroid.x() * centroid.y(),
      moments.sum_xy / point_count - centroid.x() * centroid.y(),
      moments.sum_yy / point_count - centroid.y() * centroid.y();
  if (!covariance.allFinite())
    return false;

  const Eigen::SelfAdjointEigenSolver<Eigen::Matrix2d> solver(covariance);
  if (solver.info() != Eigen::Success)
    return false;
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
  while (angle >= kHalfPi)
    angle -= kPi;
  while (angle < -kHalfPi)
    angle += kPi;
  return angle;
}

Eigen::Matrix3f legacyInitialPose(const PointCloud& left_template, const PointCloud& right_template,
                                  const PointCloud& left_target, const PointCloud& right_target) {
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

} // namespace

InitialPoseResult computeInitialPose(const PointCloud& left_template,
                                     const PointCloud& right_template,
                                     const PointCloud& left_target, const PointCloud& right_target,
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

std::optional<std::size_t> selectBestTemplateByIcp(const std::vector<TemplateIcpScore>& scores,
                                                   double minimum_score, double epsilon) {
  std::optional<std::size_t> best;
  double best_minimum = 0;
  double best_mean = 0;
  for (std::size_t index = 0; index < scores.size(); ++index) {
    const auto& score = scores[index];
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
        const auto& current = scores[*best];
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

bool cloudsIdentical2D(const PointCloud& a, const PointCloud& b) {
  if (a.size() != b.size())
    return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i].x != b[i].x || a[i].y != b[i].y)
      return false;
  }
  return true;
}

std::string classifyRadiusMode(double radius, double minimum, double maximum, bool is_fixed) {
  if (is_fixed)
    return "fixed";
  const auto is_close = [](double value, double limit) {
    return std::fabs(value - limit) <= 1e-6 * std::max(1.0, std::fabs(limit));
  };
  if (is_close(radius, minimum) || is_close(radius, maximum))
    return "clamped";
  return "free";
}

double circleCircleDistance(const Eigen::VectorXf& circle1, const Eigen::VectorXf& circle2,
                            Eigen::Vector2f* start_pt, Eigen::Vector2f* end_pt) {
  Eigen::Vector2f c1{circle1[0], circle1[1]};
  Eigen::Vector2f c2{circle2[0], circle2[1]};
  Eigen::Vector2f v = (c2 - c1).normalized();
  auto r1 = circle1[2], r2 = circle2[2];
  *start_pt = v * r1 + c1;
  *end_pt = c2 - v * r2;
  return (c2 - c1).norm() - (r1 + r2);
}

PointT getEndPointofCloud(const PointCloud& cloud, bool is_left_end, std::vector<int>* indices) {
  if (cloud.empty())
    throw std::runtime_error("Empty input cloud");
  if (!indices)
    return is_left_end ? cloud[0] : cloud.back();
  if (indices->empty())
    throw std::runtime_error("Empty input indices");
  bool ascend = cloud[0].x <= cloud.back().x;
  std::sort(indices->begin(), indices->end());
  int index = ascend == is_left_end ? indices->at(0) : indices->back();
  return cloud[index];
}
} // namespace detection::utils
