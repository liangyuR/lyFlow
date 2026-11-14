#pragma once
// std-image 包内算子的注册函数声明，由 register.cpp 显式调用。
#include "lyflow/operator.h"
#include "lyflow/registry.h"

namespace lyflow::ops {

void registerIoLoadImage(Registry& r);
void registerIoSaveImage(Registry& r);
void registerImageToGray(Registry& r);
void registerImageResize(Registry& r);
void registerImageCrop(Registry& r);
void registerImageBlur(Registry& r);
void registerImageNormalize(Registry& r);
void registerImageThreshold(Registry& r);
void registerImageMorphology(Registry& r);
void registerImageFindCircle(Registry& r);
void registerImageRegionStats(Registry& r);
void registerImageToTensor(Registry& r);
void registerTensorToImage(Registry& r);
void registerCloudFromDepth(Registry& r);
void registerCloudToDepthImage(Registry& r);
void registerImageAnalysis(Registry& r);
void registerImageCalipers(Registry& r);
void registerImageDifference(Registry& r);
void registerImageDetection(Registry& r);
void registerImageYolo(Registry& r);
void registerImageP1Basic(Registry& r);
void registerImageP1Calibration(Registry& r);
void registerImageP1Matching(Registry& r);
void registerImageP1Circles(Registry& r);
void registerImageHighlights(Registry& r);

} // namespace lyflow::ops

namespace lyflow::std_image {
void registerBoardCalib(Registry& r);
void registerLoadCalib(Registry& r);
}
