#include "p2_matching.h"

#include <Eigen/Cholesky>
#include <algorithm>
#include <cmath>
#include <exception>
#include <map>

#if defined(LYFLOW_WITH_KISS_MATCHER)
#include <kiss_matcher/KISSMatcher.hpp>
#include <small_gicp/registration/registration_helper.hpp>
#include <tbb/task_arena.h>

#include "lyflow_matching_runtime.hpp"
#endif

namespace lyflow::algo::p2 {
namespace {
Status failure(const char* code, const std::string& message) {
  return Status::Error(Phase::Execute, code, message);
}

Status cancelled(const ExecContext& ctx) {
  return ctx.cancelled() ? failure("cancelled", "三维匹配已在阶段边界取消") : Status::Ok();
}

bool input(const PointCloud& cloud) {
  if (!cloud.channelsConsistent() || cloud.pointCount() < 4 || cloud.pointCount() > 8192)
    return false;
  for (const auto value : cloud.xyz)
    if (!std::isfinite(value))
      return false;
  return true;
}

struct Normalized {
  Eigen::Vector3d origin;
  std::vector<Eigen::Vector3f> points;
};

Normalized normalized(const PointCloud& cloud, double scale) {
  Eigen::Vector3d lo(cloud.xyz[0], cloud.xyz[1], cloud.xyz[2]), hi = lo;
  for (std::size_t i = 0; i < cloud.pointCount(); ++i) {
    const Eigen::Vector3d point(cloud.xyz[3 * i], cloud.xyz[3 * i + 1], cloud.xyz[3 * i + 2]);
    lo = lo.cwiseMin(point);
    hi = hi.cwiseMax(point);
  }
  Normalized output;
  output.origin = (lo + hi) / 2;
  output.points.reserve(cloud.pointCount());
  for (std::size_t i = 0; i < cloud.pointCount(); ++i) {
    const Eigen::Vector3d p(cloud.xyz[3 * i], cloud.xyz[3 * i + 1], cloud.xyz[3 * i + 2]);
    const Eigen::Vector3d value = (p - output.origin) * scale;
    if (!value.allFinite() || value.cwiseAbs().maxCoeff() > 1000)
      throw std::range_error("归一化跨度超过KISS安全栅格范围；需显式ROI或调整分辨率");
    output.points.push_back(value.cast<float>());
  }
  return output;
}

std::uint64_t neighborBound(const Normalized& cloud, double radius, ExecContext& ctx) {
  using Cell = std::array<std::int64_t, 3>;
  std::map<Cell, std::uint64_t> cells;
  for (const auto& q : cloud.points) {
    Cell cell{};
    for (int axis = 0; axis < 3; ++axis)
      cell[axis] = static_cast<std::int64_t>(std::floor(q[axis] / radius));
    ++cells[cell];
  }
  std::uint64_t bound = 0;
  for (const auto& item : cells) {
    if (ctx.cancelled())
      break;
    std::uint64_t neighbors = 0;
    for (int z = -1; z <= 1; ++z)
      for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
          const auto found = cells.find({item.first[0] + x, item.first[1] + y, item.first[2] + z});
          if (found != cells.end())
            neighbors += found->second;
        }
    bound += item.second * neighbors;
  }
  return bound;
}

Eigen::Matrix4d restore(const Eigen::Matrix3d& rotation, const Eigen::Vector3d& translation,
                        const Normalized& model, const Normalized& scene, double scale) {
  Eigen::Matrix4d matrix = Eigen::Matrix4d::Identity();
  matrix.topLeftCorner<3, 3>() = rotation;
  matrix.topRightCorner<3, 1>() = scene.origin + translation / scale - rotation * model.origin;
  return matrix;
}
} // namespace

