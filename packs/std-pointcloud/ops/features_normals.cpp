#include <pcl/features/normal_3d.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/search/kdtree.h>

#include <cmath>

#include "ops.h"
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

  pcl::NormalEstimation<pcl::PointXYZ, pcl::Normal> ne;
  ne.setInputCloud(cloud);
  ne.setSearchMethod(pcl::make_shared<pcl::search::KdTree<pcl::PointXYZ>>());
  if (byRadius) {
    ne.setRadiusSearch(radius);
  } else {
    ne.setKSearch(kSearch);
  }
  if (flip) ne.setViewPoint(viewpoint[0], viewpoint[1], viewpoint[2]);

  pcl::PointCloud<pcl::Normal> normals;
  ne.compute(normals);

  if (ctx.cancelled()) return Status::Ok();
  ctx.progress(0.9f);

  // 输出是「输入点云 + normals 通道」，所以从 in 拷一份而不是 adapter::fromPcl()——
  // 后者只带 xyz，intensity/rgb 会凭空消失。
  PointCloud out = in;
  out.normals.assign(in.pointCount() * 3, 0.0f);
  std::size_t degenerate = 0;
  for (std::size_t i = 0; i < in.pointCount() && i < normals.size(); ++i) {
    const auto& nrm = normals[i];
    if (!std::isfinite(nrm.normal_x) || !std::isfinite(nrm.normal_y) ||
        !std::isfinite(nrm.normal_z)) {
      // 邻域不足时 PCL 填 NaN。这里置零而不是留着：NaN 流到下游，
      // 体素栅格一做平均整片法线就全成了 NaN，再往下就是 3D 视图黑屏。
      ++degenerate;
      continue;
    }
    out.normals[i * 3] = nrm.normal_x;
    out.normals[i * 3 + 1] = nrm.normal_y;
    out.normals[i * 3 + 2] = nrm.normal_z;
  }
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
  op.label = "Estimate Normals";
  op.category = "Features";
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
  op.capabilities = {/*cancellable=*/false, /*previewable=*/false, /*deterministic=*/true};
  op.compute = &compute;

  r.addOperator(std::move(op));
}

}  // namespace lyflow::ops
