#include <cmath>

#include "dts_ops.h"

namespace lyflow::dts {
namespace {

nlohmann::json migrateLegacyGeometry(const nlohmann::json& params) {
  if (!params.is_object() || params.value("sourceContract", "") != "dts.geometry.v1")
    throw std::invalid_argument("旧DTS几何适配需要明确的dts.geometry.v1来源声明");
  auto migrated = params;
  const auto producer = params.value("sourceOp", "dts.seal_root");
  if (producer != "dts.seal_dome" && producer != "dts.pick_metal" && producer != "dts.seal_root" &&
      producer != "dts.flush")
    throw std::invalid_argument("旧DTS几何生产者不受支持");
  migrated["sourceOp"] = producer;
  return migrated;
}

Status compute(const Inputs& in, const ParamView& p, Outputs& out, ExecContext&) {
  const bool legacy = p.choice("sourceContract") == "dts.geometry.v1";
  if (!legacy && p.choice("sourceContract") != "dts.geometry.v2")
    return Status::Error(Phase::Execute, "bad_param", "必须显式声明DTS几何来源契约",
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
    out.set("geometry", Data::point2d(legacy ? metricGeometry(*source) : *source));
  } else {
    const auto* source = value.asLine2D();
    if (!source || source->unit != Unit2D::Meter)
      return bad();
    for (int i = 0; i < 2; ++i)
      if (!std::isfinite(source->point[i]) || !std::isfinite(source->dir[i]) ||
          (source->hasSegment &&
           (!std::isfinite(source->start[i]) || !std::isfinite(source->end[i]))))
        return bad();
    if (std::hypot(source->dir[0], source->dir[1]) == 0)
      return bad();
    out.set("geometry", Data::line2d(legacy ? metricGeometry(*source) : *source));
  }
  return Status::Ok();
}

} // namespace

void registerGeometryAdapter(Registry& r) {
  OperatorDesc op;
  op.id = "dts.geometry_to_metric";
  op.version = "2.0.0";
  op.label = "DTS几何显式契约适配";
  op.category = "DTS/数据适配";
  op.keywords = {"legacy", "DTS", "units", "millimeter", "胶条", "毫米", "米", "单位适配"};
  op.doc = "仅适配明确声明为dts.geometry.v1的毫米载荷，其旧Meter标签不能作为真实米制证据。"
           "按生产者选择点或线，将位置/端点乘0.001，方向不缩放；保留原XZ平面，不假装旋转到XY。"
           "dts.geometry.v2明确声明载荷已经是米，原样通过，防止重复换算并保留旧图的适配节点身份。"
           "不处理Measurement，后者继续按自身unit消费。";
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
  contract.options = {{"unspecified", "未声明"},
                      {"dts.geometry.v1", "DTS v1毫米几何"},
                      {"dts.geometry.v2", "DTS v2米制几何"}};
  Param producer = contract;
  producer.name = "sourceOp";
  producer.label = "旧生产者";
  producer.def = Value::text("dts.seal_root");
  producer.options = {{"dts.seal_dome", "胶条顶点轴"},
                      {"dts.pick_metal", "钣金基准线"},
                      {"dts.seal_root", "胶条根部点"},
                      {"dts.flush", "面差垂足"}};
  op.params = {contract, producer};
  op.migrations = {{1, migrateLegacyGeometry, nullptr}};
  op.compute = &compute;
  r.addOperator(std::move(op));
}

} // namespace lyflow::dts
