// 直线拟合与间隙两侧的圆拟合。两者都直接调 detection::utils 里的拟合函数。
#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <set>

#include "gap_detection/GapUtils.hpp"
#include "gap_ops.h"
#include "std_bridge.hpp"
#include "lyflow/kernel/profile_features.h"
#include "profile_circles.h"

namespace lyflow::packs::gap {
namespace {

namespace utils = ::detection::utils;

nlohmann::json numberOrNull(double v) {
  return std::isfinite(v) ? nlohmann::json(v) : nlohmann::json();
}

/// 内点相对圆心的**方位角**（度，圆周均值）。0° = +x，90° = 正上方（测量帧里 y 越小越高），
/// 所以「点云贴在圆的左上半边」≈ 135°。折进 (−180, 180]。
///
/// 这是弧长覆盖看不出来的那一维：arcCoverageDeg 只说内点张开多少度，不说它们落在圆的哪
/// 一侧。同一段点云贴在圆的左上（对）和贴在圆的顶部加底部（歪了），弧长可以一模一样。
/// 用圆周均值而不是算术平均，免得在 ±180° 首尾相接处算出中间那个相反的方向。
double inlierBearingDeg(const GapCloud& cloud, const Eigen::VectorXf& circle,
                        const pcl::Indices& inliers) {
  if (circle.size() < 3 || inliers.empty())
    return std::numeric_limits<double>::quiet_NaN();
  double sumSin = 0;
  double sumCos = 0;
  std::size_t used = 0;
  for (const auto index : inliers) {
    if (index < 0 || static_cast<std::size_t>(index) >= cloud.size())
      continue;
    const double angle = std::atan2(-(cloud[index].y - circle[1]), cloud[index].x - circle[0]);
    sumSin += std::sin(angle);
    sumCos += std::cos(angle);
    ++used;
  }
  // 内点绕圆心均匀分布时合矢量退化成 0，方位角没有意义 —— 报 NaN，调用方当作「不满足」。
  if (used == 0 || std::hypot(sumSin, sumCos) < 1e-9) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return std::atan2(sumSin, sumCos) * 180.0 / M_PI;
}

/// 两个方位角之间的最短夹角（度），恒为 [0, 180]。
double bearingDeltaDeg(double a, double b) {
  const double delta = std::fmod(std::fabs(a - b), 360.0);
  return delta > 180.0 ? 360.0 - delta : delta;
}

nlohmann::json circleQualityJson(const GapCloud& cloud, const Eigen::VectorXf& circle,
                                 const pcl::Indices& inliers, const std::string& model,
                                 const std::string& radiusMode) {
  nlohmann::json q;
  q["model"] = model;
  q["radiusMode"] = radiusMode;
  q["pointCount"] = cloud.size();
  q["inlierCount"] = inliers.size();
  q["inlierRatio"] = cloud.empty()
                         ? nlohmann::json()
                         : nlohmann::json(static_cast<double>(inliers.size()) / cloud.size());
  double radius = std::numeric_limits<double>::quiet_NaN();
  double centerX = std::numeric_limits<double>::quiet_NaN();
  double centerY = std::numeric_limits<double>::quiet_NaN();
  if (circle.size() >= 3) {
    radius = circle[2] * kScale;
    centerX = circle[0] * kScale;
    centerY = circle[1] * kScale;
  }
  q["radiusMm"] = numberOrNull(radius);
  q["centerXMm"] = numberOrNull(centerX);
  q["centerYMm"] = numberOrNull(centerY);

  double rms = std::numeric_limits<double>::quiet_NaN();
  double maximum = std::numeric_limits<double>::quiet_NaN();
  double arcCoverage = std::numeric_limits<double>::quiet_NaN();
  if (circle.size() >= 3 && !inliers.empty()) {
    double squareSum = 0;
    double worst = 0;
    std::vector<double> angles;
    angles.reserve(inliers.size());
    for (const auto index : inliers) {
      if (index < 0 || static_cast<std::size_t>(index) >= cloud.size())
        continue;
      const auto& point = cloud[index];
      const double deltaX = point.x - circle[0];
      const double deltaY = point.y - circle[1];
      const double residual = std::fabs(std::hypot(deltaX, deltaY) - circle[2]) * kScale;
      squareSum += residual * residual;
      worst = std::max(worst, residual);
      double angle = std::atan2(deltaY, deltaX);
      if (angle < 0)
        angle += 2 * M_PI;
      angles.push_back(angle);
    }
    if (!angles.empty()) {
      rms = std::sqrt(squareSum / static_cast<double>(angles.size()));
      maximum = worst;
      std::sort(angles.begin(), angles.end());
      double largestGap = angles.front() + 2 * M_PI - angles.back();
      for (std::size_t i = 1; i < angles.size(); ++i) {
        largestGap = std::max(largestGap, angles[i] - angles[i - 1]);
      }
      arcCoverage = (2 * M_PI - largestGap) * 180.0 / M_PI;
    }
  }
  q["rmsResidualMm"] = numberOrNull(rms);
  q["maxResidualMm"] = numberOrNull(maximum);
  q["arcCoverageDeg"] = numberOrNull(arcCoverage);
  // 内点落在圆的哪一侧。和 arcCoverageDeg 是两回事：那个说张开多少度，这个说朝哪边。
  q["inlierBearingDeg"] = numberOrNull(inlierBearingDeg(cloud, circle, inliers));
  return q;
}

// ---------------------------------------------------------------- 圆拟合

struct SideConfig {
  double rMin = 0;
  double rMax = 0;
  double rFixed = 0;
  gap_std::RadiusPrior prior;
};

struct SideResult {
  Eigen::VectorXf circle;
  pcl::Indices indices;
  GapCloud cloud;
  profile_scene::CircleTraceSide trace;
  std::array<profile_scene::CircleTraceSide, 2> candidateTraces;
  bool fitted = false;
  /// 相机分开拟合且两台都合格：留到最后按 |gap − nominal| 二选一
  bool deferred = false;
  std::array<Eigen::VectorXf, 2> candidates;
  std::array<pcl::Indices, 2> candidateIndices;
  std::array<GapCloud, 2> candidateClouds;
  std::array<lyflow::std_pc::Circle2DFitReport, 2> candidateReports;
  std::string model = "circle";
  /// 走 circleFit2D 那条路时它报回来的半径去向（先验 / 钉在界上）；约束那条路不填。
  lyflow::std_pc::Circle2DFitReport report;
  bool reported = false;
};

/// 内点覆盖的圆弧角度（度）。和 circleQualityJson 里报的是同一个量 —— 那边只是顺手
/// 算出来写进 quality，这里要拿它当判据，所以单独抽一份。
double arcCoverageDeg(const GapCloud& cloud, const Eigen::VectorXf& circle,
                      const pcl::Indices& inliers) {
  if (circle.size() < 3 || inliers.empty())
    return 0.0;
  std::vector<double> angles;
  angles.reserve(inliers.size());
  for (const auto index : inliers) {
    if (index < 0 || static_cast<std::size_t>(index) >= cloud.size())
      continue;
    double angle = std::atan2(cloud[index].y - circle[1], cloud[index].x - circle[0]);
    if (angle < 0)
      angle += 2 * M_PI;
    angles.push_back(angle);
  }
  if (angles.empty())
    return 0.0;
  std::sort(angles.begin(), angles.end());
  double largestGap = angles.front() + 2 * M_PI - angles.back();
  for (std::size_t i = 1; i < angles.size(); ++i) {
    largestGap = std::max(largestGap, angles[i] - angles[i - 1]);
  }
  return (2 * M_PI - largestGap) * 180.0 / M_PI;
}

/// 圆心相对参考线的高度（米）。正 = 圆心在线**上方**（测量帧里 y 越小越高）。
double centerAboveLine(const Eigen::VectorXf& circle, const lyflow::Line2D& line) {
  const double dx = line.dir[0];
  const double dy = line.dir[1];
  if (std::fabs(dx) < 1e-9)
    return std::numeric_limits<double>::quiet_NaN();
  const double at = line.point[1] + (circle[0] - line.point[0]) * (dy / dx);
  return at - circle[1];
}

/// 定半径下的圆心细化：对内点做几步 Gauss-Newton，只动圆心。
Eigen::Vector2d refineCenterFixedRadius(const GapCloud& cloud, const pcl::Indices& idx,
                                        Eigen::Vector2d center, double radius) {
  for (int it = 0; it < 50; ++it) {
    Eigen::Matrix2d jtj = Eigen::Matrix2d::Zero();
    Eigen::Vector2d jtr = Eigen::Vector2d::Zero();
    for (const auto k : idx) {
      const Eigen::Vector2d v(cloud[static_cast<std::size_t>(k)].x - center.x(),
                              cloud[static_cast<std::size_t>(k)].y - center.y());
      const double d = v.norm();
      if (!(d > 0))
        continue;
      const Eigen::Vector2d g = -v / d; // ∂(d − r)/∂c
      jtj += g * g.transpose();
      jtr += g * (d - radius);
    }
    const Eigen::Vector2d step = jtj.ldlt().solve(-jtr);
    if (!step.allFinite())
      break;
    center += step;
    if (step.norm() < 1e-10)
      break;
  }
  return center;
}

/// 带约束的圆拟合：圆心高度带、相切带（都要 line）和内点方位角带，各条都可以单独开。
/// 相切带比的是圆顶（圆心高度 + 半径）：圆角从平面圆滑拐下去时圆顶就压在面线上，
/// 而缝里更深处那段弧上拟出来的圆半径小、圆心高度却差不多，只有圆顶悬在面下面分得开。
/// 共用的 gap_std::circleFit2D 不带这些判据，所以这里自己跑一遍 RANSAC，只在配了约束时走。
/// 采样用固定种子，同样的输入永远给同样的输出（算子声明了 deterministic）。
///
/// `line` 为空表示不查圆心高度与圆顶；`tolM` / `topTolM <= 0` 各自不查；`bearingTolDeg <= 0` 表示不查方位角。
/// `rFixed > 0` 时半径钉死：两点加半径定圆心（每对点两个解），细化也只动圆心。
bool circleFitConstrained(const GapCloud& cloud, const lyflow::Line2D* line, double aboveM,
                          double tolM, double topAboveM, double topTolM, double bearingDeg,
                          double bearingTolDeg, double distThresh, double rMin, double rMax,
                          double rFixed, Eigen::VectorXf* circle, pcl::Indices* inliers,
                          ExecContext* ctx) {
  const std::size_t n = cloud.size();
  if (n < 3)
    return false;
  const bool fixedRadius = rFixed > 0;
  const bool checkCenter = line != nullptr && tolM > 0;
  const bool checkTop = line != nullptr && topTolM > 0;
  const bool checkBearing = bearingTolDeg > 0;
  // 方位角要先有内点才能算，所以它在下面按候选的内点集单独查；这里只查半径、圆心高度与圆顶。
  const auto ok = [&](double cx, double cy, double r) {
    if (!std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(r) ||
        std::fabs(cx) > std::numeric_limits<float>::max() ||
        std::fabs(cy) > std::numeric_limits<float>::max())
      return false;
    if (!(r >= rMin && r <= rMax))
      return false;
    if (!checkCenter && !checkTop)
      return true;
    Eigen::VectorXf probe(3);
    probe << static_cast<float>(cx), static_cast<float>(cy), static_cast<float>(r);
    const double above = centerAboveLine(probe, *line);
    if (!std::isfinite(above))
      return false;
    if (checkCenter && std::fabs(above - aboveM) > tolM)
      return false;
    return !checkTop || std::fabs(above + r - topAboveM) <= topTolM;
  };
  const auto bearingOk = [&](const Eigen::Vector3d& c, const pcl::Indices& idx) {
    if (!checkBearing)
      return true;
    Eigen::VectorXf probe(3);
    probe << static_cast<float>(c[0]), static_cast<float>(c[1]), static_cast<float>(c[2]);
    const double bearing = inlierBearingDeg(cloud, probe, idx);
    return std::isfinite(bearing) && bearingDeltaDeg(bearing, bearingDeg) <= bearingTolDeg;
  };
  std::mt19937 rng(20260916U);
  std::uniform_int_distribution<std::size_t> pick(0, n - 1);
  std::size_t bestCount = 0;
  Eigen::Vector3d best(0, 0, 0);
  const auto consider = [&](double ux, double uy, double r) {
    if (!ok(ux, uy, r))
      return;
    std::size_t count = 0;
    pcl::Indices candidate;
    for (std::size_t q = 0; q < n; ++q) {
      if (std::fabs(std::hypot(cloud[q].x - ux, cloud[q].y - uy) - r) < distThresh) {
        ++count;
        if (checkBearing)
          candidate.push_back(static_cast<int>(q));
      }
    }
    if (count <= bestCount)
      return;
    if (!bearingOk(Eigen::Vector3d(ux, uy, r), candidate))
      return;
    bestCount = count;
    best = Eigen::Vector3d(ux, uy, r);
  };
  // 迭代数与 PCL 那条路（Circle2DFitOptions::maxIterations）取齐，免得「加了约束反而
  // 更容易拟出来」只是因为采样次数不同。
  for (int iter = 0; iter < 10000; ++iter) {
    if (ctx && ctx->cancelled())
      return false;
    const std::size_t a = pick(rng);
    const std::size_t b = pick(rng);
    if (a == b)
      continue;
    const double ax = cloud[a].x, ay = cloud[a].y;
    const double bx = cloud[b].x, by = cloud[b].y;
    if (fixedRadius) {
      const double half = 0.5 * std::hypot(bx - ax, by - ay);
      if (!(half > 1e-12) || half > rFixed)
        continue;
      const double h = std::sqrt(rFixed * rFixed - half * half);
      const double mx = 0.5 * (ax + bx), my = 0.5 * (ay + by);
      const double nx = -(by - ay) / (2 * half), ny = (bx - ax) / (2 * half);
      consider(mx + h * nx, my + h * ny, rFixed);
      consider(mx - h * nx, my - h * ny, rFixed);
      continue;
    }
    const std::size_t c = pick(rng);
    if (b == c || a == c)
      continue;
    const double cx = cloud[c].x, cy = cloud[c].y;
    const double d = 2.0 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));
    if (std::fabs(d) < 1e-15)
      continue;
    const double ux = ((ax * ax + ay * ay) * (by - cy) + (bx * bx + by * by) * (cy - ay) +
                       (cx * cx + cy * cy) * (ay - by)) /
                      d;
    const double uy = ((ax * ax + ay * ay) * (cx - bx) + (bx * bx + by * by) * (ax - cx) +
                       (cx * cx + cy * cy) * (bx - ax)) /
                      d;
    consider(ux, uy, std::hypot(ax - ux, ay - uy));
  }
  if (bestCount < 3)
    return false;
  // 用内点做一次代数重拟；重拟后仍要满足约束，否则退回粗解。
  pcl::Indices keep;
  for (std::size_t i = 0; i < n; ++i) {
    if (std::fabs(std::hypot(cloud[i].x - best[0], cloud[i].y - best[1]) - best[2]) < distThresh) {
      keep.push_back(static_cast<int>(i));
    }
  }
  Eigen::Vector3d refined = best;
  if (keep.size() >= 3 && fixedRadius) {
    const Eigen::Vector2d c =
        refineCenterFixedRadius(cloud, keep, Eigen::Vector2d(best[0], best[1]), rFixed);
    const Eigen::Vector3d candidate(c.x(), c.y(), rFixed);
    if (c.allFinite() && ok(c.x(), c.y(), rFixed) && bearingOk(candidate, keep)) {
      refined = candidate;
    }
  } else if (keep.size() >= 3) {
    Eigen::MatrixXd A(keep.size(), 3);
    Eigen::VectorXd rhs(keep.size());
    for (std::size_t k = 0; k < keep.size(); ++k) {
      const auto& q = cloud[static_cast<std::size_t>(keep[k])];
      A(static_cast<Eigen::Index>(k), 0) = 2.0 * q.x;
      A(static_cast<Eigen::Index>(k), 1) = 2.0 * q.y;
      A(static_cast<Eigen::Index>(k), 2) = 1.0;
      rhs(static_cast<Eigen::Index>(k)) =
          static_cast<double>(q.x) * q.x + static_cast<double>(q.y) * q.y;
    }
    const Eigen::Vector3d sol = A.colPivHouseholderQr().solve(rhs);
    const double rr = std::sqrt(std::max(sol[2] + sol[0] * sol[0] + sol[1] * sol[1], 0.0));
    // 重拟后两条约束都要仍然成立，否则退回粗解。
    const Eigen::Vector3d candidate(sol[0], sol[1], rr);
    if (ok(sol[0], sol[1], rr) && bearingOk(candidate, keep))
      refined = candidate;
  }
  circle->resize(3);
  (*circle)[0] = static_cast<float>(refined[0]);
  (*circle)[1] = static_cast<float>(refined[1]);
  (*circle)[2] = static_cast<float>(refined[2]);
  inliers->clear();
  for (std::size_t i = 0; i < n; ++i) {
    if (std::fabs(std::hypot(cloud[i].x - refined[0], cloud[i].y - refined[1]) - refined[2]) <
        distThresh) {
      inliers->push_back(static_cast<int>(i));
    }
  }
  return inliers->size() >= 3;
}

