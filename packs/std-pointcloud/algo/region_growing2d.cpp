#include "algo/region_growing2d.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>
#include <numeric>

namespace lyflow::std_pc {
namespace {

constexpr double kPi = 3.14159265358979323846;

struct Normal {
  float x;
  float y;
  float curvature;
};

/// Rust f32::total_cmp 的键：按它比较就是 IEEE 754 totalOrder（NaN 有确定的位置）。
std::int32_t totalOrderKey(float value) {
  std::int32_t bits;
  std::memcpy(&bits, &value, sizeof bits);
  bits ^= static_cast<std::int32_t>(static_cast<std::uint32_t>(bits >> 31) >> 1);
  return bits;
}

/// 每个点的 k 个最近邻（含自己），平方距离按 float 算，升序，等距按下标。n ≥ k 由调用方保证。
std::vector<std::int32_t> neighbourTable(const float* xy, std::size_t n, std::size_t k) {
  std::vector<std::int32_t> table(n * k);
  std::vector<std::pair<float, std::int32_t>> distances(n);
  for (std::size_t i = 0; i < n; ++i) {
    const float qx = xy[2 * i];
    const float qy = xy[2 * i + 1];
    for (std::size_t j = 0; j < n; ++j) {
      const float dx = xy[2 * j] - qx;
      const float dy = xy[2 * j + 1] - qy;
      distances[j] = {dx * dx + dy * dy, static_cast<std::int32_t>(j)};
    }
    // 有限点的平方距离 ≥ +0，pair 的字典序就是 split.rs 的 (total_cmp, 下标)。
    std::partial_sort(distances.begin(), distances.begin() + static_cast<std::ptrdiff_t>(k),
                      distances.end());
    for (std::size_t m = 0; m < k; ++m)
      table[i * k + m] = distances[m].second;
  }
  return table;
}

/// 邻域 x-y 协方差（PCL computeMeanAndCovarianceMatrix）：乘积按 float 算再按 double 累加。
/// 邻域里一个有限点都没有返回 false。
bool meanAndCovariance(const float* xy, const std::int32_t* indices, std::size_t count,
                       double covariance[4]) {
  double accu[5] = {0, 0, 0, 0, 0};
  std::size_t finite = 0;
  for (std::size_t m = 0; m < count; ++m) {
    const float x = xy[2 * static_cast<std::size_t>(indices[m])];
    const float y = xy[2 * static_cast<std::size_t>(indices[m]) + 1];
    if (!std::isfinite(x) || !std::isfinite(y))
      continue;
    ++finite;
    accu[0] += static_cast<double>(x * x);
    accu[1] += static_cast<double>(x * y);
    accu[2] += static_cast<double>(y * y);
    accu[3] += static_cast<double>(x);
    accu[4] += static_cast<double>(y);
  }
  if (finite == 0)
    return false;
  const double n = static_cast<double>(finite);
  for (double& value : accu)
    value /= n;
  const double c1 = accu[1] - accu[3] * accu[4];
  covariance[0] = accu[0] - accu[3] * accu[3];
  covariance[1] = c1;
  covariance[2] = c1;
  covariance[3] = accu[2] - accu[4] * accu[4];
  return true;
}

std::vector<Normal> estimateNormals(const float* xy, std::size_t n,
                                    const std::vector<std::int32_t>& table, std::size_t k) {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  std::vector<Normal> normals(n);
  for (std::size_t i = 0; i < n; ++i) {
    double covariance[4];
    if (k < 2 || !meanAndCovariance(xy, table.data() + i * k, k, covariance)) {
      normals[i] = {nan, nan, nan};
      continue;
    }
    double vector[2];
    const double eigenvalue = eigen22(covariance, vector);
    const double eigSum = covariance[0] + covariance[3];
    const double curvature = eigSum != 0.0 ? std::abs(eigenvalue / eigSum) : 0.0;
    Normal normal{static_cast<float>(vector[0]), static_cast<float>(vector[1]),
                  static_cast<float>(curvature)};
    if (normal.y < 0.0f) {
      normal.x = -normal.x;
      normal.y = -normal.y;
    }
    normals[i] = normal;
  }
  return normals;
}

/// PCL RegionGrowing 的平滑模式。返回按生长顺序的簇（每簇下标升序），未按大小筛。
std::vector<std::vector<std::int32_t>> growRegions(const std::vector<std::int32_t>& table,
                                                   std::size_t k,
                                                   const std::vector<Normal>& normals,
                                                   const RegionGrowing2DOptions& options) {
  const std::size_t count = normals.size();
  // setSmoothnessThreshold(float)：double 里算 angle / 180 * π 再按 float 收下；
  // validatePoint 里的 std::cos(theta_threshold_) 是 float 的 cos。
  const float theta = static_cast<float>(options.angleThresholdDeg / 180.0 * kPi);
  const float cosineThreshold = std::cos(theta);

  std::vector<std::size_t> order(count);
  std::iota(order.begin(), order.end(), std::size_t{0});
  std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
    return totalOrderKey(normals[a].curvature) < totalOrderKey(normals[b].curvature);
  });

