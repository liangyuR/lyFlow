#include "p2_fit.h"

#include <Eigen/Eigenvalues>
#include <Eigen/SVD>
#include <algorithm>
#include <cmath>

#include "../ops/p1_common.h"
#include "lyflow_pcl/adapter.h"

namespace lyflow::algo::p2 {
namespace {
using Mat3 = Eigen::Matrix3d;
using V6 = Eigen::Matrix<double, 6, 1>;
using M6 = Eigen::Matrix<double, 6, 6>;
constexpr std::size_t kMaxPoints = 1024U * 1024U;
const double kSearchSpan = std::sqrt(static_cast<double>(std::numeric_limits<float>::max())) / 8;

Status failure(const std::string& text, const std::string& code = "bad_input") {
  return Status::Error(Phase::Execute, code, text);
}

Status checkCloud(const PointCloud& cloud) {
  Status status = ops::p1::validate(cloud, 3);
  if (!status.ok)
    return status;
  if (cloud.pointCount() > kMaxPoints)
    return failure("配准单云最多1048576点", "resource_limit");
  if (cloud.hasNormals())
    for (std::size_t i = 0; i < cloud.pointCount(); ++i)
      if (!(Vec3(cloud.normals[3 * i], cloud.normals[3 * i + 1], cloud.normals[3 * i + 2]).norm() >
            0))
        return failure("法线必须非零；不默默修补无效通道");
  return Status::Ok();
}

struct Pair {
  Vec3 a, b, normal;
};

Status correspondences(const PointCloud& source, const TargetIndex& target, const Mat4& pose,
                       const CloudFitOptions& options, ExecContext& ctx, FitBudget& budget,
                       std::vector<Pair>& pairs, std::vector<std::int32_t>* assignments = nullptr) {
  if (source.pointCount() > budget.maximumQueries - std::min(budget.queries, budget.maximumQueries))
    return failure("配准累计近邻查询超过预算", "resource_limit");
  budget.queries += source.pointCount();
  pairs.clear();
  pairs.reserve(source.pointCount());
  if (assignments)
    assignments->assign(source.pointCount(), -1);
  std::vector<int> indices(1);
  std::vector<float> distances(1);
  for (std::size_t i = 0; i < source.pointCount(); ++i) {
    if ((i & 255U) == 0 && ctx.cancelled())
      return failure("配准已取消", "cancelled");
    const Vec3 p =
        pose.topLeftCorner<3, 3>() * ops::p1::point(source, i) + pose.topRightCorner<3, 1>();
    if (!p.allFinite() || p.cwiseAbs().maxCoeff() > std::numeric_limits<float>::max() ||
        (p - ops::p1::point(*target.cloud, 0)).cwiseAbs().maxCoeff() > kSearchSpan)
      continue;
    if (target.tree.nearestKSearch(pcl::PointXYZ(static_cast<float>(p.x()),
                                                 static_cast<float>(p.y()),
                                                 static_cast<float>(p.z())),
                                   1, indices, distances) != 1)
      continue;
    const auto index = static_cast<std::size_t>(indices[0]);
    const Vec3 q = ops::p1::point(*target.cloud, index);
    if ((p - q).norm() > options.maxCorrespondenceDistance)
      continue;
    Vec3 normal = Vec3::Zero();
    if (options.pointToPlane) {
      normal = Vec3(target.cloud->normals[index * 3], target.cloud->normals[index * 3 + 1],
                    target.cloud->normals[index * 3 + 2])
                   .normalized();
    }
    pairs.push_back({p, q, normal});
    if (assignments)
      (*assignments)[i] = static_cast<std::int32_t>(index);
  }
  return Status::Ok();
}

bool observable(const std::vector<Pair>& pairs, bool plane, ExecContext& ctx) {
  if (pairs.size() < (plane ? 6U : 3U))
    return false;
  Vec3 a = Vec3::Zero(), b = Vec3::Zero();
  for (std::size_t i = 0; i < pairs.size(); ++i) {
    if ((i & 255U) == 0 && ctx.cancelled())
      return false;
    a += pairs[i].a;
    b += pairs[i].b;
  }
  a /= static_cast<double>(pairs.size());
  b /= static_cast<double>(pairs.size());
  Mat3 sourceScatter = Mat3::Zero(), targetScatter = Mat3::Zero(), covariance = Mat3::Zero();
  M6 h = M6::Zero();
  for (std::size_t i = 0; i < pairs.size(); ++i) {
    if ((i & 255U) == 0 && ctx.cancelled())
      return false;
    const auto& pair = pairs[i];
    const Vec3 x = pair.a - a, y = pair.b - b;
    sourceScatter += x * x.transpose();
    targetScatter += y * y.transpose();
    covariance += x * y.transpose();
    if (plane) {
      V6 j;
      j.head<3>() = x.cross(pair.normal);
      j.tail<3>() = pair.normal;
      h += j * j.transpose();
    }
  }
  for (const Mat3* scatter : {&sourceScatter, &targetScatter}) {
    Eigen::SelfAdjointEigenSolver<Mat3> eigen(*scatter);
    if (eigen.info() != Eigen::Success || !eigen.eigenvalues().allFinite() ||
        !(eigen.eigenvalues()[2] > 0) || eigen.eigenvalues()[1] <= eigen.eigenvalues()[2] * 1e-10)
      return false;
  }
  Eigen::JacobiSVD<Mat3> svd(covariance);
  if (!svd.singularValues().allFinite() || !(svd.singularValues()[0] > 0) ||
      svd.singularValues()[1] <= svd.singularValues()[0] * 1e-10)
    return false;
  if (!plane)
    return true;
  V6 scale;
  for (int axis = 0; axis < 6; ++axis) {
    if (!(h(axis, axis) > 0))
      return false;
    scale[axis] = 1 / std::sqrt(h(axis, axis));
  }
  const M6 normalized = scale.asDiagonal() * h * scale.asDiagonal();
  Eigen::SelfAdjointEigenSolver<M6> eigen(normalized);
  return eigen.info() == Eigen::Success && eigen.eigenvalues().allFinite() &&
         eigen.eigenvalues()[5] > 0 && eigen.eigenvalues()[0] > eigen.eigenvalues()[5] * 1e-10;
}

bool step(const std::vector<Pair>& pairs, bool plane, Mat4& delta) {
  if (pairs.size() < (plane ? 6U : 3U))
    return false;
  Vec3 center = Vec3::Zero(), other = Vec3::Zero();
  for (const auto& pair : pairs) {
    center += pair.a;
    other += pair.b;
  }
  center /= static_cast<double>(pairs.size());
  other /= static_cast<double>(pairs.size());
  Mat3 rotation;
  Vec3 translation;
  if (!plane) {
    Mat3 covariance = Mat3::Zero();
    for (const auto& pair : pairs)
      covariance += (pair.a - center) * (pair.b - other).transpose();
    Eigen::JacobiSVD<Mat3> svd(covariance, Eigen::ComputeFullU | Eigen::ComputeFullV);
    if (!svd.singularValues().allFinite() || !(svd.singularValues()[0] > 0) ||
        svd.singularValues()[1] <= svd.singularValues()[0] * 1e-10)
      return false;
    Mat3 parity = Mat3::Identity();
    parity(2, 2) = (svd.matrixV() * svd.matrixU().transpose()).determinant() < 0 ? -1 : 1;
    rotation = svd.matrixV() * parity * svd.matrixU().transpose();
    translation = other - rotation * center;
  } else {
    M6 h = M6::Zero();
    V6 g = V6::Zero();
    for (const auto& pair : pairs) {
      V6 j;
      j.head<3>() = (pair.a - center).cross(pair.normal);
      j.tail<3>() = pair.normal;
      h += j * j.transpose();
      g += j * pair.normal.dot(pair.b - pair.a);
    }
    V6 scale;
    for (int i = 0; i < 6; ++i) {
      if (!(h(i, i) > 0))
        return false;
      scale[i] = 1 / std::sqrt(h(i, i));
    }
    const M6 normalized = scale.asDiagonal() * h * scale.asDiagonal();
    Eigen::SelfAdjointEigenSolver<M6> eigen(normalized);
    if (eigen.info() != Eigen::Success || eigen.eigenvalues()[0] <= eigen.eigenvalues()[5] * 1e-10)
      return false;
    const V6 update = scale.asDiagonal() * normalized.ldlt().solve(scale.asDiagonal() * g);
    if (!update.allFinite() || update.head<3>().norm() > 0.5)
      return false;
    const double angle = update.head<3>().norm();
    rotation = angle > 0 ? Eigen::AngleAxisd(angle, update.head<3>() / angle).toRotationMatrix()
                         : Mat3::Identity();
    translation = center - rotation * center + update.tail<3>();
  }
  delta = Mat4::Identity();
  delta.topLeftCorner<3, 3>() = rotation;
  delta.topRightCorner<3, 1>() = translation;
  return delta.allFinite();
}
} // namespace

Status validateFitOptions(const CloudFitOptions& options) {
  for (double value : {options.maxCorrespondenceDistance, options.maxRmse,
                       options.translationTolerance, options.rotationTolerance})
    if (!std::isfinite(value) || value <= 0 || value > 1e6)
      return failure("距离/收敛门限必须为(0,1e6]有限数", "bad_param");
  if (!std::isfinite(options.minInlierRatio) || options.minInlierRatio <= 0 ||
      options.minInlierRatio > 1 || options.maxIterations < 1 || options.maxIterations > 1000)
    return failure("匹配比例必须为(0,1]，迭代次数为[1,1000]", "bad_param");
  return Status::Ok();
}

Status prepareTarget(const PointCloud& target, TargetIndex& index, ExecContext& ctx) {
  const Status status = checkCloud(target);
  if (!status.ok)
    return status;
  for (std::size_t i = 0; i < target.pointCount(); ++i) {
    if ((i & 255U) == 0 && ctx.cancelled())
      return failure("索引构建已取消", "cancelled");
    if ((ops::p1::point(target, i) - ops::p1::point(target, 0)).cwiseAbs().maxCoeff() > kSearchSpan)
      return failure("目标跨度超过float近邻索引安全范围");
  }
  index.points = ops::adapter::toPcl(target);
  index.tree.setInputCloud(index.points);
  index.cloud = &target;
  if (ctx.cancelled())
    return failure("索引构建已取消", "cancelled");
  return Status::Ok();
}

Status evaluateCloudFit(const PointCloud& source, const TargetIndex& target, const Mat4& matrix,
                        const CloudFitOptions& options, ExecContext& ctx, FitBudget& budget,
                        CloudFitResult& result) {
  result = CloudFitResult{};
  Status status = checkCloud(source);
  if (!status.ok)
    return status;
  status = validateFitOptions(options);
  if (!status.ok)
    return status;
  if (!target.cloud || (options.pointToPlane && !target.cloud->hasNormals()))
    return failure("目标索引未初始化或点面模式缺少法线");
  if (!ops::p1::transform(matrix, result.transform) ||
      !ops::p1::rigid(result.transform, result.pose) ||
      !ops::p1::transformed(source, result.pose, result.aligned, ctx)) {
    if (ctx.cancelled())
      return failure("配准交付已取消", "cancelled");
    return failure("交付变换或点云超出float32刚体/坐标范围");
  }
  for (std::size_t i = 0; i < source.pointCount(); ++i) {
    if ((i & 255U) == 0 && ctx.cancelled())
      return failure("配准交付已取消", "cancelled");
    for (int axis = 0; axis < 3; ++axis) {
      const float* row = result.transform.m + 4 * axis;
      const float value = row[0] * source.xyz[3 * i] + row[1] * source.xyz[3 * i + 1] +
                          row[2] * source.xyz[3 * i + 2] + row[3];
      if (!std::isfinite(value))
        return failure("交付float32计算溢出");
      result.aligned.xyz[3 * i + axis] = value;
    }
  }
  std::vector<Pair> pairs;
  status = correspondences(result.aligned, target, Mat4::Identity(), options, ctx, budget, pairs,
                           &result.targetIndices);
  if (!status.ok)
    return status;
  result.matchedCount = pairs.size();
  result.inlierRatio = static_cast<double>(pairs.size()) / source.pointCount();
  double sum = 0;
  for (const auto& pair : pairs)
    sum += (pair.a - pair.b).squaredNorm();
  if (!pairs.empty())
    result.rmse = std::sqrt(sum / static_cast<double>(pairs.size()));
  for (const auto index : result.targetIndices)
    if (index >= 0)
      result.targetSupport.push_back(index);
  std::sort(result.targetSupport.begin(), result.targetSupport.end());
  result.targetSupport.erase(std::unique(result.targetSupport.begin(), result.targetSupport.end()),
                             result.targetSupport.end());
  result.observable =
      result.targetSupport.size() >= 3 && observable(pairs, options.pointToPlane, ctx);
  result.converged = result.observable;
  if (ctx.cancelled())
    return failure("交付支持可观性检查已取消", "cancelled");
  result.accepted = result.observable && result.inlierRatio >= options.minInlierRatio &&
                    result.rmse <= options.maxRmse;
  return Status::Ok();
}

Status refineCloudFit(const PointCloud& source, const TargetIndex& target, const Mat4& initial,
                      const CloudFitOptions& options, ExecContext& ctx, FitBudget& budget,
                      CloudFitResult& result) {
  Status status = checkCloud(source);
  if (!status.ok)
    return status;
  status = validateFitOptions(options);
  if (!status.ok)
    return status;
  if (!target.cloud || (options.pointToPlane && !target.cloud->hasNormals()))
    return failure("目标索引未初始化或点面ICP缺少目标法线");
  Transform native;
  Mat4 rounded;
  if (!ops::p1::transform(initial, native) || !ops::p1::rigid(native, rounded))
    return failure("初始变换必须是有限右手刚体变换");
  Mat4 pose = initial;
  bool converged = false;
  int iterations = 0;
  for (int iteration = 0; iteration < options.maxIterations; ++iteration) {
    if (ctx.cancelled())
      return failure("ICP已取消", "cancelled");
    std::vector<Pair> pairs;
    status = correspondences(source, target, pose, options, ctx, budget, pairs);
    if (!status.ok)
      return status;
    Mat4 delta;
    if (!step(pairs, options.pointToPlane, delta))
      break;
    pose = delta * pose;
    iterations = iteration + 1;
    if (delta.topRightCorner<3, 1>().norm() <= options.translationTolerance &&
        Eigen::AngleAxisd(delta.topLeftCorner<3, 3>()).angle() <= options.rotationTolerance) {
      converged = true;
      break;
    }
  }
  status = evaluateCloudFit(source, target, pose, options, ctx, budget, result);
  if (!status.ok)
    return status;
  result.iterations = iterations;
  result.converged = converged && result.observable;
  result.accepted = result.accepted && result.converged;
  return Status::Ok();
}
} // namespace lyflow::algo::p2
