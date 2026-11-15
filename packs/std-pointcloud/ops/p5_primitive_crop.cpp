#include "p1_common.h"
#include "../../std-geometry/ops/p5_pose_geometry_common.h"
#include <pcl/search/kdtree.h>
#include <pcl/point_types.h>

namespace lyflow::ops {
namespace {
namespace g = lyflow::packs::std_geometry::p5;
using Json = nlohmann::json;
using Ids = std::vector<std::int32_t>;
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
Status crop(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx, bool remove) {
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
  Json report = Json::array();
  Json free = Json::array(), collision = Json::array(), uncertain = Json::array();
  const bool unknown = std::find(valid.begin(), valid.end(), 0) != valid.end();
  const auto max = p.integer("maxCount");
  if (max < 0)
    return g::badParam("碰撞点数阈值必须非负", "maxCount");
  if (remove && p.choice("mode") == "model_radius") {
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
      auto pose = item;
      g::unsafe(pose);
      const auto support = groupIndices.values.size() - offset;
      const bool hit = support > static_cast<std::size_t>(max);
      const bool unobserved = unknown || support == 0;
      (hit ? collision : unobserved ? uncertain : free).push_back(pose);
      groups.push_back({pose["id"].get<std::string>(), offset, support});
      report.push_back({{"id", pose["id"]},
                        {"count", support},
                        {"collision", hit},
                        {"state", hit          ? "collision"
                                  : unobserved ? "unknown"
                                               : "free"}});
    }
  } else {
    if (remove && p.choice("mode") != "shape")
      return g::badParam("未知移除模式", "mode");
    if (count && n > kWork / count)
      return g::resource();
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
      const auto insideCount = groupIndices.values.size() - offset;
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
      g::unsafe(item);
      const bool hit = insideCount > static_cast<std::size_t>(max);
      const bool unobserved = unknown || insideCount == 0;
      (hit ? collision : unobserved ? uncertain : free).push_back(item);
      groups.push_back({item["id"].get<std::string>(), offset, insideCount});
      report.push_back({{"id", item["id"]},
                        {"count", insideCount},
                        {"collision", hit},
                        {"state", hit          ? "collision"
                                  : unobserved ? "unknown"
                                               : "free"}});
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
  auto inside = cloud->select(inner), outside = cloud->select(outer);
  PointCloud result;
  if (remove && p.choice("outputMode") == "preserve_order") {
    result = *cloud;
    result.id = PointCloud{}.id;
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    for (auto i : inner)
      for (int k = 0; k < 3; ++k)
        result.xyz[3 * i + k] = nan;
  } else if (!remove || p.choice("outputMode") == "compact")
    result = outside;
  else
    return g::badParam("未知输出模式", "outputMode");
  Record quality{"cloud.PrimitiveCrop.v1",
                 {{"frame", p.text("frame")},
                  {"unit", "m"},
                  {"basis", "observed_cloud_point_count"},
                  {"physicalCollisionSafetyVerified", false},
                  {"sourceCloudId", std::to_string(cloud->id)},
                  {"pointCount", n},
                  {"validCount", validCount},
                  {"unknownCount", invalid.size()},
                  {"selectedCount", inner.size()},
                  {"items", report},
                  {"boundary", p.flag("includeBoundary") ? "closed" : "open"}}};
  auto groupRecord = cloud_groups::make(*cloud, groups, p.text("frame"));
  if (!g::bounded(quality) || !g::bounded(groupRecord))
    return g::resource();
  auto freePoses = poses::make3DList(free, p.text("frame")),
       collisionPoses = poses::make3DList(collision, p.text("frame"));
  s = g::checked(freePoses);
  if (!s.ok)
    return s;
  s = g::checked(collisionPoses);
  if (!s.ok)
    return s;
  if (ctx.cancelled())
    return g::cancelled();
  auto unknownPoses = poses::make3DList(uncertain, p.text("frame"));
  s = g::checked(unknownPoses);
  if (!s.ok)
    return s;
  out.set("unknownPoses", Data::record(std::move(unknownPoses)));
  out.set("cloud", Data::cloud(std::move(result)));
  out.set("inner", Data::cloud(std::move(inside)));
  out.set("outer", Data::cloud(std::move(outside)));
  out.set("innerIndices", Data::indices({cloud->id, std::move(inner)}));
  out.set("outerIndices", Data::indices({cloud->id, std::move(outer)}));
  out.set("invalidIndices", Data::indices({cloud->id, std::move(invalid)}));
  out.set("groupIndices", Data::indices(std::move(groupIndices)));
  out.set("groups", Data::record(std::move(groupRecord)));
  out.set("freePoses", Data::record(std::move(freePoses)));
  out.set("collisionPoses", Data::record(std::move(collisionPoses)));
  out.set("validity", Data::tensor(std::move(validity)));
  out.set("quality", Data::record(std::move(quality)));
  return Status::Ok();
}
}
void registerP5PrimitiveCrop(Registry& r) {
  for (bool remove : {false, true}) {
    auto op = g::descriptor(remove ? "cloud.remove_primitives" : "cloud.crop_primitives",
                            remove ? "按工件有限体或模型移除点云" : "按工件有限体裁云与碰撞检查",
                            "primitive box cylinder sphere crop remove collision");
    op.inputs = {g::port("cloud", "PointCloud"), g::record("poses"),
                 g::port("posesToCloud", "Transform", false)};
    if (remove)
      op.inputs.push_back(g::port("model", "PointCloud", false));
    op.outputs = {g::port("cloud", "PointCloud"),
                  g::port("inner", "PointCloud"),
                  g::port("outer", "PointCloud"),
                  g::port("innerIndices", "Indices"),
                  g::port("outerIndices", "Indices"),
                  g::port("invalidIndices", "Indices"),
                  g::port("groupIndices", "Indices"),
                  g::record("groups"),
                  g::record("freePoses"),
                  g::record("collisionPoses"),
                  g::record("unknownPoses"),
                  g::port("validity", "Tensor"),
                  g::record("quality")};
    op.params = {g::text("frame", "点云米制参考帧", "world"),
                 g::choice("shape", "有限几何", "box", {"box", "cylinder", "sphere"}),
                 g::choice("dimensions", "尺寸来源", "pose", {"pose", "override"}),
                 g::vec("size", "覆盖全尺寸米", {1, 1, 1}),
                 g::flag("heightUnknown", "显式声明未知高度", false),
                 g::number("maxHeight", "未知高度替代米", 1, "m"),
                 g::vec("padding", "每侧外扩米", {0, 0, 0}),
                 g::choice("anchor", "输入位姿锚点", "center", {"center", "top"}),
                 g::flag("includeBoundary", "包含几何表面", true),
                 g::integer("maxCount", "碰撞点数阈值", 0)};
    if (remove) {
      op.params.push_back(g::choice("mode", "移除依据", "shape", {"shape", "model_radius"}));
      op.params.push_back(g::number("radius", "采样模型半径", .001, "m"));
      op.params.push_back(
          g::choice("outputMode", "输出点序", "compact", {"compact", "preserve_order"}));
    }
    op.compute = g::atomic([remove](const Inputs& in, const ParamView& p, Outputs& out,
                                    ExecContext& ctx) { return crop(in, p, out, ctx, remove); });
    r.addOperator(std::move(op));
  }
}
}
