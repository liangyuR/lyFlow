// 迁自 xyz-gap-inspector 的 GapUtils::fitLine / fitCircle / fitCircleFixedRadius。
// PCL 调用、常数、比较方向照原实现写；改其中任何一处都会改变 gap 包的读数。
#include "algo/fit2d.h"

#include <pcl/ModelCoefficients.h>
#include <pcl/common/io.h>
#include <pcl/filters/extract_indices.h>
#include <pcl/sample_consensus/ransac.h>
#include <pcl/sample_consensus/sac_model_circle.h>
#include <pcl/sample_consensus/sac_model_line.h>
#include <pcl/segmentation/sac_segmentation.h>

#include <Eigen/Dense>
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace lyflow::std_pc {
namespace {

constexpr double kPi = 3.14159265358979323846;

/// 定半径圆心的最小二乘细化（GapUtils::fitCircleFixedRadius）。
void refineFixedRadiusCenter(const Cloud2D& cloud, const pcl::Indices& indices, double radius,
                             double* centerX, double* centerY) {
  const int m = static_cast<int>(indices.size());
  Eigen::MatrixXd x(2, m);
  for (int i = 0; i < m; i++) {
    x(0, i) = cloud[indices[static_cast<std::size_t>(i)]].x;
    x(1, i) = cloud[indices[static_cast<std::size_t>(i)]].y;
  }
  Eigen::Vector2d center(*centerX, *centerY);
  Eigen::MatrixXd j(2 * m, 2);
  Eigen::VectorXd rhs(2 * m);
  auto id = Eigen::Matrix2d::Identity();
  const int maxIteration = 1000;
  const double step = 0.1;
  const double tolerance = 1e-7;
  for (int it = 0; it < maxIteration; ++it) {
    for (int i = 0; i < m; ++i) {
      auto v = x.col(i) - center;
      auto d = v.norm();
      auto twoI = 2 * i;
      j.block<2, 2>(twoI, 0) = id - (id - v * v.transpose() / (d * d)) * radius / d;
      rhs.segment<2>(twoI) = v / d * (d - radius);
    }
    Eigen::Vector2d delta = j.colPivHouseholderQr().solve(rhs);
    center += step * delta;
    if (delta.norm() < tolerance || it == maxIteration - 1) {
      *centerX = center[0];
      *centerY = center[1];
      break;
    }
  }
}

/// 收内点，算术与 SampleConsensusModelCircle2D::selectWithinDistance 一致（float 平方比较），
/// 只是不先查半径界 —— 钉在界上的半径转成 float 之后可能比 double 的界多出一个 ULP，
/// PCL 那条会因此判模型非法、一个点都不收。
void selectCircleInliers(const Cloud2D& cloud, const Eigen::VectorXf& c, double threshold,
                         pcl::Indices* inliers) {
  inliers->clear();
  const float sqrInner = (c[2] <= threshold ? 0.0F : (c[2] - threshold) * (c[2] - threshold));
  const float sqrOuter = (c[2] + threshold) * (c[2] + threshold);
  for (std::size_t i = 0; i < cloud.size(); ++i) {
    const float sqrDist = (cloud[i].x - c[0]) * (cloud[i].x - c[0]) +
                          (cloud[i].y - c[1]) * (cloud[i].y - c[1]);
    if (sqrDist <= sqrOuter && sqrDist >= sqrInner) inliers->push_back(static_cast<int>(i));
  }
}

/// 带半径先验的几何细化（Levenberg–Marquardt）。最小化
///   Σ ((|pᵢ − c| − r) / noise)² + ((r − prior) / sigma)²
/// 初值是 RANSAC 的圆，点是它的内点 —— 与 PCL 的 optimizeModelCoefficients 同一个起点，
/// 只是多一项先验。短弧上半径与圆心沿法向几乎可以互相抵消，点残差对 r 很平，先验项
/// 就把 r 收向 prior；弧长时点残差对 r 很陡，先验项几乎不起作用。
Eigen::VectorXf refineCircleWithPrior(const Cloud2D& cloud, const pcl::Indices& indices,
                                      const Eigen::VectorXf& start, double noise, double prior,
                                      double sigma) {
  const auto cost = [&](const Eigen::Vector3d& p) {
    double s = 0;
    for (const int i : indices) {
      const auto& q = cloud[static_cast<std::size_t>(i)];
      const double e = (std::hypot(q.x - p[0], q.y - p[1]) - p[2]) / noise;
      s += e * e;
    }
    const double e = (p[2] - prior) / sigma;
    return s + e * e;
  };
  Eigen::Vector3d p(start[0], start[1], start[2]);
  double current = cost(p);
  double lambda = 1e-3;
  for (int it = 0; it < 200; ++it) {
    Eigen::Matrix3d h = Eigen::Matrix3d::Zero();
    Eigen::Vector3d g = Eigen::Vector3d::Zero();
    for (const int i : indices) {
      const auto& q = cloud[static_cast<std::size_t>(i)];
      const double dx = q.x - p[0], dy = q.y - p[1];
      const double d = std::hypot(dx, dy);
      if (!(d > 0)) continue;
      const Eigen::Vector3d j(-dx / d / noise, -dy / d / noise, -1.0 / noise);
      const double e = (d - p[2]) / noise;
      h += j * j.transpose();
      g += j * e;
    }
    const Eigen::Vector3d jp(0, 0, 1.0 / sigma);
    h += jp * jp.transpose();
    g += jp * ((p[2] - prior) / sigma);
    bool improved = false;
    Eigen::Vector3d step = Eigen::Vector3d::Zero();
    for (int tries = 0; tries < 20 && !improved; ++tries) {
      Eigen::Matrix3d damped = h;
      damped.diagonal() *= 1.0 + lambda;
      step = damped.ldlt().solve(-g);
      const Eigen::Vector3d next = p + step;
      const double c = cost(next);
      if (std::isfinite(c) && c < current) {
        p = next;
        current = c;
        lambda = std::max(lambda / 10.0, 1e-12);
        improved = true;
      } else {
        lambda *= 10.0;
      }
    }
    if (!improved || step.norm() < 1e-12) break;
  }
  Eigen::VectorXf out(3);
  out << static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2]);
  return out;
}

}  // namespace

