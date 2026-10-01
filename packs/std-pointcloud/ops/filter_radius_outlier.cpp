#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/search/kdtree.h>

#include <cstdint>
#include <vector>

#include "ops.h"
#include "parallel.h"
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

  // 与 pcl::RadiusOutlierRemoval（vcpkg 的 1.12）逐点同一套：同一种不排序的 KD 树；没有非有限点（is_dense）时
  // 找 minNeighbors + 1 个近邻（含自己），最远那个在半径内才留 —— 等价于「半径内连自己至少 minNeighbors + 1 个」，
  // 却不用把半径内的点全列出来；有非有限点时退回半径搜索。只是逐点那一圈按线程预算分段并行（parallel.h）。
  pcl::search::KdTree<pcl::PointXYZ> tree(false);
  tree.setInputCloud(cloud);
  if (ctx.cancelled()) return Status::Ok();

  const std::size_t n = in.pointCount();
  const int meanK = minNeighbors + 1;
  const double maxSqr = radius * radius;
  std::vector<char> keptFlag(n, 0);
  ctx.progress(0.2f, "数邻居");
  parallelFor(n, ctx.threadBudget(), [&](std::size_t begin, std::size_t end) {
    if (ctx.cancelled()) return false;
    pcl::Indices nn(static_cast<std::size_t>(meanK));
    std::vector<float> d2(static_cast<std::size_t>(meanK));
    for (std::size_t i = begin; i < end; ++i) {
      const auto index = static_cast<pcl::index_t>(i);
      if (cloud->is_dense) {
        const int k = tree.nearestKSearch(index, meanK, nn, d2);
        keptFlag[i] = k == meanK && !(maxSqr < d2[static_cast<std::size_t>(meanK) - 1]);
      } else {
        // 只要知道「够不够 minNeighbors + 1 个」：最多要这么多个，稠密处就不必把半径内的几百个点全列出来。
        // 判据不变 —— 返回 min(半径内的个数, meanK)，大于 minNeighbors 当且仅当半径内至少 meanK 个
        const int k = tree.radiusSearch(index, radius, nn, d2, static_cast<unsigned int>(meanK));
        keptFlag[i] = k > minNeighbors;
      }
    }
    return true;
  });
  if (ctx.cancelled()) return Status::Ok();
  ctx.progress(0.9f);

  std::vector<std::int32_t> keep;
  keep.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    if (keptFlag[i]) {
      keep.push_back(static_cast<std::int32_t>(i));
    } else {
      removed.values.push_back(static_cast<std::int32_t>(i));
    }
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
  op.label = "半径离群点剔除";
  op.category = "过滤/离群点";
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
  op.capabilities = {/*cancellable=*/true, /*previewable=*/false, /*deterministic=*/true};
  op.compute = &compute;

  r.addOperator(std::move(op));
}

}  // namespace lyflow::ops
