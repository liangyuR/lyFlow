#include <cmath>

#include "ops.h"

namespace lyflow::ops {
namespace {

Status compute(const Inputs& inputs, const ParamView&, Outputs& outputs, ExecContext& ctx) {
  const PointCloud& in = *inputs.get("cloud").asCloud();
  const Transform& t = *inputs.get("transform").asTransform();

  PointCloud out;
  const std::size_t n = in.pointCount();
  out.xyz.resize(n * 3);
  out.intensity = in.intensity;
  out.rgb = in.rgb;
  if (in.hasNormals()) out.normals.resize(n * 3);

  Ticker ticker(ctx, n);
  for (std::size_t i = 0; i < n; ++i) {
    if (ticker.tick(i)) return Status::Ok();
    const float x = in.xyz[i * 3], y = in.xyz[i * 3 + 1], z = in.xyz[i * 3 + 2];
    out.xyz[i * 3]     = t.m[0] * x + t.m[1] * y + t.m[2] * z + t.m[3];
    out.xyz[i * 3 + 1] = t.m[4] * x + t.m[5] * y + t.m[6] * z + t.m[7];
    out.xyz[i * 3 + 2] = t.m[8] * x + t.m[9] * y + t.m[10] * z + t.m[11];

    // 法线只吃旋转部分，不吃平移 —— 漏了这条的表现是「变换之后着色全乱」，
    // 而且只在下游真的用到法线时才暴露。
    if (in.hasNormals()) {
      const float nx = in.normals[i * 3], ny = in.normals[i * 3 + 1], nz = in.normals[i * 3 + 2];
      float ox = t.m[0] * nx + t.m[1] * ny + t.m[2] * nz;
      float oy = t.m[4] * nx + t.m[5] * ny + t.m[6] * nz;
      float oz = t.m[8] * nx + t.m[9] * ny + t.m[10] * nz;
      const float len = std::sqrt(ox * ox + oy * oy + oz * oz);
      if (len > 1e-9f) { ox /= len; oy /= len; oz /= len; }
      out.normals[i * 3] = ox;
      out.normals[i * 3 + 1] = oy;
      out.normals[i * 3 + 2] = oz;
    }
  }

  outputs.set("cloud", Data::cloud(std::move(out)));
  return Status::Ok();
}

}  // namespace

void registerTransformApply(Registry& r) {
  OperatorDesc op;
  op.id = "transform.apply";
  op.version = "1.0.0";
  op.label = "应用变换";
  op.category = "变换";
  op.keywords = {"transform", "apply", "rigid", "变换", "应用"};
  op.doc = "把 4x4 变换作用到点云上。法线同时被旋转并重新归一化。";

  op.inputs = {
      Port{"cloud",     "PointCloud", "Cloud",     "输入点云。", true},
      Port{"transform", "Transform",  "Transform", "要作用的变换。", true},
  };
  op.outputs = {Port{"cloud", "PointCloud", "Cloud", "变换后的点云。", true}};

  op.capabilities = {/*cancellable=*/true, /*previewable=*/true, /*deterministic=*/true};
  op.compute = &compute;

  r.addOperator(std::move(op));
}

}  // namespace lyflow::ops
