// std-image 包的注册入口（docs/image-plan.md 阶段 2）。OpenCV 的依赖只在这个包里。
// 调用顺序就是 manifest 里的顺序，也是算子面板里的顺序。
#include "ops.h"

namespace lyflow::packs::std_image {

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
}

}  // namespace lyflow::packs::std_image
