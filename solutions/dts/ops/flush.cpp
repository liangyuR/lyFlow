#include <cmath>

#include "dts_ops.h"
#include "lyflow/kernel/metrology.h"

namespace lyflow::dts {
namespace {

Status compute(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext& ctx) {
  try {
    if (ctx.cancelled())
      return cancelledStatus();
    const auto& root = checkedRecord(inputs.get("root"), "DtsRoot.v3");
    const Line2D mm = lineMillimeters(root.at("metalLineMm"));
    const double rx = number(root, "x"), rz = number(root, "z");
    const double normalSign = mm.dir[0] >= 0 ? 1.0 : -1.0;
    const double businessSign = (params.choice("sealBulge") == "+z" ? 1.0 : -1.0) *
                                (params.choice("positiveDirection") == "deeper" ? -1.0 : 1.0);
    kernel::metrology::SignedPointLine result;
    std::string why;
    if (!kernel::metrology::signedPointLine(
            {rx, rz}, {mm.point[0], mm.point[1]}, {mm.dir[0], mm.dir[1]},
            {-mm.dir[1] * normalSign * businessSign, mm.dir[0] * normalSign * businessSign},
            kernel::metrology::DirectionPolicy::DeclaredUnit, result, why))
      return Status::Error(Phase::Execute, "bad_input", why, {}, "root");
    Measurement measurement;
    measurement.value = result.distance;
    measurement.ok = std::isfinite(result.distance);
    measurement.unit = "mm";
    if (!measurement.ok)
      measurement.message = "面差算出来不是有限值";
    Point2D foot;
    foot.p[0] = static_cast<float>(result.foot[0]);
    foot.p[1] = static_cast<float>(result.foot[1]);
    const Data geometry = Data::point2d(metricGeometry(foot));
    nlohmann::json info = sceneRecord();
    info["footMm"] = {foot.p[0], foot.p[1]};
    if (ctx.cancelled())
      return cancelledStatus();
    outputs.set("flush", Data::measurement(measurement));
    outputs.set("foot", geometry);
    outputs.set("footInfo", Data::record(Record{"DtsFlush.v3", std::move(info)}));
    return Status::Ok();
  } catch (const std::exception& error) {
    return sceneError(error, "root");
  }
}

} // namespace

void registerFlush(Registry& r) {
  OperatorDesc op;
  op.id = "dts.flush";
  op.version = "3.0.0";
  op.migrations = {
      {1, rejectLegacyProfileNode, nullptr},
      {2, nullptr, [](const nlohmann::json&, const std::vector<MigrationInput>& inputs) {
         return profile_contract::retireInputs(inputs, {"metal"});
       }}};
  op.label = "胶条面差测量";
  op.category = "DTS/面差";
  op.keywords = {"seal flush measurement",
                 "flushness",
                 "perpendicular distance",
                 "段差",
                 "垂直距离",
                 "胶条高度差",
                 "flush",
                 "垂距"};
  op.doc = "胶条底部到基准线的垂距。判定不在这里做 —— 标准表按 (段号, 站位号) 存在库里，\n"
           "图拿不到也不该拿，所以 verdict / 公差留空。";
  op.inputs = {Port{"root", "Record", "Root", "胶条底部，含挑面时的毫米基准线。", true}};
  op.outputs = {Port{"flush", "Measurement", "Flush", "面差，单位 mm。", true},
                Port{"foot", "Point2D", "Foot", "米制垂足，给视图画那条虚线。", true},
                Port{"footInfo", "Record", "Foot Info", "DtsFlush.v3毫米垂足。", true}};

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

} // namespace lyflow::dts