bool fitLine2D(const Cloud2D& cloud, Eigen::VectorXf* line, pcl::Indices* inliers,
               const Line2DFitOptions& options) {
  // 线扫剖面常常带同一表面的两条近平行迹线（相隔约 0.3 mm 的二次反射）。
  // 假设选择用更紧的阈值隔出单条迹线，配置阈值只在重收内点时用。
  constexpr float kTightThreshFactor = 3.0F;
  constexpr double kParallelToleranceDeg = 3.0;
  constexpr float kCompanionOffsetUpperCapFactor = 10.0F;
  constexpr std::size_t kMinCompanionPoints = 5;
  constexpr double kCompanionSupportRatio = 0.3;

  const float distThresh = options.distThresh;
  const float tightThresh = distThresh / kTightThreshFactor;

  pcl::SampleConsensusModelLine<Point2DT>::Ptr modelLine(
      new pcl::SampleConsensusModelLine<Point2DT>(cloud.makeShared(), false));
  pcl::RandomSampleConsensus<Point2DT> lineRansac(modelLine);
  lineRansac.setDistanceThreshold(tightThresh);
  lineRansac.setMaxIterations(options.maxIterations);
  lineRansac.computeModel();
  pcl::Indices inliers1;
  lineRansac.getInliers(inliers1);
  if (inliers1.size() < 2) {
    // 稀疏云的单段回退，与原实现同一条路径
    lineRansac.setDistanceThreshold(distThresh);
    lineRansac.computeModel();
    lineRansac.getInliers(*inliers);
    if (inliers->size() < 2) return false;
    Eigen::VectorXf lineRes;
    lineRansac.getModelCoefficients(lineRes);
    if (lineRes.size() == 0) return false;
    if (options.optimize) {
      modelLine->optimizeModelCoefficients(*inliers, lineRes, *line);
    } else {
      *line = lineRes;
    }
    return line->size() > 0;
  }
  Eigen::VectorXf coeff1Raw;
  lineRansac.getModelCoefficients(coeff1Raw);
  Eigen::VectorXf coeff1;
  if (options.optimize) {
    modelLine->optimizeModelCoefficients(inliers1, coeff1Raw, coeff1);
  } else {
    coeff1 = coeff1Raw;
  }

  // inliers1 的补集（升序，pcl::getInliers 已排序）
  pcl::Indices complement;
  complement.reserve(cloud.size() - inliers1.size());
  std::size_t nextInlier = 0;
  for (std::size_t i = 0; i < cloud.size(); ++i) {
    if (nextInlier < inliers1.size() && static_cast<std::size_t>(inliers1[nextInlier]) == i) {
      ++nextInlier;
    } else {
      complement.push_back(static_cast<int>(i));
    }
  }

  pcl::Indices inliers2;
  Eigen::VectorXf coeff2Raw, coeff2;
  bool hasCoeff2 = false;
  if (complement.size() >= kMinCompanionPoints) {
    // 只在补集里采样，伴线搜索才不会又找回 inliers1
    pcl::SampleConsensusModelLine<Point2DT>::Ptr companionModel(
        new pcl::SampleConsensusModelLine<Point2DT>(cloud.makeShared(), complement, false));
    pcl::RandomSampleConsensus<Point2DT> companionRansac(companionModel);
    companionRansac.setDistanceThreshold(tightThresh);
    companionRansac.setMaxIterations(options.maxIterations);
    companionRansac.computeModel();
    companionRansac.getInliers(inliers2);
    if (inliers2.size() >= 2) {
      companionRansac.getModelCoefficients(coeff2Raw);
      if (options.optimize) {
        modelLine->optimizeModelCoefficients(inliers2, coeff2Raw, coeff2);
      } else {
        coeff2 = coeff2Raw;
      }
      hasCoeff2 = true;
    }
  }

  bool useLine2 = false;
  if (hasCoeff2 &&
      inliers2.size() >= std::max<std::size_t>(
                             kMinCompanionPoints,
                             static_cast<std::size_t>(kCompanionSupportRatio * inliers1.size()))) {
    Eigen::Vector2f d1(coeff1[3], coeff1[4]);
    Eigen::Vector2f d2(coeff2[3], coeff2[4]);
    d1.normalize();
    d2.normalize();
    const double parallelCos = std::cos(kParallelToleranceDeg * kPi / 180.0);
    const bool parallel = std::fabs(static_cast<double>(d1.dot(d2))) >= parallelCos;

    // line1 的法线，朝 +y，所以正的 offset 表示「在上面」
    Eigen::Vector2f n(-d1.y(), d1.x());
    n.normalize();
    if (n.y() < 0) n = -n;
    const Eigen::Vector2f p1(coeff1[0], coeff1[1]);
    const Eigen::Vector2f p2(coeff2[0], coeff2[1]);
    const float offset = n.dot(p2 - p1);

    // 近竖直的线没有「上面那条」可言，这条规则跳过
    const bool dualTrace = parallel && std::fabs(offset) > distThresh &&
                           std::fabs(offset) <= kCompanionOffsetUpperCapFactor * distThresh &&
                           std::fabs(n.y()) > 0.5F;

    if (dualTrace) {
      useLine2 = offset > 0;
    } else {
      useLine2 = inliers2.size() > inliers1.size();
    }
  }

  const pcl::Indices& winnerInliers = useLine2 ? inliers2 : inliers1;
  const Eigen::VectorXf& winnerRaw = useLine2 ? coeff2Raw : coeff1Raw;
  if (options.optimize) {
    modelLine->optimizeModelCoefficients(winnerInliers, winnerRaw, *line);
  } else {
    *line = winnerRaw;
  }
  if (line->size() == 0) return false;
  // 下游（截取、端点、质量指标）要的是精化方向上的配置接受带
  modelLine->selectWithinDistance(*line, distThresh, *inliers);
  return inliers->size() >= 2;
}

