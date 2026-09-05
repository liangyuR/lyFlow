#include <pcl/ModelCoefficients.h>
#include <pcl/PointIndices.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/sample_consensus/method_types.h>
#include <pcl/sample_consensus/model_types.h>
#include <pcl/segmentation/sac_segmentation.h>

#include <cmath>

#include "ops/ops.h"
#include "ops/pcl/adapter.h"

namespace lyflow::ops {
namespace {

constexpr double kPi = 3.14159265358979323846;

Status compute(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext& ctx) {
  const PointCloud& in = *inputs.get("cloud").asCloud();
  const double distanceThreshold = params.number("distanceThreshold");
  const auto maxIterations = static_cast<int>(params.integer("maxIterations"));
  const bool useAxis = params.flag("constrainAxis");
  const std::array<float, 3> axis = params.vec3("axis");
  const double epsAngleDeg = params.number("epsAngle");

  if (in.pointCount() < 3) {
    return Status::Error(Phase::Execute, "bad_input", "拟合平面至少需要 3 个点", {}, "cloud");
  }
  if (useAxis) {
    const double len = std::sqrt(static_cast<double>(axis[0]) * axis[0] +
                                 static_cast<double>(axis[1]) * axis[1] +
                                 static_cast<double>(axis[2]) * axis[2]);
    if (len < 1e-6) {
      return Status::Error(Phase::Execute, "bad_param", "轴约束的方向向量不能是零向量", "axis");
    }
  }

  ctx.progress(0.1f, "RANSAC");
  auto cloud = adapter::toPcl(in);

  pcl::SACSegmentation<pcl::PointXYZ> seg;
  seg.setOptimizeCoefficients(true);
  // 带轴约束时换成 PERPENDICULAR_PLANE：地面提取里「最大的平面」常是墙面，
  // 加约束比事后拿法线去筛省事。
  seg.setModelType(useAxis ? pcl::SACMODEL_PERPENDICULAR_PLANE : pcl::SACMODEL_PLANE);
  seg.setMethodType(pcl::SAC_RANSAC);
  seg.setDistanceThreshold(distanceThreshold);
  seg.setMaxIterations(maxIterations);
  if (useAxis) {
    seg.setAxis(Eigen::Vector3f(axis[0], axis[1], axis[2]));
    seg.setEpsAngle(epsAngleDeg * kPi / 180.0);
  }
  seg.setInputCloud(cloud);

  pcl::PointIndices inliers;
  pcl::ModelCoefficients coefficients;
  seg.segment(inliers, coefficients);

  if (ctx.cancelled()) return Status::Ok();
  ctx.progress(0.9f);

  if (inliers.indices.empty() || coefficients.values.size() < 4) {
    return Status::Error(Phase::Execute, "bad_input",
                         "在给定阈值下找不到平面（试着放大 Distance Threshold 或放宽轴约束）",
                         "distanceThreshold");
  }

  Indices out;
  out.sourceCloudId = in.id;
  out.values = adapter::fromPclIndices(inliers);

  Plane plane;
  // PCL 的 ax+by+cz+d=0 与本项目的 n·p+d=0 是同一个式子，法向量已归一化。
  plane.normal[0] = coefficients.values[0];
  plane.normal[1] = coefficients.values[1];
  plane.normal[2] = coefficients.values[2];
  plane.d = coefficients.values[3];

  ctx.log(LogLevel::Info, "平面内点 " + std::to_string(out.values.size()) + " / " +
                              std::to_string(in.pointCount()));
  outputs.set("inliers", Data::indices(std::move(out)));
  outputs.set("plane", Data::plane(plane));
  return Status::Ok();
}

}  // namespace

void registerSegmentRansacPlane(Registry& r) {
  OperatorDesc op;
  op.id = "segment.ransac_plane";
  op.version = "1.0.0";
  op.label = "RANSAC Plane";
  op.category = "Segment";
  op.keywords = {"plane", "ransac", "ground", "segmentation", "平面", "地面", "分割"};
  op.doc = "用 RANSAC 拟合最大平面，输出内点下标与平面方程。接 Extract Indices 就能把地面分离出去。";

  op.inputs = {Port{"cloud", "PointCloud", "Cloud", "输入点云。", true}};
  op.outputs = {
      Port{"inliers", "Indices", "Inliers", "落在平面上的点下标。", true},
      Port{"plane",   "Plane",   "Plane",   "平面方程 n·p + d = 0。", true},
  };

  Param dist;
  dist.name = "distanceThreshold";
  dist.type = ParamType::Float;
  dist.label = "Distance Threshold";
  dist.doc = "点到平面的距离小于此值算内点。大致取传感器噪声的 2~3 倍。";
  dist.def = Value::number(0.01);
  dist.min = 1e-6;
  dist.max = 100.0;
  dist.softMax = 0.1;
  dist.step = 0.002;
  dist.unit = "m";

  Param iters;
  iters.name = "maxIterations";
  iters.type = ParamType::Int;
  iters.label = "Max Iterations";
  iters.def = Value::integer(200);
  iters.min = 1.0;
  iters.max = 1000000.0;
  iters.softMax = 2000.0;
  iters.advanced = true;

  Param constrain;
  constrain.name = "constrainAxis";
  constrain.type = ParamType::Bool;
  constrain.label = "Constrain To Axis";
  constrain.doc = "只接受法向量接近给定轴的平面。找地面时打开，否则可能拟合到墙上。";
  constrain.def = Value::boolean(false);
  constrain.group = "Axis Constraint";

  Param axis;
  axis.name = "axis";
  axis.type = ParamType::Vec3f;
  axis.label = "Axis";
  axis.def = Value::vec({0.0, 0.0, 1.0});
  axis.min = -1.0;
  axis.max = 1.0;
  axis.step = 0.1;
  axis.group = "Axis Constraint";
  axis.visibleWhen = Condition{"constrainAxis", Value::boolean(true), {}};

  Param eps;
  eps.name = "epsAngle";
  eps.type = ParamType::Float;
  eps.label = "Angle Tolerance";
  eps.doc = "允许的法向量偏离角度。";
  eps.def = Value::number(15.0);
  eps.min = 0.0;
  eps.max = 90.0;
  eps.step = 1.0;
  eps.unit = "deg";
  eps.group = "Axis Constraint";
  eps.visibleWhen = Condition{"constrainAxis", Value::boolean(true), {}};

  op.params = {dist, iters, constrain, axis, eps};
  // PCL 的 segment() 一旦进去就出不来，没有轮询点。如实申报 false ——
  // 前端据此知道这个节点按 Esc 不会立刻停。
  op.capabilities = {/*cancellable=*/false, /*previewable=*/false, /*deterministic=*/true};
  op.compute = &compute;

  r.addOperator(std::move(op));
}

}  // namespace lyflow::ops
