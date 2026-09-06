// std-pointcloud 包的注册入口（ADR-0013 / ADR-0014）。
#include "lyflow/cloud_io.h"
#include "ops.h"

namespace lyflow::packs::std_pointcloud {

void registerPackOps(Registry& r) {
  // 顺序即面板里同分类下的排列顺序，按「一条 pipeline 从左到右」排。
  // 它同时是 manifest 里的顺序 —— 改动会改变 manifest 的字节。
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

  // 2D 量测域的四个算子追加在最后：原来那 14 个的相对顺序不动，
  // manifest 里它们的描述因此逐字节不变（ADR-0014 的 S5 仍然成立）。
  ops::registerFilterCropBox2D(r);
  ops::registerFitLine2D(r);
  ops::registerFitCircle2D(r);
  ops::registerRegisterIcp2D(r);

  // 写盘格式的知识只有本包有，core 的 C ABI 经这个钩子转交（ADR-0014）。
  setCloudWriter(&ops::saveCloudToFile);
}

}  // namespace lyflow::packs::std_pointcloud
