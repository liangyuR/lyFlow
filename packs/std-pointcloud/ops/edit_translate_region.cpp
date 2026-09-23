#include <cmath>

#include "ops.h"

namespace lyflow::ops {
namespace {

Status compute(const Inputs& inputs, const ParamView& params, Outputs& outputs,
               ExecContext& ctx) {
  const PointCloud& in = *inputs.get("cloud").asCloud();
  const std::string& kind = params.choice("regionKind");
  const std::array<float, 3> t = params.vec3("translation");

  std::array<float, 3> point{}, normal{}, lo{}, hi{};
  if (kind == "halfspace") {
    point = params.vec3("point");
    normal = params.vec3("normal");
    const double len = std::sqrt(static_cast<double>(normal[0]) * normal[0] +
                                static_cast<double>(normal[1]) * normal[1] +
                                static_cast<double>(normal[2]) * normal[2]);
    if (!(len > 1e-9)) {
      return Status::Error(Phase::Execute, "bad_param",
                           "半空间的法向是零向量，分不出哪一侧", "normal");
    }
  } else {
    lo = params.vec3("boxMin");
    hi = params.vec3("boxMax");
    for (int i = 0; i < 3; ++i) {
      if (lo[i] > hi[i]) {
        return Status::Error(Phase::Execute, "bad_param",
                             std::string("Box Min 的第 ") + static_cast<char>('X' + i) +
                                 " 分量必须不大于 Box Max 的对应分量",
                             "boxMin");
      }
    }
  }

  const std::size_t n = in.pointCount();
  PointCloud out;
  out.xyz = in.xyz;
  out.intensity = in.intensity;
  out.normals = in.normals;
  out.rgb = in.rgb;

  Ticker ticker(ctx, n);
  for (std::size_t p = 0; p < n; ++p) {
    if (ticker.tick(p)) return Status::Ok();
    const float x = in.xyz[p * 3], y = in.xyz[p * 3 + 1], z = in.xyz[p * 3 + 2];
    bool selected;
    if (kind == "halfspace") {
      const double d = static_cast<double>(x - point[0]) * normal[0] +
                       static_cast<double>(y - point[1]) * normal[1] +
                       static_cast<double>(z - point[2]) * normal[2];
      selected = d > 0.0;
    } else {
      selected = x >= lo[0] && x <= hi[0] && y >= lo[1] && y <= hi[1] && z >= lo[2] && z <= hi[2];
    }
    if (!selected) continue;
    out.xyz[p * 3] = x + t[0];
    out.xyz[p * 3 + 1] = y + t[1];
    out.xyz[p * 3 + 2] = z + t[2];
  }

  outputs.set("cloud", Data::cloud(std::move(out)));
  return Status::Ok();
}

}

void registerEditTranslateRegion(Registry& r) {
  OperatorDesc op;
  op.id = "edit.translate_region";
  op.version = "1.0.0";
  op.label = "平移选区";
  op.category = "编辑";
  op.keywords = {"translate", "region", "perturb", "halfspace", "box",
                 "平移", "选区", "扰动", "半空间", "合成位移"};
  op.doc =
      "把几何选区内的点整体平移，其余点原样。选区是半空间（点 + 法向，取 dot(p-point, "
      "normal) > 0 的一侧）或轴对齐盒（闭区间）。平移量与选区参数（point / boxMin / boxMax）"
      "和输入云同帧同单位，都是米 —— 传感器帧与测量帧都是米（gap.to_measurement_frame "
      "只交换 y 与 z，不换单位），只有 Measurement 那类读数是毫米，所以 lyflow perturb 的 "
      "--expect 写 ±1000 而不是 ±1。点数与点序不变，intensity / rgb / normals 原样带过。"
      "主要用途是合成位移：把缝的一侧推开若干毫米，看读数跟不跟得上（lyflow perturb）。"
      "法向通道不跟着转：只做平移，不做旋转。\n"
      "选区是**纯几何**的，不认识缝、壁、锚点这些语义：切分面穿过一面近竖直的壁时，壁上一半"
      "的点会跟着动；压在下游要用的锚点上时，锚点跟着动而读出「完美跟随」—— 选区必须先在"
      "剖面上核对。";

  op.inputs = {Port{"cloud", "PointCloud", "Cloud", "待扰动的点云。", true}};
  op.outputs = {Port{"cloud", "PointCloud", "Cloud", "选区内已平移的点云。", true}};

  Param kind;
  kind.name = "regionKind";
  kind.type = ParamType::Enum;
  kind.label = "Region Kind";
  kind.doc = "选区形状。";
  kind.def = Value::text("halfspace");
  kind.options = {
      EnumOption{"halfspace", "半空间", "取 dot(p - point, normal) > 0 的那一侧。"},
      EnumOption{"box", "盒", "轴对齐长方体，闭区间。"},
  };

  Param point;
  point.name = "point";
  point.type = ParamType::Vec3f;
  point.label = "Point";
  point.doc = "半空间切分面上的一点。与输入云同帧同单位，是米。";
  point.def = Value::vec({0.0, 0.0, 0.0});
  point.step = 0.001;
  point.unit = "m";
  point.visibleWhen.param = "regionKind";
  point.visibleWhen.eq = Value::text("halfspace");

  Param normal;
  normal.name = "normal";
  normal.type = ParamType::Vec3f;
  normal.label = "Normal";
  normal.doc = "半空间的法向，指向被选中的那一侧。零向量是错误。";
  normal.def = Value::vec({1.0, 0.0, 0.0});
  normal.step = 0.01;
  normal.visibleWhen.param = "regionKind";
  normal.visibleWhen.eq = Value::text("halfspace");

  Param boxMin;
  boxMin.name = "boxMin";
  boxMin.type = ParamType::Vec3f;
  boxMin.label = "Box Min";
  boxMin.doc = "盒的下界，闭区间。";
  boxMin.def = Value::vec({-1.0, -1.0, -1.0});
  boxMin.step = 0.001;
  boxMin.unit = "m";
  boxMin.visibleWhen.param = "regionKind";
  boxMin.visibleWhen.eq = Value::text("box");

  Param boxMax;
  boxMax.name = "boxMax";
  boxMax.type = ParamType::Vec3f;
  boxMax.label = "Box Max";
  boxMax.doc = "盒的上界，闭区间。";
  boxMax.def = Value::vec({1.0, 1.0, 1.0});
  boxMax.step = 0.001;
  boxMax.unit = "m";
  boxMax.visibleWhen.param = "regionKind";
  boxMax.visibleWhen.eq = Value::text("box");

  Param translation;
  translation.name = "translation";
  translation.type = ParamType::Vec3f;
  translation.label = "Translation";
  translation.doc = "选区内的点加上的位移。与输入云同帧同单位，是米：1 mm 写 0.001。";
  translation.def = Value::vec({0.0, 0.0, 0.0});
  translation.step = 0.001;
  translation.unit = "m";

  op.params = {kind, point, normal, boxMin, boxMax, translation};
  op.capabilities.cancellable = true;
  op.capabilities.previewable = true;
  op.capabilities.deterministic = true;
  op.compute = &compute;

  r.addOperator(std::move(op));
}

}
