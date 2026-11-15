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
    const auto context = profile_contract::context(source);
    const auto pieces = checkedPieces(inputs.get("faces"), source, active);
    const auto& fj = inputs.get("faces").asRecord()->data;
    const auto& dj = checkedRecord(inputs.get("dome"), "DtsDome.v2", context);
    const auto faces = facesFromJson(fj);
    if (faces.empty())
      return Status::Error(Phase::Execute, "no_metal_face", "面表是空的", {}, "faces");
    Dome dome;
    dome.xPeak = number(dj, "xPeak");
    dome.zPeak = number(dj, "zPeak");
    dome.xLo = number(dj, "xLo");
    dome.xHi = number(dj, "xHi");
    ApexAxis axis;
    axis.cx = number(dj, "centerX");
    axis.cz = number(dj, "centerZ");
    axis.radius = number(dj, "radiusMm");
    axis.angleDeg = number(dj, "apexAngleDeg");
    PickParams pp;
    pp.angleToApexDeg = params.number("angleToApexDeg");
    pp.angleTolDeg = params.number("angleTolDeg");
    pp.fitLenMm = params.number("fitLenMm");
    pp.sealSign = params.choice("sealSide") == "-x" ? -1 : 1;
    const View all = mergedView(active.samples, pieces, [&ctx] { return ctx.cancelled(); });
    MetalPick pick;
    std::vector<MetalPick> rejected;
    if (!pickMetal(faces, dome, axis, all, pp, pick, &rejected, [&ctx] { return ctx.cancelled(); }))
      return Status::Error(
          Phase::Execute, "no_metal_face",
          "钣金侧没有角度合格的面（候选 " + std::to_string(rejected.size()) + " 块）", {}, "faces");
    Line2D line;
    line.point[0] = static_cast<float>(pick.line.px);
    line.point[1] = static_cast<float>(pick.line.pz);
    line.dir[0] = static_cast<float>(pick.line.dx);
    line.dir[1] = static_cast<float>(pick.line.dz);
    const Face& face = faces.at(static_cast<std::size_t>(pick.faceIndex));
    line.hasSegment = true;
    const double a = pp.sealSign > 0 ? pick.edgeX - pp.fitLenMm : pick.edgeX;
    const double b = pp.sealSign > 0 ? pick.edgeX : pick.edgeX + pp.fitLenMm;
    line.start[0] = static_cast<float>(a);
    line.start[1] =
        static_cast<float>(pick.line.pz + (a - pick.line.px) * pick.line.dz / pick.line.dx);
    line.end[0] = static_cast<float>(b);
    line.end[1] =
        static_cast<float>(pick.line.pz + (b - pick.line.px) * pick.line.dz / pick.line.dx);
    const Data geometry = Data::line2d(metricGeometry(line));
    const auto geometryContext = profile_contract::geometryContext(source, geometry);
    nlohmann::json rejectedJson = nlohmann::json::array();
    for (const auto& candidate : rejected)
      rejectedJson.push_back({{"faceIndex", candidate.faceIndex},
                              {"gapMm", candidate.gapMm},
                              {"relDeg", candidate.relDeg},
                              {"reason", "角度或侧别不符"}});
    nlohmann::json j = sceneRecord(source);
    j.update({{"faceIndex", pick.faceIndex},
              {"gapMm", pick.gapMm},
              {"relDeg", pick.relDeg},
              {"edgeX", pick.edgeX},
              {"angleDeg", pick.line.angleDeg()},
              {"rmsMm", pick.line.rms},
              {"nInliers", pick.line.n},
              {"faceX0", face.x0},
              {"faceX1", face.x1},
              {"faceLenMm", face.lenMm},
              {"sealSign", pp.sealSign},
              {"rejected", std::move(rejectedJson)},
              {"metalLineMm", lineMillimeters(line)},
              {"metalGeometryHash", geometryContext.at("geometryHash")}});
    if (ctx.cancelled())
      return cancelledStatus();
    outputs.set("metal", geometry);
    outputs.set("pick", Data::record(Record{"DtsMetalPick.v2", std::move(j)}));
    outputs.set("metalContext", Data::record(Record{"profile.Geometry.v1", geometryContext}));
    return Status::Ok();
  } catch (const kernel::profile::Cancelled&) {
    return cancelledStatus();
  } catch (const std::exception& error) {
    return sceneError(error, "pick");
  }
}

} // namespace

void registerPickMetal(Registry& r) {
  OperatorDesc op;
  op.id = "dts.pick_metal";
  op.version = "2.0.0";
  op.migrations = {{1, requireGraphMigration, nullptr}};
  op.label = "钣金基准面提取";
  op.category = "DTS/钣金";
  op.keywords = {"sheet metal datum extraction",
                 "metal reference surface",
                 "datum line",
                 "sheet metal",
                 "钣金基准面",
                 "基准线拟合",
                 "参考面提取",
                 "metal",
                 "reference",
                 "钣金",
                 "基准"};
  op.doc = "在胶条钣金侧的面里挑基准面：与顶点轴的夹角落在容差内、离胶条最近的那一块，\n"
           "基准线只在该面朝胶条那一端取一小段拟合 —— 整块面可能几十毫米长且远端微弯，\n"
           "拿整块拟合会把弯曲摊进基准线里。\n"
           "同一截面上钣金侧常有两族面（实测夹角差约 70°，是同一道屋脊的两侧），\n"
           "`angleToApexDeg` 决定取哪一族。";
  op.inputs = {Port{"clean", "Profile", "Clean", "清理过的轮廓。", true},
               Port{"faces", "Record", "Faces", "面表。", true},
               Port{"dome", "Record", "Dome", "胶条主体。", true}};
  op.outputs = {Port{"metal", "Line2D", "Metal", "基准线，带拟合段的两个端点。", true},
                Port{"pick", "Record", "Pick", "挑中的面与全部落选候选（DtsMetalPick.v2）。", true},
                Port{"metalContext", "Record", "Metal Context",
                     "profile.Geometry.v1米制基准线上下文。", true}};

  Param ang;
  ang.name = "angleToApexDeg";
  ang.type = ParamType::Float;
  ang.label = "与顶点轴夹角";
  ang.doc = "实测外板那一族在 −34..−75°，紧贴胶条那一族在 −116..−145°。";
  ang.def = Value::number(-55.0);
  ang.unit = "deg";
  ang.softMin = -180.0;
  ang.softMax = 0.0;

  Param tol;
  tol.name = "angleTolDeg";
  tol.type = ParamType::Float;
  tol.label = "夹角容差";
  tol.def = Value::number(40.0);
  tol.min = 0.0;
  tol.unit = "deg";

  Param fit;
  fit.name = "fitLenMm";
  fit.type = ParamType::Float;
  fit.label = "基准线拟合长度";
  fit.doc = "在选中面朝胶条那一端取这么长一段拟合。";
  fit.def = Value::number(5.0);
  fit.min = 0.5;
  fit.unit = "mm";

  Param side;
  side.name = "sealSide";
  side.type = ParamType::Enum;
  side.label = "胶条在哪侧";
  side.doc = "传感器相对车门的姿态由机器人程序定死，八段实测胶条一律在 +x 侧。不靠点数猜。";
  side.def = Value::text("+x");
  side.options = {EnumOption{"+x", "+x（钣金在小 x 侧）", ""}, EnumOption{"-x", "-x", ""}};

  op.params = {ang, tol, fit, side};
  op.capabilities = {true, true, true};
  op.compute = &compute;
  r.addOperator(std::move(op));
}

} // namespace lyflow::dts
