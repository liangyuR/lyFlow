#include <cmath>

#include "ops.h"

namespace lyflow::ops {
namespace {

Status compute(const Inputs&, const ParamView& params, Outputs& outputs, ExecContext&) {
  const std::array<float, 3> t = params.vec3("translation");
  const std::array<float, 3> deg = params.vec3("rotation");

  const double toRad = 3.14159265358979323846 / 180.0;
  const double rx = deg[0] * toRad, ry = deg[1] * toRad, rz = deg[2] * toRad;
  const double cx = std::cos(rx), sx = std::sin(rx);
  const double cy = std::cos(ry), sy = std::sin(ry);
  const double cz = std::cos(rz), sz = std::sin(rz);

  // 内旋 X→Y→Z，即 R = Rz * Ry * Rx。这是 ROS / PCL 一系的惯例，
  // 换一种约定会让同一组欧拉角在不同工具里得到不同结果，必须写死并写清楚。
  const double m00 = cz * cy;
  const double m01 = cz * sy * sx - sz * cx;
  const double m02 = cz * sy * cx + sz * sx;
  const double m10 = sz * cy;
  const double m11 = sz * sy * sx + cz * cx;
  const double m12 = sz * sy * cx - cz * sx;
  const double m20 = -sy;
  const double m21 = cy * sx;
  const double m22 = cy * cx;

  Transform out;
  out.m[0] = static_cast<float>(m00); out.m[1] = static_cast<float>(m01); out.m[2] = static_cast<float>(m02); out.m[3] = t[0];
  out.m[4] = static_cast<float>(m10); out.m[5] = static_cast<float>(m11); out.m[6] = static_cast<float>(m12); out.m[7] = t[1];
  out.m[8] = static_cast<float>(m20); out.m[9] = static_cast<float>(m21); out.m[10] = static_cast<float>(m22); out.m[11] = t[2];
  out.m[12] = 0; out.m[13] = 0; out.m[14] = 0; out.m[15] = 1;

  outputs.set("transform", Data::transform(out));
  return Status::Ok();
}

}  // namespace

void registerTransformMake(Registry& r) {
  OperatorDesc op;
  op.id = "transform.make";
  op.version = "1.0.0";
  op.label = "构造变换";
  op.category = "变换";
  op.keywords = {"transform", "pose", "translate", "rotate", "变换", "位姿", "平移", "旋转"};
  op.doc = "由平移和欧拉角构造 4x4 刚体变换。旋转顺序为内旋 X→Y→Z（R = Rz·Ry·Rx）。";

  op.outputs = {Port{"transform", "Transform", "Transform", "构造出的变换。", true}};

  Param t;
  t.name = "translation";
  t.type = ParamType::Vec3f;
  t.label = "Translation";
  t.def = Value::vec({0.0, 0.0, 0.0});
  t.step = 0.01;
  t.unit = "m";

  Param rot;
  rot.name = "rotation";
  rot.type = ParamType::Vec3f;
  rot.label = "Rotation";
  rot.doc = "欧拉角，单位是度。";
  rot.def = Value::vec({0.0, 0.0, 0.0});
  rot.softMin = -180.0;
  rot.softMax = 180.0;
  rot.step = 1.0;
  rot.unit = "°";
  rot.componentLabels = {"RX", "RY", "RZ"};

  op.params = {t, rot};
  op.capabilities = {/*cancellable=*/false, /*previewable=*/false, /*deterministic=*/true};
  op.compute = &compute;

  r.addOperator(std::move(op));
}

}  // namespace lyflow::ops
