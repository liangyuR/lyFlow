#pragma once

#include <Eigen/Geometry>

#include "lyflow/operator.h"

namespace lyflow::algo::p2 {

struct GlobalBackendOptions {
  double voxelSize = 0.001;
  double normalRadiusFactor = 3;
  double featureRadiusFactor = 5;
  int maxCorrespondences = 512;
  bool ratioTest = false;
};

struct GlobalBackendResult {
  Eigen::Matrix4d pose = Eigen::Matrix4d::Identity();
  bool valid = false;
  std::size_t initialCorrespondences = 0;
  std::size_t finalCorrespondences = 0;
  std::size_t modelFeatures = 0;
  std::size_t sceneFeatures = 0;
  std::size_t rotationInliers = 0;
  std::size_t translationInliers = 0;
  std::uint64_t neighborPairsBound = 0;
  std::uint64_t estimatedMemoryBytes = 0;
  double unitScale = 1;
};

struct GicpBackendOptions {
  double voxelSize = 0.001;
  double maxCorrespondenceDistance = 0.003;
  double translationTolerance = 1e-7;
  double rotationTolerance = 1e-7;
  int maxIterations = 50;
};

struct GicpBackendResult {
  Eigen::Matrix4d pose = Eigen::Matrix4d::Identity();
  bool converged = false;
  std::size_t inliers = 0;
  int iterations = 0;
};

Status kissGlobalCandidate(const PointCloud& model, const PointCloud& scene,
                           const GlobalBackendOptions& options, ExecContext& ctx,
                           GlobalBackendResult& result);
Status refineSmallGicp(const PointCloud& model, const PointCloud& scene,
                       const Eigen::Matrix4d& initial, const GicpBackendOptions& options,
                       ExecContext& ctx, GicpBackendResult& result);

} // namespace lyflow::algo::p2
