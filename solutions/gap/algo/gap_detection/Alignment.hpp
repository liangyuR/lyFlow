/*
 * Copyright (c) XYZ Robotics Inc. - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 * Author: jianming huang <jianming.huang@xyzrobotics.ai>, 2023/03/07
 */

#pragma once

#include <limits>

#include "domain/detection/DetectionConfiguration.hpp"
#include "domain/detection/DetectionTypes.hpp"

namespace detection {

struct AlignmentOutcome {
  Eigen::Matrix3f transform = Eigen::Matrix3f::Identity();
  double score = std::numeric_limits<double>::quiet_NaN();
  bool bidirectional = false;
  bool success = false;
  double degenerate_eigenvalue_ratio = std::numeric_limits<double>::quiet_NaN();
  bool degenerate_locked = false;
};

AlignmentOutcome alignCloud2D(const PointCloud& source, const PointCloud& target,
                              const Eigen::Matrix3f& initialPose,
                              const domain::IcpConfiguration& params, double degenerateRatio);

} // namespace detection