  std::vector<std::int32_t> labels(count, -1);
  std::vector<std::size_t> sizes;
  std::size_t segmented = 0;
  std::size_t seedCounter = 0;
  bool haveSeed = count > 0;
  std::size_t seed = haveSeed ? order[0] : 0;
  while (segmented < count) {
    if (!haveSeed)
      break;
    const auto segment = static_cast<std::int32_t>(sizes.size());

    std::deque<std::size_t> queue;
    queue.push_back(seed);
    labels[seed] = segment;
    std::size_t inSegment = 1;
    while (!queue.empty()) {
      const std::size_t current = queue.front();
      queue.pop_front();
      const std::int32_t* neighbours = table.data() + current * k;
      for (std::size_t m = 0; m < k; ++m) {
        const auto index = static_cast<std::size_t>(neighbours[m]);
        if (labels[index] != -1)
          continue;
        // 邻点与**当前种子**的法线夹角。
        const float dot = std::abs(normals[index].x * normals[current].x +
                                   normals[index].y * normals[current].y + 0.0f * 0.0f);
        if (dot < cosineThreshold)
          continue;
        labels[index] = segment;
        ++inSegment;
        // 曲率 > 阈值就不再当种子；NaN 比较为假，也当种子。
        const bool tooCurved = normals[index].curvature > options.curvatureThreshold;
        if (!tooCurved)
          queue.push_back(index);
      }
    }
    segmented += inSegment;
    sizes.push_back(inSegment);

    haveSeed = false;
    for (std::size_t position = seedCounter + 1; position < count; ++position) {
      if (labels[order[position]] == -1) {
        seed = order[position];
        seedCounter = position;
        haveSeed = true;
        break;
      }
    }
  }

  std::vector<std::vector<std::int32_t>> clusters(sizes.size());
  for (std::size_t s = 0; s < sizes.size(); ++s)
    clusters[s].reserve(sizes[s]);
  for (std::size_t index = 0; index < count; ++index) {
    if (labels[index] >= 0)
      clusters[static_cast<std::size_t>(labels[index])].push_back(static_cast<std::int32_t>(index));
  }
  return clusters;
}

} // namespace

double eigen22(const double mat[4], double vector[2]) {
  if (std::abs(mat[1]) <= std::numeric_limits<double>::min()) {
    // PCL 原样：拿 c0 去和 c2（互协方差）比，见头文件。
    if (mat[0] < mat[2]) {
      vector[0] = 1.0;
      vector[1] = 0.0;
      return mat[0];
    }
    vector[0] = 0.0;
    vector[1] = 1.0;
    return mat[2];
  }
  const double trace = 0.5 * (mat[0] + mat[3]);
  const double determinant = mat[0] * mat[3] - mat[1] * mat[1];
  double temp = trace * trace - determinant;
  if (temp < 0.0)
    temp = 0.0;
  const double eigenvalue = trace - std::sqrt(temp);
  vector[0] = -mat[1];
  vector[1] = mat[0] - eigenvalue;
  const double norm = std::sqrt(vector[0] * vector[0] + vector[1] * vector[1]);
  vector[0] /= norm;
  vector[1] /= norm;
  return eigenvalue;
}

RegionGrowing2DResult regionGrowing2D(const float* xy, std::size_t pointCount,
                                      const RegionGrowing2DOptions& options) {
  RegionGrowing2DResult result;
  if (options.neighbors < 2 || pointCount < static_cast<std::size_t>(options.neighbors))
    return result;
  const auto k = static_cast<std::size_t>(options.neighbors);

  const std::vector<std::int32_t> table = neighbourTable(xy, pointCount, k);
  const std::vector<Normal> normals = estimateNormals(xy, pointCount, table, k);
  for (auto& cluster : growRegions(table, k, normals, options)) {
    if (cluster.size() >= options.minClusterSize && cluster.size() <= options.maxClusterSize)
      result.clusters.push_back(std::move(cluster));
  }
  if (result.clusters.size() < 2)
    return result;

  // 点数降序（稳定）取最大两簇；x 均值小的是左。
  std::vector<std::size_t> bySize(result.clusters.size());
  std::iota(bySize.begin(), bySize.end(), std::size_t{0});
  std::stable_sort(bySize.begin(), bySize.end(), [&](std::size_t a, std::size_t b) {
    return result.clusters[a].size() > result.clusters[b].size();
  });
  const auto meanX = [&](const std::vector<std::int32_t>& cluster) {
    double sum = 0.0;
    for (const std::int32_t i : cluster)
      sum += static_cast<double>(xy[2 * static_cast<std::size_t>(i)]);
    return sum / static_cast<double>(cluster.size());
  };
  const auto& biggest = result.clusters[bySize[0]];
  const auto& second = result.clusters[bySize[1]];
  const bool biggestIsLeft = meanX(biggest) < meanX(second);
  result.left = biggestIsLeft ? biggest : second;
  result.right = biggestIsLeft ? second : biggest;
  return result;
}

} // namespace lyflow::std_pc
