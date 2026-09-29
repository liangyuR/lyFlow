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

}  // namespace lyflow::ops
