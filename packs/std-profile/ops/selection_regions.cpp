#include <vector>

#include "lyflow/operator.h"
#include "lyflow/registry.h"
#include "lyflow_profile/selection.h"

namespace lyflow::packs::std_profile {
namespace {
Param choice(const char* name, const char* initial, std::initializer_list<const char*> values) {
  Param p;
  p.name = name;
  p.label = name;
  p.type = ParamType::Enum;
  p.def = Value::text(initial);
  for (const auto* value : values)
    p.options.push_back({value, value});
  return p;
}

Status select(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext& ctx,
              bool byBox) {
  profile_contract::SelectionOptions options;
  options.mode = params.choice("mode");
  options.emptyPolicy = params.choice("emptyPolicy");
  profile_contract::SelectionResult result;
  Status status;
  if (byBox) {
    options.bounds = params.choice("bounds");
    status =
        profile_contract::cropProfile(inputs.get("profile"), inputs.get("box"),
                                      inputs.get("boxContext").asRecord(), options, result, ctx);
  } else {
    options.invalidPolicy = params.choice("invalidPolicy");
    status = profile_contract::selectProfileIndices(
        inputs.get("profile"), *inputs.get("indices").asIndices(), options, result, ctx);
  }
  if (!status.ok)
    return status;
  outputs.set("profile", std::move(result.profile));
  outputs.set("sourceProfile", inputs.get("profile"));
  outputs.set("selected", Data::indices(std::move(result.selected)));
  outputs.set("rejected", Data::indices(std::move(result.rejected)));
  outputs.set("selection", Data::record(std::move(result.selection)));
  return Status::Ok();
}
}
void registerProfileRegionSelection(Registry& registry) {
  for (const bool byBox : {true, false}) {
    OperatorDesc op;
    op.id = byBox ? "profile.crop_box" : "profile.select_indices";
    op.version = "2.0.0";
    op.migrations = {
        {1, nullptr, [](const nlohmann::json&, const std::vector<MigrationInput>& inputs) {
           return profile_contract::retireInputs(inputs, {"provenance"});
         }}};
    op.label = byBox ? "轮廓矩形选区" : "轮廓索引选区";
    op.category = "轮廓/选择与适配";
    op.keywords = {"profile", "selection", "crop", "indices", "轮廓", "选区", "索引"};
    op.capabilities.deterministic = true;
    op.capabilities.cancellable = true;
    op.inputs = {{"profile", "Profile", "轮廓", "完整原始采样与物理源槽映射。", true}};
    if (byBox) {
      op.inputs.push_back({"box", "Box2D", "ROI", "同空间米制矩形。", true});
      op.inputs.push_back(
          {"boxContext", "Record", "ROI上下文", "profile.Geometry.v3，须与轮廓同空间。", true});
    } else {
      op.inputs.push_back(
          {"indices", "Indices", "槽索引", "当前cloud.id上的严格升序唯一槽。", true});
    }
    op.outputs = {{"profile", "Profile", "选区轮廓", "不修改原输入，保留通道与物理源索引。", true},
                  {"sourceProfile", "Profile", "原始轮廓", "selected/rejected的索引空间。", true},
                  {"selected", "Indices", "保留槽", "当前输入cloud.id上的槽。", true},
                  {"rejected", "Indices", "排除槽", "与保留槽互补。", true},
                  {"selection", "Record", "选区描述",
                   "profile.RegionSelection.v2计数，数组留在原生载荷。", true}};
    op.params = {choice("mode", "compact", {"compact", "preserve_slots"}),
                 choice("emptyPolicy", "error", {"error", "allow"})};
    if (byBox)
      op.params.push_back(choice("bounds", "closed", {"open", "closed"}));
    else
      op.params.push_back(choice("invalidPolicy", "reject", {"reject", "preserve"}));
    op.doc =
        byBox
            ? "在声明分析平面以米制ROI筛选有效样本，边界开闭显式；保序、保通道与sourceIndices。"
            : "仅接受同一cloud.id上的严格升序唯一槽；不排序、不去重、不改变权重。无效槽需显式选择保留。";
    op.compute = [byBox](const Inputs& in, const ParamView& params, Outputs& out,
                         ExecContext& ctx) { return select(in, params, out, ctx, byBox); };
    registry.addOperator(std::move(op));
  }
}
}
