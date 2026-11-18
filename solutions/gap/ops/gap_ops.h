#pragma once
// gap 算子包的公共部分：注册函数声明、LyFlow ↔ PCL 点云互转、mm/m 换算。
// 端口上的坐标一律是米；参数一律是毫米（G4，与 StandardGap.yml 一致）。
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include <Eigen/Core>

#include "domain/detection/DetectionTypes.hpp"
#include "lyflow/data.h"
#include "lyflow/operator.h"
#include "lyflow/registry.h"

namespace lyflow::packs::gap {

// DetectionTypes.hpp 在全局作用域里定义了 PointCloud / PointT，
// 而 lyflow 命名空间里也有一个 PointCloud —— 起个别名，两边都不用写全限定。
using GapCloud = ::PointCloud;
using GapPoint = ::PointT;

/// 配置里的毫米 → 端口上的米。原算法一律用 scale_=1000 做这件事。
constexpr double kScale = 1000.0;
inline float mmToM(double mm) { return static_cast<float>(mm / kScale); }
inline double mToMm(double m) { return m * kScale; }

/// LyFlow 点云 → PCL PointXYZRGB。rgb 通道按原算法的约定进 r/g/b（强度在 r 上）。
GapCloud toPcl(const lyflow::PointCloud& cloud);

/// PCL → LyFlow。始终带上 rgb 通道，下游的强度门限才有东西可用。
lyflow::PointCloud fromPcl(const GapCloud& cloud);

/// Box2D（米）→ 原算法的 ROI 矩阵：col(0)=min，col(1)=max。
Eigen::Matrix2f toRoiMatrix(const lyflow::Box2D& box);

/// 把一个 Measurement 塞进输出端口，顺手写一条日志。
void setMeasurement(Outputs& outputs, const char* port, double valueMm, bool ok,
                    const std::string& message);

// ------------------------------------------------------------------ 注册函数
void registerPatrolPolicy(Registry& r);
void registerResultBundle(Registry& r);
void registerStandardGapImporter(Registry& r);
void registerSnippets(Registry& r);

} // namespace lyflow::packs::gap
