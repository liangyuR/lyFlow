// 段差、间隙、判定。三个算子都很短 —— 真正的复杂度在上游的拟合里。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <set>
#include <string>

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
  utils::pointLineDistance(toLineCoefficients(base), end, &start);
  // signed（默认）给带符号垂距：法线取 (dir.y, -dir.x)，点在基准线下方（y 更大）为负。
  // 关掉才取绝对值 —— 闭合缝上参考点几乎落在基准线上，绝对值会把响应折回去。
  double distanceMm = static_cast<double>((end - start).norm()) * kScale;
  if (params.flag("signed")) {
    Eigen::Vector2f normal(base.dir[1], -base.dir[0]);
    const float norm = normal.norm();
    if (norm > 0) {
      normal /= norm;
      distanceMm = static_cast<double>(normal.dot(end - start)) * kScale;
    }
  }
  const double valueMm = distanceMm * params.number("scale") + params.number("offset");

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

// ------------------------------------------------------------ 软装夹角（corner）

constexpr double kPi = 3.14159265358979323846;

/// 一条带端点的线段里离顶点远的那一端 —— 用来定「这条翼面从顶点往哪边走」。
/// 没有端点就退回 line.point，它总落在拟合用的那段点云中间。
Eigen::Vector2f farEndOf(const lyflow::Line2D& line, const Eigen::Vector2f& vertex) {
  if (!line.hasSegment) return Eigen::Vector2f(line.point[0], line.point[1]);
  const Eigen::Vector2f a(line.start[0], line.start[1]);
  const Eigen::Vector2f b(line.end[0], line.end[1]);
  return (a - vertex).squaredNorm() >= (b - vertex).squaredNorm() ? a : b;
}

std::string degText(double deg) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.1f", deg);
  return buf;
}

Status cornerVertex(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                    ExecContext& ctx) {
  const lyflow::Line2D& left = *inputs.get("lineLeft").asLine2D();
  const lyflow::Line2D& right = *inputs.get("lineRight").asLine2D();

  Eigen::Vector2f dL(left.dir[0], left.dir[1]);
  Eigen::Vector2f dR(right.dir[0], right.dir[1]);
  if (!(dL.norm() > 0) || !(dR.norm() > 0)) {
    return Status::Error(Phase::Execute, "bad_input", "翼面直线的方向是零向量");
  }
  dL.normalize();
  dR.normalize();

  // 两线求交。近平行时行列式趋零、交点飞到无穷远 —— 这是本算法唯一的退化模式。先在这里
  // 挡住，再由 minAngle/maxAngle 挡住「交点还在、但几何已经不对」的情形。
  const float det = dL.x() * dR.y() - dL.y() * dR.x();
  if (std::fabs(det) < 1e-6F) {
    return Status::Error(Phase::Execute, "invalid_geometry", "两条翼面线近乎平行，交点不成立");
  }
  const Eigen::Vector2f pL(left.point[0], left.point[1]);
  const Eigen::Vector2f pR(right.point[0], right.point[1]);
  const Eigen::Vector2f w = pR - pL;
  const Eigen::Vector2f vertex = pL + ((w.x() * dR.y() - w.y() * dR.x()) / det) * dL;

  // 夹角取「两条翼面各自从顶点出发的方向」之间的角，∈ (0, 180)。直接拿 dir 点乘不行：
  // dir 的正负是拟合给的，60° 的尖角会被算成 120°。
  Eigen::Vector2f aL = farEndOf(left, vertex) - vertex;
  Eigen::Vector2f aR = farEndOf(right, vertex) - vertex;
  double angleDeg = 0;
  if (aL.norm() > 0 && aR.norm() > 0) {
    aL.normalize();
    aR.normalize();
    angleDeg = std::acos(std::clamp(static_cast<double>(aL.dot(aR)), -1.0, 1.0)) * 180.0 / kPi;
  }
  const double minAngle = params.number("minAngle");
  const double maxAngle = params.number("maxAngle");
  if (angleDeg < minAngle || angleDeg > maxAngle) {
    return Status::Error(Phase::Execute, "invalid_geometry",
                         "夹角 " + degText(angleDeg) + "° 不在 [" + degText(minAngle) + ", " +
                             degText(maxAngle) + "] 之内");
  }

  // u：间隙的量取方向。与 gap.gap definition A 同一约定 —— 沿基准面、从基准件指向另一件。
  Eigen::Vector2f u = dL;
  if (inputs.has("baseLine")) {
    const lyflow::Line2D& base = *inputs.get("baseLine").asLine2D();
    const Eigen::Vector2f b(base.dir[0], base.dir[1]);
    if (b.norm() > 0) u = b;
  }
  u.normalize();
  if (u.x() < 0) u = -u;
  const Eigen::Vector2f nrm(-u.y(), u.x());

  // 金件顶点是模板坐标系里的常数（和四个业务 ROI 一样），用 ICP 变换搬到当前样本上。
  Eigen::Vector2f origin(mmToM(params.number("originX")), mmToM(params.number("originY")));
  bool aligned = false;
  if (inputs.has("alignment")) {
    const lyflow::Record* rec = inputs.get("alignment").asRecord();
    if (rec == nullptr || rec->type != "GapAlignment") {
      return Status::Error(Phase::Execute, "bad_input", "输入不是 GapAlignment", {}, "alignment");
    }
    if (rec->data.contains("left") && rec->data["left"].contains("transform")) {
      const Eigen::Matrix3f t = transformFromJson(rec->data["left"]["transform"]);
      const Eigen::Vector3f q = t * Eigen::Vector3f(origin.x(), origin.y(), 1.0F);
      origin = Eigen::Vector2f(q.x(), q.y());
      aligned = true;
    }
  }

  const Eigen::Vector2f delta = vertex - origin;
  const double scale = params.number("scale");
  const double alongU = mToMm(static_cast<double>(delta.dot(u)));
  const double alongN = mToMm(static_cast<double>(delta.dot(nrm)));
  const double gapMm = alongU * scale + params.number("gapOffset");
  const double flushMm = alongN * scale + params.number("flushOffset");

  lyflow::Point2D vp;
  vp.p[0] = vertex.x();
  vp.p[1] = vertex.y();
  outputs.set("vertex", Data::point2d(vp));
  setMeasurement(outputs, "gap", gapMm, true, {});
  setMeasurement(outputs, "flush", flushMm, true, {});

  lyflow::Measurement angle;
  angle.value = angleDeg;
  angle.ok = std::isfinite(angleDeg);
  angle.unit = "deg";
  outputs.set("angle", Data::measurement(std::move(angle)));
  outputs.set("segment", Data::line2d(segmentOf(origin, vertex)));

  lyflow::Record quality;
  quality.type = "GapCornerQuality";
  quality.data["vertexMm"] = {mToMm(vertex.x()), mToMm(vertex.y())};
  quality.data["originMm"] = {mToMm(origin.x()), mToMm(origin.y())};
  quality.data["deltaMm"] = {{"u", alongU}, {"n", alongN}};
  quality.data["angleDeg"] = angleDeg;
  quality.data["baseDirection"] = {u.x(), u.y()};
  quality.data["scale"] = scale;
  quality.data["alignmentApplied"] = aligned;
  outputs.set("quality", Data::record(std::move(quality)));

  ctx.log(LogLevel::Info, "corner: 顶点 (" + degText(mToMm(vertex.x())) + ", " +
                              degText(mToMm(vertex.y())) + ") mm，夹角 " + degText(angleDeg) +
                              "°");
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

std::vector<Issue> validateGap(const ParamView& params, const std::set<std::string>& connected) {
  std::vector<Issue> issues;
  if (params.choice("definition") == "A" && !connected.count("baseLine")) {
    issues.push_back(Issue::error("bad_param", "definition A 需要接基准线（baseLine）",
                                  "definition", "baseLine"));
  }
  return issues;
}

}  // namespace

