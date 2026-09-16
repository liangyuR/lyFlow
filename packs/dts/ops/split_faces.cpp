#include "dts_ops.h"

namespace lyflow::dts {
namespace {

Status compute(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext&) {
  const PointCloud& in = *inputs.get("clean").asCloud();
  const Profile prof = profileFromCloud(in);
  const std::vector<Piece> pieces =
      splitPieces(prof, params.number("gapMm"), params.number("jumpMm"),
                  static_cast<int>(params.integer("minPoints")));
  if (pieces.empty()) {
    return Status::Error(Phase::Execute, "no_segments", "切不出任何分片", {}, "clean");
  }
  FaceParams fp;
  fp.fitLenMm = params.number("fitLenMm");
  fp.maxRmsMm = params.number("maxRmsMm");
  fp.angleBreakDeg = params.number("angleBreakDeg");
  fp.minLenMm = params.number("minLenMm");
  const std::vector<Face> faces = splitFaces(prof, pieces, fp);

  nlohmann::json j;
  j["pieces"] = nlohmann::json::array();
  for (const Piece& q : pieces) {
    const View v = pieceView(prof, q);
    j["pieces"].push_back({{"i0", q.i0},
                           {"i1", q.i1},
                           {"n", q.i1 - q.i0 + 1},
                           {"x0", v.x(0)},
                           {"x1", v.x(v.size() - 1)}});
  }
  j["faces"] = nlohmann::json::array();
  for (const Face& f : faces) j["faces"].push_back(faceToJson(f));
  outputs.set("faces", Data::record(Record{"DtsFaces", std::move(j)}));
  return Status::Ok();
}

}  // namespace

void registerSplitFaces(Registry& r) {
  OperatorDesc op;
  op.id = "dts.split_faces";
  op.version = "1.0.0";
  op.label = "切分平面片";
  op.category = "DTS/分面";
  op.keywords = {"faces", "split", "flat", "分面", "平面"};
  op.doc =
      "先按 x 间断与 z 跳变切成分片，再在每个分片里滑窗找平：\n"
      "rms 达标的窗口、相邻窗口夹角不超过阈值的连成一块面，合并后整体拟合。\n"
      "输出的面表是挑钣金面的依据 —— 挑面从藏在启发式里变成看着这张表挑。";
  op.inputs = {Port{"clean", "PointCloud", "Clean", "清理过的轮廓。", true}};
  op.outputs = {Port{"faces", "Record", "Faces", "分片与面表（DtsFaces）。", true}};

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

  Param minPoints;
  minPoints.name = "minPoints";
  minPoints.type = ParamType::Int;
  minPoints.label = "分片最少点数";
  minPoints.def = Value::integer(15);
  minPoints.min = 3.0;

  op.params = {mk("gapMm", "x 间断", 1.0, "相邻有效点的 x 间隔超过它就断开。"),
               mk("jumpMm", "z 跳变", 1.5, "相邻有效点的 z 落差超过它就断开。"),
               minPoints,
               mk("fitLenMm", "滑窗长度", 5.0, "找平用的窗口长度。"),
               mk("maxRmsMm", "平面 rms 上限", 0.05, "窗口的直线拟合残差低于它才算平。"),
               mk("angleBreakDeg", "角度断开阈", 6.0,
                  "相邻窗口夹角超过它就当成两块面。", "deg"),
               mk("minLenMm", "最短面长", 3.0, "合并后短于它的面丢掉。")};
  op.capabilities = {false, true, true};
  op.compute = &compute;
  r.addOperator(std::move(op));
}

}  // namespace lyflow::dts
