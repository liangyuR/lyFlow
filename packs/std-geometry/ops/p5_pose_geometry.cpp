#include "p5_pose_geometry_common.h"
#include <numeric>

namespace lyflow::packs::std_geometry {
namespace {
using namespace p5;

Status symmetry(const ParamView& p, int& ax, int& order) {
  ax = axis(p.choice("symmetryAxis"));
  const auto n = p.integer("symmetryOrder");
  if (ax < 0 || n < 1 || n > 4096)
    return badParam("对称轴须为X/Y/Z，对称阶数须1..4096", "symmetryOrder");
  order = static_cast<int>(n);
  return Status::Ok();
}
Status flip(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  Record r;
  auto s = list(in, "poses", r);
  if (!s.ok)
    return s;
  int ax, n;
  s = symmetry(p, ax, n);
  if (!s.ok)
    return s;
  int align = axis(p.choice("alignAxis"));
  auto target = vector(p, "direction");
  if (align < 0 || align == ax || !normalize(target))
    return badParam("被优化轴须不同于对称轴，方向须非零有限", "direction");
  if (r.data["items"].size() * static_cast<std::size_t>(n) > 1000000)
    return resource();
  for (auto& item : r.data["items"]) {
    if (ctx.cancelled())
      return cancelled();
    const auto q = item["rotation"].get<poses::Quaternion>();
    auto best = q;
    double score = -2;
    for (int k = 0; k < n; ++k) {
      auto candidate = poses::multiply(q, turn(ax, 2 * poses::kPi * k / n));
      double d = dot(rotated(candidate, basis(align)), target);
      if (d > score + 1e-14) {
        score = d;
        best = candidate;
      }
    }
    item["rotation"] = best;
    unsafe(item);
  }
  return put(out, "poses", std::move(r));
}
Status rotate(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  Record r;
  auto s = list(in, "poses", r);
  if (!s.ok)
    return s;
  const int fixed = axis(p.choice("fixedAxis")), moving = axis(p.choice("rotateAxis"));
  if (!p.flag("continuousSymmetry") || fixed < 0 || moving < 0 || fixed == moving)
    return badParam("须声明连续轴对称，固定轴和旋转轴必须不同", "continuousSymmetry");
  const auto ref = vector(p, "reference");
  if (!finite(ref))
    return badParam("参考向量必须有限", "reference");
  for (auto& item : r.data["items"]) {
    if (ctx.cancelled())
      return cancelled();
    auto q = item["rotation"].get<poses::Quaternion>();
    auto target = ref;
    if (p.choice("mode") == "point")
      target = sub(ref, jvector(item["position"]));
    else if (p.choice("mode") != "direction")
      return badParam("未知旋转模式", "mode");
    if (!p.flag("useZ"))
      target[2] = 0;
    if (p.flag("reverse"))
      target = mul(target, -1);
    const auto f = rotated(q, basis(fixed)), v = rotated(q, basis(moving));
    if (!normalize(target))
      return bad("参考点重合或方向为零");
    target = sub(target, mul(f, dot(f, target)));
    if (std::hypot(target[0], target[1], target[2]) < 1e-12 || !normalize(target))
      return bad("目标方向平行固定轴，连续轴姿态不可确定");
    const double a = std::atan2(dot(f, cross(v, target)), dot(v, target));
    item["rotation"] = poses::multiply(q, turn(fixed, a));
    unsafe(item);
  }
  return put(out, "poses", std::move(r));
}
Status compare(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  Record a, b;
  auto s = list(in, "a", a);
  if (!s.ok)
    return s;
  s = list(in, "b", b);
  if (!s.ok)
    return s;
  s = transformList(in, "bToA", b, a.data["frame"]);
  if (!s.ok)
    return s;
  std::vector<std::pair<std::size_t, std::size_t>> pairs;
  s = paired(a, b, p.choice("pairing"), pairs);
  if (!s.ok)
    return s;
  const auto tolerance = vector(p, "positionTolerance"), angles = vector(p, "axisTolerance");
  const double limit = p.number("angleTolerance");
  for (double v : tolerance)
    if (!validNumber(v) || v < 0)
      return badParam("位置容差须有限非负米", "positionTolerance");
  for (double v : angles)
    if (!validNumber(v, poses::kPi) || v < 0)
      return badParam("轴夹角容差须0..pi弧度", "axisTolerance");
  if (!std::isfinite(limit) || limit < 0 || limit > poses::kPi)
    return badParam("旋转容差须0..pi弧度", "angleTolerance");
  int ax, n;
  s = symmetry(p, ax, n);
  if (!s.ok)
    return s;
  if (pairs.size() * static_cast<std::size_t>(n) > 1000000)
    return resource();
  Json results = Json::array();
  bool matched = true;
  for (const auto [i, j] : pairs) {
    if (ctx.cancelled())
      return cancelled();
    const auto& aa = a.data["items"][i];
    const auto& bb = b.data["items"][j];
    auto qa = aa["rotation"].get<poses::Quaternion>(), qb = bb["rotation"].get<poses::Quaternion>();
    double best = 10;
    auto chosen = qb;
    if (p.flag("continuousSymmetry")) {
      auto rel = poses::multiply(inverse(qa), qb);
      const double phi = 2 * std::atan2(-rel[ax], rel[3]);
      chosen = poses::multiply(qb, turn(ax, phi));
      best = angular(qa, chosen);
    } else
      for (int k = 0; k < n; ++k) {
        auto candidate = poses::multiply(qb, turn(ax, 2 * poses::kPi * k / n));
        double d = angular(qa, candidate);
        if (d < best) {
          best = d;
          chosen = candidate;
        }
      }
    auto delta = sub(jvector(bb["position"]), jvector(aa["position"]));
    if (!finite(delta))
      return bad("位置差溢出");
    if (p.choice("translationFrame") == "local")
      delta = rotated(inverse(qa), delta);
    else if (p.choice("translationFrame") != "parent")
      return badParam("未知平移参考系", "translationFrame");
    V3 ad{};
    bool ok = best <= limit;
    for (int k = 0; k < 3; ++k) {
      auto axisA = rotated(qa, basis(k)), axisB = rotated(chosen, basis(k));
      if (!normalize(axisA) || !normalize(axisB))
        return bad("位姿轴方向退化");
      const auto crossAxis = cross(axisA, axisB);
      ad[k] = std::atan2(std::hypot(crossAxis[0], crossAxis[1], crossAxis[2]), dot(axisA, axisB));
      ok = ok && std::abs(delta[k]) <= tolerance[k] && ad[k] <= angles[k];
    }
    matched = matched && ok;
    results.push_back({{"id", aa["id"]},
                       {"otherId", bb["id"]},
                       {"translation", delta},
                       {"axisAngles", ad},
                       {"angle", best},
                       {"matched", ok},
                       {"relativeRotation", poses::multiply(inverse(qa), chosen)}});
  }
  Record result{"geometry.PoseComparison3D.v1",
                {{"frame", a.data["frame"]},
                 {"unit", "m"},
                 {"angleUnit", "rad"},
                 {"translationFrame", p.choice("translationFrame")},
                 {"matched", matched},
                 {"items", results}}};
  if (!bounded(result))
    return resource();
  if (!matched && p.flag("alarm"))
    return Status::Error(Phase::Execute, "tolerance_exceeded", "三维实例差超出声明容差");
  if (ctx.cancelled())
    return cancelled();
  out.set("result", Data::record(std::move(result)));
  out.set("matched", Data::record(values::boolean(matched)));
  return Status::Ok();
}

bool noncollinear(const std::vector<V3>& points) {
  if (points.size() < 3)
    return false;
  V3 first = points[0];
  double extent = 0;
  std::size_t far = 0;
  for (std::size_t i = 1; i < points.size(); ++i) {
    auto d = sub(points[i], first);
    double n = std::hypot(d[0], d[1], d[2]);
    if (n > extent) {
      extent = n;
      far = i;
    }
  }
  if (!(extent > 0) || !std::isfinite(extent))
    return false;
  auto d = mul(sub(points[far], first), 1 / extent);
  double maxCross = 0;
  for (auto point : points) {
    auto c = cross(d, mul(sub(point, first), 1 / extent));
    maxCross = std::max(maxCross, std::hypot(c[0], c[1], c[2]));
  }
  return maxCross > 1e-8;
}
bool fit(const std::vector<V3>& a, const std::vector<V3>& b, poses::Quaternion& q, V3& t) {
  if (!noncollinear(a) || !noncollinear(b))
    return false;
  V3 ca{}, cb{};
  for (std::size_t i = 0; i < a.size(); ++i) {
    ca = add(ca, mul(a[i], 1. / a.size()));
    cb = add(cb, mul(b[i], 1. / b.size()));
  }
  double scale = 0;
  for (auto point : a) {
    auto d = sub(point, ca);
    for (double v : d)
      scale = std::max(scale, std::abs(v));
  }
  for (auto point : b) {
    auto d = sub(point, cb);
    for (double v : d)
      scale = std::max(scale, std::abs(v));
  }
  if (!(scale > 0) || !std::isfinite(scale))
    return false;
  double s[3][3]{};
  for (std::size_t k = 0; k < a.size(); ++k) {
    auto x = mul(sub(a[k], ca), 1 / scale), y = mul(sub(b[k], cb), 1 / scale);
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j)
        s[i][j] += x[i] * y[j] / a.size();
  }
  double h[4][4]{};
  h[0][0] = s[0][0] + s[1][1] + s[2][2];
  h[0][1] = h[1][0] = s[1][2] - s[2][1];
  h[0][2] = h[2][0] = s[2][0] - s[0][2];
  h[0][3] = h[3][0] = s[0][1] - s[1][0];
  for (int i = 0; i < 3; ++i) {
    h[i + 1][i + 1] = 2 * s[i][i] - h[0][0];
    for (int j = i + 1; j < 3; ++j)
      h[i + 1][j + 1] = h[j + 1][i + 1] = s[i][j] + s[j][i];
  }
  double v[4][4]{};
  for (int i = 0; i < 4; ++i)
    v[i][i] = 1;
  for (int sweep = 0; sweep < 40; ++sweep)
    for (int i = 0; i < 4; ++i)
      for (int j = i + 1; j < 4; ++j) {
        if (std::abs(h[i][j]) < 1e-16)
          continue;
        const double angle = .5 * std::atan2(2 * h[i][j], h[j][j] - h[i][i]);
        double c = std::cos(angle), sn = std::sin(angle);
        for (int k = 0; k < 4; ++k) {
          double x = h[k][i], y = h[k][j];
          h[k][i] = c * x - sn * y;
          h[k][j] = sn * x + c * y;
        }
        for (int k = 0; k < 4; ++k) {
          double x = h[i][k], y = h[j][k];
          h[i][k] = c * x - sn * y;
          h[j][k] = sn * x + c * y;
          double vx = v[k][i], vy = v[k][j];
          v[k][i] = c * vx - sn * vy;
          v[k][j] = sn * vx + c * vy;
        }
      }
  int best = 0;
  for (int i = 1; i < 4; ++i)
    if (h[i][i] > h[best][best])
      best = i;
  double second = -std::numeric_limits<double>::infinity();
  for (int i = 0; i < 4; ++i)
    if (i != best)
      second = std::max(second, h[i][i]);
  if (h[best][best] - second <= 1e-12 * std::max(1., std::abs(h[best][best])))
    return false;
  q = {v[1][best], v[2][best], v[3][best], v[0][best]};
  if (!poses::normalizeQuaternion(q))
    return false;
  t = sub(cb, rotated(q, ca));
  return finite(t);
}
Status makeTemplate(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  Record r;
  auto s = list(in, "poses", r);
  if (!s.ok)
    return s;
  double error = p.number("tolerance");
  if (!validNumber(error) || error < 0)
    return badParam("模板容差须有限非负米", "tolerance");
  if (r.data["items"].size() < 2 || r.data["items"].size() > 256)
    return bad("刚体模板须2..256个有身份定位点");
  Json items = Json::array();
  for (const auto& item : r.data["items"]) {
    if (ctx.cancelled())
      return cancelled();
    const auto v = jvector(item["position"]);
    for (double x : v)
      if (!validNumber(x))
        return bad("模板坐标超出有限米制范围");
    items.push_back({{"id", item["id"]}, {"position", v}, {"tolerance", error}});
  }
  out.set("template", Data::record({"geometry.RigidTemplate3D.v1",
                                    {{"frame", r.data["frame"]},
                                     {"unit", "m"},
                                     {"pairTolerance", error},
                                     {"items", items}}}));
  return Status::Ok();
}
Status rigidEvaluate(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx,
                     const std::string& mode, bool requireTransform) {
  Record observed;
  auto s = list(in, "poses", observed);
  if (!s.ok)
    return s;
  const auto* ref = in.get("template").asRecord();
  std::string why;
  if (!ref || ref->type != "geometry.RigidTemplate3D.v1" || !bounded(*ref))
    return bad("需要有界RigidTemplate3D.v1", "template");
  const auto& d = ref->data;
  if (!d.is_object() || d.size() != 4 || d.value("unit", Json{}) != "m" || !d.contains("frame") ||
      !d["frame"].is_string() || d["frame"].get<std::string>().empty() || !d.contains("items") ||
      !d["items"].is_array() || d["items"].size() < 2 || d["items"].size() > 256 ||
      !d.contains("pairTolerance") || !values::safeNumber(d["pairTolerance"]) ||
      !validNumber(d["pairTolerance"].get<double>()) || d["pairTolerance"].get<double>() < 0)
    return bad("刚体模板结构、单位或容差非法", "template");
  if (d["items"].size() != observed.data["items"].size())
    return bad("模板和观测定位点数量不匹配");
  std::map<std::string, Json> obs;
  for (const auto& item : observed.data["items"])
    obs.emplace(item["id"].get<std::string>(), item);
  std::set<std::string> used;
  std::vector<V3> a, b;
  std::vector<double> tolerances;
  Json ids = Json::array();
  for (const auto& item : d["items"]) {
    if (!item.is_object() || item.size() != 3 || !item.contains("id") || !item["id"].is_string() ||
        !item.contains("position") || !poses::numericArray(item["position"], 3) ||
        !item.contains("tolerance") || !values::safeNumber(item["tolerance"]))
      return bad("模板点结构非法", "template");
    auto id = item["id"].get<std::string>();
    if (id.empty() || !used.insert(id).second || !obs.count(id))
      return bad("模板ID重复、为空或缺少观测", "template");
    auto source = jvector(obs[id]["position"]), target = jvector(item["position"]);
    double tol = item["tolerance"].get<double>();
    if (!validNumber(tol) || tol < 0)
      return bad("模板点容差非法", "template");
    for (double x : source)
      if (!validNumber(x))
        return bad("观测坐标超出预算");
    for (double x : target)
      if (!validNumber(x))
        return bad("模板坐标超出预算");
    a.push_back(source);
    b.push_back(target);
    tolerances.push_back(tol);
    ids.push_back(id);
  }
  Json residuals = Json::array();
  bool accepted = true, observable = true;
  Transform native;
  poses::Quaternion q{0, 0, 0, 1};
  V3 t{};
  double maximum = 0;
  if (mode == "distances") {
    for (std::size_t i = 0; i < a.size(); ++i) {
      if (ctx.cancelled())
        return cancelled();
      for (std::size_t j = i + 1; j < a.size(); ++j) {
        auto x = sub(a[i], a[j]), y = sub(b[i], b[j]);
        double measured = std::hypot(x[0], x[1], x[2]), expected = std::hypot(y[0], y[1], y[2]),
               error = std::abs(measured - expected);
        maximum = std::max(maximum, error);
        accepted = accepted && error <= d["pairTolerance"].get<double>();
        if (residuals.size() < 512)
          residuals.push_back({{"a", ids[i]},
                               {"b", ids[j]},
                               {"measured", measured},
                               {"expected", expected},
                               {"error", error}});
      }
    }
  } else if (mode == "registration") {
    observable = fit(a, b, q, t);
    accepted = observable;
    if (observable) {
      auto r = poses::rotationMatrix(q);
      for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j)
          native.m[i * 4 + j] = static_cast<float>(r[i * 3 + j]);
        if (std::abs(t[i]) > std::numeric_limits<float>::max())
          return bad("配准平移无法交付float32");
        native.m[i * 4 + 3] = static_cast<float>(t[i]);
      }
      M4 m;
      poses::Quaternion nq;
      if (!rigid(&native, m, nq, why))
        return bad("原生float32配准非刚体");
      for (std::size_t i = 0; i < a.size(); ++i) {
        auto delta = sub(pose_detail::apply(m, a[i]), b[i]);
        double error = std::hypot(delta[0], delta[1], delta[2]);
        maximum = std::max(maximum, error);
        accepted = accepted && error <= tolerances[i];
        residuals.push_back({{"id", ids[i]}, {"error", error}, {"tolerance", tolerances[i]}});
      }
    }
  } else
    return badParam("未知刚体检查模式", "mode");
  if (requireTransform && !observable)
    return Status::Error(Phase::Execute, "unobservable", "刚体定位点退化，不能交付有效变换");
  if (!accepted && p.flag("alarm"))
    return Status::Error(Phase::Execute, "rigid_check_failed",
                         observable ? "刚体残差超容差" : "刚体定位点退化不可观");
  Record result{
      "geometry.RigidCheck3D.v1",
      {{"sourceFrame", observed.data["frame"]},
       {"targetFrame", d["frame"]},
       {"unit", "m"},
       {"mode", mode},
       {"accepted", accepted},
       {"observable", observable},
       {"transformAvailable", mode == "registration" && observable},
       {"transform", mode == "registration" && observable ? Json(native.m) : Json(nullptr)},
       {"maximumError", observable ? Json(maximum) : Json(nullptr)},
       {"status",
        observable ? (accepted ? "within_tolerance" : "out_of_tolerance") : "unobservable"},
       {"residuals", residuals},
       {"residualCount", mode == "distances" ? a.size() * (a.size() - 1) / 2
                         : observable        ? a.size()
                                             : 0},
       {"residualsTruncated", mode == "distances" && a.size() * (a.size() - 1) / 2 > 512}}};
  if (!bounded(result))
    return resource();
  if (ctx.cancelled())
    return cancelled();
  out.set("result", Data::record(std::move(result)));
  out.set("accepted", Data::record(values::boolean(accepted)));
  if (requireTransform)
    out.set("transform", Data::transform(native));
  return Status::Ok();
}

