// glue.bead_width「量胶宽」（glue-plan §2.3，D1 / D4 / D5 / D7 / D10 / D16）。
#include <algorithm>
#include <cmath>
#include <limits>

#include "glue.h"

namespace lyflow::packs::glue {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

StationSpec specOf(const ParamView& params) {
  StationSpec spec;
  spec.swirl = params.choice("form") == "swirl";
  spec.step = params.number("stationStep");
  spec.searchHalf = params.number("searchHalf");
  spec.window = params.number("window");
  spec.presentRatio = params.number("presentRatio");
  spec.centerRatio = params.number("centerRatio");
  spec.contrastRatio = params.number("contrastRatio");
  const auto range = params.vec2("widthRange");
  spec.runs.widthMin = range[0];
  spec.runs.widthMax = range[1];
  spec.runs.mergeGap = params.number("mergeGap");
  spec.runs.contrastMin = params.number("contrastMin");
  return spec;
}

std::vector<Px> pxList(const std::vector<P2>& pts) {
  std::vector<Px> out;
  out.reserve(pts.size());
  for (const P2& p : pts) out.push_back(Px{p.x, p.y});
  return out;
}

Px px(P2 p) { return Px{p.x, p.y}; }

Status compute(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext&) {
  const cv::Mat gray = grayOf(inputs.get("image"));
  if (gray.empty()) {
    return Status::Error(Phase::Execute, "bad_input", "需要全分辨率非空 u8 图像；其他位深先接 image.normalize，缩小预览请改用完整运行", {}, "image");
  }
  Polyline line;
  Json pathInfo;
  if (!readPath(inputs.get("path"), &line, &pathInfo)) {
    return Status::Error(Phase::Execute, "bad_input", "path 不是完整的 glue.Path", {}, "path");
  }
  Metric metric;
  if (Status s = metricFromInput(inputs, "calib", &metric); !s.ok) return s;

  const StationSpec spec = specOf(params);
  const bool pathOk = pathInfo.value("ok", false);
  const bool bright = pathInfo.value("polarity", std::string("dark")) == "bright";
  const int widthMax = pathInfo.value("widthMax", 90);
  double zoneStart = line.s.front();
  double zoneEnd = line.s.back();
  if (const auto z = pathInfo.find("zone"); z != pathInfo.end() && z->is_array() && z->size() == 2) {
    zoneStart = numOf((*z)[0]);
    zoneEnd = numOf((*z)[1]);
  }
  const StationResult res =
      measureStations(gray, line.view(), pathOk, zoneStart, zoneEnd, bright, widthMax, spec);
  const std::vector<Station>& st = res.stations;

  std::vector<double> widths;
  for (const Station& x : st) {
    if (!x.present) continue;
    const double w = metric.distance(x.left(), x.right());
    if (std::isfinite(w)) widths.push_back(w);
  }
  const double coverage = st.empty() ? 0.0
                                     : static_cast<double>(std::count_if(st.begin(), st.end(),
                                                                         [](const Station& x) { return x.present; })) /
                                           static_cast<double>(st.size());
  double mean = kNaN, lo = kNaN, hi = kNaN;
  if (!widths.empty()) {
    double sum = 0;
    lo = widths.front();
    hi = widths.front();
    for (double w : widths) {
      sum += w;
      lo = std::min(lo, w);
      hi = std::max(hi, w);
    }
    mean = sum / static_cast<double>(widths.size());
  }

  const std::string form = spec.swirl ? "swirl" : "straight";
  Json info;
  info["form"] = form;
  info["stationStep"] = spec.step;
  info["unit"] = metric.unit();
  info["pathOk"] = pathOk;
  info["message"] = pathOk ? (widths.empty() ? "没有一站量到胶" : "") : kNoBeadMessage;
  info["zone"] = Json::array({zoneStart, zoneEnd});
  info["stations"] = st.size();
  info["present"] = widths.size();
  info["coverage"] = numOrNull(coverage, 4);
  info["wRef"] = numOrNull(res.wRef, 3);
  info["peakRef"] = numOrNull(res.peakRef, 1);
  info["envelopeHalf"] = spec.swirl ? numOrNull(res.envelopeHalf, 3) : Json();
  info["searchHalf"] = spec.searchHalf;
  info["polarity"] = bright ? "bright" : "dark";
  info["widthMax"] = widthMax;
  info["calib"] = metric.calib ? std_image::planeCalibJson(*metric.calib) : Json();
  Json stations = stationsJson(st, spec.step, form, metric);
  outputs.set("bead", beadBundle(polylineRecord(line.s, line.points, line.tangents),
                                 std::move(stations), std::move(info)));
  const std::string why = pathOk ? "没有一站量到胶" : kNoBeadMessage;
  outputs.set("widthMean", measurement(mean, metric.unit(), why));
  outputs.set("widthMin", measurement(lo, metric.unit(), why));
  outputs.set("widthMax", measurement(hi, metric.unit(), why));
  lyflow::Measurement cov;
  cov.value = coverage;
  cov.ok = true;
  cov.unit = "";
  cov.message = pathOk ? "" : kNoBeadMessage;
  outputs.set("coverage", Data::measurement(std::move(cov)));

  // 叠画：胶路、有胶的站（两边连线）、无胶的站、两条胶边
  Overlay2D ov;
  ov.polyline(pathOk ? "path" : "missing", pxList(line.points));
  std::vector<std::array<Px, 2>> present, missing;
  const double halfMark = std::max(6.0, 0.5 * res.wRef);
  for (const Station& x : st) {
    if (x.present) {
      present.push_back({px(x.left()), px(x.right())});
    } else {
      missing.push_back({px(x.c - x.n * halfMark), px(x.c + x.n * halfMark)});
    }
  }
  ov.segments("station", present);
  ov.segments("missing", missing, missing.empty() ? "" : "无胶 " + std::to_string(missing.size()) + " 站");
  for (const auto& [a, b] : presentRuns(st)) {
    std::vector<Px> left, right;
    for (std::size_t i = a; i <= b; ++i) {
      left.push_back(px(st[i].left()));
      right.push_back(px(st[i].right()));
    }
    ov.polyline("edge.left", left);
    ov.polyline("edge.right", right);
  }
  outputs.set("overlay", ov.data());
  return Status::Ok();
}

std::vector<Issue> validate(const ParamView& params, const std::set<std::string>&) {
  std::vector<Issue> issues;
  const auto range = params.vec2("widthRange");
  if (!(range[0] >= 0) || !(range[1] > range[0])) {
    issues.push_back(Issue::error("bad_param",
                                  "widthRange 要满足 0 ≤ 下限 < 上限（现在是 [" + fmt(range[0]) + ", " +
                                      fmt(range[1]) + "]）",
                                  "widthRange"));
  }
  const double half = params.number("searchHalf");
  if (half < 0.5 * range[1]) {
    issues.push_back(Issue::warning("bad_param",
                                    "searchHalf 比 widthRange 上限的一半还小：最宽的胶在卡尺里放不下",
                                    "searchHalf"));
  }
  if (params.choice("form") == "swirl" && params.number("window") < params.number("stationStep")) {
    issues.push_back(Issue::warning("bad_param", "window 比 stationStep 还短：外包络只取到本站自己",
                                    "window"));
  }
  return issues;
}

}  // namespace

