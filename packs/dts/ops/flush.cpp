#include <cmath>

#include "dts_ops.h"

namespace lyflow::dts {
namespace {

Status compute(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext&) {
  const nlohmann::json* rj = recordData(inputs.get("root"), "DtsRoot");
  const Line2D* metal = inputs.get("metal").asLine2D();
  if (rj == nullptr || metal == nullptr) {
    return Status::Error(Phase::Execute, "type_mismatch", "root / metal 类型不对", {}, "root");
  }
  Line line;
  line.px = metal->point[0];
  line.pz = metal->point[1];
  line.dx = metal->dir[0];
  line.dz = metal->dir[1];
  const double rx = rj->value("x", 0.0);
  const double rz = rj->value("z", 0.0);
  const double signed_ = line.signedDistance(rx, rz);
  const double toward = params.choice("sealBulge") == "+z" ? signed_ : -signed_;
  const double flush = params.choice("positiveDirection") == "deeper" ? -toward : toward;

  Measurement m;
  m.value = flush;
  m.ok = std::isfinite(flush);
  m.unit = "mm";
  if (!m.ok) m.message = "面差算出来不是有限值";

  const double foot0 = rx - signed_ * line.nx();
  const double foot1 = rz - signed_ * line.nz();
  outputs.set("flush", Data::measurement(m));
  Point2D foot;
  foot.p[0] = static_cast<float>(foot0);
  foot.p[1] = static_cast<float>(foot1);
  outputs.set("foot", Data::point2d(foot));
  return Status::Ok();
}

}  // namespace

void registerFlush(Registry& r) {
  OperatorDesc op;
  op.id = "dts.flush";
  op.version = "1.0.0";
  op.label = "面差";
  op.category = "DTS/面差";
  op.keywords = {"flush", "面差", "垂距"};
  op.doc =
      "胶条底部到基准线的垂距。判定不在这里做 —— 标准表按 (段号, 站位号) 存在库里，\n"
      "图拿不到也不该拿，所以 verdict / 公差留空。";
  op.inputs = {Port{"root", "Record", "Root", "胶条底部。", true},
               Port{"metal", "Line2D", "Metal", "钣金基准线。", true}};
  op.outputs = {Port{"flush", "Measurement", "Flush", "面差，单位 mm。", true},
                Port{"foot", "Point2D", "Foot", "垂足，给视图画那条虚线。", true}};

  Param bulge;
  bulge.name = "sealBulge";
  bulge.type = ParamType::Enum;
  bulge.label = "凸起方向";
  bulge.def = Value::text("+z");
  bulge.options = {EnumOption{"+z", "+z（朝传感器）", ""}, EnumOption{"-z", "-z", ""}};

  Param dir;
  dir.name = "positiveDirection";
  dir.type = ParamType::Enum;
  dir.label = "正方向";
  dir.doc = "2026-08-27 确认：胶条底比钣金面更远离传感器时为正。";
  dir.def = Value::text("deeper");
  dir.options = {EnumOption{"deeper", "更远离传感器为正", ""},
                 EnumOption{"toward_sensor", "更靠近传感器为正", ""}};

  op.params = {bulge, dir};
  op.capabilities = {false, true, true};
  op.compute = &compute;
  r.addOperator(std::move(op));
}

}  // namespace lyflow::dts
