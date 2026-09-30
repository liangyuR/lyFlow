#include "lyflow/registry.h"
#include "ops/ops.h"

namespace lyflow {
namespace test {
// core/tests/e2e/register_e2e_ops.cpp：只在 LYFLOW_TEST_OPS=1 时注册编辑器 e2e 用的测试算子。
void registerE2eOps(Registry& r);
}  // namespace test

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

  r.addType(PortType{
      "Tensor", "#f0b429", {},
      "稠密 float32 张量，行主序。推理算子的输入输出；只有形状与统计量进 Inspector。"});

  r.addType(PortType{
      "Error", "#e5484d", {},
      "一条失败的 Status。只出现在声明了 acceptsError 的输入端口上（ADR-0016）。"});

  // m8-plan L1。端口上一律写成 `Bundle<kind>`，kind 由算子包在 manifest 的 bundles 段声明；
  // 这一项只给所有 Bundle 端口一个共同的颜色，裸的 "Bundle" 不能当端口类型用。
  r.addType(PortType{
      "Bundle", "#c8a86b", {},
      "一组有名字的字段（Bundle<kind>）：一根线带一组有关系的数据，字段表见 manifest 的 bundles。"});

  // docs/image-plan.md I2：一个类型，通道数与位深是运行时属性（同点云的可选通道）；
  // 要求几个通道的端口用 shape 契约声明（图像按 [高, 宽, 通道] 读，ADR-0026）。
  r.addType(PortType{
      "Image", "#2dd4bf", {},
      "2D 图像：行主序、通道交错，通道 1/3/4（灰度 / RGB / RGBA），位深 u8/u16/f32。"
      "像素走二进制通道，Inspector 只看尺寸与逐通道统计。"});
}

}  // namespace

void registerBuiltinOps(Registry& r) {
  registerBuiltinTypes(r);

  // core 只剩两个算子（S1 / ADR-0014）。点云算法在 packs/std-pointcloud/。
  // 下面的夹心注册顺序钉住 manifest 的字节兼容性，理由见 ADR-0014。
  ops::registerGenSynthetic(r);
  registerStdPacks(r);
  ops::registerUtilReroute(r);
  ops::registerFlowOps(r);
  registerExternalPacks(r);
  // 排在最后：不设 LYFLOW_TEST_OPS 时什么都不注册，manifest 与之前逐字节相同
  test::registerE2eOps(r);
}

}  // namespace lyflow