void registerBeadWidth(Registry& r) {
  OperatorDesc op;
  op.id = "glue.bead_width";
  op.version = "1.0.0";
  op.label = "量胶宽";
  op.category = "涂胶/积木";
  op.keywords = {"glue", "bead", "width", "涂胶", "胶宽", "螺旋胶", "积木"};
  op.doc =
      "沿胶路每隔 stationStep 布一站，站上的卡尺沿法向（行进方向右侧为正）量胶的两边：边取响应的半高处"
      "（每站按本站峰值自适应），暗段之间的缝小于 mergeGap 且合并后不宽于 widthRange 上限就合并（高光条不会"
      "把一条胶劈成两条）。胶宽 = 两边之间的距离，没接 calib 是 px，接了 image.PlaneCalib 按映射后两点的距离出 mm。\n"
      "有无胶逐站判（D16）：参考宽 / 参考峰值 = 全检测区候选暗段宽度 / 峰值的中位数；直胶要宽 ≥ presentRatio × 参考宽、"
      "中心偏离胶路 ≤ max(6, centerRatio × 参考宽)、峰值 ≥ contrastRatio × 参考峰值；螺旋胶（form = swirl）本站自己要有"
      "一个够宽、够浓、落在包络内的暗段，两边取 ±window/2 内所有暗段的并集（外包络，D5）。"
      "胶路没找到（path.info.ok = false）时全部判无胶。";
  op.inputs = {
      Port{"image", "Image", "Image", "与 bead_path 同一帧。", true},
      Port{"path", "Bundle<glue.Path>", "Path", "glue.bead_path 的胶路。", true},
      withContract(Port{"calib", "Record", "Calib",
                        "可选：image.PlaneCalib（图像 px → 工作平面 mm 的单应）。接了宽度出 mm。", false},
                   {{"recordType", std_image::kPlaneCalibType}}),
  };
  op.outputs = {
      Port{"bead", "Bundle<glue.Bead>", "Bead", "逐站的胶（line + stations + info）。", true},
      Port{"widthMean", "Measurement", "Width Mean", "有胶各站宽度的均值。", true},
      Port{"widthMin", "Measurement", "Width Min", "有胶各站宽度的最小值。", true},
      Port{"widthMax", "Measurement", "Width Max", "有胶各站宽度的最大值。", true},
      Port{"coverage", "Measurement", "Coverage", "有胶的站占全部站的比例（0–1）。", true},
      Port{"overlay", "Record", "Overlay", "lyflow.overlay2d：每站两边（有胶 / 无胶两种 role）与两条胶边。", true},
  };
  Param form = enumParam("form", "Form", "straight", "直胶还是螺旋胶。螺旋胶的宽度取外包络。",
                         {EnumOption{"straight", "直胶", ""}, EnumOption{"swirl", "螺旋胶", "取外包络。"}});
  Param range = vec2Param("widthRange", "Width Range", 4.0, 90.0, "px", {"Min", "Max"},
                          "认作胶的暗段宽度范围。上限别超过 bead_path 的 widthMax（结构元比胶窄，胶就没有响应）。");
  Param step = floatParam("stationStep", "Station Step", 4.0, "px", "相邻两站沿胶路的间隔。", true);
  step.min = 1.0;
  Param searchHalf = floatParam("searchHalf", "Search Half", 60.0, "px", "卡尺沿法向往两侧各伸多长。", true);
  searchHalf.min = 2.0;
  Param mergeGap = floatParam("mergeGap", "Merge Gap", 8.0, "px",
                              "两段暗段之间的缝不超过它就合并（粗胶中间的高光条）。", true);
  mergeGap.min = 0.0;
  Param window = visibleWhen(floatParam("window", "Window", 30.0, "px",
                                        "螺旋胶取外包络的窗口长度（沿胶路，本站两侧各一半）。约一个螺距。", true),
                             "form", "swirl");
  window.min = 0.0;
  Param contrastMin = floatParam("contrastMin", "Contrast Min", 16.0, "",
                                 "卡尺上一段响应的峰值至少这么高才算胶（灰度级）。", true);
  contrastMin.min = 1.0;
  Param presentRatio = floatParam("presentRatio", "Present Ratio", 0.5, "",
                                  "D16：一站的胶至少要有参考宽的这么多倍宽才算有胶。", true);
  presentRatio.min = 0.0;
  presentRatio.max = 1.0;
  Param centerRatio = floatParam("centerRatio", "Center Ratio", 0.3, "",
                                 "D16（直胶）：胶的中心偏离胶路不超过 max(6 px, 它 × 参考宽)。", true);
  centerRatio.min = 0.0;
  Param contrastRatio = floatParam(
      "contrastRatio", "Contrast Ratio", 0.3, "",
      "一站的胶至少要有参考峰值（全检测区候选暗段峰值的中位数）的这么多倍对比度才算有胶：断口两头模糊出来的"
      "残影宽度、位置都对，只是淡。演示数据满胶段每站都在 0.72 倍以上；定得太高，羽化过的断口两头会多算进去一站。",
      true);
  contrastRatio.min = 0.0;
  contrastRatio.max = 1.0;
  op.params = {form,        range,        step,         searchHalf,   mergeGap,
               window,      contrastMin,  presentRatio, centerRatio,  contrastRatio};
  op.capabilities = {/*cancellable=*/false, /*previewable=*/false, /*deterministic=*/true};
  op.compute = &compute;
  op.validate = &validate;
  r.addOperator(std::move(op));
}

}  // namespace lyflow::packs::glue
