#include <limits>

#include "lyflow/operator.h"
#include "lyflow/profile.h"
#include "lyflow/registry.h"
#include "lyflow_pcl/filters.h"
#include "lyflow_profile/context.h"

namespace lyflow::packs::std_profile {
namespace {
Status bad(const std::string& message, const char* port = "profile") {
  return Status::Error(Phase::Execute, "bad_input", message, {}, port);
}
Status cancelled() { return Status::Error(Phase::Execute, "cancelled", "轮廓半径去噪已取消"); }
Status radiusOutlier(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                     ExecContext& ctx) {
  const auto& source = inputs.get("profile");
  const auto* input = source.asProfile();
  std::string why;
  if (ctx.cancelled())
    return cancelled();
  Profile metric;
  if (!convertProfileUnit(*input, "m", metric, why, [&ctx] { return ctx.cancelled(); }))
    return ctx.cancelled() ? cancelled() : bad(why);
  PointCloud working = *metric.cloud;
  const float nan = std::numeric_limits<float>::quiet_NaN();
  for (std::size_t i = 0; i < input->pointCount(); ++i) {
    if ((i & 8191U) == 0 && ctx.cancelled())
      return cancelled();
    if (!input->validity[i])
      for (int axis = 0; axis < 3; ++axis)
        working.xyz[3 * i + axis] = nan;
  }
  pcl_support::RadiusSelection selected;
  if (auto status = pcl_support::radiusOutlier(working, params.number("radius"),
                                               params.integer("minNeighbors"), selected, ctx);
      !status.ok)
    return status;
  selected.kept.sourceCloudId = selected.removed.sourceCloudId = input->cloud->id;
  Profile result = *input;
  result.cloud = std::make_shared<const PointCloud>(input->cloud->select(selected.kept.values));
  result.validity.clear();
  result.sourceIndices.clear();
  result.validity.reserve(selected.kept.values.size());
  result.sourceIndices.reserve(selected.kept.values.size());
  for (std::size_t i = 0; i < selected.kept.values.size(); ++i) {
    if ((i & 8191U) == 0 && ctx.cancelled())
      return cancelled();
    const auto slot = static_cast<std::size_t>(selected.kept.values[i]);
    result.validity.push_back(input->validity[slot]);
    result.sourceIndices.push_back(input->sourceIndices[slot]);
  }
  const auto data = Data::profile(std::move(result));
  const auto selection = Data::record(Record{"profile.RadiusSelection.v2",
                                             {{"schemaVersion", 2},
                                              {"unit", "m"},
                                              {"radius", params.number("radius")},
                                              {"minNeighbors", params.integer("minNeighbors")},
                                              {"indexSpace", "input_profile_slots"},
                                              {"keptCount", selected.kept.values.size()},
                                              {"removedCount", selected.removed.values.size()},
                                              {"sourceIndexMapping", "profile.sourceIndices"}}});
  if (ctx.cancelled())
    return cancelled();
  outputs.set("profile", data);
  outputs.set("sourceProfile", source);
  outputs.set("kept", Data::indices(std::move(selected.kept)));
  outputs.set("removed", Data::indices(std::move(selected.removed)));
  outputs.set("selection", selection);
  return Status::Ok();
}
}
void registerProfileRadiusOutlier(Registry& registry) {
  OperatorDesc op;
  op.id = "profile.radius_outlier";
  op.version = "2.0.0";
  op.migrations = {
      {1, nullptr, [](const nlohmann::json&, const std::vector<MigrationInput>& inputs) {
         return profile_contract::retireInputs(inputs, {"provenance"});
       }}};
  op.label = "轮廓半径离群样本剔除";
  op.category = "轮廓/过滤";
  op.keywords = {"profile", "radius", "outlier", "neighbors", "轮廓", "半径", "去噪"};
  op.doc =
      "以米制XYZ工作副本调用共享KD树邻域内核；无效槽不参与邻域，输出压缩但保留全部原通道与sourceIndices。";
  op.inputs = {{"profile", "Profile", "轮廓", "明确m/mm单位和原槽映射。", true}};
  op.outputs = {{"profile", "Profile", "过滤轮廓", "有效邻域样本，保序并保留物理源槽。", true},
                {"sourceProfile", "Profile", "原始轮廓", "kept/removed所属索引空间。", true},
                {"kept", "Indices", "保留槽", "绑定输入cloud.id的原槽。", true},
                {"removed", "Indices", "排除槽", "绑定输入cloud.id的原槽。", true},
                {"selection", "Record", "选择信息",
                 "profile.RadiusSelection.v2：单位、参数与计数，不复制源槽数组。", true}};
  Param radius;
  radius.name = "radius";
  radius.label = "邻域半径";
  radius.type = ParamType::Float;
  radius.def = Value::number(.05);
  radius.unit = "m";
  radius.min = 1e-6;
  radius.max = 1000;
  Param minimum;
  minimum.name = "minNeighbors";
  minimum.label = "最少邻居数";
  minimum.type = ParamType::Int;
  minimum.def = Value::integer(5);
  minimum.min = 1;
  minimum.max = 10000;
  op.params = {radius, minimum};
  op.capabilities.deterministic = true;
  op.capabilities.cancellable = true;
  op.compute = &radiusOutlier;
  registry.addOperator(std::move(op));
}
}
