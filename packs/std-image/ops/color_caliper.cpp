#include "lyflow_cv/color_caliper.h"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace lyflow::std_image::inspection {

using line_scan::kCaliperStep;
using line_scan::RunSplitSpec;

CaliperSampleKind scoreKind(double score, double segmentMin) {
  if (score < 0)
    return CaliperSampleKind::Unknown;
  if (score < 0.5 * segmentMin)
    return CaliperSampleKind::Background;
  return score < segmentMin ? CaliperSampleKind::Weak : CaliperSampleKind::Signal;
}

bool informative(CaliperSampleKind kind) { return kind <= CaliperSampleKind::Signal; }

bool outside(CaliperSampleKind kind) { return kind >= CaliperSampleKind::Outside; }

const char* sampleCause(CaliperSampleKind kind, bool distinguishOutside) {
  switch (kind) {
  case CaliperSampleKind::Highlight:
    return "highlight";
  case CaliperSampleKind::Dark:
    return "dark";
  case CaliperSampleKind::Outside:
    return "outside";
  case CaliperSampleKind::Roi:
    return distinguishOutside ? "roi" : "outside";
  case CaliperSampleKind::Image:
    return distinguishOutside ? "image" : "outside";
  case CaliperSampleKind::Unknown:
    return "unknown";
  default:
    return nullptr;
  }
}

double caliperOffset(double half, int sample) { return -half + sample * kCaliperStep; }

std::pair<int, int> samplesBeyond(const line_scan::Run& run, double half, int count) {
  const double lo = std::ceil((run.lo + half) / kCaliperStep - 1e-6) - 1;
  const double hi = std::floor((run.hi + half) / kCaliperStep + 1e-6) + 1;
  return {static_cast<int>(std::clamp(lo, -1.0, double(count))),
          static_cast<int>(std::clamp(hi, -1.0, double(count)))};
}

const char* boundaryCause(bool complete, int sample, const std::vector<CaliperSampleKind>& kinds,
                          bool distinguishOutside) {
  if (complete)
    return "edge";
  if (sample < 0 || sample >= static_cast<int>(kinds.size()))
    return "caliper";
  const auto* cause = sampleCause(kinds[static_cast<std::size_t>(sample)], distinguishOutside);
  return cause ? cause : "edge";
}

Json unknownSpans(const std::vector<CaliperSampleKind>& kinds, double half,
                  bool distinguishOutside) {
  Json spans = Json::array();
  const int count = static_cast<int>(kinds.size());
  for (int i = 0; i < count;) {
    const auto* cause = sampleCause(kinds[static_cast<std::size_t>(i)], distinguishOutside);
    if (!cause) {
      ++i;
      continue;
    }
    int j = i;
    while (j + 1 < count) {
      const auto* next = sampleCause(kinds[static_cast<std::size_t>(j + 1)], distinguishOutside);
      if (!next || std::string_view(next) != cause)
        break;
      ++j;
    }
    spans.push_back(
        {{"lo", caliperOffset(half, i)}, {"hi", caliperOffset(half, j)}, {"cause", cause}});
    i = j + 1;
  }
  return spans;
}

