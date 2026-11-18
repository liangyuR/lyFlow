#include <algorithm>
#include <cmath>

#include "cloud_transform.h"
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
  if (in.hasNormals())
    out.normals.resize(n * 3);

  Ticker ticker(ctx, n);
  for (std::size_t i = 0; i < n; ++i) {
    if (ticker.tick(i))
      return Status::Error(Phase::Execute, "cancelled", "点云变换已取消");
    const float* p = &in.xyz[i * 3];
    const auto transformed = transformPoint(t, p);
    if (std::any_of(p, p + 3, [](float x) { return std::isinf(x); }) ||
        (std::all_of(p, p + 3, [](float x) { return std::isfinite(x); }) &&
         !std::all_of(transformed.begin(), transformed.end(),
                      [](float x) { return std::isfinite(x); })))
      return Status::Error(Phase::Execute, "bad_input", "点云坐标超出 float32 有限范围", {},
                           "cloud");
    std::copy(transformed.begin(), transformed.end(), &out.xyz[i * 3]);

    if (in.hasNormals()) {
      const double nx = in.normals[i * 3], ny = in.normals[i * 3 + 1], nz = in.normals[i * 3 + 2];
      double ox = t.m[0] * nx + t.m[1] * ny + t.m[2] * nz;
      double oy = t.m[4] * nx + t.m[5] * ny + t.m[6] * nz;
      double oz = t.m[8] * nx + t.m[9] * ny + t.m[10] * nz;
      const double len = std::hypot(ox, oy, oz);
      if (!std::isfinite(len))
        return Status::Error(Phase::Execute, "bad_input", "点云法线必须有限", {}, "cloud");
      if (len > 0) {
        ox /= len;
        oy /= len;
        oz /= len;
      }
      out.normals[i * 3] = static_cast<float>(ox);
      out.normals[i * 3 + 1] = static_cast<float>(oy);
      out.normals[i * 3 + 2] = static_cast<float>(oz);
    }
  }

  outputs.set("cloud", Data::cloud(std::move(out)));
  return Status::Ok();
}

} // namespace

void registerTransformApply(Registry& r) {
  OperatorDesc op;
  op.id = "transform.apply";
  op.version = "1.1.0";
  op.label = "点云变换";
  op.category = "变换";
  op.keywords = {"point cloud transformation",
                 "rigid transform application",
                 "coordinate transformation",
                 "坐标变换",
                 "刚体变换",
                 "法线旋转",
                 "transform",
                 "apply",
                 "rigid",
                 "变换",
                 "应用"};
  op.doc = "把 4x4 变换作用到点云上。法线同时被旋转并重新归一化。";

  op.inputs = {
      Port{"cloud", "PointCloud", "Cloud", "输入点云。", true},
      Port{"transform", "Transform", "Transform", "要作用的变换。", true},
  };
  op.outputs = {Port{"cloud", "PointCloud", "Cloud", "变换后的点云。", true}};

  op.capabilities = {/*cancellable=*/true, /*previewable=*/true, /*deterministic=*/true};
  op.compute = &compute;

  r.addOperator(std::move(op));
}

} // namespace lyflow::ops
