#pragma once

#include "pose_common.h"
#include <map>
#include <set>

namespace lyflow::packs::std_geometry::p5 {
using namespace pose_detail;
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
inline Status list(const Inputs& in, const char* name, Record& r) {
  std::string why;
  if (!poses::read3DList(in.get(name), r, why))
    return bad(why, name);
  return Status::Ok();
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
inline Status transformList(const Inputs& in, const char* port, Record& r,
                            const std::string& target) {
  if (in.has(port) && !in.get(port).asTransform())
    return bad("需要原生 Transform", port);
  std::string why;
  bool invalidResult = false;
  return kernel::transformPoses3D(r, in.has(port) ? in.get(port).asTransform() : nullptr, target,
                                  why, &invalidResult)
             ? Status::Ok()
             : bad(why, invalidResult ? "poses" : port);
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
