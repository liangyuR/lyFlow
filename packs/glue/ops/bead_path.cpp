// glue.bead_path「定胶路」（glue-plan §2.3，D1 / D2 / D3 / D6 / D15）。
#include <cmath>

#include "glue.h"

namespace lyflow::packs::glue {
namespace {

PathSpec specOf(const ParamView& params) {
  PathSpec spec;
  const auto nozzle = params.vec2("nozzle");
  spec.nozzle = P2(nozzle[0], nozzle[1]);
  const auto zone = params.vec2("zone");
  spec.zoneStart = zone[0];
  spec.zoneEnd = zone[1];
  const auto sector = params.vec2("sector");
  spec.sectorFromDeg = sector[0];
  spec.sectorToDeg = sector[1];
  spec.useHeading = params.choice("headingSource") == "param";
  spec.headingDeg = params.number("heading");
  spec.headingTolDeg = params.number("headingTol");
  spec.widthMax = static_cast<int>(std::lround(params.number("widthMax")));
  spec.bright = params.choice("polarity") == "bright";
  spec.maxGap = params.number("maxGap");
  spec.minCoverage = params.number("minCoverage");
  spec.contrastMin = params.number("contrastMin");
  spec.sharpMin = params.number("sharpMin");
  return spec;
}

/// 圆弧（叠画用）：以 center 为心、半径 r，从 a0 到 a1 度，每 2° 一个点。
std::vector<Px> arc(P2 center, double r, double a0, double a1) {
  std::vector<Px> pts;
  const int n = std::max(2, static_cast<int>(std::ceil((a1 - a0) / 2.0)) + 1);
  for (int i = 0; i < n; ++i) {
    const P2 p = center + dirOfDeg(a0 + (a1 - a0) * i / (n - 1)) * r;
    pts.push_back(Px{p.x, p.y});
  }
  return pts;
}

std::vector<Px> pxList(const std::vector<P2>& pts) {
  std::vector<Px> out;
  out.reserve(pts.size());
  for (const P2& p : pts) out.push_back(Px{p.x, p.y});
  return out;
}

Status compute(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext&) {
  const cv::Mat gray = grayOf(inputs.get("image"));
  if (gray.empty()) {
    return Status::Error(Phase::Execute, "bad_input", "需要全分辨率非空 u8 图像；其他位深先接 image.normalize，缩小预览请改用完整运行", {}, "image");
  }
  const PathSpec spec = specOf(params);
  const BeadPath path = findBeadPath(gray, spec);

  Json info;
  info["ok"] = path.ok;
  info["message"] = path.ok ? "" : kNoBeadMessage;
  info["reason"] = path.reason;
  info["heading"] = numOrNull(path.headingDeg, 2);
  info["headingSource"] = path.headingSource;
  info["coverage"] = numOrNull(path.coverage, 4);
  info["sharpness"] = numOrNull(path.sharpness, 3);
  info["beadlike"] = path.beadlike;
  info["residual"] = numOrNull(path.residual, 3);
  info["zone"] = Json::array({spec.zoneStart, spec.zoneEnd});
  info["nozzle"] = pxJson(spec.nozzle);
  info["polarity"] = spec.bright ? "bright" : "dark";
  info["widthMax"] = spec.widthMax;
  info["lineSource"] = path.fallback ? "fallback" : "fit";
  info["coarseCount"] = path.coarse.size();
  Json cands = Json::array();
  for (const PathCandidate& c : path.candidates) {
    cands.push_back(Json{{"heading", c.headingDeg},
                         {"fanScore", numOrNull(c.fanScore, 2)},
                         {"coverage", numOrNull(c.coverage, 4)},
                         {"sharpness", numOrNull(c.sharpness, 3)},
                         {"beadlike", c.beadlike},
                         {"score", numOrNull(c.score, 4)}});
  }
  info["candidates"] = std::move(cands);
  outputs.set("path", pathBundle(polylineRecord(path.s, path.points, path.tangents), std::move(info)));

  // 叠画：喷嘴、搜索的扇区、检测区两端的弧、粗找到的胶点、胶路
  Overlay2D ov;
  ov.circle("nozzle", Px{spec.nozzle.x, spec.nozzle.y}, 6.0, "喷嘴");
  double a0 = spec.useHeading ? spec.headingDeg - spec.headingTolDeg : spec.sectorFromDeg;
  double a1 = spec.useHeading ? spec.headingDeg + spec.headingTolDeg : spec.sectorToDeg;
  const bool fullCircle = a1 - a0 >= 360.0 - 1e-6;
  if (!fullCircle) {
    const P2 e0 = spec.nozzle + dirOfDeg(a0) * spec.zoneEnd;
    const P2 e1 = spec.nozzle + dirOfDeg(a1) * spec.zoneEnd;
    ov.segments("sector", {{Px{spec.nozzle.x, spec.nozzle.y}, Px{e0.x, e0.y}},
                           {Px{spec.nozzle.x, spec.nozzle.y}, Px{e1.x, e1.y}}});
  } else {
    a0 = -180.0;
    a1 = 180.0;
  }
  ov.polyline("zone", arc(spec.nozzle, spec.zoneStart, a0, a1), fullCircle,
              "s = " + fmt(spec.zoneStart, 0));
  ov.polyline("zone", arc(spec.nozzle, spec.zoneEnd, a0, a1), fullCircle,
              "s = " + fmt(spec.zoneEnd, 0));
  ov.points("coarse", pxList(path.coarse));
  if (path.ok) {
    ov.polyline("path", pxList(path.points), false, "胶路");
  } else {
    ov.polyline("missing", pxList(path.points), false, kNoBeadMessage);
    ov.text("ng", std::string(kNoBeadMessage) + "：" + path.reason,
            Px{spec.nozzle.x + 12.0, spec.nozzle.y + 24.0});
  }
  outputs.set("overlay", ov.data());
  return Status::Ok();
}

std::vector<Issue> validate(const ParamView& params, const std::set<std::string>&) {
  std::vector<Issue> issues;
  const auto zone = params.vec2("zone");
  if (!(zone[0] >= 0) || !(zone[1] > zone[0] + 8)) {
    issues.push_back(Issue::error("bad_param",
                                  "zone 要满足 0 ≤ start 且 end 比 start 至少远 8 px（现在是 [" +
                                      fmt(zone[0], 0) + ", " + fmt(zone[1], 0) + "]）",
                                  "zone"));
  }
  if (params.choice("headingSource") == "search") {
    const auto sector = params.vec2("sector");
    if (!(sector[1] > sector[0]) || sector[1] - sector[0] > 360.0 + 1e-6) {
      issues.push_back(Issue::error("bad_param",
                                    "sector 要从小到大、跨度不超过 360°（现在是 [" + fmt(sector[0], 0) +
                                        ", " + fmt(sector[1], 0) + "]）",
                                    "sector"));
    }
  }
  return issues;
}

}  // namespace

void registerBeadPath(Registry& r) {
  OperatorDesc op;
  op.id = "glue.bead_path";
  op.version = "1.0.0";
  op.label = "定胶路";
  op.category = "涂胶/积木";
  op.keywords = {"glue", "bead", "path", "nozzle", "涂胶", "胶路", "喷嘴", "积木"};
  op.doc =
      "在检测区里粗找胶点，稳健拟合一条光滑的胶路（跨得过断口，覆盖整个检测区）。检测区是从喷嘴出胶点"
      "沿胶路的弧长 s 的一段，离开喷嘴（D2：喷嘴附近约 100 px 有拉丝、胶头、阴影，不可信）。\n"
      "胶的响应用黑顶帽（闭运算 − 原图），不用固定灰度阈值：背景从全黑到过曝都有，胶只在「比局部背景暗」"
      "这一点上稳定。方向：heading 给了（headingSource = param）只在 heading ± headingTol 里找；没给就在 "
      "sector 扇区里扇形搜索，取前三个相距 ≥ 15° 的峰各试一遍，留「覆盖率 ×（像胶 ? 1 : 0.1）」最高的"
      "（射线本身的响应不到最强那条四分之一的再打对折）。像胶 = 暗带边缘陡（D15：零件上的压痕、阴影也是暗带，"
      "但边缘是缓的）。粗找时胶点要往外走（离喷嘴 ≥ 0.75·s、方位不出搜索范围 20°）；一步接一步时横着不超过半个"
      "胶宽；断口之后重新接上的胶点要与此前的胶一样浓、一样陡（直胶还要一样宽），跟丢了就退回选中的那条射线上找。"
      "拟合之后再做一次 σ = 12 px 的平滑，s 按这条光滑胶路的弧长算。\n"
      "覆盖率低于 minCoverage 或不像胶时 info.ok = false、message 是「检测区内没找到胶」，下游照常跑出"
      "「全段无胶」—— 没有胶本身就是一个 NG 结果，不是执行错误。";
  op.inputs = {Port{"image", "Image", "Image", "胶枪相机的一帧（三通道先转灰度）。", true}};
  op.outputs = {
      Port{"path", "Bundle<glue.Path>", "Path", "胶路（line）与它是怎么找到的（info）。", true},
      Port{"overlay", "Record", "Overlay",
           "lyflow.overlay2d：喷嘴、扇区、检测区两端的弧、粗找到的胶点、胶路。", true},
  };

  Param nozzle = vec2Param("nozzle", "Nozzle", 640.0, 512.0, "px", {"X", "Y"},
                           "喷嘴出胶点，图像像素坐标。相机装在胶枪上，它在图里是固定的：在图像视图里点一下。");
  Param zone = vec2Param("zone", "Zone", 100.0, 400.0, "px", {"Start", "End"},
                         "检测区：从喷嘴沿胶路的弧长 s 的一段。start 离开喷嘴（默认 100 px），粗找从 start 开始。");
  zone.min = 0.0;
  Param sector = vec2Param("sector", "Sector", -180.0, 180.0, "deg", {"From", "To"},
                           "没给 heading 时胶离开喷嘴的方向范围（度，图像坐标：0° 朝右、−90° 朝上）。"
                           "默认整圈；知道胶大致朝哪边走时收窄它，扇形搜索更不容易被别的暗带带偏。");
  Param source = enumParam(
      "headingSource", "Heading Source", "search",
      "search = 在 sector 里扇形搜索；param = 只在 heading ± headingTol 里找（G3 由机器人经顶层图参数逐帧给）。",
      {EnumOption{"search", "扇形搜索", ""}, EnumOption{"param", "用 heading 参数", ""}}, true);
  Param heading = visibleWhen(floatParam("heading", "Heading", -90.0, "deg",
                                         "胶的方向（度，图像坐标）。headingSource = param 时生效。", true),
                              "headingSource", "param");
  Param headingTol = visibleWhen(
      floatParam("headingTol", "Heading Tol", 15.0, "deg", "heading 两侧各放宽多少度。", true),
      "headingSource", "param");
  headingTol.min = 1.0;
  Param widthMax = floatParam("widthMax", "Width Max", 90.0, "px",
                              "期望的最大胶宽。黑顶帽的结构元直径 = 它 + 1，比它宽的暗带不会有响应。", true);
  widthMax.min = 4.0;
  widthMax.max = 400.0;
  Param polarity = enumParam("polarity", "Polarity", "dark", "胶比背景暗（dark，黑顶帽）还是亮（bright，白顶帽）。",
                             {EnumOption{"dark", "暗胶", ""}, EnumOption{"bright", "亮胶", ""}}, true);
  Param maxGap = floatParam("maxGap", "Max Gap", 120.0, "px",
                            "粗找时连续没找到胶的最长距离：超过它就不再接受新的胶点，后面的胶路按已找到的胶点外推"
                            "（免得在长断口里滑到别的暗带上）。比它短的断口照样跨得过去。",
                            true);
  maxGap.min = 0.0;
  Param minCoverage = floatParam("minCoverage", "Min Coverage", 0.3, "",
                                 "粗找找到胶的步数占比低于它就算没找到胶。", true);
  minCoverage.min = 0.0;
  minCoverage.max = 1.0;
  Param contrastMin = floatParam("contrastMin", "Contrast Min", 16.0, "",
                                 "粗找时卡尺上一段响应的峰值至少这么高才算胶（灰度级）。", true);
  contrastMin.min = 1.0;
  Param sharpMin = floatParam("sharpMin", "Sharp Min", 0.4, "",
                              "D15：暗带边缘陡度的第 25 百分位至少这么高才算像胶。直胶实测 0.84–1.20、"
                              "螺旋胶 0.64–0.80，零件上的压痕 ≤ 0。",
                              true);
  op.params = {nozzle, zone,    sector,      source,      heading,  headingTol,
               widthMax, polarity, maxGap, minCoverage, contrastMin, sharpMin};
  for (auto& p : op.params)
    if (p.name == "nozzle" || p.name == "zone" || p.name == "sector" || p.name == "heading")
      p.tuningRole = "geometry";
  op.capabilities = {/*cancellable=*/false, /*previewable=*/false, /*deterministic=*/true};
  op.compute = &compute;
  op.validate = &validate;
  r.addOperator(std::move(op));
}

}  // namespace lyflow::packs::glue
