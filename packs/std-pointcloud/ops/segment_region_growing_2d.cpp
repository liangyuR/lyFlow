#include <string>
#include <vector>

#include "algo/region_growing2d.h"
#include "ops.h"

namespace lyflow::ops {
namespace {

Status compute(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext& ctx) {
  const PointCloud& in = *inputs.get("cloud").asCloud();

  std_pc::RegionGrowing2DOptions options;
  options.neighbors = static_cast<int>(params.integer("neighbors"));
  options.angleThresholdDeg = params.number("angleThresh");
  options.curvatureThreshold = static_cast<float>(params.number("curvatureThresh"));
  const std::int64_t minSize = params.integer("minClusterSize");
  const std::int64_t maxSize = params.integer("maxClusterSize");
  if (options.neighbors < 2) {
    return Status::Error(Phase::Execute, "bad_param", "近邻数至少是 2", "neighbors");
  }
  if (minSize < 1 || maxSize < minSize) {
    return Status::Error(Phase::Execute, "bad_param", "簇大小上限必须不小于下限", "maxClusterSize");
  }
  options.minClusterSize = static_cast<std::size_t>(minSize);
  options.maxClusterSize = static_cast<std::size_t>(maxSize);

  const std::size_t n = in.pointCount();
  std::vector<float> xy(2 * n);
  for (std::size_t i = 0; i < n; ++i) {
    xy[2 * i] = in.xyz[3 * i];
    xy[2 * i + 1] = in.xyz[3 * i + 1];
  }
  if (ctx.cancelled())
    return Status::Ok();
  const std_pc::RegionGrowing2DResult result = std_pc::regionGrowing2D(xy.data(), n, options);

  std::vector<std::int64_t> sizes;
  sizes.reserve(result.clusters.size());
  for (const auto& cluster : result.clusters)
    sizes.push_back(static_cast<std::int64_t>(cluster.size()));
  const std::string summary = std::to_string(n) + " 点 → " +
                              std::to_string(result.clusters.size()) + " 簇（簇大小在 [" +
                              std::to_string(minSize) + ", " + std::to_string(maxSize) + "] 内）";

  if (result.clusters.size() < 2) {
    std::string why = n < static_cast<std::size_t>(options.neighbors)
                          ? "点数 " + std::to_string(n) + " 少于近邻数 " +
                                std::to_string(options.neighbors) + "，估不了法线"
                          : summary + "，分不出左右两簇";
    return Status::Error(Phase::Execute, "split_failed", why, {}, "cloud");
  }

  outputs.set("left", Data::cloud(in.select(result.left)));
  outputs.set("right", Data::cloud(in.select(result.right)));

  Record record;
  record.type = "RegionGrowing2DResult";
  record.data["clusterCount"] = result.clusters.size();
  record.data["clusterSizes"] = sizes;
  record.data["leftCount"] = result.left.size();
  record.data["rightCount"] = result.right.size();
  outputs.set("result", Data::record(std::move(record)));

  ctx.log(LogLevel::Info, summary + "，左 " + std::to_string(result.left.size()) + " / 右 " +
                              std::to_string(result.right.size()));
  return Status::Ok();
}

Param intParam(const char* name, const char* label, std::int64_t def, double min, const char* doc) {
  Param p;
  p.name = name;
  p.type = ParamType::Int;
  p.label = label;
  p.doc = doc;
  p.def = Value::integer(def);
  p.min = min;
  return p;
}

} // namespace

void registerSegmentRegionGrowing2D(Registry& r) {
  OperatorDesc op;
  op.id = "segment.region_growing_2d";
  op.version = "1.0.0";
  op.label = "区域生长 2D";
  op.category = "分割";
  op.keywords = {"region growing", "cluster", "split", "segmentation", "2d",
                 "区域生长",       "聚类",    "分簇",  "左右"};
  op.doc =
      "在 XY 平面上按法线夹角做区域生长（PCL RegionGrowing 的平滑模式，z 不参与），"
      "取点数最多的两簇，x 均值小的从 Left 出、大的从 Right 出。\n"
      "法线由每个点的 Neighbors 个最近邻（含自己）的协方差估出，生长时比较邻点与当前种子的法线夹角，"
      "小于 Angle Thresh 就并入。簇大小不在 [Min, Max] 内的丢掉。\n"
      "剩下不到两簇报 split_failed（点数少于近邻数时同样如此）。"
      "典型用法：标定圆柱在一条剖面里的两段弧，分开后各接一个「拟合圆 2D」。";
  op.inputs = {withContract(Port{"cloud", "PointCloud", "Cloud", "待分割的点云，只看 XY。", true},
                            {{"finite", true}})};
  op.outputs = {
      Port{"left", "PointCloud", "Left", "最大两簇里 x 均值小的那簇，保留输入的通道，按输入顺序。",
           true},
      Port{"right", "PointCloud", "Right", "最大两簇里 x 均值大的那簇。", true},
      withExample(Port{"result", "Record", "Result",
                       "RegionGrowing2DResult：clusterCount、clusterSizes（按生长顺序）、"
                       "leftCount、rightCount。",
                       true},
                  {{"kind", "Record"},
                   {"type", "RegionGrowing2DResult"},
                   {"data",
                    {{"clusterCount", 2},
                     {"clusterSizes", {412, 389}},
                     {"leftCount", 412},
                     {"rightCount", 389}}}}),
  };

  Param angle;
  angle.name = "angleThresh";
  angle.type = ParamType::Float;
  angle.label = "Angle Thresh";
  angle.doc = "邻点与种子的法线夹角小于它才并入同一簇。";
  angle.def = Value::number(50.0);
  angle.unit = "deg";
  angle.min = 0.0;
  angle.max = 180.0;
  angle.step = 1.0;

  Param curvature;
  curvature.name = "curvatureThresh";
  curvature.type = ParamType::Float;
  curvature.label = "Curvature Thresh";
  curvature.doc = "曲率大于它的点并入簇但不再当种子。二维曲率最大 0.5，默认 1 即每个点都当种子。";
  curvature.def = Value::number(1.0);
  curvature.min = 0.0;
  curvature.step = 0.05;
  curvature.advanced = true;

  Param minSize = intParam("minClusterSize", "Min Cluster Size", 100, 1.0, "点数少于它的簇丢掉。");
  Param maxSize =
      intParam("maxClusterSize", "Max Cluster Size", 100000, 1.0, "点数多于它的簇丢掉。");
  maxSize.advanced = true;

  op.params = {
      intParam("neighbors", "Neighbors", 50, 2.0,
               "估法线与生长时看的最近邻数（含自己）。点数少于它时什么也分不出。"),
      angle,
      minSize,
      maxSize,
      curvature,
  };
  op.capabilities = {/*cancellable=*/false, /*previewable=*/true, /*deterministic=*/true};
  op.compute = &compute;

  r.addOperator(std::move(op));
}

} // namespace lyflow::ops
