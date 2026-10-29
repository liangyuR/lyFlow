// glue.track_path「粗跟踪与拟合」（docs/glue-measure-ops.md §3）：沿一个方向逐步找胶点，稳健拟合光滑胶路。
// 门限全部是参数；不输出 ok —— 覆盖率、陡度够不够、选哪个候选方向由宿主判。
#include <cmath>

#include "glue.h"

namespace lyflow::packs::glue {
namespace {

TrackSpec specOf(const ParamView& params) {
  TrackSpec spec;
  const auto anchor = params.vec2("anchor");
  spec.anchor = P2(anchor[0], anchor[1]);
  const auto zone = params.vec2("zone");
  spec.zoneStart = zone[0];
  spec.zoneEnd = zone[1];
  const auto bearing = params.vec2("bearingRange");
  spec.bearingFrom = bearing[0];
  spec.bearingTo = bearing[1];
  spec.bearingMargin = params.number("bearingMargin");
  spec.outwardRatio = params.number("outwardRatio");
  spec.searchHalf = params.number("searchHalf");
  spec.runs.halfMax = params.choice("split") == "half_max";
  spec.runs.segmentMin = params.number("segmentMin");
  spec.runs.peakMin = params.number("peakMin");
  spec.runs.mergeGap = params.number("mergeGap");
  spec.runs.mergePeakRatio = params.number("mergePeakRatio");
  spec.runs.mergeWidthMax = params.number("mergeWidthMax");
  spec.widthMin = params.number("widthMin");
  spec.widthMax = params.number("widthMax");
  spec.acceptOffsetRatio = params.number("acceptOffsetRatio");
  spec.coarseStep = params.number("coarseStep");
  spec.maxGap = params.number("maxGap");
  spec.gapSteps = params.number("gapSteps");
  spec.historyMin = static_cast<int>(params.integer("historyMin"));
  spec.jumpRatio = params.number("jumpRatio");
  spec.jumpMin = params.number("jumpMin");
  spec.reacquirePeakRatio = params.number("reacquirePeakRatio");
  spec.reacquireWidthRatio = params.number("reacquireWidthRatio");
  spec.steadyIqrRatio = params.number("steadyIqrRatio");
  spec.sharpMin = params.number("sharpMin");
  spec.rayFallback = params.flag("rayFallback");
  spec.rayFallbackCoverage = params.number("rayFallbackCoverage");
  spec.edgeProbe = params.number("edgeProbe");
  spec.backgroundProbe = params.number("backgroundProbe");
  spec.dirWindow = params.number("dirWindow");
  return spec;
}

std::vector<Px> pxList(const std::vector<P2>& pts) {
  std::vector<Px> out;
  out.reserve(pts.size());
  for (const P2& p : pts)
    out.push_back(Px{p.x, p.y});
  return out;
}

Status compute(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext&) {
  Field R, g;
  if (!f32FieldOf(inputs.get("response"), &R))
    return badImage("response", true);
  if (!f32FieldOf(inputs.get("gray"), &g))
    return badImage("gray", true);
  const TrackSpec spec = specOf(params);

  Json rec;
  const bool fromFan = inputs.has("fan");
  const int peakIndex = static_cast<int>(params.integer("peakIndex"));
  double heading = params.number("heading");
  bool valid = true;
  if (fromFan) {
    const Record* fan = inputs.get("fan").asRecord();
    if (!fan || fan->type != kFanScanType) {
      return Status::Error(Phase::Execute, "bad_input", "fan 不是 glue.FanScan", {}, "fan");
    }
    const auto peaks = fan->data.find("peaks");
    valid = peaks != fan->data.end() && peaks->is_array() &&
            peakIndex < static_cast<int>(peaks->size()) &&
            (*peaks)[static_cast<std::size_t>(peakIndex)].value("angle", Json()).is_number();
    if (valid)
      heading = (*peaks)[static_cast<std::size_t>(peakIndex)]["angle"].get<double>();
  }
  rec["headingSource"] = fromFan ? "fan" : "param";
  rec["peakIndex"] = fromFan ? Json(peakIndex) : Json();
  rec["valid"] = valid;
  rec["heading"] = valid ? numOrNull(heading, 3) : Json();
  rec["anchor"] = pxJson(spec.anchor);
  rec["zone"] = Json::array({spec.zoneStart, spec.zoneEnd});
  rec["bearingRange"] = Json::array({spec.bearingFrom, spec.bearingTo});

  Overlay2D ov;
  ov.circle("anchor", Px{spec.anchor.x, spec.anchor.y}, 6.0, "锚点");
  std::vector<double> s;
  std::vector<P2> points, tangents;
  if (!valid) {
    for (const char* k :
         {"steps", "found", "coverage", "sharpness", "fitted", "residual", "stoppedAt"})
      rec[k] = Json();
    rec["lineSource"] = "none";
    rec["points"] = Json::array();
  } else {
    const TrackResult tr = trackBead(R, g, spec, heading);
    const int minPoints = static_cast<int>(params.integer("minPoints"));
    const bool fitted = static_cast<int>(tr.points.size()) >= minPoints;
    double residual = std::numeric_limits<double>::quiet_NaN();
    if (fitted) {
      std::vector<std::pair<double, P2>> coarse;
      for (const TrackPoint& q : tr.points)
        coarse.emplace_back(q.s, q.point);
      fitBeadPath(coarse, spec.anchor, spec.zoneStart, spec.zoneEnd, spec.coarseStep,
                  params.number("fitWindow"), params.number("fitSigma"), &s, &points, &tangents,
                  &residual);
    } else {
      straightBeadPath(spec.anchor, heading, spec.zoneStart, spec.zoneEnd, &s, &points, &tangents);
    }
    rec["steps"] = tr.steps;
    rec["found"] = tr.points.size();
    rec["coverage"] = numOrNull(tr.coverage, 4);
    rec["sharpness"] = numOrNull(tr.sharpness, 3);
    rec["fitted"] = fitted;
    rec["lineSource"] = fitted ? "fit" : "ray";
    rec["residual"] = numOrNull(residual, 3);
    rec["stoppedAt"] = numOrNull(tr.stoppedAt, 3);
    Json pts = Json::array();
    std::vector<P2> coarsePts;
    for (const TrackPoint& q : tr.points) {
      pts.push_back(Json{{"s", round3(q.s)},
                         {"point", pxJson(q.point)},
                         {"mid", round3(q.mid)},
                         {"width", round3(q.width)},
                         {"peak", numOrNull(q.peak, 3)},
                         {"sharpness", numOrNull(q.sharpness, 3)},
                         {"onRay", q.onRay}});
      coarsePts.push_back(q.point);
    }
    rec["points"] = std::move(pts);
    ov.points("coarse", pxList(coarsePts));
    ov.polyline(fitted ? "path" : "ray", pxList(points), false, fitted ? "胶路" : "没拟合");
  }
  outputs.set("line", polylineRecord(s, points, tangents));
  Record out;
  out.type = kTrackType;
  out.data = std::move(rec);
  outputs.set("track", Data::record(std::move(out)));
  outputs.set("overlay", ov.data());
  return Status::Ok();
}

std::vector<Issue> validate(const ParamView& params, const std::set<std::string>&) {
  std::vector<Issue> issues;
  const auto zone = params.vec2("zone");
  if (!(zone[0] >= 0) || !(zone[1] >= zone[0] + 8)) {
    issues.push_back(Issue::error("bad_param",
                                  "zone 要满足 0 ≤ start 且 end 比 start 至少远 8 px（现在是 [" +
                                      fmt(zone[0], 0) + ", " + fmt(zone[1], 0) + "]）",
                                  "zone"));
  }
  const auto bearing = params.vec2("bearingRange");
  if (!(bearing[1] > bearing[0]) || bearing[1] - bearing[0] > 360.0 + 1e-6) {
    issues.push_back(
        Issue::error("bad_param", "bearingRange 要从小到大、跨度不超过 360°", "bearingRange"));
  }
  if (params.number("widthMax") < params.number("widthMin")) {
    issues.push_back(Issue::error("bad_param", "widthMax 不能小于 widthMin", "widthMax"));
  }
  return issues;
}

} // namespace

void registerTrackPath(Registry& r) {
  OperatorDesc op;
  op.id = "glue.track_path";
  op.version = "1.0.0";
  op.label = "粗跟踪与拟合";
  op.category = "涂胶/测量";
  op.keywords = {"glue", "track", "path", "fit", "涂胶", "跟踪", "拟合", "胶路", "测量"};
  op.doc =
      "从锚点沿起始方向，在检测区 zone 里每 coarseStep 走一步：每步在法向卡尺上取暗段（分段同 glue.lateral_runs），"
      "只留宽度在 [widthMin, widthMax] 的，过连续性门（紧接上一步横向不跳、断口后重新接上的要一样浓 / 一样宽 / 一样陡），"
      "取离预测点最近的作胶点；跟丢时可退回起始方向的射线上找。胶点 ≥ minPoints 时局部线性稳健拟合、高斯平滑成"
      "一条覆盖检测区的胶路（第一个点离锚点恰好 zone[0]，每 2 px 一点），否则输出沿起始方向的直线。\n"
      "接了 fan：起始方向 = fan.peaks[peakIndex].angle（没有这个峰时 valid = false、line 为空）。"
      "不输出 ok：覆盖率、陡度够不够由宿主判。";
  op.inputs = {
      Port{"response", "Image", "Response", "glue.response 的 response。", true},
      Port{"gray", "Image", "Gray", "glue.response 的 gray（逐点陡度）。", true},
      withContract(Port{"fan", "Record", "Fan",
                        "可选：glue.fan_scan 的 scan；接了就用它的第 peakIndex 个峰作起始方向。",
                        false},
                   {{"recordType", kFanScanType}}),
  };
  op.outputs = {
      withContract(Port{"line", "Record", "Line",
                        "glue.Polyline：拟合胶路（或沿起始方向的直线；valid = false 时为空）。",
                        true},
                   {{"recordType", kPolylineType}}),
      Port{
          "track", "Record", "Track",
          "glue.Track：headingSource、peakIndex、valid、heading、steps、found、coverage、sharpness、"
          "fitted、lineSource、residual、stoppedAt、points[{s, point, mid, width, peak, sharpness, onRay}]。",
          true},
      Port{"overlay", "Record", "Overlay", "lyflow.overlay2d：锚点、胶点、拟合胶路。", true},
  };

  const auto geom = [](Param p) {
    p.tuningRole = "geometry";
    return p;
  };
  const double kOpen = std::numeric_limits<double>::quiet_NaN();
  Param zone = geom(vec2Param("zone", "Zone", 100.0, 400.0, "px", {"Start", "End"},
                              "检测区：沿胶路离锚点的弧长 s 的一段。"));
  zone.min = 0.0;
  Param split = enumParam(
      "split", "Split", "connected", "卡尺暗段的拆分方式（1.1 跟踪用 connected）。",
      {EnumOption{"connected", "连通段", ""}, EnumOption{"half_max", "半高拆分", ""}}, true);
  op.params = {
      geom(vec2Param("anchor", "Anchor", 640.0, 512.0, "px", {"X", "Y"}, "锚点（出胶点）。")),
      geom(floatParam("heading", "Heading", -90.0, "deg", "起始方向（度；没接 fan 时用）。")),
      intParam("peakIndex", "Peak Index", 0, "接了 fan 时取第几个峰。", 0, 19),
      zone,
      geom(vec2Param("bearingRange", "Bearing Range", -180.0, 180.0, "deg", {"From", "To"},
                     "胶点相对锚点的方位范围；跨度 ≥ 360 时不查方位。", true)),
      rangedFloat("bearingMargin", "Bearing Margin", 20.0, "deg",
                  "胶点方位可超出 bearingRange 的余量。", 0.0, 180.0, true),
      rangedFloat("outwardRatio", "Outward Ratio", 0.75, "", "胶点离锚点至少 outwardRatio × s。",
                  0.0, 1.0, true),
      rangedFloat("searchHalf", "Search Half", 90.0, "px", "卡尺半长（1.1 = widthMax）。", 4.0,
                  400.0, true),
      split,
      rangedFloat("segmentMin", "Segment Min", 8.0, "", "分段门限：响应 > 它的相邻采样连成一段。",
                  0.0, kOpen, true),
      rangedFloat("peakMin", "Peak Min", 16.0, "", "段（及半高子段）的峰值下限。", 0.0, kOpen,
                  true),
      rangedFloat("mergeGap", "Merge Gap", 8.0, "px", "合并缝。", 0.0, kOpen, true),
      rangedFloat("mergePeakRatio", "Merge Peak Ratio", 0.0, "",
                  "较淡者峰值 ≥ 它 × 较浓者才合并；0 = 不查。", 0.0, 1.0, true),
      rangedFloat("mergeWidthMax", "Merge Width Max", 90.0, "px", "合并后不宽于它才合并。", 1.0,
                  kOpen, true),
      rangedFloat("widthMin", "Width Min", 4.0, "px", "胶点暗段宽度下限。", 0.0, kOpen, true),
      rangedFloat("widthMax", "Width Max", 90.0, "px", "胶点暗段宽度上限。", 0.0, kOpen, true),
      rangedFloat("acceptOffsetRatio", "Accept Offset Ratio", 0.6, "",
                  "胶点中心离预测点 < 它 × widthMax 才接受（也是退回射线的触发条件）。", 0.0, 1.0,
                  true),
      rangedFloat("coarseStep", "Coarse Step", 8.0, "px", "步长。", 1.0, 100.0, true),
      rangedFloat("maxGap", "Max Gap", 120.0, "px",
                  "找到过胶点后连续这么远没找到，就不再接受新胶点。", 0.0, kOpen, true),
      rangedFloat("gapSteps", "Gap Steps", 1.5, "",
                  "离上一个胶点超过 gapSteps × coarseStep 算断口之后。", 1.0, kOpen, true),
      intParam("historyMin", "History Min", 3, "已有这么多胶点后连续性门才生效。", 1, 100, true),
      rangedFloat("jumpRatio", "Jump Ratio", 0.5, "",
                  "紧接上一步时横向 ≤ max(jumpRatio × 胶宽中位数, jumpMin)。", 0.0, kOpen, true),
      rangedFloat("jumpMin", "Jump Min", 8.0, "px", "同上。", 0.0, kOpen, true),
      rangedFloat("reacquirePeakRatio", "Reacquire Peak Ratio", 0.5, "",
                  "断口后重新接上的峰值 ≥ 它 × 已有峰值中位数。", 0.0, kOpen, true),
      rangedFloat("reacquireWidthRatio", "Reacquire Width Ratio", 0.5, "",
                  "胶宽稳定时重新接上的宽度 ≥ 它 × 已有胶宽中位数。", 0.0, kOpen, true),
      rangedFloat("steadyIqrRatio", "Steady IQR Ratio", 0.3, "",
                  "胶宽 IQR < 它 × 中位数算稳定（直胶）。", 0.0, kOpen, true),
      floatParam("sharpMin", "Sharp Min", 0.4, "",
                 "断口后重新接上的暗段陡度（可算出时）下限；也是退回射线的前提。", true),
      boolParam("rayFallback", "Ray Fallback", true, "断口之后跟丢时退回起始方向的射线上找。",
                true),
      rangedFloat("rayFallbackCoverage", "Ray Fallback Coverage", 0.6, "",
                  "退回射线的前提：已有胶点 ≥ 它 × 到上一个胶点为止的步数。", 0.0, 1.0, true),
      rangedFloat("edgeProbe", "Edge Probe", 3.0, "px", "陡度：每条边内外各取这么远的灰度差。", 0.5,
                  20.0, true),
      rangedFloat("backgroundProbe", "Background Probe", 8.0, "px",
                  "陡度：暗度的背景取两边外这么远。", 1.0, 50.0, true),
      rangedFloat("dirWindow", "Dir Window", 48.0, "px", "用最近这么长 s 内的胶点做主方向。", 0.0,
                  kOpen, true),
      intParam("minPoints", "Min Points", 3, "胶点不少于它才拟合，否则输出直线。", 3, 1000, true),
      rangedFloat("fitWindow", "Fit Window", 40.0, "px", "局部线性拟合的半窗。", 1.0, kOpen, true),
      rangedFloat("fitSigma", "Fit Sigma", 12.0, "px", "拟合后的高斯平滑 σ；0 = 不平滑。", 0.0,
                  100.0, true),
  };
  op.capabilities = {/*cancellable=*/false, /*previewable=*/false, /*deterministic=*/true};
  op.compute = &compute;
  op.validate = &validate;
  r.addOperator(std::move(op));
}

} // namespace lyflow::packs::glue