GapCloud cropStrict(const GapCloud& src, const Eigen::Matrix2f& roi,
                    std::vector<std::int32_t>* slots = nullptr) {
  GapCloud out;
  gap_std::roiCrop2D(src, &out, roi);
  if (slots) {
    slots->clear();
    const Eigen::Vector2f lo = roi.col(0), hi = roi.col(1);
    for (std::size_t i = 0; i < src.size(); ++i)
      if (lyflow::std_pc::insideBox2D(src[i].x, src[i].y, lo, hi, lyflow::std_pc::Bounds2D::Open))
        slots->push_back(static_cast<std::int32_t>(i));
  }
  return out;
}

} // namespace

namespace profile_scene::detail {
Status fitCirclePairClouds(const Inputs& inputs, const ParamView& params, Outputs& destination,
                           ExecContext& ctx, CirclePairTrace* trace) {
  std::unordered_map<std::string, Data> pending;
  Outputs outputs(pending);
  const auto cancelled = [&] {
    return Status::Error(Phase::Execute, "cancelled", "间隙圆拟合已取消");
  };
  if (ctx.cancelled())
    return cancelled();

  const lyflow::PointCloud& mergedIn = *inputs.get("merged").asCloud();
  const GapCloud merged = toPcl(mergedIn);
  const GapCloud primary = toPcl(*inputs.get("primary").asCloud());
  const GapCloud secondary = toPcl(*inputs.get("secondary").asCloud());
  const lyflow::Box2D boxes[2] = {*inputs.get("boxLeft").asBox2D(),
                                  *inputs.get("boxRight").asBox2D()};

  const float distThresh = mmToM(params.number("distThresh"));
  const double retryDistanceMm = params.number("retryDistance");
  const bool fallback = params.flag("cameraFallback");
  const bool selectClosestNominal = params.flag("selectClosestNominal");
  const std::string preferred = params.choice("preferredCamera");
  const double nominal = params.number("nominal");
  const double offset = params.number("offset");

  SideConfig cfg[2];
  cfg[0] = {params.number("leftRadiusMin") / kScale, params.number("leftRadiusMax") / kScale,
            params.flag("leftRadiusFixed") ? params.number("leftRadiusValue") / kScale : 0.0};
  cfg[1] = {params.number("rightRadiusMin") / kScale, params.number("rightRadiusMax") / kScale,
            params.flag("rightRadiusFixed") ? params.number("rightRadiusValue") / kScale : 0.0};
  // 半径软先验：sigma <= 0 不加。作用于 circleFit2D 那条路：不带圆心 / 方位带的一侧，
  // 以及 guard 档先拟的那一次；always 档与 guard 出带后重来的约束那条路自己跑 RANSAC，不吃先验。
  cfg[0].prior = {params.number("leftRadiusPrior") / kScale,
                  params.number("leftRadiusPriorSigma") / kScale};
  cfg[1].prior = {params.number("rightRadiusPrior") / kScale,
                  params.number("rightRadiusPriorSigma") / kScale};

  // 逐侧选相机。默认 Both = 用合并云。两台锁在不同界面上时（夹胶玻璃），
  // 合并云里是相距一两毫米的两层点，拟出来的圆没有意义 —— 那种点位把这一侧钉到一台上。
  const std::string sideCamera[2] = {params.choice("leftCamera"), params.choice("rightCamera")};
  // 圆心高度带：tol <= 0 表示不启用，那时走 circleFit2D。
  const double centerAbove[2] = {params.number("leftCenterAbove") / kScale,
                                 params.number("rightCenterAbove") / kScale};
  const double centerTol[2] = {params.number("leftCenterTol") / kScale,
                               params.number("rightCenterTol") / kScale};
  const std::string centerMode[2] = {params.choice("leftCenterMode"),
                                     params.choice("rightCenterMode")};
  // 相切带：圆顶（圆心高度 + 半径）相对 refLine 的高度。tol <= 0 表示不启用。档位跟 *CenterMode 走。
  const double tangentAbove[2] = {params.number("leftTangentAbove") / kScale,
                                  params.number("rightTangentAbove") / kScale};
  const double tangentTol[2] = {params.number("leftTangentTol") / kScale,
                                params.number("rightTangentTol") / kScale};
  // 弱拟合的地板。圆拟合本身只要 3 个内点就算成功，短弧上拟出来的圆心可以跑到点云外面去，
  // 而残差照样很小 —— 这两个量是唯一看得出来的。
  const std::size_t minInliers[2] = {static_cast<std::size_t>(params.integer("leftMinInliers")),
                                     static_cast<std::size_t>(params.integer("rightMinInliers"))};
  const double minArc[2] = {params.number("leftMinArcDeg"), params.number("rightMinArcDeg")};
  // 内点方位角带：点云应当贴在圆的哪一侧。tol <= 0 = 不检查。不需要 refLine。
  const double bearing[2] = {params.number("leftArcBearingDeg"),
                             params.number("rightArcBearingDeg")};
  const double bearingTol[2] = {params.number("leftArcBearingTolDeg"),
                                params.number("rightArcBearingTolDeg")};
  const lyflow::Line2D* refLine =
      inputs.has("refLine") ? inputs.get("refLine").asLine2D() : nullptr;
  const lyflow::Line2D* refLineRight =
      inputs.has("refLineRight") ? inputs.get("refLineRight").asLine2D() : nullptr;
  // 两侧各比各的线。右侧不接 refLineRight 就退回 refLine。缝两侧贴的是不同的面时
  // （左圆贴基准面、右圆贴玻璃面）才需要分开，同一条线也能服务两侧。
  // 配了高度带却没接线的组合 validate 已经挡过。
  const lyflow::Line2D* sideRefLine[2] = {refLine,
                                          refLineRight != nullptr ? refLineRight : refLine};

  constexpr std::size_t kMinimumCameraInliers = 8;
  const double maxRadiusDifference = 0.25 / kScale;
  const double maxCenterDifference = 0.75 / kScale;

  // 一侧的拟合：guard 先按原样拟、只有圆心落到带外才换约束那条路（带内的帧结果与不加带
  // 相同，所以给「本来就拟得对」的点位挂一条宽带纯属保险）；always 一律走约束。
  // 抽成 lambda 是为了「弱了就换合并云再来一次」能原样重跑。
  const auto fitOneSide = [&](SideResult& side, int i) {
    if (ctx.cancelled())
      return false;
    const bool constrained = centerTol[i] > 0 || tangentTol[i] > 0 || bearingTol[i] > 0;
    const bool guardOnly = constrained && centerMode[i] == "guard";
    if (!constrained || guardOnly) {
      side.fitted =
          gap_std::circleFit2D(side.cloud, &side.circle, &side.indices, distThresh, cfg[i].rMin,
                               cfg[i].rMax, cfg[i].rFixed, cfg[i].prior, &side.report);
      if (ctx.cancelled())
        return false;
      if (!side.fitted && retryDistanceMm > 0) {
        side.indices.clear();
        side.fitted = gap_std::circleFit2D(side.cloud, &side.circle, &side.indices,
                                           mmToM(retryDistanceMm), cfg[i].rMin, cfg[i].rMax,
                                           cfg[i].rFixed, cfg[i].prior, &side.report);
      }
      side.reported = true;
    }
    bool needBand = constrained && !guardOnly;
    if (guardOnly) {
      // 任何一条约束落空都要重来。
      needBand = !side.fitted;
      if (!needBand && centerTol[i] > 0) {
        const double above = centerAboveLine(side.circle, *sideRefLine[i]);
        needBand = !std::isfinite(above) || std::fabs(above - centerAbove[i]) > centerTol[i];
      }
      if (!needBand && tangentTol[i] > 0) {
        const double top = centerAboveLine(side.circle, *sideRefLine[i]) + side.circle[2];
        needBand = !std::isfinite(top) || std::fabs(top - tangentAbove[i]) > tangentTol[i];
      }
      if (!needBand && bearingTol[i] > 0) {
        const double b = inlierBearingDeg(side.cloud, side.circle, side.indices);
        needBand = !std::isfinite(b) || bearingDeltaDeg(b, bearing[i]) > bearingTol[i];
      }
    }
    if (needBand) {
      side.indices.clear();
      side.reported = false;
      side.fitted = circleFitConstrained(side.cloud, sideRefLine[i], centerAbove[i], centerTol[i],
                                         tangentAbove[i], tangentTol[i], bearing[i], bearingTol[i],
                                         distThresh, cfg[i].rMin, cfg[i].rMax, cfg[i].rFixed,
                                         &side.circle, &side.indices, &ctx);
      if (ctx.cancelled())
        return false;
      if (!side.fitted && retryDistanceMm > 0) {
        side.indices.clear();
        side.fitted = circleFitConstrained(
            side.cloud, sideRefLine[i], centerAbove[i], centerTol[i], tangentAbove[i],
            tangentTol[i], bearing[i], bearingTol[i], mmToM(retryDistanceMm), cfg[i].rMin,
            cfg[i].rMax, cfg[i].rFixed, &side.circle, &side.indices, &ctx);
      }
      if (side.fitted) {
        std::string tag = "circle";
        if (centerTol[i] > 0)
          tag += "-center";
        if (tangentTol[i] > 0)
          tag += "-tangent";
        if (bearingTol[i] > 0)
          tag += "-bearing";
        side.model = tag + "-band";
      }
    }
    return side.fitted;
  };

  SideResult sides[2];
  for (int i = 0; i < 2; ++i) {
    SideResult& side = sides[i];
    const Eigen::Matrix2f roi = toRoiMatrix(boxes[i]);
    const GapCloud& source = sideCamera[i] == "Primary"     ? primary
                             : sideCamera[i] == "Secondary" ? secondary
                                                            : merged;
    side.trace.source = sideCamera[i] == "Primary"     ? CircleSource::Primary
                        : sideCamera[i] == "Secondary" ? CircleSource::Secondary
                                                       : CircleSource::Merged;
    side.cloud = cropStrict(source, roi, trace ? &side.trace.slots : nullptr);
    if (side.cloud.empty()) {
      return Status::Error(Phase::Execute, "roi_empty",
                           std::string(i == 0 ? "左" : "右") + "间隙 ROI 里没有点", {},
                           i == 0 ? "boxLeft" : "boxRight");
    }
    fitOneSide(side, i);
    if (ctx.cancelled())
      return cancelled();
    // 弱就重来：钉死单相机时先退回合并云（那台被挡住的时候另一台往往是好的），
    // 合并云也弱就当没拟出来，交给下面既有的回退，最后报失败 —— 宁可没有也别给个错的。
    const auto weak = [&](const SideResult& s2) {
      if (!s2.fitted)
        return true;
      if (minInliers[i] > 0 && s2.indices.size() < minInliers[i])
        return true;
      return minArc[i] > 0 && arcCoverageDeg(s2.cloud, s2.circle, s2.indices) < minArc[i];
    };
    if (weak(side) && sideCamera[i] != "Both") {
      SideResult retry;
      retry.trace.source = CircleSource::Merged;
      retry.cloud = cropStrict(merged, roi, trace ? &retry.trace.slots : nullptr);
      retry.fitted = fitOneSide(retry, i);
      if (ctx.cancelled())
        return cancelled();
      if (!weak(retry)) {
        retry.model += "-merged-retry";
        side = std::move(retry);
      }
    }
    if (weak(side)) {
      side.fitted = false;
      side.indices.clear();
    }

    if (side.fitted || !fallback || sideCamera[i] != "Both")
      continue;

    // 相机分开拟合的回退：两台相机各自的 ROI 点各拟合一个圆，固定半径照样生效。
    CircleTraceSide primaryTrace{CircleSource::Primary, {}},
        secondaryTrace{CircleSource::Secondary, {}};
    GapCloud primaryRoi = cropStrict(primary, roi, trace ? &primaryTrace.slots : nullptr);
    GapCloud secondaryRoi = cropStrict(secondary, roi, trace ? &secondaryTrace.slots : nullptr);
    const double separatedFixed = cfg[i].rFixed;
    Eigen::VectorXf primaryCircle, secondaryCircle;
    pcl::Indices primaryIndices, secondaryIndices;
    lyflow::std_pc::Circle2DFitReport primaryReport, secondaryReport;
    const bool primaryOk =
        gap_std::circleFit2D(primaryRoi, &primaryCircle, &primaryIndices, distThresh, cfg[i].rMin,
                             cfg[i].rMax, separatedFixed, cfg[i].prior, &primaryReport);
    if (ctx.cancelled())
      return cancelled();
    const bool secondaryOk = gap_std::circleFit2D(secondaryRoi, &secondaryCircle, &secondaryIndices,
                                                  distThresh, cfg[i].rMin, cfg[i].rMax,
                                                  separatedFixed, cfg[i].prior, &secondaryReport);
    if (ctx.cancelled())
      return cancelled();
    const bool primaryEligible = primaryOk && primaryIndices.size() >= kMinimumCameraInliers;
    const bool secondaryEligible = secondaryOk && secondaryIndices.size() >= kMinimumCameraInliers;
    const bool consistent =
        primaryOk && secondaryOk && primaryEligible && secondaryEligible &&
        std::fabs(primaryCircle[2] - secondaryCircle[2]) <= maxRadiusDifference &&
        (primaryCircle.head<2>() - secondaryCircle.head<2>()).norm() <= maxCenterDifference;

    const auto take = [&](bool takePrimary) {
      side.cloud = takePrimary ? primaryRoi : secondaryRoi;
      side.trace = takePrimary ? primaryTrace : secondaryTrace;
      side.circle = takePrimary ? primaryCircle : secondaryCircle;
      side.indices = takePrimary ? primaryIndices : secondaryIndices;
      side.fitted = true;
      side.model =
          takePrimary ? "camera-separated-circle-primary" : "camera-separated-circle-secondary";
      side.report = takePrimary ? primaryReport : secondaryReport;
      side.reported = true;
    };
    if (selectClosestNominal && primaryEligible && secondaryEligible) {
      side.deferred = true;
      side.candidates = {primaryCircle, secondaryCircle};
      side.candidateIndices = {primaryIndices, secondaryIndices};
      side.candidateClouds = {primaryRoi, secondaryRoi};
      side.candidateTraces = {primaryTrace, secondaryTrace};
      side.candidateReports = {primaryReport, secondaryReport};
      take(true);
    } else if (selectClosestNominal && primaryEligible) {
      take(true);
    } else if (selectClosestNominal && secondaryEligible) {
      take(false);
    } else if (preferred == "Left" && primaryEligible) {
      take(true);
    } else if (preferred == "Right" && secondaryEligible) {
      take(false);
    } else if (consistent && primaryIndices.size() >= secondaryIndices.size()) {
      take(true);
    } else if (consistent) {
      take(false);
    }
    if (side.fitted) {
      ctx.log(LogLevel::Info,
              std::string(i == 0 ? "gap_left" : "gap_right") + " 走了相机分开拟合的回退");
    }
  }

  for (int i = 0; i < 2; ++i) {
    if (!sides[i].fitted) {
      return Status::Error(Phase::Execute, "circle_fit_failed",
                           std::string(i == 0 ? "gap_left" : "gap_right") + " 圆拟合失败", {},
                           i == 0 ? "boxLeft" : "boxRight");
    }
  }

  // 两侧都可能有两个候选：按 |gap − nominal| 最小挑一组，平局取内点多的（§3.9）。
  if (sides[0].deferred || sides[1].deferred) {
    const int leftCount = sides[0].deferred ? 2 : 1;
    const int rightCount = sides[1].deferred ? 2 : 1;
    double bestDelta = std::numeric_limits<double>::infinity();
    std::size_t bestInliers = 0;
    int bestLeft = 0;
    int bestRight = 0;
    bool found = false;
    for (int l = 0; l < leftCount; ++l) {
      const Eigen::VectorXf& c1 = sides[0].deferred ? sides[0].candidates[l] : sides[0].circle;
      for (int rr = 0; rr < rightCount; ++rr) {
        const Eigen::VectorXf& c2 = sides[1].deferred ? sides[1].candidates[rr] : sides[1].circle;
        Eigen::Vector2f start, end;
        // 候选打分用 definition B 的圆心距：挑的是哪台相机，与下游按哪种定义出值无关
        const double d = utils::circleCircleDistance(c1, c2, &start, &end);
        const double gapMm = std::fabs(d * kScale) + offset;
        if (!std::isfinite(gapMm) || start.x() > end.x())
          continue;
        const double delta = std::fabs(gapMm - nominal);
        const std::size_t inliers = (sides[0].deferred ? sides[0].candidateIndices[l].size() : 0) +
                                    (sides[1].deferred ? sides[1].candidateIndices[rr].size() : 0);
        if (delta < bestDelta - 1e-9 ||
            (std::fabs(delta - bestDelta) <= 1e-9 && inliers > bestInliers)) {
          found = true;
          bestDelta = delta;
          bestInliers = inliers;
          bestLeft = l;
          bestRight = rr;
        }
      }
    }
    if (!found) {
      return Status::Error(Phase::Execute, "circle_fit_failed", "按标称值挑相机候选时全部无效");
    }
    const int picked[2] = {bestLeft, bestRight};
    for (int i = 0; i < 2; ++i) {
      if (!sides[i].deferred)
        continue;
      sides[i].circle = sides[i].candidates[picked[i]];
      sides[i].indices = sides[i].candidateIndices[picked[i]];
      sides[i].cloud = sides[i].candidateClouds[picked[i]];
      sides[i].trace = sides[i].candidateTraces[picked[i]];
      sides[i].report = sides[i].candidateReports[picked[i]];
      sides[i].model = std::string("camera-separated-circle-") +
                       (picked[i] == 0 ? "primary" : "secondary") + "-closest-gap-nominal";
    }
  }

  if (ctx.cancelled())
    return cancelled();
  for (int i = 0; i < 2; ++i)
    if (sides[i].circle.size() < 3 || !sides[i].circle.allFinite() || sides[i].circle[2] <= 0)
      return Status::Error(Phase::Execute, "invalid_geometry", "圆拟合返回非法系数");
  static const char* kCircle[2] = {"left", "right"};
  static const char* kInliers[2] = {"leftInliers", "rightInliers"};
  static const char* kClouds[2] = {"leftCloud", "rightCloud"};
  for (int i = 0; i < 2; ++i) {
    lyflow::Circle2D circle;
    circle.center[0] = sides[i].circle[0];
    circle.center[1] = sides[i].circle[1];
    circle.radius = sides[i].circle[2];
    outputs.set(kCircle[i], Data::circle2d(circle));

    lyflow::PointCloud fitted = fromPcl(sides[i].cloud);
    lyflow::Indices idx;
    idx.sourceCloudId = fitted.id;
    idx.values.reserve(sides[i].indices.size());
    for (auto v : sides[i].indices)
      idx.values.push_back(static_cast<std::int32_t>(v));
    outputs.set(kClouds[i], Data::cloud(std::move(fitted)));
    outputs.set(kInliers[i], Data::indices(std::move(idx)));
  }

  lyflow::Record quality;
  quality.type = "GapFitQualityPair";
  static const char* kSides[2] = {"left", "right"};
  for (int i = 0; i < 2; ++i) {
    const bool isFixed = cfg[i].rFixed > 0;
    std::string radiusMode =
        sides[i].circle.size() >= 3
            ? utils::classifyRadiusMode(sides[i].circle[2], cfg[i].rMin, cfg[i].rMax, isFixed)
            : std::string();
    using lyflow::std_pc::Circle2DRadiusOutcome;
    const Circle2DRadiusOutcome outcome = sides[i].report.radius;
    if (sides[i].reported && radiusMode == "free" && outcome == Circle2DRadiusOutcome::Prior) {
      radiusMode = "prior";
    }
    quality.data[kSides[i]] = circleQualityJson(sides[i].cloud, sides[i].circle, sides[i].indices,
                                                sides[i].model, radiusMode);
    // 细化把半径推出界、被钉回界上的那一下：记下钉之前的半径，复核时看得出它想去哪儿。
    if (sides[i].reported && (outcome == Circle2DRadiusOutcome::ClampedMin ||
                              outcome == Circle2DRadiusOutcome::ClampedMax)) {
      quality.data[kSides[i]]["radiusBeforeClampMm"] = sides[i].report.unclampedRadius * kScale;
    }
    // 接了参考线就报圆心与圆顶相对它的高度：调圆心高度带、相切带时直接拿这两个量看分布。
    if (sideRefLine[i] != nullptr && sides[i].circle.size() >= 3) {
      const double above = centerAboveLine(sides[i].circle, *sideRefLine[i]);
      if (std::isfinite(above)) {
        quality.data[kSides[i]]["centerAboveRefMm"] = above * kScale;
        quality.data[kSides[i]]["topAboveRefMm"] = (above + sides[i].circle[2]) * kScale;
      }
    }
  }
  outputs.set("quality", Data::record(std::move(quality)));
  if (ctx.cancelled())
    return cancelled();
  CirclePairTrace candidateTrace;
  if (trace)
    for (int i = 0; i < 2; ++i)
      candidateTrace.sides[i] = std::move(sides[i].trace);
  for (auto& entry : pending)
    destination.set(entry.first, std::move(entry.second));
  if (trace)
    *trace = std::move(candidateTrace);
  return Status::Ok();
}
}
namespace fine {

std::vector<Issue> validateFitGapCircles(const ParamView& params,
                                         const std::set<std::string>& connected) {
  std::vector<Issue> issues;
  const bool refLeft = connected.count("refLine") != 0;
  const bool refRight = refLeft || connected.count("refLineRight") != 0;
  if (params.number("leftCenterTol") > 0 && !refLeft) {
    issues.push_back(Issue::error("bad_param", "配了左侧圆心高度带就必须接 refLine（参考线）",
                                  "leftCenterTol", "refLine"));
  }
  if (params.number("rightCenterTol") > 0 && !refRight) {
    issues.push_back(Issue::error("bad_param",
                                  "配了右侧圆心高度带就必须接 refLineRight 或 refLine（参考线）",
                                  "rightCenterTol", "refLineRight"));
  }
  if (params.number("leftTangentTol") > 0 && !refLeft) {
    issues.push_back(Issue::error("bad_param", "配了左侧相切带就必须接 refLine（参考线）",
                                  "leftTangentTol", "refLine"));
  }
  if (params.number("rightTangentTol") > 0 && !refRight) {
    issues.push_back(Issue::error("bad_param",
                                  "配了右侧相切带就必须接 refLineRight 或 refLine（参考线）",
                                  "rightTangentTol", "refLineRight"));
  }
  for (const char* side : {"left", "right"}) {
    const std::string s(side);
    const double rMin = params.number(s + "RadiusMin");
    const double rMax = params.number(s + "RadiusMax");
    if (!(rMax > rMin)) {
      issues.push_back(Issue::error("bad_param", "半径上限必须大于下限", s + "RadiusMax"));
      continue;
    }
    if (params.flag(s + "RadiusFixed")) {
      const double r = params.number(s + "RadiusValue");
      if (!(r > rMin && r < rMax)) {
        issues.push_back(
            Issue::error("bad_param", "固定半径必须落在上下限之间", s + "RadiusValue"));
      }
    }
    if (params.number(s + "RadiusPriorSigma") > 0) {
      const double prior = params.number(s + "RadiusPrior");
      if (!(prior >= rMin && prior <= rMax)) {
        issues.push_back(
            Issue::error("bad_param", "半径先验必须落在上下限之间", s + "RadiusPrior"));
      }
      if (params.flag(s + "RadiusFixed")) {
        issues.push_back(
            Issue::warning("bad_param", "半径已经钉死，先验不起作用", s + "RadiusPriorSigma"));
      }
      // guard 档先按 circleFit2D（带先验）拟，在带内就留下这个结果，先验是生效的；
      // 只有 always 档一律走带约束的拟合，先验才真的被绕过。
      const bool constrained = params.number(s + "CenterTol") > 0 ||
                               params.number(s + "TangentTol") > 0 ||
                               params.number(s + "ArcBearingTolDeg") > 0;
      if (constrained && params.choice(s + "CenterMode") != "guard") {
        issues.push_back(Issue::warning(
            "bad_param",
            "配了圆心高度带、相切带或方位角带（always 档）的一侧走带约束的拟合，不吃半径先验",
            s + "RadiusPriorSigma"));
      }
    }
  }
  return issues;
}

} // namespace fine

