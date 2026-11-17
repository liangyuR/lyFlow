#pragma once
// gap 算子包的公共部分：注册函数声明、LyFlow ↔ PCL 点云互转、mm/m 换算。
// 端口上的坐标一律是米；参数一律是毫米（G4，与 StandardGap.yml 一致）。
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include <Eigen/Core>

#include "domain/detection/DetectionTypes.hpp"
#include "gap_ml/OnnxRoiPredictor.hpp"
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

/// Eigen 的 3x3 SE(2) 变换 → JSON 数组（行主序 9 个数），Record 里存变换用。
std::vector<double> transformToJson(const Eigen::Matrix3f& m);

/// 把一个 Measurement 塞进输出端口，顺手写一条日志。
void setMeasurement(Outputs& outputs, const char* port, double valueMm, bool ok,
                    const std::string& message);

/// 文件大小 + mtime，读不到返回空串。externalKey 与模型缓存键都用它。
std::string fileStamp(const std::filesystem::path& p);

/// 按「模型路径 + mtime」缓存的 OnnxRoiPredictor（H2）。构造失败返回 nullptr、
/// message 是一句人话；指针归缓存所有，调用方不接管。
const ::gap::ml::OnnxRoiPredictor* predictorFor(const std::filesystem::path& model,
                                                std::string* message);

// ------------------------------------------------------------------ 注册函数
void registerJudge(Registry& r);
void registerMeasureReference(Registry& r);
void registerResultBundle(Registry& r);
void registerStandardGapImporter(Registry& r);
void registerSnippets(Registry& r);
void registerGraphMigrations(Registry& r);

} // namespace lyflow::packs::gap