bool stationaryGicpStep(const Eigen::Matrix<double, 6, 6>& hessian,
                        const Eigen::Matrix<double, 6, 1>& gradient, double error,
                        std::size_t inliers, std::size_t iteration,
                        const GicpBackendOptions& options) {
  if (options.maxIterations < 2 ||
      iteration >= static_cast<std::size_t>(options.maxIterations - 1) || inliers < 4 ||
      !std::isfinite(error) || error < 0 || !hessian.allFinite() || !gradient.allFinite() ||
      !std::isfinite(options.voxelSize) || options.voxelSize <= 0 ||
      !std::isfinite(options.translationTolerance) || options.translationTolerance <= 0 ||
      !std::isfinite(options.rotationTolerance) || options.rotationTolerance <= 0)
    return false;
  const double scale = hessian.cwiseAbs().maxCoeff();
  if (scale <= 0 || (hessian - hessian.transpose()).cwiseAbs().maxCoeff() > scale * 1e-12)
    return false;
  const Eigen::LDLT<Eigen::Matrix<double, 6, 6>> decomposition(hessian);
  if (decomposition.info() != Eigen::Success || !decomposition.isPositive() ||
      !decomposition.vectorD().allFinite() || decomposition.vectorD().maxCoeff() <= 0 ||
      decomposition.vectorD().minCoeff() / decomposition.vectorD().maxCoeff() <= 1e-12)
    return false;
  const Eigen::Matrix<double, 6, 1> step = decomposition.solve(-gradient);
  if (decomposition.info() != Eigen::Success || !step.allFinite())
    return false;
  const auto residual = (hessian * step + gradient).eval();
  const double magnitude = std::max(1.0, gradient.cwiseAbs().maxCoeff());
  return residual.allFinite() && residual.cwiseAbs().maxCoeff() <= magnitude * 1e-10 &&
         step.head<3>().norm() <= options.rotationTolerance &&
         step.tail<3>().norm() <= options.translationTolerance * (0.01 / options.voxelSize);
}

Status kissGlobalCandidate(const PointCloud& model, const PointCloud& scene,
                           const GlobalBackendOptions& options, ExecContext& ctx,
                           GlobalBackendResult& result) {
  result = {};
  if (auto status = cancelled(ctx); !status.ok)
    return status;
  if (!input(model) || !input(scene))
    return failure("bad_input", "KISS后端需要各4..8192个有限点，先显式粗采样");
  if (!std::isfinite(options.voxelSize) || options.voxelSize < 1e-6 || options.voxelSize > 1 ||
      !std::isfinite(options.normalRadiusFactor) || options.normalRadiusFactor < 1 ||
      options.normalRadiusFactor > 10 || !std::isfinite(options.featureRadiusFactor) ||
      options.featureRadiusFactor <= options.normalRadiusFactor ||
      options.featureRadiusFactor > 20 || options.maxCorrespondences < 16 ||
      options.maxCorrespondences > 2048)
    return failure("bad_param", "KISS采样、邻域或对应数量参数非法");
#if !defined(LYFLOW_WITH_KISS_MATCHER)
  return failure("backend_unavailable", "构建未包含KISS-Matcher CPU后端");
#else
  try {
    const double scale = 0.01 / options.voxelSize;
    const auto source = normalized(model, scale), target = normalized(scene, scale);
    const auto sourcePairs = neighborBound(source, 0.01 * options.featureRadiusFactor, ctx);
    const auto targetPairs = neighborBound(target, 0.01 * options.featureRadiusFactor, ctx);
    if (auto status = cancelled(ctx); !status.ok)
      return status;
    result.neighborPairsBound = sourcePairs + targetPairs;
    result.estimatedMemoryBytes = 64ULL * 1024 * 1024 + 16 * std::max(sourcePairs, targetPairs) +
                                  1024ULL * (model.pointCount() + scene.pointCount()) +
                                  32ULL * options.maxCorrespondences * options.maxCorrespondences +
                                  32ULL * std::max(model.pointCount(), scene.pointCount()) *
                                      std::min(16, std::max(1, ctx.threadBudget()));
    if (result.neighborPairsBound > 16ULL * 1024 * 1024 ||
        result.estimatedMemoryBytes > 512ULL * 1024 * 1024)
      return failure("resource_limit", "KISS半径邻域上界超过16Mi对或预计内存超过512MiB");
    kiss_matcher::KISSMatcherConfig config(0.01f, true, false, 1.0f, options.maxCorrespondences,
                                           static_cast<float>(options.normalRadiusFactor),
                                           static_cast<float>(options.featureRadiusFactor));
    config.use_ratio_test_ = options.ratioTest;
    config.robin_mode_ = "max_core";
    const int threads = std::min(16, std::max(1, ctx.threadBudget()));
    tbb::task_arena arena(threads);
    arena.execute([&] {
      lyflow_matching_runtime::ThreadBudget budget(threads);
      kiss_matcher::KISSMatcher matcher(config);
      const auto coarse = matcher.estimate(source.points, target.points);
      result.valid = coarse.valid;
      result.initialCorrespondences = matcher.getInitialCorrespondences().size();
      result.finalCorrespondences = matcher.getFinalCorrespondences().size();
      const auto featurePoints = matcher.getKeypointsFromFasterPFH();
      result.modelFeatures = std::get<0>(featurePoints).size();
      result.sceneFeatures = std::get<1>(featurePoints).size();
      result.rotationInliers = matcher.getNumRotationInliers();
      result.translationInliers = matcher.getNumFinalInliers();
      result.unitScale = scale;
      if (coarse.valid)
        result.pose = restore(coarse.rotation, coarse.translation, source, target, scale);
    });
    if (auto status = cancelled(ctx); !status.ok)
      return status;
    if (result.valid && (!result.pose.allFinite() ||
                         std::abs(result.pose.topLeftCorner<3, 3>().determinant() - 1) > 1e-5))
      return failure("backend_error", "KISS输出不是有限右手刚体变换");
    return Status::Ok();
  } catch (const std::bad_alloc&) {
    return failure("resource_limit", "KISS后端内存分配失败");
  } catch (const std::range_error& e) {
    return failure("bad_input", e.what());
  } catch (const std::exception& e) {
    return failure("backend_error", std::string("KISS-Matcher：") + e.what());
  }
#endif
}