namespace {

Param numParam(const char* name, const char* label, double def, const char* unit, const char* group,
               const char* doc) {
  Param p;
  p.name = name;
  p.type = ParamType::Float;
  p.label = label;
  p.doc = doc;
  p.def = Value::number(def);
  p.unit = unit;
  p.group = group;
  return p;
}

Param boolParam(const char* name, const char* label, bool def, const char* group, const char* doc) {
  Param p;
  p.name = name;
  p.type = ParamType::Bool;
  p.label = label;
  p.doc = doc;
  p.def = Value::boolean(def);
  p.group = group;
  return p;
}

Param minInliersParam(const char* name, const char* label, const char* group) {
  Param p;
  p.name = name;
  p.type = ParamType::Int;
  p.label = label;
  p.doc = "这一侧内点少于它就算没拟出来。0 = 不检查（默认）。圆拟合本身 3 个内点就算成功，"
          "而短弧上拟出来的圆心能跑到点云外面去、残差照样很小 —— 这是看得出来的量之一。";
  p.def = Value::integer(0);
  p.advanced = true;
  p.group = group;
  return p;
}

Param minArcParam(const char* name, const char* label, const char* group) {
  Param p;
  p.name = name;
  p.type = ParamType::Float;
  p.label = label;
  p.doc = "内点覆盖的圆弧角度小于它就算没拟出来。0 = 不检查（默认）。比内点数更直接："
          "弧太短时三个参数的圆本来就定不住，圆心往哪边跑全看噪声。";
  p.def = Value::number(0.0);
  p.unit = "°";
  p.advanced = true;
  p.group = group;
  return p;
}

Param bearingParam(const char* name, const char* label, const char* group) {
  Param p;
  p.name = name;
  p.type = ParamType::Float;
  p.label = label;
  p.doc = "内点应当落在圆的哪一侧，用相对圆心的方位角表示：0° = 正右，90° = 正上"
          "（测量帧里 y 越小越高），135° = 左上。配合同侧的 ArcBearingTolDeg 使用。";
  p.def = Value::number(0.0);
  p.unit = "°";
  p.advanced = true;
  p.group = group;
  return p;
}

Param bearingTolParam(const char* name, const char* label, const char* group) {
  Param p;
  p.name = name;
  p.type = ParamType::Float;
  p.label = label;
  p.doc = "方位角的容差，<= 0 表示不加这个约束（默认）。弧长覆盖只说内点张开多少度，不说它们"
          "落在圆的哪一侧 —— 点云贴在圆的左上（对）和贴在顶部加底部（歪了）弧长可以一模一样，"
          "这一条是唯一分得开的。";
  p.def = Value::number(0.0);
  p.unit = "°";
  p.advanced = true;
  p.group = group;
  return p;
}

Param centerModeParam(const char* name, const char* label, const char* group) {
  Param p;
  p.name = name;
  p.type = ParamType::Enum;
  p.label = label;
  p.doc =
      "圆心高度带（以及同侧的相切带、方位角带）怎么用。always 一律走带约束的拟合；guard 先按原样拟，"
      "只有落到带外才重来 —— 带内的帧结果与不加带相同，所以「本来就拟得对、只是想上个保险」的点位该用 guard。";
  p.def = Value::text("always");
  p.group = group;
  p.advanced = true;
  p.options = {EnumOption{"always", "Always", "一律走带约束的拟合。"},
               EnumOption{"guard", "Guard", "只在出带时才重拟，带内结果与不加带相同。"}};
  return p;
}

Param sideCameraParam(const char* name, const char* label, const char* group) {
  Param p;
  p.name = name;
  p.type = ParamType::Enum;
  p.label = label;
  p.doc = "这一侧的圆用哪台相机的点。Both = 合并云（默认）。两台锁在不同界面上时（例如夹胶"
          "玻璃，一台看表面一台看夹胶层），合并云里是相距一两毫米的两层点，拟出来的圆没有"
          "意义 —— 那种点位把这一侧钉到一台上。钉死之后这一侧不再走相机分开的回退；"
          "那台被遮挡、拟得太弱（minInliers / minArcDeg）时自动退回合并云重拟一次。";
  p.def = Value::text("Both");
  p.group = group;
  p.advanced = true;
  p.options = {EnumOption{"Both", "Both（合并云）", ""},
               EnumOption{"Primary", "Primary（Master）", ""},
               EnumOption{"Secondary", "Secondary（Slave）", ""}};
  return p;
}

} // namespace