namespace {

CaliperSampleKind unobserved(const ColorMapsView& maps, P2 p) {
  if (!maps.score.containsFootprint(p)) {
    if (maps.image.width <= 0 || maps.image.height <= 0)
      return CaliperSampleKind::Outside;
    return p.x < 0 || p.y < 0 || p.x >= maps.image.width - 1 || p.y >= maps.image.height - 1
               ? CaliperSampleKind::Image
               : CaliperSampleKind::Roi;
  }
  const int x = static_cast<int>(std::floor(p.x)) - maps.frame.x;
  const int y = static_cast<int>(std::floor(p.y)) - maps.frame.y;
  auto kind = CaliperSampleKind::Unknown;
  for (int dy = 0; dy < 2; ++dy)
    for (int dx = 0; dx < 2; ++dx) {
      const auto state = maps.state.at<float>(y + dy, x + dx);
      if (state == static_cast<int>(color::ColorState::Highlight))
        return CaliperSampleKind::Highlight;
      if (state == static_cast<int>(color::ColorState::Dark))
        kind = CaliperSampleKind::Dark;
    }
  return kind;
}

Status compute(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext&) {
  ColorMapsView maps;
  if (!readColorMaps(inputs.get("maps"), &maps))
    return Status::Error(Phase::Execute, "bad_input", "maps 需要带帧与状态的颜色得分图", {},
                         "maps");
  Polyline line;
  if (!readLine(inputs.get("line"), &line))
    return Status::Error(Phase::Execute, "bad_input", "line 需要图像折线", {}, "line");

  RunSplitSpec spec;
  spec.halfMax = params.choice("split") == "half_max";
  spec.segmentMin = params.number("segmentMin");
  spec.peakMin = params.number("peakMin");
  const double half = params.number("searchHalf");
  const double step = params.number("stationStep");
  const auto range = params.vec2("sRange");
  double a = 0, b = 0;
  const auto poses = layStations(line, step, range[0], range[1], &a, &b);
  const int count = static_cast<int>(std::floor(2.0 * half / kCaliperStep + 1e-9)) + 1;
  Tensor trace;
  trace.shape = {static_cast<std::int64_t>(poses.size()), count, 2};
  trace.data.reserve(trace.elementCount());
  Json stations = Json::array();
  std::vector<CaliperSampleKind> kinds(static_cast<std::size_t>(count));
  for (const auto& st : poses) {
    const auto scores = line_scan::sampleCaliper(maps.score, st.c, st.n, half);
    int inFrame = 0, valid = 0;
    for (int i = 0; i < count; ++i) {
      const double score = scores[static_cast<std::size_t>(i)];
      auto kind = scoreKind(score, spec.segmentMin);
      if (kind == CaliperSampleKind::Unknown)
        kind = unobserved(maps, st.c + st.n * caliperOffset(half, i));
      kinds[static_cast<std::size_t>(i)] = kind;
      valid += informative(kind);
      inFrame += !outside(kind);
      trace.data.push_back(static_cast<float>(score));
      trace.data.push_back(static_cast<float>(kind));
    }
    Json runs = Json::array();
    for (const auto& run : line_scan::rawCaliperRuns(scores, half, spec)) {
      const auto [lo, hi] = samplesBeyond(run, half, count);
      runs.push_back({{"lo", run.lo},
                      {"hi", run.hi},
                      {"peak", run.peak},
                      {"loComplete", run.loComplete},
                      {"hiComplete", run.hiComplete},
                      {"loCause", boundaryCause(run.loComplete, lo, kinds, true)},
                      {"hiCause", boundaryCause(run.hiComplete, hi, kinds, true)}});
    }
    stations.push_back({{"s", st.s},
                        {"center", exactPx(st.c)},
                        {"normal", exactPx(st.n)},
                        {"validFraction", double(inFrame) / count},
                        {"infoFraction", double(valid) / count},
                        {"unknownSpans", unknownSpans(kinds, half, true)},
                        {"runs", std::move(runs)}});
  }
  Record record{kColorStationsType,
                {{"count", poses.size()},
                 {"stationStep", step},
                 {"sRange", poses.empty() ? Json() : Json::array({a, b})},
                 {"searchHalf", half},
                 {"split", spec.halfMax ? "half_max" : "connected"},
                 {"stations", std::move(stations)}}};
  Bundle observation(kColorCaliperKind);
  observation.set("stations", Data::record(std::move(record)));
  observation.set("trace", Data::tensor(std::move(trace)));
  observation.set("line", inputs.get("line"));
  outputs.set("observation", Data::bundle(std::move(observation)));
  return Status::Ok();
}
} // namespace

void registerColorCaliper(Registry& registry) {
  registry.addBundle(
      {kColorCaliperKind,
       "彩色卡尺观测",
       "按折线布站的颜色得分、区段与可见性。",
       "",
       {{"stations", "Record", "image.ColorCaliperStations.v1：原始段、边界和无信息区间。"},
        {"trace", "Tensor", "[站,采样,2]。每个采样给出得分和分类码，间距为 0.5 px。"},
        {"line", "Record", "布站时使用的原折线。"}}});
  OperatorDesc op;
  op.id = "image.color_caliper";
  op.version = "1.0.0";
  op.label = "彩色卡尺观测";
  op.category = "图像/测量";
  op.keywords = {"color", "caliper", "observation", "彩色", "卡尺"};
  op.doc = "沿图像折线的站位法向采样颜色得分。给出原始色段的两条边、边界原因与无信息区间。"
           "trace 的采样分类依次是 0 明确背景、1 弱得分、2 达分段门限、3 未分类无信息、"
           "4 高光、5 暗区、6 未区分的外部、7 ROI 外、8 图像外。";
  op.inputs = {
      Port{"maps", "Bundle<glue.ColorMaps>", "Maps", "颜色得分与观测状态图。", true},
      withContract(Port{"line", "Record", "Line", "图像像素折线。", true},
                   {{"recordType", kPolylineType}}),
  };
  op.outputs = {Port{"observation", "Bundle<image.ColorCaliper.v1>", "Observation",
                     "原始区段与采样观测。", true}};
  auto sRange = vec2Param("sRange", "S Range", 0.0, 0.0, "px", {"From", "To"},
                          "布站范围，[0, 0] 为整条折线。");
  sRange.tuningRole = "geometry";
  op.params = {
      rangedFloat("stationStep", "Station Step", 4.0, "px", "站距。", 0.5, 200.0),
      std::move(sRange),
      rangedFloat("searchHalf", "Search Half", 60.0, "px", "卡尺半长。", 1.0, 600.0),
      enumParam("split", "Split", "half_max", "选择半高拆分或保留整个连通段。",
                {EnumOption{"half_max", "半高拆分", ""}, EnumOption{"connected", "连通段", ""}}),
      rangedFloat("segmentMin", "Segment Min", 0.3, "", "分段得分门限。", 0.0, 1.0),
      rangedFloat("peakMin", "Peak Min", 0.6, "", "段的得分峰值下限。", 0.0, 1.0),
  };
  op.capabilities = {/*cancellable=*/false, /*previewable=*/false, /*deterministic=*/true};
  op.compute = &compute;
  registry.addOperator(std::move(op));
}

} // namespace lyflow::std_image::inspection
