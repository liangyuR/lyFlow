#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "gap_ops.h"
#include "std_bridge.hpp"

namespace lyflow::packs::gap {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

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
  double pointX = kNaN;
  double pointY = kNaN;
  double dirX = kNaN;
  double dirY = kNaN;
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

  double rms = kNaN;
  double maximum = kNaN;
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

lyflow::Line2D segmentOf(const Eigen::Vector2d& start, const Eigen::Vector2d& end) {
  lyflow::Line2D seg;
  seg.hasSegment = true;
  seg.start[0] = static_cast<float>(start.x());
  seg.start[1] = static_cast<float>(start.y());
  seg.end[0] = static_cast<float>(end.x());
  seg.end[1] = static_cast<float>(end.y());
  seg.point[0] = static_cast<float>(start.x());
  seg.point[1] = static_cast<float>(start.y());
  const Eigen::Vector2d d = end - start;
  const double n = d.norm();
  seg.dir[0] = n > 0 ? static_cast<float>(d.x() / n) : 1.0f;
  seg.dir[1] = n > 0 ? static_cast<float>(d.y() / n) : 0.0f;
  return seg;
}

struct WindowFit {
  std::size_t pointCount = 0;
  std::size_t inlierCount = 0;
  double y = kNaN;
  double k = kNaN;
  bool ok = false;
};

struct CameraFit {
  bool valid = false;
  WindowFit left;
  WindowFit right;
};

GapCloud windowOf(const GapCloud& cloud, double lo, double hi) {
  GapCloud out;
  for (const auto& p : cloud) {
    if (p.x > lo && p.x < hi) out.push_back(p);
  }
  out.width = static_cast<std::uint32_t>(out.size());
  out.height = 1;
  return out;
}

WindowFit fitWindow(const GapCloud& window, double xc, std::size_t minPoints, float distThresh) {
  WindowFit f;
  f.pointCount = window.size();
  if (f.pointCount < minPoints) return f;
  Eigen::VectorXf coefficients;
  pcl::Indices inliers;
  if (!gap_std::lineFit2D(window, &coefficients, &inliers, distThresh)) return f;
  if (coefficients.size() < 5) return f;
  double dx = coefficients[3];
  double dy = coefficients[4];
  const double norm = std::hypot(dx, dy);
  if (!(norm > 0)) return f;
  dx /= norm;
  dy /= norm;
  if (dx < 0) {
    dx = -dx;
    dy = -dy;
  }
  if (std::fabs(dx) < 1e-9) return f;
  f.k = dy / dx;
  f.y = static_cast<double>(coefficients[1]) + (xc - static_cast<double>(coefficients[0])) * f.k;
  f.inlierCount = inliers.size();
  f.ok = true;
  return f;
}

Eigen::VectorXf averagedCoefficients(double xc, double y, double k) {
  Eigen::VectorXf c(6);
  const double norm = std::hypot(1.0, k);
  c << static_cast<float>(xc), static_cast<float>(y), 0.0f, static_cast<float>(1.0 / norm),
      static_cast<float>(k / norm), 0.0f;
  return c;
}

pcl::Indices residualInliers(const GapCloud& cloud, const Eigen::VectorXf& line, double thresh) {
  pcl::Indices out;
  const double dx = line[3];
  const double dy = line[4];
  const double norm = std::hypot(dx, dy);
  if (!(norm > 0)) return out;
  for (std::size_t i = 0; i < cloud.size(); ++i) {
    const double residual =
        std::fabs((cloud[i].x - line[0]) * dy - (cloud[i].y - line[1]) * dx) / norm;
    if (residual <= thresh) out.push_back(static_cast<pcl::index_t>(i));
  }
  return out;
}

std::string mmText(double mm) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.3f", mm);
  return buf;
}

nlohmann::json windowJson(const WindowFit& w) {
  nlohmann::json j;
  j["pointCount"] = w.pointCount;
  j["inlierCount"] = w.inlierCount;
  j["yAtGrooveMm"] = numberOrNull(w.ok ? mToMm(w.y) : kNaN);
  j["slope"] = numberOrNull(w.ok ? w.k : kNaN);
  return j;
}

