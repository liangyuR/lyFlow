#pragma once
// 合成图像的测试算子 test.make_image / test.take_gray（docs/image-plan.md 阶段 1）。
// 与 param_showcase_op.h 同一个身份：doctest 经 ensureTestOps() 注册；编辑器 e2e 跑的是真的 app，
// 所以也编进 core，只在进程环境里 LYFLOW_TEST_OPS=1 时注册（core/tests/e2e/register_e2e_ops.cpp）。
// 阶段 1 还没有 OpenCV，图像数据域从 core 到连线查看器的整条链路靠它有东西可看。
#include <cmath>
#include <cstring>
#include <limits>

#include "lyflow/operator.h"
#include "lyflow/registry.h"

namespace lyflow::test {
namespace image {

/// (x, y, c) 处的合成值：水平渐变 + 竖直渐变 + 通道偏移，三种位深各自铺满一段量程，
/// 测试按同一个公式断言（testImageValue）。
inline double testImageValue(PixelDepth depth, std::int32_t x, std::int32_t y, std::int32_t c) {
  switch (depth) {
    case PixelDepth::U8:
      return static_cast<double>((x * 3 + y * 5 + c * 60) % 256);
    case PixelDepth::U16:
      return static_cast<double>((x * 300 + y * 7 + c * 1000) % 65536);
    case PixelDepth::F32:
      return static_cast<double>(x) * 0.5 + static_cast<double>(y) * 0.25 + c * 100.0;
  }
  return 0;
}

inline Image makeTestImage(std::int32_t w, std::int32_t h, std::int32_t c, PixelDepth d,
                           bool injectNaN = false) {
  Image img = Image::allocate(w, h, c, d);
  std::uint8_t* p = img.mutablePixels();
  for (std::int32_t y = 0; y < h; ++y) {
    for (std::int32_t x = 0; x < w; ++x) {
      for (std::int32_t k = 0; k < c; ++k) {
        const double v = testImageValue(d, x, y, k);
        const std::size_t at = (static_cast<std::size_t>(y) * w + x) * c + k;
        if (d == PixelDepth::U8) {
          p[at] = static_cast<std::uint8_t>(v);
        } else if (d == PixelDepth::U16) {
          const std::uint16_t u = static_cast<std::uint16_t>(v);
          std::memcpy(p + at * 2, &u, 2);
        } else {
          const float f = (injectNaN && x == 0 && y == 0) ? std::numeric_limits<float>::quiet_NaN()
                                                          : static_cast<float>(v);
          std::memcpy(p + at * 4, &f, 4);
        }
      }
    }
  }
  return img;
}

inline PixelDepth depthOf(const std::string& name) {
  if (name == "u16") return PixelDepth::U16;
  if (name == "f32") return PixelDepth::F32;
  return PixelDepth::U8;
}

inline Status makeImageCompute(const Inputs&, const ParamView& params, Outputs& outputs,
                               ExecContext&) {
  const auto w = static_cast<std::int32_t>(params.integer("width"));
  const auto h = static_cast<std::int32_t>(params.integer("height"));
  const auto c = static_cast<std::int32_t>(params.integer("channels"));
  if (c != 1 && c != 3 && c != 4) {
    return Status::Error(Phase::Execute, "bad_param", "channels 只能是 1 / 3 / 4", "channels");
  }
  outputs.set("image", Data::image(makeTestImage(w, h, c, depthOf(params.choice("depth")),
                                                 params.flag("injectNaN"))));
  return Status::Ok();
}

inline Status takeGrayCompute(const Inputs& inputs, const ParamView&, Outputs& outputs,
                              ExecContext&) {
  const Image* img = inputs.get("image").asImage();
  Measurement m;
  m.ok = img != nullptr;
  m.unit = "px²";  // 像素个数，量纲是 px²（预览按 s² 换回原图，ADR-0028）
  m.value = img ? static_cast<double>(img->pixelCount()) : 0.0;
  outputs.set("pixels", Data::measurement(m));
  return Status::Ok();
}

}  // namespace image

/// 注册 test.make_image 与 test.take_gray。已经注册过就什么都不做。
inline void registerImageTestOps(Registry& r) {
  if (r.find("test.make_image")) return;
  {
    OperatorDesc op;
    op.id = "test.make_image";
    op.version = "1.0.0";
    op.label = "合成图像";
    op.category = "Test";
    op.keywords = {"image", "test", "图像", "合成"};
    op.doc = "只在测试里注册：按宽高、通道、位深产一张确定的渐变图（阶段 1 的图像来源）。";
    op.outputs = {Port{"image", "Image", "Image", "合成的渐变图。", true}};
    auto intParam = [](const char* name, std::int64_t def, double min, double max) {
      Param p;
      p.name = name;
      p.type = ParamType::Int;
      p.label = name;
      p.def = Value::integer(def);
      p.min = min;
      p.max = max;
      return p;
    };
    Param depth;
    depth.name = "depth";
    depth.type = ParamType::Enum;
    depth.label = "depth";
    depth.def = Value::text("u8");
    depth.options = {EnumOption{"u8", "u8", ""}, EnumOption{"u16", "u16", ""},
                     EnumOption{"f32", "f32", ""}};
    Param nan;
    nan.name = "injectNaN";
    nan.type = ParamType::Bool;
    nan.label = "injectNaN";
    nan.def = Value::boolean(false);
    nan.doc = "f32 时把 (0, 0) 写成 NaN。";
    op.params = {intParam("width", 64, 1, 8192), intParam("height", 48, 1, 8192),
                 intParam("channels", 3, 1, 4), depth, nan};
    op.capabilities = {false, false, true};
    op.compute = &image::makeImageCompute;
    r.addOperator(std::move(op));
  }
  {
    OperatorDesc op;
    op.id = "test.take_gray";
    op.version = "1.0.0";
    op.label = "要灰度图";
    op.category = "Test";
    op.doc = "只在测试里注册：输入端口声明 shape 契约 [-1, -1, 1]，输出像素数。";
    op.inputs = {withContract(Port{"image", "Image", "Image", "单通道图像。", true},
                              nlohmann::json{{"shape", {-1, -1, 1}}})};
    op.outputs = {Port{"pixels", "Measurement", "Pixels", "像素数。", true}};
    op.capabilities = {false, false, true};
    op.compute = &image::takeGrayCompute;
    r.addOperator(std::move(op));
  }
}

}  // namespace lyflow::test
