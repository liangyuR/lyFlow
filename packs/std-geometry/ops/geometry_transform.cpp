#include "pose_common.h"
#include "lyflow/kernel/geometry.h"
#include <type_traits>

namespace lyflow::packs::std_geometry {
namespace {

using namespace pose_detail;

Status transform2D(const Inputs& in, const ParamView& p, Outputs& out, ExecContext&) {
  const auto* transform = in.get("transform").asTransform();
  M4 matrix;
  double scale = 1;
  std::string why;
  if (!similarity2D(transform, matrix, scale, false, why))
    return bad(why, "transform");
  kernel::Geometry2D geometry;
  const auto& data = in.get("geometry");
  if (const auto* value = data.asPoint2D())
    geometry = *value;
  else if (const auto* value = data.asLine2D())
    geometry = *value;
  else if (const auto* value = data.asCircle2D())
    geometry = *value;
  else
    return bad("仅支持 Point2D、Line2D 和 Circle2D", "geometry");
  kernel::Geometry2D result;
  kernel::GeometryIssue issue;
  if (!kernel::transformGeometry2D(geometry, *transform,
                                   p.choice("unit") == "m" ? Unit2D::Meter : Unit2D::Pixel, result,
                                   issue))
    return issue.parameter ? badParam(issue.message, "unit")
                           : bad(issue.message, issue.operand == 1 ? "transform" : "geometry");
  std::visit(
      [&out](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, Point2D>)
          out.set("geometry", Data::point2d(value));
        else if constexpr (std::is_same_v<T, Line2D>)
          out.set("geometry", Data::line2d(value));
        else
          out.set("geometry", Data::circle2d(value));
      },
      result);
  return Status::Ok();
}
Status transformPlane(const Inputs& in, const ParamView&, Outputs& out, ExecContext&) {
  const auto* transform = in.get("transform").asTransform();
  M4 matrix;
  std::string why;
  if (!affine(transform, matrix, why))
    return bad(why, "transform");
  const auto* plane = in.get("plane").asPlane();
  if (!plane)
    return bad("需要有效平面", "plane");
  Plane result;
  kernel::GeometryIssue issue;
  if (!kernel::transformPlane(*plane, *transform, result, issue))
    return bad(issue.message, issue.operand == 1 ? "transform" : "plane");
  out.set("plane", Data::plane(result));
  return Status::Ok();
}

} // namespace

void registerGeometryTransforms(Registry& r) {
  OperatorDesc op = descriptor("geometry.transform_2d", "二维几何相似变换",
                               "2D point line circle transform similarity");
  op.category = "几何/变换";
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
  op.category = "几何/变换";
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
