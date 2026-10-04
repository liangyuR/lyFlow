#include <pcl/common/centroid.h>
#include <pcl/features/feature.h>
#include <pcl/features/normal_3d.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/search/kdtree.h>

#include <atomic>
#include <cmath>
#include <vector>

#include "ops.h"
#include "parallel.h"
#include "lyflow_pcl/adapter.h"

namespace lyflow::ops {
namespace {

Status compute(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext& ctx) {
  const PointCloud& in = *inputs.get("cloud").asCloud();
  const bool byRadius = params.choice("mode") == "radius";
  const auto kSearch = static_cast<int>(params.integer("kSearch"));
  const double radius = params.number("radius");
  const std::array<float, 3> viewpoint = params.vec3("viewpoint");
  const bool flip = params.flag("flipTowardsViewpoint");

  if (in.pointCount() == 0) {
    outputs.set("cloud", Data::cloud(in.select({})));
    return Status::Ok();
  }
  if (!byRadius && static_cast<std::size_t>(kSearch) >= in.pointCount()) {
    return Status::Error(Phase::Execute, "bad_param",
                         "K Search (" + std::to_string(kSearch) + ") 不能大于等于点数 (" +
                             std::to_string(in.pointCount()) + ")",
                         "kSearch");
  }

  ctx.progress(0.1f, "构建 KD 树");
  auto cloud = adapter::toPcl(in);

  // 与 pcl::NormalEstimation（vcpkg 的 1.12）逐点同一套：同一种排序的 KD 树、K 近邻或半径邻域、
  // 均值与协方差 → 最小特征值的特征向量。只是逐点那一圈按线程预算分段并行（parallel.h；NormalEstimationOMP 在
  // 没开 OpenMP 的 PCL 里是单线程，200 万点 12 s）。
  // 朝向：只在 flipTowardsViewpoint 开着时翻 —— PCL 的 NormalEstimation 不论如何都朝视点（没设就是原点）翻，
  // 以前这个开关关掉也照翻
  pcl::search::KdTree<pcl::PointXYZ> tree;
  tree.setInputCloud(cloud);
  if (ctx.cancelled()) return Status::Ok();

  const std::size_t n = in.pointCount();
  PointCloud out = in;
  out.normals.assign(n * 3, 0.0f);
  std::atomic<std::size_t> degenerateCount{0};
  ctx.progress(0.2f, "估计法线");
  parallelFor(n, ctx.threadBudget(), [&](std::size_t begin, std::size_t end) {
    if (ctx.cancelled()) return false;
    pcl::Indices nn;
    std::vector<float> d2;
    EIGEN_ALIGN16 Eigen::Matrix3f covariance;
    Eigen::Vector4f centroid;
    std::size_t bad = 0;
    for (std::size_t i = begin; i < end; ++i) {
      const pcl::PointXYZ& pt = (*cloud)[i];
      const auto index = static_cast<pcl::index_t>(i);
      const bool finitePoint = std::isfinite(pt.x) && std::isfinite(pt.y) && std::isfinite(pt.z);
      const int found = !finitePoint ? 0
                        : byRadius   ? tree.radiusSearch(index, radius, nn, d2, 0)
                                     : tree.nearestKSearch(index, kSearch, nn, d2);
      float nx = 0.0f, ny = 0.0f, nz = 0.0f, curvature = 0.0f;
      if (found == 0 || nn.size() < 3 || pcl::computeMeanAndCovarianceMatrix(*cloud, nn, covariance, centroid) == 0) {
        ++bad;
        continue;
      }
      pcl::solvePlaneParameters(covariance, nx, ny, nz, curvature);
      if (!std::isfinite(nx) || !std::isfinite(ny) || !std::isfinite(nz)) {
        // 邻域退化时 PCL 填 NaN。这里置零而不是留着：NaN 流到下游，
        // 体素栅格一做平均整片法线就全成了 NaN，再往下就是 3D 视图黑屏。
        ++bad;
        continue;
      }
      if (flip) pcl::flipNormalTowardsViewpoint(pt, viewpoint[0], viewpoint[1], viewpoint[2], nx, ny, nz);
      out.normals[i * 3] = nx;
      out.normals[i * 3 + 1] = ny;
      out.normals[i * 3 + 2] = nz;
    }
    degenerateCount += bad;
    return true;
  });
  if (ctx.cancelled()) return Status::Ok();
  ctx.progress(0.9f);
  const std::size_t degenerate = degenerateCount.load();

  if (degenerate > 0) {
    ctx.log(LogLevel::Warn,
            std::to_string(degenerate) + " 个点的邻域不足，法线置零（把半径或 K 调大）");
  }

  outputs.set("cloud", Data::cloud(std::move(out)));
  return Status::Ok();
}

}  // namespace

void registerFeaturesNormals(Registry& r) {
  OperatorDesc op;
  op.id = "features.normals";
  op.version = "1.0.0";
  op.label = "估计法线";
  op.category = "特征";
  op.keywords = {"normal", "normals", "法线", "法向量"};
  op.doc = "估计每个点的法线，写进点云的 normals 通道。原有的强度与颜色通道保持不变。";

  op.inputs = {Port{"cloud", "PointCloud", "Cloud", "输入点云。", true}};
  op.outputs = {Port{"cloud", "PointCloud", "Cloud", "带法线通道的点云。", true}};

  Param mode;
  mode.name = "mode";
  mode.type = ParamType::Enum;
  mode.label = "Neighborhood";
  mode.doc = "用固定个数的近邻还是固定半径的球邻域。密度不均匀的点云用半径更稳。";
  mode.def = Value::text("k");
  mode.options = {
      EnumOption{"k", "K Nearest", "固定近邻个数，密度变化时邻域的物理尺度会跟着变。"},
      EnumOption{"radius", "Radius", "固定物理半径，尺度一致但稀疏区可能邻域不足。"},
  };

  Param k;
  k.name = "kSearch";
  k.type = ParamType::Int;
  k.label = "K Search";
  k.def = Value::integer(20);
  k.min = 3.0;
  k.max = 10000.0;
  k.softMax = 100.0;
  k.visibleWhen = Condition{"mode", Value::text("k"), {}};

  Param radius;
  radius.name = "radius";
  radius.type = ParamType::Float;
  radius.label = "Radius";
  radius.def = Value::number(0.03);
  radius.min = 1e-6;
  radius.max = 1000.0;
  radius.softMax = 0.5;
  radius.step = 0.005;
  radius.unit = "m";
  radius.visibleWhen = Condition{"mode", Value::text("radius"), {}};

  Param flip;
  flip.name = "flipTowardsViewpoint";
  flip.type = ParamType::Bool;
  flip.label = "Flip Towards Viewpoint";
  flip.doc = "让法线统一朝向视点。渲染和配准通常需要一致的朝向。";
  flip.def = Value::boolean(true);
  flip.advanced = true;

  Param viewpoint;
  viewpoint.name = "viewpoint";
  viewpoint.type = ParamType::Vec3f;
  viewpoint.label = "Viewpoint";
  viewpoint.def = Value::vec({0.0, 0.0, 0.0});
  viewpoint.step = 0.1;
  viewpoint.unit = "m";
  viewpoint.advanced = true;
  viewpoint.visibleWhen = Condition{"flipTowardsViewpoint", Value::boolean(true), {}};

  op.params = {mode, k, radius, flip, viewpoint};
  op.capabilities = {/*cancellable=*/true, /*previewable=*/false, /*deterministic=*/true};
  op.compute = &compute;

  r.addOperator(std::move(op));
}

}  // namespace lyflow::ops
