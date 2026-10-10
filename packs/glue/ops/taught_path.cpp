// glue.taught_path「示教胶路」（glue-plan §6，T1 / T2）：示教的胶中心线（图像 px 折线）→ 与 glue.bead_path 同形的
// glue.Path，下游 bead_width / bead_breaks / edge_distance / judge 一个不改照样接。它不看图、不找胶：
// 胶到底在不在线上、横着偏了多少，由 bead_width 沿这条线逐站量（T3）。
#include <cmath>

#include "glue.h"

namespace lyflow::packs::glue {
namespace {

/// points 参数：JSON 数组 [[x, y], …]（图像 px）。读不出来时 why 写给人看的原因。
bool parsePoints(const std::string& text, std::vector<P2>* out, std::string* why) {
  const Json j = Json::parse(text, nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded() || !j.is_array()) {
    *why = "points 要是 JSON 数组 [[x, y], …]（图像像素）";
    return false;
  }
  std::vector<P2> pts;
  for (std::size_t i = 0; i < j.size(); ++i) {
    const P2 p = pxOf(j[i]);
    if (!finite(p)) {
      *why = "points 的第 " + std::to_string(i + 1) + " 个点不是两个有限的数";
      return false;
    }
    pts.push_back(p);
  }
  if (pts.size() < 2) {
    *why = "points 至少要两个点（现在 " + std::to_string(pts.size()) + " 个）";
    return false;
  }
  *out = std::move(pts);
  return true;
}

constexpr double kMinLength = 8.0;  ///< 折线与检测区最短多长（与 bead_path 的 zone 同一个下限）

/// 加载期与执行期共用的检查：折线读得出来、不退化，zone 落在折线上。没问题返回空。
std::vector<Issue> check(const ParamView& params, std::vector<P2>* ptsOut, double* totalOut) {
  std::vector<Issue> issues;
  std::vector<P2> pts;
  std::string why;
  if (!parsePoints(params.text("points"), &pts, &why)) {
    issues.push_back(Issue::error("bad_param", why, "points"));
    return issues;
  }
  const double total = polylineLength(pts);
  if (!(total >= kMinLength)) {
    issues.push_back(Issue::error("bad_param",
                                  "示教折线退化了：总长 " + fmt(total) + " px，至少要 " + fmt(kMinLength, 0) +
                                      " px（点全重合在一处？）",
                                  "points"));
    return issues;
  }
  const auto zone = params.vec2("zone");
  const double z1 = zone[1] > 0 ? zone[1] : total;
  if (!(zone[0] >= 0) || !(z1 >= zone[0] + kMinLength) || z1 > total + 1e-6) {
    issues.push_back(Issue::error(
        "bad_param",
        "zone 要满足 0 ≤ start、end 比 start 至少远 8 px、end 不超过示教折线的总长 " + fmt(total) +
            " px（end = 0 表示到折线末端；现在是 [" + fmt(zone[0]) + ", " + fmt(zone[1]) + "]）",
        "zone"));
  }
  if (ptsOut) *ptsOut = std::move(pts);
  if (totalOut) *totalOut = total;
  return issues;
}

std::vector<Px> pxList(const std::vector<P2>& pts) {
  std::vector<Px> out;
  out.reserve(pts.size());
  for (const P2& p : pts) out.push_back(Px{p.x, p.y});
  return out;
}

Status compute(const Inputs&, const ParamView& params, Outputs& outputs, ExecContext&) {
  std::vector<P2> pts;
  double total = 0;
  // validate 已经拦过；这里再查一遍是给绕过加载期校验的调用方（例如直接拼 ParamMap 的测试）
  if (const std::vector<Issue> issues = check(params, &pts, &total); !issues.empty()) {
    return Status::Error(Phase::Execute, "bad_param", issues.front().status.message,
                         issues.front().status.paramPath);
  }
  const auto zone = params.vec2("zone");
  const double z0 = zone[0];
  const double z1 = zone[1] > 0 ? zone[1] : total;
  std::vector<double> s;
  std::vector<P2> points, tangents;
  if (!resampleTaught(pts, z0, z1, &s, &points, &tangents)) {
    return Status::Error(Phase::Execute, "bad_param", "示教折线按 zone 重采样失败", "zone");
  }
  const double tolerance = params.number("tolerance");
  const bool bright = params.choice("polarity") == "bright";
  const int widthMax = static_cast<int>(std::lround(params.number("widthMax")));

  // 与 bead_path 的 glue.PathInfo 同一组字段；搜索才有的那几个是 null —— 这条线不是找出来的（T2）
  Json info;
  info["ok"] = true;
  info["message"] = "";
  info["reason"] = "示教胶路：" + std::to_string(pts.size()) + " 个示教点，总长 " + fmt(total) + " px，没有在图里找胶";
  info["lineSource"] = "taught";
  info["headingSource"] = "taught";
  info["heading"] = Json();
  info["coverage"] = Json();
  info["sharpness"] = Json();
  info["beadlike"] = Json();
  info["residual"] = Json();
  info["zone"] = Json::array({round3(z0), round3(z1)});
  info["nozzle"] = Json();
  info["polarity"] = bright ? "bright" : "dark";
  info["widthMax"] = widthMax;
  info["tolerance"] = tolerance;
  info["sharpMin"] = params.number("sharpMin");
  info["taughtCount"] = pts.size();
  info["taughtLength"] = round3(total);
  info["coarseCount"] = 0;
  info["candidates"] = Json::array();
  outputs.set("path", pathBundle(polylineRecord(s, points, tangents), std::move(info)));

  // 叠画：整条示教折线与示教点、检测区那一段、检测区两端各一道 ± tolerance 的横杠（胶中心允许落在的走廊）
  Overlay2D ov;
  ov.polyline("taught", pxList(pts), false, "示教胶路");
  ov.points("taught", pxList(pts));
  ov.polyline("path", pxList(points), false, "检测区");
  const double bar = std::max(tolerance, 4.0);
  std::vector<std::array<Px, 2>> ends;
  for (const std::size_t i : {std::size_t{0}, points.size() - 1}) {
    const P2 n = perp(tangents[i]);
    const P2 a = points[i] - n * bar;
    const P2 b = points[i] + n * bar;
    ends.push_back({Px{a.x, a.y}, Px{b.x, b.y}});
  }
  ov.segments("zone", ends, "s = " + fmt(z0, 0) + " … " + fmt(z1, 0) + "，± " + fmt(tolerance, 0) + " px");
  outputs.set("overlay", ov.data());
  return Status::Ok();
}

std::vector<Issue> validate(const ParamView& params, const std::set<std::string>&) {
  return check(params, nullptr, nullptr);
}

}  // namespace

void registerTaughtPath(Registry& r) {
  OperatorDesc op;
  op.id = "glue.taught_path";
  op.version = "1.0.0";
  op.label = "示教胶路";
  op.category = "涂胶/积木";
  op.keywords = {"glue", "bead", "path", "taught", "polyline", "涂胶", "胶路", "示教", "折线", "积木"};
  op.doc =
      "把示教的胶中心线（图像像素折线，从喷嘴一侧往外排）变成与 glue.bead_path 同形的胶路 glue.Path，"
      "下游 bead_width / bead_breaks / edge_distance / judge 照样接。不看图、不找胶：拍照点固定（相机装在胶枪上），"
      "胶该在哪里由示教给定 —— 缺胶时不会被旁边的零件暗边、阴影拽走，断口位置就落在示教线上。\n"
      "s 从第一个示教点沿折线量起（px），检测区 zone 是 s 的一段，end = 0 表示到折线末端。info.lineSource = taught、"
      "ok = true；bead_path 搜索才有的字段（heading、coverage、sharpness、candidates…）是 null / 空。\n"
      "胶中心允许偏离示教线 tolerance（机器人 / 工件偏差、示教没点准）：bead_width 沿线逐站找出胶实际的偏移，"
      "轨迹之外的暗段不算胶；沿线一站都没有胶时判「检测区内没找到胶」。";
  op.inputs = {};
  op.outputs = {
      Port{"path", "Bundle<glue.Path>", "Path", "示教胶路（line）与它的来历（info，lineSource = taught）。", true},
      Port{"overlay", "Record", "Overlay",
           "lyflow.overlay2d：示教折线与示教点（taught）、检测区那一段（path）、两端的 ± tolerance 横杠（zone）。", true},
  };

  Param points;
  points.name = "points";
  points.type = ParamType::Text;
  points.label = "Points";
  points.doc =
      "示教的胶中心线：JSON 数组 [[x, y], …]，图像像素，从喷嘴一侧往外排，至少两个点。在示教图上沿胶的中心点出来"
      "（弯道多点几个：相邻两段的夹角最好不超过 15°，卡尺沿折线的法向布）。";
  points.def = Value::text("[]");
  points.rows = 4;
  points.placeholder = "[[640, 500], [640, 200]]";
  points.tuningRole = "geometry";
  Param zone = vec2Param("zone", "Zone", 0.0, 0.0, "px", {"Start", "End"},
                         "检测区：从第一个示教点沿折线的弧长 s 的一段。end = 0 表示到折线末端。"
                         "喷嘴附近（拉丝、胶头、阴影）与收胶端不检就把它们截掉。");
  zone.min = 0.0;
  zone.tuningRole = "geometry";
  Param tolerance = floatParam("tolerance", "Tolerance", 20.0, "px",
                               "胶中心最远离示教线多少还算这条胶（机器人 / 工件偏差加示教误差）。比它远的暗段不算胶 —— "
                               "收紧它，旁边平行的零件暗边、阴影就进不来；bead_width 的卡尺至少伸到 tolerance + 胶宽上限 / 2。");
  tolerance.min = 0.0;
  tolerance.max = 400.0;
  Param widthMax = floatParam("widthMax", "Width Max", 90.0, "px",
                              "期望的最大胶宽：bead_width 的黑顶帽结构元直径 = 它 + 1，比它宽的暗带不会有响应（与 bead_path 同义）。",
                              true);
  widthMax.min = 4.0;
  widthMax.max = 400.0;
  Param polarity = enumParam("polarity", "Polarity", "dark", "胶比背景暗（dark，黑顶帽）还是亮（bright，白顶帽）。",
                             {EnumOption{"dark", "暗胶", ""}, EnumOption{"bright", "亮胶", ""}}, true);
  Param sharpMin = floatParam("sharpMin", "Sharp Min", 0.4, "",
                              "D15 用在整条示教线上：bead_width 沿线认出的暗段，边缘陡度的第 25 百分位不到它就算没有胶"
                              "（压痕、阴影也是暗带，边缘却是缓的）。与 bead_path 的 sharpMin 同义。",
                              true);
  op.params = {points, zone, tolerance, widthMax, polarity, sharpMin};
  op.capabilities = {/*cancellable=*/false, /*previewable=*/false, /*deterministic=*/true};
  op.compute = &compute;
  op.validate = &validate;
  r.addOperator(std::move(op));
}

}  // namespace lyflow::packs::glue
