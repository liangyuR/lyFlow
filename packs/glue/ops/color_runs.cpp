// glue.color_runs「彩色横向卡尺」（docs/glue-measure-ops.md §1.3.5）：沿胶路布站，每站卡尺上的全部胶色段与无信息区间。
// 输出 glue.LateralRuns 的超集；不按宽度筛、不挑段、不判有无胶 —— 无信息的站是不是断胶也由宿主判。
#include <cmath>

#include "glue.h"

namespace lyflow::packs::glue {
namespace {

constexpr double kRes = 0.5; // 与 caliperRuns 的采样间距相同

struct ColorRun {
  Run run;
  ColorCause loCause = ColorCause::None; ///< None = 降到半高以下的真边
  ColorCause hiCause = ColorCause::None;
  bool loCaliper = false; ///< 段顶到了卡尺端点
  bool hiCaliper = false;
};

const char* edgeCause(bool complete, bool caliper, ColorCause c) {
  if (complete)
    return "edge";
  if (caliper)
    return "caliper";
  return c == ColorCause::None ? "edge" : causeName(c);
}

bool unknown(ColorCause c) { return c == ColorCause::Highlight || c == ColorCause::Dark; }

Status compute(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext&) {
  ColorMapsView maps;
  if (!readColorMaps(inputs.get("maps"), &maps))
    return Status::Error(Phase::Execute, "bad_input",
                         "需要 glue.color_map 输出的 maps（Bundle<glue.ColorMaps>）", {}, "maps");
  Polyline line;
  if (!readLine(inputs.get("line"), &line))
    return Status::Error(Phase::Execute, "bad_input", "line 不是 glue.Polyline", {}, "line");

  RunSplitSpec spec;
  spec.halfMax = params.choice("split") == "half_max";
  spec.segmentMin = params.number("segmentMin");
  spec.peakMin = params.number("peakMin");
  spec.mergeGap = params.number("mergeGap");
  spec.mergePeakRatio = 0;
  spec.mergeWidthMax = params.number("mergeWidthMax");
  const double half = params.number("searchHalf");
  const double step = params.number("stationStep");
  const double mergeUnknownGap = params.number("mergeUnknownGap");
  const auto range = params.vec2("sRange");
  double a = 0, b = 0;
  const std::vector<StationPose> poses = layStations(line, step, range[0], range[1], &a, &b);
  const int count = static_cast<int>(std::floor(2.0 * half / kRes + 1e-9)) + 1;
  const auto tAt = [&](int i) { return -half + i * kRes; };

  Json stations = Json::array();
  Overlay2D ov;
  std::vector<std::array<Px, 2>> calipers, runSegs, unknownSegs;
  std::vector<double> v(static_cast<std::size_t>(count));
  std::vector<ColorCause> cause(static_cast<std::size_t>(count));
  for (const StationPose& st : poses) {
    int inFrame = 0, valid = 0;
    for (int i = 0; i < count; ++i) {
      const P2 p = st.c + st.n * tAt(i);
      v[i] = maps.score.at(p);
      cause[i] = v[i] >= 0 ? ColorCause::None : causeAt(maps.state, maps.frame, p);
      if (v[i] >= 0)
        ++valid;
      if (cause[i] != ColorCause::Outside)
        ++inFrame;
    }
    // 无信息区间：同一原因的连续采样
    Json spans = Json::array();
    for (int i = 0; i < count;) {
      if (cause[i] == ColorCause::None) {
        ++i;
        continue;
      }
      int j = i;
      while (j + 1 < count && cause[j + 1] == cause[i])
        ++j;
      spans.push_back(Json{{"lo", tAt(i)}, {"hi", tAt(j)}, {"cause", causeName(cause[i])}});
      const P2 p0 = st.c + st.n * tAt(i), p1 = st.c + st.n * tAt(j);
      unknownSegs.push_back({Px{p0.x, p0.y}, Px{p1.x, p1.y}});
      i = j + 1;
    }
    // 段的两条边外侧是什么：真边、卡尺端点，还是被无信息的采样截断
    std::vector<ColorRun> runs;
    for (const Run& r : caliperRuns(maps.score, st.c, st.n, half, spec)) {
      ColorRun cr;
      cr.run = r;
      const int lo = static_cast<int>(std::ceil((r.lo + half) / kRes - 1e-6)) - 1;
      const int hi = static_cast<int>(std::floor((r.hi + half) / kRes + 1e-6)) + 1;
      cr.loCaliper = lo < 0;
      cr.hiCaliper = hi >= count;
      if (!r.loComplete && !cr.loCaliper)
        cr.loCause = cause[lo];
      if (!r.hiComplete && !cr.hiCaliper)
        cr.hiCause = cause[hi];
      runs.push_back(cr);
    }
    // 被高光 / 暗区隔开的两段胶并回去（湿胶的高光条比 mergeGap 宽时）。反光点的芯饱和、四周一圈发白却没饱和，
    // 那一圈有信息但得分低（两半相邻的边因此是「真边」）：只看缝里无信息的采样是否过半
    std::vector<ColorRun> merged;
    for (const ColorRun& cr : runs) {
      if (!merged.empty()) {
        ColorRun& m = merged.back();
        const int from = static_cast<int>(std::floor((m.run.hi + half) / kRes + 1e-6)) + 1;
        const int to = static_cast<int>(std::ceil((cr.run.lo + half) / kRes - 1e-6)) - 1;
        int blind = 0, total = 0;
        for (int i = std::max(from, 0); i <= std::min(to, count - 1); ++i) {
          ++total;
          blind += unknown(cause[i]) ? 1 : 0;
        }
        if (total > 0 && 2 * blind >= total && cr.run.lo - m.run.hi <= mergeUnknownGap &&
            cr.run.hi - m.run.lo <= spec.mergeWidthMax) {
          m.run.hi = cr.run.hi;
          m.run.hiComplete = cr.run.hiComplete;
          m.run.completeEdges = m.run.completeEdges && cr.run.completeEdges;
          m.run.peak = std::max(m.run.peak, cr.run.peak);
          m.run.parts += cr.run.parts;
          m.hiCause = cr.hiCause;
          m.hiCaliper = cr.hiCaliper;
          continue;
        }
      }
      merged.push_back(cr);
    }
    Json js = Json::array();
    for (const ColorRun& cr : merged) {
      const Run& r = cr.run;
      double sum = 0;
      int n = 0, blind = 0;
      for (int i = 0; i < count; ++i) {
        const double t = tAt(i);
        if (t < r.lo || t > r.hi)
          continue;
        if (v[i] >= 0) {
          sum += v[i];
          ++n;
        } else {
          ++blind;
        }
      }
      js.push_back(Json{{"lo", exact(r.lo)},
                        {"hi", exact(r.hi)},
                        {"width", exact(r.width())},
                        {"mid", exact(r.mid())},
                        {"peak", exact(r.peak)},
                        {"parts", r.parts},
                        {"loComplete", r.loComplete},
                        {"hiComplete", r.hiComplete},
                        {"loCause", edgeCause(r.loComplete, cr.loCaliper, cr.loCause)},
                        {"hiCause", edgeCause(r.hiComplete, cr.hiCaliper, cr.hiCause)},
                        {"meanScore", n > 0 ? exact(sum / n) : Json()},
                        {"unknownWidth", blind * kRes},
                        {"edgeLo", Json()},
                        {"edgeHi", Json()},
                        {"depth", Json()},
                        {"sharpness", Json()},
                        {"level", 1}});
      const P2 p0 = st.c + st.n * r.lo, p1 = st.c + st.n * r.hi;
      runSegs.push_back({Px{p0.x, p0.y}, Px{p1.x, p1.y}});
    }
    stations.push_back(Json{{"s", exact(st.s)},
                            {"center", exactPx(st.c)},
                            {"normal", exactPx(st.n)},
                            {"validFraction", count > 0 ? exact(double(inFrame) / count) : Json()},
                            {"infoFraction", count > 0 ? exact(double(valid) / count) : Json()},
                            {"unknownSpans", std::move(spans)},
                            {"runs", std::move(js)}});
    const P2 e0 = st.c - st.n * half, e1 = st.c + st.n * half;
    calipers.push_back({Px{e0.x, e0.y}, Px{e1.x, e1.y}});
  }

  Json rec;
  rec["count"] = poses.size();
  rec["source"] = "color";
  rec["stationStep"] = step;
  rec["sRange"] = poses.empty() ? Json() : Json::array({exact(a), exact(b)});
  rec["searchHalf"] = half;
  rec["split"] = spec.halfMax ? "half_max" : "connected";
  rec["mergeUnknownGap"] = mergeUnknownGap;
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
  ov.segments("unknown", unknownSegs);
  outputs.set("overlay", ov.data());
  return Status::Ok();
}

} // namespace

void registerColorRuns(Registry& r) {
  OperatorDesc op;
  op.id = "glue.color_runs";
  op.version = "1.0.0";
  op.label = "彩色横向卡尺";
  op.category = "涂胶/彩色";
  op.keywords = {"glue", "color", "caliper", "runs", "width", "涂胶", "彩色", "卡尺", "胶宽"};
  op.doc =
      "沿胶路每 stationStep 一站，在法向 t ∈ [−searchHalf, searchHalf] 上每 0.5 px 采样 glue.color_map 的得分，"
      "按 glue.lateral_runs 的规则分段（segmentMin、peakMin、半高取边、mergeGap 合并）。另外：\n"
      "- 每站给出 infoFraction（有信息的采样占比）与 unknownSpans[{lo, hi, cause}]（cause = highlight / dark / outside）；\n"
      "- 每段给出两条边外侧是什么（loCause / hiCause = edge 真边 / caliper 卡尺端点 / highlight / dark / outside）、"
      "段内有效采样的平均得分 meanScore 与段内无信息的宽度 unknownWidth；\n"
      "- 两段之间缝里的采样过半是高光 / 暗区、缝 ≤ mergeUnknownGap、合并后 ≤ mergeWidthMax 时并成一段（湿胶被高光劈开）。\n"
      "输出 glue.LateralRuns 的超集（source = color；edgeLo、edgeHi、depth、sharpness 为 null，level 恒为 1）。"
      "不按宽度筛、不挑段、不判有无胶。";
  op.inputs = {
      Port{"maps", "Bundle<glue.ColorMaps>", "Maps",
           "glue.color_map 的 maps（score 分段、state 给无信息的原因）。", true},
      withContract(Port{"line", "Record", "Line", "glue.Polyline：胶路。", true},
                   {{"recordType", kPolylineType}}),
  };
  op.outputs = {
      Port{
          "runs", "Record", "Runs",
          "glue.LateralRuns（source = color）：count、stationStep、sRange、searchHalf、split、mergeUnknownGap、"
          "stations[{s, center, normal, validFraction, infoFraction, unknownSpans[], runs[{lo, hi, width, mid, "
          "peak, parts, loComplete, hiComplete, loCause, hiCause, meanScore, unknownWidth, …}]}]。",
          true},
      Port{"overlay", "Record", "Overlay", "lyflow.overlay2d：胶路、卡尺线、胶色段、无信息区间。",
           true},
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
      rangedFloat("segmentMin", "Segment Min", 0.3, "", "分段门限：得分 > 它的相邻采样连成一段。",
                  0.0, 1.0),
      rangedFloat("peakMin", "Peak Min", 0.6, "", "段的得分峰值下限。", 0.0, 1.0),
      rangedFloat("mergeGap", "Merge Gap", 8.0, "px",
                  "合并缝（不看缝里是什么）：胶面上没饱和的镜面反光会把胶劈成两段。", 0.0, kOpen,
                  true),
      rangedFloat("mergeUnknownGap", "Merge Unknown Gap", 16.0, "px",
                  "两段之间缝里的采样过半是高光 / 暗区时，缝不超过它就合并。", 0.0, kOpen, true),
      rangedFloat("mergeWidthMax", "Merge Width Max", 90.0, "px", "合并后不宽于它才合并。", 1.0,
                  kOpen, true),
  };
  op.capabilities = {/*cancellable=*/false, /*previewable=*/false, /*deterministic=*/true};
  op.compute = &compute;
  r.addOperator(std::move(op));
}

} // namespace lyflow::packs::glue
