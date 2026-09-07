#include "algo/fit2d.h"
#include "ops.h"

namespace lyflow::ops {
namespace {

Status compute(const Inputs& inputs, const ParamView& params, Outputs& outputs,
               ExecContext& ctx) {
  const PointCloud& in = *inputs.get("cloud").asCloud();
  if (in.pointCount() < 3) {
    return Status::Error(Phase::Execute, "bad_input", "点太少，拟合不了圆", {}, "cloud");
  }

  std_pc::Circle2DFitOptions options;
  options.distThresh = static_cast<float>(params.number("distThresh"));
  options.minRadius = params.number("rMin");
  options.maxRadius = params.number("rMax");
  options.maxIterations = static_cast<int>(params.integer("maxIterations"));
  const double fixedRadius = params.number("fixedRadius");
  if (!(options.maxRadius > options.minRadius)) {
    return Status::Error(Phase::Execute, "bad_param", "半径上限必须大于下限", "rMax");
  }
  if (fixedRadius > 0 && !(options.maxRadius > fixedRadius && options.minRadius < fixedRadius)) {
    return Status::Error(Phase::Execute, "bad_param", "固定半径必须落在上下限之间", "fixedRadius");
  }

  const std_pc::Cloud2D cloud = std_pc::toCloud2D(in);
  Eigen::VectorXf coefficients;
  pcl::Indices indices;
  const bool ok = fixedRadius > 0
                      ? std_pc::fitCircleFixedRadius2D(cloud, fixedRadius, &coefficients, &indices,
                                                       options)
                      : std_pc::fitCircle2D(cloud, &coefficients, &indices, options);
  if (!ok) {
    return Status::Error(Phase::Execute, "fit_failed", "圆拟合失败（内点不足）", {}, "cloud");
  }

  Circle2D circle;
  circle.center[0] = coefficients[0];
  circle.center[1] = coefficients[1];
  circle.radius = coefficients[2];
  outputs.set("circle", Data::circle2d(circle));

  Indices out;
  out.sourceCloudId = in.id;
  out.values.reserve(indices.size());
  for (auto i : indices) out.values.push_back(static_cast<std::int32_t>(i));
  outputs.set("inliers", Data::indices(std::move(out)));
  ctx.log(LogLevel::Info, "圆心 (" + std::to_string(circle.center[0]) + ", " +
                              std::to_string(circle.center[1]) + ") 半径 " +
                              std::to_string(circle.radius) + "，内点 " +
                              std::to_string(indices.size()));
  return Status::Ok();
}

Param numParam(const char* name, const char* label, double def, const char* doc) {
  Param p;
  p.name = name;
  p.type = ParamType::Float;
  p.label = label;
  p.doc = doc;
  p.def = Value::number(def);
  p.unit = "m";
  p.min = 0.0;
  p.step = 0.0001;
  return p;
}

}  // namespace

void registerFitCircle2D(Registry& r) {
  OperatorDesc op;
  op.id = "fit.circle_2d";
  op.version = "1.0.0";
  op.label = "拟合圆 2D";
  op.category = "拟合/圆";
  op.keywords = {"circle", "ransac", "fit", "2d", "圆", "拟合"};
  op.doc =
      "在 XY 平面上拟合一个圆（PCL 的 SACMODEL_CIRCLE2D）。\n"
      "Fixed Radius 大于 0 时半径被钉死：先按上下限拟合一次，"
      "再用定半径最小二乘重定圆心并按新圆重筛内点。";
  op.inputs = {Port{"cloud", "PointCloud", "Cloud", "待拟合的点云，只看 XY。", true}};
  op.outputs = {
      Port{"circle", "Circle2D", "Circle", "拟合出的圆。", true},
      Port{"inliers", "Indices", "Inliers", "内点下标，指向输入点云。", true},
  };

  Param iterations;
  iterations.name = "maxIterations";
  iterations.type = ParamType::Int;
  iterations.label = "Max Iterations";
  iterations.doc = "RANSAC 的最大迭代次数。";
  iterations.def = Value::integer(10000);
  iterations.min = 1.0;

  op.params = {
      numParam("distThresh", "Dist Thresh", 0.00003, "内点判定距离。"),
      numParam("rMin", "Radius Min", 0.0005, "半径搜索下限。"),
      numParam("rMax", "Radius Max", 0.01, "半径搜索上限。"),
      iterations,
      numParam("fixedRadius", "Fixed Radius", 0.0, "钉死半径。0 = 自由半径。"),
  };
  op.capabilities = {/*cancellable=*/false, /*previewable=*/true, /*deterministic=*/true};
  op.compute = &compute;

  r.addOperator(std::move(op));
}

}  // namespace lyflow::ops
