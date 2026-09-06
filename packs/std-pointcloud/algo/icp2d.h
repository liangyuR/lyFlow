#pragma once
// 2D point-to-plane ICP。源码迁自 xyz-gap-inspector 的 gap_core/Icp2D，
// 命名空间改为 lyflow::std_pc，方法名改成 lowerCamel（T3）；数值一行未动。
#include <pcl/kdtree/kdtree_flann.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>

#include <Eigen/Core>
#include <vector>

#include "algo/cloud2d.h"

namespace lyflow::std_pc {

struct Icp2DOptions {
  double maxMatchingDistance = 0.001;
  double fitnessDistance = 0.01;
  int maxIterationCount = 1000;
  int normalKnn = 10;
  double minDiffRotErr = 1e-3;
  double minDiffTransErr = 1e-4;
  int smoothLength = 4;
};

struct Icp2DResult {
  Eigen::Matrix3f transform = Eigen::Matrix3f::Identity();
  float fitness = 0.0F;
  int iterations = 0;
  bool converged = false;
};

class Icp2D {
 public:
  explicit Icp2D(const Icp2DOptions& options);

  void setTarget(const Cloud2D& target);
  void setSource(const Cloud2D& source);

  Icp2DResult align(const Eigen::Matrix3f& initPose = Eigen::Matrix3f::Identity());

  float computeFitness(float fitDist) const;

  const Icp2DOptions& options() const { return options_; }

 private:
  Icp2DOptions options_;
  Eigen::Matrix3d targetInTargetMean_ = Eigen::Matrix3d::Identity();
  std::vector<Eigen::Vector2d> targetPoints_;
  pcl::PointCloud<pcl::PointXYZ>::Ptr targetCloud_;
  pcl::KdTreeFLANN<pcl::PointXYZ> targetTree_;
  std::vector<Eigen::Vector2d> sourcePoints_;
  std::vector<Eigen::Vector2d> sourceNormals_;
  std::vector<double> lastSquaredDistances_;
};

}  // namespace lyflow::std_pc
