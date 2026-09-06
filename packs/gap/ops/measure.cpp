// 段差、间隙、判定。三个算子都很短 —— 真正的复杂度在上游的拟合里。
#include <cmath>
#include <limits>

#include "gap_detection/GapUtils.hpp"
#include "gap_ops.h"

namespace lyflow::packs::gap {
namespace {

namespace utils = ::detection::utils;

Eigen::VectorXf toLineCoefficients(const lyflow::Line2D& line) {
  Eigen::VectorXf c(6);
  c << line.point[0], line.point[1], 0.0f, line.dir[0], line.dir[1], 0.0f;
  return c;
}

lyflow::Line2D segmentOf(const Eigen::Vector2f& start, const Eigen::Vector2f& end) {
  lyflow::Line2D seg;
  seg.hasSegment = true;
  seg.start[0] = start.x();
  seg.start[1] = start.y();
  seg.end[0] = end.x();
  seg.end[1] = end.y();
  seg.point[0] = start.x();
  seg.point[1] = start.y();
  const Eigen::Vector2f d = end - start;
  const float n = d.norm();
  seg.dir[0] = n > 0 ? d.x() / n : 1.0f;
  seg.dir[1] = n > 0 ? d.y() / n : 0.0f;
  return seg;
}

Status flush(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext&) {
  const lyflow::Line2D& base = *inputs.get("baseLine").asLine2D();
  const lyflow::Point2D& ref = *inputs.get("refPoint").asPoint2D();
  const Eigen::Vector2f end(ref.p[0], ref.p[1]);
  Eigen::Vector2f start;
  // 法线强制朝 −y，返回的是带符号距离；最终值取绝对值再加 offset（§3.7）
  utils::pointLineDistance(toLineCoefficients(base), end, &start);
  const double valueMm = std::fabs(static_cast<double>((end - start).norm()) * kScale) +
                         params.number("offset");

  setMeasurement(outputs, "value", valueMm, true, {});
  outputs.set("segment", Data::line2d(segmentOf(start, end)));

  // 原算法在这里把垂足 start 并进基准线段（insertPoint2Segment），而 gap definition A
  // 的方向 u 取的正是**并过之后**的那条线段 —— 所以基准线要从这里再出一份。
  lyflow::Line2D extended = base;
  if (base.hasSegment) {
    Eigen::Matrix2f segment;
    segment << base.start[0], base.end[0], base.start[1], base.end[1];
    const Eigen::Matrix2f merged = utils::insertPoint2Segment(segment, start);
    extended.start[0] = merged(0, 0);
    extended.start[1] = merged(1, 0);
    extended.end[0] = merged(0, 1);
    extended.end[1] = merged(1, 1);
  }
  outputs.set("baseLine", Data::line2d(extended));
  return Status::Ok();
}

Status gapValue(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext&) {
  const lyflow::Circle2D& left = *inputs.get("left").asCircle2D();
  const lyflow::Circle2D& right = *inputs.get("right").asCircle2D();
  const double offset = params.number("offset");
  const bool definitionA = params.choice("definition") == "A";

  Eigen::Vector2f start;
  Eigen::Vector2f end;
  if (definitionA) {
    if (!inputs.has("baseLine")) {
      return Status::Error(Phase::Execute, "bad_input", "definition A 需要基准线", {}, "baseLine");
    }
    const lyflow::Line2D& base = *inputs.get("baseLine").asLine2D();
    if (!base.hasSegment) {
      return Status::Error(Phase::Execute, "bad_input",
                           "definition A 要的是带两个端点的基准线（与 ROI 框的交点）", {},
                           "baseLine");
    }
    Eigen::Vector2f u(base.end[0] - base.start[0], base.end[1] - base.start[1]);
    u.normalize();
    if (u.x() < 0) u = -u;  // 从左板指向右板
    const Eigen::Vector2f c1(left.center[0], left.center[1]);
    const Eigen::Vector2f c2(right.center[0], right.center[1]);
    const float gapA = (c2 - c1).dot(u) - left.radius - right.radius;
    if (!(gapA >= 0)) {
      return Status::Error(Phase::Execute, "invalid_geometry",
                           "definition A：两条切线交叉了（gap < 0）");
    }
    start = c1 + left.radius * u;
    end = start + gapA * u;
  } else {
    Eigen::VectorXf c1(3), c2(3);
    c1 << left.center[0], left.center[1], left.radius;
    c2 << right.center[0], right.center[1], right.radius;
    utils::circleCircleDistance(c1, c2, &start, &end);
  }

  // start.x > end.x 时原算法报 NAN 而不是一个负值（§3 与 getGap）
  const bool crossed = start.x() > end.x();
  const double valueMm =
      crossed ? std::numeric_limits<double>::quiet_NaN()
              : std::fabs(static_cast<double>((end - start).norm()) * kScale) + offset;
  setMeasurement(outputs, "value", valueMm, !crossed, crossed ? "两侧交叉，gap 取 NaN" : "");
  outputs.set("segment", Data::line2d(segmentOf(start, end)));
  return Status::Ok();
}

Status judge(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext&) {
  lyflow::Measurement m = *inputs.get("value").asMeasurement();
  const double nominal = params.number("nominal");
  const double upper = params.number("upper");
  const double lower = params.number("lower");
  const double margin = params.number("margin");
  const bool patrol = params.flag("patrol");

  m.hasLimits = true;
  m.nominal = nominal;
  m.upper = nominal + upper;
  m.lower = nominal + lower;
  if (!m.ok || !std::isfinite(m.value)) {
    m.verdict = "fail";
  } else if (m.value > m.upper) {
    m.verdict = patrol ? "margin" : "high";
  } else if (m.value < m.lower) {
    m.verdict = patrol ? "margin" : "low";
  } else if (margin > 0 && (m.value > m.upper - margin || m.value < m.lower + margin)) {
    m.verdict = "margin";
  } else {
    m.verdict = "ok";
  }
  outputs.set("value", Data::measurement(std::move(m)));
  return Status::Ok();
}

Param numParam(const char* name, const char* label, double def, const char* doc) {
  Param p;
  p.name = name;
  p.type = ParamType::Float;
  p.label = label;
  p.doc = doc;
  p.def = Value::number(def);
  p.unit = "mm";
  return p;
}

}  // namespace

void registerFlush(Registry& r) {
  OperatorDesc op;
  op.id = "gap.flush";
  op.version = "1.0.0";
  op.label = "Flush";
  op.category = "Gap/Measure";
  op.keywords = {"flush", "段差"};
  op.doc = "段差 = 参考点到基准线的距离，取绝对值再加 offset。符号在原算法里是死代码（§3.7）。";
  op.inputs = {
      Port{"baseLine", "Line2D", "Base Line", "基准面拟合出的直线。", true},
      Port{"refPoint", "Point2D", "Ref Point", "参考面那一侧取到的点。", true},
  };
  op.outputs = {
      Port{"value", "Measurement", "Value", "段差，毫米。", true},
      Port{"segment", "Line2D", "Segment", "垂足到参考点的那一段。", true},
      Port{"baseLine", "Line2D", "Base Line", "并进垂足之后的基准线段，gap definition A 用它。",
           true},
  };
  op.params = {numParam("offset", "Offset", 0.0, "加在绝对值上的偏置。")};
  op.capabilities = {false, true, true};
  op.compute = &flush;
  r.addOperator(std::move(op));
}

void registerGap(Registry& r) {
  OperatorDesc op;
  op.id = "gap.gap";
  op.version = "1.0.0";
  op.label = "Gap";
  op.category = "Gap/Measure";
  op.keywords = {"gap", "间隙"};
  op.doc =
      "间隙。definition B 是两圆的圆心连线距离；definition A 是沿基准面方向的两条切线之间的"
      "距离，需要基准线的两个端点定方向。";
  op.inputs = {
      Port{"left", "Circle2D", "Left", "左圆。", true},
      Port{"right", "Circle2D", "Right", "右圆。", true},
      Port{"baseLine", "Line2D", "Base Line", "definition A 必需：带端点的基准线。", false},
  };
  op.outputs = {
      Port{"value", "Measurement", "Value", "间隙，毫米。", true},
      Port{"segment", "Line2D", "Segment", "量到的那一段。", true},
  };

  Param definition;
  definition.name = "definition";
  definition.type = ParamType::Enum;
  definition.label = "Definition";
  definition.def = Value::text("B");
  definition.options = {EnumOption{"B", "B（圆心连线）", ""},
                        EnumOption{"A", "A（切线距离）", ""}};

  op.params = {definition, numParam("offset", "Offset", 0.0, "加在绝对值上的偏置。")};
  op.capabilities = {false, true, true};
  op.compute = &gapValue;
  r.addOperator(std::move(op));
}

void registerJudge(Registry& r) {
  OperatorDesc op;
  op.id = "gap.judge";
  op.version = "1.0.0";
  op.label = "Judge";
  op.category = "Gap/Measure";
  op.keywords = {"judge", "tolerance", "判定", "公差"};
  op.doc =
      "按标称值与上下偏差判定。margin 是「接近边界」的宽度，"
      "patrol 是巡检模式：超差只标 margin，不判 NG。";
  op.inputs = {Port{"value", "Measurement", "Value", "待判定的测量值。", true}};
  op.outputs = {Port{"value", "Measurement", "Value", "带判定字段的测量值。", true}};

  Param patrol;
  patrol.name = "patrol";
  patrol.type = ParamType::Bool;
  patrol.label = "Patrol";
  patrol.doc = "巡检模式：超差也只标 margin，不判 high/low。";
  patrol.def = Value::boolean(false);

  op.params = {
      numParam("nominal", "Nominal", 0.0, "标称值。"),
      numParam("upper", "Upper Deviation", 1.0, "上偏差（相对标称值）。"),
      numParam("lower", "Lower Deviation", -1.0, "下偏差（相对标称值，通常是负数）。"),
      numParam("margin", "Margin", 0.0, "离边界多近算 margin。0 = 不标。"),
      patrol,
  };
  op.capabilities = {false, true, true};
  op.compute = &judge;
  r.addOperator(std::move(op));
}

}  // namespace lyflow::packs::gap