void registerFlush(Registry& r) {
  OperatorDesc op;
  op.id = "gap.flush";
  op.version = "1.1.0";
  op.label = "面差";
  op.category = "间隙/测量";
  op.keywords = {"flush", "段差", "面差"};
  op.doc =
      "段差 = 参考点到基准线的带符号垂距 × scale + offset（signed 关掉时取绝对值）。"
      "基准线的方向完全来自上游拟合：基准面拟歪了，垂距连带 scale 一起错。";
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
  Param flushScale;
  flushScale.name = "scale";
  flushScale.type = ParamType::Float;
  flushScale.label = "Scale";
  flushScale.doc =
      "读数增益：value = 垂距 × scale + offset。量的是垂距，要别的方向（比如水平开口）"
      "就用它折算，算子本身不知道该往哪个方向折 —— 罗石 点 7 要的是水平开口，而基准线"
      "倾斜 29°，水平量 = 垂距 / |n_x| = 垂距 × 2.06。";
  flushScale.def = Value::number(1.0);

  Param flushSigned;
  flushSigned.name = "signed";
  flushSigned.type = ParamType::Bool;
  flushSigned.label = "Signed";
  flushSigned.doc =
      "输出带符号的垂距（法线 (dir.y, -dir.x)，参考点在基准线下方为负）。默认打开；"
      "关掉取绝对值，会把「缝张开」和「缝收紧」折成同一个方向 —— 参考点几乎落在基准线上的"
      "闭合缝尤其不能关。";
  flushSigned.def = Value::boolean(true);

  op.params = {numParam("offset", "Offset", 0.0, "加在距离上的偏置。"), flushScale, flushSigned};
  op.capabilities = {false, true, true};
  op.compute = &flush;
  r.addOperator(std::move(op));
}

