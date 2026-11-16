#include "lyflow_pcl/filters.h"

#include <cmath>
#include <limits>
#include <vector>

#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/search/kdtree.h>

#include "lyflow_pcl/adapter.h"
#include "lyflow_pcl/parallel.h"

namespace lyflow::pcl_support {
namespace {
Status bad(const char* message, const char* parameter = "") {
  return Status::Error(Phase::Execute, *parameter ? "bad_param" : "bad_input", message, parameter);
}
Status cancelled() { return Status::Error(Phase::Execute, "cancelled", "半径去噪已取消"); }
}
Status radiusOutlier(const PointCloud& input, double radius, std::int64_t minNeighbors,
                     RadiusSelection& out, ExecContext& ctx) {
  if (!std::isfinite(radius) || radius < 1e-6 || radius > 1000 || !std::isfinite(radius * radius))
    return bad("邻域半径必须为1e-6到1000的有限数", "radius");
  if (minNeighbors < 1 || minNeighbors > 10000 || minNeighbors >= std::numeric_limits<int>::max())
    return bad("最少邻居数必须为1到10000", "minNeighbors");
  if (input.xyz.size() % 3 != 0 || !input.channelsConsistent() ||
      input.pointCount() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
    return bad("输入点云坐标、通道或索引范围非法");
  if (ctx.cancelled())
    return cancelled();
  const auto n = input.pointCount();
  std::size_t finiteCount = 0;
  for (std::size_t i = 0; i < n; ++i) {
    if ((i & 8191U) == 0 && ctx.cancelled())
      return cancelled();
    bool finite = true;
    for (int axis = 0; axis < 3; ++axis) {
      const float value = input.xyz[3 * i + axis];
      if (std::isinf(value))
        return bad("点云包含无限坐标；缺失采样请显式使用NaN");
      finite = finite && std::isfinite(value);
    }
    finiteCount += finite;
  }
  std::vector<char> flags(n, 0);
  if (finiteCount > static_cast<std::size_t>(minNeighbors)) {
    ctx.progress(.1F, "构建 KD 树");
    auto cloud = ops::adapter::toPcl(input);
    pcl::search::KdTree<pcl::PointXYZ> tree(false);
    tree.setInputCloud(cloud);
    if (ctx.cancelled())
      return cancelled();
    const int minimum = static_cast<int>(minNeighbors);
    const int meanK = minimum + 1;
    const double maximumSquared = radius * radius;
    ctx.progress(.2F, "数邻居");
    ops::parallelFor(n, ctx.threadBudget(), [&](std::size_t begin, std::size_t end) {
      if (ctx.cancelled())
        return false;
      pcl::Indices neighbors(static_cast<std::size_t>(meanK));
      std::vector<float> squared(static_cast<std::size_t>(meanK));
      for (std::size_t i = begin; i < end; ++i) {
        const auto index = static_cast<pcl::index_t>(i);
        if (cloud->is_dense) {
          const int count = tree.nearestKSearch(index, meanK, neighbors, squared);
          flags[i] =
              count == meanK && !(maximumSquared < squared[static_cast<std::size_t>(meanK) - 1]);
        } else {
          const auto& point = (*cloud)[i];
          if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z))
            continue;
          const int count = tree.radiusSearch(index, radius, neighbors, squared,
                                              static_cast<unsigned int>(meanK));
          flags[i] = count > minimum;
        }
      }
      return true;
    });
    if (ctx.cancelled())
      return cancelled();
    ctx.progress(.9F);
  }
  RadiusSelection result;
  result.kept.sourceCloudId = result.removed.sourceCloudId = input.id;
  result.kept.values.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    if ((i & 8191U) == 0 && ctx.cancelled())
      return cancelled();
    (flags[i] ? result.kept.values : result.removed.values).push_back(static_cast<std::int32_t>(i));
  }
  if (ctx.cancelled())
    return cancelled();
  out = std::move(result);
  return Status::Ok();
}
}
