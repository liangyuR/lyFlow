#include "algo/icp2d.h"
#include "ops.h"

namespace lyflow::ops {
namespace {

/// SE(2) 的 3x3 ↔ LyFlow 的 4x4 行主序。z 轴原样保留。
Transform toTransform(const Eigen::Matrix3f& m) {
  Transform t;
  t.m[0] = m(0, 0); t.m[1] = m(0, 1); t.m[2] = 0; t.m[3] = m(0, 2);
  t.m[4] = m(1, 0); t.m[5] = m(1, 1); t.m[6] = 0; t.m[7] = m(1, 2);
  t.m[8] = 0; t.m[9] = 0; t.m[10] = 1; t.m[11] = 0;
  t.m[12] = 0; t.m[13] = 0; t.m[14] = 0; t.m[15] = 1;
  return t;
}

Eigen::Matrix3f fromTransform(const Transform& t) {
  Eigen::Matrix3f m = Eigen::Matrix3f::Identity();
  m(0, 0) = t.m[0]; m(0, 1) = t.m[1]; m(0, 2) = t.m[3];
  m(1, 0) = t.m[4]; m(1, 1) = t.m[5]; m(1, 2) = t.m[7];
  return m;
}

Status compute(const Inputs& inputs, const ParamView& params, Outputs& outputs,
               ExecContext& ctx) {
  const PointCloud& source = *inputs.get("source").asCloud();
  const PointCloud& target = *inputs.get("target").asCloud();
  if (source.pointCount() == 0 || target.pointCount() == 0) {
    return Status::Error(Phase::Execute, "bad_input", "源云或目标云是空的", {},
                         source.pointCount() == 0 ? "source" : "target");
  }

  std_pc::Icp2DOptions options;
  options.maxMatchingDistance = params.number("maxMatchingDist");
  options.fitnessDistance = params.number("fitnessDist");
  options.maxIterationCount = static_cast<int>(params.integer("maxIterations"));
  options.normalKnn = static_cast<int>(params.integer("normalKnn"));
  options.smoothLength = static_cast<int>(params.integer("smoothLength"));
  options.minDiffRotErr = params.number("minDiffRot");
  options.minDiffTransErr = params.number("minDiffTrans");

  Eigen::Matrix3f init = Eigen::Matrix3f::Identity();
  if (inputs.has("init")) {
    const Transform* t = inputs.get("init").asTransform();
    if (t != nullptr) init = fromTransform(*t);
  }

  std_pc::Icp2D icp(options);
  icp.setSource(std_pc::toCloud2D(source));
  icp.setTarget(std_pc::toCloud2D(target));
  const std_pc::Icp2DResult result = icp.align(init);

  outputs.set("transform", Data::transform(toTransform(result.transform)));

  Record record;
  record.type = "Icp2DResult";
  record.data["fitness"] = result.fitness;
  record.data["iterations"] = result.iterations;
  record.data["converged"] = result.converged;
  outputs.set("result", Data::record(std::move(record)));

  ctx.log(LogLevel::Info, "icp fitness=" + std::to_string(result.fitness) + " iterations=" +
                              std::to_string(result.iterations) +
                              (result.converged ? " converged" : " not converged"));
  return Status::Ok();
}

Param numParam(const char* name, const char* label, double def, const char* unit, const char* doc) {
  Param p;
  p.name = name;
  p.type = ParamType::Float;
  p.label = label;
  p.doc = doc;
  p.def = Value::number(def);
  p.unit = unit;
  p.min = 0.0;
  return p;
}

Param intParam(const char* name, const char* label, std::int64_t def, const char* doc) {
  Param p;
  p.name = name;
  p.type = ParamType::Int;
  p.label = label;
  p.doc = doc;
  p.def = Value::integer(def);
  p.min = 1.0;
  return p;
}

}  // namespace

void registerRegisterIcp2D(Registry& r) {
  OperatorDesc op;
  op.id = "register.icp_2d";
  op.version = "1.0.0";
  op.label = "ICP 2D";
  op.category = "Register/ICP";
  op.keywords = {"icp", "register", "align", "2d", "配准", "对齐"};
  op.doc =
      "XY 平面上的 point-to-plane ICP。源云的法线由 knn 邻域现估，"
      "收敛判据是最近 Smooth Length 步的平均旋转/平移增量都低于阈值。\n"
      "输出的 Transform 是把源云变到目标云的 4x4（z 轴恒等）。";
  op.inputs = {
      Port{"source", "PointCloud", "Source", "待配准的点云。", true},
      Port{"target", "PointCloud", "Target", "参考点云。", true},
      Port{"init", "Transform", "Init", "可选：初始位姿。不接就是单位阵。", false},
  };
  op.outputs = {
      Port{"transform", "Transform", "Transform", "源 → 目标的变换。", true},
      Port{"result", "Record", "Result", "Icp2DResult：fitness、iterations、converged。", true},
  };

  op.params = {
      numParam("maxMatchingDist", "Max Matching Dist", 0.001, "m",
               "配对距离上限，超过就不算一对。"),
      numParam("fitnessDist", "Fitness Dist", 0.01, "m", "算 fitness 时的距离阈值。"),
      intParam("maxIterations", "Max Iterations", 1000, "最大迭代次数。"),
      intParam("normalKnn", "Normal KNN", 10, "估源云法线的邻域大小。"),
      intParam("smoothLength", "Smooth Length", 4, "收敛判据的滑动窗长度。"),
      numParam("minDiffRot", "Min Diff Rot", 1e-3, "rad", "平均旋转增量低于它就算收敛。"),
      numParam("minDiffTrans", "Min Diff Trans", 1e-4, "m", "平均平移增量低于它就算收敛。"),
  };
  op.capabilities = {/*cancellable=*/false, /*previewable=*/false, /*deterministic=*/true};
  op.compute = &compute;

  r.addOperator(std::move(op));
}

}  // namespace lyflow::ops
