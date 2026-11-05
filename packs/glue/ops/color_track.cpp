// glue.color_track「彩色粗跟踪」（docs/glue-measure-ops.md §1.3.4）：在得分图上沿一个方向逐步找胶点，稳健拟合光滑胶路。
// 高光、暗区挡住的一步是「无信息」，不计入 maxGap；不输出 ok —— 覆盖率够不够由宿主判。
#include <cmath>

#include "glue.h"

namespace lyflow::packs::glue {
namespace {

ColorTrackSpec specOf(const ParamView& params) {
  ColorTrackSpec spec;
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
  spec.runs.mergePeakRatio = 0;
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
  spec.reacquireWidthRatio = params.number("reacquireWidthRatio");
  spec.steadyIqrRatio = params.number("steadyIqrRatio");
  spec.dirWindow = params.number("dirWindow");
  spec.infoMin = params.number("infoMin");
  spec.mergeUnknownGap = params.number("mergeUnknownGap");
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
  ColorMapsView maps;
  if (!readColorMaps(inputs.get("maps"), &maps))
    return Status::Error(Phase::Execute, "bad_input",
                         "需要 glue.color_map 输出的 maps（Bundle<glue.ColorMaps>）", {}, "maps");
  const ColorTrackSpec spec = specOf(params);

  Json rec;
  const bool fromFan = inputs.has("fan");
  const int peakIndex = static_cast<int>(params.integer("peakIndex"));
  double heading = params.number("heading");
  bool valid = true;
  if (fromFan) {
    const Record* fan = inputs.get("fan").asRecord();
    if (!fan || fan->type != kFanScanType)
      return Status::Error(Phase::Execute, "bad_input", "fan 不是 glue.FanScan", {}, "fan");
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
  rec["heading"] = valid ? exact(heading) : Json();
  rec["anchor"] = exactPx(spec.anchor);
  rec["zone"] = Json::array({spec.zoneStart, spec.zoneEnd});
  rec["bearingRange"] = Json::array({spec.bearingFrom, spec.bearingTo});

  Overlay2D ov;
  ov.circle("anchor", Px{spec.anchor.x, spec.anchor.y}, 6.0, "锚点");
  std::vector<double> s;
  std::vector<P2> points, tangents;
  if (!valid) {
    for (const char* k : {"steps", "found", "unknownSteps", "coverage", "coverageInformative",
                          "sharpness", "fitted", "residual", "stoppedAt"})
      rec[k] = Json();
    rec["lineSource"] = "none";
    rec["unknownS"] = Json::array();
    rec["points"] = Json::array();
  } else {
    const ColorTrackResult tr = colorTrack(maps.score, spec, heading);
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
    rec["unknownSteps"] = tr.unknownSteps;
    rec["coverage"] = exact(tr.coverage);
    rec["coverageInformative"] = exact(tr.coverageInformative);
    rec["sharpness"] = Json();
    rec["fitted"] = fitted;
    rec["lineSource"] = fitted ? "fit" : "ray";
    rec["residual"] = exact(residual);
    rec["stoppedAt"] = exact(tr.stoppedAt);
    Json unknown = Json::array();
    for (double u : tr.unknownS)
      unknown.push_back(exact(u));
    rec["unknownS"] = std::move(unknown);
    Json pts = Json::array();
    std::vector<P2> coarsePts;
    for (const TrackPoint& q : tr.points) {
      pts.push_back(Json{{"s", exact(q.s)},
                         {"point", exactPx(q.point)},
                         {"mid", exact(q.mid)},
                         {"width", exact(q.width)},
                         {"peak", exact(q.peak)},
                         {"sharpness", Json()},
                         {"onRay", false}});
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

void registerColorTrack(Registry& r) {
  OperatorDesc op;
  op.id = "glue.color_track";
  op.version = "1.0.0";
  op.label = "彩色粗跟踪";
  op.category = "涂胶/彩色";
  op.keywords = {"glue", "color", "track", "path", "fit", "涂胶", "彩色", "跟踪", "拟合", "胶路"};
  op.doc =
      "glue.track_path 的彩色版：从锚点沿起始方向，在检测区 zone 里每 coarseStep 一步，在得分图的法向卡尺上取段"
      "（分段同 glue.lateral_runs），只留宽度在 [widthMin, widthMax] 的，过连续性门后取离预测点最近的作胶点；"
      "没有陡度门、不退回射线。没找到胶、且接受窗（± acceptOffsetRatio × widthMax）里有效采样不到 infoMin 的一步"
      "是「无信息」（高光、暗区挡住了），不计入 maxGap。胶点 ≥ minPoints 时稳健拟合成胶路，否则输出直线。\n"
      "track 另给 unknownSteps 与 coverageInformative = found / (steps − unknownSteps)。不输出 ok。";
  op.inputs = {
      Port{"maps", "Bundle<glue.ColorMaps>", "Maps",
           "glue.color_map 的 maps（score 找胶点；NaN 与块外的采样算无效）。", true},
      withContract(Port{"fan", "Record", "Fan",
                        "可选：glue.color_fan 的 scan；接了就用它的第 peakIndex 个峰作起始方向。",
                        false},
                   {{"recordType", kFanScanType}}),
  };
  op.outputs = {
      withContract(Port{"line", "Record", "Line",
                        "glue.Polyline：拟合胶路（或沿起始方向的直线；valid = false 时为空）。",
                        true},
                   {{"recordType", kPolylineType}}),
      Port{"track", "Record", "Track",
           "glue.Track：与 glue.track_path 同形（sharpness 恒为 null），另有 unknownSteps、"
           "coverageInformative、unknownS[]。",
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
      rangedFloat("searchHalf", "Search Half", 90.0, "px", "卡尺半长。", 4.0, 400.0, true),
      enumParam("split", "Split", "connected", "卡尺上得分段的拆分方式。",
                {EnumOption{"connected", "连通段", ""}, EnumOption{"half_max", "半高拆分", ""}},
                true),
      rangedFloat("segmentMin", "Segment Min", 0.3, "", "分段门限：得分 > 它的相邻采样连成一段。",
                  0.0, 1.0, true),
      rangedFloat("peakMin", "Peak Min", 0.6, "", "段的得分峰值下限。", 0.0, 1.0, true),
      rangedFloat("mergeGap", "Merge Gap", 8.0, "px", "合并缝。", 0.0, kOpen, true),
      rangedFloat("mergeWidthMax", "Merge Width Max", 90.0, "px", "合并后不宽于它才合并。", 1.0,
                  kOpen, true),
      rangedFloat(
          "mergeUnknownGap", "Merge Unknown Gap", 16.0, "px",
          "同 glue.color_runs：两段之间缝里明确是背景的采样不过半时，缝不超过它就合并，胶点取合并段的中点"
          "（湿胶中间一条反光带把胶劈成两半时不只跟一半）。",
          0.0, kOpen, true),
      rangedFloat("widthMin", "Width Min", 4.0, "px", "胶点段宽度下限。", 0.0, kOpen, true),
      rangedFloat("widthMax", "Width Max", 90.0, "px", "胶点段宽度上限。", 0.0, kOpen, true),
      rangedFloat("acceptOffsetRatio", "Accept Offset Ratio", 0.6, "",
                  "胶点中心离预测点 < 它 × widthMax 才接受；这个窗也用来算无信息。", 0.0, 1.0,
                  true),
      rangedFloat("coarseStep", "Coarse Step", 8.0, "px", "步长。", 1.0, 100.0, true),
      rangedFloat("maxGap", "Max Gap", 120.0, "px",
                  "找到过胶点后，有信息却没找到的步累计这么远，就不再接受新胶点。", 0.0, kOpen,
                  true),
      rangedFloat("gapSteps", "Gap Steps", 1.5, "",
                  "离上一个胶点超过 gapSteps × coarseStep 算断口之后。", 1.0, kOpen, true),
      intParam("historyMin", "History Min", 3, "已有这么多胶点后连续性门才生效。", 1, 100, true),
      rangedFloat("jumpRatio", "Jump Ratio", 0.5, "",
                  "紧接上一步时横向 ≤ max(jumpRatio × 胶宽中位数, jumpMin)。", 0.0, kOpen, true),
      rangedFloat("jumpMin", "Jump Min", 8.0, "px", "同上。", 0.0, kOpen, true),
      rangedFloat("reacquireWidthRatio", "Reacquire Width Ratio", 0.5, "",
                  "胶宽稳定时重新接上的宽度 ≥ 它 × 已有胶宽中位数。", 0.0, kOpen, true),
      rangedFloat("steadyIqrRatio", "Steady IQR Ratio", 0.3, "",
                  "胶宽 IQR < 它 × 中位数算稳定（直胶）。", 0.0, kOpen, true),
      rangedFloat("dirWindow", "Dir Window", 48.0, "px", "用最近这么长 s 内的胶点做主方向。", 0.0,
                  kOpen, true),
      rangedFloat("infoMin", "Info Min", 0.5, "",
                  "接受窗里有效采样占比低于它、又没找到胶的一步算无信息。", 0.0, 1.0, true),
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
