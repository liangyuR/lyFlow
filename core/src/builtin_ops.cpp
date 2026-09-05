#include "lyflow/registry.h"
#include "ops/ops.h"

namespace lyflow {
namespace {

// 端口类型表。前端拿 color 给端口和连线着色 —— 零成本、收益极高，
// 所以 M0 就做（见 docs/operator-manifest.md）。
void registerBuiltinTypes(Registry& r) {
  r.addType(PortType{
      "Any", "#8a8f98", {},
      "通配类型。给 Reroute、Debug View 这类透传节点用，可与任意类型互连。"});

  r.addType(PortType{
      "PointCloud", "#4a9eff", {},
      "无序点云，至少含 XYZ。"});

  r.addType(PortType{
      "PointCloudXYZI", "#4a9eff", {"PointCloud"},
      "带强度字段的点云。可隐式当作 PointCloud 使用。"});

  r.addType(PortType{
      "Indices", "#c586c0", {},
      "点下标集合。指向某个点云，本身不含坐标。"});

  r.addType(PortType{
      "Transform", "#a0d030", {},
      "4x4 刚体变换矩阵。"});
}

}  // namespace

void registerBuiltinOps(Registry& r) {
  registerBuiltinTypes(r);

  // 加新算子在这里加一行。前端不用动。
  ops::registerIoLoadPcd(r);
  ops::registerFilterVoxelGrid(r);
  ops::registerFilterPassthrough(r);
}

}  // namespace lyflow
