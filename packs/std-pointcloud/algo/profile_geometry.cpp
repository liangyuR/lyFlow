#include "algo/profile_geometry.h"

#include <pcl/kdtree/kdtree_flann.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>

#include <Eigen/Eigenvalues>
#include <algorithm>
#include <cstddef>

namespace lyflow::std_pc {

std::optional<Eigen::Vector2d> estimateNormalFromNeighborhood(
    const std::vector<Eigen::Vector2d>& neighborhood) {
  if (neighborhood.size() < 2) return std::nullopt;
  const double count = static_cast<double>(neighborhood.size());
  Eigen::Vector2d centroid = Eigen::Vector2d::Zero();
  for (const auto& point : neighborhood) centroid += point;
  centroid /= count;
  Eigen::Matrix2d covariance = Eigen::Matrix2d::Zero();
  for (const auto& point : neighborhood) {
    const Eigen::Vector2d delta = point - centroid;
    covariance += delta * delta.transpose();
  }
  covariance /= count;
  const Eigen::SelfAdjointEigenSolver<Eigen::Matrix2d> solver(covariance);
  if (solver.info() != Eigen::Success) return std::nullopt;
  const Eigen::Vector2d normal = solver.eigenvectors().col(0).normalized();
  return Eigen::Vector2d(normal.cwiseMax(-1.0).cwiseMin(1.0));
}

std::vector<Eigen::Vector2d> estimateProfileNormals(const std::vector<Eigen::Vector2d>& points,
                                                    int knn) {
  std::vector<Eigen::Vector2d> normals(points.size(), Eigen::Vector2d::Zero());
  if (points.size() < 2) return normals;

  const int neighborCount = std::min(static_cast<int>(points.size()), std::max(2, knn));
  pcl::PointCloud<pcl::PointXYZ>::Ptr cloud(new pcl::PointCloud<pcl::PointXYZ>);
  cloud->resize(points.size());
  for (std::size_t i = 0; i < points.size(); ++i) {
    (*cloud)[i].x = static_cast<float>(points[i].x());
    (*cloud)[i].y = static_cast<float>(points[i].y());
    (*cloud)[i].z = 0.0F;
  }
  pcl::KdTreeFLANN<pcl::PointXYZ> tree;
  tree.setInputCloud(cloud);

  std::vector<int> neighborIndices;
  std::vector<float> neighborDistances;
  std::vector<Eigen::Vector2d> neighborhood;
  neighborhood.reserve(static_cast<std::size_t>(neighborCount));
  for (std::size_t i = 0; i < points.size(); ++i) {
    neighborIndices.clear();
    neighborDistances.clear();
    const int found =
        tree.nearestKSearch(static_cast<int>(i), neighborCount, neighborIndices, neighborDistances);
    if (found < 2) continue;
    neighborhood.clear();
    for (auto index : neighborIndices) {
      neighborhood.push_back(points[static_cast<std::size_t>(index)]);
    }
    const auto normal = estimateNormalFromNeighborhood(neighborhood);
    if (normal) normals[i] = *normal;
  }
  return normals;
}

PointToPlaneJacobian2D computePointToPlaneJacobian(const Eigen::Vector2d& point,
                                                   const Eigen::Vector2d& normal) {
  PointToPlaneJacobian2D jacobian;
  jacobian.translationX = normal.x();
  jacobian.translationY = normal.y();
  jacobian.rotation = point.x() * normal.y() - point.y() * normal.x();
  return jacobian;
}

}  // namespace lyflow::std_pc
