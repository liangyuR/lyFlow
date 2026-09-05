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
}

}  // namespace

void registerBuiltinOps(Registry& r) {
  registerBuiltinTypes(r);

  // 加新算子在这里加一行，前端不用动（ADR-0003）。
  // 顺序即面板里同分类下的排列顺序，按「一条 pipeline 从左到右」排。
  ops::registerGenSynthetic(r);
  ops::registerIoLoadPcd(r);
  ops::registerIoSavePcd(r);

  ops::registerFilterPassthrough(r);
  ops::registerFilterVoxelGrid(r);
  ops::registerFilterCropBox(r);
  ops::registerFilterRandomSample(r);
  ops::registerFilterStatisticalOutlier(r);
  ops::registerFilterRadiusOutlier(r);

  ops::registerFeaturesNormals(r);

  ops::registerSegmentRansacPlane(r);
  ops::registerSegmentExtractIndices(r);

  ops::registerTransformMake(r);
  ops::registerTransformApply(r);
  ops::registerUtilMerge(r);
  ops::registerUtilReroute(r);
}

}  // namespace lyflow
