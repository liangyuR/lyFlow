// glue.part_edges「找零件边」（docs/glue-measure-ops.md §5）：沿胶路布站，每站两侧的全部合格零件边跳变。
// 不投票选侧、不剔野、不算距离 —— 那些归宿主。
#include <cmath>

#include "algo/edge.h"
#include "glue.h"

namespace lyflow::packs::glue {
namespace {

Status compute(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext&) {
  const cv::Mat gray = grayOf(inputs.get("image"));
  if (gray.empty())
    return badImage("image", false);
  Polyline line;
  if (!readLine(inputs.get("line"), &line))
    return Status::Error(Phase::Execute, "bad_input", "line 不是 glue.Polyline", {}, "line");

  EdgeScanSpec spec;
  spec.contrastMin = params.number("contrastMin");
  spec.farRun = params.number("farRun");
  spec.farDropRatio = params.number("farDropRatio");
  spec.descentRatio = params.number("descentRatio");
  spec.darkToBright = params.choice("polarity") == "dark_to_bright";
  const std::string sides = params.choice("sides");
  const bool right = sides != "left", left = sides != "right";
  const double start = params.number("start");
  const double length = params.number("searchLength");
  const double step = params.number("stationStep");
  const auto range = params.vec2("sRange");
  double a = 0, b = 0;
  const std::vector<StationPose> poses = layStations(line, step, range[0], range[1], &a, &b);

  // 灰度原样采样（不做二维平滑），剖面上再做一维平滑；只转换站点够得着的那一块
  Field field;
  if (!poses.empty()) {
    double x0 = std::numeric_limits<double>::infinity(), y0 = x0, x1 = -x0, y1 = -x0;
    for (const StationPose& st : poses) {
      x0 = std::min(x0, st.c.x);
      y0 = std::min(y0, st.c.y);
      x1 = std::max(x1, st.c.x);
      y1 = std::max(y1, st.c.y);
    }
    const double reach = start + length + 4.0;
    const cv::Rect roi(static_cast<int>(std::floor(x0 - reach)),
                       static_cast<int>(std::floor(y0 - reach)),
                       static_cast<int>(std::ceil(x1 - x0 + 2 * reach)) + 1,
                       static_cast<int>(std::ceil(y1 - y0 + 2 * reach)) + 1);
    const cv::Rect inner = roi & cv::Rect(0, 0, gray.cols, gray.rows);
    if (!inner.empty()) {
      cv::Mat g;
      gray(inner).convertTo(g, CV_32F);
      field = Field(g, inner.tl(), inner);
    }
  }

  Overlay2D ov;
  std::vector<Px> parts;
  Json stations = Json::array();
  for (const StationPose& st : poses) {
    const auto side = [&](P2 dir) {
      int samples = 0;
      const std::vector<EdgeHit> hits =
          findPartEdges(field, st.c, dir, start, length, spec, false, &samples);
      Json edges = Json::array();
      for (const EdgeHit& h : hits) {
        const P2 q = st.c + dir * h.t;
        edges.push_back(Json{{"t", exact(h.t)},
                             {"point", exactPx(q)},
                             {"contrast", exact(h.contrast)},
                             {"farMargin", exact(h.farMargin)},
                             {"descent", exact(h.descent)}});
        parts.push_back(Px{q.x, q.y});
      }
      return Json{{"length", samples > 0 ? samples - 1 : 0}, {"edges", std::move(edges)}};
    };
    stations.push_back(Json{{"s", exact(st.s)},
                            {"center", exactPx(st.c)},
                            {"normal", exactPx(st.n)},
                            {"right", right ? side(st.n) : Json()},
                            {"left", left ? side(-st.n) : Json()}});
  }

  Json rec;
  rec["count"] = poses.size();
  rec["stationStep"] = step;
  rec["sRange"] = poses.empty() ? Json() : Json::array({exact(a), exact(b)});
  rec["sides"] = sides;
  rec["start"] = start;
  rec["searchLength"] = length;
  rec["polarity"] = spec.darkToBright ? "dark_to_bright" : "bright_to_dark";
  rec["stations"] = std::move(stations);
  Record out;
  out.type = kPartEdgesType;
  out.data = std::move(rec);
  outputs.set("edges", Data::record(std::move(out)));

  std::vector<Px> path;
  for (const P2& p : line.points)
    path.push_back(Px{p.x, p.y});
  if (!path.empty())
    ov.polyline("path", path, false, "胶路");
  ov.points("part", parts);
  outputs.set("overlay", ov.data());
  return Status::Ok();
}

std::vector<Issue> validate(const ParamView& params, const std::set<std::string>&) {
  std::vector<Issue> issues;
  if (params.number("searchLength") < params.number("farRun") + 10.0) {
    issues.push_back(
        Issue::error("bad_param", "searchLength 要比 farRun 至少长 10 px", "searchLength"));
  }
  return issues;
}

} // namespace

void registerPartEdges(Registry& r) {
  OperatorDesc op;
  op.id = "glue.part_edges";
  op.version = "1.0.0";
  op.label = "找零件边";
  op.category = "涂胶/测量";
  op.keywords = {"glue", "edge", "part", "margin", "涂胶", "零件边", "边距", "测量"};
  op.doc =
      "沿胶路每 stationStep 一站，向两侧沿法向从 start 起每 1 px 取原始灰度剖面（出图截断），7 抽头 σ = 1.5 的一维高斯后，"
      "找全部合格跳变：极性对（默认从胶往外 亮 → 暗）、下降率是局部最大且 > descentRatio × contrastMin、"
      "近侧 − 远侧均值 ≥ contrastMin、近侧 − 远侧最大值 ≥ farDropRatio × contrastMin（远侧持续 farRun px 暗下去）。"
      "位置按三点抛物线细化。\n与 1.1 edge_distance 的 findPartEdge 同一套判据，只是不在第一个处停下；"
      "选侧、剔野、算距离由宿主做。";
  op.inputs = {
      Port{"image", "Image", "Image", "与 glue.response 同一帧（原始灰度）。", true},
      withContract(Port{"line", "Record", "Line", "glue.Polyline：胶路。", true},
                   {{"recordType", kPolylineType}}),
  };
  op.outputs = {
      Port{
          "edges", "Record", "Edges",
          "glue.PartEdges：count、stationStep、sRange、sides、start、searchLength、polarity、"
          "stations[{s, center, normal, right / left: {length, edges[{t, point, contrast, farMargin, "
          "descent}]}}]。",
          true},
      Port{"overlay", "Record", "Overlay", "lyflow.overlay2d：胶路与每个跳变点。", true},
  };
  Param sRange = vec2Param("sRange", "S Range", 0.0, 0.0, "px", {"From", "To"},
                           "布站范围；[0, 0] = 整条胶路，否则与胶路求交。");
  sRange.tuningRole = "geometry";
  op.params = {
      rangedFloat("stationStep", "Station Step", 4.0, "px", "站距。", 0.5, 200.0),
      sRange,
      enumParam("sides", "Sides", "both", "找哪一侧（right = 法向那一侧）。",
                {EnumOption{"both", "两侧", ""}, EnumOption{"right", "右侧", ""},
                 EnumOption{"left", "左侧", ""}}),
      rangedFloat("start", "Start", 0.0, "px", "剖面从离胶路多远开始。", 0.0,
                  std::numeric_limits<double>::quiet_NaN()),
      rangedFloat("searchLength", "Search Length", 200.0, "px", "剖面长度（从 start 起）。", 20.0,
                  2000.0),
      enumParam("polarity", "Polarity", "bright_to_dark",
                "从胶往外看零件边是 亮 → 暗 还是 暗 → 亮。",
                {EnumOption{"bright_to_dark", "亮 → 暗", ""},
                 EnumOption{"dark_to_bright", "暗 → 亮", ""}}),
      rangedFloat("contrastMin", "Contrast Min", 40.0, "", "近侧均值 − 远侧均值至少这么大。", 1.0,
                  std::numeric_limits<double>::quiet_NaN()),
      rangedFloat("farRun", "Far Run", 12.0, "px", "远侧要持续这么长。", 1.0, 200.0, true),
      rangedFloat("farDropRatio", "Far Drop Ratio", 0.5, "",
                  "近侧均值 − 远侧最大值 ≥ 它 × contrastMin。", 0.0, 1.0, true),
      rangedFloat("descentRatio", "Descent Ratio", 0.05, "", "候选点的下降率 > 它 × contrastMin。",
                  0.0, 1.0, true),
  };
  op.capabilities = {/*cancellable=*/false, /*previewable=*/false, /*deterministic=*/true};
  op.compute = &compute;
  op.validate = &validate;
  r.addOperator(std::move(op));
}

} // namespace lyflow::packs::glue
