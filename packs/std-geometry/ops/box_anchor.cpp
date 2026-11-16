#include <cmath>
#include <limits>

#include "lyflow/operator.h"
#include "lyflow/registry.h"

namespace lyflow::packs::std_geometry {
namespace {
Status boxAnchor(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                 ExecContext& ctx) {
  const auto* box = inputs.get("box").asBox2D();
  if (!box || (box->unit != Unit2D::Meter && box->unit != Unit2D::Pixel))
    return Status::Error(Phase::Execute, "bad_input", "需要米制或像素Box2D", {}, "box");
  const auto& anchor = params.choice("anchor");
  if (anchor != "minimum" && anchor != "center" && anchor != "maximum")
    return Status::Error(Phase::Execute, "bad_param", "框参考点类型非法", "anchor");
  Point2D point;
  point.unit = box->unit;
  for (int i = 0; i < 2; ++i) {
    if (!std::isfinite(box->min[i]) || !std::isfinite(box->max[i]) || box->min[i] > box->max[i])
      return Status::Error(Phase::Execute, "bad_input", "框范围必须有限且有序", {}, "box");
    const double value = anchor == "minimum" ? box->min[i]
                         : anchor == "maximum"
                             ? box->max[i]
                             : 0.5 * (static_cast<double>(box->min[i]) + box->max[i]);
    if (!std::isfinite(value) || std::fabs(value) > std::numeric_limits<float>::max())
      return Status::Error(Phase::Execute, "bad_input", "框参考点超出有限float范围", {}, "box");
    point.p[i] = static_cast<float>(value);
  }
  if (ctx.cancelled())
    return Status::Error(Phase::Execute, "cancelled", "框参考点提取已取消");
  outputs.set("point", Data::point2d(point));
  return Status::Ok();
}
}
void registerBoxAnchor(Registry& registry) {
  OperatorDesc op;
  op.id = "geometry.box_anchor";
  op.version = "1.0.0";
  op.label = "矩形参考点提取";
  op.category = "几何/构造";
  op.keywords = {"box", "anchor", "minimum", "center", "矩形", "中心", "参考点"};
  op.inputs = {{"box", "Box2D", "矩形", "有限有序范围，单位原样保留。", true}};
  op.outputs = {{"point", "Point2D", "参考点", "矩形最小角、中心或最大角。", true}};
  Param anchor;
  anchor.name = "anchor";
  anchor.label = "参考点";
  anchor.type = ParamType::Enum;
  anchor.def = Value::text("minimum");
  anchor.options = {{"minimum", "最小角"}, {"center", "中心"}, {"maximum", "最大角"}};
  op.params = {anchor};
  op.capabilities.deterministic = true;
  op.capabilities.cancellable = true;
  op.doc = "纯几何坐标提取，保持米/像素单位；中心以double计算后写入Point2D，不声明Profile来源。";
  op.compute = &boxAnchor;
  registry.addOperator(std::move(op));
}
}
