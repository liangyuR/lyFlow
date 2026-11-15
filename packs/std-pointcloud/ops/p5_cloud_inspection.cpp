#include <array>
#include <random>

#include "p1_common.h"
#include "lyflow/pose.h"

namespace lyflow::ops {
namespace {
using namespace p1;
using Json = nlohmann::json;
constexpr std::size_t maxPoints = 1000000;
struct PrecisionLoss {};
Param flag(const char* name, const char* label, bool value) {
  Param p;
  p.name = name;
  p.label = label;
  p.type = ParamType::Bool;
  p.def = Value::boolean(value);
  return p;
}
Status boxes(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  try {
    const auto* record = in.get("boxes").asRecord();
    std::string why;
    if (!record || !poses::validate(*record, why) || record->type != poses::kPoses3DType)
      return error("需要米制vision.Poses3D.v1：" + why, "boxes");
    const auto& items = record->data["items"];
    if (items.size() > 128)
      return error("盒体数量超过128", "boxes", "resource_limit");
    const auto mode = p.choice("sampling"), coordinates = p.choice("coordinates");
    const auto count = p.integer("pointsPerFace"), seed = p.integer("seed");
    const double spacing = p.number("wireSpacing"), angle = p.number("minVisibleAngle");
    const auto eyeValues = p.vec3("viewPoint"), color = p.vec3("color");
    Vec3 eye(eyeValues[0], eyeValues[1], eyeValues[2]);
    if ((mode != "surface" && mode != "volume") ||
        (coordinates != "pose" && coordinates != "local") || count < 1 || count > 10000 ||
        seed < 0 || seed > 2147483647 || !std::isfinite(spacing) || spacing <= 0 ||
        !std::isfinite(angle) || angle < 0 || angle > 89 || !eye.allFinite())
      return error("采样、坐标、数量、种子、间距或观察参数非法", "sampling", "bad_param");
    for (double value : color)
      if (!std::isfinite(value) || value < 0 || value > 255 || value != std::floor(value))
        return error("RGB颜色需要0..255整数", "color", "bad_param");
    if (mode == "volume" && p.flag("visibleOnly"))
      return error("体积采样无可见面概念", "visibleOnly", "bad_param");
    if (mode == "volume" && p.flag("normals"))
      return error("体积内部没有表面法线，请关闭normals", "normals", "bad_param");
    std::uint64_t budget = 0;
    for (const auto& item : items) {
      const auto size = item["size"].get<std::vector<double>>();
      for (double side : size) {
        if (!std::isfinite(side / spacing) || side / spacing > maxPoints)
          return error("线框密度超过预算", "wireSpacing", "resource_limit");
        budget += 4 * (static_cast<std::uint64_t>(std::ceil(side / spacing)) + 1);
      }
      budget += 6 * static_cast<std::uint64_t>(count);
      if (budget > maxPoints)
        return error("盒体和线框累计点数超过1M", "pointsPerFace", "resource_limit");
    }
    PointCloud cloud, wire;
    Indices indices, wireIndices;
    indices.sourceCloudId = cloud.id;
    wireIndices.sourceCloudId = wire.id;
    std::vector<cloud_groups::Group> groups, wireGroups;
    Json metadata = Json::array();
    std::mt19937 generator(static_cast<std::uint32_t>(seed));
    auto random = [&]() { return (static_cast<double>(generator()) + .5) / 4294967296.; };
    std::uint64_t work = 0;
    double precisionLimit = 0;
    auto append = [&](PointCloud& target, Indices& targetIndices, const Vec3& q, const Vec3& n,
                      const Mat3& r, const Vec3& t) {
      ++work;
      Vec3 position = coordinates == "pose" ? (r * q + t).eval() : q;
      Vec3 normal = coordinates == "pose" ? (r * n).eval() : n;
      if (!position.allFinite() ||
          position.cwiseAbs().maxCoeff() > std::numeric_limits<float>::max() || !normal.allFinite())
        throw PrecisionLoss{};
      const Vec3 delivered = position.cast<float>().cast<double>();
      if ((delivered - position).norm() > precisionLimit)
        throw PrecisionLoss{};
      targetIndices.values.push_back(static_cast<std::int32_t>(target.pointCount()));
      target.push(static_cast<float>(position.x()), static_cast<float>(position.y()),
                  static_cast<float>(position.z()));
      if (p.flag("normals"))
        for (int c = 0; c < 3; ++c)
          target.normals.push_back(static_cast<float>(normal[c]));
      if (p.flag("colors"))
        for (double value : color)
          target.rgb.push_back(static_cast<std::uint8_t>(value));
    };
    for (const auto& item : items) {
      if (ctx.cancelled())
        return error("任务已取消", "boxes", "cancelled");
      const auto q = item["rotation"].get<poses::Quaternion>();
      const auto rotation = poses::rotationMatrix(q);
      Mat3 r;
      for (int i = 0; i < 9; ++i)
        r(i / 3, i % 3) = rotation[i];
      Vec3 t(item["position"][0].get<double>(), item["position"][1].get<double>(),
             item["position"][2].get<double>());
      Vec3 half(item["size"][0].get<double>() / 2, item["size"][1].get<double>() / 2,
                item["size"][2].get<double>() / 2);
      precisionLimit = half.minCoeff() * 2e-4;
      std::set<std::array<float, 3>> corners;
      for (int x : {-1, 1})
        for (int y : {-1, 1})
          for (int z : {-1, 1}) {
            const Vec3 local(x * half.x(), y * half.y(), z * half.z());
            const Vec3 world = coordinates == "pose" ? (r * local + t).eval() : local;
            if (!world.allFinite() ||
                world.cwiseAbs().maxCoeff() > std::numeric_limits<float>::max())
              throw PrecisionLoss{};
            const Eigen::Vector3f rounded = world.cast<float>();
            if ((rounded.cast<double>() - world).norm() > precisionLimit)
              throw PrecisionLoss{};
            corners.insert({rounded.x(), rounded.y(), rounded.z()});
          }
      if (corners.size() != 8)
        throw PrecisionLoss{};
      const auto start = cloud.pointCount(), wireStart = wire.pointCount();
      std::array<bool, 6> visible{};
      Json faces = Json::array();
      for (int axis = 0; axis < 3; ++axis)
        for (int sign : {-1, 1}) {
          Vec3 n = Vec3::Zero(), center = Vec3::Zero();
          n[axis] = sign;
          center[axis] = sign * half[axis];
          const Vec3 sight = eye - (r * center + t);
          const int face = axis * 2 + (sign > 0);
          visible[face] = !p.flag("visibleOnly") ||
                          (sight.norm() > 0 &&
                           sight.normalized().dot(r * n) > std::sin(angle * poses::kPi / 180));
          if (!visible[face])
            continue;
          faces.push_back(face);
          if (mode == "surface")
            for (int k = 0; k < count; ++k) {
              if ((work & 1023) == 0 && ctx.cancelled())
                return error("任务已取消", "boxes", "cancelled");
              Vec3 position = center;
              for (int j = 0; j < 3; ++j)
                if (j != axis)
                  position[j] = (2 * random() - 1) * half[j];
              append(cloud, indices, position, n, r, t);
            }
        }
      if (mode == "volume")
        for (int k = 0; k < count * 6; ++k) {
          if ((work & 1023) == 0 && ctx.cancelled())
            return error("任务已取消", "boxes", "cancelled");
          Vec3 position;
          for (int j = 0; j < 3; ++j)
            position[j] = (2 * random() - 1) * half[j];
          append(cloud, indices, position, Vec3::Zero(), r, t);
        }
      for (int axis = 0; axis < 3; ++axis) {
        const int b = (axis + 1) % 3, c = (axis + 2) % 3;
        const auto steps = static_cast<std::size_t>(std::ceil(2 * half[axis] / spacing));
        for (int sb : {-1, 1})
          for (int sc : {-1, 1}) {
            if (!visible[b * 2 + (sb > 0)] && !visible[c * 2 + (sc > 0)])
              continue;
            Vec3 normal = Vec3::Zero();
            if (visible[b * 2 + (sb > 0)])
              normal[b] = sb;
            if (visible[c * 2 + (sc > 0)])
              normal[c] = sc;
            normal.normalize();
            for (std::size_t k = 0; k <= steps; ++k) {
              if ((work & 1023) == 0 && ctx.cancelled())
                return error("任务已取消", "boxes", "cancelled");
              Vec3 position;
              position[axis] = -half[axis] + 2 * half[axis] * k / steps;
              position[b] = sb * half[b];
              position[c] = sc * half[c];
              append(wire, wireIndices, position, normal, r, t);
            }
          }
      }
      const auto id = item["id"].get<std::string>();
      groups.push_back({id, start, cloud.pointCount() - start});
      wireGroups.push_back({id, wireStart, wire.pointCount() - wireStart});
      metadata.push_back({{"id", id},
                          {"sourcePose", item},
                          {"visibleFaces", faces},
                          {"pointCount", cloud.pointCount() - start},
                          {"wirePointCount", wire.pointCount() - wireStart}});
      ctx.progress(static_cast<float>(work) / std::max<std::uint64_t>(1, budget));
    }
    const std::string frame =
        coordinates == "pose" ? record->data["frame"].get<std::string>() : "box_local";
    Record info{"geometry.BoxClouds.v1",
                {{"frame", frame},
                 {"unit", "m"},
                 {"coordinates", coordinates},
                 {"sampling", mode},
                 {"seed", seed},
                 {"measured", false},
                 {"items", metadata}}};
    if (!values::boundedJson(info.data, why, true))
      return error("盒体元数据超过预算：" + why, "boxes", "resource_limit");
    if (ctx.cancelled())
      return error("任务已取消", "boxes", "cancelled");
    auto grouping = cloud_groups::make(cloud, groups, frame);
    auto wireGrouping = cloud_groups::make(wire, wireGroups, frame);
    out.set("cloud", Data::cloud(std::move(cloud)));
    out.set("indices", Data::indices(std::move(indices)));
    out.set("groups", Data::record(std::move(grouping)));
    out.set("wireframe", Data::cloud(std::move(wire)));
    out.set("wireIndices", Data::indices(std::move(wireIndices)));
    out.set("wireGroups", Data::record(std::move(wireGrouping)));
    out.set("quality", Data::record(std::move(info)));
    return Status::Ok();
  } catch (const PrecisionLoss&) {
    return error("盒体无法保真交付float32：角点须互异且量化误差不超过最小边长的1e-4", "boxes",
                 "precision_loss");
  } catch (const std::bad_alloc&) {
    return error("盒体采样内存不足", "boxes", "resource_limit");
  } catch (const std::exception& e) {
    return error(e.what(), "boxes");
  }
}
}

void registerP5CloudInspection(Registry& r) {
  auto op =
      p1::descriptor("cloud.box_sample", "长方体点云采样",
                     "generate box cloud surface volume wireframe visible faces deterministic");
  op.inputs = {withContract(Port{"boxes", "Record", "长方体", "米制三维位姿集合", true},
                            {{"recordType", poses::kPoses3DType}})};
  op.outputs = {{"cloud", "PointCloud", "采样云", "合成表面或体积点", true},
                {"indices", "Indices", "采样索引", "与groups对应", true},
                {"groups", "Record", "盒体分组", cloud_groups::kType, true},
                {"wireframe", "PointCloud", "线框云", "有界边采样，不是Mesh", true},
                {"wireIndices", "Indices", "线框索引", "与wireGroups对应", true},
                {"wireGroups", "Record", "线框分组", cloud_groups::kType, true},
                {"quality", "Record", "采样信息", "geometry.BoxClouds.v1", true}};
  op.params = {
      p1::choice("sampling", "采样模式", "surface", {{"surface", "表面"}, {"volume", "体积"}}),
      p1::choice("coordinates", "输出坐标", "pose",
                 {{"pose", "应用位姿"}, {"local", "各盒体局部"}}),
      p1::integer("pointsPerFace", "每面点数", 1000, 1, 10000),
      p1::integer("seed", "确定性种子", 7, 0, 2147483647),
      p1::number("wireSpacing", "线框间距", .002, 1e-9, 1e6, "m"),
      flag("visibleOnly", "仅可见面", false),
      p1::number("minVisibleAngle", "最小可见角", 0, 0, 89, "deg"),
      p1::vector("viewPoint", "观察点", {0, 0, 0}),
      flag("normals", "生成表面法线", true),
      flag("colors", "生成RGB", true),
      p1::vector("color", "RGB颜色", {64, 192, 255})};
  op.doc =
      "长方体中心锚定；每面确定性均匀采样或6倍每面点数的体积采样，输出独立云与稳定ID分组。可见面按观察点/法线判定。local坐标需逐组使用quality.sourcePose解释，不能作为统一世界坐标；不生成网格或真实观测。";
  op.compute = boxes;
  r.addOperator(std::move(op));
}
}
