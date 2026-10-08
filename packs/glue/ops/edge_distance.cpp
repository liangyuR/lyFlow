// glue.edge_distance「量边距」（glue-plan §2.3，D8 / D9）。
#include <cmath>
#include <limits>

#include "algo/edge.h"
#include "glue.h"

namespace lyflow::packs::glue {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

Px px(P2 p) { return Px{p.x, p.y}; }

Status compute(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext&) {
  const cv::Mat gray = grayOf(inputs.get("image"));
  if (gray.empty()) {
    return Status::Error(Phase::Execute, "bad_input", "需要全分辨率非空 u8 图像；其他位深先接 image.normalize，缩小预览请改用完整运行", {}, "image");
  }
  BeadView bead;
  std::vector<Station> st;
  if (!readBead(inputs.get("bead"), &bead) || !readStations(bead.stations, &st)) {
    return Status::Error(Phase::Execute, "bad_input", "bead 不是完整的 glue.Bead", {}, "bead");
  }
  Metric metric;
  if (Status s = metricFromInput(inputs, "calib", &metric); !s.ok) return s;

  EdgeSpec spec;
  spec.searchLength = params.number("searchLength");
  spec.contrastMin = params.number("contrastMin");
  spec.farRun = params.number("farRun");
  spec.darkToBright = params.choice("edgePolarity") == "dark_to_bright";
  spec.fromCenter = params.choice("reference") == "bead_center";
  const EdgeResult er = measureEdges(gray, st, spec);

  Json s = Json::array(), edge = Json::array(), from = Json::array(), distance = Json::array(),
       distancePx = Json::array(), status = Json::array();
  std::vector<double> kept;
  int searched = 0, found = 0;
  Overlay2D ov;
  std::vector<Px> partPts, outliers;
  std::vector<std::array<Px, 2>> segs;
  for (std::size_t i = 0; i < st.size(); ++i) {
    const EdgeStation& es = er.stations[i];
    s.push_back(round3(st[i].s));
    searched += es.searched ? 1 : 0;
    found += es.found ? 1 : 0;
    if (!es.found) {
      edge.push_back(Json());
      from.push_back(Json());
      distance.push_back(Json());
      distancePx.push_back(Json());
      status.push_back(es.searched ? "none" : "nobead");
      continue;
    }
    edge.push_back(pxJson(es.edge));
    from.push_back(pxJson(es.from));
    distancePx.push_back(round3(es.distancePx));
    if (es.kept) {
      // 零件边从胶边外 3 px 才开始找，所以距离总是正的
      const double d = metric.distance(es.from, es.edge);
      distance.push_back(numOrNull(d));
      status.push_back("ok");
      if (std::isfinite(d)) kept.push_back(d);
      partPts.push_back(px(es.edge));
      segs.push_back({px(es.from), px(es.edge)});
    } else {
      distance.push_back(Json());
      status.push_back("outlier");
      outliers.push_back(px(es.edge));
    }
  }
  double mean = kNaN, lo = kNaN, hi = kNaN;
  if (!kept.empty()) {
    double sum = 0;
    lo = hi = kept.front();
    for (double d : kept) {
      sum += d;
      lo = std::min(lo, d);
      hi = std::max(hi, d);
    }
    mean = sum / static_cast<double>(kept.size());
  }
  Json rec;
  rec["side"] = er.side > 0 ? "right" : (er.side < 0 ? "left" : "none");
  rec["votes"] = Json{{"right", er.votesRight}, {"left", er.votesLeft}};
  rec["reference"] = spec.fromCenter ? "bead_center" : "bead_edge";
  rec["edgePolarity"] = spec.darkToBright ? "dark_to_bright" : "bright_to_dark";
  rec["unit"] = metric.unit();
  rec["count"] = st.size();
  rec["searched"] = searched;
  rec["found"] = found;
  rec["kept"] = kept.size();
  rec["s"] = std::move(s);
  rec["edge"] = std::move(edge);
  rec["from"] = std::move(from);
  rec["distance"] = std::move(distance);
  rec["distancePx"] = std::move(distancePx);
  rec["status"] = std::move(status);
  Record out;
  out.type = kEdgeType;
  out.data = std::move(rec);
  outputs.set("edge", Data::record(std::move(out)));
  const std::string why = searched == 0 ? "没有一站有胶" : "两侧都没找到零件边";
  outputs.set("distanceMean", measurement(mean, metric.unit(), why));
  outputs.set("distanceMin", measurement(lo, metric.unit(), why));
  outputs.set("distanceMax", measurement(hi, metric.unit(), why));

  ov.segments("distance", segs);
  ov.points("part", partPts, er.side == 0 ? "" : (er.side > 0 ? "零件边（右侧）" : "零件边（左侧）"));
  ov.points("outlier", outliers);
  outputs.set("overlay", ov.data());
  return Status::Ok();
}

std::vector<Issue> validate(const ParamView& params, const std::set<std::string>&) {
  std::vector<Issue> issues;
  if (params.number("searchLength") < params.number("farRun") + 10.0) {
    issues.push_back(Issue::error("bad_param", "searchLength 要比 farRun 至少长 10 px", "searchLength"));
  }
  return issues;
}

}  // namespace

