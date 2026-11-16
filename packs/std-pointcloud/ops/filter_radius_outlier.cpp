#include "ops.h"
#include "lyflow_pcl/filters.h"

namespace lyflow::ops {
namespace {

Status compute(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext& ctx) {
  const auto* input = inputs.get("cloud").asCloud();
  if (!input)
    return Status::Error(Phase::Execute, "bad_input", "需要PointCloud", {}, "cloud");
  pcl_support::RadiusSelection selected;
  if (auto status = pcl_support::radiusOutlier(*input, params.number("radius"),
                                               params.integer("minNeighbors"), selected, ctx);
      !status.ok)
    return status;
  auto cloud = Data::cloud(input->select(selected.kept.values));
  if (ctx.cancelled())
    return Status::Error(Phase::Execute, "cancelled", "半径去噪已取消");
  ctx.log(LogLevel::Info, "剔除 " + std::to_string(selected.removed.values.size()) + " 个稀疏点");
  outputs.set("cloud", std::move(cloud));
  outputs.set("removed", Data::indices(std::move(selected.removed)));
  return Status::Ok();
}

} // namespace

void registerFilterRadiusOutlier(Registry& r) {
  OperatorDesc op;
  op.id = "filter.radius_outlier";
  op.version = "1.0.1";
  op.label = "半径离群点剔除";
  op.category = "过滤/离群点";
  op.keywords = {"radius outlier removal",
                 "ROR",
                 "radius neighbor filter",
                 "孤立点剔除",
                 "半径滤波",
                 "点云去噪",
                 "outlier",
                 "radius",
                 "noise",
                 "半径",
                 "离群",
                 "去噪"};
  op.doc =
      "半径内邻居数不足的点被认为是孤立噪点。比统计法更直观：直接说「多远之内至少要有几个同伴」。";

  op.inputs = {Port{"cloud", "PointCloud", "Cloud", "输入点云。", true}};
  op.outputs = {
      Port{"cloud", "PointCloud", "Cloud", "保留下来的点。", true},
      Port{"removed", "Indices", "Removed", "被剔除的点下标。", true},
  };

  Param radius;
  radius.name = "radius";
  radius.type = ParamType::Float;
  radius.label = "Search Radius";
  radius.doc = "邻域半径。量纲与点云一致（通常是米）。";
  radius.def = Value::number(0.05);
  radius.min = 1e-6;
  radius.max = 1000.0;
  radius.softMax = 1.0;
  radius.step = 0.005;
  radius.unit = "m";

  Param minNeighbors;
  minNeighbors.name = "minNeighbors";
  minNeighbors.type = ParamType::Int;
  minNeighbors.label = "Min Neighbors";
  minNeighbors.doc = "半径内至少要有几个邻居才算有效点。";
  minNeighbors.def = Value::integer(5);
  minNeighbors.min = 1.0;
  minNeighbors.max = 10000.0;
  minNeighbors.softMax = 100.0;

  op.params = {radius, minNeighbors};
  op.capabilities = {/*cancellable=*/true, /*previewable=*/false, /*deterministic=*/true};
  op.compute = &compute;

  r.addOperator(std::move(op));
}

} // namespace lyflow::ops