Status rigidCheck(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  return rigidEvaluate(in, p, out, ctx, p.choice("mode"), false);
}
Status rigidRegister(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  return rigidEvaluate(in, p, out, ctx, "registration", true);
}

Status extend(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  const int ax = axis(p.choice("newAxis"));
  const double offset = p.number("offset"), pixelScale = p.number("pixelToMeters");
  const auto frame = p.text("frame");
  if (ax < 0 || !validNumber(offset) || !std::isfinite(pixelScale) || pixelScale <= 0 ||
      frame.empty())
    return badParam("轴、偏移、比例和目标帧必须合法", "frame");
  M4 m{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  poses::Quaternion tq{0, 0, 0, 1};
  std::string why;
  if (in.has("transform") && !rigid(in.get("transform").asTransform(), m, tq, why))
    return bad(why, "transform");
  const auto lift = [&](double x, double y, bool direction = false) {
    V3 v{};
    v[ax] = direction ? 0 : offset;
    int k = 0;
    for (int j = 0; j < 3; ++j)
      if (j != ax)
        v[j] = (k++ == 0 ? x : y);
    return pose_detail::apply(m, v, direction);
  };
  int count = 0;
  for (const char* name : {"poses", "point", "line", "circle", "features"})
    count += in.has(name) ? 1 : 0;
  if (count != 1)
    return bad("必须且只能连接一种二维输入");
  if (in.has("features")) {
    const auto* source = in.get("features").asRecord();
    if (!source || source->type != "geometry.Features2D.v1" || !bounded(*source))
      return bad("需要有界Features2D.v1集合", "features");
    const auto& data = source->data;
    if (!data.is_object() || data.size() != 3 || !data.contains("frame") ||
        !data["frame"].is_string() || data["frame"].get<std::string>().empty() ||
        !data.contains("unit") || (data["unit"] != "px" && data["unit"] != "m") ||
        !data.contains("items") || !data["items"].is_array())
      return bad("二维几何集合帧/单位/结构非法", "features");
    const double scale = data["unit"] == "px" ? pixelScale : 1;
    Json output = Json::array();
    std::set<std::string> ids;
    for (const auto& item : data["items"]) {
      if (ctx.cancelled())
        return cancelled();
      if (!item.is_object() || !item.contains("id") || !item["id"].is_string() ||
          item["id"].get<std::string>().empty() ||
          !ids.insert(item["id"].get<std::string>()).second || !item.contains("type") ||
          !item["type"].is_string() || !item.contains("position") ||
          !poses::numericArray(item["position"], 2))
        return bad("二维几何项ID/类型/位置非法", "features");
      const std::string type = item["type"];
      Json feature = item;
      auto metadataPose = poses::make2D("validation", "embedding", "m", {0, 0}, 0, {1, 1});
      for (const char* key :
           {"attributes", "name", "classId", "classLabel", "confidence", "pickable"})
        if (item.contains(key))
          metadataPose.data[key] = item[key];
      if (!poses::validate(metadataPose, why))
        return bad("二维几何元数据非法：" + why, "features");
      feature["position"] = lift(item["position"][0].get<double>() * scale,
                                 item["position"][1].get<double>() * scale);
      if (type == "line") {
        if (!item.contains("direction") || !poses::numericArray(item["direction"], 2) ||
            item.contains("start") != item.contains("end"))
          return bad("线方向或有限端点结构非法", "features");
        auto direction =
            lift(item["direction"][0].get<double>(), item["direction"][1].get<double>(), true);
        if (!normalize(direction))
          return bad("线方向必须非零有限", "features");
        feature["direction"] = direction;
        if (item.contains("start")) {
          if (!poses::numericArray(item["start"], 2) || !poses::numericArray(item["end"], 2))
            return bad("线段端点非法", "features");
          auto start = lift(item["start"][0].get<double>() * scale,
                            item["start"][1].get<double>() * scale),
               end =
                   lift(item["end"][0].get<double>() * scale, item["end"][1].get<double>() * scale);
          direction = sub(end, start);
          if (!normalize(direction))
            return bad("线段端点必须不同");
          feature["position"] = start;
          feature["start"] = start;
          feature["end"] = end;
          feature["direction"] = direction;
          feature["hasSegment"] = true;
        } else
          feature["hasSegment"] = false;
      } else if (type == "circle") {
        if (!item.contains("radius") || !values::safeNumber(item["radius"]) ||
            item["radius"].get<double>() <= 0)
          return bad("圆半径必须正有限", "features");
        feature["radius"] = item["radius"].get<double>() * scale;
        feature["normal"] = cross(lift(1, 0, true), lift(0, 1, true));
      } else if (type != "point")
        return bad("二维集合仅支持point/line/circle", "features");
      static const std::set<std::string> metadata{"id",         "type",       "position",
                                                  "attributes", "name",       "classId",
                                                  "classLabel", "confidence", "pickable"};
      for (auto it = item.begin(); it != item.end(); ++it)
        if (!metadata.count(it.key()) &&
            !(type == "line" &&
              (it.key() == "direction" || it.key() == "start" || it.key() == "end")) &&
            !(type == "circle" && it.key() == "radius"))
          return bad("二维几何项包含未知或不匹配字段", "features");
      if (feature.contains("pickable"))
        feature["pickable"] = false;
      output.push_back(std::move(feature));
    }
    Record result{
        "geometry.Features3D.v1",
        {{"frame", frame}, {"unit", "m"}, {"sourceFrame", data["frame"]}, {"items", output}}};
    if (!bounded(result))
      return bad("三维集合结果非有限或超预算");
    out.set("features", Data::record(std::move(result)));
    out.set("poses", Data::record(poses::make3DList(Json::array(), frame)));
    return Status::Ok();
  }
  if (in.has("poses")) {
    Record r;
    if (!poses::read2DList(in.get("poses"), r, why))
      return bad(why);
    const double scale = r.data["unit"] == "px" ? pixelScale : 1;
    Json items = Json::array();
    for (const auto& item : r.data["items"]) {
      if (ctx.cancelled())
        return cancelled();
      double a = item["angle"].get<double>();
      auto x = lift(std::cos(a), std::sin(a), true);
      auto y = lift(-std::sin(a), std::cos(a), true);
      auto z = cross(x, y);
      poses::Matrix3 rot{x[0], y[0], z[0], x[1], y[1], z[1], x[2], y[2], z[2]};
      auto center = lift(item["position"][0].get<double>() * scale,
                         item["position"][1].get<double>() * scale);
      auto record = poses::make3D(item["id"], frame, center, poses::quaternion(rot),
                                  {item["size"][0].get<double>() * scale,
                                   item["size"][1].get<double>() * scale, p.number("thickness")});
      for (const char* key : {"name", "classId", "classLabel", "confidence", "attributes"})
        if (item.contains(key))
          record.data[key] = item[key];
      unsafe(record.data);
      items.push_back(record.data);
    }
    auto status = put(out, "poses", poses::make3DList(std::move(items), frame));
    if (!status.ok)
      return status;
    out.set("features",
            Data::record({"geometry.Features3D.v1",
                          {{"frame", frame}, {"unit", "m"}, {"items", Json::array()}}}));
    return Status::Ok();
  }
  Json feature;
  double scale = 1;
  if (in.has("point")) {
    const auto* v = in.get("point").asPoint2D();
    if (!v || (v->unit != Unit2D::Meter && v->unit != Unit2D::Pixel))
      return bad("需要Point2D");
    scale = v->unit == Unit2D::Pixel ? pixelScale : 1;
    feature = {{"type", "point"}, {"position", lift(v->p[0] * scale, v->p[1] * scale)}};
  }
  if (in.has("line")) {
    const auto* v = in.get("line").asLine2D();
    if (!v || (v->unit != Unit2D::Meter && v->unit != Unit2D::Pixel))
      return bad("需要Line2D");
    scale = v->unit == Unit2D::Pixel ? pixelScale : 1;
    auto dir = lift(v->dir[0], v->dir[1], true);
    if (!normalize(dir))
      return bad("直线方向必须非零有限");
    feature = {{"type", "line"},
               {"position", lift(v->point[0] * scale, v->point[1] * scale)},
               {"direction", dir},
               {"hasSegment", v->hasSegment}};
    if (v->hasSegment) {
      auto start = lift(v->start[0] * scale, v->start[1] * scale),
           end = lift(v->end[0] * scale, v->end[1] * scale);
      auto d = sub(end, start);
      if (!normalize(d))
        return bad("线段端点必须不同");
      feature["start"] = start;
      feature["end"] = end;
      feature["position"] = start;
      feature["direction"] = d;
    }
  }
  if (in.has("circle")) {
    const auto* v = in.get("circle").asCircle2D();
    if (!v || (v->unit != Unit2D::Meter && v->unit != Unit2D::Pixel) || !std::isfinite(v->radius) ||
        v->radius <= 0)
      return bad("圆半径须正有限");
    scale = v->unit == Unit2D::Pixel ? pixelScale : 1;
    auto normal = cross(lift(1, 0, true), lift(0, 1, true));
    feature = {{"type", "circle"},
               {"position", lift(v->center[0] * scale, v->center[1] * scale)},
               {"normal", normal},
               {"radius", v->radius * scale}};
  }
  Record result{"geometry.Features3D.v1",
                {{"frame", frame}, {"unit", "m"}, {"items", Json::array({feature})}}};
  if (!bounded(result))
    return bad("升维结果包含非有限或超限数值");
  if (ctx.cancelled())
    return cancelled();
  out.set("features", Data::record(std::move(result)));
  out.set("poses", Data::record(poses::make3DList(Json::array(), frame)));
  return Status::Ok();
}
}

