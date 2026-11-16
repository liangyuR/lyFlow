#include <cmath>
#include <limits>
#include <vector>

#include "lyflow/operator.h"
#include "lyflow/profile.h"
#include "lyflow/registry.h"
#include "lyflow_profile/context.h"
#include "lyflow_profile/selection.h"

namespace lyflow::packs::std_profile {
namespace {
using Json = nlohmann::json;
Status bad(const std::string& message, const char* port = "profile",
           const char* code = "bad_input") {
  return Status::Error(Phase::Execute, code, message, {}, port);
}
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
  const auto* provenance = inputs.has("provenance") ? inputs.get("provenance").asRecord() : nullptr;
  if (inputs.has("provenance") && !provenance)
    return bad("来源必须为Record", "provenance");
  profile_contract::SelectionOptions options;
  options.mode = params.choice("mode");
  options.emptyPolicy = params.choice("emptyPolicy");
  profile_contract::SelectionResult result;
  Status status;
  if (byBox) {
    const auto* context = inputs.get("boxContext").asRecord();
    if (!context)
      return bad("需要米制Box2D和有效上下文", "boxContext");
    options.bounds = params.choice("bounds");
    status = profile_contract::cropProfile(inputs.get("profile"), inputs.get("box"), *context,
                                           provenance, options, result, ctx);
  } else {
    const auto* indices = inputs.get("indices").asIndices();
    if (!indices)
      return bad("需要原生Indices", "indices");
    options.invalidPolicy = params.choice("invalidPolicy");
    status = profile_contract::selectProfileIndices(inputs.get("profile"), *indices, provenance,
                                                    options, result, ctx);
  }
  if (!status.ok)
    return status;
  outputs.set("profile", std::move(result.profile));
  outputs.set("sourceProfile", inputs.get("profile"));
  outputs.set("selected", Data::indices(std::move(result.selected)));
  outputs.set("rejected", Data::indices(std::move(result.rejected)));
  outputs.set("selection", Data::record(std::move(result.selection)));
  outputs.set("provenance", Data::record(std::move(result.provenance)));
  return Status::Ok();
}
}
void registerProfileRegionSelection(Registry& registry) {
  for (const bool byBox : {true, false}) {
    OperatorDesc op;
    op.id = byBox ? "profile.crop_box" : "profile.select_indices";
    op.version = "1.0.0";
    op.label = byBox ? "轮廓矩形选区" : "轮廓索引选区";
    op.category = "轮廓/选择与适配";
    op.keywords = {"profile", "selection", "crop", "indices", "轮廓", "选区", "索引"};
    op.capabilities.deterministic = true;
    op.capabilities.cancellable = true;
    op.inputs = {{"profile", "Profile", "轮廓", "完整原始采样与物理源槽映射。", true}};
    if (byBox) {
      op.inputs.push_back({"box", "Box2D", "ROI", "同空间米制矩形。", true});
      op.inputs.push_back(
          {"boxContext", "Record", "ROI上下文", "绑定实际框内容的物理空间。", true});
    } else {
      op.inputs.push_back(
          {"indices", "Indices", "槽索引", "当前cloud.id上的严格升序唯一槽。", true});
    }
    op.inputs.push_back(
        {"provenance", "Record", "来源", "可选CompositeSources.v1，严格绑定输入。", false});
    op.outputs = {{"profile", "Profile", "选区轮廓", "不修改原输入，保留通道与物理源索引。", true},
                  {"sourceProfile", "Profile", "原始轮廓", "selected/rejected的索引空间。", true},
                  {"selected", "Indices", "保留槽", "当前输入cloud.id上的槽。", true},
                  {"rejected", "Indices", "排除槽", "与保留槽互补。", true},
                  {"selection", "Record", "选区描述", "小元数据，数组留在原生载荷。", true},
                  {"provenance", "Record", "来源", "绑定输出Profile的来源映射。", true}};
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
