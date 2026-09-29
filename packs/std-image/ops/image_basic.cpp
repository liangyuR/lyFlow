// 图像的基础处理：灰度、缩放、裁剪、模糊、位深 / 线性拉伸。都是一进一出，不改输入。
#include <algorithm>
#include <cmath>

#include "ops.h"
#include "params.h"

namespace lyflow::ops {
namespace {

using img::badParam;

// -------------------------------------------------------------- image.to_gray

Status toGrayCompute(const Inputs& inputs, const ParamView&, Outputs& outputs, ExecContext&) {
  const Image& in = *inputs.get("image").asImage();
  if (in.channels == 1) {
    outputs.set("image", inputs.get("image"));  // 已经是灰度：原样传下去，不拷贝
    return Status::Ok();
  }
  cv::Mat out;
  // 公共模型是 RGB(A)（ADR-0026），所以这里是 RGB2GRAY，不是 OpenCV 惯常的 BGR2GRAY
  cv::cvtColor(cvx::view(in), out, in.channels == 4 ? cv::COLOR_RGBA2GRAY : cv::COLOR_RGB2GRAY);
  return img::putMat(outputs, "image", std::move(out));
}

// --------------------------------------------------------------- image.resize

int interpolationOf(const std::string& name) {
  if (name == "nearest") return cv::INTER_NEAREST;
  if (name == "linear") return cv::INTER_LINEAR;
  if (name == "cubic") return cv::INTER_CUBIC;
  return cv::INTER_AREA;
}

Status resizeCompute(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                     ExecContext&) {
  const Image& in = *inputs.get("image").asImage();
  int w = 0;
  int h = 0;
  if (params.choice("mode") == "size") {
    w = static_cast<int>(params.integer("width"));
    h = static_cast<int>(params.integer("height"));
  } else {
    const double s = params.number("scale");
    if (!(s > 0)) return badParam("缩放倍数要大于 0", "scale");
    w = std::max(1, static_cast<int>(std::lround(in.width * s)));
    h = std::max(1, static_cast<int>(std::lround(in.height * s)));
  }
  cv::Mat out;
  cv::resize(cvx::view(in), out, cv::Size(w, h), 0, 0, interpolationOf(params.choice("interpolation")));
  return img::putMat(outputs, "image", std::move(out));
}

// ----------------------------------------------------------------- image.crop

Status cropCompute(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext&) {
  const Image& in = *inputs.get("image").asImage();
  const auto roi = params.vec4("roi");
  int x0 = static_cast<int>(std::floor(roi[0]));
  int y0 = static_cast<int>(std::floor(roi[1]));
  int x1 = static_cast<int>(std::ceil(roi[2]));
  int y1 = static_cast<int>(std::ceil(roi[3]));
  if (inputs.has("box")) {
    // 接了框就用框：像素坐标的 Box2D（通常来自 image.region_stats 的 bbox）
    const Box2D& b = *inputs.get("box").asBox2D();
    if (b.unit != Unit2D::Pixel) {
      return img::badInput("框的单位是米（来自点云算子）；图像裁剪要像素坐标的框", "box");
    }
    x0 = static_cast<int>(std::floor(b.min[0]));
    y0 = static_cast<int>(std::floor(b.min[1]));
    x1 = static_cast<int>(std::ceil(b.max[0]));
    y1 = static_cast<int>(std::ceil(b.max[1]));
  }
  x0 = std::clamp(x0, 0, in.width);
  y0 = std::clamp(y0, 0, in.height);
  x1 = std::clamp(x1, 0, in.width);
  y1 = std::clamp(y1, 0, in.height);
  if (x1 <= x0 || y1 <= y0) {
    return badParam("裁剪框落在图像外面（图像 " + std::to_string(in.width) + "×" +
                        std::to_string(in.height) + "），裁出来是空的",
                    "roi");
  }
  // ROI 子矩阵不连续，putMat 里 fromMat 会 clone 一份连续的
  return img::putMat(outputs, "image", cvx::view(in)(cv::Rect(x0, y0, x1 - x0, y1 - y0)));
}

// ----------------------------------------------------------------- image.blur

Status blurCompute(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext&) {
  const Image& in = *inputs.get("image").asImage();
  const int k = static_cast<int>(params.integer("ksize"));
  if (k % 2 == 0) return badParam("核大小要是奇数", "ksize");
  const std::string& method = params.choice("method");
  cv::Mat out;
  if (method == "median") {
    // 中值滤波在 OpenCV 里 ksize > 5 时只收 u8
    if (k > 5 && in.depth != PixelDepth::U8) {
      return badParam("核大于 5 的中值滤波只支持 u8 图像；先接 image.normalize 或把核改小", "ksize");
    }
    cv::medianBlur(cvx::view(in), out, k);
  } else if (method == "box") {
    cv::blur(cvx::view(in), out, cv::Size(k, k));
  } else {
    cv::GaussianBlur(cvx::view(in), out, cv::Size(k, k), params.number("sigma"));
  }
  return img::putMat(outputs, "image", std::move(out));
}

// ------------------------------------------------------------ image.normalize

Status normalizeCompute(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                        ExecContext&) {
  const Image& in = *inputs.get("image").asImage();
  const int target = cvx::cvDepthOf(params.choice("depth"));
  const cv::Mat src = cvx::view(in);
  cv::Mat out;
  if (params.flag("stretch")) {
    // 每个通道各自拉到目标位深的满量程；NaN 不参与（f32 输入时 minMaxLoc 会被 NaN 带歪，先屏蔽掉）
    std::vector<cv::Mat> planes;
    cv::split(src, planes);
    for (cv::Mat& p : planes) {
      cv::Mat mask = p == p;  // 非 NaN
      double lo = 0, hi = 0;
      cv::minMaxLoc(p, &lo, &hi, nullptr, nullptr, mask);
      const double span = hi - lo;
      const double scale = span > 0 ? cvx::fullScale(target) / span : 0.0;
      cv::Mat q;
      p.convertTo(q, target, scale, -lo * scale);
      p = q;
    }
    cv::merge(planes, out);
  } else {
    src.convertTo(out, target, params.number("scale"), params.number("offset"));
  }
  return img::putMat(outputs, "image", std::move(out));
}

}  // namespace

void registerImageToGray(Registry& r) {
  OperatorDesc op;
  op.id = "image.to_gray";
  op.version = "1.0.0";
  op.label = "转灰度";
  op.category = "图像/基础";
  op.keywords = {"gray", "grey", "grayscale", "灰度"};
  op.doc = "RGB(A) 转单通道（按 BT.601 加权），alpha 丢掉；已经是灰度的原样传下去。位深不变。";
  op.inputs = {img::imageIn()};
  op.outputs = {img::imageOut("image", "单通道图像。")};
  op.capabilities = {false, false, true};
  op.compute = &toGrayCompute;
  r.addOperator(std::move(op));
}

void registerImageResize(Registry& r) {
  OperatorDesc op;
  op.id = "image.resize";
  op.version = "1.0.0";
  op.label = "缩放";
  op.category = "图像/基础";
  op.keywords = {"resize", "scale", "缩放", "尺寸"};
  op.doc = "按倍数或指定尺寸缩放。缩小默认用区域平均（不出摩尔纹），放大建议线性或三次。";
  op.inputs = {img::imageIn()};
  op.outputs = {img::imageOut()};
  Param mode = img::enumParam("mode", "Mode", "scale",
                              {EnumOption{"scale", "按倍数", ""}, EnumOption{"size", "指定尺寸", ""}},
                              "按倍数，还是给定输出的宽高。");
  Param scale = img::floatParam("scale", "Scale", 0.5, "宽高各乘这个倍数。");
  scale.min = 0.0001;
  scale.softMin = 0.1;
  scale.softMax = 4.0;
  scale.step = 0.05;
  scale.visibleWhen = img::when("mode", Value::text("scale"));
  Param width = img::intParam("width", "Width", 640, 1, 65535, "输出宽度，像素。");
  width.unit = "px";
  width.visibleWhen = img::when("mode", Value::text("size"));
  Param height = img::intParam("height", "Height", 480, 1, 65535, "输出高度，像素。");
  height.unit = "px";
  height.visibleWhen = img::when("mode", Value::text("size"));
  Param interp = img::enumParam("interpolation", "Interpolation", "area",
                                {EnumOption{"area", "区域平均", "缩小首选"},
                                 EnumOption{"linear", "线性", ""},
                                 EnumOption{"cubic", "三次", "放大更锐"},
                                 EnumOption{"nearest", "最近邻", "掩膜 / 标签图用它，不会插出中间值"}},
                                "插值方式。");
  op.params = {mode, scale, width, height, interp};
  op.capabilities = {false, false, true};
  op.compute = &resizeCompute;
  r.addOperator(std::move(op));
}

void registerImageCrop(Registry& r) {
  OperatorDesc op;
  op.id = "image.crop";
  op.version = "1.0.0";
  op.label = "裁剪";
  op.category = "图像/基础";
  op.keywords = {"crop", "roi", "裁剪", "截取"};
  op.doc = "按像素矩形 [x0, y0, x1, y1] 裁出一块（主预览里可以在输入图上拖框）；接了 box 就用框"
           "（像素单位的 Box2D）。超出图像的部分截掉。";
  op.inputs = {img::imageIn(),
               Port{"box", "Box2D", "Box", "可选：像素坐标的框，接了就不看 ROI 参数。", false}};
  op.outputs = {img::imageOut()};
  Param roi;
  roi.name = "roi";
  roi.type = ParamType::Vec4f;
  roi.label = "ROI";
  roi.doc = "[x0, y0, x1, y1]，像素，左上角是原点、y 向下；右下角不含。";
  roi.def = Value::vec({0.0, 0.0, 256.0, 256.0});
  roi.unit = "px";
  roi.step = 1;
  roi.semantic = "roi";
  roi.componentLabels = {"X0", "Y0", "X1", "Y1"};
  op.params = {roi};
  op.capabilities = {false, false, true};
  op.compute = &cropCompute;
  r.addOperator(std::move(op));
}

void registerImageBlur(Registry& r) {
  OperatorDesc op;
  op.id = "image.blur";
  op.version = "1.0.0";
  op.label = "平滑";
  op.category = "图像/基础";
  op.keywords = {"blur", "smooth", "gaussian", "median", "平滑", "滤波", "去噪"};
  op.doc = "高斯 / 中值 / 均值平滑。中值对椒盐噪声最好、保边；高斯最通用。";
  op.inputs = {img::imageIn()};
  op.outputs = {img::imageOut()};
  Param method = img::enumParam("method", "Method", "gaussian",
                                {EnumOption{"gaussian", "高斯", ""}, EnumOption{"median", "中值", "保边"},
                                 EnumOption{"box", "均值", ""}},
                                "平滑方式。");
  Param k = img::intParam("ksize", "Kernel", 5, 1, 99, "核大小，奇数。");
  k.unit = "px";
  k.step = 2;
  Param sigma = img::floatParam("sigma", "Sigma", 0.0, "高斯的标准差；0 = 按核大小自动算。");
  sigma.min = 0.0;
  sigma.visibleWhen = img::when("method", Value::text("gaussian"));
  op.params = {method, k, sigma};
  op.capabilities = {false, false, true};
  op.compute = &blurCompute;
  r.addOperator(std::move(op));
}

void registerImageNormalize(Registry& r) {
  OperatorDesc op;
  op.id = "image.normalize";
  op.version = "1.0.0";
  op.label = "位深与拉伸";
  op.category = "图像/基础";
  op.keywords = {"normalize", "convert", "depth", "stretch", "8bit", "位深", "拉伸", "归一化"};
  op.doc = "转换位深。拉伸：每个通道的 min..max 拉到目标位深的满量程（f32 是 0..1）；"
           "不拉伸：输出 = 输入 × scale + offset，超出量程截断。";
  op.inputs = {img::imageIn()};
  op.outputs = {img::imageOut()};
  Param stretch = img::boolParam("stretch", "Stretch", true, "按每个通道的实际范围拉满。");
  Param scale = img::floatParam("scale", "Scale", 1.0, "不拉伸时乘的系数（u16 → u8 常用 1/257）。");
  scale.enabledWhen = img::when("stretch", Value::boolean(false));
  Param offset = img::floatParam("offset", "Offset", 0.0, "不拉伸时加的偏移。");
  offset.enabledWhen = img::when("stretch", Value::boolean(false));
  op.params = {img::depthParam("depth", "Depth", "u8", "输出位深。"), stretch, scale, offset};
  op.capabilities = {false, false, true};
  op.compute = &normalizeCompute;
  r.addOperator(std::move(op));
}

}  // namespace lyflow::ops
