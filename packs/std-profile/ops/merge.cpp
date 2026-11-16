#include "lyflow/operator.h"
#include "lyflow/registry.h"
#include "lyflow_profile/merge.h"

namespace lyflow::packs::std_profile {
namespace {
Status failure(const std::string& why, const char* port = "a", const char* code = "bad_input") {
  return Status::Error(Phase::Execute, code, why, {}, port);
}
Status merge(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext& ctx) {
  const auto* aProvenance = inputs.get("aProvenance").asRecord();
  const auto* bProvenance = inputs.get("bProvenance").asRecord();
  if (inputs.has("aProvenance") && !aProvenance)
    return failure("首来源必须为Record", "aProvenance");
  if (inputs.has("bProvenance") && !bProvenance)
    return failure("次来源必须为Record", "bProvenance");
  profile_contract::MergeOptions options{params.text("roleA"), params.text("roleB"),
                                         params.choice("channelPolicy")};
  profile_contract::MergeResult result;
  std::string why;
  if (!profile_contract::mergeProfiles(inputs.get("a"), inputs.get("b"), aProvenance, bProvenance,
                                       options, result, why, [&ctx] { return ctx.cancelled(); }))
    return failure(why, "a", why == "轮廓合并已取消" ? "cancelled" : "bad_input");
  for (const auto& name : result.info.data["droppedChannels"])
    ctx.log(LogLevel::Warn, "显式intersection策略丢弃不共有的通道：" + name.get<std::string>());
  outputs.set("profile", std::move(result.profile));
  outputs.set("provenance", Data::record(std::move(result.provenance)));
  outputs.set("info", Data::record(std::move(result.info)));
  return Status::Ok();
}

Param text(const char* name, const char* label, const char* value) {
  Param result;
  result.name = name;
  result.label = label;
  result.type = ParamType::String;
  result.def = Value::text(value);
  return result;
}
} // namespace

void registerProfileMerge(Registry& registry) {
  OperatorDesc op;
  op.id = "profile.merge";
  op.version = "1.0.0";
  op.label = "轮廓有序合并";
  op.category = "轮廓/选择与适配";
  op.keywords = {"profile merge concatenate provenance", "轮廓 拼接 合并 来源"};
  op.inputs = {{"a", "Profile", "首轮廓", "同空间同单位的第一路轮廓。", true},
               {"b", "Profile", "次轮廓", "按a后b拼接，不按逻辑角色推断相机身份。", true},
               {"aProvenance", "Record", "首来源", "可选profile.CompositeSources.v1。", false},
               {"bProvenance", "Record", "次来源", "可选profile.CompositeSources.v1。", false}};
  op.outputs = {{"profile", "Profile", "合并轮廓", "保留原始槽映射的派生复合轮廓。", true},
                {"provenance", "Record", "复合来源", "有界分段声明，不包含大型原生数组。", true},
                {"info", "Record", "合并信息", "显式通道策略、丢弃通道及样本数量。", true}};
  auto policy = text("channelPolicy", "通道策略", "require_equal");
  policy.type = ParamType::Enum;
  policy.options = {{"require_equal", "要求相同通道"}, {"intersection", "显式取通道交集"}};
  op.params = {text("roleA", "首逻辑角色", "a"), text("roleB", "次逻辑角色", "b"), policy};
  op.doc = "按a后b合并同空间轮廓，原sourceCount分配不重叠索引区间；逻辑角色仅为软件来源声明。"
           "默认拒绝非空输入通道不同，intersection显式丢弃不共有通道并告警，不补造观测。";
  op.capabilities.deterministic = true;
  op.capabilities.cancellable = true;
  op.compute = &merge;
  registry.addOperator(std::move(op));
}
} // namespace lyflow::packs::std_profile
