#pragma once

#include "pose_common.h"
#include <map>
#include <set>

namespace lyflow::packs::std_geometry::p5 {
using namespace pose_detail;
inline V3 add(V3 a, V3 b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
inline V3 mul(V3 a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
inline V3 vector(const ParamView& p, const char* name) {
  const auto found = p.raw().find(name);
  if (found != p.raw().end() && found->second.kind() == Value::Kind::FloatVec) {
    const auto& v = found->second.vecValue();
    if (v.size() == 3)
      return {v[0], v[1], v[2]};
  }
  const auto nan = std::numeric_limits<double>::quiet_NaN();
  return {nan, nan, nan};
}
inline V3 jvector(const Json& j) { return j.get<V3>(); }
inline int axis(const std::string& s) { return s == "X" ? 0 : s == "Y" ? 1 : s == "Z" ? 2 : -1; }
inline V3 basis(int i) {
  V3 a{};
  a[i] = 1;
  return a;
}
inline V3 rotated(const poses::Quaternion& q, V3 v) {
  const auto r = poses::rotationMatrix(q);
  return {productSum({{r[0], v[0]}, {r[1], v[1]}, {r[2], v[2]}}),
          productSum({{r[3], v[0]}, {r[4], v[1]}, {r[5], v[2]}}),
          productSum({{r[6], v[0]}, {r[7], v[1]}, {r[8], v[2]}})};
}
inline poses::Quaternion turn(int axis, double angle) {
  poses::Quaternion q{0, 0, 0, std::cos(angle / 2)};
  q[axis] = std::sin(angle / 2);
  return q;
}
inline poses::Quaternion inverse(poses::Quaternion q) {
  for (int i = 0; i < 3; ++i)
    q[i] = -q[i];
  return q;
}
inline double angular(poses::Quaternion a, poses::Quaternion b) {
  auto q = poses::multiply(inverse(a), b);
  poses::normalizeQuaternion(q);
  return 2 * std::atan2(std::hypot(q[0], q[1], q[2]), std::abs(q[3]));
}
inline Status list(const Inputs& in, const char* name, Record& r) {
  std::string why;
  if (!poses::read3DList(in.get(name), r, why))
    return bad(why, name);
  return Status::Ok();
}
inline bool validNumber(double v, double bound = 1e12) {
  return std::isfinite(v) && std::abs(v) <= bound;
}
inline Status checked(Record& r) {
  std::string why;
  return poses::validate(r, why) ? Status::Ok() : bad(why);
}
inline void unsafe(Json& item) { item["pickable"] = false; }
inline Status cancelled() { return Status::Error(Phase::Execute, "cancelled", "已取消"); }
inline Status resource() {
  return Status::Error(Phase::Execute, "resource_limit", "超过几何工作量或存储预算");
}
inline ComputeFn atomic(ComputeFn compute) {
  return [compute = std::move(compute)](const Inputs& in, const ParamView& p, Outputs& out,
                                        ExecContext& ctx) {
    if (ctx.cancelled())
      return cancelled();
    std::unordered_map<std::string, Data> pending;
    Outputs temporary(pending);
    try {
      auto status = compute(in, p, temporary, ctx);
      if (!status.ok)
        return status;
      if (ctx.cancelled())
        return cancelled();
      for (auto& item : pending)
        out.set(item.first, std::move(item.second));
      return Status::Ok();
    } catch (const std::bad_alloc&) {
      return resource();
    } catch (const Json::exception& e) {
      return bad(e.what());
    }
  };
}
inline bool bounded(const Record& r) {
  std::string why;
  return values::boundedJson(r.data, why, true);
}
inline Status transformList(const Inputs& in, const char* port, Record& r,
                            const std::string& target) {
  if (!in.has(port))
    return r.data["frame"] == target ? Status::Ok()
                                     : bad("跨帧必须显式提供 source→target Transform", port);
  M4 m;
  poses::Quaternion q;
  std::string why;
  if (!rigid(in.get(port).asTransform(), m, q, why))
    return bad(why, port);
  r.data["frame"] = target;
  for (auto& item : r.data["items"]) {
    item["frame"] = target;
    item["position"] = pose_detail::apply(m, jvector(item["position"]));
    item["rotation"] = poses::multiply(q, item["rotation"].get<poses::Quaternion>());
  }
  return checked(r);
}
inline Status paired(const Record& a, const Record& b, const std::string& mode,
                     std::vector<std::pair<std::size_t, std::size_t>>& pairs) {
  const auto& aa = a.data["items"];
  const auto& bb = b.data["items"];
  if (aa.size() != bb.size())
    return bad("配对列表长度必须相同");
  if (mode == "index") {
    for (std::size_t i = 0; i < aa.size(); ++i)
      pairs.emplace_back(i, i);
  } else if (mode == "id") {
    std::map<std::string, std::size_t> ids;
    for (std::size_t i = 0; i < bb.size(); ++i)
      ids.emplace(bb[i]["id"].get<std::string>(), i);
    for (std::size_t i = 0; i < aa.size(); ++i) {
      auto it = ids.find(aa[i]["id"].get<std::string>());
      if (it == ids.end())
        return bad("缺少相同ID的对应实例");
      pairs.emplace_back(i, it->second);
    }
  } else
    return badParam("未知配对模式", "pairing");
  return Status::Ok();
}
inline Port port(const char* name, const char* type, bool required = true) {
  return {name, type, name, "显式帧/单位及绑定见算子契约", required};
}
}
