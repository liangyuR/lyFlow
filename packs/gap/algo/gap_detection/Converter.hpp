/*
 * Copyright (c) XYZ Robotics Inc. - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 * Author: jianming huang <jianming.huang@xyzrobotics.ai>, 2023/02/15
 */
#pragma once

#include <vector>
#include <string>

#include "domain/detection/DetectionTypes.hpp"

namespace conv {
enum AxisType { kX, kY, kZ };
Eigen::Matrix4f EigenMatrix3fto4f(const Eigen::Matrix3f &m_2d, AxisType ignore_dim);

Eigen::Matrix3f EigenMatrix4fto3f(const Eigen::Matrix4f &m_3d, AxisType ignore_dim);

std::vector<std::vector<float>> EigenMatrix4f2VectorOfVector(const Eigen::Matrix4f &tf);

Eigen::Matrix4f VectorOfVector2EigenMatrix4f(const std::vector<std::vector<float>> &pose_vec);

Eigen::Matrix4f swapCloudAxis(const PointCloud &cloud, PointCloud *cloud_out, AxisType a,
                              AxisType b);

std::string MeasureToText(const std::string &value, const std::string &nominal,
                          const std::string &prefix);

}  // namespace conv
