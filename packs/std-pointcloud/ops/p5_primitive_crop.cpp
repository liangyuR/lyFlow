#include "p1_common.h"
#include "lyflow/graph_migration.h"
#include "lyflow/kernel/rigid.h"
#include <pcl/search/kdtree.h>
#include <pcl/point_types.h>

namespace lyflow::ops {
namespace {
namespace g {
using namespace lyflow::kernel;
inline Status bad(const std::string& text, const char* port = "poses") {
  return Status::Error(Phase::Execute, "bad_input", text, {}, port);
}
inline Status badParam(const std::string& text, const char* name) {
  return Status::Error(Phase::Execute, "bad_param", text, name);
}
inline Param text(const char* name, const char* label, const char* def = "") {
  Param p;
  p.name = name;
  p.label = label;
  p.type = ParamType::String;
  p.def = Value::text(def);
  return p;
}
inline Param number(const char* name, const char* label, double def = 0, const char* unit = "") {
  Param p;
  p.name = name;
  p.label = label;
  p.type = ParamType::Float;
  p.def = Value::number(def);
  p.unit = unit;
  return p;
}
inline Param integer(const char* name, const char* label, std::int64_t def = 0) {
  Param p = number(name, label);
  p.type = ParamType::Int;
  p.def = Value::integer(def);
  return p;
}
inline Param flag(const char* name, const char* label, bool def = false) {
  Param p = text(name, label);
  p.type = ParamType::Bool;
  p.def = Value::boolean(def);
  return p;
}
inline Param vec(const char* name, const char* label, std::vector<double> values) {
  Param p = text(name, label);
  p.type = values.size() == 2   ? ParamType::Vec2f
           : values.size() == 3 ? ParamType::Vec3f
                                : ParamType::Vec4f;
  p.def = Value::vec(std::move(values));
  return p;
}
inline Param choice(const char* name, const char* label, const char* def,
                    std::initializer_list<const char*> values) {
  Param p = text(name, label, def);
  p.type = ParamType::Enum;
  for (const char* value : values)
    p.options.push_back({value, value, ""});
  return p;
}
inline OperatorDesc descriptor(const char* id, const char* label, const char* keywords) {
  OperatorDesc op;
  op.id = id;
  op.version = "1.0.0";
  op.label = label;
  op.category = "几何/工件位姿";
  op.keywords = {keywords, label, "pose", "工件", "位姿", "坐标系"};
  op.capabilities = {true, false, true};
  return op;
}
inline Port record(const char* name, bool required = true) {
  return {name, "Record", name, "版本化 Record，具体载荷与单位见算子说明。", required};
}
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
inline Port port(const char* name, const char* type, bool required = true) {
  return {name, type, name, "显式帧/单位及绑定见算子契约", required};
}
}

using Json = nlohmann::json;
using Ids = std::vector<std::int32_t>;
constexpr const char* kSupportType = "cloud.PrimitiveSupport.v1";
constexpr std::size_t kPoints = 4U * 1024U * 1024U, kSupport = 16U * 1024U * 1024U;
constexpr std::uint64_t kWork = 64U * 1024U * 1024U;
struct Volume {
  g::V3 center, half;
  poses::Quaternion inverse;
  std::string shape;
};
bool member(const Volume& v, g::V3 p, bool closed) {
  p = g::rotated(v.inverse, g::sub(p, v.center));
  const auto le = [&](double a, double b) { return closed ? a <= b : a < b; };
  if (v.shape == "box")
    return le(std::abs(p[0]), v.half[0]) && le(std::abs(p[1]), v.half[1]) &&
           le(std::abs(p[2]), v.half[2]);
  if (v.shape == "cylinder")
    return le(std::hypot(p[0], p[1]), v.half[0]) && le(std::abs(p[2]), v.half[2]);
  return le(std::hypot(p[0], p[1], p[2]), v.half[0]);
}
Status prepare(const Inputs& in, const ParamView& p, const PointCloud*& cloud, Record& poses,
               std::vector<unsigned char>& valid) {
  cloud = in.get("cloud").asCloud();
  if (!cloud || !cloud->channelsConsistent())
    return g::bad("点云缺失或可选通道不对齐", "cloud");
  if (cloud->pointCount() > kPoints || in.get("cloud").byteSize() > 128U * 1024U * 1024U)
    return g::resource();
  if (p.text("frame").empty() || p.text("frame").size() > 1024)
    return g::badParam("点云参考帧不能为空", "frame");
  auto s = g::list(in, "poses", poses);
  if (!s.ok)
    return s;
  s = g::transformList(in, "posesToCloud", poses, p.text("frame"));
  if (!s.ok)
    return s;
  valid.resize(cloud->pointCount(), 1);
  for (std::size_t i = 0; i < cloud->pointCount(); ++i)
    for (int k = 0; k < 3; ++k) {
      const float v = cloud->xyz[i * 3 + k];
      if (std::isinf(v))
        return g::bad("点云不接受无穷坐标", "cloud");
      if (std::isfinite(v) && std::abs(v) > 1e12)
        return g::bad("点云坐标超出安全距离计算范围", "cloud");
      if (std::isnan(v))
        valid[i] = 0;
    }
  for (float v : cloud->intensity)
    if (!std::isfinite(v))
      return g::bad("强度必须有限", "cloud");
  for (float v : cloud->normals)
    if (!std::isfinite(v))
      return g::bad("法线必须有限", "cloud");
  return Status::Ok();
}
Status volumes(const Record& poses, const ParamView& p, std::vector<Volume>& result) {
  const auto shape = p.choice("shape");
  if (shape != "box" && shape != "cylinder" && shape != "sphere")
    return g::badParam("未知有限裁剪体", "shape");
  const bool override = p.choice("dimensions") == "override";
  if (!override && p.choice("dimensions") != "pose")
    return g::badParam("未知尺寸来源", "dimensions");
  const auto extra = g::vector(p, "padding");
  for (double x : extra)
    if (!g::validNumber(x) || x < 0)
      return g::badParam("padding是有限非负的每侧米制外扩", "padding");
  for (const auto& item : poses.data["items"]) {
    auto size = override ? g::vector(p, "size") : g::jvector(item["size"]);
    auto center = g::jvector(item["position"]);
    auto q = item["rotation"].get<poses::Quaternion>();
    if (p.flag("heightUnknown"))
      size[2] = p.number("maxHeight");
    for (double x : size)
      if (!g::validNumber(x) || x <= 0)
        return g::badParam("有限体三维全尺寸必须为正有限米", "size");
    if (shape == "cylinder" && std::abs(size[0] - size[1]) > 1e-10 * std::max(size[0], size[1]))
      return g::bad("圆柱X/Y直径必须相同");
    if (shape == "sphere" && (std::abs(size[0] - size[1]) > 1e-10 * std::max(size[0], size[1]) ||
                              std::abs(size[0] - size[2]) > 1e-10 * std::max(size[0], size[2])))
      return g::bad("球体三轴直径必须相同");
    if (p.choice("anchor") == "top")
      center = g::sub(center, g::rotated(q, {0, 0, size[2] * .5}));
    else if (p.choice("anchor") != "center")
      return g::badParam("锚点必须center或top", "anchor");
    auto half = g::add(g::mul(size, .5), extra);
    if (!g::finite(center) || !g::finite(half))
      return g::bad("裁剪体几何溢出");
    for (double x : center)
      if (!g::validNumber(x))
        return g::bad("裁剪体坐标超出安全计算范围");
    if (shape == "cylinder" && extra[0] != extra[1])
      return g::badParam("圆柱径向外扩必须X/Y一致", "padding");
    if (shape == "sphere" && (extra[0] != extra[1] || extra[0] != extra[2]))
      return g::badParam("球体外扩必须三轴一致", "padding");
    result.push_back({center, half, g::inverse(q), shape});
  }
  return Status::Ok();
}
g::V3 point(const PointCloud& c, std::size_t i) {
  return {c.xyz[i * 3], c.xyz[i * 3 + 1], c.xyz[i * 3 + 2]};
}
Status query(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  const PointCloud* cloud = nullptr;
  Record poses;
  std::vector<unsigned char> valid;
  auto s = prepare(in, p, cloud, poses, valid);
  if (!s.ok)
    return s;
  const auto n = cloud->pointCount();
  const auto count = poses.data["items"].size();
  if (count && n > kWork / count)
    return g::resource();
  if (ctx.cancelled())
    return g::cancelled();
  std::vector<unsigned char> selected(n, 0);
  Indices groupIndices{cloud->id, {}};
  std::vector<cloud_groups::Group> groups;
  Json observations = Json::array();
  const auto observe = [&](Json pose, std::size_t offset) {
    g::unsafe(pose);
    const auto support = groupIndices.values.size() - offset;
    groups.push_back({pose.at("id").get<std::string>(), offset, support});
    observations.push_back({{"pose", std::move(pose)}, {"count", support}});
  };
  if (p.choice("mode") == "model_radius") {
    const auto* model = in.has("model") ? in.get("model").asCloud() : nullptr;
    if (!model || !model->channelsConsistent() || model->pointCount() == 0 ||
        model->pointCount() > kPoints)
      return g::bad("model_radius需要非空本地米制采样模型", "model");
    const double radius = p.number("radius");
    if (!g::validNumber(radius, 1e6) || radius < 1e-12)
      return g::badParam("半径须1e-12..1e6米", "radius");
    if (count && model->pointCount() > kWork / count)
      return g::resource();
    auto scene = pcl::make_shared<pcl::PointCloud<pcl::PointXYZ>>();
    Ids original;
    scene->reserve(n);
    original.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
      if (valid[i]) {
        scene->push_back(
            pcl::PointXYZ(cloud->xyz[3 * i], cloud->xyz[3 * i + 1], cloud->xyz[3 * i + 2]));
        original.push_back(static_cast<std::int32_t>(i));
      }
    for (float x : model->xyz)
      if (!g::validNumber(x))
        return g::bad("采样模型坐标须有限", "model");
    pcl::search::KdTree<pcl::PointXYZ> tree(false);
    if (!scene->empty())
      tree.setInputCloud(scene);
    std::uint64_t work = 0;
    for (const auto& item : poses.data["items"]) {
      if (ctx.cancelled())
        return g::cancelled();
      const auto q = item["rotation"].get<poses::Quaternion>();
      const auto t = g::jvector(item["position"]);
      std::vector<unsigned char> local(n, 0);
      for (std::size_t j = 0; j < model->pointCount(); ++j) {
        if ((j & 255) == 0 && ctx.cancelled())
          return g::cancelled();
        const auto query = g::add(g::rotated(q, point(*model, j)), t);
        for (double x : query)
          if (!g::validNumber(x))
            return g::bad("变换后的模型超出float32");
        const pcl::PointXYZ f(static_cast<float>(query[0]), static_cast<float>(query[1]),
                              static_cast<float>(query[2]));
        if (scene->empty())
          continue;
        Ids hits;
        std::vector<float> distances;
        tree.radiusSearch(f, radius, hits, distances);
        if (work > kWork - hits.size())
          return g::resource();
        work += hits.size();
        for (auto hit : hits) {
          const auto source = static_cast<std::size_t>(original[hit]);
          const auto delta = g::sub(point(*cloud, source), {f.x, f.y, f.z});
          if (std::hypot(delta[0], delta[1], delta[2]) <= radius) {
            selected[source] = 1;
            local[source] = 1;
          }
        }
      }
      const auto offset = groupIndices.values.size();
      for (std::size_t i = 0; i < n; ++i)
        if (local[i]) {
          if (groupIndices.values.size() >= kSupport)
            return g::resource();
          groupIndices.values.push_back(static_cast<std::int32_t>(i));
        }
      observe(item, offset);
    }
  } else {
    std::vector<Volume> vs;
    s = volumes(poses, p, vs);
    if (!s.ok)
      return s;
    for (std::size_t j = 0; j < vs.size(); ++j) {
      const auto offset = groupIndices.values.size();
      for (std::size_t i = 0; i < n; ++i) {
        if ((i & 8191) == 0 && ctx.cancelled())
          return g::cancelled();
        if (valid[i] && member(vs[j], point(*cloud, i), p.flag("includeBoundary"))) {
          selected[i] = 1;
          if (groupIndices.values.size() >= kSupport)
            return g::resource();
          groupIndices.values.push_back(static_cast<std::int32_t>(i));
        }
      }
      auto item = poses.data["items"][j];
      auto originalPose = item;
      if (originalPose.contains("attributes"))
        originalPose["attributes"].erase("sourcePose3D");
      item["attributes"]["sourcePose3D"] = originalPose;
      item["position"] = vs[j].center;
      item["size"] = g::mul(vs[j].half, 2);
      item["attributes"]["shape"] = vs[j].shape;
      for (const char* key : {"circleRadius", "radius", "normal"})
        item["attributes"].erase(key);
      observe(std::move(item), offset);
    }
  }
  Ids inner, outer, invalid;
  inner.reserve(n);
  outer.reserve(n);
  std::size_t validCount = 0;
  Tensor validity;
  validity.shape = {static_cast<std::int64_t>(n)};
  validity.data.resize(n);
  for (std::size_t i = 0; i < n; ++i) {
    if (!valid[i])
      invalid.push_back(static_cast<std::int32_t>(i));
    else
      ++validCount;
    (selected[i] ? inner : outer).push_back(static_cast<std::int32_t>(i));
    validity.data[i] = valid[i] && !selected[i] ? 1.f : 0.f;
  }
  Record support{kSupportType,
                 {{"frame", p.text("frame")},
                  {"unit", "m"},
                  {"basis", "observed_cloud_point_count"},
                  {"sourceCloudId", std::to_string(cloud->id)},
                  {"pointCount", n},
                  {"validCount", validCount},
                  {"unknownCount", invalid.size()},
                  {"selectedCount", inner.size()},
                  {"items", std::move(observations)},
                  {"boundary", p.flag("includeBoundary") ? "closed" : "open"}}};
  auto groupRecord = cloud_groups::make(*cloud, groups, p.text("frame"));
  std::string why;
  if (!values::boundedJson(support.data, why, true) || !g::bounded(groupRecord))
    return g::resource();
  out.set("innerIndices", Data::indices({cloud->id, std::move(inner)}));
  out.set("outerIndices", Data::indices({cloud->id, std::move(outer)}));
  out.set("invalidIndices", Data::indices({cloud->id, std::move(invalid)}));
  out.set("groupIndices", Data::indices(std::move(groupIndices)));
  out.set("groups", Data::record(std::move(groupRecord)));
  out.set("validity", Data::tensor(std::move(validity)));
  out.set("support", Data::record(std::move(support)));
  return Status::Ok();
}

Status classify(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  const auto& support = *in.get("support").asRecord();
  const auto& data = support.data;
  std::string why;
  if (support.type != kSupportType || !values::boundedJson(data, why, true) || !data.is_object() ||
      !poses::textField(data, "frame") || !poses::textField(data, "sourceCloudId") ||
      data.value("unit", Json()) != "m" ||
      data.value("basis", Json()) != "observed_cloud_point_count" ||
      (data.value("boundary", Json()) != "open" && data.value("boundary", Json()) != "closed") ||
      !data.contains("pointCount") || !cloud_groups::nonnegativeInteger(data["pointCount"]) ||
      !data.contains("unknownCount") || !cloud_groups::nonnegativeInteger(data["unknownCount"]) ||
      !data.contains("validCount") || !cloud_groups::nonnegativeInteger(data["validCount"]) ||
      !data.contains("selectedCount") || !cloud_groups::nonnegativeInteger(data["selectedCount"]) ||
      !data.contains("items") || !data["items"].is_array())
    return g::bad("需要有效的几何体点云支持记录", "support");
  const auto& frame = data["frame"].get_ref<const std::string&>();
  const auto pointCount = data["pointCount"].get<std::size_t>();
  const auto unknownCount = data["unknownCount"].get<std::size_t>();
  const auto validCount = data["validCount"].get<std::size_t>();
  const auto selectedCount = data["selectedCount"].get<std::size_t>();
  if (unknownCount > pointCount || validCount != pointCount - unknownCount ||
      selectedCount > validCount)
    return g::bad("几何体支持记录中的总点数与有效点数不一致", "support");

  const auto maxCount = static_cast<std::size_t>(p.integer("maxCount"));
  Json free = Json::array(), collision = Json::array(), uncertain = Json::array();
  Json report = Json::array();
  std::set<std::string> ids;
  for (const auto& observation : data["items"]) {
    if (ctx.cancelled())
      return g::cancelled();
    if (!observation.is_object() || !observation.contains("pose") ||
        !observation.contains("count") || !cloud_groups::nonnegativeInteger(observation["count"]))
      return g::bad("几何体支持项需要位姿和点数", "support");
    const auto& pose = observation["pose"];
    const auto count = observation["count"].get<std::size_t>();
    if (count > selectedCount || !poses::validateItem(pose, 3, why) || pose["frame"] != frame ||
        !ids.insert(pose["id"].get<std::string>()).second)
      return g::bad("几何体支持项的位姿、身份或点数无效", "support");
    const bool hit = count > maxCount;
    const bool unobserved = unknownCount != 0 || count == 0;
    auto item = pose;
    g::unsafe(item);
    (hit ? collision : unobserved ? uncertain : free).push_back(std::move(item));
    report.push_back({{"id", pose["id"]},
                      {"count", count},
                      {"collision", hit},
                      {"state", hit          ? "collision"
                                : unobserved ? "unknown"
                                             : "free"}});
  }
  Record quality{"cloud.PrimitiveCrop.v1", data};
  quality.data["items"] = std::move(report);
  quality.data["physicalCollisionSafetyVerified"] = false;
  out.set("freePoses", Data::record(poses::make3DList(std::move(free), frame)));
  out.set("collisionPoses", Data::record(poses::make3DList(std::move(collision), frame)));
  out.set("unknownPoses", Data::record(poses::make3DList(std::move(uncertain), frame)));
  out.set("quality", Data::record(std::move(quality)));
  return Status::Ok();
}

Status removeIndices(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  const auto& cloud = *in.get("cloud").asCloud();
  const auto& indices = *in.get("indices").asIndices();
  if (indices.sourceCloudId != cloud.id)
    return g::bad("移除索引必须绑定当前源点云", "indices");
  for (const auto i : indices.values)
    if (i < 0 || static_cast<std::size_t>(i) >= cloud.pointCount())
      return g::bad("移除索引越界", "indices");
  if (p.choice("outputMode") == "compact")
    out.set("cloud", Data::cloud(cloud.selectInverse(indices.values)));
  else {
    PointCloud result = cloud;
    result.id = PointCloud{}.id;
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    Ticker ticker(ctx, indices.values.size());
    for (std::size_t k = 0; k < indices.values.size(); ++k) {
      if (ticker.tick(k))
        return g::cancelled();
      const auto offset = 3 * static_cast<std::size_t>(indices.values[k]);
      std::fill_n(result.xyz.begin() + offset, 3, nan);
    }
    out.set("cloud", Data::cloud(std::move(result)));
  }
  return Status::Ok();
}

Port supportPort() {
  return withContract(Port{"support", "Record", "几何体支持",
                           "几何体位姿、命中数及无效观测，计数判定可复用。", true},
                      {{"recordType", kSupportType}});
}

void migratePrimitiveCrop(Json& level, const Json& rule, Json* outer) {
  using namespace graph_migration;
  splitRuleKeys(rule, {"kind", "source"});
  const auto id = text(rule.at("source"), "node");
  const auto& nodes = level.at("nodes");
  const auto index = nodeIndex(nodes, id);
  require(index < nodes.size(), "旧裁云节点不存在: " + id);
  const bool remove = nodes[index].value("op", "") == "cloud.remove_primitives";
  const auto& source =
      splitSource(level, rule, remove ? "cloud.remove_primitives" : "cloud.crop_primitives");
  std::string extract = id + "__extract", check = id + "__classify", apply = id + "__remove";
  for (auto* derived : {&extract, &check, &apply})
    while (nodeIndex(nodes, *derived) != nodes.size())
      *derived += "_";

  Json replacements = Json::array(
      {{{"id", id}, {"op", "cloud.query_primitives"}, {"opVersion", "1.0.0"}},
       {{"id", extract}, {"op", "segment.extract_indices"}, {"opVersion", "1.0.0"}},
       {{"id", check}, {"op", "cloud.classify_primitive_hits"}, {"opVersion", "1.0.0"}}});
  SplitRoutes inputs{
      {"cloud", {{id, "cloud"}, {extract, "cloud"}}},
      {"poses", {{id, "poses"}}},
      {"posesToCloud", {{id, "posesToCloud"}}},
  };
  SplitRoutes outputs{{"cloud", {{extract, "rest"}}},
                      {"inner", {{extract, "selected"}}},
                      {"outer", {{extract, "rest"}}}};
  for (const char* name :
       {"innerIndices", "outerIndices", "invalidIndices", "groupIndices", "groups", "validity"})
    outputs[name] = {{id, name}};
  for (const char* name : {"freePoses", "collisionPoses", "unknownPoses", "quality"})
    outputs[name] = {{check, name}};
  SplitRoutes params{{"maxCount", {{check, "maxCount"}}}};
  for (const char* name : {"frame", "shape", "dimensions", "size", "heightUnknown", "maxHeight",
                           "padding", "anchor", "includeBoundary"})
    params[name] = {{id, name}};
  Json links = Json::array({{{"from", {{"node", id}, {"port", "innerIndices"}}},
                             {"to", {{"node", extract}, {"port", "indices"}}}},
                            {{"from", {{"node", id}, {"port", "support"}}},
                             {"to", {{"node", check}, {"port", "support"}}}}});
  if (remove) {
    inputs["model"] = {{id, "model"}};
    params["mode"] = {{id, "mode"}};
    params["radius"] = {{id, "radius"}};
    const auto outputMode = source.value("params", Json::object()).value("outputMode", "compact");
    if (outputMode == "compact" && parameterBindings(level, outer, id, "outputMode").empty()) {
      params["outputMode"] = {};
    } else {
      replacements.push_back(
          {{"id", apply}, {"op", "cloud.remove_indices"}, {"opVersion", "1.0.0"}});
      inputs["cloud"].push_back({apply, "cloud"});
      outputs["cloud"] = {{apply, "cloud"}};
      params["outputMode"] = {{apply, "outputMode"}};
      links.push_back({{"from", {{"node", id}, {"port", "innerIndices"}}},
                       {"to", {{"node", apply}, {"port", "indices"}}}});
    }
  }
  splitNode(level, outer, id, std::move(replacements), inputs, outputs, params, std::move(links));
}

} // namespace

void registerP5PrimitiveCrop(Registry& r) {
  auto op = g::descriptor("cloud.query_primitives", "查询有限几何体的点云命中",
                          "primitive box cylinder sphere query model radius");
  op.doc = "输出按几何体分组的命中、并集与无效观测。所有索引绑定源点云。";
  op.inputs = {g::port("cloud", "PointCloud"), g::record("poses"),
               g::port("posesToCloud", "Transform", false), g::port("model", "PointCloud", false)};
  op.outputs = {g::port("innerIndices", "Indices"),
                g::port("outerIndices", "Indices"),
                g::port("invalidIndices", "Indices"),
                g::port("groupIndices", "Indices"),
                g::record("groups"),
                g::port("validity", "Tensor"),
                supportPort()};
  op.params = {g::text("frame", "点云米制参考帧", "world"),
               g::choice("shape", "有限几何", "box", {"box", "cylinder", "sphere"}),
               g::choice("dimensions", "尺寸来源", "pose", {"pose", "override"}),
               g::vec("size", "覆盖全尺寸米", {1, 1, 1}),
               g::flag("heightUnknown", "显式声明未知高度", false),
               g::number("maxHeight", "未知高度替代米", 1, "m"),
               g::vec("padding", "每侧外扩米", {0, 0, 0}),
               g::choice("anchor", "输入位姿锚点", "center", {"center", "top"}),
               g::flag("includeBoundary", "包含几何表面", true),
               g::choice("mode", "查询依据", "shape", {"shape", "model_radius"}),
               g::number("radius", "采样模型半径", .001, "m")};
  op.compute = g::atomic(query);
  r.addOperator(std::move(op));

  op = g::descriptor("cloud.classify_primitive_hits", "按几何体命中点数分类",
                     "observed point support collision classification");
  op.doc = "命中数严格超过上限时为碰撞；含无效观测或零支持时为未知，其余为可见空闲。";
  op.inputs = {supportPort()};
  op.outputs = {g::record("freePoses"), g::record("collisionPoses"), g::record("unknownPoses"),
                g::record("quality")};
  auto maxCount = g::integer("maxCount", "碰撞点数阈值");
  maxCount.min = 0;
  op.params = {std::move(maxCount)};
  op.compute = g::atomic(classify);
  r.addOperator(std::move(op));

  op = g::descriptor("cloud.remove_indices", "按原云索引移除点",
                     "remove point cloud indices compact preserve order");
  op.doc = "紧凑输出保留点，或保留源点序并将移除点的 XYZ 置为 NaN。保留已有通道。";
  op.inputs = {g::port("cloud", "PointCloud"), g::port("indices", "Indices")};
  op.outputs = {g::port("cloud", "PointCloud")};
  op.params = {g::choice("outputMode", "输出点序", "compact", {"compact", "preserve_order"})};
  op.compute = g::atomic(removeIndices);
  r.addOperator(std::move(op));

  r.addGraphMigration({"primitiveCrop.v1", {}, migratePrimitiveCrop});
  for (const char* id : {"cloud.crop_primitives", "cloud.remove_primitives"})
    r.addRetiredOperator(
        id, "请用 lyflow migrate 的 primitiveCrop.v1 规则拆成查询、索引应用和计数分类");
}
} // namespace lyflow::ops
