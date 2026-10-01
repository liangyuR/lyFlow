// 深度图 ↔ 点云（docs/image-plan.md §4.1，阶段 4）：图像域与点云域在同一张图里互转。
// 针孔模型、OpenCV 的相机坐标约定（x 右、y 下、z 前，米）。这两个算子不需要 OpenCV。
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

#include "ops.h"
#include "params.h"

namespace lyflow::ops {
namespace {

using img::badInput;
using img::badParam;

struct Intrinsics {
  double fx, fy, cx, cy;
};

Status readIntrinsics(const ParamView& params, Intrinsics& k) {
  k = {params.number("fx"), params.number("fy"), params.number("cx"), params.number("cy")};
  if (!(k.fx > 0)) return badParam("fx 要大于 0（像素为单位的焦距）", "fx");
  if (!(k.fy > 0)) return badParam("fy 要大于 0（像素为单位的焦距）", "fy");
  return Status::Ok();
}

/// 第 i 个像素第 c 个通道的原值（i 是行主序的像素下标）。
double sample(const Image& img, std::size_t i, int c) {
  const std::uint8_t* p = img.pixels.get() + (i * img.channels + c) * static_cast<std::size_t>(img.depth);
  switch (img.depth) {
    case PixelDepth::U8:
      return *p;
    case PixelDepth::U16: {
      std::uint16_t v;
      std::memcpy(&v, p, sizeof v);
      return v;
    }
    case PixelDepth::F32: {
      float v;
      std::memcpy(&v, p, sizeof v);
      return v;
    }
  }
  return 0;
}

/// 彩色图的一个通道转成 0..255（D5）：u8 原样，u16 取高 8 位，f32 按 0..1 映射。
std::uint8_t colorByte(const Image& img, std::size_t i, int c) {
  const double v = sample(img, i, c);
  if (img.depth == PixelDepth::U8) return static_cast<std::uint8_t>(v);
  if (img.depth == PixelDepth::U16) return static_cast<std::uint8_t>(static_cast<unsigned>(v) >> 8);
  if (!std::isfinite(v)) return 0;
  return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0));
}

// ------------------------------------------------------------ cloud.from_depth

Status fromDepthCompute(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                        ExecContext& ctx) {
  const Image& depth = *inputs.get("depth").asImage();
  Intrinsics k{};
  if (Status s = readIntrinsics(params, k); !s.ok) return s;
  const double scale = params.number("depthScale");
  if (!(scale > 0)) return badParam("depthScale 要大于 0（深度值 × 它 = 米）", "depthScale");
  const double minZ = params.number("minDepth");
  const double maxZ = params.number("maxDepth");
  const int step = static_cast<int>(params.integer("step"));
  if (maxZ > 0 && minZ > maxZ) {
    return badParam("minDepth 比 maxDepth 还大，一个点都出不来（maxDepth = 0 表示不限）", "minDepth");
  }
  const Image* color = nullptr;
  if (inputs.has("color")) {
    color = inputs.get("color").asImage();
    if (color->width != depth.width || color->height != depth.height) {
      return badInput("彩色图 " + std::to_string(color->width) + "×" + std::to_string(color->height) +
                          " 与深度图 " + std::to_string(depth.width) + "×" + std::to_string(depth.height) +
                          " 不一样大（要先配准到深度图上）",
                      "color");
    }
    if (color->channels < 3) return badInput("彩色图要是 RGB / RGBA（收到单通道）", "color");
  }

  PointCloud cloud;
  const std::size_t expected = depth.pixelCount() / (static_cast<std::size_t>(step) * step) + 1;
  cloud.reserve(expected);
  if (color) cloud.rgb.reserve(expected * 3);
  for (std::int32_t v = 0; v < depth.height; v += step) {
    if (ctx.cancelled()) return Status::Ok();
    for (std::int32_t u = 0; u < depth.width; u += step) {
      const std::size_t i = static_cast<std::size_t>(v) * depth.width + u;
      const double raw = sample(depth, i, 0);
      const double z = raw * scale;
      // D4：0、非有限值、范围外都是「没测到」
      if (!std::isfinite(z) || z <= 0 || z < minZ || (maxZ > 0 && z > maxZ)) continue;
      cloud.push(static_cast<float>((u - k.cx) * z / k.fx), static_cast<float>((v - k.cy) * z / k.fy),
                 static_cast<float>(z));
      if (color) {
        cloud.rgb.push_back(colorByte(*color, i, 0));
        cloud.rgb.push_back(colorByte(*color, i, 1));
        cloud.rgb.push_back(colorByte(*color, i, 2));
      }
    }
  }
  ctx.log(LogLevel::Info, "深度图 " + std::to_string(depth.width) + "×" + std::to_string(depth.height) +
                              " → " + std::to_string(cloud.pointCount()) + " 个点");
  outputs.set("cloud", Data::cloud(std::move(cloud)));
  return Status::Ok();
}

