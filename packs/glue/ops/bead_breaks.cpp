// glue.bead_breaks「查断胶」（glue-plan §2.3，D7）：胶路上连续无胶的站 ≥ minLength 就是一处断胶。
#include <algorithm>
#include <cmath>

#include "glue.h"

namespace lyflow::packs::glue {
namespace {

std::vector<Px> pxList(const std::vector<P2>& pts) {
  std::vector<Px> out;
  out.reserve(pts.size());
  for (const P2& p : pts) out.push_back(Px{p.x, p.y});
  return out;
}

Status compute(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext&) {
  BeadView bead;
  std::vector<Station> st;
  if (!readBead(inputs.get("bead"), &bead) || !readStations(bead.stations, &st)) {
    return Status::Error(Phase::Execute, "bad_input", "bead 不是完整的 glue.Bead", {}, "bead");
  }
  const Metric metric = metricFromInfo(bead.info);
  const double step = bead.info.value("stationStep", 4.0);
  double z0 = bead.line.s.front(), z1 = bead.line.s.back();
  if (const auto z = bead.info.find("zone"); z != bead.info.end() && z->is_array() && z->size() == 2) {
    z0 = numOf((*z)[0]);
    z1 = numOf((*z)[1]);
  }
  const double minLength = params.number("minLength");
  const bool atEnds = params.flag("countAtZoneEnds");

  Json breaks = Json::array(), ignored = Json::array();
  int shortGaps = 0;
  double longest = 0;
  Overlay2D ov;
  for (std::size_t i = 0; i < st.size();) {
    if (st[i].present) {
      ++i;
      continue;
    }
    const std::size_t first = i;
    std::size_t last = i;
    while (last + 1 < st.size() && !st[last + 1].present) ++last;
    i = last + 1;
    // 断口在两个有胶站之间：头尾各让出半个站距，再夹在检测区里
    const double sStart = std::max(z0, st[first].s - 0.5 * step);
    const double sEnd = std::min(z1, st[last].s + 0.5 * step);
    const bool atStart = first == 0;
    const bool atEnd = last + 1 == st.size();
    const std::vector<P2> along = bead.line.between(sStart, sEnd);
    const double lengthPx = sEnd - sStart;
    const double len = metric.calib ? metric.length(along) : lengthPx;
    if (!(len >= minLength)) {
      shortGaps += 1;
      continue;
    }
    Json b{{"sStart", round3(sStart)},
           {"sEnd", round3(sEnd)},
           {"length", numOrNull(len, 3)},
           {"lengthPx", round3(lengthPx)},
           {"start", pxJson(along.front())},
           {"end", pxJson(along.back())},
           {"stations", last - first + 1},
           {"atZoneStart", atStart},
           {"atZoneEnd", atEnd}};
    if (!atEnds && (atStart || atEnd)) {
      b["reason"] = "贴着检测区的端点，countAtZoneEnds = false 不计";
      ignored.push_back(std::move(b));
      ov.polyline("coarse", pxList(along), false, "端点处无胶 " + fmt(len) + " " + metric.unit() + "（不计）");
      continue;
    }
    longest = std::max(longest, len);
    ov.polyline("break", pxList(along), false, "断胶 " + fmt(len) + " " + metric.unit());
    breaks.push_back(std::move(b));
  }

  Json rec;
  rec["count"] = breaks.size();
  rec["longest"] = round3(longest);
  rec["unit"] = metric.unit();
  rec["minLength"] = minLength;
  rec["stationStep"] = step;
  rec["countAtZoneEnds"] = atEnds;
  rec["pathOk"] = bead.info.value("pathOk", false);
  rec["breaks"] = std::move(breaks);
  rec["ignored"] = std::move(ignored);
  rec["shortGaps"] = shortGaps;
  const double count = static_cast<double>(rec["count"].get<std::size_t>());
  Record out;
  out.type = kBreaksType;
  out.data = std::move(rec);
  outputs.set("breaks", Data::record(std::move(out)));
  lyflow::Measurement mc;
  mc.value = count;
  mc.ok = true;
  mc.unit = "";
  outputs.set("count", Data::measurement(std::move(mc)));
  lyflow::Measurement ml;
  ml.value = longest;
  ml.ok = true;
  ml.unit = metric.unit();
  outputs.set("longest", Data::measurement(std::move(ml)));
  outputs.set("overlay", ov.data());
  return Status::Ok();
}

}  // namespace

void registerBeadBreaks(Registry& r) {
  OperatorDesc op;
  op.id = "glue.bead_breaks";
  op.version = "1.0.0";
  op.label = "查断胶";
  op.category = "涂胶/积木";
  op.keywords = {"glue", "bead", "break", "gap", "涂胶", "断胶", "积木"};
  op.doc =
      "胶路上连续无胶的站连成一段，长度 ≥ minLength 就是一处断胶（D7：一根卡尺偶发失手不报缺陷）。"
      "一段断口从它第一个无胶站往前半个站距起、到最后一个无胶站往后半个站距止，夹在检测区里；"
      "长度按 bead 的单位（没接标定是 px，接了是 mm，按映射后的胶路长度算）。胶路没找到时整个检测区就是一处。"
      "countAtZoneEnds = false 时贴着检测区端点的那几段不计（起胶、收胶帧）。";
  op.inputs = {Port{"bead", "Bundle<glue.Bead>", "Bead", "glue.bead_width 的逐站结果。", true}};
  op.outputs = {
      Port{"breaks", "Record", "Breaks",
           "glue.Breaks：count、longest、unit、breaks（每段 sStart、sEnd、length、start / end 两端点）。", true},
      Port{"count", "Measurement", "Count", "断胶几处。", true},
      Port{"longest", "Measurement", "Longest", "最长的一处有多长（没有就是 0）。", true},
      Port{"overlay", "Record", "Overlay", "lyflow.overlay2d：每处断口沿胶路画一段。", true},
  };
  Param minLength = floatParam("minLength", "Min Length", 20.0, "px",
                               "连续无胶至少这么长才算断胶。接了标定时按 mm 解释（与 bead 的单位相同）。");
  minLength.min = 0.0;
  Param atEnds = boolParam("countAtZoneEnds", "Count At Zone Ends", true,
                           "贴着检测区两端的无胶段算不算断胶。起胶、收胶那几帧可以关掉。", true);
  op.params = {minLength, atEnds};
  op.capabilities = {/*cancellable=*/false, /*previewable=*/false, /*deterministic=*/true};
  op.compute = &compute;
  r.addOperator(std::move(op));
}

}  // namespace lyflow::packs::glue
