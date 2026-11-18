#include "glue.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace lyflow::packs::solution_glue {
namespace {

using lyflow::std_image::inspection::boundaryCause;
using lyflow::std_image::inspection::caliperOffset;
using lyflow::std_image::inspection::informative;
using lyflow::std_image::inspection::outside;
using lyflow::std_image::inspection::sampleCause;
using lyflow::std_image::inspection::samplesBeyond;
using lyflow::std_image::inspection::unknownSpans;
using lyflow::std_image::line_scan::kCaliperStep;
using lyflow::std_image::line_scan::mergeCaliperRuns;

struct Station {
  double s;
  P2 center, normal;
  std::vector<Run> raw;
  std::vector<CaliperSampleKind> kinds;
  std::vector<char> background, noInfo;
};

std::vector<ColorMergeGuide> mergeGuides(const std::vector<Station>& stations, double half,
                                         double gap, double widthMax, bool extent) {
  std::vector<ColorMergeGuide> seeds(stations.size()), guides(stations.size());
  std::vector<double> widths;
  for (std::size_t i = 0; i < stations.size(); ++i) {
    const auto& st = stations[i];
    auto runs = st.raw;
    mergeAcrossGaps(&runs, st.background, half, gap, widthMax, nullptr,
                    extent ? &st.noInfo : nullptr, extent);
    if (const Run* r = nearestRun(runs)) {
      seeds[i] = {r->width(), r->mid()};
      widths.push_back(r->width());
    }
  }
  if (widths.size() < 3)
    return guides;
  const double reference = median(widths);
  std::vector<std::size_t> anchors;
  const auto cost = [&](double width, double center) {
    return std::fabs(width - reference) + 2.0 * std::fabs(center);
  };
  for (std::size_t i = 0; i < seeds.size(); ++i) {
    for (const Run& r : stations[i].raw) {
      if (!std::isfinite(seeds[i].width) ||
          cost(r.width(), r.mid()) < cost(seeds[i].width, seeds[i].center))
        seeds[i] = {r.width(), r.mid()};
    }
    if (seeds[i].width >= 0.7 * reference && seeds[i].width <= 1.3 * reference &&
        std::fabs(seeds[i].center) <= 0.5 * reference)
      anchors.push_back(i);
  }
  for (std::size_t i = 0; i < stations.size(); ++i) {
    auto nearby = anchors;
    nearby.erase(std::remove(nearby.begin(), nearby.end(), i), nearby.end());
    std::stable_sort(nearby.begin(), nearby.end(), [&](std::size_t a, std::size_t b) {
      return std::fabs(stations[a].s - stations[i].s) < std::fabs(stations[b].s - stations[i].s);
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

bool readObservation(const Json& metadata, const Tensor& trace, double half, double widthMax,
                     std::vector<Station>& stations) {
  const auto& series = metadata.at("stations");
  if (!series.is_array() || trace.shape.size() != 3 || trace.shape[2] != 2 ||
      trace.shape[0] != static_cast<std::int64_t>(series.size()) ||
      trace.shape[1] != std::floor(2.0 * half / kCaliperStep + 1e-9) + 1 || trace.shape[1] <= 0 ||
      trace.shape[1] > std::numeric_limits<int>::max())
    return false;
  const auto count = static_cast<std::size_t>(trace.shape[1]);
  RunSplitSpec rawSpec;
  rawSpec.mergeGap = 0;
  rawSpec.mergeWidthMax = widthMax;
  stations.reserve(series.size());
  for (const auto& row : series) {
    Station st;
    st.s = row.at("s").get<double>();
    st.center = pxOf(row.at("center"));
    st.normal = pxOf(row.at("normal"));
    if (!std::isfinite(st.s) || !finite(st.center) || !finite(st.normal))
      return false;
    std::vector<Run> raw;
    for (const auto& entry : row.at("runs")) {
      Run r;
      r.lo = entry.at("lo").get<double>();
      r.hi = entry.at("hi").get<double>();
      r.peak = entry.at("peak").get<double>();
      if (!std::isfinite(r.lo) || !std::isfinite(r.hi) || !std::isfinite(r.peak))
        return false;
      r.loComplete = entry.at("loComplete").get<bool>();
      r.hiComplete = entry.at("hiComplete").get<bool>();
      r.completeEdges = r.loComplete && r.hiComplete;
      raw.push_back(r);
    }
    st.raw = mergeCaliperRuns(raw, rawSpec);
    st.kinds.reserve(count);
    st.background.reserve(count);
    st.noInfo.reserve(count);
    const auto base = stations.size() * count * 2;
    for (std::size_t k = 0; k < count; ++k) {
      const float code = trace.data[base + k * 2 + 1];
      if (!(code >= 0 && code <= static_cast<int>(CaliperSampleKind::Image)) ||
          std::floor(code) != code)
        return false;
      const auto kind = static_cast<CaliperSampleKind>(static_cast<int>(code));
      st.kinds.push_back(kind);
      st.background.push_back(kind == CaliperSampleKind::Background);
      st.noInfo.push_back(!informative(kind) && !outside(kind));
    }
    stations.push_back(std::move(st));
  }
  return true;
}

std::array<Px, 2> segment(const Station& st, double lo, double hi) {
  const P2 a = st.center + st.normal * lo, b = st.center + st.normal * hi;
  return {Px{a.x, a.y}, Px{b.x, b.y}};
}

Status compute(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext&) {
  const auto& bundle = *inputs.get("observation").asBundle();
  const Record& observed = *bundle.field("stations")->asRecord();
  const Tensor& trace = *bundle.field("trace")->asTensor();
  auto invalid = [] {
    return Status::Error(Phase::Execute, "bad_input", "彩色卡尺观测的站位或采样数据无效", {},
                         "observation");
  };
  try {
    using lyflow::std_image::inspection::kColorStationsType;
    if (observed.type != kColorStationsType)
      return invalid();
    const auto& metadata = observed.data;
    const double half = metadata.at("searchHalf").get<double>();
    const bool extent = params.choice("widthPolicy") == "extent";
    const double widthMax = extent ? 2.0 * half : params.number("mergeWidthMax");
    const double unknownGap = params.number("mergeUnknownGap");
    const double gap = std::max(params.number("mergeGap"), unknownGap);
    std::vector<Station> stations;
    if (!std::isfinite(half) || half <= 0 ||
        !readObservation(metadata, trace, half, widthMax, stations))
      return invalid();
    const int count = static_cast<int>(trace.shape[1]);
    const auto guides = unknownGap > 0 ? mergeGuides(stations, half, gap, widthMax, extent)
                                       : std::vector<ColorMergeGuide>(stations.size());
    Json result = metadata;
    result["source"] = "color";
    result["widthPolicy"] = extent ? "extent" : "range";
    result["mergeUnknownGap"] = unknownGap;
    std::vector<std::array<Px, 2>> calipers, runSegs, unknownSegs;
    for (std::size_t i = 0; i < stations.size(); ++i) {
      const auto& st = stations[i];
      auto runs = st.raw;
      std::vector<std::pair<int, int>> samples;
      samples.reserve(runs.size());
      for (Run& r : runs) {
        const auto [lo, hi] = samplesBeyond(r, half, count);
        samples.emplace_back(lo, hi);
        if (!extent)
          continue;
        if (!r.loComplete && lo >= 0 && lo < count)
          r.lo = caliperOffset(half, lo + 1);
        if (!r.hiComplete && hi >= 0 && hi < count)
          r.hi = caliperOffset(half, hi - 1);
      }
      const auto identities = mergeAcrossGaps(&runs, st.background, half, gap, widthMax, &guides[i],
                                              extent ? &st.noInfo : nullptr, extent);
      Json sections = Json::array();
      for (std::size_t j = 0; j < runs.size(); ++j) {
        Run& r = runs[j];
        const auto [first, last] = identities[j];
        const int loSample = samples[first].first, hiSample = samples[last].second;
        const bool loOpen = !r.loComplete, hiOpen = !r.hiComplete;
        if (extent)
          completeAgainstBackground(&r, st.kinds, half, guides[i].width, 0.55);
        double sum = 0;
        int n = 0, blind = 0;
        for (int k = 0; k < count; ++k) {
          const double t = caliperOffset(half, k);
          if (t < r.lo || t > r.hi)
            continue;
          if (informative(st.kinds[k])) {
            sum += trace.data[(i * static_cast<std::size_t>(count) + k) * 2];
            ++n;
          } else {
            ++blind;
          }
        }
        Json run{{"lo", r.lo},
                 {"hi", r.hi},
                 {"width", r.width()},
                 {"mid", r.mid()},
                 {"peak", r.peak},
                 {"parts", r.parts},
                 {"loComplete", r.loComplete},
                 {"hiComplete", r.hiComplete},
                 {"loCause", boundaryCause(r.loComplete, loSample, st.kinds, extent)},
                 {"hiCause", boundaryCause(r.hiComplete, hiSample, st.kinds, extent)},
                 {"meanScore", n > 0 ? exact(sum / n) : Json()},
                 {"unknownWidth", blind * kCaliperStep}};
        if (loOpen && r.loComplete && loSample >= 0 && loSample < count)
          if (auto cause = sampleCause(st.kinds[loSample], extent))
            run["loBackground"] = cause;
        if (hiOpen && r.hiComplete && hiSample >= 0 && hiSample < count)
          if (auto cause = sampleCause(st.kinds[hiSample], extent))
            run["hiBackground"] = cause;
        sections.push_back(std::move(run));
        runSegs.push_back(segment(st, r.lo, r.hi));
      }
      auto& row = result.at("stations").at(i);
      row["runs"] = std::move(sections);
      if (!extent)
        row["unknownSpans"] = unknownSpans(st.kinds, half, false);
      for (const auto& span : row.at("unknownSpans"))
        unknownSegs.push_back(
            segment(st, span.at("lo").get<double>(), span.at("hi").get<double>()));
      calipers.push_back(segment(st, -half, half));
    }
    Polyline line;
    if (!readLine(*bundle.field("line"), &line))
      return invalid();
    std::vector<Px> path;
    for (const auto& p : line.points)
      path.push_back({p.x, p.y});
    Overlay2D ov;
    if (!path.empty())
      ov.polyline("path", path, false, "胶路");
    ov.segments("station", calipers);
    ov.segments("run", runSegs);
    ov.segments("unknown", unknownSegs);
    outputs.set("runs", Data::record({"glue.ColorRuns.v1", std::move(result)}));
    outputs.set("overlay", ov.data());
    return Status::Ok();
  } catch (const Json::exception&) {
    return invalid();
  }
}

} // namespace

void registerMergeColorRuns(Registry& r) {
  r.addRetiredOperator(
      "glue.color_runs",
      "用 image.color_caliper 生成观测，再将 observation 接入 glue.merge_color_runs。");
  OperatorDesc op;
  op.id = "glue.merge_color_runs";
  op.version = "1.0.0";
  op.label = "胶色卡尺补缝";
  op.category = "涂胶/彩色";
  op.keywords = {"glue", "color", "caliper", "runs", "width", "涂胶", "彩色", "卡尺", "胶宽"};
  op.doc = "按卡尺中的明确背景、两侧边界和可信邻站的宽度与中心合并色段。extent 保留卡尺覆盖内"
           "的完整边界与不完整原因。各站的 unknownSpans 保留观测证据，供宿主判定。";
  op.inputs = {Port{"observation", "Bundle<image.ColorCaliper.v1>", "Observation",
                    "image.color_caliper 的原始观测。", true}};
  op.outputs = {
      withContract(Port{"runs", "Record", "Runs", "胶色段、边界成因和无信息区间。", true},
                   {{"recordType", "glue.ColorRuns.v1"}}),
      Port{"overlay", "Record", "Overlay", "胶路、卡尺线、合并后色段和无信息区间。", true},
  };
  const double kOpen = std::numeric_limits<double>::quiet_NaN();
  op.params = {
      rangedFloat("mergeGap", "Merge Gap", 8.0, "px", "无明确背景的短缝合并上限。", 0.0, kOpen,
                  true),
      rangedFloat("mergeUnknownGap", "Merge Unknown Gap", 16.0, "px",
                  "无明确背景的高光缝基础上限，可信邻站允许按半个参考胶宽自适应。0 关闭该自适应。",
                  0.0, kOpen, true),
      enumParam("widthPolicy", "Width Policy", "range",
                "range 采用合并宽度上限。extent 在卡尺范围内保留完整边界，仍考虑背景与邻站证据。",
                {EnumOption{"range", "宽度范围", ""}, EnumOption{"extent", "完整边界", ""}}, true),
      rangedFloat("mergeWidthMax", "Merge Width Max", 90.0, "px", "range 的最大合并宽度。", 1.0,
                  kOpen, true),
  };
  op.capabilities = {/*cancellable=*/false, /*previewable=*/false, /*deterministic=*/true};
  op.compute = &compute;
  r.addOperator(std::move(op));
}

} // namespace lyflow::packs::solution_glue
