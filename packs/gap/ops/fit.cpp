// 直线拟合与间隙两侧的圆拟合。两者都直接调 detection::utils 里的拟合函数。
#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <set>

#include "gap_detection/GapUtils.hpp"
#include "gap_ops.h"
#include "std_bridge.hpp"

namespace lyflow::packs::gap {
namespace {

namespace utils = ::detection::utils;

nlohmann::json numberOrNull(double v) {
  return std::isfinite(v) ? nlohmann::json(v) : nlohmann::json();
}

nlohmann::json lineQualityJson(const GapCloud& cloud, const Eigen::VectorXf& line,
                               const pcl::Indices& inliers) {
  nlohmann::json q;
  q["model"] = "line";
  q["pointCount"] = cloud.size();
  q["inlierCount"] = inliers.size();
  q["inlierRatio"] =
      cloud.empty() ? nlohmann::json()
                    : nlohmann::json(static_cast<double>(inliers.size()) / cloud.size());
  double pointX = std::numeric_limits<double>::quiet_NaN();
  double pointY = std::numeric_limits<double>::quiet_NaN();
  double dirX = std::numeric_limits<double>::quiet_NaN();
  double dirY = std::numeric_limits<double>::quiet_NaN();
  if (line.size() >= 5) {
    pointX = line[0] * kScale;
    pointY = line[1] * kScale;
    const double norm = std::hypot(line[3], line[4]);
    if (norm > 0) {
      dirX = line[3] / norm;
      dirY = line[4] / norm;
    }
  }
  q["linePointXMm"] = numberOrNull(pointX);
  q["linePointYMm"] = numberOrNull(pointY);
  q["lineDirX"] = numberOrNull(dirX);
  q["lineDirY"] = numberOrNull(dirY);

  double rms = std::numeric_limits<double>::quiet_NaN();
  double maximum = std::numeric_limits<double>::quiet_NaN();
  if (line.size() >= 5 && !inliers.empty()) {
    const double dx = line[3];
    const double dy = line[4];
    const double norm = std::hypot(dx, dy);
    double squareSum = 0;
    double worst = 0;
    std::size_t valid = 0;
    if (norm > 0) {
      for (const auto index : inliers) {
        if (index < 0 || static_cast<std::size_t>(index) >= cloud.size()) continue;
        const auto& point = cloud[index];
        const double residual =
            std::fabs((point.x - line[0]) * dy - (point.y - line[1]) * dx) / norm * kScale;
        squareSum += residual * residual;
        worst = std::max(worst, residual);
        ++valid;
      }
    }
    if (valid > 0) {
      rms = std::sqrt(squareSum / static_cast<double>(valid));
      maximum = worst;
    }
  }
  q["rmsResidualMm"] = numberOrNull(rms);
  q["maxResidualMm"] = numberOrNull(maximum);
  return q;
}

/// 内点相对圆心的**方位角**（度，圆周均值）。0° = +x，90° = 正上方（测量帧里 y 越小越高），
/// 所以「点云贴在圆的左上半边」≈ 135°。折进 (−180, 180]。
///
/// 这是弧长覆盖看不出来的那一维：arcCoverageDeg 只说内点张开多少度，不说它们落在圆的哪
/// 一侧。同一段点云贴在圆的左上（对）和贴在圆的顶部加底部（歪了），弧长可以一模一样。
/// 用圆周均值而不是算术平均，免得在 ±180° 首尾相接处算出中间那个相反的方向。
double inlierBearingDeg(const GapCloud& cloud, const Eigen::VectorXf& circle,
                        const pcl::Indices& inliers) {
  if (circle.size() < 3 || inliers.empty()) return std::numeric_limits<double>::quiet_NaN();
  double sumSin = 0;
  double sumCos = 0;
  std::size_t used = 0;
  for (const auto index : inliers) {
    if (index < 0 || static_cast<std::size_t>(index) >= cloud.size()) continue;
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
  q["inlierRatio"] =
      cloud.empty() ? nlohmann::json()
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
      if (index < 0 || static_cast<std::size_t>(index) >= cloud.size()) continue;
      const auto& point = cloud[index];
      const double deltaX = point.x - circle[0];
      const double deltaY = point.y - circle[1];
      const double residual = std::fabs(std::hypot(deltaX, deltaY) - circle[2]) * kScale;
      squareSum += residual * residual;
      worst = std::max(worst, residual);
      double angle = std::atan2(deltaY, deltaX);
      if (angle < 0) angle += 2 * M_PI;
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

lyflow::Line2D lineFromCoefficients(const Eigen::VectorXf& c) {
  lyflow::Line2D line;
  line.point[0] = c[0];
  line.point[1] = c[1];
  const float nx = std::hypot(c[3], c[4]);
  line.dir[0] = nx > 0 ? c[3] / nx : 1.0f;
  line.dir[1] = nx > 0 ? c[4] / nx : 0.0f;
  return line;
}

/// 直线方向 a 相对 b 的夹角，折进 (-90, 90]：直线没有正反，差 180° 是同一条线。
double lineAngleDeltaDeg(double ax, double ay, double bx, double by) {
  const double d = (std::atan2(ay, ax) - std::atan2(by, bx)) * 180.0 / M_PI;
  double r = std::fmod(d, 180.0);
  if (r > 90.0) r -= 180.0;
  if (r <= -90.0) r += 180.0;
  return r;
}

/// 方向钉死之后，直线只剩法向偏移一个自由度。取全部点投影的中位定位置（中位对
/// 「一半点落在台阶另一侧」这种情形免疫），再用内点的中位收一次。
/// coefficients 仍按 PCL 的 6 维直线模型写回，下游的端点/质量逻辑零改动。
bool fitFixedDirection(const GapCloud& cloud, double dirX, double dirY, float dist,
                       Eigen::VectorXf* coefficients, pcl::Indices* indices) {
  const std::size_t n = cloud.size();
  if (n < 2) return false;
  const double len = std::hypot(dirX, dirY);
  if (!(len > 0)) return false;
  const double ux = dirX / len;
  const double uy = dirY / len;
  const double nx = -uy;  // 法向
  const double ny = ux;
  std::vector<double> proj;
  proj.reserve(n);
  for (const auto& q : cloud) proj.push_back(q.x * nx + q.y * ny);
  const auto median = [](std::vector<double> v) {
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
    return v[v.size() / 2];
  };
  double t = median(proj);
  std::vector<double> kept;
  pcl::Indices inliers;
  for (int pass = 0; pass < 2; ++pass) {
    kept.clear();
    inliers.clear();
    for (std::size_t i = 0; i < n; ++i) {
      if (std::fabs(proj[i] - t) <= dist) {
        inliers.push_back(static_cast<int>(i));
        kept.push_back(proj[i]);
      }
    }
    // 方向被钉住之后，点沿法向的散布 ≈ ROI 宽度 × 两条线的夹角；窄 ROI 上这个量能
    // 直接超过 distThresh，按内点收就一个都收不到。那时退而取最近的一半点 ——
    // 位置本来就只由中位定，收不到内点不该让整帧算不出来。
    if (kept.size() < 2) {
      std::vector<std::size_t> order(n);
      for (std::size_t i = 0; i < n; ++i) order[i] = i;
      const std::size_t half = std::max<std::size_t>(2, n / 2);
      std::partial_sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(half),
                        order.end(), [&](std::size_t a, std::size_t b) {
                          return std::fabs(proj[a] - t) < std::fabs(proj[b] - t);
                        });
      order.resize(half);
      std::sort(order.begin(), order.end());
      for (std::size_t i : order) {
        inliers.push_back(static_cast<int>(i));
        kept.push_back(proj[i]);
      }
    }
    t = median(kept);
  }
  coefficients->resize(6);
  (*coefficients)[0] = static_cast<float>(t * nx);
  (*coefficients)[1] = static_cast<float>(t * ny);
  (*coefficients)[2] = 0.0f;
  (*coefficients)[3] = static_cast<float>(ux);
  (*coefficients)[4] = static_cast<float>(uy);
  (*coefficients)[5] = 0.0f;
  *indices = std::move(inliers);
  return true;
}

/// 沿直线方向的单位向量，朝 +x（竖直时朝 +y）。首尾端点与 toward 投影都按它算。
Eigen::Vector2d lineAxis(const Eigen::VectorXf& coefficients) {
  Eigen::Vector2d u(coefficients[3], coefficients[4]);
  const double len = u.norm();
  if (!(len > 0)) return Eigen::Vector2d(1, 0);
  u /= len;
  if (u.x() < 0 || (u.x() == 0 && u.y() < 0)) u = -u;
  return u;
}

double projectOnAxis(const GapPoint& p, const Eigen::Vector2d& u) {
  return p.x * u.x() + p.y * u.y();
}

Status fitLine(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext& ctx) {
  const lyflow::PointCloud& in = *inputs.get("cloud").asCloud();
  const lyflow::Box2D& box = *inputs.get("box").asBox2D();
  const lyflow::Box2D& toward = *inputs.get("toward").asBox2D();
  const GapCloud cloud = toPcl(in);
  if (cloud.size() < 2) {
    return Status::Error(Phase::Execute, "insufficient_points", "点太少，拟合不了直线", {},
                         "cloud");
  }
  const float dist = mmToM(params.number("distThresh"));
  const auto segmentPoints = static_cast<std::size_t>(params.integer("segmentPoints"));
  const std::string lineType = params.choice("lineType");
  // 「靠缝那一头」由 toward 框的中心定：内点沿直线方向投影，离它的投影近的那一头就是。
  const double towardX = 0.5 * (static_cast<double>(toward.min[0]) + toward.max[0]);
  const double towardY = 0.5 * (static_cast<double>(toward.min[1]) + toward.max[1]);

  // 方向约束：把方向锚到另一条线上（通常是 gap.datum_window 在旁边长面上拟的基准线）。
  // dirMode 不是 free 时 refLine 一定接着 —— validate 已经挡过。
  const std::string dirMode = params.choice("dirMode");
  const lyflow::Line2D* refLine = inputs.has("refLine") ? inputs.get("refLine").asLine2D() : nullptr;
  const double nominal = params.number("dirNominalDeg");
  const double tol = params.number("dirTolDeg");
  double pinX = 1.0;
  double pinY = 0.0;
  if (refLine != nullptr) {
    const double a = std::atan2(refLine->dir[1], refLine->dir[0]) + nominal * M_PI / 180.0;
    pinX = std::cos(a);
    pinY = std::sin(a);
  }

  Eigen::VectorXf coefficients;
  pcl::Indices indices;
  bool fitted = lineType == "fit"
                    ? gap_std::lineFit2D(cloud, &coefficients, &indices, dist)
                    : gap_std::lineFit2D(cloud, &coefficients, &indices,
                                         lineType == "vertical" ? "vertical line"
                                                                : "horizontal line",
                                         dist);
  if (!fitted && dirMode == "free") {
    return Status::Error(Phase::Execute, "line_fit_failed", "直线拟合失败", {}, "cloud");
  }

  // 截取靠缝那一端再拟合一次：按沿直线方向的投影，留下离 toward 最近的 segmentPoints 个内点。
  // 不看点序 —— 点在文件里朝哪个方向排，与缝在哪一侧无关。
  bool segmentApplied = false;
  if (fitted && segmentPoints > 0 && segmentPoints < indices.size()) {
    const Eigen::Vector2d u = lineAxis(coefficients);
    const double target = towardX * u.x() + towardY * u.y();
    std::stable_sort(indices.begin(), indices.end(), [&](int a, int b) {
      return std::fabs(projectOnAxis(cloud[a], u) - target) <
             std::fabs(projectOnAxis(cloud[b], u) - target);
    });
    indices.resize(segmentPoints);
    std::sort(indices.begin(), indices.end());
    segmentApplied = true;
    const GapCloud segment(cloud, indices);
    pcl::Indices second;
    if (!gap_std::lineFit2D(segment, &coefficients, &second, dist / 3)) {
      if (dirMode == "free") {
        return Status::Error(Phase::Execute, "line_fit_failed", "截取之后的第二次直线拟合失败",
                             {}, "cloud");
      }
      fitted = false;
    } else {
      std::size_t j = 0;
      for (auto i : second) indices[j++] = indices[static_cast<std::size_t>(i)];
      indices.resize(second.size());
    }
  } else if (fitted && segmentPoints > 0) {
    ctx.log(LogLevel::Warn, "内点 " + std::to_string(indices.size()) + " 个，不多于 segmentPoints " +
                                std::to_string(segmentPoints) + "，没有截取靠缝那一端");
  }
  if (fitted && indices.empty()) {
    if (dirMode == "free") {
      return Status::Error(Phase::Execute, "line_fit_failed", "拟合之后一个内点都没有", {},
                           "cloud");
    }
    fitted = false;
  }

  // fixed 一律钉死；band 只在自由拟合失败、或方向出了带宽时才钉 —— 合规帧照旧。
  bool pinned = dirMode == "fixed";
  if (dirMode == "band" && !pinned) {
    pinned = !fitted || std::fabs(lineAngleDeltaDeg(coefficients[3], coefficients[4], pinX,
                                                    pinY)) > tol;
  }
  if (pinned && !fitFixedDirection(cloud, pinX, pinY, dist, &coefficients, &indices)) {
    return Status::Error(Phase::Execute, "line_fit_failed",
                         "方向钉死之后仍然拟不出直线（内点不足）", {}, "cloud");
  }
  if (!pinned && !fitted) {
    return Status::Error(Phase::Execute, "line_fit_failed", "直线拟合失败", {}, "cloud");
  }
  // 内点太少时宁可报失败：ROI 跑偏、窗口落到没点的地方时，拟合照样"成功"，
  // 只是拟出一条没意义的线，下游拿它当基准就是把错误往前传。
  const auto minInliers = static_cast<std::size_t>(params.integer("minInliers"));
  if (indices.size() < minInliers) {
    return Status::Error(Phase::Execute, "insufficient_points",
                         "内点只有 " + std::to_string(indices.size()) + " 个，少于 minInliers " +
                             std::to_string(minInliers),
                         "minInliers", "cloud");
  }
  std::sort(indices.begin(), indices.end());

  // 端点、innerEnd 都按最终那条线的方向投影来挑，与点序无关。
  const Eigen::Vector2d u = lineAxis(coefficients);
  const double target = towardX * u.x() + towardY * u.y();
  std::size_t first = 0, last = 0, inner = 0;
  for (std::size_t k = 1; k < indices.size(); ++k) {
    const double sk = projectOnAxis(cloud[indices[k]], u);
    if (sk < projectOnAxis(cloud[indices[first]], u)) first = k;
    if (sk > projectOnAxis(cloud[indices[last]], u)) last = k;
    if (std::fabs(sk - target) < std::fabs(projectOnAxis(cloud[indices[inner]], u) - target)) {
      inner = k;
    }
  }

  lyflow::Line2D line = lineFromCoefficients(coefficients);
  // 端点两种口径：ROI 框交点，或内点里沿直线方向最靠两头的那两个真实云点。
  // gap definition A 的方向 u 就取自这两个端点。
  if (params.choice("endpoints") == "inlier_ends") {
    const GapPoint& a = cloud[indices[first]];
    const GapPoint& b = cloud[indices[last]];
    line.hasSegment = true;
    line.start[0] = a.x;
    line.start[1] = a.y;
    line.end[0] = b.x;
    line.end[1] = b.y;
  } else {
    const Eigen::Matrix2f ends = utils::getIntersectionRL(toRoiMatrix(box), coefficients);
    if (std::isfinite(ends(0, 0)) && std::isfinite(ends(0, 1))) {
      line.hasSegment = true;
      line.start[0] = ends(0, 0);
      line.start[1] = ends(1, 0);
      line.end[0] = ends(0, 1);
      line.end[1] = ends(1, 1);
    } else {
      ctx.log(LogLevel::Warn, "拟合出的直线与 ROI 框没有两个交点，line 只带方程不带端点");
    }
  }
  outputs.set("line", Data::line2d(line));

  const GapPoint& innerPoint = cloud[indices[inner]];
  lyflow::Point2D end;
  end.p[0] = innerPoint.x;
  end.p[1] = innerPoint.y;
  outputs.set("innerEnd", Data::point2d(end));

  lyflow::Indices out;
  out.sourceCloudId = in.id;
  out.values.reserve(indices.size());
  for (auto i : indices) out.values.push_back(static_cast<std::int32_t>(i));
  outputs.set("inliers", Data::indices(std::move(out)));

  lyflow::Record quality;
  quality.type = "GapFitQuality";
  quality.data = lineQualityJson(cloud, coefficients, indices);
  quality.data["segmentApplied"] = segmentApplied;
  outputs.set("quality", Data::record(std::move(quality)));
  return Status::Ok();
}

std::vector<Issue> validateFitLine(const ParamView& params,
                                   const std::set<std::string>& connected) {
  std::vector<Issue> issues;
  if (params.choice("dirMode") != "free" && !connected.count("refLine")) {
    issues.push_back(
        Issue::error("bad_param", "dirMode 不是 free 时必须接 refLine", "dirMode", "refLine"));
  }
  return issues;
}

// ---------------------------------------------------------------- 圆拟合

struct SideConfig {
  double rMin = 0;
  double rMax = 0;
  double rFixed = 0;
};

struct SideResult {
  Eigen::VectorXf circle;
  pcl::Indices indices;
  GapCloud cloud;
  bool fitted = false;
  /// 相机分开拟合且两台都合格：留到最后按 |gap − nominal| 二选一
  bool deferred = false;
  std::array<Eigen::VectorXf, 2> candidates;
  std::array<pcl::Indices, 2> candidateIndices;
  std::array<GapCloud, 2> candidateClouds;
  std::string model = "circle";
};

/// 内点覆盖的圆弧角度（度）。和 circleQualityJson 里报的是同一个量 —— 那边只是顺手
/// 算出来写进 quality，这里要拿它当判据，所以单独抽一份。
double arcCoverageDeg(const GapCloud& cloud, const Eigen::VectorXf& circle,
                      const pcl::Indices& inliers) {
  if (circle.size() < 3 || inliers.empty()) return 0.0;
  std::vector<double> angles;
  angles.reserve(inliers.size());
  for (const auto index : inliers) {
    if (index < 0 || static_cast<std::size_t>(index) >= cloud.size()) continue;
    double angle = std::atan2(cloud[index].y - circle[1], cloud[index].x - circle[0]);
    if (angle < 0) angle += 2 * M_PI;
    angles.push_back(angle);
  }
  if (angles.empty()) return 0.0;
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
  if (std::fabs(dx) < 1e-9) return std::numeric_limits<double>::quiet_NaN();
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
      if (!(d > 0)) continue;
      const Eigen::Vector2d g = -v / d;  // ∂(d − r)/∂c
      jtj += g * g.transpose();
      jtr += g * (d - radius);
    }
    const Eigen::Vector2d step = jtj.ldlt().solve(-jtr);
    if (!step.allFinite()) break;
    center += step;
    if (step.norm() < 1e-10) break;
  }
  return center;
}

/// 带约束的圆拟合：圆心高度带（要 line）和内点方位角带，两条都可以单独开。
/// 共用的 gap_std::circleFit2D 不带这些判据，所以这里自己跑一遍 RANSAC，只在配了约束时走。
/// 采样用固定种子，同样的输入永远给同样的输出（算子声明了 deterministic）。
///
/// `line` 为空表示不查圆心高度；`bearingTolDeg <= 0` 表示不查方位角。
/// `rFixed > 0` 时半径钉死：两点加半径定圆心（每对点两个解），细化也只动圆心。
bool circleFitConstrained(const GapCloud& cloud, const lyflow::Line2D* line, double aboveM,
                          double tolM, double bearingDeg, double bearingTolDeg, double distThresh,
                          double rMin, double rMax, double rFixed, Eigen::VectorXf* circle,
                          pcl::Indices* inliers) {
  const std::size_t n = cloud.size();
  if (n < 3) return false;
  const bool fixedRadius = rFixed > 0;
  const bool checkCenter = line != nullptr && tolM > 0;
  const bool checkBearing = bearingTolDeg > 0;
  // 方位角要先有内点才能算，所以它在下面按候选的内点集单独查；这里只查半径和圆心高度。
  const auto ok = [&](double cx, double cy, double r) {
    if (!(r >= rMin && r <= rMax)) return false;
    if (!checkCenter) return true;
    Eigen::VectorXf probe(3);
    probe << static_cast<float>(cx), static_cast<float>(cy), static_cast<float>(r);
    const double above = centerAboveLine(probe, *line);
    return std::isfinite(above) && std::fabs(above - aboveM) <= tolM;
  };
  const auto bearingOk = [&](const Eigen::Vector3d& c, const pcl::Indices& idx) {
    if (!checkBearing) return true;
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
    if (!ok(ux, uy, r)) return;
    std::size_t count = 0;
    pcl::Indices candidate;
    for (std::size_t q = 0; q < n; ++q) {
      if (std::fabs(std::hypot(cloud[q].x - ux, cloud[q].y - uy) - r) < distThresh) {
        ++count;
        if (checkBearing) candidate.push_back(static_cast<int>(q));
      }
    }
    if (count <= bestCount) return;
    if (!bearingOk(Eigen::Vector3d(ux, uy, r), candidate)) return;
    bestCount = count;
    best = Eigen::Vector3d(ux, uy, r);
  };
  // 迭代数与 PCL 那条路（Circle2DFitOptions::maxIterations）取齐，免得「加了约束反而
  // 更容易拟出来」只是因为采样次数不同。
  for (int iter = 0; iter < 10000; ++iter) {
    const std::size_t a = pick(rng);
    const std::size_t b = pick(rng);
    if (a == b) continue;
    const double ax = cloud[a].x, ay = cloud[a].y;
    const double bx = cloud[b].x, by = cloud[b].y;
    if (fixedRadius) {
      const double half = 0.5 * std::hypot(bx - ax, by - ay);
      if (!(half > 1e-12) || half > rFixed) continue;
      const double h = std::sqrt(rFixed * rFixed - half * half);
      const double mx = 0.5 * (ax + bx), my = 0.5 * (ay + by);
      const double nx = -(by - ay) / (2 * half), ny = (bx - ax) / (2 * half);
      consider(mx + h * nx, my + h * ny, rFixed);
      consider(mx - h * nx, my - h * ny, rFixed);
      continue;
    }
    const std::size_t c = pick(rng);
    if (b == c || a == c) continue;
    const double cx = cloud[c].x, cy = cloud[c].y;
    const double d = 2.0 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));
    if (std::fabs(d) < 1e-15) continue;
    const double ux = ((ax * ax + ay * ay) * (by - cy) + (bx * bx + by * by) * (cy - ay) +
                       (cx * cx + cy * cy) * (ay - by)) / d;
    const double uy = ((ax * ax + ay * ay) * (cx - bx) + (bx * bx + by * by) * (ax - cx) +
                       (cx * cx + cy * cy) * (bx - ax)) / d;
    consider(ux, uy, std::hypot(ax - ux, ay - uy));
  }
  if (bestCount < 3) return false;
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
      rhs(static_cast<Eigen::Index>(k)) = static_cast<double>(q.x) * q.x + static_cast<double>(q.y) * q.y;
    }
    const Eigen::Vector3d sol = A.colPivHouseholderQr().solve(rhs);
    const double rr = std::sqrt(std::max(sol[2] + sol[0] * sol[0] + sol[1] * sol[1], 0.0));
    // 重拟后两条约束都要仍然成立，否则退回粗解。
    const Eigen::Vector3d candidate(sol[0], sol[1], rr);
    if (ok(sol[0], sol[1], rr) && bearingOk(candidate, keep)) refined = candidate;
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

