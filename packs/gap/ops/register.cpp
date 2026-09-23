// 算子包的注册入口。LyFlow 的 CMake 生成的 registerOpPacks 调它（ADR-0013）。
#include "gap_fine.h"

namespace lyflow::packs::gap {

void registerPackOps(Registry& r) {
  // Bundle 声明先于用到它的端口（m8-plan L2）。
  registerBundles(r);
  // 顺序即节点面板里同分类下的排列顺序，按一条 pipeline 从左到右排。
  registerLoadProfilePair(r);
  registerToMeasurementFrame(r);
  registerDropNonFinite(r);
  registerProfileTensor(r);
  registerLabelsFromLogits(r);
  registerRoiFromLabels(r);
  registerLabelsToCloud(r);
  registerRollAnchoredCrop(r);
  registerDatumWindow(r);
  registerOverallRoi(r);
  registerLoadTemplate(r);
  registerAlignTemplate(r);
  registerSelectAlignment(r);
  registerBusinessRois(r);
  registerFitLine(r);
  registerSelectedPoint(r);
  registerNearestToLine(r);
  registerFitGapCircles(r);
  registerFlush(r);
  registerGap(r);
  registerCornerVertex(r);
  registerGrooveJoint(r);
  registerNotchWidth(r);
  registerPointOffset(r);
  registerCameraGuard(r);
  registerJudge(r);
  registerResultBundle(r);
  registerMeasureReference(r);
  registerBundleOps(r);
  // 积木算子拷细粒度算子的参数声明（paramOf），所以排在它们后面。
  registerBlockOps(r);
  registerStandardGapImporter(r);
}

}  // namespace lyflow::packs::gap
