#include "lyflow/kernel/constraints.h"
#include "lyflow/operator.h"
#include "lyflow/registry.h"

namespace lyflow::packs::std_geometry {
namespace {
Param text(std::string name, std::string label, std::string initial) {
  Param p;
  p.name = std::move(name);
  p.label = std::move(label);
  p.type = ParamType::String;
  p.def = Value::text(std::move(initial));
  return p;
}

Status constructConstraints(const Inputs&, const ParamView& p, Outputs& out, ExecContext& ctx) {
  if (p.text("sourceFrame").empty() || p.text("sourceFrame").size() > 1024 ||
      p.text("targetFrame").empty() || p.text("targetFrame").size() > 1024)
    return Status::Error(Phase::Execute, "bad_param", "源/目标帧必须显式有效", "sourceFrame");
  if (ctx.cancelled())
    return Status::Error(Phase::Execute, "cancelled", "约束构造已取消", {}, "items");
  nlohmann::json data;
  kernel::constraints::Error issue;
  if (!kernel::constraints::construct3D(p.text("items"), p.text("sourceFrame"),
                                        p.text("targetFrame"), p.choice("mode"), data, issue))
    return Status::Error(Phase::Execute, issue.code, issue.message, issue.field);
  if (ctx.cancelled())
    return Status::Error(Phase::Execute, "cancelled", "约束构造已取消", {}, "items");
  out.set("constraints", Data::record({kernel::constraints::kRecordType, std::move(data)}));
  return Status::Ok();
}
} // namespace

void registerConstraints3D(Registry& r) {
  OperatorDesc constructor;
  constructor.id = "register.constraints_3d";
  constructor.label = "三维几何约束构造";
  constructor.version = "1.0.0";
  constructor.category = "点云/三维分析";
  constructor.keywords = {"weighted 3D constraints point line plane datum 321 JSON constructor",
                          constructor.label};
  constructor.capabilities = {true, false, true};
  constructor.outputs = {
      withContract(Port{"constraints", "Record", "三维约束", "cloud.Constraints3D.v1。", true},
                   {{"recordType", "cloud.Constraints3D.v1"}})};
  Param items = text("items", "约束项JSON", "[]");
  items.type = ParamType::Text;
  Param mode = text("mode", "约束模型校验", "mixed");
  mode.type = ParamType::Enum;
  mode.options = {{"mixed", "混合点线面"}, {"weighted_321", "加权3-2-1基准"}};
  constructor.params = {text("sourceFrame", "源参考帧", "source"),
                        text("targetFrame", "目标参考帧", "target"), std::move(mode),
                        std::move(items)};
  constructor.doc =
      "从最多64KiB的items JSON构造有界cloud.Constraints3D.v1。复用拟合器相同的字段、单位、权重、3-2-1结构校验；不接受任意Record或大点云数组。几何可观性与质量由下游拟合器验证。";
  constructor.compute = constructConstraints;
  r.addOperator(std::move(constructor));
}
} // namespace lyflow::packs::std_geometry
