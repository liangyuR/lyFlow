#include "algo/icp2d.h"

#include <Eigen/Cholesky>
#include <Eigen/SVD>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

#include "algo/profile_geometry.h"

namespace lyflow::std_pc {
namespace {

Eigen::Matrix3d makeRigid2D(double angle, double x, double y) {
  Eigen::Matrix3d matrix = Eigen::Matrix3d::Identity();
  const double cosine = std::cos(angle);
  const double sine = std::sin(angle);
  matrix(0, 0) = cosine;
  matrix(0, 1) = -sine;
  matrix(1, 0) = sine;
  matrix(1, 1) = cosine;
  matrix(0, 2) = x;
  matrix(1, 2) = y;
  return matrix;
}

double wrapAngle(double angle) { return std::atan2(std::sin(angle), std::cos(angle)); }

}  // namespace

Icp2D::Icp2D(const Icp2DOptions& options) : options_(options) {
  targetCloud_.reset(new pcl::PointCloud<pcl::PointXYZ>);
}

void Icp2D::setTarget(const Cloud2D& target) {
  targetPoints_.clear();
  targetInTargetMean_ = Eigen::Matrix3d::Identity();
  targetCloud_.reset(new pcl::PointCloud<pcl::PointXYZ>);
  targetPoints_.reserve(target.size());
  for (const auto& point : target) {
    if (!std::isfinite(point.x) || !std::isfinite(point.y)) continue;
    targetPoints_.emplace_back(static_cast<double>(point.x), static_cast<double>(point.y));
  }
  if (targetPoints_.empty()) return;

  Eigen::Vector2d mean = Eigen::Vector2d::Zero();
  for (const auto& point : targetPoints_) mean += point;
  mean /= static_cast<double>(targetPoints_.size());
  targetInTargetMean_(0, 2) = mean.x();
  targetInTargetMean_(1, 2) = mean.y();

  targetCloud_->resize(targetPoints_.size());
  for (std::size_t i = 0; i < targetPoints_.size(); ++i) {
    targetPoints_[i] -= mean;
    (*targetCloud_)[i].x = static_cast<float>(targetPoints_[i].x());
    (*targetCloud_)[i].y = static_cast<float>(targetPoints_[i].y());
    (*targetCloud_)[i].z = 0.0F;
  }
  targetTree_.setInputCloud(targetCloud_);
}

void Icp2D::setSource(const Cloud2D& source) {
  sourcePoints_.clear();
  sourceNormals_.clear();
  lastSquaredDistances_.clear();
  sourcePoints_.reserve(source.size());
  for (const auto& point : source) {
    if (!std::isfinite(point.x) || !std::isfinite(point.y)) continue;
    sourcePoints_.emplace_back(static_cast<double>(point.x), static_cast<double>(point.y));
  }
  sourceNormals_ = estimateProfileNormals(sourcePoints_, options_.normalKnn);
}

Icp2DResult Icp2D::align(const Eigen::Matrix3f& initPose) {
  Icp2DResult result;
  const Eigen::Matrix3d initial = initPose.cast<double>();
  const Eigen::Matrix3d targetMeanInSource = targetInTargetMean_.inverse() * initial;
  lastSquaredDistances_.assign(sourcePoints_.size(), std::numeric_limits<double>::infinity());
  result.transform = (targetInTargetMean_ * targetMeanInSource).cast<float>();
  result.fitness = computeFitness(static_cast<float>(options_.fitnessDistance));
  if (sourcePoints_.empty() || targetPoints_.empty()) return result;

  const Eigen::Matrix2d initialRotation = targetMeanInSource.block<2, 2>(0, 0);
  const Eigen::Vector2d initialTranslation = targetMeanInSource.block<2, 1>(0, 2);
  std::vector<Eigen::Vector2d> basePoints(sourcePoints_.size());
  std::vector<Eigen::Vector2d> baseNormals(sourcePoints_.size());
  for (std::size_t i = 0; i < sourcePoints_.size(); ++i) {
    basePoints[i] = initialRotation * sourcePoints_[i] + initialTranslation;
    baseNormals[i] = initialRotation * sourceNormals_[i];
  }

  const double maxSquaredDistance = options_.maxMatchingDistance * options_.maxMatchingDistance;
  const std::size_t smoothLength = static_cast<std::size_t>(std::max(1, options_.smoothLength));

  Eigen::Matrix3d iteration = Eigen::Matrix3d::Identity();
  std::vector<double> angles{0.0};
  std::vector<Eigen::Vector2d> translations(1, Eigen::Vector2d::Zero());

  std::vector<int> nearestIndex;
  std::vector<float> nearestSquaredDistance;
  std::vector<double> squaredDistances(sourcePoints_.size());

  for (int step = 0; step < options_.maxIterationCount; ++step) {
    const Eigen::Matrix2d rotation = iteration.block<2, 2>(0, 0);
    const Eigen::Vector2d translation = iteration.block<2, 1>(0, 2);
    Eigen::Matrix3d information = Eigen::Matrix3d::Zero();
    Eigen::Vector3d gradient = Eigen::Vector3d::Zero();
    std::size_t matched = 0;
    std::fill(squaredDistances.begin(), squaredDistances.end(),
              std::numeric_limits<double>::infinity());

    for (std::size_t i = 0; i < basePoints.size(); ++i) {
      const Eigen::Vector2d reading = rotation * basePoints[i] + translation;
      pcl::PointXYZ query;
      query.x = static_cast<float>(reading.x());
      query.y = static_cast<float>(reading.y());
      query.z = 0.0F;
      nearestIndex.clear();
      nearestSquaredDistance.clear();
      if (targetTree_.nearestKSearch(query, 1, nearestIndex, nearestSquaredDistance) == 0) {
        continue;
      }
      const Eigen::Vector2d& reference = targetPoints_[static_cast<std::size_t>(nearestIndex[0])];
      const Eigen::Vector2d delta = reading - reference;
      const double squaredDistance = delta.squaredNorm();
      if (squaredDistance > maxSquaredDistance) continue;
      squaredDistances[i] = squaredDistance;

      const Eigen::Vector2d normal = rotation * baseNormals[i];
      const auto jacobian = computePointToPlaneJacobian(reading, normal);
      const Eigen::Vector3d row(jacobian.rotation, jacobian.translationX, jacobian.translationY);
      information += row * row.transpose();
      gradient -= row * delta.dot(normal);
      ++matched;
    }

    lastSquaredDistances_ = squaredDistances;
    ++result.iterations;
    if (matched == 0) break;

    Eigen::Vector3d solution = Eigen::Vector3d::Zero();
    bool solved = false;
    const Eigen::LLT<Eigen::Matrix3d> cholesky(information);
    if (cholesky.info() == Eigen::Success) {
      solution = cholesky.solve(gradient);
      solved = solution.allFinite();
    }
    if (!solved) {
      solution = information.jacobiSvd(Eigen::ComputeThinU | Eigen::ComputeThinV).solve(gradient);
    }
    if (!solution.allFinite()) break;

    iteration = makeRigid2D(solution(0), solution(1), solution(2)) * iteration;
    angles.push_back(std::atan2(iteration(1, 0), iteration(0, 0)));
    translations.push_back(iteration.block<2, 1>(0, 2));

    if (angles.size() > smoothLength) {
      const std::size_t last = angles.size() - 1;
      const std::size_t first = angles.size() - smoothLength;
      double rotationError = 0;
      double translationError = 0;
      for (std::size_t i = first; i <= last; ++i) {
        rotationError += std::abs(wrapAngle(angles[i] - angles[i - 1]));
        translationError += (translations[i] - translations[i - 1]).norm();
      }
      rotationError /= static_cast<double>(smoothLength);
      translationError /= static_cast<double>(smoothLength);
      if (rotationError < options_.minDiffRotErr && translationError < options_.minDiffTransErr) {
        result.converged = true;
        break;
      }
    }
  }

  result.transform = (targetInTargetMean_ * iteration * targetMeanInSource).cast<float>();
  result.fitness = computeFitness(static_cast<float>(options_.fitnessDistance));
  return result;
}

float Icp2D::computeFitness(float fitDist) const {
  if (lastSquaredDistances_.empty()) return 0.0F;
  const double threshold = static_cast<double>(fitDist) * static_cast<double>(fitDist);
  std::size_t matched = 0;
  for (double squaredDistance : lastSquaredDistances_) {
    if (squaredDistance < threshold) ++matched;
  }
  return static_cast<float>(static_cast<double>(matched) /
                            static_cast<double>(lastSquaredDistances_.size()));
}

}  // namespace lyflow::std_pc
