// 图像 ↔ 张量（docs/image-plan.md I9）。两者之间没有隐式转换：推理要的预处理（缩放、mean/std、
// HWC → NCHW）必须是图上看得见的参数，不能藏在类型系统里。这两个算子不需要 OpenCV，放在这个包里
// 是因为它们属于图像域。
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

#include "ops.h"
#include "params.h"

namespace lyflow::ops {
namespace {

using img::badInput;
using img::badParam;

// -------------------------------------------------------------- image.to_tensor

/// 一种位深的整张图。原来每个值都调一次 Image::at（不内联、每次按位深 switch），1200 万像素的 RGB 要 230 ms；
/// 这里按位深走裸指针。逐值的算术与原来一字不差（double 乘 scale、减 mean、除 std，最后截成 float），
/// 所以张量逐位不变。
template <typename P>
void toTensorRows(const Image& in, double scale, const std::array<double, 3>& mean, const std::array<double, 3>& stdv,
                  bool planar, float* out) {
  const std::int64_t h = in.height, w = in.width, ch = in.channels;
  const std::uint8_t* base = in.pixels.get();
  const std::size_t rowBytes = in.rowBytes();
  for (std::int64_t y = 0; y < h; ++y) {
    const std::uint8_t* row = base + static_cast<std::size_t>(y) * rowBytes;
    for (std::int64_t x = 0; x < w; ++x) {
      for (std::int64_t c = 0; c < ch; ++c) {
        P raw;
        std::memcpy(&raw, row + static_cast<std::size_t>(x * ch + c) * sizeof(P), sizeof(P));
        // mean / std 只作用在前三个通道上；alpha（第 4 个）只乘 scale
        const double v = static_cast<double>(raw) * scale;
        const double norm = c < 3 ? (v - mean[c]) / stdv[c] : v;
        const std::size_t at = planar ? static_cast<std::size_t>((c * h + y) * w + x)
                                      : static_cast<std::size_t>((y * w + x) * ch + c);
        out[at] = static_cast<float>(norm);
      }
    }
  }
}

Status toTensorCompute(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                       ExecContext&) {
  const Image& in = *inputs.get("image").asImage();
  const std::string& layout = params.choice("layout");
  const double scale = params.number("scale");
  const auto mean = params.vec3("mean");
  const auto stdv = params.vec3("std");
  for (int c = 0; c < 3; ++c) {
    if (!(stdv[c] > 0)) return badParam("std 的每一项都要大于 0", "std");
  }
  const std::int64_t h = in.height, w = in.width, ch = in.channels;
  Tensor t;
  if (layout == "NCHW") t.shape = {1, ch, h, w};
  else if (layout == "NHWC") t.shape = {1, h, w, ch};
  else if (layout == "CHW") t.shape = {ch, h, w};
  else t.shape = {h, w, ch};
  t.data.resize(static_cast<std::size_t>(h * w * ch));
  const bool planar = layout == "NCHW" || layout == "CHW";
  const std::array<double, 3> m{mean[0], mean[1], mean[2]};
  const std::array<double, 3> s{stdv[0], stdv[1], stdv[2]};
  switch (in.depth) {
    case PixelDepth::U8: toTensorRows<std::uint8_t>(in, scale, m, s, planar, t.data.data()); break;
    case PixelDepth::U16: toTensorRows<std::uint16_t>(in, scale, m, s, planar, t.data.data()); break;
    case PixelDepth::F32: toTensorRows<float>(in, scale, m, s, planar, t.data.data()); break;
  }
  outputs.set("tensor", Data::tensor(std::move(t)));
  return Status::Ok();
}

// -------------------------------------------------------------- tensor.to_image

/// 按布局把形状读成 (n, c, h, w) 与「通道是否交错」。读不出返回 false。
bool readLayout(const std::vector<std::int64_t>& s, const std::string& layout, std::int64_t& n,
                std::int64_t& c, std::int64_t& h, std::int64_t& w, bool& interleaved) {
  n = 1;
  c = 1;
  interleaved = false;
  if (layout == "HW" && s.size() == 2) { h = s[0]; w = s[1]; return true; }
  if (layout == "HWC" && s.size() == 3) { h = s[0]; w = s[1]; c = s[2]; interleaved = true; return true; }
  if (layout == "CHW" && s.size() == 3) { c = s[0]; h = s[1]; w = s[2]; return true; }
  if (layout == "NHWC" && s.size() == 4) {
    n = s[0]; h = s[1]; w = s[2]; c = s[3]; interleaved = true; return true;
  }
  if (layout == "NCHW" && s.size() == 4) { n = s[0]; c = s[1]; h = s[2]; w = s[3]; return true; }
  return false;
}

/// auto：与连线查看器的张量视图同一套猜法（末维 1/3/4 是交错的通道，否则首维是）。
std::string guessLayout(const std::vector<std::int64_t>& s) {
  const auto channelish = [](std::int64_t v) { return v == 1 || v == 3 || v == 4; };
  if (s.size() == 2) return "HW";
  if (s.size() == 3) return channelish(s[2]) ? "HWC" : "CHW";
  if (s.size() == 4) return channelish(s[3]) ? "NHWC" : "NCHW";
  return "";
}

Status toImageCompute(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                      ExecContext&) {
  const Tensor& t = *inputs.get("tensor").asTensor();
  if (!t.consistent()) return badInput("张量的数据与形状对不上", "tensor");
  std::string layout = params.choice("layout");
  if (layout == "auto") layout = guessLayout(t.shape);
  std::int64_t n = 0, c = 0, h = 0, w = 0;
  bool interleaved = false;
  if (!readLayout(t.shape, layout, n, c, h, w, interleaved)) {
    return badInput("形状 " + t.shapeString() + " 按 " + (layout.empty() ? "auto" : layout) +
                        " 读不成图像；在 Layout 里指定布局",
                    "tensor");
  }
  if (c != 1 && c != 3 && c != 4) {
    return badInput("通道数是 " + std::to_string(c) + "，图像只能是 1 / 3 / 4 个通道", "tensor");
  }
  if (h <= 0 || w <= 0 || h > (1 << 20) || w > (1 << 20)) {
    return badInput("图像的宽高不合理：" + std::to_string(w) + "×" + std::to_string(h), "tensor");
  }
  const std::int64_t slice = params.integer("slice");
  if (slice < 0 || slice >= n) {
    return badParam("批里只有 " + std::to_string(n) + " 张，第 " + std::to_string(slice) + " 张不存在",
                    "slice");
  }
  const std::size_t plane = static_cast<std::size_t>(h * w);
  const std::size_t base = static_cast<std::size_t>(slice) * plane * static_cast<std::size_t>(c);
  const auto valueAt = [&](std::int64_t x, std::int64_t y, std::int64_t k) {
    const std::size_t p = static_cast<std::size_t>(y * w + x);
    return t.data[base + (interleaved ? p * static_cast<std::size_t>(c) + static_cast<std::size_t>(k)
                                      : static_cast<std::size_t>(k) * plane + p)];
  };

  const bool toU8 = params.choice("depth") == "u8";
  float lo = 0, hi = 1;
  if (toU8) {
    // 拉伸到 0..255：按这一张的有限值范围
    lo = std::numeric_limits<float>::infinity();
    hi = -lo;
    for (std::size_t i = 0; i < plane * static_cast<std::size_t>(c); ++i) {
      const float v = t.data[base + i];
      if (!std::isfinite(v)) continue;
      lo = std::min(lo, v);
      hi = std::max(hi, v);
    }
    if (!(hi > lo)) { lo = 0; hi = 1; }
  }
  Image img = Image::allocate(static_cast<std::int32_t>(w), static_cast<std::int32_t>(h),
                              static_cast<std::int32_t>(c), toU8 ? PixelDepth::U8 : PixelDepth::F32);
  // 逐值的写法与原来一字不差（(v - lo) / (hi - lo) * 255 在 float 里算、lround、截断）；
  // 只是 u8 / f32 与交错 / 分平面不再每个值判一次
  const auto fill = [&](auto&& put) {
    for (std::int64_t y = 0; y < h; ++y) {
      for (std::int64_t x = 0; x < w; ++x) {
        for (std::int64_t k = 0; k < c; ++k) {
          put(static_cast<std::size_t>((y * w + x) * c + k), valueAt(x, y, k));
        }
      }
    }
  };
  if (toU8) {
    std::uint8_t* out8 = img.mutablePixels();
    fill([&](std::size_t at, float v) {
      const float u = std::isfinite(v) ? (v - lo) / (hi - lo) * 255.0f : 0.0f;
      out8[at] = static_cast<std::uint8_t>(std::lround(std::clamp(u, 0.0f, 255.0f)));
    });
  } else {
    float* out32 = reinterpret_cast<float*>(img.mutablePixels());
    if (interleaved) {
      std::memcpy(out32, t.data.data() + base, plane * static_cast<std::size_t>(c) * sizeof(float));
    } else {
      fill([&](std::size_t at, float v) { out32[at] = v; });
    }
  }
  outputs.set("image", Data::image(std::move(img)));
  return Status::Ok();
}

}  // namespace

void registerImageToTensor(Registry& r) {
  OperatorDesc op;
  op.id = "image.to_tensor";
  op.version = "1.0.0";
  op.label = "图像转张量";
  op.category = "图像/转换";
  op.keywords = {"tensor", "onnx", "preprocess", "nchw", "张量", "预处理", "推理"};
  op.doc = "推理前的预处理：值 × scale，再按通道 (v − mean) / std，排成指定布局的 float32 张量。"
           "u8 图像配 ImageNet 模型的常见写法：scale = 1/255，mean = (0.485, 0.456, 0.406)，"
           "std = (0.229, 0.224, 0.225)。尺寸先接 image.resize 调好。";
  op.inputs = {img::imageIn()};
  op.outputs = {Port{"tensor", "Tensor", "Tensor", "float32 张量，接 ml.onnx_run。", true}};
  Param layout = img::enumParam("layout", "Layout", "NCHW",
                                {EnumOption{"NCHW", "NCHW", "大多数 ONNX 视觉模型"},
                                 EnumOption{"NHWC", "NHWC", "TensorFlow 系的模型"},
                                 EnumOption{"CHW", "CHW", "不带批维"},
                                 EnumOption{"HWC", "HWC", "不带批维、通道交错"}},
                                "张量的维度顺序。");
  Param scale = img::floatParam("scale", "Scale", 1.0 / 255.0, "先乘的系数。");
  Param mean;
  mean.name = "mean";
  mean.type = ParamType::Vec3f;
  mean.label = "Mean";
  mean.doc = "乘完 scale 后每个通道减去的均值（单通道图只用第一项）。";
  mean.def = Value::vec({0.0, 0.0, 0.0});
  mean.componentLabels = {"R", "G", "B"};
  Param stdv = mean;
  stdv.name = "std";
  stdv.label = "Std";
  stdv.doc = "减完均值后每个通道除以的标准差。";
  stdv.def = Value::vec({1.0, 1.0, 1.0});
  op.params = {layout, scale, mean, stdv};
  op.capabilities = {false, false, true};
  op.compute = &toTensorCompute;
  r.addOperator(std::move(op));
}

void registerTensorToImage(Registry& r) {
  OperatorDesc op;
  op.id = "tensor.to_image";
  op.version = "1.0.0";
  op.label = "张量转图像";
  op.category = "图像/转换";
  op.keywords = {"tensor", "image", "mask", "张量", "图像", "分割结果"};
  op.doc = "把张量（分割掩膜、热力图、生成的图）读成图像。批里取一张；u8 时按这一张的范围拉到 0..255，"
           "f32 原值保留。通道要是 1 / 3 / 4。";
  op.inputs = {Port{"tensor", "Tensor", "Tensor", "要读成图像的张量。", true}};
  op.outputs = {img::imageOut()};
  op.params = {
      img::enumParam("layout", "Layout", "auto",
                     {EnumOption{"auto", "自动", "末维 1/3/4 当交错通道，否则首维当通道"},
                      EnumOption{"HW", "HW", ""}, EnumOption{"HWC", "HWC", ""},
                      EnumOption{"CHW", "CHW", ""}, EnumOption{"NHWC", "NHWC", ""},
                      EnumOption{"NCHW", "NCHW", ""}},
                     "怎么读形状。"),
      img::intParam("slice", "Slice", 0, 0, 1 << 20, "带批维时取第几张。"),
      img::enumParam("depth", "Depth", "f32",
                     {EnumOption{"f32", "f32", "原值"}, EnumOption{"u8", "u8", "拉伸到 0..255"}},
                     "输出位深。"),
  };
  op.capabilities = {false, false, true};
  op.compute = &toImageCompute;
  r.addOperator(std::move(op));
}

}  // namespace lyflow::ops