Status grooveJoint(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                   ExecContext& ctx) {
  const GapCloud primary = toPcl(*inputs.get("primary").asCloud());
  const GapCloud secondary = toPcl(*inputs.get("secondary").asCloud());

  GapCloud merged;
  merged.reserve(primary.size() + secondary.size());
  for (const auto& p : secondary) merged.push_back(p);
  for (const auto& p : primary) merged.push_back(p);
  std::stable_sort(merged.points.begin(), merged.points.end(),
                   [](const GapPoint& a, const GapPoint& b) { return a.x < b.x; });
  merged.width = static_cast<std::uint32_t>(merged.size());
  merged.height = 1;
  if (merged.size() < 20) {
    return Status::Error(Phase::Execute, "insufficient_points",
                         "两台相机合起来不到 20 个点，定位不了槽", {}, "primary");
  }

  const bool baseLeft = params.choice("baseSide") == "left";
  const double depthMinM = params.number("grooveDepthMin") / kScale;
  const auto deepestCount =
      static_cast<std::size_t>(std::max<std::int64_t>(1, params.integer("deepestCount")));
  const double leftNearM = params.number("leftNear") / kScale;
  const double leftFarM = params.number("leftFar") / kScale;
  const double rightNearM = params.number("rightNear") / kScale;
  const double rightFarM = params.number("rightFar") / kScale;
  const double lineDistMm = params.number("lineDistThresh");
  const double lineDistM = lineDistMm / kScale;
  const auto minLinePoints =
      static_cast<std::size_t>(std::max<std::int64_t>(2, params.integer("minLinePoints")));
  const double gapDepthMm = params.number("gapDepth");
  const double scale = params.number("scale");
  const double gapOffset = params.number("gapOffset");
  const double flushOffset = params.number("flushOffset");

  Eigen::VectorXf coarse;
  pcl::Indices coarseInliers;
  if (!gap_std::lineFit2D(merged, &coarse, &coarseInliers, mmToM(0.5)) || coarse.size() < 5) {
    return Status::Error(Phase::Execute, "surface_fit_failed", "整体粗拟合失败，量不出深度", {},
                         "primary");
  }
  Eigen::Vector2d n0(-static_cast<double>(coarse[4]), static_cast<double>(coarse[3]));
  const double n0Norm = n0.norm();
  if (!(n0Norm > 0)) {
    return Status::Error(Phase::Execute, "surface_fit_failed", "粗拟合出的方向是零向量", {},
                         "primary");
  }
  n0 /= n0Norm;
  if (n0.y() < 0) n0 = -n0;
  const Eigen::Vector2d p0(coarse[0], coarse[1]);

  std::vector<double> depth(merged.size());
  for (std::size_t i = 0; i < merged.size(); ++i) {
    depth[i] = (Eigen::Vector2d(merged[i].x, merged[i].y) - p0).dot(n0);
  }
  std::vector<std::size_t> deep;
  for (std::size_t i = 0; i < depth.size(); ++i) {
    if (depth[i] > depthMinM) deep.push_back(i);
  }
  if (deep.size() < 3) {
    return Status::Error(Phase::Execute, "groove_not_found",
                         "比粗拟合面深 " + mmText(params.number("grooveDepthMin")) +
                             " mm 的点不足 3 个，这里没有槽",
                         {}, "primary");
  }
  std::stable_sort(deep.begin(), deep.end(),
                   [&](std::size_t a, std::size_t b) { return depth[a] > depth[b]; });
  const std::size_t taken = std::min(deepestCount, deep.size());
  std::vector<double> xs;
  xs.reserve(taken);
  for (std::size_t i = 0; i < taken; ++i) xs.push_back(merged[deep[i]].x);
  std::sort(xs.begin(), xs.end());
  const double xc = (xs.size() % 2 == 1) ? xs[xs.size() / 2]
                                         : 0.5 * (xs[xs.size() / 2 - 1] + xs[xs.size() / 2]);
  const double grooveDepth = depth[deep[0]];
  const GapPoint& deepest = merged[deep[0]];

  const double leftLo = xc - leftFarM;
  const double leftHi = xc - leftNearM;
  const double rightLo = xc + rightNearM;
  const double rightHi = xc + rightFarM;

  const GapCloud* cameras[2] = {&primary, &secondary};
  CameraFit fits[2];
  for (int i = 0; i < 2; ++i) {
    const GapCloud leftWindow = windowOf(*cameras[i], leftLo, leftHi);
    const GapCloud rightWindow = windowOf(*cameras[i], rightLo, rightHi);
    fits[i].left.pointCount = leftWindow.size();
    fits[i].right.pointCount = rightWindow.size();
    if (leftWindow.size() < minLinePoints || rightWindow.size() < minLinePoints) continue;
    fits[i].left = fitWindow(leftWindow, xc, minLinePoints, static_cast<float>(lineDistM));
    fits[i].right = fitWindow(rightWindow, xc, minLinePoints, static_cast<float>(lineDistM));
    fits[i].valid = fits[i].left.ok && fits[i].right.ok;
  }

  double yLeft = 0;
  double kLeft = 0;
  double yRight = 0;
  double kRight = 0;
  int validCount = 0;
  for (const CameraFit& f : fits) {
    if (!f.valid) continue;
    yLeft += f.left.y;
    kLeft += f.left.k;
    yRight += f.right.y;
    kRight += f.right.k;
    ++validCount;
  }
  if (validCount == 0) {
    return Status::Error(Phase::Execute, "surface_fit_failed",
                         "两台相机在槽两侧的窗口里都拟合不出面", {}, "primary");
  }
  yLeft /= validCount;
  kLeft /= validCount;
  yRight /= validCount;
  kRight /= validCount;

  const double yBase = baseLeft ? yLeft : yRight;
  const double kBase = baseLeft ? kLeft : kRight;
  const double yRef = baseLeft ? yRight : yLeft;
  const double kRef = baseLeft ? kRight : kLeft;
  const double cosB = 1.0 / std::hypot(1.0, kBase);
  const double flushRawM = (yBase - yRef) * cosB;
  const double flushMm = mToMm(flushRawM) + flushOffset;

  const double yLow = std::max(yBase, yRef);
  const Eigen::Vector2d nB(kBase * cosB, -cosB);
  const Eigen::Vector2d pB(xc, yLow);
  const double levelM = gapDepthMm / kScale;
  constexpr std::size_t kNone = static_cast<std::size_t>(-1);
  std::size_t edgeLeft = kNone;
  std::size_t edgeRight = kNone;
  for (std::size_t i = 0; i < merged.size(); ++i) {
    const Eigen::Vector2d q(merged[i].x, merged[i].y);
    if (!((q - pB).dot(nB) > -levelM)) continue;
    if (q.x() < xc) {
      edgeLeft = i;
    } else if (q.x() > xc && edgeRight == kNone) {
      edgeRight = i;
    }
  }
  if (edgeLeft == kNone || edgeRight == kNone) {
    return Status::Error(Phase::Execute, "slot_edge_not_found",
                         "在低面下方 " + mmText(gapDepthMm) + " mm 的水平面上，槽的一侧没有边",
                         {}, "primary");
  }
  const double widthRawM = static_cast<double>(merged[edgeRight].x) - merged[edgeLeft].x;
  const double gapMm = mToMm(widthRawM) * scale + gapOffset;

  setMeasurement(outputs, "gap", gapMm, std::isfinite(gapMm), {});
  setMeasurement(outputs, "flush", flushMm, std::isfinite(flushMm), {});

  const auto lineFor = [&](double y, double k, double x0, double x1) {
    lyflow::Line2D line;
    line.point[0] = static_cast<float>(xc);
    line.point[1] = static_cast<float>(y);
    const double norm = std::hypot(1.0, k);
    line.dir[0] = static_cast<float>(1.0 / norm);
    line.dir[1] = static_cast<float>(k / norm);
    line.hasSegment = true;
    line.start[0] = static_cast<float>(x0);
    line.start[1] = static_cast<float>(y + (x0 - xc) * k);
    line.end[0] = static_cast<float>(x1);
    line.end[1] = static_cast<float>(y + (x1 - xc) * k);
    return line;
  };
  const lyflow::Line2D leftLine = lineFor(yLeft, kLeft, leftLo, leftHi);
  const lyflow::Line2D rightLine = lineFor(yRight, kRight, rightLo, rightHi);
  outputs.set("baseLine", Data::line2d(baseLeft ? leftLine : rightLine));
  outputs.set("refLine", Data::line2d(baseLeft ? rightLine : leftLine));
  outputs.set("gapSegment",
              Data::line2d(segmentOf(Eigen::Vector2d(merged[edgeLeft].x, merged[edgeLeft].y),
                                     Eigen::Vector2d(merged[edgeRight].x, merged[edgeRight].y))));
  outputs.set("flushSegment", Data::line2d(segmentOf(Eigen::Vector2d(xc, yBase),
                                                     Eigen::Vector2d(xc, yRef))));

  lyflow::Point2D groove;
  groove.p[0] = static_cast<float>(xc);
  groove.p[1] = deepest.y;
  outputs.set("groove", Data::point2d(groove));

  lyflow::Record quality;
  quality.type = "GapGrooveQuality";
  quality.data["grooveXMm"] = mToMm(xc);
  quality.data["grooveDepthMm"] = mToMm(grooveDepth);
  quality.data["deepestCount"] = taken;
  quality.data["baseSide"] = baseLeft ? "left" : "right";
  quality.data["flushRawMm"] = mToMm(flushRawM);
  quality.data["widthRawMm"] = mToMm(widthRawM);
  quality.data["levelDepthMm"] = gapDepthMm;
  quality.data["lowerSurface"] = yBase >= yRef ? "base" : "ref";
  quality.data["edgeLeftMm"] = {mToMm(merged[edgeLeft].x), mToMm(merged[edgeLeft].y)};
  quality.data["edgeRightMm"] = {mToMm(merged[edgeRight].x), mToMm(merged[edgeRight].y)};
  quality.data["base"] = {{"yAtGrooveMm", mToMm(yBase)}, {"slope", kBase}};
  quality.data["ref"] = {{"yAtGrooveMm", mToMm(yRef)}, {"slope", kRef}};
  static const char* kCameraNames[2] = {"primary", "secondary"};
  for (int i = 0; i < 2; ++i) {
    quality.data["cameras"][kCameraNames[i]] = {{"valid", fits[i].valid},
                                                {"left", windowJson(fits[i].left)},
                                                {"right", windowJson(fits[i].right)}};
  }
  quality.data["scale"] = scale;
  outputs.set("quality", Data::record(std::move(quality)));

  const GapCloud mergedLeft = windowOf(merged, leftLo, leftHi);
  const GapCloud mergedRight = windowOf(merged, rightLo, rightHi);
  const Eigen::VectorXf leftCoefficients = averagedCoefficients(xc, yLeft, kLeft);
  const Eigen::VectorXf rightCoefficients = averagedCoefficients(xc, yRight, kRight);
  const GapCloud& baseCloud = baseLeft ? mergedLeft : mergedRight;
  const GapCloud& refCloud = baseLeft ? mergedRight : mergedLeft;
  const Eigen::VectorXf& baseCoefficients = baseLeft ? leftCoefficients : rightCoefficients;
  const Eigen::VectorXf& refCoefficients = baseLeft ? rightCoefficients : leftCoefficients;

  lyflow::Record qualityBase;
  qualityBase.type = "GapFitQuality";
  qualityBase.data = lineQualityJson(baseCloud, baseCoefficients,
                                     residualInliers(baseCloud, baseCoefficients, lineDistM));
  outputs.set("qualityBase", Data::record(std::move(qualityBase)));

  lyflow::Record qualityRef;
  qualityRef.type = "GapFitQuality";
  qualityRef.data =
      lineQualityJson(refCloud, refCoefficients, residualInliers(refCloud, refCoefficients,
                                                                lineDistM));
  outputs.set("qualityRef", Data::record(std::move(qualityRef)));

  ctx.log(LogLevel::Info, "groove: 槽心 x " + mmText(mToMm(xc)) + " mm，深度 " +
                              mmText(mToMm(grooveDepth)) + " mm，两侧边 x " +
                              mmText(mToMm(merged[edgeLeft].x)) + " / " +
                              mmText(mToMm(merged[edgeRight].x)) + " mm，flush " +
                              mmText(flushMm) + " mm");
  return Status::Ok();
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

Param intParam(const char* name, const char* label, std::int64_t def, const char* group,
               const char* doc) {
  Param p;
  p.name = name;
  p.type = ParamType::Int;
  p.label = label;
  p.doc = doc;
  p.def = Value::integer(def);
  p.group = group;
  return p;
}

}  // namespace

