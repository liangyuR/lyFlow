// glue.judge「判定」（glue-plan §2.3，D7）：断胶、窄胶、宽胶、边距超差都是「连续 ≥ 最短长度的同类站」，
// 胶路没找到就是一条「检测区内没找到胶」。
#include <algorithm>
#include <cmath>
#include <map>

#include "glue.h"

namespace lyflow::packs::glue {
namespace {

struct Defect {
  std::string type;  // missing / break / narrow / wide / near / far
  double sStart = 0;
  double sEnd = 0;
  double length = 0;
  double value = 0;
  double limit = 0;
  std::string unit;
  std::string message;
};

const char* typeName(const std::string& type) {
  static const std::map<std::string, const char*> kNames = {
      {"missing", "无胶"}, {"break", "断胶"}, {"narrow", "窄胶"},
      {"wide", "宽胶"},    {"near", "边距过近"}, {"far", "边距过远"}};
  const auto it = kNames.find(type);
  return it == kNames.end() ? "缺陷" : it->second;
}

std::vector<Px> pxList(const std::vector<P2>& pts) {
  std::vector<Px> out;
  out.reserve(pts.size());
  for (const P2& p : pts) out.push_back(Px{p.x, p.y});
  return out;
}

Px px(P2 p) { return Px{p.x, p.y}; }

/// 逐站的一个量（宽度或边距，NaN = 这一站没有）连续超出界限的几段。
struct Series {
  std::vector<double> s;
  std::vector<double> v;
};

void runsOutOf(const Series& x, bool below, double limit, const std::string& type, double step,
               double z0, double z1, double minLen, const Metric& metric, const Polyline& line,
               const std::string& unit, std::vector<Defect>* out) {
  for (std::size_t i = 0; i < x.v.size();) {
    const auto bad = [&](std::size_t k) {
      return std::isfinite(x.v[k]) && (below ? x.v[k] < limit : x.v[k] > limit);
    };
    if (!bad(i)) {
      ++i;
      continue;
    }
    std::size_t j = i;
    double worst = x.v[i];
    while (j + 1 < x.v.size() && bad(j + 1)) {
      ++j;
      worst = below ? std::min(worst, x.v[j]) : std::max(worst, x.v[j]);
    }
    const double sStart = std::max(z0, x.s[i] - 0.5 * step);
    const double sEnd = std::min(z1, x.s[j] + 0.5 * step);
    const double len = metric.calib ? metric.length(line.between(sStart, sEnd)) : sEnd - sStart;
    i = j + 1;
    if (len < minLen) continue;
    Defect d;
    d.type = type;
    d.sStart = sStart;
    d.sEnd = sEnd;
    d.length = len;
    d.value = worst;
    d.limit = limit;
    d.unit = unit;
    d.message = std::string(typeName(type)) + (below ? " 最小 " : " 最大 ") + fmt(worst) + " " + unit +
                (below ? " < " : " > ") + fmt(limit) + " " + unit + "（s " + fmt(sStart, 0) + "–" +
                fmt(sEnd, 0) + "，长 " + fmt(len) + " " + unit + "）";
    out->push_back(std::move(d));
  }
}

Status compute(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext&) {
  BeadView bead;
  std::vector<Station> st;
  if (!readBead(inputs.get("bead"), &bead) || !readStations(bead.stations, &st)) {
    return Status::Error(Phase::Execute, "bad_input", "bead 不是完整的 glue.Bead", {}, "bead");
  }
  const Record* breaks = inputs.get("breaks").asRecord();
  if (!breaks) return Status::Error(Phase::Execute, "bad_input", "breaks 不是 Record", {}, "breaks");
  const Record* edge = inputs.has("edge") ? inputs.get("edge").asRecord() : nullptr;

  const Metric metric = metricFromInfo(bead.info);
  const std::string unit = bead.info.value("unit", std::string("px"));
  const bool pathOk = bead.info.value("pathOk", false);
  const double step = bead.info.value("stationStep", 4.0);
  double z0 = bead.line.s.front(), z1 = bead.line.s.back();
  if (const auto z = bead.info.find("zone"); z != bead.info.end() && z->is_array() && z->size() == 2) {
    z0 = numOf((*z)[0]);
    z1 = numOf((*z)[1]);
  }
  const auto widthLimits = params.vec2("widthLimits");
  const auto distanceLimits = params.vec2("distanceLimits");
  const double maxBreak = params.number("maxBreak");
  const double minLen = params.number("minDefectLength");

  std::vector<Defect> defects;
  Json warnings = Json::array();
  if (!pathOk) {
    Defect d;
    d.type = "missing";
    d.sStart = z0;
    d.sEnd = z1;
    d.length = metric.calib ? metric.length(bead.line.between(z0, z1)) : z1 - z0;
    d.value = numOf(bead.info.value("coverage", Json()));
    d.limit = 0;
    d.unit = unit;
    d.message = kNoBeadMessage;
    defects.push_back(std::move(d));
  } else {
    // 断胶：bead_breaks 报出来的每一段（≥ 它的 minLength），比允许的长就是缺陷；maxBreak < 0 不判
    const std::string breakUnit = breaks->data.value("unit", unit);
    if (maxBreak >= 0) {
      for (const Json& b : breaks->data.value("breaks", Json::array())) {
        const double len = numOf(b.value("length", Json()));
        if (!(len > maxBreak)) continue;
        Defect d;
        d.type = "break";
        d.sStart = numOf(b.value("sStart", Json()));
        d.sEnd = numOf(b.value("sEnd", Json()));
        d.length = len;
        d.value = len;
        d.limit = maxBreak;
        d.unit = breakUnit;
        d.message = "断胶 " + fmt(len) + " " + breakUnit + "（s " + fmt(d.sStart, 0) + "–" +
                    fmt(d.sEnd, 0) + "，允许 ≤ " + fmt(maxBreak) + " " + breakUnit + "）";
        defects.push_back(std::move(d));
      }
    }
    // 窄胶 / 宽胶：有胶的站连续超出 widthLimits（0 = 不判那一侧）
    Series width;
    const Json& jw = bead.stations.value("width", Json::array());
    for (std::size_t i = 0; i < st.size(); ++i) {
      width.s.push_back(st[i].s);
      width.v.push_back(st[i].present && i < jw.size() ? numOf(jw[i]) : std::nan(""));
    }
    if (widthLimits[0] > 0) {
      runsOutOf(width, true, widthLimits[0], "narrow", step, z0, z1, minLen, metric, bead.line, unit,
                &defects);
    }
    if (widthLimits[1] > 0) {
      runsOutOf(width, false, widthLimits[1], "wide", step, z0, z1, minLen, metric, bead.line, unit,
                &defects);
    }
    // 边距过近 / 过远：只看量到了、也没被剔掉的站
    if (distanceLimits[0] > 0 || distanceLimits[1] > 0) {
      if (!edge) {
        warnings.push_back("设了 distanceLimits，但没接 edge：边距没判");
      } else if (edge->data.value("side", std::string("none")) == "none") {
        warnings.push_back("设了 distanceLimits，但两侧都没找到零件边：边距没判");
      } else {
        Series dist;
        const Json& js = edge->data.value("s", Json::array());
        const Json& jd = edge->data.value("distance", Json::array());
        for (std::size_t i = 0; i < js.size() && i < jd.size(); ++i) {
          dist.s.push_back(numOf(js[i]));
          dist.v.push_back(numOf(jd[i]));
        }
        const std::string du = edge->data.value("unit", unit);
        if (distanceLimits[0] > 0) {
          runsOutOf(dist, true, distanceLimits[0], "near", step, z0, z1, minLen, metric, bead.line,
                    du, &defects);
        }
        if (distanceLimits[1] > 0) {
          runsOutOf(dist, false, distanceLimits[1], "far", step, z0, z1, minLen, metric, bead.line,
                    du, &defects);
        }
      }
    }
  }

  // 结论：缺陷按类型数一遍，一句话写清楚
  const char* order[] = {"missing", "break", "narrow", "wide", "near", "far"};
  Json counts = Json::object();
  for (const char* t : order) counts[t] = 0;
  Json list = Json::array();
  for (const Defect& d : defects) {
    counts[d.type] = counts[d.type].get<int>() + 1;
    const std::vector<P2> along = bead.line.between(d.sStart, d.sEnd);
    list.push_back(Json{{"type", d.type},
                        {"sStart", round3(d.sStart)},
                        {"sEnd", round3(d.sEnd)},
                        {"length", numOrNull(d.length)},
                        {"value", numOrNull(d.value)},
                        {"limit", numOrNull(d.limit)},
                        {"unit", d.unit},
                        {"start", pxJson(along.front())},
                        {"end", pxJson(along.back())},
                        {"message", d.message}});
  }
  const bool ok = defects.empty();
  std::string message = "OK";
  if (!pathOk) {
    message = kNoBeadMessage;
  } else if (!ok) {
    message = "NG：";
    bool firstPart = true;
    for (const char* t : order) {
      const int n = counts[t].get<int>();
      if (n == 0) continue;
      if (!firstPart) message += "；";
      firstPart = false;
      message += std::string(typeName(t)) + " " + std::to_string(n) + " 处";
      if (std::string(t) == "break") {
        double longest = 0;
        for (const Defect& d : defects) {
          if (d.type == "break") longest = std::max(longest, d.length);
        }
        message += "（最长 " + fmt(longest) + " " + unit + "）";
      }
    }
  }
  Json verdict;
  verdict["ok"] = ok;
  verdict["message"] = message;
  verdict["unit"] = unit;
  verdict["pathOk"] = pathOk;
  verdict["defectCount"] = defects.size();
  verdict["counts"] = counts;
  verdict["defects"] = std::move(list);
  verdict["warnings"] = std::move(warnings);
  verdict["limits"] = Json{{"width", Json::array({widthLimits[0], widthLimits[1]})},
                           {"distance", Json::array({distanceLimits[0], distanceLimits[1]})},
                           {"maxBreak", maxBreak},
                           {"minDefectLength", minLen}};
  Record rec;
  rec.type = kVerdictType;
  rec.data = std::move(verdict);
  outputs.set("verdict", Data::record(std::move(rec)));
  lyflow::Measurement m;
  m.value = ok ? 1.0 : 0.0;
  m.ok = true;
  m.unit = "";
  m.verdict = ok ? "ok" : "ng";
  m.message = message;
  outputs.set("ok", Data::measurement(std::move(m)));

  // 叠画：胶路、两条胶边、零件边点、全部缺陷、结论
  Overlay2D ov;
  ov.polyline(pathOk ? "path" : "missing", pxList(bead.line.points));
  for (const auto& [a, b] : presentRuns(st)) {
    std::vector<Px> left, right;
    for (std::size_t i = a; i <= b; ++i) {
      left.push_back(px(st[i].left()));
      right.push_back(px(st[i].right()));
    }
    ov.polyline("edge.left", left);
    ov.polyline("edge.right", right);
  }
  if (edge) {
    std::vector<Px> part;
    const Json& je = edge->data.value("edge", Json::array());
    const Json& jst = edge->data.value("status", Json::array());
    for (std::size_t i = 0; i < je.size() && i < jst.size(); ++i) {
      if (jst[i] == "ok") part.push_back(px(pxOf(je[i])));
    }
    ov.points("part", part);
  }
  for (const Defect& d : defects) {
    if (d.type == "missing") continue;
    ov.polyline("defect", pxList(bead.line.between(d.sStart, d.sEnd)), false, d.message);
  }
  ov.text(ok ? "ok" : "ng", message, Px{10.0, 22.0});
  outputs.set("overlay", ov.data());
  return Status::Ok();
}

std::vector<Issue> validate(const ParamView& params, const std::set<std::string>&) {
  std::vector<Issue> issues;
  for (const char* name : {"widthLimits", "distanceLimits"}) {
    const auto v = params.vec2(name);
    if (v[0] < 0 || v[1] < 0) {
      issues.push_back(Issue::error("bad_param", std::string(name) + " 不能是负数（0 = 不判那一侧）", name));
    } else if (v[0] > 0 && v[1] > 0 && v[1] <= v[0]) {
      issues.push_back(Issue::error("bad_param", std::string(name) + " 的上限要大于下限", name));
    }
  }
  return issues;
}

}  // namespace

void registerJudge(Registry& r) {
  OperatorDesc op;
  op.id = "glue.judge";
  op.version = "1.0.0";
  op.label = "判定";
  op.category = "涂胶/积木";
  op.keywords = {"glue", "bead", "judge", "verdict", "ok", "ng", "涂胶", "判定", "积木"};
  op.doc =
      "把断胶、窄胶、宽胶、边距过近 / 过远汇成一个 OK / NG。除断胶外都是「连续 ≥ minDefectLength 的同类站」"
      "才算一处缺陷（D7）；断胶由 bead_breaks 按它的 minLength 连好，比 maxBreak 长的判 NG。"
      "胶路没找到时只报一条「检测区内没找到胶」。限值与 bead 同单位（没接标定 px，接了 mm）；"
      "宽度、边距的限值 0 表示不判那一侧。\n"
      "ok 输出：value 1 = OK、0 = NG，verdict 字段 ok / ng，message 是结论那一句。";
  op.inputs = {
      Port{"bead", "Bundle<glue.Bead>", "Bead", "glue.bead_width 的逐站结果。", true},
      withContract(Port{"breaks", "Record", "Breaks", "glue.bead_breaks 的断口。", true},
                   {{"recordType", kBreaksType}}),
      withContract(Port{"edge", "Record", "Edge", "可选：glue.edge_distance 的边距。不接就不判边距。", false},
                   {{"recordType", kEdgeType}}),
  };
  op.outputs = {
      Port{"verdict", "Record", "Verdict",
           "glue.Verdict：ok、message、defectCount、counts（按类型）、defects（type / sStart / sEnd / length / value / "
           "limit / unit / message）。",
           true},
      Port{"ok", "Measurement", "OK", "1 = OK、0 = NG。", true},
      Port{"overlay", "Record", "Overlay", "lyflow.overlay2d：胶路、两边、零件边点、全部缺陷与结论。", true},
  };
  Param width = vec2Param("widthLimits", "Width Limits", 0.0, 0.0, "px", {"Min", "Max"},
                          "胶宽的下限与上限（与 bead 同单位）。0 = 不判那一侧。");
  width.min = 0.0;
  Param distance = vec2Param("distanceLimits", "Distance Limits", 0.0, 0.0, "px", {"Min", "Max"},
                             "边距的下限与上限（与 edge 同单位）。0 = 不判那一侧；要接 edge。");
  distance.min = 0.0;
  Param maxBreak = floatParam("maxBreak", "Max Break", 0.0, "px",
                              "允许的断口长度：比它长的断口判 NG。0 = 一处都不允许（bead_breaks 报出来的每一段都判 NG），"
                              "负数 = 不判断胶。");
  Param minLen = floatParam("minDefectLength", "Min Defect Length", 20.0, "px",
                            "窄胶、宽胶、边距超差要连续这么长才算一处缺陷（一根卡尺偶发失手不报）。0 = 一站就算。");
  minLen.min = 0.0;
  op.params = {width, distance, maxBreak, minLen};
  op.capabilities = {/*cancellable=*/false, /*previewable=*/false, /*deterministic=*/true};
  op.compute = &compute;
  op.validate = &validate;
  r.addOperator(std::move(op));
}

}  // namespace lyflow::packs::glue