GapCloud cropStrict(const GapCloud& src, const Eigen::Matrix2f& roi) {
  GapCloud out;
  gap_std::roiCrop2D(src, &out, roi);
  return out;
}

Status fitGapCircles(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                     ExecContext& ctx) {
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
  const lyflow::Line2D* refLine = inputs.has("refLine") ? inputs.get("refLine").asLine2D() : nullptr;
  const lyflow::Line2D* refLineRight =
      inputs.has("refLineRight") ? inputs.get("refLineRight").asLine2D() : nullptr;
  // 两侧各比各的线。右侧不接 refLineRight 就退回 refLine。缝两侧贴的是不同的面时
  // （左圆贴基准面、右圆贴玻璃面）才需要分开，同一条线也能服务两侧。
  // 配了高度带却没接线的组合 validate 已经挡过。
  const lyflow::Line2D* sideRefLine[2] = {refLine, refLineRight != nullptr ? refLineRight : refLine};

  constexpr std::size_t kMinimumCameraInliers = 8;
  const double maxRadiusDifference = 0.25 / kScale;
  const double maxCenterDifference = 0.75 / kScale;

  // 一侧的拟合：guard 先按原样拟、只有圆心落到带外才换约束那条路（带内的帧结果与不加带
  // 相同，所以给「本来就拟得对」的点位挂一条宽带纯属保险）；always 一律走约束。
  // 抽成 lambda 是为了「弱了就换合并云再来一次」能原样重跑。
  const auto fitOneSide = [&](SideResult& side, int i) {
    const bool constrained = centerTol[i] > 0 || bearingTol[i] > 0;
    const bool guardOnly = constrained && centerMode[i] == "guard";
    if (!constrained || guardOnly) {
      side.fitted = gap_std::circleFit2D(side.cloud, &side.circle, &side.indices, distThresh,
                                     cfg[i].rMin, cfg[i].rMax, cfg[i].rFixed);
      if (!side.fitted && retryDistanceMm > 0) {
        side.indices.clear();
        side.fitted = gap_std::circleFit2D(side.cloud, &side.circle, &side.indices,
                                       mmToM(retryDistanceMm), cfg[i].rMin, cfg[i].rMax,
                                       cfg[i].rFixed);
      }
    }
    bool needBand = constrained && !guardOnly;
    if (guardOnly) {
      // 任何一条约束落空都要重来。
      needBand = !side.fitted;
      if (!needBand && centerTol[i] > 0) {
        const double above = centerAboveLine(side.circle, *sideRefLine[i]);
        needBand = !std::isfinite(above) || std::fabs(above - centerAbove[i]) > centerTol[i];
      }
      if (!needBand && bearingTol[i] > 0) {
        const double b = inlierBearingDeg(side.cloud, side.circle, side.indices);
        needBand = !std::isfinite(b) || bearingDeltaDeg(b, bearing[i]) > bearingTol[i];
      }
    }
    if (needBand) {
      side.indices.clear();
      side.fitted = circleFitConstrained(side.cloud, sideRefLine[i], centerAbove[i], centerTol[i],
                                         bearing[i], bearingTol[i], distThresh, cfg[i].rMin,
                                         cfg[i].rMax, cfg[i].rFixed, &side.circle, &side.indices);
      if (!side.fitted && retryDistanceMm > 0) {
        side.indices.clear();
        side.fitted = circleFitConstrained(side.cloud, sideRefLine[i], centerAbove[i], centerTol[i],
                                           bearing[i], bearingTol[i], mmToM(retryDistanceMm),
                                           cfg[i].rMin, cfg[i].rMax, cfg[i].rFixed, &side.circle,
                                           &side.indices);
      }
      if (side.fitted) {
        side.model = centerTol[i] > 0 ? (bearingTol[i] > 0 ? "circle-center-bearing-band"
                                                           : "circle-center-band")
                                      : "circle-bearing-band";
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
    side.cloud = cropStrict(source, roi);
    if (side.cloud.empty()) {
      return Status::Error(Phase::Execute, "roi_empty",
                           std::string(i == 0 ? "左" : "右") + "间隙 ROI 里没有点", {},
                           i == 0 ? "boxLeft" : "boxRight");
    }
    fitOneSide(side, i);
    // 弱就重来：钉死单相机时先退回合并云（那台被挡住的时候另一台往往是好的），
    // 合并云也弱就当没拟出来，交给下面既有的回退，最后报失败 —— 宁可没有也别给个错的。
    const auto weak = [&](const SideResult& s2) {
      if (!s2.fitted) return true;
      if (minInliers[i] > 0 && s2.indices.size() < minInliers[i]) return true;
      return minArc[i] > 0 && arcCoverageDeg(s2.cloud, s2.circle, s2.indices) < minArc[i];
    };
    if (weak(side) && sideCamera[i] != "Both") {
      SideResult retry;
      retry.cloud = cropStrict(merged, roi);
      retry.fitted = fitOneSide(retry, i);
      if (!weak(retry)) {
        retry.model += "-merged-retry";
        side = std::move(retry);
      }
    }
    if (weak(side)) {
      side.fitted = false;
      side.indices.clear();
    }

    if (side.fitted || !fallback || sideCamera[i] != "Both") continue;

    // 相机分开拟合的回退：两台相机各自的 ROI 点各拟合一个圆，固定半径照样生效。
    GapCloud primaryRoi = cropStrict(primary, roi);
    GapCloud secondaryRoi = cropStrict(secondary, roi);
    const double separatedFixed = cfg[i].rFixed;
    Eigen::VectorXf primaryCircle, secondaryCircle;
    pcl::Indices primaryIndices, secondaryIndices;
    const bool primaryOk = gap_std::circleFit2D(primaryRoi, &primaryCircle, &primaryIndices,
                                            distThresh, cfg[i].rMin, cfg[i].rMax, separatedFixed);
    const bool secondaryOk =
        gap_std::circleFit2D(secondaryRoi, &secondaryCircle, &secondaryIndices, distThresh,
                         cfg[i].rMin, cfg[i].rMax, separatedFixed);
    const bool primaryEligible = primaryOk && primaryIndices.size() >= kMinimumCameraInliers;
    const bool secondaryEligible = secondaryOk && secondaryIndices.size() >= kMinimumCameraInliers;
    const bool consistent =
        primaryOk && secondaryOk && primaryEligible && secondaryEligible &&
        std::fabs(primaryCircle[2] - secondaryCircle[2]) <= maxRadiusDifference &&
        (primaryCircle.head<2>() - secondaryCircle.head<2>()).norm() <= maxCenterDifference;

    const auto take = [&](bool takePrimary) {
      side.cloud = takePrimary ? primaryRoi : secondaryRoi;
      side.circle = takePrimary ? primaryCircle : secondaryCircle;
      side.indices = takePrimary ? primaryIndices : secondaryIndices;
      side.fitted = true;
      side.model = takePrimary ? "camera-separated-circle-primary"
                               : "camera-separated-circle-secondary";
    };
    if (selectClosestNominal && primaryEligible && secondaryEligible) {
      side.deferred = true;
      side.candidates = {primaryCircle, secondaryCircle};
      side.candidateIndices = {primaryIndices, secondaryIndices};
      side.candidateClouds = {primaryRoi, secondaryRoi};
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
      ctx.log(LogLevel::Info, std::string(i == 0 ? "gap_left" : "gap_right") +
                                  " 走了相机分开拟合的回退");
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
        if (!std::isfinite(gapMm) || start.x() > end.x()) continue;
        const double delta = std::fabs(gapMm - nominal);
        const std::size_t inliers =
            (sides[0].deferred ? sides[0].candidateIndices[l].size() : 0) +
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
      if (!sides[i].deferred) continue;
      sides[i].circle = sides[i].candidates[picked[i]];
      sides[i].indices = sides[i].candidateIndices[picked[i]];
      sides[i].cloud = sides[i].candidateClouds[picked[i]];
      sides[i].model = std::string("camera-separated-circle-") +
                       (picked[i] == 0 ? "primary" : "secondary") + "-closest-gap-nominal";
    }
  }

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
    for (auto v : sides[i].indices) idx.values.push_back(static_cast<std::int32_t>(v));
    outputs.set(kClouds[i], Data::cloud(std::move(fitted)));
    outputs.set(kInliers[i], Data::indices(std::move(idx)));
  }

  lyflow::Record quality;
  quality.type = "GapFitQualityPair";
  static const char* kSides[2] = {"left", "right"};
  for (int i = 0; i < 2; ++i) {
    const bool isFixed = cfg[i].rFixed > 0;
    const std::string radiusMode =
        sides[i].circle.size() >= 3
            ? utils::classifyRadiusMode(sides[i].circle[2], cfg[i].rMin, cfg[i].rMax, isFixed)
            : std::string();
    quality.data[kSides[i]] =
        circleQualityJson(sides[i].cloud, sides[i].circle, sides[i].indices, sides[i].model,
                          radiusMode);
  }
  outputs.set("quality", Data::record(std::move(quality)));
  return Status::Ok();
}

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
    issues.push_back(Issue::error(
        "bad_param", "配了右侧圆心高度带就必须接 refLineRight 或 refLine（参考线）",
        "rightCenterTol", "refLineRight"));
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
        issues.push_back(Issue::error("bad_param", "固定半径必须落在上下限之间", s + "RadiusValue"));
      }
    }
  }
  return issues;
}

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
  p.doc =
      "这一侧内点少于它就算没拟出来。0 = 不检查（默认）。圆拟合本身 3 个内点就算成功，"
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
  p.doc =
      "内点覆盖的圆弧角度小于它就算没拟出来。0 = 不检查（默认）。比内点数更直接："
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
  p.doc =
      "内点应当落在圆的哪一侧，用相对圆心的方位角表示：0° = 正右，90° = 正上"
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
  p.doc =
      "方位角的容差，<= 0 表示不加这个约束（默认）。弧长覆盖只说内点张开多少度，不说它们"
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
      "圆心高度带怎么用。always 一律走带约束的拟合；guard 先按原样拟，只有圆心落到带外"
      "才重来 —— 带内的帧结果与不加带相同，所以「本来就拟得对、只是想上个保险」的点位该用 guard。";
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
  p.doc =
      "这一侧的圆用哪台相机的点。Both = 合并云（默认）。两台锁在不同界面上时（例如夹胶"
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

}  // namespace