namespace profile_scene {
std::vector<Param> circlePairParams() {
  Param preferred;
  preferred.name = "preferredCamera";
  preferred.type = ParamType::Enum;
  preferred.label = "Preferred Camera";
  preferred.def = Value::text("Both");
  preferred.group = "Camera Fallback";
  preferred.options = {EnumOption{"Both", "Both", ""}, EnumOption{"Left", "Left（primary）", ""},
                       EnumOption{"Right", "Right（secondary）", ""}};

  return {
      numParam("distThresh", "Dist Thresh", 0.03, "mm", "", "圆内点判定距离。"),
      numParam("retryDistance", "Retry Distance", 0.0, "mm", "",
               "第一次失败后用它再试一次。0 = 不重试。"),
      numParam("nominal", "Nominal", 0.0, "mm", "",
               "间隙标称值，挑相机候选时用。填错会稳定地挑错一侧的圆。"),
      numParam("offset", "Offset", 0.0, "mm", "", "间隙偏置，挑相机候选时要算进去。"),
      boolParam("leftRadiusFixed", "Left Fixed", false, "Left Radius", "固定左圆半径。"),
      numParam("leftRadiusValue", "Left Radius", 1.0, "mm", "Left Radius", "固定的左圆半径。"),
      numParam("leftRadiusMin", "Left Min", 0.5, "mm", "Left Radius", "左圆半径下限。"),
      numParam("leftRadiusMax", "Left Max", 2.0, "mm", "Left Radius", "左圆半径上限。"),
      numParam(
          "leftRadiusPrior", "Left Prior", 0.0, "mm", "Left Radius",
          "左圆半径的软先验（典型半径），配合 leftRadiusPriorSigma 使用。量一批弧够长的正常帧，"
          "取半径中位数。"),
      numParam("leftRadiusPriorSigma", "Left Prior Sigma", 0.0, "mm", "Left Radius",
               "先验的宽度，<= 0 表示不加（默认）。细化的目标函数多一项 ((r − prior) / sigma)²，"
               "点残差按 distThresh / 2 归一：弧长时听点的，弧短时半径往先验收，结果连续，"
               "不再贴在上下限上。上下限仍然是硬兜底 —— 加先验时放宽到先验 ± 4 sigma 左右。"
               "太窄等于把半径钉死（钉错会把误差逼到圆心上）。"),
      boolParam("rightRadiusFixed", "Right Fixed", false, "Right Radius", "固定右圆半径。"),
      numParam("rightRadiusValue", "Right Radius", 1.0, "mm", "Right Radius", "固定的右圆半径。"),
      numParam("rightRadiusMin", "Right Min", 0.3, "mm", "Right Radius", "右圆半径下限。"),
      numParam("rightRadiusMax", "Right Max", 1.8, "mm", "Right Radius", "右圆半径上限。"),
      numParam("rightRadiusPrior", "Right Prior", 0.0, "mm", "Right Radius",
               "右圆半径的软先验，含义同 leftRadiusPrior。"),
      numParam("rightRadiusPriorSigma", "Right Prior Sigma", 0.0, "mm", "Right Radius",
               "右圆先验的宽度，<= 0 表示不加。含义同 leftRadiusPriorSigma。"),
      boolParam("cameraFallback", "Camera Fallback", true, "Camera Fallback",
                "合并云拟合失败时按相机分开再试。只在合并云失败后触发；被遮挡的那台若先给出"
                "合格圆，读到的是它自己的阴影。"),
      boolParam("selectClosestNominal", "Select Closest Nominal", true, "Camera Fallback",
                "两台相机都合格时按 |gap − nominal| 挑。"),
      preferred,
      sideCameraParam("leftCamera", "Left Camera", "Left Radius"),
      sideCameraParam("rightCamera", "Right Camera", "Right Radius"),
      numParam("leftCenterAbove", "Left Center Above", 0.0, "mm", "Left Radius",
               "左圆圆心应当高出 refLine 多少。配合 leftCenterTol 使用。"),
      numParam("leftCenterTol", "Left Center Tol", 0.0, "mm", "Left Radius",
               "圆心高度的容差，<= 0 表示不加这个约束。它只在 RANSAC 里筛候选、不把圆心焊到"
               "那个高度：带比真实散布还窄时合格候选被筛光，这一侧直接拟不出 —— 先量一批正常帧。"),
      centerModeParam("leftCenterMode", "Left Center Mode", "Left Radius"),
      numParam("rightCenterAbove", "Right Center Above", 0.0, "mm", "Right Radius",
               "右圆圆心应当高出 refLine 多少。配合 rightCenterTol 使用。"),
      numParam("rightCenterTol", "Right Center Tol", 0.0, "mm", "Right Radius",
               "圆心高度的容差，<= 0 表示不加这个约束。含义同 leftCenterTol。"),
      centerModeParam("rightCenterMode", "Right Center Mode", "Right Radius"),
      numParam(
          "leftTangentAbove", "Left Tangent Above", 0.0, "mm", "Left Radius",
          "左圆圆顶（圆心高度 + 半径）应当高出 refLine 多少，配合 leftTangentTol 使用。0 = 圆顶正好"
          "压在面线上，即圆角从这张面圆滑拐下去、与面相切。"),
      numParam(
          "leftTangentTol", "Left Tangent Tol", 0.0, "mm", "Left Radius",
          "相切带的容差，<= 0 表示不加（默认）。缝里更深处还有一段弧时（翻边、胶），拟到那上面的圆"
          "半径小、圆心高度却和拐角圆差不多，圆心高度带拦不住，只有圆顶悬在面下面分得开。"
          "档位跟 leftCenterMode 走；refLine 要接这一侧自己那张面的线。先量一批正常帧的 "
          "quality.topAboveRefMm 再定。"),
      numParam(
          "rightTangentAbove", "Right Tangent Above", 0.0, "mm", "Right Radius",
          "右圆圆顶应当高出参考线多少（refLineRight，不接就用 refLine），含义同 leftTangentAbove。"),
      numParam("rightTangentTol", "Right Tangent Tol", 0.0, "mm", "Right Radius",
               "右侧相切带的容差，<= 0 表示不加。含义同 leftTangentTol。"),
      bearingParam("leftArcBearingDeg", "Left Arc Bearing", "Left Radius"),
      bearingTolParam("leftArcBearingTolDeg", "Left Arc Bearing Tol", "Left Radius"),
      bearingParam("rightArcBearingDeg", "Right Arc Bearing", "Right Radius"),
      bearingTolParam("rightArcBearingTolDeg", "Right Arc Bearing Tol", "Right Radius"),
      minInliersParam("leftMinInliers", "Left Min Inliers", "Left Radius"),
      minArcParam("leftMinArcDeg", "Left Min Arc", "Left Radius"),
      minInliersParam("rightMinInliers", "Right Min Inliers", "Right Radius"),
      minArcParam("rightMinArcDeg", "Right Min Arc", "Right Radius"),
  };
}
}

} // namespace lyflow::packs::gap
