// glue.polyline「折线胶路」（docs/glue-measure-ops.md §6）：参数给定的折线 → glue.Polyline（示教走廊用）。
// 与 taught_path 同一个重采样（resampleTaught），不带 tolerance、sharpMin 这些示教语义。
#include <cmath>

#include "glue.h"

namespace lyflow::packs::glue {
namespace {

constexpr double kMinLength = 8.0;

/// 读 points 并检查 zone。没问题返回空；pts / total 给了就写出来。
std::vector<Issue> check(const ParamView& params, std::vector<P2>* ptsOut, double* totalOut) {
  std::vector<Issue> issues;
  const Json j = Json::parse(params.text("points"), nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded() || !j.is_array()) {
    issues.push_back(
        Issue::error("bad_param", "points 要是 JSON 数组 [[x, y], …]（图像像素）", "points"));
    return issues;
  }
  std::vector<P2> pts;
  for (std::size_t i = 0; i < j.size(); ++i) {
    const P2 p = pxOf(j[i]);
    if (!finite(p)) {
      issues.push_back(Issue::error(
          "bad_param", "points 的第 " + std::to_string(i + 1) + " 个点不是两个有限的数", "points"));
      return issues;
    }
    pts.push_back(p);
  }
  if (pts.size() < 2) {
    issues.push_back(Issue::error(
        "bad_param", "points 至少要两个点（现在 " + std::to_string(pts.size()) + " 个）",
        "points"));
    return issues;
  }
  const double total = polylineLength(pts);
  if (!(total >= kMinLength)) {
    issues.push_back(Issue::error(
        "bad_param", "折线总长 " + fmt(total) + " px，至少要 " + fmt(kMinLength, 0) + " px",
        "points"));
    return issues;
  }
  const auto zone = params.vec2("zone");
  const double z1 = zone[1] > 0 ? zone[1] : total;
  if (!(zone[0] >= 0) || !(z1 > zone[0]) || z1 > total + 1e-6) {
    issues.push_back(Issue::error("bad_param",
                                  "zone 要满足 0 ≤ start < end ≤ 折线总长 " + fmt(total) +
                                      " px（end = 0 表示到末端；现在是 [" + fmt(zone[0]) + ", " +
                                      fmt(zone[1]) + "]）",
                                  "zone"));
  }
  if (ptsOut)
    *ptsOut = std::move(pts);
  if (totalOut)
    *totalOut = total;
  return issues;
}

std::vector<Px> pxList(const std::vector<P2>& pts) {
  std::vector<Px> out;
  out.reserve(pts.size());
  for (const P2& p : pts)
    out.push_back(Px{p.x, p.y});
  return out;
}

Status compute(const Inputs&, const ParamView& params, Outputs& outputs, ExecContext&) {
  std::vector<P2> pts;
  double total = 0;
  if (const std::vector<Issue> issues = check(params, &pts, &total); !issues.empty()) {
    return Status::Error(Phase::Execute, "bad_param", issues.front().status.message,
                         issues.front().status.paramPath);
  }
  const auto zone = params.vec2("zone");
  const double z1 = zone[1] > 0 ? zone[1] : total;
  std::vector<double> s;
  std::vector<P2> points, tangents;
  if (!resampleTaught(pts, zone[0], z1, &s, &points, &tangents))
    return Status::Error(Phase::Execute, "bad_param", "折线按 zone 重采样失败", "zone");
  outputs.set("line", polylineRecord(s, points, tangents));
  Overlay2D ov;
  ov.polyline("taught", pxList(pts), false, "折线");
  ov.polyline("path", pxList(points), false, "胶路");
  outputs.set("overlay", ov.data());
  return Status::Ok();
}

std::vector<Issue> validate(const ParamView& params, const std::set<std::string>&) {
  return check(params, nullptr, nullptr);
}

} // namespace

void registerPolyline(Registry& r) {
  OperatorDesc op;
  op.id = "glue.polyline";
  op.version = "1.0.0";
  op.label = "折线胶路";
  op.category = "涂胶/测量";
  op.keywords = {"glue", "polyline", "taught", "corridor", "涂胶", "折线", "示教", "走廊"};
  op.doc =
      "把参数给定的折线（图像 px）按弧长重采样成胶路：s 从第一个点沿折线量起，取 zone 那一段，每 2 px 一点"
      "（两端各一点），切向为中心差分。end = 0 表示到末端；相邻重合的点跳过。不看图、不找胶。";
  op.inputs = {};
  op.outputs = {
      withContract(Port{"line", "Record", "Line", "glue.Polyline：zone 那一段。", true},
                   {{"recordType", kPolylineType}}),
      Port{"overlay", "Record", "Overlay", "lyflow.overlay2d：整条折线与 zone 那一段。", true},
  };
  Param points;
  points.name = "points";
  points.type = ParamType::Text;
  points.label = "Points";
  points.doc = "折线：JSON 数组 [[x, y], …]，图像像素，至少两个点、总长 ≥ 8 px。";
  points.def = Value::text("[]");
  points.rows = 4;
  points.placeholder = "[[640, 500], [640, 200]]";
  points.tuningRole = "geometry";
  Param zone = vec2Param("zone", "Zone", 0.0, 0.0, "px", {"Start", "End"},
                         "取哪一段：s 的区间；end = 0 表示到末端。");
  zone.min = 0.0;
  zone.tuningRole = "geometry";
  op.params = {points, zone};
  op.capabilities = {/*cancellable=*/false, /*previewable=*/false, /*deterministic=*/true};
  op.compute = &compute;
  op.validate = &validate;
  r.addOperator(std::move(op));
}

} // namespace lyflow::packs::glue
