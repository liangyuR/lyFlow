#pragma once
// 2D point-to-plane ICP。源码迁自 xyz-gap-inspector 的 gap_core/Icp2D，
// 默认走迁移时的最小二乘路径，可选启用鲁棒权重。
#include <pcl/kdtree/kdtree_flann.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>

#include <Eigen/Core>
#include <limits>
#include <vector>

#include "algo/cloud2d.h"

namespace lyflow::std_pc {

enum class Icp2DRobustLoss { None, Huber, Tukey };

struct Icp2DOptions {
  double maxMatchingDistance = 0.001;
  double fitnessDistance = 0.01;
  int maxIterationCount = 1000;
  int normalKnn = 10;
  double minDiffRotErr = 1e-3;
  double minDiffTransErr = 1e-4;
  int smoothLength = 4;
  Icp2DRobustLoss robustLoss = Icp2DRobustLoss::None;
  double robustScale = 0.001;
};

struct Icp2DResult {
  Eigen::Matrix3f transform = Eigen::Matrix3f::Identity();
  float fitness = 0.0F;
  int iterations = 0;
  bool converged = false;
};

struct Icp2DQuality {
  std::size_t sourceCount = 0;
  std::size_t targetCount = 0;
  std::size_t matchedCount = 0;
  std::size_t normalMatchedCount = 0;
  std::size_t effectiveMatchedCount = 0;
  double inlierRatio = 0.0;
  double pointRmse = std::numeric_limits<double>::quiet_NaN();
  double planeRmse = std::numeric_limits<double>::quiet_NaN();
  double weightedPlaneRmse = std::numeric_limits<double>::quiet_NaN();
  double weightSum = 0.0;
};

class Icp2D {
public:
  explicit Icp2D(const Icp2DOptions& options);

  void setTarget(const Cloud2D& target);
  void setSource(const Cloud2D& source);

  Icp2DResult align(const Eigen::Matrix3f& initPose = Eigen::Matrix3f::Identity());

  float computeFitness(float fitDist) const;
  Icp2DQuality evaluatePose(const Eigen::Matrix3f& pose) const;

  const Icp2DOptions& options() const { return options_; }
  std::size_t sourcePointCount() const { return sourcePoints_.size(); }
  std::size_t targetPointCount() const { return targetPoints_.size(); }

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

} // namespace lyflow::std_pc
