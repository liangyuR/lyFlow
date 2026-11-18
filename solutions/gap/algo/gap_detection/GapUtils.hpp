/*
 * Copyright (c) XYZ Robotics Inc. - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 * Author: jianming huang <jianming.huang@xyzrobotics.ai>, 2023/02/02
 */

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "domain/detection/DetectionConfiguration.hpp"
#include "domain/detection/DetectionTypes.hpp"

namespace detection::utils {

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

InitialPoseResult computeInitialPose(const PointCloud& left_template,
                                     const PointCloud& right_template,
                                     const PointCloud& left_target, const PointCloud& right_target,
                                     domain::InitialPoseMode mode);

std::optional<std::size_t> selectBestTemplateByIcp(const std::vector<TemplateIcpScore>& scores,
                                                   double minimum_score, double epsilon = 1e-6);

// True when both clouds hold the same 2D (x, y) point sequence, e.g. a mirrored template pair
// where one merged cloud was stored to both the left and right files.
bool cloudsIdentical2D(const PointCloud& a, const PointCloud& b);

// Classifies a fitted circle's radius against the configured search bounds, for ML export/
// evaluation. is_fixed takes precedence (the radius was pinned, not fitted free); otherwise a
// radius within relative 1e-6 of either bound is "clamped" (RANSAC pushed it to the search-space
// edge), and anything else is "free". minimum/maximum/radius must share the same unit.
std::string classifyRadiusMode(double radius, double minimum, double maximum, bool is_fixed);

double circleCircleDistance(const Eigen::VectorXf& circle1, const Eigen::VectorXf& circle2,
                            Eigen::Vector2f* start_pt, Eigen::Vector2f* end_pt);

// assume input cloud is monotonous about x
PointT getEndPointofCloud(const PointCloud& cloud, bool is_left_end,
                          std::vector<int>* indices = nullptr);

} // namespace detection::utils
