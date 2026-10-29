// glue.fan_scan「扇形扫描」（docs/glue-measure-ops.md §2）：每个方向一条射线上的平均响应，可选前 N 个峰。
// 只测量：选哪个方向、峰够不够强由宿主判。
#include <cmath>

#include "glue.h"

namespace lyflow::packs::glue {
namespace {

FanSpec specOf(const ParamView& params) {
  FanSpec spec;
  const auto anchor = params.vec2("anchor");
  spec.anchor = P2(anchor[0], anchor[1]);
  const auto sector = params.vec2("sector");
  spec.from = sector[0];
  spec.to = sector[1];
  const auto radial = params.vec2("radial");
  spec.r0 = radial[0];
  spec.r1 = radial[1];
  spec.angleStep = params.number("angleStep");
  spec.radialStep = params.number("radialStep");
  spec.invalidMax = params.number("invalidMax");
  spec.peakCount = static_cast<int>(params.integer("peakCount"));
  spec.peakSeparation = params.number("peakSeparation");
  spec.includeInvalid = params.flag("includeInvalid");
  return spec;
}

std::vector<Px> arc(P2 center, double r, double a0, double a1) {
  std::vector<Px> pts;
  const int n = std::max(2, static_cast<int>(std::ceil((a1 - a0) / 2.0)) + 1);
  for (int i = 0; i < n; ++i) {
    const P2 p = center + dirOfDeg(a0 + (a1 - a0) * i / (n - 1)) * r;
    pts.push_back(Px{p.x, p.y});
  }
  return pts;
}

Status compute(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext&) {
  MapsView maps;
  if (!readMaps(inputs.get("maps"), &maps))
    return badImage("maps", true);
  const Field& R = maps.response;
  const FanSpec spec = specOf(params);
  const FanResult fan = fanScan(R, spec);

  Json rec;
  rec["anchor"] = exactPx(spec.anchor);
  rec["sector"] = Json::array({spec.from, spec.to});
  rec["radial"] = Json::array({spec.r0, spec.r1});
  rec["angleStep"] = spec.angleStep;
  rec["radialStep"] = spec.radialStep;
  rec["samples"] = fan.samples;
  Json angles = Json::array(), scores = Json::array(), invalid = Json::array();
  for (std::size_t i = 0; i < fan.angles.size(); ++i) {
    angles.push_back(exact(fan.angles[i]));
    scores.push_back(exact(fan.scores[i]));
    invalid.push_back(fan.invalid[i]);
  }
  rec["angles"] = std::move(angles);
  rec["scores"] = std::move(scores);
  rec["invalid"] = std::move(invalid);
  Json peaks = Json::array();
  for (const FanPeak& p : fan.peaks) {
    peaks.push_back(Json{{"index", p.index}, {"angle", exact(p.angle)}, {"score", exact(p.score)}});
  }
  rec["peaks"] = std::move(peaks);
  Record out;
  out.type = kFanScanType;
  out.data = std::move(rec);
  outputs.set("scan", Data::record(std::move(out)));

  Overlay2D ov;
  const Px a{spec.anchor.x, spec.anchor.y};
  ov.circle("anchor", a, 6.0, "锚点");
  const bool fullCircle = spec.to - spec.from >= 360.0 - 1e-6;
  if (!fullCircle) {
    const P2 e0 = spec.anchor + dirOfDeg(spec.from) * spec.r1;
    const P2 e1 = spec.anchor + dirOfDeg(spec.to) * spec.r1;
    ov.segments("sector", {{a, Px{e0.x, e0.y}}, {a, Px{e1.x, e1.y}}});
  }
  ov.polyline("radial", arc(spec.anchor, spec.r0, spec.from, spec.to), fullCircle,
              "r = " + fmt(spec.r0, 0));
  ov.polyline("radial", arc(spec.anchor, spec.r1, spec.from, spec.to), fullCircle,
              "r = " + fmt(spec.r1, 0));
  for (std::size_t k = 0; k < fan.peaks.size(); ++k) {
    const P2 e = spec.anchor + dirOfDeg(fan.peaks[k].angle) * spec.r1;
    ov.polyline("peak", {a, Px{e.x, e.y}}, false,
                "#" + std::to_string(k) + " " + fmt(fan.peaks[k].angle) + "° " +
                    fmt(fan.peaks[k].score));
  }
  outputs.set("overlay", ov.data());
  return Status::Ok();
}

std::vector<Issue> validate(const ParamView& params, const std::set<std::string>&) {
  std::vector<Issue> issues;
  const auto sector = params.vec2("sector");
  if (!(sector[1] > sector[0]) || sector[1] - sector[0] > 360.0 + 1e-6) {
    issues.push_back(Issue::error("bad_param",
                                  "sector 要从小到大、跨度不超过 360°（现在是 [" +
                                      fmt(sector[0], 0) + ", " + fmt(sector[1], 0) + "]）",
                                  "sector"));
  }
  const auto radial = params.vec2("radial");
  if (!(radial[0] >= 0) || !(radial[1] > radial[0])) {
    issues.push_back(Issue::error("bad_param",
                                  "radial 要满足 0 ≤ r0 < r1（现在是 [" + fmt(radial[0], 0) + ", " +
                                      fmt(radial[1], 0) + "]）",
                                  "radial"));
  }
  return issues;
}

} // namespace

void registerFanScan(Registry& r) {
  OperatorDesc op;
  op.id = "glue.fan_scan";
  op.version = "1.0.0";
  op.label = "扇形扫描";
  op.category = "涂胶/测量";
  op.keywords = {"glue", "fan", "sector", "heading", "涂胶", "扇形", "方向", "测量"};
  op.doc =
      "从锚点出发，在扇区里每隔 angleStep 一条射线，射线在径向区间 radial 上每 radialStep 采一次响应，"
      "得分 = 有效采样之和 / 总采样数；无效采样占比 ≥ invalidMax 的方向得分为 null。\n"
      "峰：有得分的方向按得分从高到低，依次取与已取的峰角差都 > peakSeparation 的，取满 peakCount 个"
      "（与 1.1 bead_path 的取法相同；includeInvalid 时无效方向排在最后也可当峰）。只测量，不挑方向。输出不舍入。";
  op.inputs = {Port{"maps", "Bundle<glue.ResponseMaps>", "Maps",
                    "glue.response 的 maps（用 response；块外的采样算无效）。", true}};
  op.outputs = {
      Port{
          "scan", "Record", "Scan",
          "glue.FanScan：anchor、sector、radial、angleStep、radialStep、samples、angles[]、scores[]、"
          "invalid[]、peaks[{index, angle, score}]。",
          true},
      Port{"overlay", "Record", "Overlay", "lyflow.overlay2d：锚点、扇区、径向区间、峰方向。",
           true},
  };
  Param anchor = vec2Param("anchor", "Anchor", 640.0, 512.0, "px", {"X", "Y"}, "锚点（出胶点）。");
  anchor.tuningRole = "geometry";
  Param sector = vec2Param("sector", "Sector", -180.0, 180.0, "deg", {"From", "To"},
                           "扫描的方向范围（度，图像坐标：0° 朝右、−90° 朝上）。");
  sector.tuningRole = "geometry";
  Param radial = vec2Param("radial", "Radial", 100.0, 250.0, "px", {"R0", "R1"},
                           "射线上采样的径向区间：t = r0, r0 + radialStep, …（t < r1）。");
  radial.min = 0.0;
  radial.tuningRole = "geometry";
  Param angleStep =
      rangedFloat("angleStep", "Angle Step", 1.0, "deg", "方向步长。", 0.1, 30.0, true);
  Param radialStep =
      rangedFloat("radialStep", "Radial Step", 2.0, "px", "射线上的采样步长。", 0.5, 50.0, true);
  Param invalidMax =
      rangedFloat("invalidMax", "Invalid Max", 0.3, "",
                  "一条射线上无效采样占比 ≥ 它时这个方向的得分为 null。", 0.0, 1.0, true);
  Param peakCount =
      intParam("peakCount", "Peak Count", 3, "输出前几个峰；0 = 不输出峰。", 0, 20, true);
  Param peakSeparation = rangedFloat("peakSeparation", "Peak Separation", 15.0, "deg",
                                     "峰之间的角差要严格大于它。", 0.0, 180.0, true);
  Param includeInvalid = boolParam(
      "includeInvalid", "Include Invalid", false,
      "有效方向凑不够 peakCount 个峰时，得分为 null 的方向（按角度顺序）也拿来补，这种峰的 score 是 null。"
      "1.1 bead_path 是这样挑的：要与 1.1 等价就打开。",
      true);
  op.params = {anchor,     sector,    radial,         angleStep,     radialStep,
               invalidMax, peakCount, peakSeparation, includeInvalid};
  op.capabilities = {/*cancellable=*/false, /*previewable=*/false, /*deterministic=*/true};
  op.compute = &compute;
  op.validate = &validate;
  r.addOperator(std::move(op));
}

} // namespace lyflow::packs::glue