void registerGrooveJoint(Registry& r) {
  OperatorDesc op;
  op.id = "gap.groove_joint";
  op.version = "1.0.0";
  op.label = "软装对接缝";
  op.category = "间隙/测量";
  op.keywords = {"groove", "slot", "seam", "对接缝", "软装", "间隙"};
  op.doc =
      "两件软装对接、中间留一条窄槽时的槽宽与面差。\n"
      "槽心由整体粗拟合面下方最深的那几个点定位（取它们 x 的中位数），不依赖模板与 ICP。\n"
      "两侧的面各在**避开圆角边**的窗口里拟合，且**每台相机分开拟合再平均** —— "
      "软材料靠近缝边时两台相机能差 0.1~0.3 mm，平均掉这份系统偏差比合并后一次拟合稳。\n"
      "flush = 参考面相对基准面在槽心处的垂距，+ = 参考面更高 / 更靠传感器。\n"
      "gap = 槽在「低面下方 gapDepth」这个水平面上的宽度：两侧最内侧的浅点之间的水平距离。"
      "gapDepth 必须落在两侧圆角/台阶之下、槽底之上 —— 罗石 点 2 实测可用带 ≈ 1.3~1.7 mm，"
      "太浅会咬到圆角把槽读宽，太深会掉进槽底把槽读窄。\n"
      "假定缝底有一条比两侧面都深的槽；缝闭合、两圆边直接相碰时没有这条槽，那种缝用 "
      "gap.notch_width。两台相机都凑不够 minLinePoints 时整帧失败。";
  op.inputs = {
      Port{"primary", "PointCloud", "Primary", "整体 ROI 裁过的 Master 云（测量帧）。", true},
      Port{"secondary", "PointCloud", "Secondary", "整体 ROI 裁过的 Slave 云（测量帧）。", true},
  };
  op.outputs = {
      Port{"gap", "Measurement", "Gap", "槽宽，毫米。", true},
      Port{"flush", "Measurement", "Flush", "面差，毫米。+ = 参考面更高。", true},
      Port{"baseLine", "Line2D", "Base Line", "平均后的基准面直线（带窗口两端）。", true},
      Port{"refLine", "Line2D", "Ref Line", "平均后的参考面直线（带窗口两端）。", true},
      Port{"gapSegment", "Line2D", "Gap Segment", "两个槽边点之间的那一段。", true},
      Port{"flushSegment", "Line2D", "Flush Segment", "槽心处基准面到参考面的那一段。", true},
      Port{"groove", "Point2D", "Groove", "槽心：x 取中位数，y 取最深点。", true},
      withExample(Port{"quality", "Record", "Quality",
                       "GapGrooveQuality：槽心、深度、两侧边、逐相机拟合。", true},
                  examples::grooveQuality()),
      withExample(Port{"qualityBase", "Record", "Quality Base",
                       "GapFitQuality：基准面窗口对平均线的残差。", true},
                  examples::fitQuality()),
      withExample(Port{"qualityRef", "Record", "Quality Ref",
                       "GapFitQuality：参考面窗口对平均线的残差。", true},
                  examples::fitQuality()),
  };

  Param baseSide;
  baseSide.name = "baseSide";
  baseSide.type = ParamType::Enum;
  baseSide.label = "Base Side";
  baseSide.doc = "哪一侧是基准件。flush 是另一侧相对它的高度。";
  baseSide.def = Value::text("left");
  baseSide.options = {EnumOption{"left", "Left（左侧为基准）", ""},
                      EnumOption{"right", "Right（右侧为基准）", ""}};

  Param scale;
  scale.name = "scale";
  scale.type = ParamType::Float;
  scale.label = "Scale";
  scale.doc = "读数增益：gap = 水平槽宽 × scale + gapOffset。";
  scale.def = Value::number(1.0);

  op.params = {
      baseSide,
      numParam("grooveDepthMin", "Groove Depth Min", 1.0, "mm", "槽定位",
               "比粗拟合面深多少才算槽里的点。"),
      intParam("deepestCount", "Deepest Count", 5, "槽定位",
               "取最深的几个点定槽心，x 取它们的中位数。"),
      numParam("leftNear", "Left Near", 1.5, "mm", "拟合窗口",
               "左窗口的内边：离槽心这么远才开始取点，避开圆角。"),
      numParam("leftFar", "Left Far", 6.0, "mm", "拟合窗口", "左窗口的外边。"),
      numParam("rightNear", "Right Near", 2.5, "mm", "拟合窗口", "右窗口的内边。"),
      numParam("rightFar", "Right Far", 7.0, "mm", "拟合窗口", "右窗口的外边。"),
      numParam("lineDistThresh", "Line Dist Thresh", 0.2, "mm", "拟合窗口",
               "面拟合的内点判定距离；qualityBase/qualityRef 的内点也按它算。"),
      intParam("minLinePoints", "Min Line Points", 8, "拟合窗口",
               "一台相机的一个窗口里少于这个点数，这台相机整个不参与平均。"),
      numParam("gapDepth", "Gap Depth", 1.4, "mm", "槽宽",
               "在低面下方多深的水平面上量槽宽。要在两侧圆角之下、槽底之上。"),
      scale,
      numParam("gapOffset", "Gap Offset", 0.0, "mm", "槽宽", "加在槽宽上的偏置。"),
      numParam("flushOffset", "Flush Offset", 0.0, "mm", "槽宽", "加在面差上的偏置。"),
  };
  op.capabilities = {false, true, true};
  op.compute = &grooveJoint;
  r.addOperator(std::move(op));
}

}  // namespace lyflow::packs::gap
