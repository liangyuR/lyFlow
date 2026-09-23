#include <cmath>
#include <cstdio>
#include <string>

#include "gap_ops.h"

namespace lyflow::packs::gap {
namespace {

std::string mmText(double mm) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.3f", mm);
  return buf;
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

Status pointOffset(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                   ExecContext& ctx) {
  const lyflow::Point2D& a = *inputs.get("a").asPoint2D();
  const lyflow::Point2D& b = *inputs.get("b").asPoint2D();
  const Eigen::Vector2f pa(a.p[0], a.p[1]);
  const Eigen::Vector2f pb(b.p[0], b.p[1]);

  const bool absDx = params.flag("absDx");
  const bool absDy = params.flag("absDy");
  const double scaleDx = params.number("scaleDx");
  const double scaleDy = params.number("scaleDy");
  const double dxOffset = params.number("dxOffset");
  const double dyOffset = params.number("dyOffset");

  double dx = static_cast<double>(pb.x() - pa.x()) * kScale * scaleDx;
  if (absDx) dx = std::fabs(dx);
  dx += dxOffset;

  double dy = static_cast<double>(pb.y() - pa.y()) * kScale * scaleDy;
  if (absDy) dy = std::fabs(dy);
  dy += dyOffset;

  const double distanceMm = static_cast<double>((pb - pa).norm()) * kScale;

  setMeasurement(outputs, "dx", dx, true, {});
  setMeasurement(outputs, "dy", dy, true, {});
  setMeasurement(outputs, "distance", distanceMm, true, {});
  outputs.set("segment", Data::line2d(segmentOf(pa, pb)));

  ctx.log(LogLevel::Info, "point_offset: dx " + mmText(dx) + " mm，dy " + mmText(dy) + " mm");
  return Status::Ok();
}

Param boolParam(const char* name, const char* label, bool def, const char* doc) {
  Param p;
  p.name = name;
  p.type = ParamType::Bool;
  p.label = label;
  p.doc = doc;
  p.def = Value::boolean(def);
  return p;
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

void registerPointOffset(Registry& r) {
  OperatorDesc op;
  op.id = "gap.point_offset";
  op.version = "1.0.0";
  op.label = "两点分量";
  op.category = "间隙/测量";
  op.keywords = {"offset", "dx", "dy", "两点", "分量", "面板边"};
  op.doc =
      "把 b 相对 a 的位移分解到测量帧的两个轴上：dx 沿 x（+ = b 在 a 右侧），"
      "dy 沿 y（+ = b 更远离传感器 / 更低）。用于面板边与翻边平台这类「缝是横着的」工况"
      "（罗石 点 5）：a 取立壁最低可见点、b 取平台外端点，dy 就是竖向开口、dx 就是横向错位。\n"
      "分量沿**测量帧坐标轴**，不沿任何拟合出来的面；零件整体转过一个角度，两个分量会一起变。"
      "a、b 由上游选点给定，本算子不检查它们是不是同一条缝的两侧。";
  op.inputs = {
      Port{"a", "Point2D", "A", "起点。", true},
      Port{"b", "Point2D", "B", "终点。", true},
  };
  op.outputs = {
      Port{"dx", "Measurement", "Dx", "b 相对 a 沿 x 的位移，毫米。", true},
      Port{"dy", "Measurement", "Dy", "b 相对 a 沿 y 的位移，毫米。", true},
      Port{"distance", "Measurement", "Distance", "a 到 b 的欧氏距离，毫米，恒 ≥ 0。", true},
      Port{"segment", "Line2D", "Segment", "a 到 b 的那一段。", true},
  };
  op.params = {
      boolParam("absDx", "Abs Dx", false,
                "输出 |dx|。正负两侧折成同一个方向，扰动灵敏度检查会跟着失效。"),
      boolParam("absDy", "Abs Dy", false,
                "输出 |dy|。正负两侧折成同一个方向，扰动灵敏度检查会跟着失效。"),
      numParam("dxOffset", "Dx Offset", 0.0, "加在 dx 上的偏置。"),
      numParam("dyOffset", "Dy Offset", 0.0, "加在 dy 上的偏置。"),
      numParam("scaleDx", "Scale Dx", 1.0, "dx 的读数增益。"),
      numParam("scaleDy", "Scale Dy", 1.0, "dy 的读数增益。"),
  };
  op.capabilities = {false, true, true};
  op.compute = &pointOffset;
  r.addOperator(std::move(op));
}

}  // namespace lyflow::packs::gap
