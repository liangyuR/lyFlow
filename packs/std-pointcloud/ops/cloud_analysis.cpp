#include <Eigen/Eigenvalues>
#include <pcl/search/kdtree.h>
#include <pcl/segmentation/extract_clusters.h>
#include <pcl/segmentation/sac_segmentation.h>
#include <iomanip>
#include <set>
#include <sstream>

#include "p1_common.h"
#include "lyflow_pcl/adapter.h"

namespace lyflow::ops {
namespace {
using namespace p1;

struct Analysis {
  Vec3 center = Vec3::Zero(), minimum = Vec3::Zero(), maximum = Vec3::Zero();
  Vec3 eigenvalues = Vec3::Zero();
  Mat3 axes = Mat3::Identity();
  bool stable = false;
};

Analysis analyze(const PointCloud& cloud, const std::vector<std::int32_t>& keep) {
  Analysis a;
  if (keep.empty())
    return a;
  a.minimum = a.maximum = point(cloud, keep.front());
  for (auto index : keep) {
    const Vec3 p = point(cloud, index);
    a.center += p;
    a.minimum = a.minimum.cwiseMin(p);
    a.maximum = a.maximum.cwiseMax(p);
  }
  a.center /= static_cast<double>(keep.size());
  Mat3 covariance = Mat3::Zero();
  for (auto index : keep) {
    const Vec3 delta = point(cloud, index) - a.center;
    covariance += delta * delta.transpose();
  }
  covariance /= static_cast<double>(keep.size());
  Eigen::SelfAdjointEigenSolver<Mat3> solver(covariance);
  if (solver.info() != Eigen::Success)
    return a;
  for (int i = 0; i < 3; ++i) {
    a.eigenvalues[i] = std::max(0.0, solver.eigenvalues()[2 - i]);
    a.axes.col(i) = solver.eigenvectors().col(2 - i);
    Eigen::Index dominant = 0;
    a.axes.col(i).cwiseAbs().maxCoeff(&dominant);
    if (a.axes(dominant, i) < 0)
      a.axes.col(i) *= -1;
  }
  a.axes.col(2) = a.axes.col(0).cross(a.axes.col(1)).normalized();
  const double scale = a.eigenvalues[0];
  a.stable = scale > 0 && a.eigenvalues[1] > scale * 1e-8 &&
             a.eigenvalues[0] - a.eigenvalues[1] > scale * 1e-6 &&
             a.eigenvalues[1] - a.eigenvalues[2] > scale * 1e-6;
  return a;
}

std::vector<std::int32_t> allIndices(const PointCloud& cloud) {
  std::vector<std::int32_t> keep(cloud.pointCount());
  std::iota(keep.begin(), keep.end(), 0);
  return keep;
}

Record statistics(const PointCloud& cloud, const std::vector<std::int32_t>& keep,
                  const std::string& frame, bool oriented) {
  const Analysis a = analyze(cloud, keep);
  Record record;
  record.type = "cloud.Statistics.v1";
  record.data = {{"valid", !keep.empty()},
                 {"count", keep.size()},
                 {"frame", frame},
                 {"unit", "m"},
                 {"sourceCloudId", std::to_string(cloud.id)},
                 {"center", array(a.center)},
                 {"minimum", array(a.minimum)},
                 {"maximum", array(a.maximum)},
                 {"eigenvalues", array(a.eigenvalues)},
                 {"orientationStable", a.stable}};
  Mat4 pose = Mat4::Identity();
  Vec3 lo = a.minimum, hi = a.maximum;
  if (oriented && !keep.empty()) {
    lo = hi = a.axes.transpose() * (point(cloud, keep.front()) - a.center);
    for (auto index : keep) {
      const Vec3 local = a.axes.transpose() * (point(cloud, index) - a.center);
      lo = lo.cwiseMin(local);
      hi = hi.cwiseMax(local);
    }
    pose.topLeftCorner<3, 3>() = a.axes;
    pose.topRightCorner<3, 1>() = a.center + a.axes * ((lo + hi) * 0.5);
  } else {
    pose.topRightCorner<3, 1>() = (lo + hi) * 0.5;
  }
  record.data["dimensions"] = array(hi - lo);
  record.data["boxToFrame"] = matrixJson(pose);
  return record;
}

Status groupInputs(const Inputs& in, const PointCloud*& cloud, const Indices*& indices,
                   std::vector<cloud_groups::Group>& groups, std::string& frame) {
  cloud = in.get("cloud").asCloud();
  indices = in.get("indices").asIndices();
  const Record* metadata = in.get("groups").asRecord();
  if (!cloud || !indices || !metadata)
    return error("需要 PointCloud、Indices 和 cloud.Groups.v1 三个输入");
  Status s = validate(*cloud);
  if (!s.ok)
    return s;
  std::string why;
  if (!cloud_groups::parse(*metadata, *cloud, *indices, groups, frame, why))
    return error(why, "groups");
  return Status::Ok();
}

std::string groupId(std::size_t index) {
  std::ostringstream id;
  id << "group-" << std::setw(4) << std::setfill('0') << index;
  return id.str();
}

void emitGroups(const Data& cloud, const std::vector<std::vector<std::int32_t>>& members,
                const std::string& frame, Outputs& out) {
  Indices indices;
  indices.sourceCloudId = cloud.asCloud()->id;
  std::vector<cloud_groups::Group> groups;
  for (const auto& member : members) {
    groups.push_back({groupId(groups.size()), indices.values.size(), member.size()});
    indices.values.insert(indices.values.end(), member.begin(), member.end());
  }
  out.set("cloud", cloud);
  out.set("indices", Data::indices(std::move(indices)));
  out.set("groups", Data::record(cloud_groups::make(*cloud.asCloud(), groups, frame)));
}

std::vector<Port> groupPorts() {
  return {{"cloud", "PointCloud", "点云", "共享的来源点云。", true},
          {"indices", "Indices", "成员索引", "sourceCloudId 指向来源点云。", true},
          withContract(Port{"groups", "Record", "分组", "cloud.Groups.v1 区间和身份。", true},
                       {{"recordType", cloud_groups::kType}})};
}

bool sameChannels(const PointCloud& a, const PointCloud& b) {
  return a.pointCount() == 0 || b.pointCount() == 0 ||
         (a.hasIntensity() == b.hasIntensity() && a.hasNormals() == b.hasNormals() &&
          a.hasRgb() == b.hasRgb());
}

void append(PointCloud& result, const PointCloud& cloud) {
  result.xyz.insert(result.xyz.end(), cloud.xyz.begin(), cloud.xyz.end());
  result.intensity.insert(result.intensity.end(), cloud.intensity.begin(), cloud.intensity.end());
  result.normals.insert(result.normals.end(), cloud.normals.begin(), cloud.normals.end());
  result.rgb.insert(result.rgb.end(), cloud.rgb.begin(), cloud.rgb.end());
}

} // namespace

void registerCloudAnalysis(Registry& r) {
  for (const auto& mode :
       {std::string("statistics"), std::string("pca"), std::string("bounds_obb")}) {
    OperatorDesc op =
        descriptor("cloud." + mode,
                   mode == "statistics" ? "点云三维统计"
                   : mode == "pca"      ? "点云主成分分析"
                                        : "点云有向包围盒",
                   "point cloud statistics centroid height PCA oriented bounding box");
    op.inputs = {{"cloud", "PointCloud", "点云", "米制有限坐标。", true}};
    op.outputs = {{"statistics", "Record", "统计", "cloud.Statistics.v1。", true},
                  {"height", "Measurement", "最大高度", "输入帧的最大 Z，空云为无效测量。", true},
                  {"pose", "Transform", "包围盒坐标系", "box→输入帧；退化主轴由统计标记。", true}};
    op.params = {text("frame", "参考帧", "input")};
    op.doc =
        "全云统计采用double；PCA轴符号规范化并构造右手系；重复特征值/退化云以orientationStable=false声明，不承诺跨帧跟踪。空云valid=false。";
    op.compute = [mode](const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
      const auto& cloud = *in.get("cloud").asCloud();
      Status s = validate(cloud);
      if (!s.ok)
        return s;
      if (!frameValid(p.text("frame")))
        return error("参考帧不能为空且最多1024字节", "frame", "bad_param");
      const auto keep = allIndices(cloud);
      Record rec = statistics(cloud, keep, p.text("frame"), mode != "statistics");
      Transform pose;
      Mat4 matrix;
      const auto& json = rec.data["boxToFrame"];
      for (int i = 0; i < 16; ++i)
        matrix(i / 4, i % 4) = json[i].get<double>();
      if (!transform(matrix, pose))
        return error("包围盒超出原生变换范围");
      Measurement height;
      height.unit = "m";
      height.ok = !keep.empty();
      height.value = keep.empty() ? 0 : rec.data["maximum"][2].get<double>();
      height.message = keep.empty() ? "空点云无最大高度" : "";
      if (ctx.cancelled())
        return Status::Ok();
      out.set("statistics", Data::record(std::move(rec)));
      out.set("height", Data::measurement(std::move(height)));
      out.set("pose", Data::transform(pose));
      return Status::Ok();
    };
    r.addOperator(std::move(op));
  }

  OperatorDesc op = descriptor("cloud.group_pack", "点云分组构造", "point cloud group packing");
  op.inputs = {{"cloud", "PointCloud", "点云", "来源点云。", true}};
  op.outputs = groupPorts();
  op.params = {text("frame", "参考帧", "input")};
  op.doc = "将单云包装为一个分组；索引使用原生Indices，不在JSON中复制点数据。";
  op.compute = [](const Inputs& in, const ParamView& p, Outputs& out, ExecContext&) {
    const auto& cloud = *in.get("cloud").asCloud();
    Status s = validate(cloud);
    if (!s.ok)
      return s;
    if (!frameValid(p.text("frame")))
      return error("参考帧无效", "frame", "bad_param");
    emitGroups(in.get("cloud"), {allIndices(cloud)}, p.text("frame"), out);
    return Status::Ok();
  };
  r.addOperator(std::move(op));

  op = descriptor("cloud.group_get", "点云分组提取", "point cloud group extraction index");
  op.inputs = groupPorts();
  op.outputs = {{"cloud", "PointCloud", "点云", "保留所有可选通道的新点云。", true},
                {"indices", "Indices", "来源索引", "仍指向原始来源点云。", true}};
  op.params = {integer("index", "组索引", 0, 0, 4095)};
  op.compute = [](const Inputs& in, const ParamView& p, Outputs& out, ExecContext&) {
    const PointCloud* cloud;
    const Indices* indices;
    std::vector<cloud_groups::Group> groups;
    std::string frame;
    Status s = groupInputs(in, cloud, indices, groups, frame);
    if (!s.ok)
      return s;
    const auto index = p.integer("index");
    if (index < 0 || static_cast<std::size_t>(index) >= groups.size())
      return error("组索引越界", "index", "index_out_of_range");
    const auto& group = groups[static_cast<std::size_t>(index)];
    Indices selected;
    selected.sourceCloudId = cloud->id;
    selected.values.assign(indices->values.begin() + group.offset,
                           indices->values.begin() + group.offset + group.count);
    out.set("cloud", Data::cloud(cloud->select(selected.values)));
    out.set("indices", Data::indices(std::move(selected)));
    return Status::Ok();
  };
  r.addOperator(std::move(op));

  op = descriptor("cloud.group_subset", "点云分组子集", "point cloud group subset selection");
  op.inputs = groupPorts();
  op.outputs = groupPorts();
  op.params = {text("selection", "组索引JSON数组", "[]")};
  op.compute = [](const Inputs& in, const ParamView& p, Outputs& out, ExecContext&) {
    const PointCloud* cloud;
    const Indices* indices;
    std::vector<cloud_groups::Group> groups;
    std::string frame;
    Status s = groupInputs(in, cloud, indices, groups, frame);
    if (!s.ok)
      return s;
    const auto& text = p.text("selection");
    if (text.size() > 65536)
      return error("组索引列表过大", "selection", "bad_param");
    std::size_t openings = 0;
    for (char token : text)
      if (token == '{' || token == '}' || (token == '[' && ++openings > 1))
        return error("selection只接受一维整数JSON数组", "selection", "bad_param");
    const auto selection = nlohmann::json::parse(text, nullptr, false);
    if (!selection.is_array() || selection.size() > cloud_groups::kMaxGroups)
      return error("selection必须是最多4096项的组索引数组", "selection", "bad_param");
    Indices selected;
    selected.sourceCloudId = cloud->id;
    std::vector<cloud_groups::Group> subset;
    std::set<std::size_t> seen;
    for (const auto& item : selection) {
      if (!cloud_groups::nonnegativeInteger(item) || item.get<std::size_t>() >= groups.size())
        return error("组索引越界", "selection", "index_out_of_range");
      const auto index = item.get<std::size_t>();
      if (!seen.insert(index).second)
        return error("组子集不接受重复身份", "selection", "bad_param");
      const auto& group = groups[index];
      subset.push_back({group.id, selected.values.size(), group.count});
      selected.values.insert(selected.values.end(), indices->values.begin() + group.offset,
                             indices->values.begin() + group.offset + group.count);
    }
    out.set("cloud", in.get("cloud"));
    out.set("indices", Data::indices(std::move(selected)));
    out.set("groups", Data::record(cloud_groups::make(*cloud, subset, frame)));
    return Status::Ok();
  };
  r.addOperator(std::move(op));

  op = descriptor("cloud.group_merge", "点云分组合并", "point cloud group flatten merge");
  op.inputs = groupPorts();
  op.outputs = {
      {"cloud", "PointCloud", "合并点云", "按分组及原生索引顺序合并，保留重复点。", true}};
  op.compute = [](const Inputs& in, const ParamView&, Outputs& out, ExecContext&) {
    const PointCloud* cloud;
    const Indices* indices;
    std::vector<cloud_groups::Group> groups;
    std::string frame;
    Status s = groupInputs(in, cloud, indices, groups, frame);
    if (!s.ok)
      return s;
    out.set("cloud", Data::cloud(cloud->select(indices->values)));
    return Status::Ok();
  };
  r.addOperator(std::move(op));

  op = descriptor("cloud.group_concat", "点云集合拼接", "point cloud collection concatenation");
  op.inputs = {{"a", "PointCloud", "点云A", "同一参考帧的云。", true},
               {"b", "PointCloud", "点云B", "通道集合必须一致。", true},
               {"indicesA", "Indices", "A成员索引", "与groupsA成对连接，可拼接已有集合。", false},
               {"groupsA", "Record", "A分组", "可选cloud.Groups.v1，必须与声明帧相同。", false},
               {"indicesB", "Indices", "B成员索引", "与groupsB成对连接。", false},
               {"groupsB", "Record", "B分组", "可选cloud.Groups.v1。", false}};
  op.outputs = groupPorts();
  op.params = {text("frame", "共同参考帧", "input")};
  op.doc =
      "拼接同帧云或已有分组集合；可选groupsA/indicesA及groupsB/indicesB必须成对。组ID加a/、b/命名空间并保留成员顺序，来源索引重新绑定到拼接云。通道不一致报错。";
  op.compute = [](const Inputs& in, const ParamView& p, Outputs& out, ExecContext&) {
    const auto& a = *in.get("a").asCloud();
    const auto& b = *in.get("b").asCloud();
    Status s = validate(a);
    if (!s.ok)
      return s;
    s = validate(b);
    if (!s.ok)
      return s;
    if (!frameValid(p.text("frame")) || !sameChannels(a, b) ||
        a.pointCount() + b.pointCount() > static_cast<std::size_t>(INT32_MAX))
      return error("帧无效、通道集合不一致或总点数超过索引范围");
    std::vector<cloud_groups::Group> aGroups, bGroups;
    Indices aIndices, bIndices;
    for (bool first : {true, false}) {
      const auto& source = first ? a : b;
      auto& sourceGroups = first ? aGroups : bGroups;
      auto& sourceIndices = first ? aIndices : bIndices;
      const char* indicesPort = first ? "indicesA" : "indicesB";
      const char* groupsPort = first ? "groupsA" : "groupsB";
      if (in.has(indicesPort) != in.has(groupsPort))
        return error("已有集合必须同时连接groups和indices", groupsPort);
      if (in.has(groupsPort)) {
        const auto* metadata = in.get(groupsPort).asRecord();
        const auto* indices = in.get(indicesPort).asIndices();
        std::string frame, why;
        if (!metadata || !indices ||
            !cloud_groups::parse(*metadata, source, *indices, sourceGroups, frame, why) ||
            frame != p.text("frame"))
          return error("已有分组来源或帧不匹配：" + why, groupsPort);
        sourceIndices = *indices;
      } else {
        sourceIndices.sourceCloudId = source.id;
        sourceIndices.values = allIndices(source);
        sourceGroups.push_back({"group-0000", 0, source.pointCount()});
      }
    }
    if (aGroups.size() + bGroups.size() > cloud_groups::kMaxGroups)
      return error("拼接集合超过4096组", "groupsA", "resource_limit");
    PointCloud cloud;
    append(cloud, a);
    append(cloud, b);
    Indices indices;
    indices.sourceCloudId = cloud.id;
    std::vector<cloud_groups::Group> groups;
    for (bool first : {true, false}) {
      const auto& sourceGroups = first ? aGroups : bGroups;
      const auto& sourceIndices = first ? aIndices : bIndices;
      for (const auto& group : sourceGroups) {
        groups.push_back(
            {std::string(first ? "a/" : "b/") + group.id, indices.values.size(), group.count});
        for (std::size_t i = group.offset; i < group.offset + group.count; ++i)
          indices.values.push_back(sourceIndices.values[i] +
                                   (first ? 0 : static_cast<std::int32_t>(a.pointCount())));
      }
    }
    auto metadata = cloud_groups::make(cloud, groups, p.text("frame"));
    std::string why;
    if (!values::boundedJson(metadata.data, why))
      return error("拼接分组元数据超过限制：" + why, "groupsA", "resource_limit");
    out.set("cloud", Data::cloud(std::move(cloud)));
    out.set("indices", Data::indices(std::move(indices)));
    out.set("groups", Data::record(std::move(metadata)));
    return Status::Ok();
  };
  r.addOperator(std::move(op));

  op = descriptor("cloud.group_statistics", "点云逐组统计", "point cloud per cluster statistics");
  op.inputs = groupPorts();
  op.outputs = {
      {"statistics", "Record", "逐组统计", "cloud.GroupStatistics.v1，身份与输入组一致。", true}};
  op.compute = [](const Inputs& in, const ParamView&, Outputs& out, ExecContext& ctx) {
    const PointCloud* cloud;
    const Indices* indices;
    std::vector<cloud_groups::Group> groups;
    std::string frame;
    Status s = groupInputs(in, cloud, indices, groups, frame);
    if (!s.ok)
      return s;
    Record result;
    result.type = "cloud.GroupStatistics.v1";
    result.data = {{"frame", frame}, {"unit", "m"}, {"groups", nlohmann::json::array()}};
    for (const auto& group : groups) {
      if (ctx.cancelled())
        return Status::Ok();
      std::vector<std::int32_t> keep(indices->values.begin() + group.offset,
                                     indices->values.begin() + group.offset + group.count);
      auto item = statistics(*cloud, keep, frame, true).data;
      item["id"] = group.id;
      result.data["groups"].push_back(std::move(item));
    }
    std::string why;
    if (!values::boundedJson(result.data, why))
      return error("逐组统计超出小型Record限制：" + why, "groups");
    out.set("statistics", Data::record(std::move(result)));
    return Status::Ok();
  };
  r.addOperator(std::move(op));

  op = descriptor("segment.euclidean_clusters", "三维欧式聚类",
                  "3D euclidean clustering cluster size filtering");
  op.inputs = {{"cloud", "PointCloud", "点云", "三维有限点坐标。", true}};
  op.outputs = groupPorts();
  op.params = {number("tolerance", "邻域距离", 0.02, 1e-9, 1e6, "m"),
               integer("minPoints", "最小簇点数", 3, 1, INT32_MAX),
               integer("maxPoints", "最大簇点数", INT32_MAX, 1, INT32_MAX),
               integer("maxGroups", "最多簇数", 4096, 1, 4096), text("frame", "参考帧", "input")};
  op.doc =
      "真正XYZ欧式连通聚类；按规范点序计算的质心XYZ、点数及完整XYZ字典序排序分配局部稳定组ID。ID不代表跨帧跟踪。超过组数上限报错，不静默丢簇。";
  op.compute = [](const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
    const auto& cloud = *in.get("cloud").asCloud();
    Status s = validate(cloud);
    if (!s.ok)
      return s;
    if (p.integer("minPoints") > p.integer("maxPoints") || !frameValid(p.text("frame")))
      return error("簇点数范围或参考帧非法", "minPoints", "bad_param");
    std::vector<std::vector<std::int32_t>> members;
    if (cloud.pointCount()) {
      auto pclCloud = adapter::toPcl(cloud);
      auto tree = pcl::make_shared<pcl::search::KdTree<pcl::PointXYZ>>();
      tree->setInputCloud(pclCloud);
      pcl::EuclideanClusterExtraction<pcl::PointXYZ> extractor;
      extractor.setClusterTolerance(p.number("tolerance"));
      extractor.setMinClusterSize(static_cast<int>(p.integer("minPoints")));
      extractor.setMaxClusterSize(static_cast<int>(p.integer("maxPoints")));
      extractor.setSearchMethod(tree);
      extractor.setInputCloud(pclCloud);
      std::vector<pcl::PointIndices> clusters;
      extractor.extract(clusters);
      if (ctx.cancelled())
        return Status::Ok();
      if (clusters.size() > static_cast<std::size_t>(p.integer("maxGroups")))
        return error("有效簇数超过maxGroups", "maxGroups", "resource_limit");
      for (const auto& cluster : clusters) {
        members.push_back(adapter::fromPclIndices(cluster));
        std::sort(members.back().begin(), members.back().end());
      }
      auto pointLess = [&](std::int32_t a, std::int32_t b) {
        for (int axis = 0; axis < 3; ++axis) {
          const float av = cloud.xyz[static_cast<std::size_t>(a) * 3 + axis];
          const float bv = cloud.xyz[static_cast<std::size_t>(b) * 3 + axis];
          if (av != bv)
            return av < bv;
        }
        return false;
      };
      struct Key {
        Vec3 center;
        std::vector<std::int32_t> points;
        std::size_t index;
      };
      std::vector<Key> keys;
      keys.reserve(members.size());
      for (std::size_t i = 0; i < members.size(); ++i) {
        auto sorted = members[i];
        std::sort(sorted.begin(), sorted.end(), pointLess);
        Vec3 center = Vec3::Zero();
        for (auto index : sorted)
          center += point(cloud, static_cast<std::size_t>(index));
        center /= static_cast<double>(sorted.size());
        keys.push_back({center, std::move(sorted), i});
      }
      std::sort(keys.begin(), keys.end(), [&](const auto& a, const auto& b) {
        for (int i = 0; i < 3; ++i)
          if (a.center[i] != b.center[i])
            return a.center[i] < b.center[i];
        if (a.points.size() != b.points.size())
          return a.points.size() < b.points.size();
        return std::lexicographical_compare(a.points.begin(), a.points.end(), b.points.begin(),
                                            b.points.end(), pointLess);
      });
      std::vector<std::vector<std::int32_t>> ordered;
      ordered.reserve(members.size());
      for (const auto& key : keys)
        ordered.push_back(std::move(members[key.index]));
      members = std::move(ordered);
    }
    emitGroups(in.get("cloud"), members, p.text("frame"), out);
    return Status::Ok();
  };
  r.addOperator(std::move(op));

  op = descriptor("segment.multi_planes", "多平面顺序提取", "multiple plane RANSAC segmentation");
  op.inputs = {{"cloud", "PointCloud", "点云", "有限三维点。", true}};
  op.outputs = groupPorts();
  op.outputs.push_back(
      {"planes", "Record", "平面参数", "cloud.Planes.v1，系数和原生组ID对应。", true});
  op.outputs.push_back({"rest", "PointCloud", "剩余云", "保留可选通道。", true});
  op.params = {number("distanceThreshold", "平面距离阈值", 0.005, 1e-9, 1e3, "m"),
               integer("minPoints", "最小平面点数", 20, 3, INT32_MAX),
               integer("maxPlanes", "最多平面数", 16, 1, 4096),
               integer("maxIterations", "每平面RANSAC迭代", 500, 1, 100000),
               text("frame", "参考帧", "input")};
  op.compute = [](const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
    const auto& cloud = *in.get("cloud").asCloud();
    Status s = validate(cloud);
    if (!s.ok)
      return s;
    if (!frameValid(p.text("frame")))
      return error("参考帧非法", "frame", "bad_param");
    std::vector<std::int32_t> remaining = allIndices(cloud);
    std::vector<std::vector<std::int32_t>> members;
    Record planes;
    planes.type = "cloud.Planes.v1";
    planes.data = {{"frame", p.text("frame")}, {"unit", "m"}, {"planes", nlohmann::json::array()}};
    for (std::int64_t step = 0;
         step < p.integer("maxPlanes") &&
         remaining.size() >= static_cast<std::size_t>(p.integer("minPoints"));
         ++step) {
      if (ctx.cancelled())
        return Status::Ok();
      auto local = adapter::toPcl(cloud.select(remaining));
      pcl::SACSegmentation<pcl::PointXYZ> fit(false);
      fit.setInputCloud(local);
      fit.setOptimizeCoefficients(true);
      fit.setModelType(pcl::SACMODEL_PLANE);
      fit.setMethodType(pcl::SAC_RANSAC);
      fit.setDistanceThreshold(p.number("distanceThreshold"));
      fit.setMaxIterations(static_cast<int>(p.integer("maxIterations")));
      pcl::PointIndices selected;
      pcl::ModelCoefficients coefficients;
      fit.segment(selected, coefficients);
      if (selected.indices.size() < static_cast<std::size_t>(p.integer("minPoints")) ||
          coefficients.values.size() != 4)
        break;
      std::vector<bool> used(remaining.size(), false);
      std::vector<std::int32_t> group, rest;
      for (int index : selected.indices) {
        if (index < 0 || static_cast<std::size_t>(index) >= remaining.size())
          return error("平面求解返回越界索引");
        used[static_cast<std::size_t>(index)] = true;
        group.push_back(remaining[static_cast<std::size_t>(index)]);
      }
      for (std::size_t i = 0; i < remaining.size(); ++i)
        if (!used[i])
          rest.push_back(remaining[i]);
      Vec3 normal(coefficients.values[0], coefficients.values[1], coefficients.values[2]);
      const double norm = normal.norm();
      if (!normal.allFinite() || !(norm > 0) || !std::isfinite(coefficients.values[3]))
        return error("平面求解返回非法系数");
      double offset = coefficients.values[3] / norm;
      normal /= norm;
      Eigen::Index dominant = 0;
      normal.cwiseAbs().maxCoeff(&dominant);
      if (normal[dominant] < 0) {
        normal *= -1;
        offset *= -1;
      }
      std::sort(group.begin(), group.end());
      const Analysis a = analyze(cloud, group);
      planes.data["planes"].push_back({{"id", groupId(members.size())},
                                       {"normal", array(normal)},
                                       {"offset", offset},
                                       {"center", array(a.center)},
                                       {"count", group.size()}});
      members.push_back(std::move(group));
      remaining = std::move(rest);
    }
    std::vector<std::size_t> order(members.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
      const auto& pa = planes.data["planes"][a];
      const auto& pb = planes.data["planes"][b];
      for (const char* field : {"center", "normal"})
        for (int axis = 0; axis < 3; ++axis) {
          const double av = pa[field][axis].get<double>(), bv = pb[field][axis].get<double>();
          if (av != bv)
            return av < bv;
        }
      return pa["offset"].get<double>() < pb["offset"].get<double>();
    });
    std::vector<std::vector<std::int32_t>> orderedMembers;
    nlohmann::json orderedPlanes = nlohmann::json::array();
    for (std::size_t rank = 0; rank < order.size(); ++rank) {
      auto item = planes.data["planes"][order[rank]];
      item["id"] = groupId(rank);
      orderedPlanes.push_back(std::move(item));
      orderedMembers.push_back(std::move(members[order[rank]]));
    }
    planes.data["planes"] = std::move(orderedPlanes);
    emitGroups(in.get("cloud"), orderedMembers, p.text("frame"), out);
    out.set("planes", Data::record(std::move(planes)));
    out.set("rest", Data::cloud(cloud.select(remaining)));
    return Status::Ok();
  };
  r.addOperator(std::move(op));
}

} // namespace lyflow::ops
