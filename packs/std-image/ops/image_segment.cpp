// 分割与测量：二值化、形态学、找圆、区域统计。
// 与 PCL 那边「尽量出 Indices」同一个意思（ADR-0005 第 4 条的图像版）：尽量出掩膜与几何 / 测量，
// 少出「又一张彩色图」。几何一律是像素坐标，unit = px（docs/image-plan.md Q2）。
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "ops.h"
#include "params.h"

namespace lyflow::ops {
namespace {

using img::badInput;
using img::badParam;

// ------------------------------------------------------------ image.threshold

Status thresholdCompute(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                        ExecContext&) {
  const Image& in = *inputs.get("image").asImage();
  const std::string& method = params.choice("method");
  const bool invert = params.flag("invert");
  const cv::Mat src = cvx::view(in);
  cv::Mat out;
  if (method == "fixed") {
    // 固定阈值对 u16 / f32 同样有意义；结果统一成 u8 掩膜 0 / 255
    cv::Mat bin;
    cv::threshold(src, bin, params.number("thresh"), 255.0,
                  invert ? cv::THRESH_BINARY_INV : cv::THRESH_BINARY);
    bin.convertTo(out, CV_8U);
  } else {
    if (in.depth != PixelDepth::U8) {
      return badInput("Otsu / 自适应阈值只支持 u8 图像；先接 image.normalize 转成 u8");
    }
    if (method == "otsu") {
      cv::threshold(src, out, 0, 255.0,
                    (invert ? cv::THRESH_BINARY_INV : cv::THRESH_BINARY) | cv::THRESH_OTSU);
    } else {
      const int block = static_cast<int>(params.integer("blockSize"));
      if (block % 2 == 0 || block < 3) return badParam("邻域大小要是 ≥ 3 的奇数", "blockSize");
      cv::adaptiveThreshold(src, out, 255.0,
                            method == "adaptive_gaussian" ? cv::ADAPTIVE_THRESH_GAUSSIAN_C
                                                          : cv::ADAPTIVE_THRESH_MEAN_C,
                            invert ? cv::THRESH_BINARY_INV : cv::THRESH_BINARY, block,
                            params.number("c"));
    }
  }
  return img::putMat(outputs, "mask", std::move(out));
}

// ----------------------------------------------------------- image.morphology

Status morphologyCompute(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                         ExecContext&) {
  const Image& in = *inputs.get("image").asImage();
  const int k = static_cast<int>(params.integer("ksize"));
  const std::string& shapeName = params.choice("shape");
  const int shape = shapeName == "ellipse" ? cv::MORPH_ELLIPSE
                    : shapeName == "cross" ? cv::MORPH_CROSS
                                           : cv::MORPH_RECT;
  const cv::Mat kernel = cv::getStructuringElement(shape, cv::Size(k, k));
  const std::string& opName = params.choice("op");
  const int op = opName == "erode"   ? cv::MORPH_ERODE
                 : opName == "dilate" ? cv::MORPH_DILATE
                 : opName == "close"  ? cv::MORPH_CLOSE
                                      : cv::MORPH_OPEN;
  cv::Mat out;
  cv::morphologyEx(cvx::view(in), out, op, kernel, cv::Point(-1, -1),
                   static_cast<int>(params.integer("iterations")));
  return img::putMat(outputs, "image", std::move(out));
}

// ---------------------------------------------------------- image.find_circle

Status findCircleCompute(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                         ExecContext& ctx) {
  const Image& in = *inputs.get("image").asImage();
  if (in.depth != PixelDepth::U8) {
    return badInput("霍夫找圆只支持 u8 图像；先接 image.normalize 转成 u8");
  }
  const double minR = params.number("minRadius");
  const double maxR = params.number("maxRadius");
  if (maxR > 0 && maxR < minR) return badParam("最大半径比最小半径还小", "maxRadius");
  // 预览里半径按比例缩过（ADR-0028）：截断会把 0.75 变成 0 —— 对 OpenCV 那是「不限」。四舍五入，正数至少 1
  const auto radius = [](double r) { return r > 0 ? std::max(1, static_cast<int>(std::lround(r))) : 0; };
  std::vector<cv::Vec3f> circles;
  cv::HoughCircles(cvx::view(in), circles, cv::HOUGH_GRADIENT, params.number("dp"),
                   params.number("minDist"), params.number("edgeThreshold"),
                   params.number("accumulatorThreshold"), radius(minR), radius(maxR));
  if (circles.empty()) {
    return Status::Error(Phase::Execute, "circle_not_found",
                         "没找到圆：调低累加阈值、放宽半径范围，或先平滑去噪", "accumulatorThreshold");
  }
  // HoughCircles 按累加值从高到低排，第一个就是最强的
  Circle2D c;
  c.center[0] = circles[0][0];
  c.center[1] = circles[0][1];
  c.radius = circles[0][2];
  c.unit = Unit2D::Pixel;
  ctx.log(LogLevel::Info, "找到 " + std::to_string(circles.size()) + " 个候选，取最强的一个");
  outputs.set("circle", Data::circle2d(c));
  return Status::Ok();
}

// --------------------------------------------------------- image.region_stats

Measurement measured(double value, const char* unit) {
  Measurement m;
  m.value = value;
  m.ok = std::isfinite(value);
  m.unit = unit;
  if (!m.ok) m.message = "区域是空的";
  return m;
}

Status regionStatsCompute(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                          ExecContext&) {
  const Image& in = *inputs.get("image").asImage();
  const int channel = static_cast<int>(params.integer("channel"));
  if (channel >= in.channels) {
    return badParam("图像只有 " + std::to_string(in.channels) + " 个通道", "channel");
  }
  // 单通道图直接借原图（不拷一份通道）
  cv::Mat plane;
  if (in.channels == 1) {
    plane = cvx::view(in);
  } else {
    cv::extractChannel(cvx::view(in), plane, channel);
  }
  const double nan = std::numeric_limits<double>::quiet_NaN();
  // 不接掩膜、也没有 NaN 要排除（不是 f32）：区域就是整张图。直接算，不必先造一张全 255 的掩膜再扫三遍 ——
  // 整数位深的和在 OpenCV 里是精确累加，有没有掩膜结果都一样
  if (!inputs.has("mask") && plane.depth() != CV_32F) {
    const int area = plane.rows * plane.cols;
    Box2D box;
    box.unit = Unit2D::Pixel;
    if (area > 0) {
      box.max[0] = static_cast<float>(plane.cols);
      box.max[1] = static_cast<float>(plane.rows);
    }
    outputs.set("mean", Data::measurement(measured(area > 0 ? cv::mean(plane)[0] : nan, "")));
    outputs.set("area", Data::measurement(measured(area > 0 ? area : nan, "px²")));
    outputs.set("bbox", Data::box2d(box));
    return Status::Ok();
  }
  cv::Mat mask;
  if (inputs.has("mask")) {
    const Image& mk = *inputs.get("mask").asImage();
    if (mk.width != in.width || mk.height != in.height) {
      return badInput("掩膜 " + std::to_string(mk.width) + "×" + std::to_string(mk.height) +
                          " 与图像 " + std::to_string(in.width) + "×" + std::to_string(in.height) +
                          " 不一样大",
                      "mask");
    }
    if (mk.channels != 1) {
      return badInput("掩膜要是单通道图（收到 " + std::to_string(mk.channels) + " 通道）；先接 image.to_gray",
                      "mask");
    }
    // 非零处算区域：直接比较原值，f32 掩膜里 0 < v < 0.5 的也算（先转 u8 会舍成 0）；NaN 不算
    const cv::Mat mv = cvx::view(mk);
    mask = mv != 0;
    if (mk.depth == PixelDepth::F32) mask &= (mv == mv);
  } else {
    mask = cv::Mat(plane.size(), CV_8U, cv::Scalar(255));
  }
  if (plane.depth() == CV_32F) mask &= (plane == plane);  // NaN 不算进区域

  const int area = cv::countNonZero(mask);
  double mean = nan;
  Box2D box;
  box.unit = Unit2D::Pixel;
  if (area > 0) {
    mean = cv::mean(plane, mask)[0];
    const cv::Rect r = cv::boundingRect(mask);
    // 像素 (x, y) 占 [x, x+1) × [y, y+1)：框的右下角是 x+w，与 image.crop 的读法一致
    box.min[0] = static_cast<float>(r.x);
    box.min[1] = static_cast<float>(r.y);
    box.max[0] = static_cast<float>(r.x + r.width);
    box.max[1] = static_cast<float>(r.y + r.height);
  }
  outputs.set("mean", Data::measurement(measured(mean, "")));
  // 面积是像素个数，量纲是 px²：预览缩小时按 s² 换回原图（ADR-0028）
  outputs.set("area", Data::measurement(measured(area > 0 ? area : nan, "px²")));
  outputs.set("bbox", Data::box2d(box));
  return Status::Ok();
}

}  // namespace

void registerImageThreshold(Registry& r) {
  OperatorDesc op;
  op.id = "image.threshold";
  op.version = "1.0.0";
  op.label = "二值化";
  op.category = "图像/分割";
  op.keywords = {"threshold", "binary", "otsu", "adaptive", "mask", "二值化", "阈值", "掩膜"};
  op.doc = "单通道图 → u8 掩膜（0 / 255）。固定阈值任何位深都行；Otsu 自动选阈值、自适应按邻域算，"
           "这两种只收 u8。";
  op.inputs = {img::grayIn()};
  op.outputs = {img::imageOut("mask", "u8 掩膜，前景 255。")};
  Param method = img::enumParam("method", "Method", "otsu",
                                {EnumOption{"fixed", "固定阈值", ""},
                                 EnumOption{"otsu", "Otsu", "双峰直方图上自动选阈值"},
                                 EnumOption{"adaptive_mean", "自适应（均值）", "光照不均时用"},
                                 EnumOption{"adaptive_gaussian", "自适应（高斯）", ""}},
                                "怎么定阈值。");
  Param thresh = img::floatParam("thresh", "Threshold", 128.0, "大于它的算前景（按输入的原值）。");
  thresh.visibleWhen = img::when("method", Value::text("fixed"));
  Param block = img::intParam("blockSize", "Block", 31, 3, 999, "自适应的邻域边长，奇数。");
  block.unit = "px";
  block.step = 2;
  block.visibleWhen.param = "method";
  block.visibleWhen.in = {Value::text("adaptive_mean"), Value::text("adaptive_gaussian")};
  Param c = img::floatParam("c", "C", 5.0, "自适应阈值 = 邻域均值 − C。");
  c.visibleWhen = block.visibleWhen;
  op.params = {method, thresh, block, c,
               img::boolParam("invert", "Invert", false, "反过来：暗的算前景。")};
  op.capabilities = {false, false, true};
  op.compute = &thresholdCompute;
  r.addOperator(std::move(op));
}

void registerImageMorphology(Registry& r) {
  OperatorDesc op;
  op.id = "image.morphology";
  op.version = "1.0.0";
  op.label = "形态学";
  op.category = "图像/分割";
  op.keywords = {"morphology", "erode", "dilate", "open", "close", "腐蚀", "膨胀", "开运算", "闭运算"};
  op.doc = "腐蚀 / 膨胀 / 开 / 闭。开运算去掉小亮点，闭运算填掉小暗洞，通常接在二值化后面。";
  op.inputs = {img::imageIn()};
  op.outputs = {img::imageOut()};
  Param k = img::intParam("ksize", "Kernel", 3, 1, 99, "结构元边长。");
  k.unit = "px";
  op.params = {
      img::enumParam("op", "Operation", "open",
                     {EnumOption{"erode", "腐蚀", ""}, EnumOption{"dilate", "膨胀", ""},
                      EnumOption{"open", "开", "先腐蚀后膨胀"}, EnumOption{"close", "闭", "先膨胀后腐蚀"}},
                     "做哪一种。"),
      img::enumParam("shape", "Shape", "rect",
                     {EnumOption{"rect", "矩形", ""}, EnumOption{"ellipse", "椭圆", ""},
                      EnumOption{"cross", "十字", ""}},
                     "结构元形状。"),
      k,
      img::intParam("iterations", "Iterations", 1, 1, 100, "重复几次。"),
  };
  op.capabilities = {false, false, true};
  op.compute = &morphologyCompute;
  r.addOperator(std::move(op));
}

void registerImageFindCircle(Registry& r) {
  OperatorDesc op;
  op.id = "image.find_circle";
  op.version = "1.0.0";
  op.label = "找圆";
  op.category = "图像/测量";
  op.keywords = {"circle", "hough", "find", "找圆", "霍夫", "孔"};
  op.doc = "霍夫梯度法找圆，取累加值最高的一个。输出像素坐标的 Circle2D（unit = px）。"
           "边缘噪声多时先接 image.blur。";
  op.inputs = {img::grayIn("image", "单通道 u8 图像。")};
  op.outputs = {Port{"circle", "Circle2D", "Circle", "最强的那个圆，像素坐标。", true}};
  Param dp = img::floatParam("dp", "dp", 1.0, "累加器分辨率与图像之比的倒数；1 = 同分辨率。");
  dp.min = 1.0;
  dp.advanced = true;
  Param minDist = img::floatParam("minDist", "Min Distance", 20.0, "两个圆心之间的最小距离。");
  minDist.unit = "px";
  minDist.min = 1.0;
  Param edge = img::floatParam("edgeThreshold", "Edge Threshold", 100.0, "Canny 的高阈值。");
  edge.advanced = true;
  Param acc = img::floatParam("accumulatorThreshold", "Accumulator", 30.0,
                              "累加阈值：越小找到的（假）圆越多。票数约等于圆周上的边缘像素数，所以按像素计。");
  // 票数随圆周长度变：预览缩小 s 倍时阈值也 ÷ s，正式运行找得到的小圆预览里才不会漏掉（ADR-0028，review 修正）
  acc.unit = "px";
  Param minR = img::floatParam("minRadius", "Min Radius", 0.0, "最小半径；0 = 不限。");
  minR.unit = "px";
  minR.min = 0.0;
  Param maxR = img::floatParam("maxRadius", "Max Radius", 0.0, "最大半径；0 = 不限。");
  maxR.unit = "px";
  maxR.min = 0.0;
  op.params = {dp, minDist, edge, acc, minR, maxR};
  op.capabilities = {false, false, true};
  op.compute = &findCircleCompute;
  r.addOperator(std::move(op));
}

void registerImageRegionStats(Registry& r) {
  OperatorDesc op;
  op.id = "image.region_stats";
  op.version = "1.0.0";
  op.label = "区域统计";
  op.category = "图像/测量";
  op.keywords = {"stats", "mean", "area", "region", "统计", "面积", "均值", "灰度"};
  op.doc = "掩膜里（没接掩膜就是整张图）某个通道的均值、像素面积与外接框。"
           "外接框是像素坐标的 Box2D，可以直接接给 image.crop。";
  op.inputs = {img::imageIn(),
               Port{"mask", "Image", "Mask", "可选：与图像一样大，非零处算区域（通常来自 image.threshold）。", false}};
  op.outputs = {Port{"mean", "Measurement", "Mean", "区域内的均值（原值）。", true},
                Port{"area", "Measurement", "Area", "区域的像素数。", true},
                Port{"bbox", "Box2D", "BBox", "区域的外接框，像素坐标。", true}};
  op.params = {img::intParam("channel", "Channel", 0, 0, 3, "统计哪个通道（0 = R 或灰度）。")};
  op.capabilities = {false, false, true};
  op.compute = &regionStatsCompute;
  r.addOperator(std::move(op));
}

}  // namespace lyflow::ops
