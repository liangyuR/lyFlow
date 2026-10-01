#pragma once
// 预览时图像按比例缩小之后，像素量的换算（docs/large-image-plan.md E4–E8，ADR-0028）。
// 图上的像素量 —— 参数、连线上的像素几何与像素量测 —— 一律是原图坐标；节点在 scale = s 的图上算时，
// 换算只发生在 compute 两侧：进 ÷ s，出 × s。算子不知道自己在预览（ADR-0011 的原则）。
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

#include "lyflow/data.h"
#include "lyflow/manifest.h"
#include "lyflow/operator.h"

namespace lyflow::exec {

/// 预览时源头图像的像素上限（E4）：超过就按 2 的幂缩小到不超过它。4 MP 下一条
/// 灰度 → 平滑 → 二值化 → 开运算 → 区域统计约 40 ms。
constexpr std::size_t kPreviewMaxPixels = std::size_t{1} << 22;

/// 把 img 缩到不超过 maxPixels 要的最小级别（缩小 2^level 倍）。不用缩是 0。
unsigned previewLevel(const Image& img, std::size_t maxPixels);

/// 节点的 s：它的图像输入里最大的 scale；没有图像输入是 1。
std::int32_t inputScale(const std::unordered_map<std::string, Data>& inputs);

/// 像素参数 ÷ s（E6、E8）：unit = px 且不是 absolute 的那些。浮点直接除；整数四舍五入，
/// 有 step 时落到 min + n × step 上（奇数核换算后还是奇数），再夹到 [min, max]；vec 逐分量。
ParamMap scalePixelParams(const OperatorDesc& op, const ParamMap& params, std::int32_t s);

/// 此刻有没有生效（visibleWhen 成立）的绝对尺寸参数（E7）。有的话本节点输出的图像回到原图比例。
bool hasActiveAbsoluteSize(const OperatorDesc& op, const ParamMap& params);

/// 像素几何（unit = Pixel 的 Box2D / Line2D / Circle2D / Point2D）与像素量测（unit "px" 乘 factor，
/// "px²" 乘 factor²，判定的上下限一起）乘以 factor；Bundle 逐字段。别的类型原样返回（同一份 Data）。
Data scalePixelData(const Data& d, double factor);

/// 把节点的一个输入换算到这个节点的比例 s 上（compute 之前那一半）：像素几何与量测 ÷ s；
/// 比例比 s 小的图像（例如 resize 到原图尺寸的掩膜，scale 1）缩到 s，几张输入图才一样大。
Data toNodeScale(const Data& d, std::int32_t s);

/// compute 之后的那一半：像素几何与量测 × s；scale 还是 1 的输出图像设成 s ——
/// 有生效的绝对尺寸参数时保持 1（resize 到指定宽高的图与输入不在一个比例上）。
void scaleOutputs(std::unordered_map<std::string, Data>& outputs, std::int32_t s,
                  bool absoluteSize);

}  // namespace lyflow::exec
