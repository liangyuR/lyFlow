#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/search/kdtree.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "ops.h"
#include "parallel.h"
#include "lyflow_pcl/adapter.h"

namespace lyflow::ops {
namespace {

Status compute(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext& ctx) {
  const PointCloud& in = *inputs.get("cloud").asCloud();
  const auto meanK = static_cast<int>(params.integer("meanK"));
  const double stddevMul = params.number("stddevMul");

  if (in.pointCount() == 0) {
    outputs.set("cloud", Data::cloud(in.select({})));
    Indices removed;
    removed.sourceCloudId = in.id;
    outputs.set("removed", Data::indices(std::move(removed)));
    return Status::Ok();
  }
  if (static_cast<std::size_t>(meanK) >= in.pointCount()) {
    return Status::Error(Phase::Execute, "bad_param",
                         "Mean K (" + std::to_string(meanK) + ") 不能大于等于点数 (" +
                             std::to_string(in.pointCount()) + ")",
                         "meanK");
  }

  ctx.progress(0.1f, "构建 KD 树");
  auto cloud = adapter::toPcl(in);

  // 与 pcl::StatisticalOutlierRemoval（vcpkg 的 1.12）逐点同一套：同一种不排序的 KD 树、K+1 近邻（第 0 个是自己）、
  // 平均距离存成 float，非有限点记 0 且不计数。只是逐点那一圈按线程预算分段并行（parallel.h）——
  // 它的 `#pragma omp` 在没开 OpenMP 的 PCL 里是单线程，200 万点 14 s。均值与标准差照它的顺序单线程累加，阈值逐位相同。
  pcl::search::KdTree<pcl::PointXYZ> tree(false);
  tree.setInputCloud(cloud);
  if (ctx.cancelled()) return Status::Ok();

  const std::size_t n = in.pointCount();
  std::vector<float> meanDist(n, 0.0f);
  std::vector<char> valid(n, 0);
  ctx.progress(0.2f, "近邻距离");
  parallelFor(n, ctx.threadBudget(), [&](std::size_t begin, std::size_t end) {
    if (ctx.cancelled()) return false;
    pcl::Indices nn(static_cast<std::size_t>(meanK) + 1);
    std::vector<float> d2(static_cast<std::size_t>(meanK) + 1);
    for (std::size_t i = begin; i < end; ++i) {
      const pcl::PointXYZ& pt = (*cloud)[i];
      if (!std::isfinite(pt.x) || !std::isfinite(pt.y) || !std::isfinite(pt.z)) continue;
      if (tree.nearestKSearch(static_cast<pcl::index_t>(i), meanK + 1, nn, d2) == 0) continue;
      double sum = 0.0;
      for (int k = 1; k < meanK + 1; ++k) sum += std::sqrt(d2[static_cast<std::size_t>(k)]);
      meanDist[i] = static_cast<float>(sum / meanK);
      valid[i] = 1;
    }
    return true;
  });
  if (ctx.cancelled()) return Status::Ok();
  ctx.progress(0.9f);

  double sum = 0.0;
  double sqSum = 0.0;
  int validCount = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const float d = meanDist[i];
    sum += d;
    sqSum += d * d;
    validCount += valid[i];
  }
  const double mean = sum / static_cast<double>(validCount);
  const double variance =
      (sqSum - sum * sum / static_cast<double>(validCount)) / (static_cast<double>(validCount) - 1);
  const double threshold = mean + stddevMul * std::sqrt(variance);

  // 产出 Indices 而不是点云（adapter.h 的约定）：拿下标回来再走 PointCloud::select，通道搬运只在一处发生
  std::vector<std::int32_t> keep;
  keep.reserve(n);
  Indices removed;
  removed.sourceCloudId = in.id;
  for (std::size_t i = 0; i < n; ++i) {
    if (meanDist[i] > threshold) {
      removed.values.push_back(static_cast<std::int32_t>(i));
    } else {
      keep.push_back(static_cast<std::int32_t>(i));
    }
  }
  PointCloud out = in.select(keep);

  ctx.log(LogLevel::Info, "剔除 " + std::to_string(removed.values.size()) + " 个离群点");
  outputs.set("cloud", Data::cloud(std::move(out)));
  outputs.set("removed", Data::indices(std::move(removed)));
  return Status::Ok();
}

}  // namespace

void registerFilterStatisticalOutlier(Registry& r) {
  OperatorDesc op;
  op.id = "filter.statistical_outlier";
  op.version = "1.0.0";
  op.label = "统计离群点剔除";
  op.category = "过滤/离群点";
  op.keywords = {"outlier", "noise", "statistical", "sor", "离群", "去噪", "统计"};
  op.doc = "按每个点到 K 近邻的平均距离剔除离群点。距离超过 均值 + n×标准差 的点被认为是噪声。";

  op.inputs = {Port{"cloud", "PointCloud", "Cloud", "输入点云。", true}};
  op.outputs = {
      Port{"cloud",   "PointCloud", "Cloud",   "保留下来的点。", true},
      Port{"removed", "Indices",    "Removed", "被剔除的点下标，可接 Extract Indices 查看。", true},
  };

  Param meanK;
  meanK.name = "meanK";
  meanK.type = ParamType::Int;
  meanK.label = "Mean K";
  meanK.doc = "参与统计的近邻个数。太小容易误杀边缘点。";
  meanK.def = Value::integer(30);
  meanK.min = 2.0;
  meanK.max = 10000.0;
  meanK.softMax = 200.0;

  Param stddev;
  stddev.name = "stddevMul";
  stddev.type = ParamType::Float;
  stddev.label = "Std Dev Multiplier";
  stddev.doc = "阈值 = 平均距离 + 本值 × 标准差。越小杀得越狠。";
  stddev.def = Value::number(1.0);
  stddev.min = 0.01;
  stddev.max = 100.0;
  stddev.softMax = 5.0;
  stddev.step = 0.1;

  op.params = {meanK, stddev};
  // PCL 的 filter() 一旦进去就出不来，没有轮询点。如实申报 false。
  op.capabilities = {/*cancellable=*/true, /*previewable=*/false, /*deterministic=*/true};
  op.compute = &compute;

  r.addOperator(std::move(op));
}

}  // namespace lyflow::ops
