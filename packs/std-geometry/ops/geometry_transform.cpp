#include "pose_common.h"

namespace lyflow::packs::std_geometry {
namespace {

using namespace pose_detail;

Status transform2D(const Inputs& in, const ParamView& p, Outputs& out, ExecContext&) {
  M4 matrix{};
  double scale = 1;
  std::string why;
  if (!similarity2D(in.get("transform").asTransform(), matrix, scale, false, why))
    return bad(why, "transform");
  const Unit2D expected = p.choice("unit") == "m" ? Unit2D::Meter : Unit2D::Pixel;
  const auto map = [&](const float* xy, float* target, bool direction = false) {
    const V3 value = pose_detail::apply(matrix, {xy[0], xy[1], 0}, direction);
    if (!nativeFloat(value[0]) || !nativeFloat(value[1]))
      return false;
    target[0] = static_cast<float>(value[0]);
    target[1] = static_cast<float>(value[1]);
    return true;
  };
  const Data& geometry = in.get("geometry");
  if (const auto* point = geometry.asPoint2D()) {
    if (point->unit != expected)
      return badParam("几何单位必须与矩阵平移单位一致", "unit");
    Point2D result = *point;
    if (!map(point->p, result.p))
      return bad("点坐标超出原生 float32 范围", "geometry");
    out.set("geometry", Data::point2d(result));
  } else if (const auto* line = geometry.asLine2D()) {
    if (line->unit != expected)
      return badParam("几何单位必须与矩阵平移单位一致", "unit");
    Line2D result = *line;
    if (!map(line->point, result.point))
      return bad("直线坐标超出 float32 范围", "geometry");
    V3 direction{};
    if (line->hasSegment) {
      if (!map(line->start, result.start) || !map(line->end, result.end))
        return bad("线段端点超出 float32 范围", "geometry");
      direction = {double(result.end[0]) - result.start[0], double(result.end[1]) - result.start[1],
                   0};
      result.point[0] = result.start[0];
      result.point[1] = result.start[1];
    } else {
      direction = pose_detail::apply(matrix, {line->dir[0], line->dir[1], 0}, true);
    }
    if (!normalize(direction))
      return bad("直线方向非法或变换后线段端点在float32精度下重合", "geometry");
    result.dir[0] = static_cast<float>(direction[0]);
    result.dir[1] = static_cast<float>(direction[1]);
    out.set("geometry", Data::line2d(result));
  } else if (const auto* circle = geometry.asCircle2D()) {
    if (circle->unit != expected)
      return badParam("几何单位必须与矩阵平移单位一致", "unit");
    const double radius = double(circle->radius) * scale;
    Circle2D result = *circle;
    if (!map(circle->center, result.center) || !nativeFloat(radius) || radius <= 0 ||
        static_cast<float>(radius) == 0)
      return bad("圆心或半径非法，或超出float32可表示范围", "geometry");
    result.radius = static_cast<float>(radius);
    out.set("geometry", Data::circle2d(result));
  } else {
    return bad("仅支持 Point2D、Line2D 和 Circle2D", "geometry");
  }
  return Status::Ok();
}

Status transformPlane(const Inputs& in, const ParamView&, Outputs& out, ExecContext&) {
  M4 m{};
  std::string why;
  if (!affine(in.get("transform").asTransform(), m, why))
    return bad(why, "transform");
  const Plane* plane = in.get("plane").asPlane();
  if (!plane || !std::isfinite(plane->d))
    return bad("需要有效平面", "plane");
  V3 normal{plane->normal[0], plane->normal[1], plane->normal[2]};
  const double oldNorm = std::hypot(normal[0], normal[1], normal[2]);
  if (!std::isfinite(oldNorm) || oldNorm == 0 || std::abs(oldNorm - 1) > 1e-5)
    return bad("平面需要单位法向", "plane");
  double scale = 0;
  for (int row = 0; row < 3; ++row)
    for (int col = 0; col < 3; ++col)
      scale = std::max(scale, std::abs(m[row * 4 + col]));
  if (scale == 0)
    return bad("仿射线性部分不可逆", "transform");
  poses::Matrix3 a{};
  for (int row = 0; row < 3; ++row)
    for (int col = 0; col < 3; ++col)
      a[row * 3 + col] = m[row * 4 + col] / scale;
  const double det = a[0] * (a[4] * a[8] - a[5] * a[7]) - a[1] * (a[3] * a[8] - a[5] * a[6]) +
                     a[2] * (a[3] * a[7] - a[4] * a[6]);
  if (!std::isfinite(det) || std::abs(det) <= 1e-12)
    return bad("仿射线性部分奇异或接近奇异（相对行列式≤1e-12）", "transform");
  const poses::Matrix3 cofactor{
      a[4] * a[8] - a[5] * a[7], a[5] * a[6] - a[3] * a[8], a[3] * a[7] - a[4] * a[6],
      a[2] * a[7] - a[1] * a[8], a[0] * a[8] - a[2] * a[6], a[1] * a[6] - a[0] * a[7],
      a[1] * a[5] - a[2] * a[4], a[2] * a[3] - a[0] * a[5], a[0] * a[4] - a[1] * a[3]};
  V3 transformed{};
  for (int i = 0; i < 3; ++i)
    transformed[i] = productSum({{cofactor[i * 3], normal[0]},
                                 {cofactor[i * 3 + 1], normal[1]},
                                 {cofactor[i * 3 + 2], normal[2]}}) /
                     (det * scale);
  const double length = std::hypot(transformed[0], transformed[1], transformed[2]);
  if (!std::isfinite(length) || length == 0)
    return bad("变换后的法向不可表示", "transform");
  const double d = productSum({{plane->d, 1},
                               {-transformed[0], m[3]},
                               {-transformed[1], m[7]},
                               {-transformed[2], m[11]}}) /
                   length;
  if (!nativeFloat(d))
    return bad("变换后的平面系数超出float32范围", "transform");
  Plane result;
  for (int i = 0; i < 3; ++i)
    result.normal[i] = static_cast<float>(transformed[i] / length);
  result.d = static_cast<float>(d);
  out.set("plane", Data::plane(result));
  return Status::Ok();
}

} // namespace

void registerGeometryTransforms(Registry& r) {
  OperatorDesc op = descriptor("geometry.transform_2d", "二维几何相似变换",
                               "2D point line circle transform similarity");
  Port input{"geometry", "Any", "Geometry", "Point2D/Line2D/Circle2D，输出保持同类型。", true};
  input.anyGroup = 1;
  Port output = input;
  op.inputs = {input,
               Port{"transform", "Transform", "Transform", "source→target 仿射矩阵。", true}};
  op.outputs = {output};
  op.params = {choice("unit", "矩阵平移单位", "m", {"m", "px"})};
  op.doc =
      "保留几何类型和单位，仅支持二维相似变换；圆不默默变椭圆。方向不带平移，有限线段端点和半径同步更新。";
  op.compute = &transform2D;
  r.addOperator(std::move(op));

  op = descriptor("geometry.transform_plane", "三维平面仿射变换",
                  "3D plane affine inverse transpose transform");
  op.inputs = {
      Port{"plane", "Plane", "Plane", "米制单位法向平面 n·p+d=0。", true},
      Port{"transform", "Transform", "Transform", "源→目标的可逆仿射矩阵，米制平移。", true}};
  op.outputs = {Port{"plane", "Plane", "Plane", "目标帧中的单位法向平面。", true}};
  op.doc =
      "法向使用逆转置，平移同步更新d后重新归一化；拒绝相对行列式≤1e-12、非有限或非仿射变换。不携带外部frame-tree协议。";
  op.compute = &transformPlane;
  r.addOperator(std::move(op));
}

} // namespace lyflow::packs::std_geometry