void registerFitLine(Registry& r) {
  OperatorDesc op;
  op.id = "gap.fit_line";
  op.version = "1.1.0";
  op.label = "拟合直线";
  op.category = "间隙/拟合";
  op.keywords = {"line", "ransac", "直线", "拟合"};
  op.doc =
      "在 ROI 里拟合一条直线：先整体拟合，再截取靠缝隙那一端的 segmentPoints 个内点、"
      "用 1/3 的阈值重拟合一次。「靠缝那一端」由 toward 框的中心定：内点沿直线方向投影，"
      "离它最近的那些留下，innerEnd 也取离它最近的那一个。line 带与 ROI 框的两个交点作端点。\n"
      "假定 ROI 里那条边确实近似一条直线；圆角或台阶进了 ROI，拟合会咬住它们。";
  op.inputs = {
      Port{"cloud", "PointCloud", "Cloud", "已经按业务 ROI 裁过的点云。", true},
      Port{"box", "Box2D", "Box", "同一个业务 ROI，用来求端点。", true},
      Port{"toward", "Box2D", "Toward",
           "缝那一侧的 ROI（只用它的中心）。决定截取哪一头、innerEnd 取哪一端。"
           "基准线/参考线接同侧的 gap 框；方向基准线接它的锚框。", true},
      Port{"refLine", "Line2D", "Ref Line",
           "方向约束的参考线，通常是 gap.datum_window 推出来的长面上拟的那条。"
           "只有 dirMode 不是 free 时才需要。", false},
  };
  op.outputs = {
      Port{"line", "Line2D", "Line", "拟合出的直线（带端点）。", true},
      Port{"inliers", "Indices", "Inliers", "内点下标，指向输入点云。", true},
      Port{"innerEnd", "Point2D", "Inner End",
           "内点里沿直线方向离 toward 中心最近的那个真实云点。", true},
      withExample(
          Port{"quality", "Record", "Quality",
               "GapFitQuality：点数、内点、残差、直线方程，以及 segmentApplied（这一帧截取了没有）。",
               true},
          examples::fitQuality()),
  };

  Param lineType;
  lineType.name = "lineType";
  lineType.type = ParamType::Enum;
  lineType.label = "Line Type";
  lineType.doc = "水平/竖直会反复重拟合直到方向合适；fit 是普通拟合。";
  lineType.def = Value::text("fit");
  lineType.advanced = true;
  lineType.options = {EnumOption{"fit", "Fit Line", ""},
                      EnumOption{"horizontal", "Horizontal Line", ""},
                      EnumOption{"vertical", "Vertical Line", ""}};

  Param distThresh;
  distThresh.name = "distThresh";
  distThresh.type = ParamType::Float;
  distThresh.label = "Dist Thresh";
  distThresh.doc =
      "内点判定距离（line_fit_distance）。过紧时第二次拟合（阈值 1/3）的内点集会在弯曲的"
      "棱边上跳、逐帧不稳 —— 收紧之前先看 quality 的 inlierRatio 与 rmsResidualMm。";
  distThresh.def = Value::number(0.1);
  distThresh.unit = "mm";

  Param segmentPoints;
  segmentPoints.name = "segmentPoints";
  segmentPoints.type = ParamType::Int;
  segmentPoints.label = "Segment Points";
  segmentPoints.doc =
      "截取多少个内点做第二次拟合。0 = 有意不截（整条都要，比如方向基准线）；"
      "不少于内点数时也不截，但那多半不是本意：quality.segmentApplied=false，并发一条 warn 日志。";
  segmentPoints.def = Value::integer(500);
  segmentPoints.min = 0.0;

  Param endpoints;
  endpoints.name = "endpoints";
  endpoints.type = ParamType::Enum;
  endpoints.label = "Endpoints";
  endpoints.doc =
      "line 的两个端点怎么取。模板路径是 ROI 交点，模型路径是首尾内点；"
      "gap definition A 的方向 u 就取自它们。";
  endpoints.def = Value::text("roi_intersection");
  endpoints.options = {EnumOption{"roi_intersection", "ROI 交点", "直线与 ROI 框的两个交点。"},
                       EnumOption{"inlier_ends", "首尾内点",
                                  "内点里沿直线方向最靠两头的两个真实云点。"}};

  Param dirMode;
  dirMode.name = "dirMode";
  dirMode.type = ParamType::Enum;
  dirMode.label = "Dir Mode";
  dirMode.doc =
      "方向怎么定。ROI 很窄时（天幕 L4 的段差基准面只有 2 mm）拟出来的方向基本是噪声，"
      "而偶尔会整条歪掉几十度 —— 那时残差反而很小，任何质量指标都看不出来。"
      "把方向锚到旁边那张长面上就没有这个问题。";
  dirMode.def = Value::text("free");
  dirMode.advanced = true;
  dirMode.options = {
      EnumOption{"free", "Free", "照旧，方向由这片点自己定。"},
      EnumOption{"band", "Band", "方向出了带宽（或自由拟合失败）才钉死，合规帧与 free 相同。"},
      EnumOption{"fixed", "Fixed", "方向一律钉死成 refLine + 标称偏置，只拟法向偏移。"}};

  Param dirNominal;
  dirNominal.name = "dirNominalDeg";
  dirNominal.type = ParamType::Float;
  dirNominal.label = "Dir Nominal";
  dirNominal.doc =
      "这条线相对 refLine 的标称倾角。两张面之间的相对倾角是零件的固有量，量一批正常帧"
      "定下来 —— 填 0 等于假设两张面平行，多半不对。它被当成常数：两张面之间真有随件变化的"
      "相对转动时，钉死方向就是把那部分变化抹掉，先看一批帧的倾角散布再选 fixed 还是 band。";
  dirNominal.def = Value::number(0.0);
  dirNominal.unit = "°";
  dirNominal.advanced = true;
  dirNominal.visibleWhen.param = "dirMode";
  dirNominal.visibleWhen.in = {Value::text("band"), Value::text("fixed")};

  Param dirTol;
  dirTol.name = "dirTolDeg";
  dirTol.type = ParamType::Float;
  dirTol.label = "Dir Tol";
  dirTol.doc = "band 用：偏离标称超过它就钉死。fixed 不看这个值。";
  dirTol.def = Value::number(12.0);
  dirTol.unit = "°";
  dirTol.advanced = true;
  dirTol.visibleWhen.param = "dirMode";
  dirTol.visibleWhen.eq = Value::text("band");

  Param minInliers;
  minInliers.name = "minInliers";
  minInliers.type = ParamType::Int;
  minInliers.label = "Min Inliers";
  minInliers.doc =
      "内点少于它就报 insufficient_points。0 = 不检查（默认）。"
      "ROI 偶尔整个跑偏时拟合不会失败，只会给一条没意义的线 —— 这是唯一拦得住的地方。";
  minInliers.def = Value::integer(0);
  minInliers.advanced = true;

  op.params = {lineType, distThresh, segmentPoints, endpoints,
               dirMode,  dirNominal, dirTol,       minInliers};
  op.capabilities = {false, false, true};
  op.compute = &fitLine;
  op.validate = &validateFitLine;
  r.addOperator(std::move(op));
}

