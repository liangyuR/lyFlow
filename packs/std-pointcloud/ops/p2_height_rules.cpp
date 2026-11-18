#include <Eigen/Eigenvalues>
#include <map>
#include <random>
#include <set>
#include <tuple>

#include "p1_common.h"
#include "lyflow/graph_migration.h"
#include "lyflow/pose.h"

namespace lyflow::ops {
namespace {
using namespace p1;
using Json = nlohmann::json;
using Ids = std::vector<std::int32_t>;
constexpr std::size_t kPoints = 1000000;
constexpr std::uint64_t kWork = 100000000;

Status limit() { return error("超过点数、工作量或结果预算", "cloud", "resource_limit"); }
bool charge(std::uint64_t& work, std::uint64_t count) {
  if (work > kWork || count > kWork - work) {
    work = kWork + 1;
    return false;
  }
  work += count;
  return true;
}
Ids indices(const PointCloud& c) {
  Ids ids(c.pointCount());
  std::iota(ids.begin(), ids.end(), 0);
  return ids;
}
Vec3 vec(const ParamView& p, const char* name) {
  const auto a = p.vec3(name);
  return Vec3(a[0], a[1], a[2]);
}
Mat3 rotation(const Json& item) {
  const auto a = poses::rotationMatrix(item["rotation"].get<poses::Quaternion>());
  Mat3 r;
  for (int i = 0; i < 9; ++i)
    r(i / 3, i % 3) = a[i];
  return r;
}
Vec3 jvec(const Json& v) {
  return Vec3(v[0].get<double>(), v[1].get<double>(), v[2].get<double>());
}
poses::Quaternion quaternion(const Mat3& r) {
  poses::Matrix3 a;
  for (int i = 0; i < 9; ++i)
    a[i] = r(i / 3, i % 3);
  return poses::quaternion(a);
}
bool inside(const Vec3& p, const Json& roi) {
  const Vec3 q = rotation(roi).transpose() * (p - jvec(roi["position"]));
  return (q.cwiseAbs().array() <= (jvec(roi["size"]) * .5).array()).all();
}
Status poseInput(const Inputs& in, const char* port, const std::string& frame, Record& r) {
  std::string why;
  if (!poses::read3DList(in.get(port), r, why) || r.data["frame"] != frame)
    return error("需要同帧米制三维位姿：" + why, port);
  return Status::Ok();
}
Status cloudInput(const Inputs& in, const ParamView& p, const PointCloud*& c) {
  c = in.get("cloud").asCloud();
  if (!c)
    return error("需要原生点云");
  if (c->pointCount() > kPoints)
    return limit();
  auto s = validate(*c);
  if (!s.ok)
    return s;
  return frameValid(p.text("frame")) ? Status::Ok() : error("帧非法", "frame", "bad_param");
}
struct Layer {
  bool valid = false;
  double height = 0;
  std::size_t support = 0;
};
Layer layer(std::vector<double> z, double thickness, std::size_t minimum, bool highest = true) {
  if (z.size() < minimum)
    return {};
  std::sort(z.begin(), z.end());
  if (!highest) {
    for (auto& v : z)
      v = -v;
    std::reverse(z.begin(), z.end());
  }
  std::size_t lo = 0, chosen = 0, end = 0;
  bool valid = false;
  for (std::size_t hi = 0; hi < z.size(); ++hi) {
    while (z[hi] - z[lo] > thickness)
      ++lo;
    if (hi + 1 - lo >= minimum) {
      chosen = lo;
      end = hi + 1;
      valid = true;
    }
  }
  if (!valid)
    return {};
  double mean = std::accumulate(z.begin() + chosen, z.begin() + end, 0.0) / (end - chosen);
  return {true, highest ? mean : -mean, end - chosen};
}
struct Fit {
  Vec3 normal = Vec3::UnitZ(), center = Vec3::Zero();
  double d = 0, rms = 0;
  Ids ids;
};
bool tls(const PointCloud& c, const Ids& ids, Fit& f) {
  if (ids.size() < 3)
    return false;
  Vec3 center = Vec3::Zero();
  for (auto i : ids)
    center += point(c, i);
  center /= ids.size();
  Mat3 cov = Mat3::Zero();
  for (auto i : ids) {
    const Vec3 q = point(c, i) - center;
    cov += q * q.transpose();
  }
  Eigen::SelfAdjointEigenSolver<Mat3> eig(cov);
  if (eig.info() != Eigen::Success ||
      eig.eigenvalues()[1] <= 1e-14 * std::max(1.0, eig.eigenvalues()[2]))
    return false;
  f.normal = eig.eigenvectors().col(0);
  Eigen::Index dominant;
  f.normal.cwiseAbs().maxCoeff(&dominant);
  if (f.normal[dominant] < 0)
    f.normal *= -1;
  f.center = center;
  f.d = -f.normal.dot(center);
  f.ids = ids;
  double sum = 0;
  for (auto i : ids)
    sum += std::pow(f.normal.dot(point(c, i)) + f.d, 2);
  f.rms = std::sqrt(sum / ids.size());
  return true;
}
bool fit(const PointCloud& c, const Ids& ids, double distance, int iterations, Fit& f,
         ExecContext& ctx, std::uint64_t& work) {
  if (ids.size() < 3)
    return false;
  std::mt19937 random(0x4c5946u);
  std::uniform_int_distribution<std::size_t> pick(0, ids.size() - 1);
  Ids best;
  for (int attempt = 0; attempt < iterations; ++attempt) {
    if (ctx.cancelled() || !charge(work, ids.size()))
      return false;
    const Vec3 a = point(c, ids[pick(random)]), b = point(c, ids[pick(random)]),
               d = point(c, ids[pick(random)]);
    Vec3 n = (b - a).cross(d - a);
    if (n.norm() <= 1e-12)
      continue;
    n.normalize();
    Ids keep;
    for (auto i : ids)
      if (std::abs(n.dot(point(c, i) - a)) <= distance)
        keep.push_back(i);
    if (keep.size() > best.size())
      best = std::move(keep);
  }
  if (!charge(work, 3 * best.size()) || !tls(c, best, f) || !charge(work, ids.size()))
    return false;
  Ids refined;
  for (auto i : ids)
    if (std::abs(f.normal.dot(point(c, i)) + f.d) <= distance)
      refined.push_back(i);
  for (int step = 0; step < 8; ++step) {
    if (ctx.cancelled() || !charge(work, 4 * refined.size()) || !tls(c, refined, f))
      return false;
    Ids trimmed;
    double squared = 0;
    for (auto index : refined) {
      const double residual = f.normal.dot(point(c, index)) + f.d;
      if (std::abs(residual) <= distance) {
        trimmed.push_back(index);
        squared += residual * residual;
      }
    }
    const bool stable = trimmed.size() == refined.size();
    f.ids = trimmed;
    f.rms = trimmed.empty() ? 0 : std::sqrt(squared / trimmed.size());
    if (stable || step == 7)
      return trimmed.size() >= 3;
    refined = std::move(trimmed);
  }
  return false;
}
void emitGroups(const PointCloud& c, const std::vector<cloud_groups::Group>& groups, const Ids& ids,
                const std::string& frame, Outputs& out, const char* indexPort,
                const char* groupPort) {
  out.set(indexPort, Data::indices(Indices{c.id, ids}));
  out.set(groupPort, Data::record(cloud_groups::make(c, groups, frame)));
}
Port rec(const char* name, const char* type, bool required = true) {
  return withContract(Port{name, "Record", name, type, required}, {{"recordType", type}});
}

void topLayer(Registry& r) {
  auto op = descriptor("cloud.top_layer", "点云顶层高度", "top layer height extraction");
  op.inputs = {{"cloud", "PointCloud", "点云", "米制有限点云", true},
               rec("groups", cloud_groups::kType, false),
               {"indices", "Indices", "分组索引", "与groups成对", false}};
  op.outputs = {rec("heights", "cloud.TopLayers.v1"),
                {"height", "Measurement", "首组高度", "无支持ok=false", true},
                {"top", "PointCloud", "顶层点云", "保留通道", true},
                {"rest", "PointCloud", "剩余点云", "保留通道", true},
                {"topIndices", "Indices", "顶层原索引", "", true},
                {"restIndices", "Indices", "剩余原索引", "", true},
                rec("topGroups", cloud_groups::kType),
                rec("restGroups", cloud_groups::kType)};
  op.params = {text("frame", "参考帧", "input"),
               vector("heightAxis", "高度方向", {0, 0, 1}),
               vector("origin", "高度原点", {0, 0, 0}),
               number("layerThickness", "层厚", .02, 1e-9, 1e3, "m"),
               integer("minPoints", "最小支持点数", 20, 1, 1000000),
               number("extractRadius", "提取半径", .02, 0, 1e3, "m"),
               number("voxelSize", "估计降采样体素", 0, 0, 1e3, "m"),
               number("minHeight", "最低高度", -1e6, -1e6, 1e6, "m"),
               number("maxHeight", "最高高度", 1e6, -1e6, 1e6, "m")};
  op.compute = [](const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
    const PointCloud* c;
    auto s = cloudInput(in, p, c);
    if (!s.ok)
      return s;
    Vec3 axis = vec(p, "heightAxis"), origin = vec(p, "origin");
    if (!axis.allFinite() || axis.norm() < 1e-12 || !origin.allFinite() ||
        p.number("minHeight") > p.number("maxHeight"))
      return error("高度轴、原点或范围非法", "heightAxis", "bad_param");
    axis.normalize();
    Ids all = indices(*c);
    std::vector<cloud_groups::Group> groups{{"0", 0, all.size()}};
    if (in.has("groups") != in.has("indices"))
      return error("groups与indices必须成对提供", "groups");
    if (in.has("groups")) {
      const auto* metadata = in.get("groups").asRecord();
      const auto* ids = in.get("indices").asIndices();
      std::string why, frame;
      if (!metadata || !ids || !cloud_groups::parse(*metadata, *c, *ids, groups, frame, why) ||
          frame != p.text("frame"))
        return error("分组非法或参考帧不匹配：" + why, "groups");
      all = ids->values;
    }
    Json items = Json::array();
    Ids top, rest;
    std::vector<cloud_groups::Group> tg, rg;
    Measurement first;
    first.unit = "m";
    for (const auto& g : groups) {
      if (ctx.cancelled())
        return Status::Ok();
      std::vector<double> z;
      std::set<std::tuple<std::int64_t, std::int64_t, std::int64_t>> voxels;
      for (std::size_t k = g.offset; k < g.offset + g.count; ++k) {
        if ((k & 8191) == 0 && ctx.cancelled())
          return Status::Ok();
        const Vec3 q = point(*c, all[k]) - origin;
        const double h = axis.dot(q);
        if (h < p.number("minHeight") || h > p.number("maxHeight"))
          continue;
        if (p.number("voxelSize") > 0) {
          const Vec3 cell = (q / p.number("voxelSize")).array().floor();
          if (cell.cwiseAbs().maxCoeff() > 9e15)
            return error("体素坐标溢出", "voxelSize", "bad_param");
          if (!voxels
                   .emplace(static_cast<std::int64_t>(cell.x()),
                            static_cast<std::int64_t>(cell.y()),
                            static_cast<std::int64_t>(cell.z()))
                   .second)
            continue;
        }
        z.push_back(h);
      }
      const Layer result = layer(z, p.number("layerThickness"), p.integer("minPoints"));
      tg.push_back({g.id, top.size(), 0});
      rg.push_back({g.id, rest.size(), 0});
      for (std::size_t k = g.offset; k < g.offset + g.count; ++k) {
        if ((k & 8191) == 0 && ctx.cancelled())
          return Status::Ok();
        if (result.valid && std::abs(axis.dot(point(*c, all[k]) - origin) - result.height) <=
                                p.number("extractRadius")) {
          top.push_back(all[k]);
          ++tg.back().count;
        } else {
          rest.push_back(all[k]);
          ++rg.back().count;
        }
      }
      items.push_back({{"id", g.id},
                       {"valid", result.valid},
                       {"height", result.valid ? Json(result.height) : Json(nullptr)},
                       {"support", result.support},
                       {"topCount", tg.back().count}});
      if (items.size() == 1 && result.valid)
        first = measurement(result.height);
    }
    if (ctx.cancelled())
      return Status::Ok();
    out.set("heights", Data::record({"cloud.TopLayers.v1",
                                     {{"frame", p.text("frame")},
                                      {"unit", "m"},
                                      {"sourceCloudId", std::to_string(c->id)},
                                      {"items", items}}}));
    out.set("height", Data::measurement(first));
    out.set("top", Data::cloud(c->select(top)));
    out.set("rest", Data::cloud(c->select(rest)));
    emitGroups(*c, tg, top, p.text("frame"), out, "topIndices", "topGroups");
    emitGroups(*c, rg, rest, p.text("frame"), out, "restIndices", "restGroups");
    return Status::Ok();
  };
  r.addOperator(std::move(op));
}

void neighborHeight(Registry& r) {
  auto op = descriptor("cloud.neighbor_height", "邻域支撑面高度估计",
                       "neighbor cloud support surface object height");
  op.inputs = {{"cloud", "PointCloud", "场景点云", "指定高度或原尺寸模式可省略", false},
               rec("instances", poses::kPoses3DType),
               rec("workspace", poses::kPose3DType, false)};
  op.version = "2.0.0";
  op.migrations = {{1, graph_migration::requireSplitMigration, nullptr}};
  op.outputs = {rec("heights", "cloud.NeighborHeights.v1")};
  op.params = {text("frame", "参考帧", "input"),
               choice("method", "高度策略", "point_plane",
                      {{"point_plane", "点面距离"},
                       {"axis_intercept", "轴向平面交点"},
                       {"top_layer", "顶层"},
                       {"mean", "均值"},
                       {"farthest", "最远点"},
                       {"provided", "指定高度"},
                       {"original", "原尺寸高度"}}),
               choice("direction", "邻域方向", "all",
                      {{"all", "全部"},
                       {"top", "上"},
                       {"left", "左"},
                       {"bottom", "下"},
                       {"right", "右"},
                       {"except_bottom", "除下侧"}}),
               choice("sizeMode", "邻域尺寸方式", "relative",
                      {{"relative", "相对尺寸"}, {"absolute", "绝对尺寸"}}),
               number("length", "邻域长度", .1, 1e-9, 1e3, "m"),
               number("width", "邻域宽度", .1, 1e-9, 1e3, "m"),
               number("lengthRatio", "长度比例", 1, 1e-6, 100),
               number("widthRatio", "宽度比例", 1, 1e-6, 100),
               number("marginX", "横向内缩", 0, 0, 1e3, "m"),
               number("marginY", "纵向内缩", 0, 0, 1e3, "m"),
               number("minHeight", "最小相对高度", .001, 0, 1e3, "m"),
               number("maxHeight", "最大相对高度", 10, 1e-9, 1e6, "m"),
               number("layerThickness", "层厚", .02, 1e-9, 1e3, "m"),
               integer("minPoints", "最小邻域支持点数", 20, 3, 1000000),
               number("planeDistance", "平面距离", .005, 1e-9, 1e3, "m"),
               number("minInlierRatio", "最小平面支持比例", .5, 0, 1),
               number("maxPlaneAngle", "支撑面法向容差", 20, 0, 89.9, "deg"),
               number("providedHeight", "指定高度", .1, 1e-9, 1e3, "m")};
  op.compute = [](const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
    const auto method = p.choice("method");
    const bool measure = method != "original" && method != "provided";
    const PointCloud* c = nullptr;
    if (measure) {
      if (!in.has("cloud"))
        return error("邻域测量需要点云", "cloud");
      if (auto s = cloudInput(in, p, c); !s.ok)
        return s;
    }
    Record poses, workspace;
    auto s = poseInput(in, "instances", p.text("frame"), poses);
    if (!s.ok)
      return s;
    if (measure && in.has("workspace")) {
      s = poseInput(in, "workspace", p.text("frame"), workspace);
      if (!s.ok || workspace.data["items"].size() != 1)
        return error("workspace必须为单个位姿", "workspace");
    }
    const auto count = poses.data["items"].size();
    if (measure &&
        (count > 256 || static_cast<std::uint64_t>(count) * c->pointCount() * 700 > kWork))
      return limit();
    if (measure && p.number("maxHeight") < p.number("minHeight"))
      return error("高度区间反向", "maxHeight", "bad_param");
    Json items = Json::array();
    std::uint64_t work = 0;
    for (const auto& obj : poses.data["items"]) {
      if (ctx.cancelled())
        return Status::Ok();
      if (!measure) {
        const double height =
            method == "original" ? obj["size"][2].get<double>() : p.number("providedHeight");
        items.push_back({{"id", obj["id"]},
                         {"valid", true},
                         {"height", height},
                         {"axisHeight", height},
                         {"support", 0},
                         {"rms", 0.0},
                         {"direction", "provided"},
                         {"reason", ""}});
        continue;
      }
      const Mat3 rot = rotation(obj);
      const Vec3 old = jvec(obj["size"]), top = jvec(obj["position"]) + rot.col(2) * old.z() * .5;
      const double w =
          p.choice("sizeMode") == "relative" ? old.x() * p.number("widthRatio") : p.number("width");
      const double h = p.choice("sizeMode") == "relative" ? old.y() * p.number("lengthRatio")
                                                          : p.number("length");
      if (!top.allFinite() || !std::isfinite(w) || !std::isfinite(h))
        return error("工件顶面位置或邻域尺寸计算超出有限范围", "instances");
      if (w <= 2 * p.number("marginX") || h <= 2 * p.number("marginY"))
        return error("邻域内缩吞没尺寸", "marginX", "bad_param");
      double best = 0, axisHeight = 0, rms = 0;
      std::size_t support = 0;
      std::string chosen;
      bool valid = false;
      for (const std::string dir : {"top", "left", "bottom", "right"}) {
        if (p.choice("direction") != "all" && p.choice("direction") != dir &&
            !(p.choice("direction") == "except_bottom" && dir != "bottom"))
          continue;
        if (!charge(work, c->pointCount()))
          return limit();
        std::vector<double> depths;
        PointCloud local;
        for (std::size_t i = 0; i < c->pointCount(); ++i) {
          if ((i & 8191) == 0 && ctx.cancelled())
            return Status::Ok();
          const Vec3 pt = point(*c, i);
          if (in.has("workspace") && !inside(pt, workspace.data["items"][0]))
            continue;
          const Vec3 q = rot.transpose() * (pt - top);
          const double depth = -q.z();
          if (depth < p.number("minHeight") || depth > p.number("maxHeight"))
            continue;
          const double mx = p.number("marginX"), my = p.number("marginY");
          bool hit = (dir == "top" && q.y() >= old.y() * .5 && q.y() <= old.y() * .5 + h &&
                      std::abs(q.x()) <= w * .5 - mx) ||
                     (dir == "bottom" && q.y() <= -old.y() * .5 && q.y() >= -old.y() * .5 - h &&
                      std::abs(q.x()) <= w * .5 - mx) ||
                     (dir == "left" && q.x() <= -old.x() * .5 && q.x() >= -old.x() * .5 - w &&
                      std::abs(q.y()) <= h * .5 - my) ||
                     (dir == "right" && q.x() >= old.x() * .5 && q.x() <= old.x() * .5 + w &&
                      std::abs(q.y()) <= h * .5 - my);
          if (hit) {
            local.push(q.x(), q.y(), q.z());
            depths.push_back(depth);
          }
        }
        if (depths.size() < static_cast<std::size_t>(p.integer("minPoints")))
          continue;
        double value = 0, axial = 0, quality = 0;
        std::size_t n = depths.size();
        if (method == "point_plane" || method == "axis_intercept") {
          Fit plane;
          if (!fit(local, indices(local), p.number("planeDistance"), 128, plane, ctx, work)) {
            if (work > kWork)
              return limit();
            continue;
          }
          if (plane.ids.size() < static_cast<std::size_t>(p.integer("minPoints")) ||
              static_cast<double>(plane.ids.size()) / depths.size() < p.number("minInlierRatio") ||
              std::abs(plane.normal.z()) < std::cos(p.number("maxPlaneAngle") * poses::kPi / 180))
            continue;
          axial = plane.d / plane.normal.z();
          value = method == "point_plane" ? std::abs(plane.d) : axial;
          quality = plane.rms;
          n = plane.ids.size();
        } else if (method == "top_layer") {
          auto l = layer(depths, p.number("layerThickness"), p.integer("minPoints"), false);
          if (!l.valid)
            continue;
          value = axial = l.height;
          n = l.support;
        } else if (method == "mean")
          value = axial = std::accumulate(depths.begin(), depths.end(), 0.0) / depths.size();
        else
          value = axial = *std::max_element(depths.begin(), depths.end());
        if (value < p.number("minHeight") || axial < p.number("minHeight") ||
            axial > p.number("maxHeight"))
          continue;
        if (!valid || value > best) {
          valid = true;
          best = value;
          axisHeight = axial;
          rms = quality;
          support = n;
          chosen = dir;
        }
      }
      items.push_back({{"id", obj["id"]},
                       {"valid", valid},
                       {"height", valid ? Json(best) : Json(nullptr)},
                       {"axisHeight", valid ? Json(axisHeight) : Json(nullptr)},
                       {"support", support},
                       {"rms", rms},
                       {"direction", chosen},
                       {"reason", valid ? "" : "insufficient_support"}});
    }
    if (ctx.cancelled())
      return Status::Ok();
    Record heights{"cloud.NeighborHeights.v1",
                   {{"frame", p.text("frame")}, {"unit", "m"}, {"items", std::move(items)}}};
    std::string why;
    if (!values::boundedJson(heights.data, why, true))
      return error("邻域高度结果超过有效记录边界：" + why, "instances", "resource_limit");
    out.set("heights", Data::record(std::move(heights)));
    return Status::Ok();
  };
  r.addOperator(std::move(op));
}

void applyHeight(Registry& r) {
  auto op = descriptor("pose.apply_height", "按实例高度更新尺寸",
                       "pose height size update preserve top face");
  op.inputs = {rec("instances", poses::kPoses3DType), rec("heights", "cloud.NeighborHeights.v1")};
  op.outputs = {rec("instances", poses::kPoses3DType)};
  op.params = {
      choice("source", "尺寸更新策略", "measured",
             {{"measured", "使用测量"}, {"original", "保留已有高度"}, {"maximum", "较大高度"}})};
  op.compute = [](const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
    Record objects;
    std::string why;
    if (!poses::read3DList(in.get("instances"), objects, why))
      return error("需要米制三维位姿：" + why, "instances");
    const auto& heights = in.get("heights").asRecord()->data;
    const auto items = heights.find("items");
    if (!heights.is_object() || heights.value("frame", Json()) != objects.data["frame"] ||
        heights.value("unit", "") != "m" || items == heights.end() || !items->is_array())
      return error("需要与位姿列表同帧的米制高度观测", "heights");
    std::map<std::string, double> byId;
    for (const auto& item : *items) {
      if (!item.is_object() || !item.contains("id") || !item["id"].is_string() ||
          !item.contains("valid") || !item["valid"].is_boolean())
        return error("高度观测需要实例 ID 和有效性", "heights");
      double height = 0;
      if (item["valid"] == true) {
        const auto axis = item.find("axisHeight");
        if (axis == item.end() || !axis->is_number() || !std::isfinite(axis->get<double>()))
          return error("有效观测需要有限轴向高度", "heights");
        height = axis->get<double>();
      }
      const auto id = item["id"].get<std::string>();
      if (!byId.try_emplace(id, item["valid"] == true ? height
                                                      : std::numeric_limits<double>::quiet_NaN())
               .second)
        return error("高度观测包含重复的实例 ID", "heights");
    }
    const auto source = p.choice("source");
    for (auto& item : objects.data["items"]) {
      if (ctx.cancelled())
        return Status::Ok();
      const auto result = byId.find(item["id"].get<std::string>());
      if (source == "original" || result == byId.end() || std::isnan(result->second))
        continue;
      const double old = item["size"][2].get<double>();
      const double size = source == "maximum" ? std::max(old, result->second) : result->second;
      const Vec3 axis = rotation(item).col(2);
      const Vec3 top = jvec(item["position"]) + axis * old * .5;
      const Vec3 center = top - axis * size * .5;
      if (!std::isfinite(size) || size <= 0 || !center.allFinite())
        return error("工件高度更新后的尺寸或位置无效", "heights");
      item["size"][2] = size;
      item["position"] = array(center);
    }
    if (ctx.cancelled())
      return Status::Ok();
    out.set("instances", Data::record(std::move(objects)));
    return Status::Ok();
  };
  r.addOperator(std::move(op));
}

void toteObservation(Registry& r) {
  auto op = descriptor("cloud.tote_observation", "料箱区域观测统计",
                       "tote observation point and recognized instance counts");
  op.inputs = {{"cloud", "PointCloud", "有效场景点云", "", true},
               rec("instances", poses::kPoses3DType),
               rec("roi", poses::kPose3DType, false)};
  op.outputs = {rec("counts", "cloud.ToteObservation.v1")};
  op.params = {text("frame", "参考帧", "input")};
  op.compute = [](const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
    const PointCloud* c;
    auto s = cloudInput(in, p, c);
    if (!s.ok)
      return s;
    Record objects, roi;
    s = poseInput(in, "instances", p.text("frame"), objects);
    if (!s.ok)
      return s;
    if (in.has("roi")) {
      s = poseInput(in, "roi", p.text("frame"), roi);
      if (!s.ok || roi.data["items"].size() != 1)
        return error("roi必须是单个位姿", "roi");
    }
    std::size_t n = 0, recognized = 0;
    for (std::size_t i = 0; i < c->pointCount(); ++i) {
      if ((i & 8191) == 0 && ctx.cancelled())
        return Status::Ok();
      if (!in.has("roi") || inside(point(*c, i), roi.data["items"][0]))
        ++n;
    }
    for (const auto& item : objects.data["items"])
      if (!in.has("roi") || inside(jvec(item["position"]), roi.data["items"][0]))
        ++recognized;
    if (ctx.cancelled())
      return Status::Ok();
    out.set("counts", Data::record({"cloud.ToteObservation.v1",
                                    {{"frame", p.text("frame")},
                                     {"observedPoints", n},
                                     {"recognizedInstances", recognized}}}));
    return Status::Ok();
  };
  r.addOperator(std::move(op));
}

void emptyTote(Registry& r) {
  auto op = descriptor("cloud.empty_tote", "料箱观测状态判定",
                       "empty tote unknown unrecognized obstacle observation");
  op.version = "2.0.0";
  op.migrations = {{1, graph_migration::requireSplitMigration, nullptr}};
  op.inputs = {rec("counts", "cloud.ToteObservation.v1"), rec("observation", values::kValueType)};
  op.outputs = {rec("status", "cloud.EmptyTote.v1"), rec("valid", values::kValueType),
                rec("empty", values::kValueType), rec("undetected", values::kValueType)};
  op.params = {integer("maxUndetectedPoints", "允许的未识别点数", 100, 0, 1000000)};
  op.compute = [](const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
    const auto& counts = in.get("counts").asRecord()->data;
    const auto frame = counts.find("frame"), observed = counts.find("observedPoints"),
               recognized = counts.find("recognizedInstances");
    const auto count = [&](const Json::const_iterator& field) {
      return field != counts.end() &&
             (field->is_number_unsigned() || (field->is_number_integer() && *field >= 0));
    };
    if (frame == counts.end() || !frame->is_string() || !count(observed) || !count(recognized))
      return error("需要带参考帧的非负观测计数", "counts");
    const auto* obs = in.get("observation").asRecord();
    std::string why;
    if (!values::validate(*obs, why) || obs->data["kind"] != "boolean")
      return error("observation必须是明确的boolean Value", "observation");
    const bool valid = obs->data["value"].get<bool>();
    const std::string state = !valid                                  ? "unknown"
                              : recognized->get<std::uint64_t>() != 0 ? "occupied"
                              : observed->get<std::uint64_t>() >
                                      static_cast<std::uint64_t>(p.integer("maxUndetectedPoints"))
                                  ? "unrecognized"
                                  : "empty";
    if (ctx.cancelled())
      return Status::Ok();
    out.set("status", Data::record({"cloud.EmptyTote.v1",
                                    {{"frame", *frame},
                                     {"valid", valid},
                                     {"state", state},
                                     {"observedPoints", *observed},
                                     {"recognizedInstances", *recognized}}}));
    out.set("valid", Data::record(values::boolean(valid)));
    out.set("empty", Data::record(values::boolean(state == "empty")));
    out.set("undetected", Data::record(values::boolean(state == "unrecognized")));
    return Status::Ok();
  };
  r.addOperator(std::move(op));
}

Json splitLink(const std::string& from, const char* output, const std::string& to,
               const char* input) {
  return {{"from", {{"node", from}, {"port", output}}}, {"to", {{"node", to}, {"port", input}}}};
}

void migrateNeighborHeight(Json& level, const Json& rule, Json* outer) {
  using namespace graph_migration;
  splitRuleKeys(rule, {"kind", "source"});
  const auto& old = splitSource(level, rule, "cloud.neighbor_height");
  const auto id = graph_migration::text(old, "id");
  const auto apply = id + "_apply";
  const auto previous = old.value("params", Json::object());
  const bool original = previous.value("source", "") == "original";
  for (const auto* binding : parameterBindings(level, outer, id, "source")) {
    const auto options = binding->value("options", Json::array());
    require(
        !options.empty() && binding->value("default", "") != "original" &&
            std::none_of(options.begin(), options.end(),
                         [](const auto& item) { return item.value("value", "") == "original"; }),
        "节点 " + id +
            " 的 source 绑定会在测量与原尺寸之间切换。请先固定 source，或从尺寸策略绑定中移除 original 并另设测量来源");
  }
  if (original)
    require(parameterBindings(level, outer, id, "method").empty(),
            "节点 " + id + " 固定使用原尺寸。请删除旧 method 的绑定后迁移");
  SplitRoutes params{{"source", {{apply, "source"}}}};
  for (const char* name :
       {"frame", "method", "direction", "sizeMode", "length", "width", "lengthRatio", "widthRatio",
        "marginX", "marginY", "minHeight", "maxHeight", "layerThickness", "minPoints",
        "planeDistance", "minInlierRatio", "maxPlaneAngle", "providedHeight"})
    params[name] = {{id, name}};
  if (original)
    params["method"].clear();
  splitNode(level, outer, id,
            Json::array({{{"id", id},
                          {"op", "cloud.neighbor_height"},
                          {"opVersion", "2.0.0"},
                          {"params", original ? Json{{"method", "original"}} : Json::object()}},
                         {{"id", apply}, {"op", "pose.apply_height"}, {"opVersion", "1.0.0"}}}),
            {{"cloud", {{id, "cloud"}}},
             {"workspace", {{id, "workspace"}}},
             {"instances", {{id, "instances"}, {apply, "instances"}}}},
            {{"heights", {{id, "heights"}}}, {"instances", {{apply, "instances"}}}}, params,
            Json::array({splitLink(id, "heights", apply, "heights")}));
}

void migrateEmptyTote(Json& level, const Json& rule, Json* outer) {
  using namespace graph_migration;
  splitRuleKeys(rule, {"kind", "source"});
  const auto id = graph_migration::text(splitSource(level, rule, "cloud.empty_tote"), "id");
  const auto counts = id + "_counts";
  splitNode(
      level, outer, id,
      Json::array({{{"id", counts}, {"op", "cloud.tote_observation"}, {"opVersion", "1.0.0"}},
                   {{"id", id}, {"op", "cloud.empty_tote"}, {"opVersion", "2.0.0"}}}),
      {{"cloud", {{counts, "cloud"}}},
       {"instances", {{counts, "instances"}}},
       {"roi", {{counts, "roi"}}},
       {"observation", {{id, "observation"}}}},
      {{"status", {{id, "status"}}},
       {"valid", {{id, "valid"}}},
       {"empty", {{id, "empty"}}},
       {"undetected", {{id, "undetected"}}}},
      {{"frame", {{counts, "frame"}}}, {"maxUndetectedPoints", {{id, "maxUndetectedPoints"}}}},
      Json::array({splitLink(counts, "counts", id, "counts")}));
}

struct UV {
  double x, y;
};
double cross(UV a, UV b, UV c) { return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); }
std::vector<UV> hull(std::vector<UV> p) {
  std::sort(p.begin(), p.end(), [](UV a, UV b) { return std::tie(a.x, a.y) < std::tie(b.x, b.y); });
  p.erase(std::unique(p.begin(), p.end(), [](UV a, UV b) { return a.x == b.x && a.y == b.y; }),
          p.end());
  if (p.size() < 3)
    return p;
  std::vector<UV> h;
  for (auto q : p) {
    while (h.size() > 1 && cross(h[h.size() - 2], h.back(), q) <= 0)
      h.pop_back();
    h.push_back(q);
  }
  const auto lower = h.size();
  for (std::size_t i = p.size() - 1; i-- > 0;) {
    auto q = p[i];
    while (h.size() > lower && cross(h[h.size() - 2], h.back(), q) <= 0)
      h.pop_back();
    h.push_back(q);
  }
  h.pop_back();
  return h;
}
struct Rectangle {
  double angle = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};
Rectangle minimumRect(const std::vector<UV>& pts, const std::vector<UV>& h) {
  Rectangle best;
  double area = std::numeric_limits<double>::infinity();
  for (std::size_t i = 0; i < h.size(); ++i) {
    const auto a = h[i], b = h[(i + 1) % h.size()];
    const double angle = std::atan2(b.y - a.y, b.x - a.x), c = std::cos(angle), s = std::sin(angle);
    Rectangle r;
    r.angle = angle;
    r.x0 = r.y0 = 1e300;
    r.x1 = r.y1 = -1e300;
    for (auto p : pts) {
      double x = c * p.x + s * p.y, y = -s * p.x + c * p.y;
      r.x0 = std::min(r.x0, x);
      r.x1 = std::max(r.x1, x);
      r.y0 = std::min(r.y0, y);
      r.y1 = std::max(r.y1, y);
    }
    double ar = (r.x1 - r.x0) * (r.y1 - r.y0);
    if (ar < area) {
      best = r;
      area = ar;
    }
  }
  return best;
}
struct GridRect {
  int x = 0, y = 0, w = 0, h = 0;
};
GridRect largest(const std::vector<unsigned char>& grid, int w, int h) {
  std::vector<int> heights(w, 0);
  GridRect best;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x)
      heights[x] = grid[y * w + x] ? heights[x] + 1 : 0;
    std::vector<int> stack;
    for (int x = 0; x <= w; ++x) {
      const int height = x == w ? 0 : heights[x];
      while (!stack.empty() && heights[stack.back()] > height) {
        int column = stack.back();
        stack.pop_back();
        int left = stack.empty() ? 0 : stack.back() + 1;
        GridRect r{left, y - heights[column] + 1, x - left, heights[column]};
        if (r.w * r.h > best.w * best.h)
          best = r;
      }
      stack.push_back(x);
    }
  }
  return best;
}

