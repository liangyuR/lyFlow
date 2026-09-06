#include "lyflow/registry.h"
#include "ops/ops.h"

namespace lyflow {
namespace {

// 端口类型表。color 供前端给端口与连线着色。
// D10：没有 PointCloudXYZI —— intensity/normals/rgb 是可选通道，不是另一个类型。
void registerBuiltinTypes(Registry& r) {
  r.addType(PortType{
      "Any", "#8a8f98", {},
      "通配类型。给 Reroute、Debug View 这类透传节点用，可与任意类型互连。"});

  r.addType(PortType{
      "PointCloud", "#4a9eff", {},
      "无序点云，至少含 XYZ；强度、法线、颜色是可选通道。"});

  r.addType(PortType{
      "Indices", "#c586c0", {},
      "点下标集合。指向某个点云，本身不含坐标。"});

  r.addType(PortType{
      "Transform", "#a0d030", {},
      "4x4 刚体变换矩阵。"});

  r.addType(PortType{
      "Plane", "#e0a030", {},
      "平面方程 n·p + d = 0，法向量已归一化。"});

  // 2D 量测域的通用载荷。坐标与点云同单位（米），3D 视图会为选中节点叠画它们。
  r.addType(PortType{
      "Box2D", "#e2725b", {},
      "轴对齐 2D 包围盒，XY 平面上的 min/max 两角。"});

  r.addType(PortType{
      "Line2D", "#5ec8c0", {},
      "2D 直线：过一点、带单位方向；可选带两个端点变成线段。"});

  r.addType(PortType{
      "Circle2D", "#7f9cf5", {},
      "2D 圆：圆心与半径。"});

  r.addType(PortType{
      "Point2D", "#d4a5f0", {},
      "XY 平面上的一个点。"});

  r.addType(PortType{
      "Measurement", "#4cd18a", {},
      "一次测量的结果：值、单位、是否成功、判定与上下限。"});

  r.addType(PortType{
      "Record", "#9aa5b1", {},
      "带类型标签的 JSON。算子包用它定义领域结构而不必改 core（ADR-0013）。"});
}

}  // namespace

void registerBuiltinOps(Registry& r) {
  registerBuiltinTypes(r);

  // core 只剩两个算子（S1 / ADR-0014）。点云算法在 packs/std-pointcloud/。
  // 这里的夹心顺序是为了让默认构建的 manifest 与拆包之前逐字节相同：
  // 合成数据源一直排在最前，reroute 一直排在标准包之后、外部包之前。
  ops::registerGenSynthetic(r);
  registerStdPacks(r);
  ops::registerUtilReroute(r);
  registerExternalPacks(r);
}

}  // namespace lyflow
