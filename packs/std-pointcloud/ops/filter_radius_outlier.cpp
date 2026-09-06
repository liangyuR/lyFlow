#include <pcl/filters/radius_outlier_removal.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>

#include "ops.h"
#include "lyflow_pcl/adapter.h"

namespace lyflow::ops {
namespace {

Status compute(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext& ctx) {
  const PointCloud& in = *inputs.get("cloud").asCloud();
  const double radius = params.number("radius");
  const auto minNeighbors = static_cast<int>(params.integer("minNeighbors"));

  Indices removed;
  removed.sourceCloudId = in.id;
  if (in.pointCount() == 0) {
    outputs.set("cloud", Data::cloud(in.select({})));
    outputs.set("removed", Data::indices(std::move(removed)));
    return Status::Ok();
  }

  ctx.progress(0.1f, "构建 KD 树");
  auto cloud = adapter::toPcl(in);

  pcl::Indices kept;
  pcl::RadiusOutlierRemoval<pcl::PointXYZ> ror;
  ror.setInputCloud(cloud);
  ror.setRadiusSearch(radius);
  ror.setMinNeighborsInRadius(minNeighbors);
  ror.filter(kept);

  if (ctx.cancelled()) return Status::Ok();
  ctx.progress(0.9f);

  const std::vector<std::int32_t> keep = adapter::fromPclIndices(kept);
  std::vector<bool> keptFlag(in.pointCount(), false);
  for (std::int32_t i : keep) {
    if (i >= 0 && static_cast<std::size_t>(i) < keptFlag.size()) keptFlag[static_cast<std::size_t>(i)] = true;
  }
  for (std::size_t i = 0; i < keptFlag.size(); ++i) {
    if (!keptFlag[i]) removed.values.push_back(static_cast<std::int32_t>(i));
  }

  ctx.log(LogLevel::Info, "剔除 " + std::to_string(removed.values.size()) + " 个稀疏点");
  outputs.set("cloud", Data::cloud(in.select(keep)));
  outputs.set("removed", Data::indices(std::move(removed)));
  return Status::Ok();
}

}  // namespace

void registerFilterRadiusOutlier(Registry& r) {
  OperatorDesc op;
  op.id = "filter.radius_outlier";
  op.version = "1.0.0";
  op.label = "Radius Outlier Removal";
  op.category = "Filter/Outlier";
  op.keywords = {"outlier", "radius", "noise", "半径", "离群", "去噪"};
  op.doc = "半径内邻居数不足的点被认为是孤立噪点。比统计法更直观：直接说「多远之内至少要有几个同伴」。";

  op.inputs = {Port{"cloud", "PointCloud", "Cloud", "输入点云。", true}};
  op.outputs = {
      Port{"cloud",   "PointCloud", "Cloud",   "保留下来的点。", true},
      Port{"removed", "Indices",    "Removed", "被剔除的点下标。", true},
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
  op.capabilities = {/*cancellable=*/false, /*previewable=*/false, /*deterministic=*/true};
  op.compute = &compute;

  r.addOperator(std::move(op));
}

}  // namespace lyflow::ops
