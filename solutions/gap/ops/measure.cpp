// 段差、间隙、判定。三个算子都很短 —— 真正的复杂度在上游的拟合里。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>

#include "gap_ops.h"
#include "profile_inspection.h"
#include "lyflow/op_params.h"
#include "lyflow/kernel/geometry.h"

namespace lyflow::packs::gap {
namespace {

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

// ------------------------------------------------------------ 软装夹角（corner）

constexpr double kPi = 3.14159265358979323846;

/// 一条带端点的线段里离顶点远的那一端 —— 用来定「这条翼面从顶点往哪边走」。
/// 没有端点就退回 line.point，它总落在拟合用的那段点云中间。
Eigen::Vector2f farEndOf(const lyflow::Line2D& line, const Eigen::Vector2f& vertex) {
  if (!line.hasSegment)
    return Eigen::Vector2f(line.point[0], line.point[1]);
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
                    ExecContext& ctx, const Point2D& explicitOrigin) {
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
  auto wingLeft = left;
  auto wingRight = right;
  wingLeft.hasSegment = wingRight.hasSegment = false;
  kernel::Intersections2D intersection;
  kernel::GeometryIssue issue;
  if (!kernel::intersect2D(wingLeft, wingRight, 1e-6, intersection, issue) ||
      intersection.points.size() != 1)
    return Status::Error(Phase::Execute, "invalid_geometry", "两条翼面线没有可表示的顶点");
  const auto intersectionPoint = intersection.points.front();
  const Eigen::Vector2f vertex(static_cast<float>(intersectionPoint.x),
                               static_cast<float>(intersectionPoint.y));

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
    if (b.norm() > 0)
      u = b;
  }
  u.normalize();
  if (u.x() < 0)
    u = -u;
  const Eigen::Vector2f nrm(-u.y(), u.x());

  const Eigen::Vector2f origin(explicitOrigin.p[0], explicitOrigin.p[1]);

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
  outputs.set("quality", Data::record(std::move(quality)));

  ctx.log(LogLevel::Info, "corner: 顶点 (" + degText(mToMm(vertex.x())) + ", " +
                              degText(mToMm(vertex.y())) + ") mm，夹角 " + degText(angleDeg) + "°");
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

} // namespace

namespace profile_scene {
std::vector<Param> cornerInspectionParams() {
  Param scale;
  scale.name = "scale";
  scale.type = ParamType::Float;
  scale.label = "Scale";
  scale.doc = "读数增益：gap = Δ·u × scale + offset。顶点位移比真实间隙大一个与夹角有关的倍数，"
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

  return {
      scale,
      numParam("gapOffset", "Gap Offset", 0.0, "加在间隙上的偏置，用它让金件读 0。"),
      numParam("flushOffset", "Flush Offset", 0.0, "加在段差上的偏置。"),
      minAngle,
      maxAngle,
  };
}
namespace inspection_detail {
Status cornerGeometry(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx,
                      const Point2D* origin) {
  return cornerVertex(in, p, out, ctx, *origin);
}
}
}

void registerPatrolPolicy(Registry& r) {
  OperatorDesc op;
  op.id = "gap.apply_patrol";
  op.version = "1.0.0";
  op.label = "间隙面差巡检判定";
  op.category = "间隙/测量";
  op.doc = "读取公差比较结果。巡检时将 high/low 标为 margin，无效量测标为 fail。";
  op.inputs = {{"value", "Measurement", "公差比较", "接收标准量测公差比较。", true}};
  op.outputs = {{"value", "Measurement", "判定结果", "保持读数、单位和限值。", true}};
  op.params = {op_params::boolParam("patrol", "巡检", false, "巡检超差时仅标 margin。")};
  op.capabilities = {false, true, true};
  op.compute = [](const Inputs& in, const ParamView& params, Outputs& out, ExecContext&) {
    Measurement result = *in.get("value").asMeasurement();
    if (!result.ok || !std::isfinite(result.value))
      result.verdict = "fail";
    else if (!result.hasLimits || result.verdict.empty())
      return op_params::badInput("需要已完成的公差比较结果", "value");
    else if (params.flag("patrol") && (result.verdict == "high" || result.verdict == "low"))
      result.verdict = "margin";
    out.set("value", Data::measurement(std::move(result)));
    return Status::Ok();
  };
  r.addOperator(std::move(op));
}

} // namespace lyflow::packs::gap
