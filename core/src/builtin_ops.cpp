#include "lyflow/registry.h"
#include "ops/ops.h"

namespace lyflow {
namespace {

// 端口类型表。前端拿 color 给端口和连线着色 —— 零成本、收益极高，
// 所以 M0 就做（见 docs/operator-manifest.md）。
//
// D10：这里**没有** PointCloudXYZI。intensity/normals/rgb 是 PointCloud 的
// 可选通道，不是另一个类型。原来那条 castableTo 看着很整齐，但它意味着
// 类型系统承诺了一件数据模型做不到的事 ——「XYZI 连到 XYZ 端口之后强度去哪了」
// 这个问题在 V1 的规则里根本没有答案。删掉比补一层隐式转换便宜得多。
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

  // 加新算子在这里加一行。前端不用动 —— 这是 ADR-0003 的承诺。
  //
  // 顺序即节点面板里同分类下的排列顺序，所以按「一条 pipeline 从左到右」排：
  // 生成/读入 → 滤波 → 特征 → 分割 → 变换 → 合并 → 写出。
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
}

}  // namespace lyflow
