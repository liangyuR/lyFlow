/*
 * Copyright (c) XYZ Robotics Inc. - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 * Author: jianming huang <jianming.huang@xyzrobotics.ai>, 2023/02/15
 */

#include "gap_detection/Converter.hpp"

namespace conv {

Eigen::Matrix4f EigenMatrix3fto4f(const Eigen::Matrix3f &m_2d, AxisType ignore_dim) {
  Eigen::Matrix4f m_3d = Eigen::Matrix4f::Identity();
  for (int row = 0; row < 3; ++row) {
    int dst_row = row < ignore_dim ? row : row + 1;
    for (int col = 0; col < 3; ++col) {
      int dst_col = col < ignore_dim ? col : col + 1;
      m_3d(dst_row, dst_col) = m_2d(row, col);
    }
  }
  return m_3d;
}

Eigen::Matrix3f EigenMatrix4fto3f(const Eigen::Matrix4f &m_3d, AxisType ignore_dim) {
  Eigen::Matrix3f m_2d = Eigen::Matrix3f::Identity();
  for (int row = 0; row < 3; ++row) {
    int src_row = row < ignore_dim ? row : row + 1;
    for (int col = 0; col < 3; ++col) {
      int src_col = col < ignore_dim ? col : col + 1;
      m_2d(row, col) = m_3d(src_row, src_col);
    }
  }
  return m_2d;
}

std::vector<std::vector<float>> EigenMatrix4f2VectorOfVector(const Eigen::Matrix4f &tf) {
  return {{tf(0, 0), tf(0, 1), tf(0, 2), tf(0, 3)},
          {tf(1, 0), tf(1, 1), tf(1, 2), tf(1, 3)},
          {tf(2, 0), tf(2, 1), tf(2, 2), tf(2, 3)},
          {tf(3, 0), tf(3, 1), tf(3, 2), tf(3, 3)}};
}

Eigen::Matrix4f VectorOfVector2EigenMatrix4f(const std::vector<std::vector<float>> &pose_vec) {
  Eigen::Matrix4f pose = Eigen::Matrix4f::Identity();
  for (int i = 0; i < pose_vec.size(); ++i) {
    for (int j = 0; j < pose_vec[i].size(); ++j) {
      pose(i, j) = pose_vec[i][j];
    }
  }
  return pose;
}

Eigen::Matrix4f swapCloudAxis(const PointCloud &cloud, PointCloud *cloud_out, AxisType a,
                              AxisType b) {
  if (cloud.empty()) {
    return Eigen::Matrix4f::Identity();
  }
  Eigen::Matrix4f transform = Eigen::Matrix4f::Identity();
  Eigen::Vector4f tmp = transform.col(a);
  transform.col(a) = transform.col(b);
  transform.col(b) = tmp;
  pcl::transformPointCloud(cloud, *cloud_out, transform);
  return transform;
}

std::string MeasureToText(const std::string &value, const std::string &nominal,
                          const std::string &prefix) {
  std::string prefix_fill = prefix, soll = nominal, ist = value;
  int n = 8;
  prefix_fill.insert(prefix_fill.end(), prefix.size() < n ? n - prefix.size() : 0, ' ');
  n -= 2;
  ist.insert(0, ist.size() < n ? n - ist.size() : 0, ' ');
  soll.insert(0, soll.size() < n ? n - soll.size() : 0, ' ');
  return prefix_fill + " Soll:" + soll + " Ist: " + ist + " (iO)";
}
}  // namespace conv
