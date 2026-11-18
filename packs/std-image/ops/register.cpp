// std-image 包的注册入口（docs/image-plan.md 阶段 2）。OpenCV 的依赖只在这个包里。
// 调用顺序就是 manifest 里的顺序，也是算子面板里的顺序。
#include "ops.h"

#include "lyflow_cv/inspection.h"
#include "lyflow_cv/template_pose.h"

namespace lyflow::std_image::color {
void registerColorMap(Registry& r);
void registerColorFan(Registry& r);
} // namespace lyflow::std_image::color

namespace lyflow::packs::std_image {
void registerResponse(Registry& r);
void registerFanScan(Registry& r);
void registerLateralRuns(Registry& r);
void registerPartEdges(Registry& r);
void registerPlaneCalibUpgrade(Registry& r);

void registerPackOps(Registry& r) {
  ops::registerIoLoadImage(r);
  ops::registerIoSaveImage(r);
  ops::registerImageToGray(r);
  ops::registerImageResize(r);
  ops::registerImageCrop(r);
  ops::registerImageBlur(r);
  ops::registerImageNormalize(r);
  ops::registerImageThreshold(r);
  ops::registerImageMorphology(r);
  ops::registerImageFindCircle(r);
  ops::registerImageRegionStats(r);
  ops::registerImageToTensor(r);
  ops::registerTensorToImage(r);
  ops::registerCloudFromDepth(r);
  ops::registerCloudToDepthImage(r);
  lyflow::std_image::registerBoardCalib(r);
  lyflow::std_image::registerLoadCalib(r);
  ops::registerImageAnalysis(r);
  ops::registerImageCalipers(r);
  ops::registerImageDifference(r);
  ops::registerImageDetection(r);
  ops::registerImageYolo(r);
  ops::registerImageP1Basic(r);
  ops::registerImageP1Calibration(r);
  ops::registerImageP1Matching(r);
  ops::registerImageP1Circles(r);
  ops::registerImageHighlights(r);
  ops::registerP2ModelProjection(r);
  ops::registerP2HeightExtraction(r);
  ops::registerP3Markers(r);
  ops::registerP3ShapeMatching(r);
  ops::registerP3Workspace(r);
  ops::registerP4Annotations(r);
  ops::registerP4Barcodes(r);
  ops::registerP4OcrImage(r);
  ops::registerP4Segmentation(r);
  ops::registerP5Projection(r);
  ops::registerP5Inspection(r);
  ops::registerP5PoseProjection(r);
  lyflow::std_image::inspection::registerInspectionBundles(r);
  registerResponse(r);
  registerFanScan(r);
  registerLateralRuns(r);
  registerPartEdges(r);
  lyflow::std_image::color::registerColorMap(r);
  lyflow::std_image::color::registerColorFan(r);
  lyflow::std_image::template_pose::registerLocate(r);
  registerPlaneCalibUpgrade(r);
}

} // namespace lyflow::packs::std_image
