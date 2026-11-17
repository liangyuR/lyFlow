#include <cmath>

#include "dts_ops.h"

namespace lyflow::dts {
namespace {

Status compute(const Inputs& inputs, const ParamView&, Outputs& outputs, ExecContext& ctx) {
  try {
    if (ctx.cancelled())
      return cancelledStatus();
    const auto* measurement = inputs.get("flush").asMeasurement();
    const auto& root = checkedRecord(inputs.get("root"), "DtsRoot.v3");
    const auto& dome = checkedRecord(inputs.get("dome"), "DtsDome.v3");
    const auto& pick = checkedRecord(inputs.get("pick"), "DtsMetalPick.v3");
    const auto& faces = checkedRecord(inputs.get("faces"), "profile.Faces.v2");
    const auto& info = checkedRecord(inputs.get("footInfo"), "DtsFlush.v3");
    const Line2D metal = lineMillimeters(pick.at("metalLineMm"));
    if (measurement->unit != "mm" || (measurement->ok && !std::isfinite(measurement->value)))
      throw std::invalid_argument("汇总面差单位或数值无效");
    nlohmann::json j = sceneRecord();
    j["ok"] = measurement->ok;
    j["flushMm"] = measurement->ok ? nlohmann::json(measurement->value) : nlohmann::json();
    j["root"] = root;
    j["dome"] = dome;
    j["pick"] = pick;
    j["faces"] = faces.at("faces");
    j["pieces"] = faces.at("pieces");
    j["metal"] = {
        {"x0", metal.start[0]},
        {"z0", metal.start[1]},
        {"x1", metal.end[0]},
        {"z1", metal.end[1]},
        {"angleDeg", std::atan2(metal.dir[1], metal.dir[0]) * 180.0 / 3.14159265358979323846},
        {"rmsMm", number(pick, "rmsMm")}};
    if (measurement->ok) {
      const auto& foot = info.at("footMm");
      j["perpendicular"] = {{"x", foot.at(0).get<float>()}, {"z", foot.at(1).get<float>()}};
    }
    if (ctx.cancelled())
      return cancelledStatus();
    outputs.set("bundle", Data::record(Record{"DtsProfileBundle.v3", std::move(j)}));
    return Status::Ok();
  } catch (const std::exception& error) {
    return sceneError(error, "flush");
  }
}

} // namespace

void registerProfileBundle(Registry& r) {
  OperatorDesc op;
  op.id = "dts.profile_bundle";
  op.version = "3.0.0";
  op.migrations = {
      {1, requireGraphMigration, nullptr},
      {2, nullptr, [](const nlohmann::json&, const std::vector<MigrationInput>& inputs) {
         return profile_contract::retireInputs(inputs, {"metal", "foot"});
       }}};
  op.label = "单剖面结果汇总";
  op.category = "DTS/输出";
  op.keywords = {"profile result bundle",
                 "profile aggregation",
                 "bundle",
                 "激光轮廓",
                 "剖面结果",
                 "检测结果汇总",
                 "result",
                 "汇总"};
  op.doc = "把这一条轮廓上所有中间结果装成一个 Record，宿主拿它画界面、落库。";
  op.inputs = {Port{"faces", "Record", "Faces", "面表。", true},
               Port{"dome", "Record", "Dome", "胶条主体。", true},
               Port{"pick", "Record", "Pick", "挑面结果。", true},
               Port{"root", "Record", "Root", "胶条底部。", true},
               Port{"flush", "Measurement", "Flush", "面差。", true},
               Port{"footInfo", "Record", "Foot Info", "DtsFlush.v3毫米垂足。", true}};
  op.outputs = {Port{"bundle", "Record", "Bundle", "DtsProfileBundle.v3。", true}};
  op.capabilities = {false, true, true};
  op.compute = &compute;
  r.addOperator(std::move(op));
}

} // namespace lyflow::dts
