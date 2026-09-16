#include <cmath>

#include "dts_ops.h"

namespace lyflow::dts {
namespace {

Status compute(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext&) {
  const PointCloud& in = *inputs.get("clean").asCloud();
  const nlohmann::json* fj = recordData(inputs.get("faces"), "DtsFaces");
  const nlohmann::json* dj = recordData(inputs.get("dome"), "DtsDome");
  if (fj == nullptr || dj == nullptr) {
    return Status::Error(Phase::Execute, "type_mismatch", "faces / dome 类型不对", {}, "faces");
  }
  const Profile prof = profileFromCloud(in);
  const std::vector<Piece> pieces = piecesFromJson(*fj);
  const std::vector<Face> faces = facesFromJson(*fj);
  if (faces.empty()) {
    return Status::Error(Phase::Execute, "no_metal_face", "面表是空的", {}, "faces");
  }
  Dome dome;
  dome.xPeak = dj->value("xPeak", 0.0);
  dome.zPeak = dj->value("zPeak", 0.0);
  dome.xLo = dj->value("xLo", 0.0);
  dome.xHi = dj->value("xHi", 0.0);
  ApexAxis axis;
  axis.cx = dj->value("centerX", 0.0);
  axis.cz = dj->value("centerZ", 0.0);
  axis.radius = dj->value("radiusMm", 0.0);
  axis.angleDeg = dj->value("apexAngleDeg", 90.0);

  PickParams pp;
  pp.angleToApexDeg = params.number("angleToApexDeg");
  pp.angleTolDeg = params.number("angleTolDeg");
  pp.fitLenMm = params.number("fitLenMm");
  const std::string side = params.choice("sealSide");
  pp.sealSign = side == "-x" ? -1 : 1;

  const View all = mergedView(prof, pieces);
  MetalPick pick;
  std::vector<MetalPick> rejected;
  if (!pickMetal(faces, dome, axis, all, pp, pick, &rejected)) {
    nlohmann::json rj = nlohmann::json::array();
    for (const MetalPick& q : rejected) {
      rj.push_back({{"faceIndex", q.faceIndex}, {"gapMm", q.gapMm}, {"relDeg", q.relDeg}});
    }
    return Status::Error(Phase::Execute, "no_metal_face",
                         "钣金侧没有角度合格的面（候选 " + std::to_string(rejected.size()) + " 块）",
                         {}, "faces");
  }
  Line2D line;
  line.point[0] = static_cast<float>(pick.line.px);
  line.point[1] = static_cast<float>(pick.line.pz);
  line.dir[0] = static_cast<float>(pick.line.dx);
  line.dir[1] = static_cast<float>(pick.line.dz);
  const Face& f = faces[static_cast<std::size_t>(pick.faceIndex)];
  line.hasSegment = true;
  const double half = pp.fitLenMm;
  const double a = pp.sealSign > 0 ? pick.edgeX - half : pick.edgeX;
  const double b = pp.sealSign > 0 ? pick.edgeX : pick.edgeX + half;
  line.start[0] = static_cast<float>(a);
  line.start[1] = static_cast<float>(pick.line.pz + (a - pick.line.px) * pick.line.dz / pick.line.dx);
  line.end[0] = static_cast<float>(b);
  line.end[1] = static_cast<float>(pick.line.pz + (b - pick.line.px) * pick.line.dz / pick.line.dx);
  outputs.set("metal", Data::line2d(line));

  nlohmann::json rj = nlohmann::json::array();
  for (const MetalPick& q : rejected) {
    rj.push_back({{"faceIndex", q.faceIndex}, {"gapMm", q.gapMm}, {"relDeg", q.relDeg},
                  {"reason", "角度或侧别不符"}});
  }
  nlohmann::json j{{"faceIndex", pick.faceIndex}, {"gapMm", pick.gapMm},
                   {"relDeg", pick.relDeg},       {"edgeX", pick.edgeX},
                   {"angleDeg", pick.line.angleDeg()}, {"rmsMm", pick.line.rms},
                   {"nInliers", pick.line.n},     {"faceX0", f.x0}, {"faceX1", f.x1},
                   {"faceLenMm", f.lenMm},        {"sealSign", pp.sealSign},
                   {"rejected", rj}};
  outputs.set("pick", Data::record(Record{"DtsMetalPick", std::move(j)}));
  return Status::Ok();
}

}  // namespace

void registerPickMetal(Registry& r) {
  OperatorDesc op;
  op.id = "dts.pick_metal";
  op.version = "1.0.0";
  op.label = "挑钣金基准面";
  op.category = "DTS/钣金";
  op.keywords = {"metal", "reference", "钣金", "基准"};
  op.doc =
      "在胶条钣金侧的面里挑基准面：与顶点轴的夹角落在容差内、离胶条最近的那一块，\n"
      "基准线只在该面朝胶条那一端取一小段拟合 —— 整块面可能几十毫米长且远端微弯，\n"
      "拿整块拟合会把弯曲摊进基准线里。\n"
      "同一截面上钣金侧常有两族面（实测夹角差约 70°，是同一道屋脊的两侧），\n"
      "`angleToApexDeg` 决定取哪一族。";
  op.inputs = {Port{"clean", "PointCloud", "Clean", "清理过的轮廓。", true},
               Port{"faces", "Record", "Faces", "面表。", true},
               Port{"dome", "Record", "Dome", "胶条主体。", true}};
  op.outputs = {Port{"metal", "Line2D", "Metal", "基准线，带拟合段的两个端点。", true},
                Port{"pick", "Record", "Pick", "挑中的面与全部落选候选（DtsMetalPick）。", true}};

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
  op.capabilities = {false, true, true};
  op.compute = &compute;
  r.addOperator(std::move(op));
}

}  // namespace lyflow::dts
