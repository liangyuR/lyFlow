#include <cmath>

#include "lyflow/operator.h"
#include "lyflow/registry.h"
#include "lyflow/kernel/rigid.h"

namespace lyflow::packs::std_geometry {
namespace {

Status compute(const Inputs&, const ParamView& params, Outputs& outputs, ExecContext&) {
  const std::array<float, 3> t = params.vec3("translation");
  const std::array<float, 3> deg = params.vec3("rotation");

  const Transform out = kernel::legacyEulerDegrees(t, deg);

  outputs.set("transform", Data::transform(out));
  return Status::Ok();
}

} // namespace

void registerTransformMake(Registry& r) {
  OperatorDesc op;
  op.id = "transform.make";
  op.version = "1.0.0";
  op.label = "刚体变换构造";
  op.category = "变换";
  op.keywords = {"rigid transform construction",
                 "pose matrix",
                 "Euler angles",
                 "SE3",
                 "位姿矩阵",
                 "欧拉角",
                 "平移旋转",
                 "transform",
                 "pose",
                 "translate",
                 "rotate",
                 "变换",
                 "位姿",
                 "平移",
                 "旋转"};
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

} // namespace lyflow::packs::std_geometry
