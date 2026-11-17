#include "lyflow/operator.h"
#include "lyflow/registry.h"
#include "lyflow_profile/merge.h"

namespace lyflow::packs::std_profile {
namespace {
Status failure(const std::string& why, const char* port = "a", const char* code = "bad_input") {
  return Status::Error(Phase::Execute, code, why, {}, port);
}
Status merge(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext& ctx) {
  profile_contract::MergeResult result;
  std::string why;
  if (!profile_contract::mergeProfiles(inputs.get("a"), inputs.get("b"),
                                       params.choice("channelPolicy"), result, why,
                                       [&ctx] { return ctx.cancelled(); }))
    return failure(why, "a", why == "轮廓合并已取消" ? "cancelled" : "bad_input");
  for (const auto& name : result.info.data["droppedChannels"])
    ctx.log(LogLevel::Warn, "显式intersection策略丢弃不共有的通道：" + name.get<std::string>());
  outputs.set("profile", std::move(result.profile));
  outputs.set("info", Data::record(std::move(result.info)));
  return Status::Ok();
}

nlohmann::json dropRoles(const nlohmann::json& params) {
  auto result = params;
  result.erase("roleA");
  result.erase("roleB");
  return result;
}
} // namespace

void registerProfileMerge(Registry& registry) {
  OperatorDesc op;
  op.id = "profile.merge";
  op.version = "2.0.0";
  op.migrations = {
      {1, &dropRoles, [](const nlohmann::json&, const std::vector<MigrationInput>& inputs) {
         return profile_contract::retireInputs(inputs, {"aProvenance", "bProvenance"});
       }}};
  op.label = "轮廓有序合并";
  op.category = "轮廓/选择与适配";
  op.keywords = {"profile merge concatenate", "轮廓 拼接 合并"};
  op.inputs = {{"a", "Profile", "首轮廓", "同空间同单位的第一路轮廓。", true},
               {"b", "Profile", "次轮廓", "按a后b拼接。", true}};
  op.outputs = {{"profile", "Profile", "合并轮廓",
                 "保留原始槽映射；sourceId为derived:composite/后按a、b顺序以+连接的父来源。", true},
                {"info", "Record", "合并信息", "显式通道策略、丢弃通道及样本数量。", true}};
  Param policy;
  policy.name = "channelPolicy";
  policy.label = "通道策略";
  policy.type = ParamType::Enum;
  policy.def = Value::text("require_equal");
  policy.options = {{"require_equal", "要求相同通道"}, {"intersection", "显式取通道交集"}};
  op.params = {policy};
  op.doc = "按a后b合并同空间同单位轮廓，原sourceCount分配不重叠索引区间；复合父来源展平拼接。"
           "默认拒绝非空输入通道不同，intersection显式丢弃不共有通道并告警，不补造观测。";
  op.capabilities.deterministic = true;
  op.capabilities.cancellable = true;
  op.compute = &merge;
  registry.addOperator(std::move(op));
}
} // namespace lyflow::packs::std_profile
