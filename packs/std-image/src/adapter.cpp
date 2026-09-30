#include "lyflow_cv/adapter.h"

#include <memory>
#include <utility>

namespace lyflow::cvx {

int cvType(const Image& img) {
  switch (img.depth) {
    case PixelDepth::U8:  return CV_MAKETYPE(CV_8U, img.channels);
    case PixelDepth::U16: return CV_MAKETYPE(CV_16U, img.channels);
    case PixelDepth::F32: return CV_MAKETYPE(CV_32F, img.channels);
  }
  return CV_MAKETYPE(CV_8U, img.channels);
}

cv::Mat view(const Image& img) {
  // const_cast 只为满足 Mat 构造函数的签名；约定是只读（见头文件）
  return cv::Mat(img.height, img.width, cvType(img), const_cast<std::uint8_t*>(img.pixels.get()),
                 img.rowBytes());
}

bool fromMat(cv::Mat m, Image& out) {
  const int ch = m.channels();
  if (m.dims != 2 || m.empty() || (ch != 1 && ch != 3 && ch != 4)) return false;
  PixelDepth depth = PixelDepth::U8;
  switch (m.depth()) {
    case CV_8U:  depth = PixelDepth::U8; break;
    case CV_16U: depth = PixelDepth::U16; break;
    case CV_32F: depth = PixelDepth::F32; break;
    default: return false;
  }
  // 两种情况要先拷一份：
  // - 不连续（一般的 ROI 子矩阵）：core 的 Image 要行紧排；
  // - 数据不归 OpenCV 管（u == nullptr）：view() 建的头、以及它的子矩阵都是这样。全宽或只有一行的子矩阵
  //   isContinuous() 为真，不拷的话输出就借着上游的像素、却不持有它 —— 上游结果被清掉 / 淘汰之后读到的是
  //   已释放的内存（review 修正，PR #1）
  if (!m.isContinuous() || m.u == nullptr) m = m.clone();
  auto holder = std::make_shared<cv::Mat>(std::move(m));
  Image img;
  img.width = holder->cols;
  img.height = holder->rows;
  img.channels = ch;
  img.depth = depth;
  img.pixels = std::shared_ptr<const std::uint8_t>(holder, holder->ptr<std::uint8_t>(0));
  out = std::move(img);
  return true;
}

cv::Mat toSupportedDepth(const cv::Mat& m) {
  const int d = m.depth();
  if (d == CV_8U || d == CV_16U || d == CV_32F) return m;
  cv::Mat out;
  m.convertTo(out, CV_MAKETYPE(CV_32F, m.channels()));
  return out;
}

int cvDepthOf(const std::string& name) {
  if (name == "u16") return CV_16U;
  if (name == "f32") return CV_32F;
  return CV_8U;
}

double fullScale(int cvDepth) {
  if (cvDepth == CV_8U) return 255.0;
  if (cvDepth == CV_16U) return 65535.0;
  return 1.0;
}

}  // namespace lyflow::cvx
