#include <cmath>

#include "dts_ops.h"

namespace lyflow::dts {
namespace {

Status compute(const Inputs& inputs, const ParamView&, Outputs& outputs, ExecContext& ctx) {
  try {
    if (ctx.cancelled())
      return cancelledStatus();
    const auto* rootRecord = inputs.get("root").asRecord();
    const auto* measurement = inputs.get("flush").asMeasurement();
    const auto& context = rootRecord->data.at("context");
    const auto& root = checkedRecord(inputs.get("root"), "DtsRoot.v2", context);
    const auto& dome = checkedRecord(inputs.get("dome"), "DtsDome.v2", context);
    const auto& pick = checkedRecord(inputs.get("pick"), "DtsMetalPick.v2", context);
    const auto& faces = checkedRecord(inputs.get("faces"), "profile.Faces.v1", context, 1);
    const auto& info = checkedRecord(inputs.get("footInfo"), "DtsFlush.v2", context);
    const Line2D metal = lineMillimeters(pick.at("metalLineMm"));
    const auto hash = profile_contract::geometryHash(inputs.get("metal"));
    if (inputs.get("metal").asLine2D()->unit != Unit2D::Meter ||
        hash != pick.at("metalGeometryHash") || hash != root.at("metalGeometryHash") ||
        hash != profile_contract::geometryHash(Data::line2d(metricGeometry(metal))))
      throw std::invalid_argument("汇总的基准线与配套毫米几何不一致");
    if (measurement->unit != "mm" ||
        (measurement->ok &&
         (!std::isfinite(measurement->value) || measurement->value != number(info, "valueMm"))))
      throw std::invalid_argument("汇总面差单位或配套数值不一致");
    nlohmann::json j{{"schemaVersion", 2}, {"workingUnit", "mm"}, {"context", context}};
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
    if (inputs.has("foot")) {
      const Data& foot = inputs.get("foot");
      if (!foot.asPoint2D() || foot.asPoint2D()->unit != Unit2D::Meter ||
          info.at("footGeometryHash") != profile_contract::geometryHash(foot))
        throw std::invalid_argument("垂足与配套几何指纹不一致");
      const auto& coordinates = info.at("footMm");
      if (!coordinates.is_array() || coordinates.size() != 2)
        throw std::invalid_argument("毫米垂足维度无效");
      Point2D legacy;
      for (int i = 0; i < 2; ++i) {
        legacy.p[i] = coordinates.at(i).get<float>();
        if (!std::isfinite(legacy.p[i]))
          throw std::invalid_argument("毫米垂足必须有限");
      }
      if (info.at("footGeometryHash") !=
          profile_contract::geometryHash(Data::point2d(metricGeometry(legacy))))
        throw std::invalid_argument("毫米垂足与米制输出不一致");
      j["perpendicular"] = {{"x", legacy.p[0]}, {"z", legacy.p[1]}};
    }
    if (ctx.cancelled())
      return cancelledStatus();
    outputs.set("bundle", Data::record(Record{"DtsProfileBundle.v2", std::move(j)}));
    return Status::Ok();
  } catch (const std::exception& error) {
    return sceneError(error, "flush");
  }
}

} // namespace

void registerProfileBundle(Registry& r) {
  OperatorDesc op;
  op.id = "dts.profile_bundle";
  op.version = "2.0.0";
  op.migrations = {{1, requireGraphMigration, nullptr}};
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
               Port{"metal", "Line2D", "Metal", "基准线。", true},
               Port{"flush", "Measurement", "Flush", "面差。", true},
               Port{"footInfo", "Record", "Foot Info", "配套DtsFlush.v2毫米垂足。", true},
               Port{"foot", "Point2D", "Foot", "垂足，可选。", false}};
  op.outputs = {Port{"bundle", "Record", "Bundle", "DtsProfileBundle.v2。", true}};
  op.capabilities = {false, true, true};
  op.compute = &compute;
  r.addOperator(std::move(op));
}

} // namespace lyflow::dts
