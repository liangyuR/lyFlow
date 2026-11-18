#include <cmath>
#include <utility>

#include "lyflow/op_params.h"
#include "lyflow/operator.h"
#include "lyflow/registry.h"

namespace lyflow::packs::std_metrology {
namespace {
Status compareTolerance(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                        ExecContext&) {
  Measurement result = *inputs.get("value").asMeasurement();
  result.hasLimits = true;
  result.nominal = params.number("nominal");
  result.upper = result.nominal + params.number("upper");
  result.lower = result.nominal + params.number("lower");
  const double margin = params.number("margin");
  if (!std::isfinite(result.upper) || !std::isfinite(result.lower) || result.lower > result.upper)
    return op_params::badParam("公差上下限须有限且下限不大于上限", "lower");
  if (!result.ok || !std::isfinite(result.value))
    result.verdict = "fail";
  else if (result.value > result.upper)
    result.verdict = "high";
  else if (result.value < result.lower)
    result.verdict = "low";
  else if (margin > 0 &&
           (result.value > result.upper - margin || result.value < result.lower + margin))
    result.verdict = "margin";
  else
    result.verdict = "ok";
  outputs.set("value", Data::measurement(std::move(result)));
  return Status::Ok();
}
}

void registerCompareTolerance(Registry& registry) {
  using op_params::floatParam;
  OperatorDesc op;
  op.id = "metrology.compare_tolerance";
  op.version = "1.0.0";
  op.label = "量测公差比较";
  op.category = "度量";
  op.doc =
      "标称值、上下偏差与边界余量均使用输入量测的单位。保留原值和有效性，附加绝对限值与比较结果。";
  op.inputs = {{"value", "Measurement", "量测", "待比较的量测。", true}};
  op.outputs = {
      {"value", "Measurement", "比较结果", "verdict 为 ok、margin、high、low 或 fail。", true}};
  auto margin = floatParam("margin", "边界余量", 0, "零表示关闭边界预警。");
  margin.min = 0;
  op.params = {floatParam("nominal", "标称值", 0, "公差围绕的标称读数。"),
               floatParam("upper", "上偏差", 1, "相对标称值。"),
               floatParam("lower", "下偏差", -1, "相对标称值。"), margin};
  op.capabilities = {false, true, true};
  op.compute = compareTolerance;
  registry.addOperator(std::move(op));
}
}
