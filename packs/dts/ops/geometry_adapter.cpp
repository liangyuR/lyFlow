#include <cmath>

#include "dts_ops.h"

namespace lyflow::dts {
namespace {

Status compute(const Inputs& in, const ParamView& p, Outputs& out, ExecContext&) {
  if (p.choice("sourceContract") != "dts.geometry.v1")
    return Status::Error(Phase::Execute, "bad_param", "必须显式声明旧DTS毫米几何契约",
                         "sourceContract");
  const auto& producer = p.choice("sourceOp");
  const auto& value = in.get("geometry");
  auto bad = [] {
    return Status::Error(Phase::Execute, "bad_input",
                         "旧DTS几何类型或数值与声明不符；不能将像素或未知来源当作毫米", {},
                         "geometry");
  };
  if (producer == "dts.seal_root" || producer == "dts.flush") {
    const auto* source = value.asPoint2D();
    if (!source || source->unit != Unit2D::Meter || !std::isfinite(source->p[0]) ||
        !std::isfinite(source->p[1]))
      return bad();
    Point2D point = *source;
    for (auto& coordinate : point.p)
      coordinate = static_cast<float>(static_cast<double>(coordinate) * 0.001);
    out.set("geometry", Data::point2d(point));
  } else {
    const auto* source = value.asLine2D();
    if (!source || source->unit != Unit2D::Meter)
      return bad();
    Line2D line = *source;
    for (int i = 0; i < 2; ++i) {
      if (!std::isfinite(line.point[i]) || !std::isfinite(line.dir[i]) ||
          (line.hasSegment && (!std::isfinite(line.start[i]) || !std::isfinite(line.end[i]))))
        return bad();
      line.point[i] = static_cast<float>(static_cast<double>(line.point[i]) * 0.001);
      if (line.hasSegment) {
        line.start[i] = static_cast<float>(static_cast<double>(line.start[i]) * 0.001);
        line.end[i] = static_cast<float>(static_cast<double>(line.end[i]) * 0.001);
      }
    }
    if (std::hypot(line.dir[0], line.dir[1]) == 0)
      return bad();
    out.set("geometry", Data::line2d(line));
  }
  return Status::Ok();
}

} // namespace

void registerGeometryAdapter(Registry& r) {
  OperatorDesc op;
  op.id = "dts.geometry_to_metric";
  op.version = "1.0.0";
  op.label = "旧DTS几何显式米制适配";
  op.category = "DTS/数据适配";
  op.keywords = {"legacy", "DTS", "units", "millimeter", "胶条", "毫米", "米", "单位适配"};
  op.doc =
      "仅适配明确声明为dts.geometry.v1的毫米载荷，其旧Meter标签不能作为真实米制证据。"
      "按生产者选择点或线，将位置/端点乘0.001，方向不缩放；保留原XZ平面，不假装旋转到XY。"
      "不处理Measurement，后者继续按自身unit消费。M3图迁移必须按上游版本插入或移除此适配，不能重复换算。";
  Port input{"geometry", "Any", "旧几何", "由指定旧DTS节点产生的点或线。", true};
  input.anyGroup = 1;
  Port output = input;
  op.inputs = {input};
  op.outputs = {output};
  Param contract;
  contract.name = "sourceContract";
  contract.label = "明确的旧契约";
  contract.type = ParamType::Enum;
  contract.def = Value::text("unspecified");
  contract.options = {{"unspecified", "未声明"}, {"dts.geometry.v1", "DTS v1毫米几何"}};
  Param producer = contract;
  producer.name = "sourceOp";
  producer.label = "旧生产者";
  producer.def = Value::text("dts.seal_root");
  producer.options = {{"dts.seal_dome", "胶条顶点轴"},
                      {"dts.pick_metal", "钣金基准线"},
                      {"dts.seal_root", "胶条根部点"},
                      {"dts.flush", "面差垂足"}};
  op.params = {contract, producer};
  op.compute = &compute;
  r.addOperator(std::move(op));
}

} // namespace lyflow::dts
