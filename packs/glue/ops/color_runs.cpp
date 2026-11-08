// glue.color_runs「彩色横向卡尺」（docs/glue-measure-ops.md §1.3.5）：沿胶路布站，每站卡尺上的全部胶色段与无信息区间。
// 输出 glue.LateralRuns 的超集；不按宽度筛、不挑段、不判有无胶 —— 无信息的站是不是断胶也由宿主判。
#include <algorithm>
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
  ColorCause loBackground = ColorCause::None; ///< 认作真边的那一侧外面是哪种无信息背景
  ColorCause hiBackground = ColorCause::None;
};

Json backgroundName(ColorCause c) { return c == ColorCause::None ? Json() : Json(causeName(c)); }

const char* edgeCause(bool complete, bool caliper, ColorCause c) {
  if (complete)
    return "edge";
  if (caliper)
    return "caliper";
  return c == ColorCause::None ? "edge" : causeName(c);
}

std::vector<ColorMergeGuide> mergeGuides(const ColorMapsView& maps,
                                         const std::vector<StationPose>& poses, double half,
                                         const RunSplitSpec& rawSpec, double gap, bool extent) {
  std::vector<ColorMergeGuide> seeds(poses.size()), guides(poses.size());
  std::vector<std::vector<Run>> rawRuns(poses.size());
  std::vector<double> widths;
  const auto count = static_cast<std::size_t>(std::floor(2.0 * half / kRes + 1e-9)) + 1;
  std::vector<char> background(count), noInfo(count);
  for (std::size_t i = 0; i < poses.size(); ++i) {
    const auto& st = poses[i];
    rawRuns[i] = caliperRuns(maps.score, st.c, st.n, half, rawSpec);
    auto runs = rawRuns[i];
    for (std::size_t k = 0; k < count; ++k) {
      const P2 p = st.c + st.n * (-half + k * kRes);
      const double score = maps.score.at(p);
      background[k] = score >= 0 && score < 0.5 * rawSpec.segmentMin;
      noInfo[k] = score < 0 && causeAt(maps.state, maps.frame, p) != ColorCause::Outside;
    }
    mergeAcrossGaps(&runs, background, half, gap, rawSpec.mergeWidthMax, nullptr,
                    extent ? &noInfo : nullptr);
    if (const Run* r = nearestRun(runs)) {
      seeds[i].width = r->width();
      seeds[i].center = r->mid();
      widths.push_back(r->width());
    }
  }
  if (widths.size() < 3)
    return guides;
  const double reference = median(widths);
  std::vector<std::size_t> anchors;
  for (std::size_t i = 0; i < seeds.size(); ++i) {
    const auto cost = [&](double width, double center) {
      return std::fabs(width - reference) + 2.0 * std::fabs(center);
    };
    for (const Run& r : rawRuns[i]) {
      if (!std::isfinite(seeds[i].width) ||
          cost(r.width(), r.mid()) < cost(seeds[i].width, seeds[i].center))
        seeds[i] = {r.width(), r.mid()};
    }
    if (seeds[i].width >= 0.7 * reference && seeds[i].width <= 1.3 * reference &&
        std::fabs(seeds[i].center) <= 0.5 * reference)
      anchors.push_back(i);
  }
  for (std::size_t i = 0; i < poses.size(); ++i) {
    auto nearby = anchors;
    nearby.erase(std::remove(nearby.begin(), nearby.end(), i), nearby.end());
    std::stable_sort(nearby.begin(), nearby.end(), [&](std::size_t a, std::size_t b) {
      return std::fabs(poses[a].s - poses[i].s) < std::fabs(poses[b].s - poses[i].s);
    });
    if (nearby.size() < 3)
      continue;
    nearby.resize(std::min<std::size_t>(nearby.size(), 5));
    std::vector<double> localWidths, centers;
    for (std::size_t j : nearby) {
      localWidths.push_back(seeds[j].width);
      centers.push_back(seeds[j].center);
    }
    guides[i] = {median(localWidths), median(centers)};
  }
  return guides;
}

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
  const bool extent = params.choice("widthPolicy") == "extent";
  if (extent)
    spec.mergeWidthMax = 2.0 * half;
  const double step = params.number("stationStep");
  const double mergeUnknownGap = params.number("mergeUnknownGap");
  const auto range = params.vec2("sRange");
  double a = 0, b = 0;
  const std::vector<StationPose> poses = layStations(line, step, range[0], range[1], &a, &b);
  const int count = static_cast<int>(std::floor(2.0 * half / kRes + 1e-9)) + 1;
  const auto tAt = [&](int i) { return -half + i * kRes; };
  RunSplitSpec rawSpec = spec;
  rawSpec.mergeGap = 0;
  const double baseGap = std::max(spec.mergeGap, mergeUnknownGap);
  const auto guides = mergeUnknownGap > 0 ? mergeGuides(maps, poses, half, rawSpec, baseGap, extent)
                                          : std::vector<ColorMergeGuide>(poses.size());

  Json stations = Json::array();
  Overlay2D ov;
  std::vector<std::array<Px, 2>> calipers, runSegs, unknownSegs;
  std::vector<double> v(static_cast<std::size_t>(count));
  std::vector<ColorCause> cause(static_cast<std::size_t>(count));
  for (std::size_t station = 0; station < poses.size(); ++station) {
    const StationPose& st = poses[station];
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
    const auto namedCause = [&](ColorCause c, int i) {
      if (extent && c == ColorCause::Outside && maps.image.width > 0 && maps.image.height > 0) {
        const P2 p = st.c + st.n * tAt(i);
        return p.x < 0 || p.y < 0 || p.x >= maps.image.width - 1 || p.y >= maps.image.height - 1
                   ? "image"
                   : "roi";
      }
      return causeName(c);
    };
    const auto boundaryCause = [&](bool complete, bool caliper, ColorCause c, double t, bool lo) {
      if (!complete && !caliper && c == ColorCause::Outside && extent) {
        const int i = lo ? static_cast<int>(std::ceil((t + half) / kRes - 1e-6)) - 1
                         : static_cast<int>(std::floor((t + half) / kRes + 1e-6)) + 1;
        return namedCause(c, i);
      }
      return edgeCause(complete, caliper, c);
    };
    // 无信息区间：同一原因的连续采样
    Json spans = Json::array();
    for (int i = 0; i < count;) {
      if (cause[i] == ColorCause::None) {
        ++i;
        continue;
      }
      int j = i;
      while (j + 1 < count && cause[j + 1] == cause[i] &&
             std::string(namedCause(cause[j + 1], j + 1)) == namedCause(cause[i], i))
        ++j;
      spans.push_back(Json{{"lo", tAt(i)}, {"hi", tAt(j)}, {"cause", namedCause(cause[i], i)}});
      const P2 p0 = st.c + st.n * tAt(i), p1 = st.c + st.n * tAt(j);
      unknownSegs.push_back({Px{p0.x, p0.y}, Px{p1.x, p1.y}});
      i = j + 1;
    }
    // 段的两条边外侧是什么：真边、卡尺端点，还是被无信息的采样截断
    std::vector<ColorRun> runs;
    for (const Run& r : caliperRuns(maps.score, st.c, st.n, half, rawSpec)) {
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
      if (extent) {
        if (!r.loComplete && !cr.loCaliper)
          cr.run.lo = tAt(lo + 1);
        if (!r.hiComplete && !cr.hiCaliper)
          cr.run.hi = tAt(hi - 1);
      }
      runs.push_back(cr);
    }
    // 被高光 / 暗区隔开的两段胶并回去（湿胶的高光条比 mergeGap 宽时）
    std::vector<char> background(static_cast<std::size_t>(count)), noInfo(background.size());
    for (int i = 0; i < count; ++i) {
      background[static_cast<std::size_t>(i)] = v[i] >= 0 && v[i] < 0.5 * spec.segmentMin;
      noInfo[static_cast<std::size_t>(i)] = v[i] < 0 && cause[i] != ColorCause::Outside;
    }
    std::vector<Run> plain;
    for (const ColorRun& cr : runs)
      plain.push_back(cr.run);
    std::vector<ColorRun> merged;
    for (const auto& [first, last] :
         mergeAcrossGaps(&plain, background, half, baseGap, spec.mergeWidthMax, &guides[station],
                         extent ? &noInfo : nullptr)) {
      ColorRun m = runs[first];
      m.run = plain[merged.size()];
      m.hiCause = runs[last].hiCause;
      m.hiCaliper = runs[last].hiCaliper;
      merged.push_back(m);
    }
    if (extent) {
      // 胶边外侧是成片的饱和 / 过暗背景（打磨面反光、黑色零件）时，这条边看得见，不当作被遮挡；
      // 半条胶落在高光里只剩半宽的，按邻站参考宽拦下
      std::vector<char> outside(static_cast<std::size_t>(count));
      for (int i = 0; i < count; ++i)
        outside[static_cast<std::size_t>(i)] = cause[i] == ColorCause::Outside;
      for (ColorRun& m : merged) {
        const bool loOpen = !m.run.loComplete, hiOpen = !m.run.hiComplete;
        if (completeAgainstBackground(&m.run, v, half, spec.segmentMin, guides[station].width,
                                      &outside)) {
          if (loOpen && m.run.loComplete)
            m.loBackground = m.loCause;
          if (hiOpen && m.run.hiComplete)
            m.hiBackground = m.hiCause;
        }
      }
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
      js.push_back(
          Json{{"lo", exact(r.lo)},
               {"hi", exact(r.hi)},
               {"width", exact(r.width())},
               {"mid", exact(r.mid())},
               {"peak", exact(r.peak)},
               {"parts", r.parts},
               {"loComplete", r.loComplete},
               {"hiComplete", r.hiComplete},
               {"loCause", boundaryCause(r.loComplete, cr.loCaliper, cr.loCause, r.lo, true)},
               {"hiCause", boundaryCause(r.hiComplete, cr.hiCaliper, cr.hiCause, r.hi, false)},
               {"loBackground", backgroundName(cr.loBackground)},
               {"hiBackground", backgroundName(cr.hiBackground)},
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
  rec["widthPolicy"] = extent ? "extent" : "range";
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
  op.version = "1.1.0";
  op.label = "彩色横向卡尺";
  op.category = "涂胶/彩色";
  op.keywords = {"glue", "color", "caliper", "runs", "width", "涂胶", "彩色", "卡尺", "胶宽"};
  op.doc =
      "沿胶路每 stationStep 一站，在法向 t ∈ [−searchHalf, searchHalf] 上每 0.5 px 采样 glue.color_map 的得分，"
      "按 glue.lateral_runs 的规则分段（segmentMin、peakMin、半高取边、mergeGap 合并）。另外：\n"
      "- 每站给出 infoFraction（有信息的采样占比）与 unknownSpans[{lo, hi, cause}]（cause = highlight / dark / outside）；\n"
      "- 每段给出两条边外侧是什么（loCause / hiCause = edge 真边 / caliper 卡尺端点 / highlight / dark / outside）、"
      "段内有效采样的平均得分 meanScore 与段内无信息的宽度 unknownWidth；\n"
      "- 合并不跨明确背景（有限得分 < segmentMin / 2），总宽不超过 mergeWidthMax。至少三个可信邻站时，"
      "允许缝宽到 max(mergeGap, mergeUnknownGap, 参考胶宽 / 2)，组合须符合参考宽度和中心，且优于每个原片段；"
      "外边被高光截断的标记保留。mergeUnknownGap = 0 关闭邻站自适应。\n"
      "输出 glue.LateralRuns 的超集（source = color；edgeLo、edgeHi、depth、sharpness 为 null，level 恒为 1）。"
      "widthPolicy=extent 时不受 mergeWidthMax 限制，合并仍限于 ±searchHalf，并保留背景阻断；"
      "观测边界 outside 在元数据可用时细分 roi/image。输出回显 widthPolicy。"
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
      rangedFloat("mergeGap", "Merge Gap", 8.0, "px", "无明确背景的短缝合并上限。", 0.0, kOpen,
                  true),
      rangedFloat(
          "mergeUnknownGap", "Merge Unknown Gap", 16.0, "px",
          "无明确背景的高光缝基础上限；可信邻站允许按半个参考胶宽自适应，仍检查组合宽度与中心。0 关闭该自适应。",
          0.0, kOpen, true),
      enumParam(
          "widthPolicy", "Width Policy", "range",
          "range 保留合并宽度上限；extent 由有限 searchHalf 限定合并宽度，仍保留背景阻断和邻站证据。",
          {EnumOption{"range", "宽度范围", ""}, EnumOption{"extent", "完整边界", ""}}, true),
      rangedFloat("mergeWidthMax", "Merge Width Max", 90.0, "px", "合并后不宽于它才合并。", 1.0,
                  kOpen, true),
  };
  op.capabilities = {/*cancellable=*/false, /*previewable=*/false, /*deterministic=*/true};
  op.compute = &compute;
  r.addOperator(std::move(op));
}

} // namespace lyflow::packs::glue