// -------------------------------------------------------- cloud.to_depth_image

Status toDepthCompute(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                      ExecContext& ctx) {
  const PointCloud& cloud = *inputs.get("cloud").asCloud();
  Intrinsics k{};
  if (Status s = readIntrinsics(params, k); !s.ok) return s;
  const auto w = static_cast<std::int32_t>(params.integer("width"));
  const auto h = static_cast<std::int32_t>(params.integer("height"));
  const bool asU16 = params.choice("depth") == "u16";
  const double scale = params.number("depthScale");
  if (asU16 && !(scale > 0)) return badParam("depthScale 要大于 0（深度值 × 它 = 米）", "depthScale");

  // z 缓冲（D7）：一个像素落多个点取最近的
  std::vector<float> zbuf(static_cast<std::size_t>(w) * h, std::numeric_limits<float>::infinity());
  std::size_t kept = 0;
  const std::size_t n = cloud.pointCount();
  for (std::size_t p = 0; p < n; ++p) {
    if ((p & 0xFFFF) == 0 && ctx.cancelled()) return Status::Ok();
    const double x = cloud.xyz[p * 3], y = cloud.xyz[p * 3 + 1], z = cloud.xyz[p * 3 + 2];
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || z <= 0) continue;
    // 先在 double 上判断落在图内、再取整：lround 返回 long，Windows 上是 32 位，超范围（z 几乎为 0 的点
    // 投出几十亿）时 MSVC 返回 0 —— 0 能过越界检查，点就被写进第 0 列 / 行（review 修正，PR #2）。
    // NaN 也在这里被比较挡掉
    const double pu = k.fx * x / z + k.cx;
    const double pv = k.fy * y / z + k.cy;
    if (!(pu > -0.5 && pu < w - 0.5 && pv > -0.5 && pv < h - 0.5)) continue;
    // u16 输出取整后为 0（比半个单位还近）或超过 65535 的点丢掉：不然前者抢下像素又被写成 0（无效），
    // 后者被截成 65535 冒充一个合法深度（D7）
    if (asU16) {
      const double d = std::round(z / scale);
      if (d < 1 || d > 65535) continue;
    }
    const auto u = static_cast<std::size_t>(std::lround(pu));
    const auto v = static_cast<std::size_t>(std::lround(pv));
    float& slot = zbuf[v * static_cast<std::size_t>(w) + u];
    if (static_cast<float>(z) < slot) {
      if (!std::isfinite(slot)) ++kept;
      slot = static_cast<float>(z);
    }
  }

  Image out = Image::allocate(w, h, 1, asU16 ? PixelDepth::U16 : PixelDepth::F32);
  for (std::size_t i = 0; i < zbuf.size(); ++i) {
    const float z = zbuf[i];
    const bool hit = std::isfinite(z);
    if (asU16) {
      // 进 z 缓冲之前按 double 查过 1 ≤ d ≤ 65535；z 缓冲存的是 float，边上差半个单位也不许溢出 / 变成 0
      const auto u16 = static_cast<std::uint16_t>(hit ? std::clamp(std::round(z / scale), 1.0, 65535.0) : 0.0);
      std::memcpy(out.mutablePixels() + i * 2, &u16, 2);
    } else {
      const float f = hit ? z : 0.0f;  // 没落到点的像素是 0，与 from_depth 的「0 = 没测到」一致
      std::memcpy(out.mutablePixels() + i * 4, &f, 4);
    }
  }
  ctx.log(LogLevel::Info, std::to_string(n) + " 个点投影到 " + std::to_string(w) + "×" + std::to_string(h) +
                              "，" + std::to_string(kept) + " 个像素有深度");
  outputs.set("depth", Data::image(std::move(out)));
  return Status::Ok();
}

/// fx fy cx cy：两个算子同一组写法，便于提升成图参数共用（D2）。
std::vector<Param> intrinsicParams() {
  auto make = [](const char* name, const char* label, double def, const char* doc) {
    Param p = img::floatParam(name, label, def, doc);
    p.unit = "px";
    p.group = "内参";
    return p;
  };
  return {make("fx", "fx", 600.0, "x 方向焦距（像素）。"), make("fy", "fy", 600.0, "y 方向焦距（像素）。"),
          make("cx", "cx", 320.0, "主点 x（像素）。"), make("cy", "cy", 240.0, "主点 y（像素）。")};
}