Status refineSmallGicp(const PointCloud& model, const PointCloud& scene,
                       const Eigen::Matrix4d& initial, const GicpBackendOptions& options,
                       ExecContext& ctx, GicpBackendResult& result) {
  result = {};
  if (auto status = cancelled(ctx); !status.ok)
    return status;
  if (!input(model) || !input(scene) || !initial.allFinite())
    return failure("bad_input", "GICP输入需要各4..8192点和有限初值");
  if (!std::isfinite(options.voxelSize) || options.voxelSize < 1e-6 || options.voxelSize > 1 ||
      !std::isfinite(options.maxCorrespondenceDistance) || options.maxCorrespondenceDistance <= 0 ||
      !std::isfinite(options.translationTolerance) || options.translationTolerance <= 0 ||
      !std::isfinite(options.rotationTolerance) || options.rotationTolerance <= 0 ||
      options.maxIterations < 1 || options.maxIterations > 1000)
    return failure("bad_param", "GICP采样、门限或迭代参数非法");
#if !defined(LYFLOW_WITH_KISS_MATCHER)
  return failure("backend_unavailable", "构建未包含small_gicp CPU后端");
#else
  try {
    const double scale = 0.01 / options.voxelSize;
    const auto source = normalized(model, scale), target = normalized(scene, scale);
    const Eigen::Matrix3d rotation = initial.topLeftCorner<3, 3>();
    Eigen::Isometry3d start = Eigen::Isometry3d::Identity();
    start.linear() = rotation;
    start.translation() =
        (rotation * source.origin + initial.topRightCorner<3, 1>() - target.origin) * scale;
    small_gicp::RegistrationSetting setting;
    setting.type = small_gicp::RegistrationSetting::GICP;
    setting.num_threads = 1;
    setting.downsampling_resolution = 0.005;
    setting.max_correspondence_distance = options.maxCorrespondenceDistance * scale;
    setting.translation_eps = options.translationTolerance * scale;
    setting.rotation_eps = options.rotationTolerance;
    setting.max_iterations = options.maxIterations;
    setting.verbose = false;
    const auto fit = small_gicp::align(target.points, source.points, start, setting);
    if (auto status = cancelled(ctx); !status.ok)
      return status;
    result.stationary =
        !fit.converged &&
        stationaryGicpStep(fit.H, fit.b, fit.error, fit.num_inliers, fit.iterations, options);
    result.converged = fit.converged || result.stationary;
    result.inliers = fit.num_inliers;
    result.iterations = fit.iterations;
    result.pose = restore(fit.T_target_source.linear(), fit.T_target_source.translation(), source,
                          target, scale);
    if (!result.pose.allFinite())
      return failure("backend_error", "small_gicp输出非有限位姿");
    return Status::Ok();
  } catch (const std::bad_alloc&) {
    return failure("resource_limit", "small_gicp后端内存分配失败");
  } catch (const std::range_error& e) {
    return failure("bad_input", e.what());
  } catch (const std::exception& e) {
    return failure("backend_error", std::string("small_gicp：") + e.what());
  }
#endif
}
} // namespace lyflow::algo::p2
