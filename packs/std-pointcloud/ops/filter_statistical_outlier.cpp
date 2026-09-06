#include <pcl/filters/statistical_outlier_removal.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>

#include "ops.h"
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

  // 产出 Indices 而不是点云（adapter.h 的约定）：PCL 只认识 XYZ，
  // 拿下标回来再走 PointCloud::select，通道搬运只在一处发生。
  pcl::Indices kept;
  pcl::StatisticalOutlierRemoval<pcl::PointXYZ> sor;
  sor.setInputCloud(cloud);
  sor.setMeanK(meanK);
  sor.setStddevMulThresh(stddevMul);
  sor.filter(kept);

  if (ctx.cancelled()) return Status::Ok();
  ctx.progress(0.9f);

  const std::vector<std::int32_t> keep = adapter::fromPclIndices(kept);
  PointCloud out = in.select(keep);

  Indices removed;
  removed.sourceCloudId = in.id;
  {
    std::vector<bool> keptFlag(in.pointCount(), false);
    for (std::int32_t i : keep) {
      if (i >= 0 && static_cast<std::size_t>(i) < keptFlag.size()) keptFlag[static_cast<std::size_t>(i)] = true;
    }
    for (std::size_t i = 0; i < keptFlag.size(); ++i) {
      if (!keptFlag[i]) removed.values.push_back(static_cast<std::int32_t>(i));
    }
  }

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
  op.label = "Statistical Outlier Removal";
  op.category = "Filter/Outlier";
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
  op.capabilities = {/*cancellable=*/false, /*previewable=*/false, /*deterministic=*/true};
  op.compute = &compute;

  r.addOperator(std::move(op));
}

}  // namespace lyflow::ops
