#pragma once

#include <Eigen/Dense>
#include <pcl/search/kdtree.h>
#include <pcl/point_types.h>
#include <limits>

#include "lyflow/operator.h"

namespace lyflow::algo::p2 {
using Vec3 = Eigen::Vector3d;
using Mat4 = Eigen::Matrix4d;

struct FitBudget {
  std::size_t maximumQueries = 64U * 1024U * 1024U;
  std::size_t queries = 0;
};

struct TargetIndex {
  const PointCloud* cloud = nullptr;
  pcl::PointCloud<pcl::PointXYZ>::Ptr points;
  pcl::search::KdTree<pcl::PointXYZ> tree;
};

struct CloudFitOptions {
  double maxCorrespondenceDistance = 0.1;
  double maxRmse = 0.01;
  double minInlierRatio = 0.7;
  double translationTolerance = 1e-7;
  double rotationTolerance = 1e-7;
  int maxIterations = 100;
  bool pointToPlane = false;
};

struct CloudFitResult {
  Mat4 pose = Mat4::Identity();
  Transform transform;
  PointCloud aligned;
  std::vector<std::int32_t> targetIndices;
  std::vector<std::int32_t> targetSupport;
  std::size_t matchedCount = 0;
  double inlierRatio = 0;
  double rmse = std::numeric_limits<double>::infinity();
  int iterations = 0;
  bool converged = false;
  bool observable = false;
  bool accepted = false;
};

Status prepareTarget(const PointCloud& target, TargetIndex& index, ExecContext& ctx);
Status validateFitOptions(const CloudFitOptions& options);
Status evaluateCloudFit(const PointCloud& source, const TargetIndex& target, const Mat4& pose,
                        const CloudFitOptions& options, ExecContext& ctx, FitBudget& budget,
                        CloudFitResult& result);
Status refineCloudFit(const PointCloud& source, const TargetIndex& target, const Mat4& initial,
                      const CloudFitOptions& options, ExecContext& ctx, FitBudget& budget,
                      CloudFitResult& result);
} // namespace lyflow::algo::p2
