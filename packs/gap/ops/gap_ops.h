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

/// ROI 框专用：**先窄化成 float 再除**（`Matrix2f << 双精度…; matrix /= scale_` 的写法）。
/// 与 mmToM 差最后一个 ULP，而严格开区间的裁剪正好卡在这一位上 —— 框一律走这一条，
/// 同一个配置值在不同算子里才落在同一个 float 上。
inline float mmToMRoi(double mm) { return static_cast<float>(mm) / 1000.0F; }

/// Line2D 方向的统一朝向：朝 +x，竖直（x 分量为 0）时朝 +y；零向量退成 (1, 0)。
/// 直线本身没有正反，但下游拿 dir 定法线（gap.flush 的符号），所以每条出端口的线都要
/// 走这里 —— RANSAC 给出的系数方向是随机的。
inline Eigen::Vector2d canonicalLineDir(double dx, double dy) {
  const double len = std::hypot(dx, dy);
  if (!(len > 0)) return Eigen::Vector2d(1.0, 0.0);
  Eigen::Vector2d u(dx / len, dy / len);
  if (u.x() < 0 || (u.x() == 0 && u.y() < 0)) u = -u;
  return u;
}

/// LyFlow 点云 → PCL PointXYZRGB。rgb 通道按原算法的约定进 r/g/b（强度在 r 上）。
GapCloud toPcl(const lyflow::PointCloud& cloud);

/// PCL → LyFlow。始终带上 rgb 通道，下游的强度门限才有东西可用。
lyflow::PointCloud fromPcl(const GapCloud& cloud);

/// 从磁盘读一个 PCD 成 PCL 点云。失败返回 false，message 是一句人话。
bool loadPcd(const std::string& path, GapCloud* out, std::string* message);

/// 非有限点原地剔除（对应 NonFinitePointPolicy::kRemove）。
void removeNonFinite(GapCloud* cloud);

/// Box2D（米）→ 原算法的 ROI 矩阵：col(0)=min，col(1)=max。
Eigen::Matrix2f toRoiMatrix(const lyflow::Box2D& box);

/// 四个数（毫米，[x_min, y_min, x_max, y_max]）→ Box2D（米）。
lyflow::Box2D boxFromMm(double xMin, double yMin, double xMax, double yMax);

/// Eigen 的 3x3 SE(2) 变换 ↔ JSON 数组（行主序 9 个数），Record 里存变换用。
std::vector<double> transformToJson(const Eigen::Matrix3f& m);
Eigen::Matrix3f transformFromJson(const nlohmann::json& j);

/// 把一个 Measurement 塞进输出端口，顺手写一条日志。
void setMeasurement(Outputs& outputs, const char* port, double valueMm, bool ok,
                    const std::string& message);

/// 文件大小 + mtime，读不到返回空串。externalKey 与模型缓存键都用它。
std::string fileStamp(const std::filesystem::path& p);

/// 按「模型路径 + mtime」缓存的 OnnxRoiPredictor（H2）。构造失败返回 nullptr、
/// message 是一句人话；指针归缓存所有，调用方不接管。
const ::gap::ml::OnnxRoiPredictor* predictorFor(const std::filesystem::path& model,
                                                std::string* message);

/// 原始 1280 槽的传感器帧点云 → 模型输入行。点数不对返回 false。
bool profileRowOf(const lyflow::PointCloud& cloud, ::gap::ml::ProfileRow* out);

/// 端口样例（m6-plan H8）。每一份都是从一次**真实 run** 的输出裁出来的：
/// R1 那张带 12 个 fallback 的导入图（KUN10 的一帧）与点 1 的 notch 图。
/// 只有 `type` 字串的 Record 端口，「`data.inlierCount` 到底存不存在」得翻算子实现
/// 才知道 —— 一份样例就够消掉这一次试错。样例不参与任何校验。
/// 值都圆过（真实浮点尾巴对读者没有信息），数组过长的截过，见 port_examples.cpp。
namespace examples {
nlohmann::json fitQuality();
nlohmann::json fitQualityPair();
nlohmann::json labels();
nlohmann::json refinements();
nlohmann::json rollCrop();
nlohmann::json alignment();
nlohmann::json cornerQuality();
nlohmann::json grooveQuality();
nlohmann::json notchQuality();
nlohmann::json cameraConsistency();
nlohmann::json resultBundle();
}  // namespace examples

// ------------------------------------------------------------------ 注册函数
void registerLoadProfilePair(Registry& r);
void registerToMeasurementFrame(Registry& r);
void registerDatumWindow(Registry& r);
void registerOverallRoi(Registry& r);
void registerLoadTemplate(Registry& r);
void registerAlignTemplate(Registry& r);
void registerSelectAlignment(Registry& r);
void registerBusinessRois(Registry& r);
void registerFitLine(Registry& r);
void registerSelectedPoint(Registry& r);
void registerNearestToLine(Registry& r);
void registerFitGapCircles(Registry& r);
void registerFlush(Registry& r);
void registerGap(Registry& r);
void registerCornerVertex(Registry& r);
void registerGrooveJoint(Registry& r);
void registerNotchWidth(Registry& r);
void registerPointOffset(Registry& r);
void registerCameraGuard(Registry& r);
void registerJudge(Registry& r);
void registerMeasureReference(Registry& r);
void registerProfileTensor(Registry& r);
void registerLabelsFromLogits(Registry& r);
void registerRoiFromLabels(Registry& r);
void registerLabelsToCloud(Registry& r);
void registerDropNonFinite(Registry& r);
void registerRollAnchoredCrop(Registry& r);
void registerResultBundle(Registry& r);
void registerStandardGapImporter(Registry& r);

}  // namespace lyflow::packs::gap
