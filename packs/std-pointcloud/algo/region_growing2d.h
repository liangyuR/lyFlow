#pragma once
// XY 平面上按法线夹角的区域生长，以及「取最大两簇、按 x 均值分左右」。
//
// 行为基准是 xyz-gap-inspector 标定里的 `calibration/split.rs`（C++ 时代 circle_fit.cpp 的
// splitAndExtractCloud：二维法线估计 + pcl::RegionGrowing），逐步复刻、逐位一致：
//
// 1. 法线：每个点取 k 个最近邻（含自己；平方距离按 float 算，升序，等距按下标），x-y 均值与协方差
//    （float 乘积按 double 累加，与 PCL computeMeanAndCovarianceMatrix 同序），PCL eigen22 取最小特征值
//    与特征向量作法线，曲率 = |最小特征值 / 迹|；法线 y < 0 时翻转（比较取 |cos|，翻转不影响分簇）。
//    eigen22 在互协方差近 0 时的分支保留 PCL 的原样（恒取特征值 = 互协方差、向量 (0, 1)），不要「修正」。
// 2. 种子按曲率升序（稳定），广度优先生长：队首种子的每个近邻（按距离升序）未分配且
//    |n_邻 · n_种子| ≥ cos(阈值) 就并入并入队（曲率 ≤ 曲率阈值才再当种子）。
// 3. 簇按 [minClusterSize, maxClusterSize] 筛；簇内下标升序。
// 4. 簇数 ≥ 2 时按点数降序（稳定）取最大两簇，x 均值小的是左。
//
// 点数少于 k（或 k < 2）时估不了法线，与 C++ 一样一个簇都不出。
// 云里不能有非有限点（算子的输入契约保证；直接调用的人自己负责）。
// 暴力 knn：标定的云最多一条剖面（1280 点），用不着 kd 树，而且先后是确定的。
#include <cstddef>
#include <cstdint>
#include <vector>

namespace lyflow::std_pc {

struct RegionGrowing2DOptions {
  /// 估法线的近邻数，同时是生长时看的近邻数（PCL setNumberOfNeighbours）。
  int neighbors = 50;
  /// 平滑阈值，度。按 float 收下再取 float 的 cos，与 PCL setSmoothnessThreshold(float) 一致。
  double angleThresholdDeg = 50.0;
  std::size_t minClusterSize = 100;
  std::size_t maxClusterSize = 100000;
  /// 曲率大于它的点并入簇但不再当种子。二维曲率最大 0.5，默认 1 即每个点都当种子。
  float curvatureThreshold = 1.0f;
};

struct RegionGrowing2DResult {
  /// 筛过大小的簇，按生长顺序；每簇的下标升序。
  std::vector<std::vector<std::int32_t>> clusters;
  /// 簇数 ≥ 2 时是最大两簇里 x 均值小 / 大的那个；否则都空。
  std::vector<std::int32_t> left;
  std::vector<std::int32_t> right;
};

/// xy 交错存放（x0, y0, x1, y1, ...），共 pointCount 个点。
RegionGrowing2DResult regionGrowing2D(const float* xy, std::size_t pointCount,
                                      const RegionGrowing2DOptions& options);

/// PCL eigen22：mat 按列主序 [c0, c1, c2, c3]（对称）。返回最小特征值，vector 写特征向量。
/// 导出只是为了测那条对角分支。
double eigen22(const double mat[4], double vector[2]);

} // namespace lyflow::std_pc