Param depthScaleParam() {
  Param p = img::floatParam("depthScale", "Depth Scale", 0.001,
                            "深度值 × 它 = 米。u16 的毫米深度图是 0.001；f32 的米制深度图设 1。");
  p.min = 0.0;
  p.unit = "m";
  return p;
}

}  // namespace

void registerCloudFromDepth(Registry& r) {
  OperatorDesc op;
  op.id = "cloud.from_depth";
  op.version = "1.0.0";
  op.label = "深度图转点云";
  op.category = "图像/转换";
  op.keywords = {"depth", "point cloud", "rgbd", "deproject", "深度图", "点云", "反投影"};
  op.doc = "按针孔内参把深度图反投影成点云（相机坐标系：x 右、y 下、z 前，米）。0、非有限值、"
           "深度范围外的像素不出点。接了 color（与深度图同样大、已配准的 RGB 图）就给点上色。"
           "要换到世界坐标系接 transform.apply。";
  op.inputs = {
      withContract(Port{"depth", "Image", "Depth", "单通道深度图（u16 毫米或 f32 米最常见）。", true},
                   nlohmann::json{{"shape", {-1, -1, 1}}}),
      Port{"color", "Image", "Color", "可选：与深度图对齐的彩色图，给点填 rgb。", false},
  };
  op.outputs = {Port{"cloud", "PointCloud", "Cloud", "相机坐标系下的点云（无序，只含有效像素）。", true}};
  std::vector<Param> params = intrinsicParams();
  params.push_back(depthScaleParam());
  Param minD = img::floatParam("minDepth", "Min Depth", 0.0, "比它近的像素不出点（米）。");
  minD.unit = "m";
  minD.min = 0.0;
  Param maxD = img::floatParam("maxDepth", "Max Depth", 0.0, "比它远的像素不出点（米）；0 = 不限。");
  maxD.unit = "m";
  maxD.min = 0.0;
  params.push_back(minD);
  params.push_back(maxD);
  Param step = img::intParam("step", "Step", 1, 1, 64, "每隔几个像素取一个；1 = 全取。");
  step.unit = "px";
  params.push_back(step);
  op.params = std::move(params);
  op.capabilities = {true, false, true};
  op.compute = &fromDepthCompute;
  r.addOperator(std::move(op));
}

void registerCloudToDepthImage(Registry& r) {
  OperatorDesc op;
  op.id = "cloud.to_depth_image";
  op.version = "1.0.0";
  op.label = "点云转深度图";
  op.category = "图像/转换";
  op.keywords = {"depth", "project", "range image", "深度图", "投影", "点云"};
  op.doc = "按针孔内参把相机坐标系下的点云投影成深度图：一个像素落多个点取最近的，没落到点的像素是 0，"
           "z ≤ 0 与投影到图外的点丢掉；u16 输出时超出量程（取整后为 0 或大于 65535）的点也丢掉。"
           "与 cloud.from_depth 互逆：同一组内参、同一个 depthScale（f32 输出是米，对应 from_depth 的 depthScale = 1）"
           "来回一趟，有效像素不变。";
  op.inputs = {Port{"cloud", "PointCloud", "Cloud", "相机坐标系下的点云（x 右、y 下、z 前，米）。", true}};
  op.outputs = {img::imageOut("depth", "单通道深度图。")};
  std::vector<Param> params = intrinsicParams();
  // 输出图的尺寸是绝对的：预览缩小时不跟着换算（ADR-0028）
  Param w = img::intParam("width", "Width", 640, 1, 16384, "输出宽度（像素）。");
  w.unit = "px";
  w.absolute = true;
  Param h = img::intParam("height", "Height", 480, 1, 16384, "输出高度（像素）。");
  h.unit = "px";
  h.absolute = true;
  params.push_back(w);
  params.push_back(h);
  params.push_back(img::enumParam("depth", "Depth", "u16",
                                  {EnumOption{"u16", "u16", "÷ depthScale 四舍五入（默认毫米）"},
                                   EnumOption{"f32", "f32", "米"}},
                                  "输出位深。"));
  Param scale = depthScaleParam();
  scale.visibleWhen = img::when("depth", Value::text("u16"));
  params.push_back(scale);
  op.params = std::move(params);
  op.capabilities = {true, false, true};
  op.compute = &toDepthCompute;
  r.addOperator(std::move(op));
}

}  // namespace lyflow::ops