void planePatches(Registry& r) {
  auto op = descriptor("cloud.plane_patches", "有限平面片几何分解",
                       "finite plane patch minimum bounding inscribed rectangle geometry");
  op.inputs = {{"cloud", "PointCloud", "场景点云", "米制", true}};
  op.outputs = {rec("instances", poses::kPoses3DType),
                rec("patches", "cloud.PlanePatches.v1"),
                rec("groups", cloud_groups::kType),
                {"indices", "Indices", "平面片原索引", "", true},
                {"uncoveredIndices", "Indices", "未覆盖原索引", "", true},
                {"uncovered", "PointCloud", "未覆盖点云", "", true}};
  op.params = {text("frame", "参考帧", "input"),
               number("planeDistance", "平面距离", .005, 1e-9, 1e3, "m"),
               integer("minPoints", "最小平面点数", 20, 3, 1000000),
               integer("maxIterations", "平面拟合次数", 128, 1, 10000),
               integer("maxPlanes", "最多平面数", 16, 1, 256),
               choice("mode", "矩形方式", "inscribed",
                      {{"bounds", "最小面积外包矩形"}, {"inscribed", "占据最大内接矩形"}}),
               number("cellSize", "占据网格间距", .01, 1e-8, 1e3, "m"),
               number("minSide", "最小边长", .02, 1e-9, 1e3, "m"),
               number("minArea", "最小面积", .0004, 1e-12, 1e6, "m2"),
               number("minCoverage", "最小占据比例", .5, 0, 1),
               number("thickness", "几何体厚度", .01, 1e-9, 1e3, "m"),
               number("clusterDistance", "可选连通聚类距离", 0, 0, 1e3, "m"),
               integer("clusterMinPoints", "最小簇点数", 3, 3, 1000000),
               number("normalAngle", "可选法线簇角度", 90, 0, 90, "deg"),
               choice("surfaceOffset", "表面偏移策略", "none",
                      {{"none", "拟合面"}, {"quantile", "正向残差分位面"}}),
               number("surfaceQuantile", "正向表面分位", .9, 0, 1),
               number("collisionDistance", "重复几何覆盖容差", .005, 0, 1e3, "m"),
               integer("maxCollisionPoints", "允许的未覆盖支持点", 0, 0, 1000000),
               number("maxCollisionRatio", "重复几何覆盖比例", .95, 0, 1)};
  op.compute = [](const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
    const PointCloud* c;
    auto status = cloudInput(in, p, c);
    if (!status.ok)
      return status;
    if (static_cast<std::uint64_t>(c->pointCount()) * (p.integer("maxIterations") + 36) *
            p.integer("maxPlanes") >
        kWork)
      return limit();
    if (p.number("normalAngle") < 90 && !c->hasNormals())
      return error("法线聚类需要原生法线通道");
    Ids remaining = indices(*c), accepted;
    std::vector<bool> covered(c->pointCount(), false);
    std::vector<cloud_groups::Group> groups;
    Json posesOut = Json::array(), patches = Json::array();
    std::uint64_t work = 0;
    for (int planeNo = 0; planeNo < p.integer("maxPlanes") &&
                          remaining.size() >= static_cast<std::size_t>(p.integer("minPoints"));
         ++planeNo) {
      if (ctx.cancelled())
        return Status::Ok();
      Fit f;
      if (!fit(*c, remaining, p.number("planeDistance"), p.integer("maxIterations"), f, ctx,
               work) ||
          f.ids.size() < static_cast<std::size_t>(p.integer("minPoints"))) {
        if (work > kWork)
          return limit();
        break;
      }
      std::set<std::int32_t> fitted(f.ids.begin(), f.ids.end());
      if (p.choice("surfaceOffset") == "quantile") {
        if (!charge(work, 32 * f.ids.size()))
          return limit();
        std::vector<double> residuals;
        for (auto index : f.ids)
          residuals.push_back(f.normal.dot(point(*c, index)) + f.d);
        std::sort(residuals.begin(), residuals.end());
        const auto rank =
            static_cast<std::size_t>(p.number("surfaceQuantile") * (residuals.size() - 1));
        const double shift = std::max(0.0, residuals[rank]);
        f.center += f.normal * shift;
        f.d -= shift;
        double squared = 0;
        for (double residual : residuals)
          squared += (residual - shift) * (residual - shift);
        f.rms = std::sqrt(squared / residuals.size());
      }
      Ids rest;
      for (auto i : remaining)
        if (!fitted.count(i))
          rest.push_back(i);
      remaining = std::move(rest);
      if (!charge(work, f.ids.size()))
        return limit();
      Ids withinSlab;
      const double supportDistance =
          std::min(p.number("planeDistance"), p.number("thickness") * .5);
      for (auto index : f.ids)
        if (std::abs(f.normal.dot(point(*c, index)) + f.d) <= supportDistance)
          withinSlab.push_back(index);
      f.ids = std::move(withinSlab);
      if (f.ids.size() < static_cast<std::size_t>(p.integer("minPoints")))
        continue;
      std::vector<Ids> components;
      if (p.number("clusterDistance") == 0 && p.number("normalAngle") == 90)
        components.push_back(f.ids);
      else {
        if (!charge(work, static_cast<std::uint64_t>(f.ids.size()) * f.ids.size()))
          return limit();
        std::vector<bool> seen(f.ids.size(), false);
        for (std::size_t seed = 0; seed < f.ids.size(); ++seed) {
          if (seen[seed])
            continue;
          std::vector<std::size_t> queue{seed};
          seen[seed] = true;
          Ids part;
          for (std::size_t qi = 0; qi < queue.size(); ++qi) {
            if (ctx.cancelled())
              return Status::Ok();
            auto a = queue[qi];
            part.push_back(f.ids[a]);
            for (std::size_t b = 0; b < f.ids.size(); ++b) {
              if (seen[b])
                continue;
              if (p.number("clusterDistance") > 0 &&
                  (point(*c, f.ids[a]) - point(*c, f.ids[b])).norm() > p.number("clusterDistance"))
                continue;
              if (p.number("normalAngle") < 90) {
                Vec3 na(c->normals[3 * f.ids[a]], c->normals[3 * f.ids[a] + 1],
                        c->normals[3 * f.ids[a] + 2]),
                    nb(c->normals[3 * f.ids[b]], c->normals[3 * f.ids[b] + 1],
                       c->normals[3 * f.ids[b] + 2]);
                if (na.norm() < 1e-12 || nb.norm() < 1e-12)
                  return error("法线聚类不接受零法线");
                if (std::abs(na.normalized().dot(nb.normalized())) <
                    std::cos(p.number("normalAngle") * poses::kPi / 180))
                  continue;
              }
              seen[b] = true;
              queue.push_back(b);
            }
          }
          if (part.size() >= static_cast<std::size_t>(p.integer("clusterMinPoints")))
            components.push_back(std::move(part));
        }
      }
      for (auto& part : components) {
        Vec3 u = f.normal.unitOrthogonal(), v = f.normal.cross(u);
        std::vector<UV> projected;
        for (auto i : part) {
          Vec3 q = point(*c, i) - f.center;
          projected.push_back({u.dot(q), v.dot(q)});
        }
        if (!charge(work, 32 * part.size()))
          return limit();
        auto boundary = hull(projected);
        if (boundary.size() < 3)
          continue;
        if (!charge(work, static_cast<std::uint64_t>(boundary.size()) * part.size()))
          return limit();
        const Rectangle bounds = minimumRect(projected, boundary);
        const double cs = p.number("cellSize"), co = std::cos(bounds.angle),
                     si = std::sin(bounds.angle);
        const double wd = (bounds.x1 - bounds.x0) / cs, hd = (bounds.y1 - bounds.y0) / cs;
        if (wd > 2000000 || hd > 2000000)
          return limit();
        const int gw = static_cast<int>(std::floor(wd + 1e-6)) + 1,
                  gh = static_cast<int>(std::floor(hd + 1e-6)) + 1;
        if (static_cast<std::uint64_t>(gw) * gh > 2000000)
          return limit();
        if (!charge(work, static_cast<std::uint64_t>(gw) * gh + part.size()))
          return limit();
        std::vector<unsigned char> grid(static_cast<std::size_t>(gw) * gh, 0);
        std::vector<std::pair<int, int>> cells;
        std::vector<UV> uv;
        for (auto q : projected) {
          UV z{co * q.x + si * q.y, -si * q.x + co * q.y};
          int x = std::clamp(static_cast<int>(std::floor((z.x - bounds.x0) / cs + 1e-6)), 0,
                             gw - 1),
              y = std::clamp(static_cast<int>(std::floor((z.y - bounds.y0) / cs + 1e-6)), 0,
                             gh - 1);
          grid[y * gw + x] = 1;
          cells.emplace_back(x, y);
          uv.push_back(z);
        }
        while (true) {
          if (ctx.cancelled())
            return Status::Ok();
          if (!charge(work, 4 * grid.size() + 2 * part.size()))
            return limit();
          GridRect gr =
              p.choice("mode") == "bounds" ? GridRect{0, 0, gw, gh} : largest(grid, gw, gh);
          if (gr.w == 0 || gr.h == 0)
            break;
          Rectangle rr = bounds;
          if (p.choice("mode") == "inscribed") {
            rr.x0 = rr.y0 = std::numeric_limits<double>::infinity();
            rr.x1 = rr.y1 = -std::numeric_limits<double>::infinity();
            for (std::size_t i = 0; i < part.size(); ++i)
              if (cells[i].first >= gr.x && cells[i].first < gr.x + gr.w &&
                  cells[i].second >= gr.y && cells[i].second < gr.y + gr.h) {
                rr.x0 = std::min(rr.x0, uv[i].x);
                rr.x1 = std::max(rr.x1, uv[i].x);
                rr.y0 = std::min(rr.y0, uv[i].y);
                rr.y1 = std::max(rr.y1, uv[i].y);
              }
          }
          for (int y = gr.y; y < gr.y + gr.h; ++y)
            for (int x = gr.x; x < gr.x + gr.w; ++x) {
              grid[y * gw + x] = 0;
            }
          const double sx = rr.x1 - rr.x0, sy = rr.y1 - rr.y0;
          if (sx < p.number("minSide") || sy < p.number("minSide") ||
              sx * sy < p.number("minArea")) {
            if (p.choice("mode") == "bounds")
              break;
            continue;
          }
          Vec3 x = u * co + v * si, y = -u * si + v * co,
               center = f.center + x * (rr.x0 + rr.x1) * .5 + y * (rr.y0 + rr.y1) * .5;
          Mat3 rot;
          rot.col(0) = x;
          rot.col(1) = y;
          rot.col(2) = f.normal;
          const double rounding = 64 * std::numeric_limits<double>::epsilon() *
                                  std::max({1.0, center.cwiseAbs().maxCoeff(), sx, sy});
          auto pose = poses::make3D("candidate", p.text("frame"),
                                    {center.x(), center.y(), center.z()}, quaternion(rot),
                                    {sx + 2 * rounding, sy + 2 * rounding, p.number("thickness")});
          const Mat3 deliveredRotation = rotation(pose.data);
          const Vec3 deliveredSize = jvec(pose.data["size"]);
          Ids support;
          std::set<std::pair<int, int>> occupied;
          double squared = 0;
          for (std::size_t i = 0; i < part.size(); ++i) {
            if (cells[i].first >= gr.x && cells[i].first < gr.x + gr.w && cells[i].second >= gr.y &&
                cells[i].second < gr.y + gr.h) {
              const Vec3 q = deliveredRotation.transpose() * (point(*c, part[i]) - center);
              const double residual = f.normal.dot(point(*c, part[i])) + f.d;
              if ((q.cwiseAbs().array() > (deliveredSize * .5).array()).any() ||
                  std::abs(residual) > p.number("planeDistance"))
                continue;
              support.push_back(part[i]);
              occupied.insert(cells[i]);
              squared += residual * residual;
            }
          }
          const double coverage = static_cast<double>(occupied.size()) / (gr.w * gr.h);
          if (coverage >= p.number("minCoverage") &&
              support.size() >= static_cast<std::size_t>(p.integer("minPoints"))) {
            if (!charge(work, support.size() * posesOut.size()))
              return limit();
            std::size_t duplicate = 0;
            for (auto index : support) {
              for (const auto& item : posesOut) {
                const Vec3 q =
                    rotation(item).transpose() * (point(*c, index) - jvec(item["position"]));
                const Vec3 size = jvec(item["size"]);
                if (std::abs(q.x()) <= size.x() * .5 + p.number("collisionDistance") &&
                    std::abs(q.y()) <= size.y() * .5 + p.number("collisionDistance") &&
                    std::abs(q.z()) <= size.z() * .5 + p.number("collisionDistance")) {
                  ++duplicate;
                  break;
                }
              }
            }
            const bool redundant =
                !posesOut.empty() &&
                static_cast<double>(duplicate) / support.size() >= p.number("maxCollisionRatio") &&
                support.size() - duplicate <=
                    static_cast<std::size_t>(p.integer("maxCollisionPoints"));
            if (!redundant) {
              if (posesOut.size() >= 256)
                return limit();
              const std::string id = "patch-" + std::to_string(posesOut.size());
              pose.data["id"] = id;
              posesOut.push_back(pose.data);
              Json corners = Json::array();
              for (auto xy :
                   {UV{rr.x0, rr.y0}, UV{rr.x1, rr.y0}, UV{rr.x1, rr.y1}, UV{rr.x0, rr.y1}})
                corners.push_back(array(f.center + x * xy.x + y * xy.y));
              patches.push_back({{"id", id},
                                 {"normal", array(f.normal)},
                                 {"offset", f.d},
                                 {"rms", std::sqrt(squared / support.size())},
                                 {"area", sx * sy},
                                 {"coverage", coverage},
                                 {"gridSize", {gr.w, gr.h}},
                                 {"occupiedCells", occupied.size()},
                                 {"boundaryTolerance", rounding},
                                 {"support", support.size()},
                                 {"corners", corners}});
              std::sort(support.begin(), support.end());
              groups.push_back({id, accepted.size(), support.size()});
              accepted.insert(accepted.end(), support.begin(), support.end());
              for (auto i : support)
                covered[i] = true;
            }
          }
          if (p.choice("mode") == "bounds")
            break;
        }
      }
    }
    if (ctx.cancelled())
      return Status::Ok();
    if (!charge(work, c->pointCount()))
      return limit();
    Ids uncovered;
    for (std::size_t i = 0; i < c->pointCount(); ++i)
      if (!covered[i])
        uncovered.push_back(i);
    Record instances = poses::make3DList(std::move(posesOut), p.text("frame"));
    Record metadata{"cloud.PlanePatches.v1",
                    {{"frame", p.text("frame")},
                     {"unit", "m"},
                     {"sourceCloudId", std::to_string(c->id)},
                     {"items", std::move(patches)}}};
    std::string why;
    if (!poses::validate(instances, why))
      return error("有限平面片位姿无效：" + why, "cloud");
    if (!values::boundedJson(metadata.data, why))
      return error("有限平面片记录无效：" + why, "cloud", "resource_limit");
    out.set("instances", Data::record(std::move(instances)));
    out.set("patches", Data::record(std::move(metadata)));
    emitGroups(*c, groups, accepted, p.text("frame"), out, "indices", "groups");
    out.set("uncoveredIndices", Data::indices(Indices{c->id, uncovered}));
    out.set("uncovered", Data::cloud(c->select(uncovered)));
    return Status::Ok();
  };
  r.addOperator(std::move(op));
}
} // namespace
void registerP2HeightRules(Registry& r) {
  topLayer(r);
  neighborHeight(r);
  applyHeight(r);
  toteObservation(r);
  emptyTote(r);
  r.addGraphMigration({"neighborHeight.v1", {}, migrateNeighborHeight});
  r.addGraphMigration({"emptyTote.v1", {}, migrateEmptyTote});
  planePatches(r);
}
} // namespace lyflow::ops