void registerEdgeDistance(Registry& r) {
  OperatorDesc op;
  op.id = "glue.edge_distance";
  op.version = "1.0.0";
  op.label = "量边距";
  op.category = "涂胶/积木";
  op.keywords = {"glue", "bead", "edge", "distance", "margin", "涂胶", "边距", "零件边", "积木"};
  op.doc =
      "胶靠零件边那一侧的胶边 → 零件边，沿胶路法向量（reference = bead_center 时从胶的中线量）。"
      "零件边在哪一侧自动判：每个有胶的站两侧都找，取多数站找到强边的那一侧（D8，不设人要填的 side）。\n"
      "零件边 = 沿法向从胶边外 3 px 起、第一个满足极性（默认 亮 → 暗）、两侧差 ≥ contrastMin、远侧持续 ≥ farRun px "
      "都暗下去的跳变，位置取跳变里下降最快的那一点（D9：只取最大梯度会被翻边上的划痕、反光抢走）。"
      "逐站结果沿 s 做滑动中值 + MAD 剔野。没接 calib 出 px，接了 image.PlaneCalib 按映射后两点的距离出 mm。";
  op.inputs = {
      Port{"image", "Image", "Image", "与 bead_width 同一帧。", true},
      Port{"bead", "Bundle<glue.Bead>", "Bead", "glue.bead_width 的逐站结果。", true},
      withContract(Port{"calib", "Record", "Calib", "可选：image.PlaneCalib。接了距离出 mm。", false},
                   {{"recordType", std_image::kPlaneCalibType}}),
  };
  op.outputs = {
      Port{"edge", "Record", "Edge",
           "glue.Edge：side（right | left | none）、每站的零件边点、量起点、距离与状态（ok / outlier / none / nobead）。",
           true},
      Port{"distanceMean", "Measurement", "Distance Mean", "各站边距的均值。", true},
      Port{"distanceMin", "Measurement", "Distance Min", "各站边距的最小值。", true},
      Port{"distanceMax", "Measurement", "Distance Max", "各站边距的最大值。", true},
      Port{"overlay", "Record", "Overlay", "lyflow.overlay2d：零件边点与每站的距离线段。", true},
  };
  Param searchLength = floatParam("searchLength", "Search Length", 200.0, "px",
                                  "从胶边外 3 px 起沿法向往外找零件边的距离。");
  searchLength.min = 20.0;
  Param reference = enumParam("reference", "Reference", "bead_edge",
                              "边距从哪里量：胶靠零件边那一侧的边（默认），或胶的中线。",
                              {EnumOption{"bead_edge", "胶的近边", ""},
                               EnumOption{"bead_center", "胶的中线", ""}},
                              true);
  Param polarity = enumParam("edgePolarity", "Edge Polarity", "bright_to_dark",
                             "从胶往外看零件边是 亮 → 暗（默认：翻边亮、外面暗）还是 暗 → 亮。",
                             {EnumOption{"bright_to_dark", "亮 → 暗", ""},
                              EnumOption{"dark_to_bright", "暗 → 亮", ""}},
                             true);
  Param contrastMin = floatParam("contrastMin", "Contrast Min", 40.0, "",
                                 "零件边两侧的灰度差至少这么大（灰度级）。", true);
  contrastMin.min = 1.0;
  Param farRun = floatParam("farRun", "Far Run", 12.0, "px",
                            "零件边的远侧要持续这么长都暗下去（翻边上细的划痕、反光条过不了这一关）。", true);
  farRun.min = 1.0;
  op.params = {searchLength, reference, polarity, contrastMin, farRun};
  op.capabilities = {/*cancellable=*/false, /*previewable=*/false, /*deterministic=*/true};
  op.compute = &compute;
  op.validate = &validate;
  r.addOperator(std::move(op));
}

}  // namespace lyflow::packs::glue
