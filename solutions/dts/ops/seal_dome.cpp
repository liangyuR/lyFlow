#include <cmath>

#include "dts_ops.h"

namespace lyflow::dts {
namespace {

Status compute(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext& ctx) {
  try {
    const Data& source = inputs.get("clean");
    ActiveProfile active;
    const Status loaded = loadProfile(source, active, ctx);
    if (!loaded.ok)
      return loaded;
    const auto pieces = checkedPieces(inputs.get("faces"), active);
    if (pieces.empty())
      return Status::Error(Phase::Execute, "no_segments", "面表里没有分片", {}, "faces");
    DomeParams dp;
    dp.bulgeSign = params.choice("sealBulge") == "+z" ? 1.0 : -1.0;
    dp.minLenMm = params.number("minLenMm");
    dp.maxLenMm = params.number("maxLenMm");
    dp.minHeightMm = params.number("minHeightMm");
    dp.maxRadiusMm = params.number("maxRadiusMm");
    dp.walkTolMm = params.number("walkTolMm");
    Dome dome;
    if (!findDome(active.samples, pieces, dp, dome, [&ctx] { return ctx.cancelled(); }))
      return Status::Error(Phase::Execute, "no_dome", "找不到像胶条主体的凸起", {}, "clean");
    const View all = mergedView(active.samples, pieces, [&ctx] { return ctx.cancelled(); });
    ApexAxis axis;
    if (!apexAxis(all, dome, params.number("apexFitHalfMm"), params.number("radiusMinMm"),
                  params.number("radiusMaxMm"), axis, [&ctx] { return ctx.cancelled(); }))
      return Status::Error(Phase::Execute, "no_apex_axis",
                           "顶部圆拟合失败或半径超出范围（胶条半径是硬判据）", {}, "clean");
    nlohmann::json j = sceneRecord();
    j.update({{"xPeak", dome.xPeak},
              {"zPeak", dome.zPeak},
              {"xLo", dome.xLo},
              {"xHi", dome.xHi},
              {"heightMm", dome.heightMm},
              {"widthMm", dome.xHi - dome.xLo},
              {"radiusMm", axis.radius},
              {"centerX", axis.cx},
              {"centerZ", axis.cz},
              {"apexAngleDeg", axis.angleDeg}});
    Line2D apex;
    apex.point[0] = static_cast<float>(axis.cx);
    apex.point[1] = static_cast<float>(axis.cz);
    const double a = axis.angleDeg * 3.14159265358979323846 / 180.0;
    apex.dir[0] = static_cast<float>(std::cos(a));
    apex.dir[1] = static_cast<float>(std::sin(a));
    apex.hasSegment = true;
    apex.start[0] = static_cast<float>(axis.cx);
    apex.start[1] = static_cast<float>(axis.cz);
    apex.end[0] = static_cast<float>(dome.xPeak);
    apex.end[1] = static_cast<float>(dome.zPeak);
    const Data geometry = Data::line2d(metricGeometry(apex));
    if (ctx.cancelled())
      return cancelledStatus();
    outputs.set("dome", Data::record(Record{"DtsDome.v3", std::move(j)}));
    outputs.set("apex", geometry);
    return Status::Ok();
  } catch (const kernel::profile::Cancelled&) {
    return cancelledStatus();
  } catch (const std::exception& error) {
    return sceneError(error, "faces");
  }
}

} // namespace

void registerSealDome(Registry& r) {
  OperatorDesc op;
  op.id = "dts.seal_dome";
  op.version = "3.0.0";
  op.migrations = {{1, requireGraphMigration, nullptr},
                   {2, nullptr, &profile_contract::keepInputs}};
  op.label = "胶条主体提取";
  op.category = "DTS/胶条";
  op.keywords = {"seal dome extraction",
                 "seal body detection",
                 "circle fitting",
                 "胶条凸起",
                 "顶点轴",
                 "圆拟合",
                 "seal",
                 "dome",
                 "circle",
                 "胶条",
                 "凸起",
                 "圆"};
  op.doc = "找胶条主体那个凸起：宽高合适、顶部圆拟合半径落在范围内的最显著凸包。\n"
           "顶点轴 = 圆心 → 顶点，是挑钣金面时量角度的参照。实测它比两端脊点的连线稳一个数量级\n"
           "（绝对角 std 1.9° 对 14.8°），因为圆拟合吃的是顶部两百多个点，不由两个端点决定。";
  op.inputs = {
      Port{"clean", "Profile", "Clean", "清理过的轮廓。", true},
      Port{"faces", "Record", "Faces", "profile.split_faces 的输出，取其中的分片。", true}};
  op.outputs = {Port{"dome", "Record", "Dome", "胶条主体（DtsDome.v3）。", true},
                Port{"apex", "Line2D", "Apex Axis", "圆心指向顶点，带端点。", true}};

  auto mk = [](const char* name, const char* label, double def, const char* doc,
               const char* unit = "mm") {
    Param p;
    p.name = name;
    p.type = ParamType::Float;
    p.label = label;
    p.doc = doc;
    p.def = Value::number(def);
    p.min = 0.0;
    p.unit = unit;
    return p;
  };

  Param bulge;
  bulge.name = "sealBulge";
  bulge.type = ParamType::Enum;
  bulge.label = "凸起方向";
  bulge.doc = "胶条朝哪个方向鼓。";
  bulge.def = Value::text("+z");
  bulge.options = {EnumOption{"+z", "+z（朝传感器）", ""}, EnumOption{"-z", "-z", ""}};

  op.params = {bulge,
               mk("minLenMm", "最小宽度", 5.0, "凸起的底宽下限。"),
               mk("maxLenMm", "最大宽度", 30.0, "超过它的算门皮大弧，不是胶条。"),
               mk("minHeightMm", "最小高度", 1.5, "凸起高度下限。"),
               mk("maxRadiusMm", "最大曲率半径", 20.0, "顶部圆拟合半径上限，用来排除大弧。"),
               mk("walkTolMm", "走坡容差", 0.15, "沿凸起两翼下行时容忍的回升量。"),
               mk("apexFitHalfMm", "顶部拟合半宽", 6.0, "顶点两侧各取多少 mm 拟合圆。"),
               mk("radiusMinMm", "半径下限", 7.0, "实测胶条半径 10.07 ± 0.17 mm。"),
               mk("radiusMaxMm", "半径上限", 14.0, "超出范围就判定这不是胶条。")};
  op.capabilities = {true, true, true};
  op.compute = &compute;
  r.addOperator(std::move(op));
}

} // namespace lyflow::dts
