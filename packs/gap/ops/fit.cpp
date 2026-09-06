// 直线拟合与间隙两侧的圆拟合。两者都直接调 detection::utils 里的拟合函数（G2）。
#include <algorithm>
#include <cmath>
#include <limits>

#include "gap_detection/GapUtils.hpp"
#include "gap_ops.h"
#include "std_bridge.hpp"

namespace lyflow::packs::gap {
namespace {

namespace utils = ::detection::utils;

lyflow::Line2D lineFromCoefficients(const Eigen::VectorXf& c) {
  lyflow::Line2D line;
  line.point[0] = c[0];
  line.point[1] = c[1];
  const float nx = std::hypot(c[3], c[4]);
  line.dir[0] = nx > 0 ? c[3] / nx : 1.0f;
  line.dir[1] = nx > 0 ? c[4] / nx : 0.0f;
  return line;
}

Status fitLine(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext& ctx) {
  const lyflow::PointCloud& in = *inputs.get("cloud").asCloud();
  const lyflow::Box2D& box = *inputs.get("box").asBox2D();
  const GapCloud cloud = toPcl(in);
  if (cloud.size() < 2) {
    return Status::Error(Phase::Execute, "insufficient_points", "点太少，拟合不了直线", {},
                         "cloud");
  }
  const float dist = mmToM(params.number("distThresh"));
  const auto segmentPoints = static_cast<std::size_t>(params.integer("segmentPoints"));
  const bool isLeft = params.choice("side") == "left";
  const std::string lineType = params.choice("lineType");

  Eigen::VectorXf coefficients;
  pcl::Indices indices;
  const bool ok = lineType == "fit"
                      ? gap_std::lineFit2D(cloud, &coefficients, &indices, dist)
                      : gap_std::lineFit2D(cloud, &coefficients, &indices,
                                           lineType == "vertical" ? "vertical line"
                                                                  : "horizontal line",
                                           dist);
  if (!ok) {
    return Status::Error(Phase::Execute, "line_fit_failed", "直线拟合失败", {}, "cloud");
  }

  // 截取靠缝隙那一端再拟合一次。方向判据见 §3.5：ascend == isLeft 留尾巴，否则留头。
  if (segmentPoints < indices.size()) {
    const bool ascend = cloud[0].x <= cloud.back().x;
    if (ascend == isLeft) {
      indices.erase(indices.begin(), indices.end() - static_cast<std::ptrdiff_t>(segmentPoints));
    } else {
      indices.resize(segmentPoints);
    }
    const GapCloud segment(cloud, indices);
    pcl::Indices second;
    if (!gap_std::lineFit2D(segment, &coefficients, &second, dist / 3)) {
      return Status::Error(Phase::Execute, "line_fit_failed", "截取之后的第二次直线拟合失败", {},
                           "cloud");
    }
    std::size_t j = 0;
    for (auto i : second) indices[j++] = indices[static_cast<std::size_t>(i)];
    indices.resize(second.size());
  }
  if (indices.empty()) {
    return Status::Error(Phase::Execute, "line_fit_failed", "拟合之后一个内点都没有", {}, "cloud");
  }

  lyflow::Line2D line = lineFromCoefficients(coefficients);
  // 端点两种口径，对应 align_cloud_ 的两支（GapDetection.cpp:861）：真取与 ROI 框的交点，
  // 假取首尾内点的真实云点。模型路径恒为假，而 definition A 的 u 就取自这两个端点。
  if (params.choice("endpoints") == "inlier_ends") {
    const GapPoint& a = cloud[indices.front()];
    const GapPoint& b = cloud[indices.back()];
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

  // getEndPointofCloud 会就地排序 indices，所以内点输出也用排序后的那一份
  const GapPoint inner = utils::getEndPointofCloud(cloud, !isLeft, &indices);
  lyflow::Point2D end;
  end.p[0] = inner.x;
  end.p[1] = inner.y;
  outputs.set("innerEnd", Data::point2d(end));

  lyflow::Indices out;
  out.sourceCloudId = in.id;
  out.values.reserve(indices.size());
  for (auto i : indices) out.values.push_back(static_cast<std::int32_t>(i));
  outputs.set("inliers", Data::indices(std::move(out)));
  return Status::Ok();
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
};

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

  constexpr std::size_t kMinimumCameraInliers = 8;
  const double maxRadiusDifference = 0.25 / kScale;
  const double maxCenterDifference = 0.75 / kScale;

  SideResult sides[2];
  for (int i = 0; i < 2; ++i) {
    SideResult& side = sides[i];
    const Eigen::Matrix2f roi = toRoiMatrix(boxes[i]);
    side.cloud = cropStrict(merged, roi);
    if (side.cloud.empty()) {
      return Status::Error(Phase::Execute, "roi_empty",
                           std::string(i == 0 ? "左" : "右") + "间隙 ROI 里没有点", {},
                           i == 0 ? "boxLeft" : "boxRight");
    }
    if (!(cfg[i].rMax > cfg[i].rMin)) {
      return Status::Error(Phase::Execute, "bad_param", "半径上限必须大于下限",
                           i == 0 ? "leftRadiusMax" : "rightRadiusMax");
    }
    if (cfg[i].rFixed > 0 &&
        !(cfg[i].rMax > cfg[i].rFixed && cfg[i].rMin < cfg[i].rFixed)) {
      return Status::Error(Phase::Execute, "bad_param", "固定半径必须落在上下限之间",
                           i == 0 ? "leftRadiusValue" : "rightRadiusValue");
    }

    side.fitted = gap_std::circleFit2D(side.cloud, &side.circle, &side.indices, distThresh,
                                   cfg[i].rMin, cfg[i].rMax, cfg[i].rFixed);
    if (!side.fitted && retryDistanceMm > 0) {
      side.indices.clear();
      side.fitted = gap_std::circleFit2D(side.cloud, &side.circle, &side.indices,
                                     mmToM(retryDistanceMm), cfg[i].rMin, cfg[i].rMax,
                                     cfg[i].rFixed);
    }
    if (side.fitted || !fallback) continue;

    // 相机分开拟合的回退：两台相机各自的 ROI 点各拟合一个圆。
    GapCloud primaryRoi = cropStrict(primary, roi);
    GapCloud secondaryRoi = cropStrict(secondary, roi);
    // select_closest_nominal 打开时强制自由半径（§3.9）
    const double separatedFixed = selectClosestNominal ? 0.0 : cfg[i].rFixed;
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
        // 候选打分永远用 definition B 的圆心距，即使最终按 definition A 算（G8）
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

}  // namespace

void registerFitLine(Registry& r) {
  OperatorDesc op;
  op.id = "gap.fit_line";
  op.version = "1.0.0";
  op.label = "Fit Line";
  op.category = "Gap/Fit";
  op.keywords = {"line", "ransac", "直线", "拟合"};
  op.doc =
      "在 ROI 里拟合一条直线：先整体拟合，再截取靠缝隙那一端的 segmentPoints 个内点、"
      "用 1/3 的阈值重拟合一次（§3.5）。line 带与 ROI 框的两个交点作端点。";
  op.inputs = {
      Port{"cloud", "PointCloud", "Cloud", "已经按业务 ROI 裁过的点云。", true},
      Port{"box", "Box2D", "Box", "同一个业务 ROI，用来求端点。", true},
  };
  op.outputs = {
      Port{"line", "Line2D", "Line", "拟合出的直线（带端点）。", true},
      Port{"inliers", "Indices", "Inliers", "内点下标，指向输入点云。", true},
      Port{"innerEnd", "Point2D", "Inner End", "内点里靠缝隙那一端的真实云点。", true},
  };

  Param side;
  side.name = "side";
  side.type = ParamType::Enum;
  side.label = "Side";
  side.doc = "这条线在缝隙的哪一侧。决定截取哪一头，以及 innerEnd 取哪一端。";
  side.def = Value::text("left");
  side.options = {EnumOption{"left", "Left", ""}, EnumOption{"right", "Right", ""}};

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
  distThresh.doc = "内点判定距离（line_fit_distance）。";
  distThresh.def = Value::number(0.1);
  distThresh.unit = "mm";

  Param segmentPoints;
  segmentPoints.name = "segmentPoints";
  segmentPoints.type = ParamType::Int;
  segmentPoints.label = "Segment Points";
  segmentPoints.doc = "截取多少个内点做第二次拟合。比内点还多就不截。";
  segmentPoints.def = Value::integer(500);

  Param endpoints;
  endpoints.name = "endpoints";
  endpoints.type = ParamType::Enum;
  endpoints.label = "Endpoints";
  endpoints.doc =
      "line 的两个端点怎么取。模板路径是 ROI 交点，模型路径是首尾内点 —— "
      "原算法按 align_cloud_ 分这两支，gap definition A 的方向 u 就取自它们。";
  endpoints.def = Value::text("roi_intersection");
  endpoints.options = {EnumOption{"roi_intersection", "ROI 交点", "直线与 ROI 框的两个交点。"},
                       EnumOption{"inlier_ends", "首尾内点", "第一个与最后一个内点的真实云点。"}};

  op.params = {side, lineType, distThresh, segmentPoints, endpoints};
  op.capabilities = {false, false, true};
  op.compute = &fitLine;
  r.addOperator(std::move(op));
}

void registerFitGapCircles(Registry& r) {
  OperatorDesc op;
  op.id = "gap.fit_gap_circles";
  op.version = "1.0.0";
  op.label = "Fit Gap Circles";
  op.category = "Gap/Fit";
  op.keywords = {"circle", "ransac", "圆", "拟合", "间隙"};
  op.doc =
      "间隙两侧的圆拟合。先在合并云的 ROI 里拟合；失败按 retryDistance 再试一次；"
      "还失败就退到「两台相机各拟合一个」，两台都合格时按 |gap − nominal| 二选一（§3.9）。";
  op.inputs = {
      Port{"merged", "PointCloud", "Merged", "合并并滤波之后的云。", true},
      Port{"primary", "PointCloud", "Primary", "整体 ROI 裁过、**未**滤波的 Master 云。", true},
      Port{"secondary", "PointCloud", "Secondary", "整体 ROI 裁过、**未**滤波的 Slave 云。", true},
      Port{"boxLeft", "Box2D", "Box Left", "左侧业务 ROI。", true},
      Port{"boxRight", "Box2D", "Box Right", "右侧业务 ROI。", true},
  };
  op.outputs = {
      Port{"left", "Circle2D", "Left", "左圆。", true},
      Port{"right", "Circle2D", "Right", "右圆。", true},
      Port{"leftCloud", "PointCloud", "Left Cloud", "左圆真正拟合用的那片点。", true},
      Port{"rightCloud", "PointCloud", "Right Cloud", "右圆真正拟合用的那片点。", true},
      Port{"leftInliers", "Indices", "Left Inliers", "左圆内点，指向 leftCloud。", true},
      Port{"rightInliers", "Indices", "Right Inliers", "右圆内点，指向 rightCloud。", true},
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
      numParam("nominal", "Nominal", 0.0, "mm", "", "间隙标称值，挑相机候选时用。"),
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
                "合并云拟合失败时按相机分开再试。"),
      boolParam("selectClosestNominal", "Select Closest Nominal", true, "Camera Fallback",
                "两台相机都合格时按 |gap − nominal| 挑；打开时强制自由半径。"),
      preferred,
  };
  op.capabilities = {false, false, true};
  op.compute = &fitGapCircles;
  r.addOperator(std::move(op));
}

}  // namespace lyflow::packs::gap