bool fitAxisLine2D(const Cloud2D& cloud, Eigen::VectorXf* line, pcl::Indices* inliers,
                   bool vertical, const Line2DFitOptions& options) {
  const int flag = vertical ? 1 : 0;
  bool meetDirection = false;
  Cloud2D::Ptr lineCloud(new Cloud2D);
  pcl::copyPointCloud(cloud, *lineCloud);
  pcl::ExtractIndices<Point2DT> extract;
  pcl::IndicesPtr indicesPtr(new pcl::Indices);
  do {
    extract.setInputCloud(lineCloud);
    extract.setIndices(indicesPtr);
    extract.setNegative(true);
    extract.setKeepOrganized(true);
    extract.filter(*lineCloud);

    if (!fitLine2D(*lineCloud, line, indicesPtr.get(), options)) return false;
    meetDirection = std::fabs((*line)[3 + flag]) > std::fabs((*line)[4 - flag]);
  } while (!meetDirection);
  inliers->assign(indicesPtr->begin(), indicesPtr->end());
  return meetDirection;
}

bool fitCircle2D(const Cloud2D& cloud, Eigen::VectorXf* circle, pcl::Indices* inliers,
                 const Circle2DFitOptions& options, Circle2DFitReport* report) {
  // 下面一步一步照 pcl::SACSegmentation::segment（PCL 1.15）搭：同样的模型、同样的 RANSAC
  // 参数（固定种子、概率 0.99、maxIterations）、同样的 LM 细化与重收内点，所以细化后
  // 半径在界内的输入与原来那一行 seg.segment 逐位相同。拆开是为了接住它扔掉的那种情况。
  inliers->clear();
  if (cloud.size() < 3) return false;
  const double threshold = options.distThresh;
  auto model = std::make_shared<pcl::SampleConsensusModelCircle2D<Point2DT>>(cloud.makeShared(),
                                                                             false);
  model->setRadiusLimits(options.minRadius, options.maxRadius);
  pcl::RandomSampleConsensus<Point2DT> sac(model, threshold);
  sac.setMaxIterations(options.maxIterations);
  if (!sac.computeModel(0)) return false;
  pcl::Indices sampleInliers;
  sac.getInliers(sampleInliers);
  Eigen::VectorXf coeff;
  sac.getModelCoefficients(coeff);
  if (coeff.size() < 3) return false;

  Circle2DFitReport local;
  Circle2DFitReport& out = report != nullptr ? *report : local;
  out = Circle2DFitReport{};

  if (!options.optimizeCoefficients) {
    *inliers = sampleInliers;
    *circle = coeff;
    out.unclampedRadius = coeff[2];
    return inliers->size() >= 3;
  }

  const bool usePrior = options.radiusPriorSigma > 0;
  Eigen::VectorXf refined(3);
  if (usePrior) {
    refined = refineCircleWithPrior(cloud, sampleInliers, coeff, threshold / 2.0,
                                    options.radiusPrior, options.radiusPriorSigma);
    out.radius = Circle2DRadiusOutcome::Prior;
  } else {
    model->optimizeModelCoefficients(sampleInliers, coeff, refined);
  }
  out.unclampedRadius = refined[2];

  // isModelValid 的比较：float 系数提升成 double 再与 double 的界比。
  const double r = refined[2];
  if (refined.allFinite() && r >= options.minRadius && r <= options.maxRadius) {
    model->selectWithinDistance(refined, threshold, *inliers);
    *circle = refined;
    return inliers->size() >= 3;
  }

  // 细化越界（或发散）。SACSegmentation 到这里会拿越界的圆去收内点、收到 0 个而失败；
  // RANSAC 那个圆本来是合格的。把半径钉在越过的界上，只重定圆心，再收一次。
  double bound;
  if (!std::isfinite(r)) {
    bound = coeff[2];
    out.radius = Circle2DRadiusOutcome::Unrefined;
  } else if (r > options.maxRadius) {
    bound = options.maxRadius;
    out.radius = Circle2DRadiusOutcome::ClampedMax;
  } else {
    bound = options.minRadius;
    out.radius = Circle2DRadiusOutcome::ClampedMin;
  }
  double xc = coeff[0];
  double yc = coeff[1];
  if (out.radius != Circle2DRadiusOutcome::Unrefined) {
    refineFixedRadiusCenter(cloud, sampleInliers, bound, &xc, &yc);
  }
  *circle = Eigen::VectorXf(3);
  (*circle)[0] = static_cast<float>(xc);
  (*circle)[1] = static_cast<float>(yc);
  (*circle)[2] = static_cast<float>(bound);
  selectCircleInliers(cloud, *circle, threshold, inliers);
  return inliers->size() >= 3;
}

bool fitCircleFixedRadius2D(const Cloud2D& cloud, double fixedRadius, Eigen::VectorXf* circle,
                            pcl::Indices* inliers, const Circle2DFitOptions& options) {
  if (!fitCircle2D(cloud, circle, inliers, options)) return false;
  if (fixedRadius <= 0) return true;

  double xc = (*circle)[0];
  double yc = (*circle)[1];
  refineFixedRadiusCenter(cloud, *inliers, fixedRadius, &xc, &yc);
  (*circle)[0] = static_cast<float>(xc);
  (*circle)[1] = static_cast<float>(yc);
  (*circle)[2] = static_cast<float>(fixedRadius);

  const Eigen::Vector2f center = {static_cast<float>(xc), static_cast<float>(yc)};
  const float distThresh = options.distThresh;
  auto isOutlier = [&](int i) {
    const Eigen::Vector2f pt = {cloud[static_cast<std::size_t>(i)].x,
                                cloud[static_cast<std::size_t>(i)].y};
    return std::fabs((center - pt).norm() - fixedRadius) > distThresh;
  };
  inliers->erase(std::remove_if(inliers->begin(), inliers->end(), isOutlier), inliers->end());
  return inliers->size() >= 3;
}

}  // namespace lyflow::std_pc
