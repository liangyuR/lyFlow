#pragma once
// 进出 OpenCV 的唯一通道（ADR-0005 第 3 条的图像版，ADR-0026）。
//
// - view：零拷贝，建一个指向 Image 像素的 cv::Mat 头。**只读** —— 像素在结果仓里被别的节点共享，
//   原地改就是改了上游的结果；要原地改先 clone()。Mat 不持有所有权，Image 要活得比它久。
// - fromMat：零拷贝交给 core。别名 shared_ptr 持有 Mat 的引用计数，core 不认识 OpenCV 也能保活。
//   不连续的 Mat（ROI 子矩阵）先 clone 成连续的一份。
// - BGR ↔ RGB 只在读写文件那一层（io.load_image / io.save_image）转，其余算子不管通道顺序。
#include <string>

#include <opencv2/core.hpp>

#include "lyflow/data.h"

namespace lyflow::cvx {

/// CV_8UC(n) / CV_16UC(n) / CV_32FC(n)。
int cvType(const Image& img);

cv::Mat view(const Image& img);

/// 位深只认 8U / 16U / 32F、通道只认 1 / 3 / 4。别的返回 false（out 不动），调用方报 bad_input。
bool fromMat(cv::Mat m, Image& out);

/// 其余位深（8S / 16S / 32S / 64F）转成 core 认的 f32。已经是 8U / 16U / 32F 的原样返回（不拷贝）。
cv::Mat toSupportedDepth(const cv::Mat& m);

/// "u8" / "u16" / "f32" → CV_8U / CV_16U / CV_32F。
int cvDepthOf(const std::string& name);

/// 这个位深的满量程：u8 255、u16 65535、f32 1。
double fullScale(int cvDepth);

}  // namespace lyflow::cvx