void registerP5PoseGeometry(Registry& r) {
  using namespace p5;
  auto op = descriptor("pose.flip_axis_3d", "三维有限旋转对称轴优化", "3D symmetry flip axis");
  op.inputs = {record("poses")};
  op.outputs = {record("poses")};
  op.params = {choice("symmetryAxis", "局部对称轴", "Z", {"X", "Y", "Z"}),
               integer("symmetryOrder", "显式对称阶数", 2),
               choice("alignAxis", "优化轴", "X", {"X", "Y", "Z"}),
               vec("direction", "父帧目标方向", {1, 0, 0})};
  op.compute = atomic(&flip);
  r.addOperator(std::move(op));
  op =
      descriptor("pose.rotate_axis_3d", "三维连续旋转对称轴优化", "3D cylinder rotate axis target");
  op.inputs = {record("poses")};
  op.outputs = {record("poses")};
  op.params = {flag("continuousSymmetry", "显式声明连续轴对称", false),
               choice("fixedAxis", "固定局部轴", "Z", {"X", "Y", "Z"}),
               choice("rotateAxis", "对齐局部轴", "X", {"X", "Y", "Z"}),
               choice("mode", "参考模式", "direction", {"direction", "point"}),
               vec("reference", "父帧方向或米制点", {1, 0, 0}),
               flag("useZ", "使用参考Z分量", true),
               flag("reverse", "反向", false)};
  op.compute = atomic(&rotate);
  r.addOperator(std::move(op));
  op = descriptor("pose.compare_3d", "三维跨帧位姿对称比较", "3D compare symmetry tolerance alarm");
  op.inputs = {record("a"), record("b"), port("bToA", "Transform", false)};
  op.outputs = {record("result"), scalarPort("matched", true)};
  op.params = {choice("pairing", "配对", "id", {"id", "index"}),
               choice("translationFrame", "位置差参考系", "parent", {"parent", "local"}),
               vec("positionTolerance", "XYZ容差米", {.001, .001, .001}),
               vec("axisTolerance", "XYZ轴夹角弧度", {.01, .01, .01}),
               number("angleTolerance", "总旋转容差", .01, "rad"),
               choice("symmetryAxis", "显式局部对称轴", "Z", {"X", "Y", "Z"}),
               integer("symmetryOrder", "显式对称阶数", 1),
               flag("continuousSymmetry", "连续轴对称", false),
               flag("alarm", "超差返回错误", false)};
  op.compute = atomic(&compare);
  r.addOperator(std::move(op));
  op = descriptor("pose.rigid_template", "刚体定位点模板记录", "rigid template register distances");
  op.inputs = {record("poses")};
  op.outputs = {record("template")};
  op.params = {number("tolerance", "米制公差", .001, "m")};
  op.compute = atomic(&makeTemplate);
  r.addOperator(std::move(op));
  op = descriptor("pose.rigid_check", "刚体多点距离与配准残差检查",
                  "rigid distances registration alarm");
  op.inputs = {record("poses"), record("template")};
  op.outputs = {record("result"), scalarPort("accepted", true)};
  op.params = {choice("mode", "检查模式", "distances", {"distances", "registration"}),
               flag("alarm", "异常返回错误", false)};
  op.compute = atomic(&rigidCheck);
  r.addOperator(std::move(op));
  op = descriptor("pose.rigid_register", "刚体定位点可观测配准",
                  "rigid observable proper registration transform residuals");
  op.inputs = {record("poses"), record("template")};
  op.outputs = {record("result"), scalarPort("accepted", true), port("transform", "Transform")};
  op.params = {flag("alarm", "残差超差返回错误", false)};
  op.compute = atomic(&rigidRegister);
  r.addOperator(std::move(op));
  op = descriptor("geometry.extend_2d", "二维几何与工件嵌入三维平面",
                  "2D 3D lift point line circle pose");
  op.inputs = {record("poses", false),
               record("features", false),
               port("point", "Point2D", false),
               port("line", "Line2D", false),
               port("circle", "Circle2D", false),
               port("transform", "Transform", false)};
  op.outputs = {record("poses"), record("features")};
  op.params = {
      choice("newAxis", "新增轴", "Z", {"X", "Y", "Z"}), number("offset", "新增轴坐标", 0, "m"),
      number("pixelToMeters", "显式像素至米比例", .001),
      number("thickness", "工件已知厚度", .001, "m"), text("frame", "输出参考帧", "world")};
  op.compute = atomic(&extend);
  r.addOperator(std::move(op));
}
}