void registerGap(Registry& r) {
  OperatorDesc op;
  op.id = "gap.gap";
  op.version = "1.0.0";
  op.label = "间隙";
  op.category = "间隙/测量";
  op.keywords = {"gap", "间隙"};
  op.doc =
      "间隙。definition B 是两圆的圆心连线距离；definition A 是沿基准面方向的两条切线之间的"
      "距离，需要接带两个端点的基准线定方向。\n"
      "两侧交叉时不给负值：definition B 输出 NaN，definition A 报 invalid_geometry；要带符号的量"
      "用 gap.flush 或 gap.point_offset。left 必须是 x 小的那一侧，接反读数直接交叉失效。";
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
  op.validate = &validateGap;
  r.addOperator(std::move(op));
}

void registerCornerVertex(Registry& r) {
  OperatorDesc op;
  op.id = "gap.corner_vertex";
  op.version = "1.0.0";
  op.label = "软装夹角";
  op.category = "间隙/测量";
  op.keywords = {"corner", "vertex", "夹角", "软装", "间隙"};
  op.doc =
      "两件软装贴合成一个夹角时的间隙。两侧翼面各给一条拟合直线，取交点作虚拟顶点 V，"
      "间隙 = V 相对金件顶点沿基准面方向的位移。相对「两侧圆拟合」的好处是 V 由几十个点的"
      "直线拟合决定，不依赖缝边那几个受遮挡影响最大的点。\n"
      "夹角接近 90° 时 V 只能沿基准线滑动，flush 与 gap 成定比、不可分辨 —— 这一路请用 "
      "gap.judge 的 patrol 模式只出值不判定。";
  op.inputs = {
      Port{"lineLeft", "Line2D", "Left Flank", "基准件翼面拟合出的直线。", true},
      Port{"lineRight", "Line2D", "Right Flank", "另一件翼面拟合出的直线。", true},
      Port{"baseLine", "Line2D", "Base Line", "基准面直线，只取方向定 u。缺省用左翼面。", false},
      withContract(
          Port{"alignment", "Record", "Alignment",
               "GapAlignment：把模板坐标系里的金件顶点搬到当前样本上。不接就当单位变换。", false},
          {{"recordType", "GapAlignment"}}),
  };
  op.outputs = {
      Port{"vertex", "Point2D", "Vertex", "两条翼面线的交点。", true},
      Port{"gap", "Measurement", "Gap", "间隙，毫米。", true},
      Port{"flush", "Measurement", "Flush", "段差，毫米。近 90° 夹角下不可观测。", true},
      Port{"angle", "Measurement", "Angle", "两条翼面的夹角，度。", true},
      Port{"segment", "Line2D", "Segment", "金件顶点到当前顶点的位移段。", true},
      withExample(
          Port{"quality", "Record", "Quality", "GapCornerQuality：顶点、位移分量、夹角。", true},
          examples::cornerQuality()),
  };

  Param scale;
  scale.name = "scale";
  scale.type = ParamType::Float;
  scale.label = "Scale";
  scale.doc =
      "读数增益：gap = Δ·u × scale + offset。顶点位移比真实间隙大一个与夹角有关的倍数，"
      "用金件加已知位移标定；没标定过的 gap 只是相对量。";
  scale.def = Value::number(1.0);

  Param minAngle;
  minAngle.name = "minAngle";
  minAngle.type = ParamType::Float;
  minAngle.label = "Min Angle";
  minAngle.doc =
      "夹角下限，超出即判失败。挡住近平行导致交点乱飞（两条线 |det| < 1e-6 也直接判失败）。";
  minAngle.def = Value::number(20.0);
  minAngle.unit = "deg";

  Param maxAngle = minAngle;
  maxAngle.name = "maxAngle";
  maxAngle.label = "Max Angle";
  maxAngle.doc = "夹角上限，超出即判失败。";
  maxAngle.def = Value::number(160.0);

  op.params = {
      numParam("originX", "Origin X", 0.0, "金件顶点在模板坐标系里的 X，毫米。"),
      numParam("originY", "Origin Y", 0.0, "金件顶点在模板坐标系里的 Y，毫米。"),
      scale,
      numParam("gapOffset", "Gap Offset", 0.0, "加在间隙上的偏置，用它让金件读 0。"),
      numParam("flushOffset", "Flush Offset", 0.0, "加在段差上的偏置。"),
      minAngle,
      maxAngle,
  };
  op.capabilities = {false, true, true};
  op.compute = &cornerVertex;
  r.addOperator(std::move(op));
}

void registerJudge(Registry& r) {
  OperatorDesc op;
  op.id = "gap.judge";
  op.version = "1.0.0";
  op.label = "判定";
  op.category = "间隙/测量";
  op.keywords = {"judge", "tolerance", "判定", "公差"};
  op.doc =
      "按标称值与上下偏差判定，不做任何统计；上游 ok=false 或值非有限一律判 fail。"
      "margin 是「接近边界」的宽度，patrol 是巡检模式：超差只标 margin，不判 NG —— "
      "那时 verdict 里分不出「接近边界」和「已经超差」。";
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