void registerFitGapCircles(Registry& r) {
  OperatorDesc op;
  op.id = "gap.fit_gap_circles";
  op.version = "1.2.0";
  op.label = "拟合间隙圆";
  op.category = "间隙/拟合";
  op.keywords = {"circle", "ransac", "圆", "拟合", "间隙"};
  op.doc =
      "间隙两侧的圆拟合。先在合并云的 ROI 里拟合；失败按 retryDistance 再试一次；"
      "还失败就退到「两台相机各拟合一个」，两台都合格时按 |gap − nominal| 二选一（§3.9）。"
      "另有两个逐侧的收紧手段：leftCamera/rightCamera 把某一侧钉到单台相机；"
      "centerAbove/centerTol 要求圆心落在 refLine 上方的一条窄带里 —— 右侧要比另一条面时"
      "单独接 refLineRight。固定半径（*RadiusFixed）在每一条路径上都生效。\n"
      "假定缝两侧各有一段看得见的圆边、半径落在上下限之内；缝闭合到两圆边相碰时 ROI 里凑不出"
      "圆弧，那种缝用 gap.notch_width。";
  op.inputs = {
      Port{"merged", "PointCloud", "Merged", "合并并滤波之后的云。", true},
      Port{"primary", "PointCloud", "Primary", "整体 ROI 裁过、**未**滤波的 Master 云。", true},
      Port{"secondary", "PointCloud", "Secondary", "整体 ROI 裁过、**未**滤波的 Slave 云。", true},
      Port{"boxLeft", "Box2D", "Box Left", "左侧业务 ROI。", true},
      Port{"boxRight", "Box2D", "Box Right", "右侧业务 ROI。", true},
      Port{"refLine", "Line2D", "Ref Line",
           "圆心高度带的参考线，通常接 flush 的基准线。只有配了 centerTol 时才需要。", false},
      Port{"refLineRight", "Line2D", "Ref Line Right",
           "右侧圆心高度带单独的参考线。不接就沿用 refLine。缝两侧贴在不同的面上时"
           "（左圆贴基准面、右圆贴玻璃面）才需要分开接。",
           false},
  };
  op.outputs = {
      Port{"left", "Circle2D", "Left", "左圆。", true},
      Port{"right", "Circle2D", "Right", "右圆。", true},
      Port{"leftCloud", "PointCloud", "Left Cloud", "左圆真正拟合用的那片点。", true},
      Port{"rightCloud", "PointCloud", "Right Cloud", "右圆真正拟合用的那片点。", true},
      Port{"leftInliers", "Indices", "Left Inliers", "左圆内点，指向 leftCloud。", true},
      Port{"rightInliers", "Indices", "Right Inliers", "右圆内点，指向 rightCloud。", true},
      withExample(Port{"quality", "Record", "Quality",
                       "GapFitQualityPair：两侧各一份 GapFitQuality。", true},
                  examples::fitQualityPair()),
  };

  Param preferred;
  preferred.name = "preferredCamera";
  preferred.type = ParamType::Enum;
  preferred.label = "Preferred Camera";
  preferred.def = Value::text("Both");
  preferred.group = "Camera Fallback";
  preferred.options = {EnumOption{"Both", "Both", ""}, EnumOption{"Left", "Left（primary）", ""},
                       EnumOption{"Right", "Right（secondary）", ""}};

  op.params = {
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
      boolParam("rightRadiusFixed", "Right Fixed", false, "Right Radius", "固定右圆半径。"),
      numParam("rightRadiusValue", "Right Radius", 1.0, "mm", "Right Radius", "固定的右圆半径。"),
      numParam("rightRadiusMin", "Right Min", 0.3, "mm", "Right Radius", "右圆半径下限。"),
      numParam("rightRadiusMax", "Right Max", 1.8, "mm", "Right Radius", "右圆半径上限。"),
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
      bearingParam("leftArcBearingDeg", "Left Arc Bearing", "Left Radius"),
      bearingTolParam("leftArcBearingTolDeg", "Left Arc Bearing Tol", "Left Radius"),
      bearingParam("rightArcBearingDeg", "Right Arc Bearing", "Right Radius"),
      bearingTolParam("rightArcBearingTolDeg", "Right Arc Bearing Tol", "Right Radius"),
      minInliersParam("leftMinInliers", "Left Min Inliers", "Left Radius"),
      minArcParam("leftMinArcDeg", "Left Min Arc", "Left Radius"),
      minInliersParam("rightMinInliers", "Right Min Inliers", "Right Radius"),
      minArcParam("rightMinArcDeg", "Right Min Arc", "Right Radius"),
  };
  op.capabilities = {false, false, true};
  op.compute = &fitGapCircles;
  op.validate = &validateFitGapCircles;
  r.addOperator(std::move(op));
}

}  // namespace lyflow::packs::gap
