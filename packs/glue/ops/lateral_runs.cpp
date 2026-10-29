// glue.lateral_runs「横向卡尺」（docs/glue-measure-ops.md §4）：沿胶路布站，每站卡尺上的全部暗段。
// 不按宽度筛、不挑段、不算参考宽、不判有无胶 —— 那些归宿主。
#include <cmath>

#include "glue.h"

namespace lyflow::packs::glue {
namespace {

Json runJson(const Run& r, const RunEdges& e, double level) {
  return Json{{"lo", exact(r.lo)},
              {"hi", exact(r.hi)},
              {"width", exact(r.width())},
              {"mid", exact(r.mid())},
              {"peak", exact(r.peak)},
              {"parts", r.parts},
              {"loComplete", r.loComplete},
              {"hiComplete", r.hiComplete},
              {"edgeLo", exact(e.edgeLo)},
              {"edgeHi", exact(e.edgeHi)},
              {"depth", exact(e.depth)},
              {"sharpness", exact(e.sharpness)},
              {"level", level >= 0 ? exact(level) : Json()}};
}

Status compute(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext&) {
  MapsView maps;
  if (!readMaps(inputs.get("maps"), &maps))
    return badImage("maps", true);
  const Field& R = maps.response;
  const Field& g = maps.gray;
  const Field& level = maps.level;
  Polyline line;
  if (!readLine(inputs.get("line"), &line))
    return Status::Error(Phase::Execute, "bad_input", "line 不是 glue.Polyline", {}, "line");

  RunSplitSpec spec;
  spec.halfMax = params.choice("split") == "half_max";
  spec.segmentMin = params.number("segmentMin");
  spec.peakMin = params.number("peakMin");
  spec.mergeGap = params.number("mergeGap");
  spec.mergePeakRatio = params.number("mergePeakRatio");
  spec.mergeWidthMax = params.number("mergeWidthMax");
  const double half = params.number("searchHalf");
  const double step = params.number("stationStep");
  const double edgeProbe = params.number("edgeProbe");
  const double backgroundProbe = params.number("backgroundProbe");
  const auto range = params.vec2("sRange");
  double a = 0, b = 0;
  const std::vector<StationPose> poses = layStations(line, step, range[0], range[1], &a, &b);

  Json stations = Json::array();
  Overlay2D ov;
  std::vector<std::array<Px, 2>> calipers, runSegs;
  for (const StationPose& st : poses) {
    double valid = 0;
    const std::vector<Run> runs = caliperRuns(R, st.c, st.n, half, spec, nullptr, &valid);
    Json js = Json::array();
    for (const Run& r : runs) {
      const RunEdges e = runEdges(g, st.c, st.n, r, edgeProbe, backgroundProbe);
      // 与 1.1 示教胶路的对比度同一个取法：lo, lo + 1, …（< hi）与 hi 上的最大值
      double lv = level.at(st.c + st.n * r.hi);
      for (double t = r.lo; t < r.hi; t += 1.0)
        lv = std::max(lv, level.at(st.c + st.n * t));
      js.push_back(runJson(r, e, lv));
      const P2 p0 = st.c + st.n * r.lo, p1 = st.c + st.n * r.hi;
      runSegs.push_back({Px{p0.x, p0.y}, Px{p1.x, p1.y}});
    }
    stations.push_back(Json{{"s", exact(st.s)},
                            {"center", exactPx(st.c)},
                            {"normal", exactPx(st.n)},
                            {"validFraction", exact(valid)},
                            {"runs", std::move(js)}});
    const P2 e0 = st.c - st.n * half, e1 = st.c + st.n * half;
    calipers.push_back({Px{e0.x, e0.y}, Px{e1.x, e1.y}});
  }

  Json rec;
  rec["count"] = poses.size();
  rec["stationStep"] = step;
  rec["sRange"] = poses.empty() ? Json() : Json::array({exact(a), exact(b)});
  rec["searchHalf"] = half;
  rec["split"] = spec.halfMax ? "half_max" : "connected";
  rec["stations"] = std::move(stations);
  Record out;
  out.type = kLateralRunsType;
  out.data = std::move(rec);
  outputs.set("runs", Data::record(std::move(out)));

  std::vector<Px> path;
  for (const P2& p : line.points)
    path.push_back(Px{p.x, p.y});
  if (!path.empty())
    ov.polyline("path", path, false, "胶路");
  ov.segments("station", calipers);
  ov.segments("run", runSegs);
  outputs.set("overlay", ov.data());
  return Status::Ok();
}

} // namespace

void registerLateralRuns(Registry& r) {
  OperatorDesc op;
  op.id = "glue.lateral_runs";
  op.version = "1.0.0";
  op.label = "横向卡尺";
  op.category = "涂胶/测量";
  op.keywords = {"glue", "caliper", "runs", "width", "涂胶", "卡尺", "暗段", "胶宽", "测量"};
  op.doc =
      "沿胶路每 stationStep 一站，在法向 t ∈ [−searchHalf, searchHalf] 上每 0.5 px 采样响应，输出全部暗段："
      "响应 > segmentMin 的相邻采样连成连通段（峰值 < peakMin 的丢掉），按连通段自己的半高取边 —— connected 一个连通段"
      "一段，half_max 每串连续 ≥ 半高的采样各一段；相邻两段缝 ≤ mergeGap、合并后 ≤ mergeWidthMax、峰值比够时合并。\n"
      "每段给出 lo / hi / width / mid（沿法向相对胶路）、peak、两边是否完整、D15 陡度的组成（edgeLo、edgeHi、depth、"
      "sharpness）与段内最亮的局部亮度 level。不按宽度筛、不挑段、不判有无胶。";
  op.inputs = {
      Port{
          "maps", "Bundle<glue.ResponseMaps>", "Maps",
          "glue.response 的 maps（response 分段、gray 算陡度、level 取局部亮度；块外的采样算无效）。",
          true},
      withContract(Port{"line", "Record", "Line", "glue.Polyline：胶路。", true},
                   {{"recordType", kPolylineType}}),
  };
  op.outputs = {
      Port{
          "runs", "Record", "Runs",
          "glue.LateralRuns：count、stationStep、sRange、searchHalf、split、stations[{s, center, normal, "
          "validFraction, runs[{lo, hi, width, mid, peak, parts, loComplete, hiComplete, edgeLo, edgeHi, "
          "depth, sharpness, level}]}]。",
          true},
      Port{"overlay", "Record", "Overlay", "lyflow.overlay2d：胶路、卡尺线、每段暗段。", true},
  };
  const double kOpen = std::numeric_limits<double>::quiet_NaN();
  Param sRange = vec2Param("sRange", "S Range", 0.0, 0.0, "px", {"From", "To"},
                           "布站范围；[0, 0] = 整条胶路，否则与胶路求交。");
  sRange.tuningRole = "geometry";
  op.params = {
      rangedFloat("stationStep", "Station Step", 4.0, "px", "站距。", 0.5, 200.0),
      sRange,
      rangedFloat("searchHalf", "Search Half", 60.0, "px", "卡尺半长。", 1.0, 600.0),
      enumParam("split", "Split", "half_max",
                "half_max = 按连通段的半高拆成子段；connected = 一个连通段一段。",
                {EnumOption{"half_max", "半高拆分", ""}, EnumOption{"connected", "连通段", ""}}),
      rangedFloat("segmentMin", "Segment Min", 8.0, "", "分段门限：响应 > 它的相邻采样连成一段。",
                  0.0, kOpen),
      rangedFloat("peakMin", "Peak Min", 16.0, "", "段峰值下限。", 0.0, kOpen),
      rangedFloat("mergeGap", "Merge Gap", 8.0, "px", "合并缝。", 0.0, kOpen, true),
      rangedFloat("mergePeakRatio", "Merge Peak Ratio", 0.4, "",
                  "较淡者峰值 ≥ 它 × 较浓者才合并；0 = 不查。", 0.0, 1.0, true),
      rangedFloat("mergeWidthMax", "Merge Width Max", 90.0, "px", "合并后不宽于它才合并。", 1.0,
                  kOpen, true),
      rangedFloat("edgeProbe", "Edge Probe", 3.0, "px", "陡度：每条边内外各取这么远的灰度差。", 0.5,
                  20.0, true),
      rangedFloat("backgroundProbe", "Background Probe", 8.0, "px",
                  "陡度：暗度的背景取两边外这么远。", 1.0, 50.0, true),
  };
  op.capabilities = {/*cancellable=*/false, /*previewable=*/false, /*deterministic=*/true};
  op.compute = &compute;
  r.addOperator(std::move(op));
}

} // namespace lyflow::packs::glue
