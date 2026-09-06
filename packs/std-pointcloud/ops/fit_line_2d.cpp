#include <cmath>

#include "algo/fit2d.h"
#include "ops.h"

namespace lyflow::ops {
namespace {

/// 直线与轴对齐框的两个交点。参数式求交，取 t 区间的两端。
/// 框外或平行时返回 false，此时 line 只带方程不带端点。
bool clipLineToBox(const Line2D& line, const Box2D& box, float out[2][2]) {
  double tLo = -1e30, tHi = 1e30;
  const double p[2] = {line.point[0], line.point[1]};
  const double d[2] = {line.dir[0], line.dir[1]};
  for (int i = 0; i < 2; ++i) {
    const double lo = box.min[i], hi = box.max[i];
    if (std::fabs(d[i]) < 1e-12) {
      if (p[i] < lo || p[i] > hi) return false;
      continue;
    }
    double t0 = (lo - p[i]) / d[i];
    double t1 = (hi - p[i]) / d[i];
    if (t0 > t1) std::swap(t0, t1);
    tLo = std::max(tLo, t0);
    tHi = std::min(tHi, t1);
  }
  if (tLo > tHi || tLo <= -1e30 || tHi >= 1e30) return false;
  for (int i = 0; i < 2; ++i) {
    out[0][i] = static_cast<float>(p[i] + tLo * d[i]);
    out[1][i] = static_cast<float>(p[i] + tHi * d[i]);
  }
  return true;
}

Status compute(const Inputs& inputs, const ParamView& params, Outputs& outputs,
               ExecContext& ctx) {
  const PointCloud& in = *inputs.get("cloud").asCloud();
  if (in.pointCount() < 2) {
    return Status::Error(Phase::Execute, "bad_input", "点太少，拟合不了直线", {}, "cloud");
  }

  std_pc::Line2DFitOptions options;
  options.distThresh = static_cast<float>(params.number("distThresh"));
  options.maxIterations = static_cast<int>(params.integer("maxIterations"));
  options.optimize = params.flag("optimize");
  if (!(options.distThresh > 0)) {
    return Status::Error(Phase::Execute, "bad_param", "内点距离必须为正", "distThresh");
  }

  const std_pc::Cloud2D cloud = std_pc::toCloud2D(in);
  Eigen::VectorXf coefficients;
  pcl::Indices indices;
  if (!std_pc::fitLine2D(cloud, &coefficients, &indices, options)) {
    return Status::Error(Phase::Execute, "fit_failed", "直线拟合失败（内点不足）", {}, "cloud");
  }

  Line2D line;
  line.point[0] = coefficients[0];
  line.point[1] = coefficients[1];
  const float norm = std::hypot(coefficients[3], coefficients[4]);
  line.dir[0] = norm > 0 ? coefficients[3] / norm : 1.0f;
  line.dir[1] = norm > 0 ? coefficients[4] / norm : 0.0f;

  if (inputs.has("clipTo")) {
    const Box2D* box = inputs.get("clipTo").asBox2D();
    float ends[2][2] = {};
    if (box != nullptr && clipLineToBox(line, *box, ends)) {
      line.hasSegment = true;
      line.start[0] = ends[0][0];
      line.start[1] = ends[0][1];
      line.end[0] = ends[1][0];
      line.end[1] = ends[1][1];
    } else {
      ctx.log(LogLevel::Warn, "直线与 Clip To 框没有两个交点，line 只带方程不带端点");
    }
  }
  outputs.set("line", Data::line2d(line));

  Indices out;
  out.sourceCloudId = in.id;
  out.values.reserve(indices.size());
  for (auto i : indices) out.values.push_back(static_cast<std::int32_t>(i));
  outputs.set("inliers", Data::indices(std::move(out)));
  return Status::Ok();
}

}  // namespace

void registerFitLine2D(Registry& r) {
  OperatorDesc op;
  op.id = "fit.line_2d";
  op.version = "1.0.0";
  op.label = "Fit Line 2D";
  op.category = "Fit/Line";
  op.keywords = {"line", "ransac", "fit", "2d", "直线", "拟合"};
  op.doc =
      "在 XY 平面上拟合一条直线（z 不参与）。RANSAC 的 random 关掉，同输入同结果。\n"
      "线扫剖面常带两条近平行的迹线：假设选择用 1/3 阈值隔出单条，"
      "再在补集里搜一次伴线，两条平行且间距合理时取靠上那条。";
  op.inputs = {
      Port{"cloud", "PointCloud", "Cloud", "待拟合的点云，只看 XY。", true},
      Port{"clipTo", "Box2D", "Clip To", "可选：拿这个框截出线段的两个端点。", false},
  };
  op.outputs = {
      Port{"line", "Line2D", "Line", "拟合出的直线；接了 Clip To 就带端点。", true},
      Port{"inliers", "Indices", "Inliers", "内点下标，指向输入点云。", true},
  };

  Param dist;
  dist.name = "distThresh";
  dist.type = ParamType::Float;
  dist.label = "Dist Thresh";
  dist.doc = "内点判定距离。假设选择阶段用它的 1/3。";
  dist.def = Value::number(0.0001);
  dist.unit = "m";
  dist.min = 0.0;
  dist.step = 0.0001;

  Param iterations;
  iterations.name = "maxIterations";
  iterations.type = ParamType::Int;
  iterations.label = "Max Iterations";
  iterations.doc = "RANSAC 的最大迭代次数。";
  iterations.def = Value::integer(1000);
  iterations.min = 1.0;

  Param optimize;
  optimize.name = "optimize";
  optimize.type = ParamType::Bool;
  optimize.label = "Optimize";
  optimize.doc = "拿内点做一次最小二乘精化。关掉就是 RANSAC 的原始假设。";
  optimize.def = Value::boolean(true);
  optimize.advanced = true;

  op.params = {dist, iterations, optimize};
  op.capabilities = {/*cancellable=*/false, /*previewable=*/true, /*deterministic=*/true};
  op.compute = &compute;

  r.addOperator(std::move(op));
}

}  // namespace lyflow::ops
