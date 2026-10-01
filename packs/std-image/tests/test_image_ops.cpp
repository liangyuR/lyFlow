// std-image 包的算子测试（docs/image-plan.md 阶段 2）：adapter 的零拷贝与 RGB 顺序、读写往返（含中文路径）、
// 每个算子的数值、像素单位的几何、单通道契约。图像一律现造（合成渐变 / cv::circle 画的圆），仓库不进图片。
#include <doctest/doctest.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <string>
#include <unordered_map>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include "exec/result_store.h"
#include "helpers.h"
#include "image_test_op.h"
#include "lyflow/operator.h"
#include "lyflow/registry.h"
#include "lyflow_cv/adapter.h"
#include "test_ops.h"

namespace {

using namespace lyflow;
using lyflow::test::Json;

/// 直接调一个算子的 compute（不经执行器）。参数先填默认值再覆盖。
/// 直接调 compute（test::OpCall），外加按端口取图像。
struct Call : test::OpCall {
  const Image& image(const char* port = "image") {
    REQUIRE(outputs.count(port));
    const Image* img = outputs[port].asImage();
    REQUIRE(img != nullptr);
    return *img;
  }
};

Image makeRgb(int w, int h) { return test::image::makeTestImage(w, h, 3, PixelDepth::U8); }

bool samePixels(const Image& a, const Image& b) {
  return a.width == b.width && a.height == b.height && a.channels == b.channels &&
         a.depth == b.depth && std::memcmp(a.pixels.get(), b.pixels.get(), a.byteSize()) == 0;
}

}  // namespace

TEST_CASE("adapter：view 与 fromMat 都不拷贝像素；不连续的子矩阵先 clone；core 不认的位深 / 通道拒掉") {
  const Image src = makeRgb(8, 4);
  const cv::Mat v = cvx::view(src);
  CHECK(v.data == src.pixels.get());
  CHECK(v.type() == CV_8UC3);
  CHECK(v.at<cv::Vec3b>(2, 5)[1] == static_cast<unsigned char>(test::image::testImageValue(PixelDepth::U8, 5, 2, 1)));

  cv::Mat owned(3, 5, CV_16UC1, cv::Scalar(1234));
  const std::uint8_t* raw = owned.data;
  Image img;
  REQUIRE(cvx::fromMat(owned, img));
  CHECK(img.pixels.get() == raw);  // 零拷贝：别名 shared_ptr 持有 Mat
  owned.release();
  CHECK(img.at(4, 2, 0) == 1234.0);  // Mat 那边放手了，像素还活着

  cv::Mat big(10, 10, CV_8UC1, cv::Scalar(7));
  Image sub;
  REQUIRE(cvx::fromMat(big(cv::Rect(2, 2, 4, 3)), sub));
  CHECK(sub.width == 4);
  CHECK(sub.consistent());

  Image bad;
  CHECK_FALSE(cvx::fromMat(cv::Mat(2, 2, CV_8UC2), bad));
  CHECK_FALSE(cvx::fromMat(cv::Mat(2, 2, CV_64FC1), bad));
  CHECK(cvx::toSupportedDepth(cv::Mat(2, 2, CV_64FC1)).depth() == CV_32F);
}

TEST_CASE("io.save_image → io.load_image 往返逐像素相同（RGB 顺序不被 BGR 颠倒、u16 保留）；中文路径；f32 存 PNG 被拒") {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / std::filesystem::u8path("lyflow-图像-测试");
  std::filesystem::create_directories(dir);
  Call c;
  c.base = dir;

  SUBCASE("RGB u8 PNG") {
    const Image rgb = makeRgb(17, 9);
    c.inputs["image"] = Data::image(rgb);
    REQUIRE(c.run("io.save_image", {{"path", Value::text("彩色.png")}}).ok);
    c.inputs.clear();
    REQUIRE(c.run("io.load_image", {{"path", Value::text("彩色.png")}}).ok);
    CHECK(samePixels(c.image(), rgb));
  }
  SUBCASE("u16 灰度 PNG，按 gray 读回仍是 u16") {
    const Image g16 = test::image::makeTestImage(12, 7, 1, PixelDepth::U16);
    c.inputs["image"] = Data::image(g16);
    REQUIRE(c.run("io.save_image", {{"path", Value::text("深度.png")}}).ok);
    c.inputs.clear();
    REQUIRE(c.run("io.load_image", {{"path", Value::text("深度.png")}, {"mode", Value::text("gray")}}).ok);
    CHECK(samePixels(c.image(), g16));
  }
  SUBCASE("f32 不能存 PNG；文件不存在报 io") {
    c.inputs["image"] = Data::image(test::image::makeTestImage(4, 4, 1, PixelDepth::F32));
    const Status s = c.run("io.save_image", {{"path", Value::text("x.png")}});
    CHECK(s.code == "bad_param");
    CHECK(s.paramPath == "path");
    c.inputs.clear();
    CHECK(c.run("io.load_image", {{"path", Value::text("没有这个.png")}}).code == "io");
  }
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

TEST_CASE("image.to_gray / resize / crop：RGB 加权、灰度原样共享像素；尺寸；像素框裁剪，米制的框被拒；全宽裁剪不借上游的像素") {
  Call c;
  Image rgb = Image::allocate(4, 2, 3, PixelDepth::U8);
  for (std::size_t i = 0; i < 8; ++i) {
    rgb.mutablePixels()[i * 3 + 0] = 200;  // R
    rgb.mutablePixels()[i * 3 + 1] = 0;
    rgb.mutablePixels()[i * 3 + 2] = 0;
  }
  c.inputs["image"] = Data::image(rgb);
  REQUIRE(c.run("image.to_gray").ok);
  // BT.601：0.299 R —— 要是按 BGR 读，这里会是 0.114 × 200 ≈ 23
  CHECK(c.image().at(0, 0, 0) == doctest::Approx(0.299 * 200).epsilon(0.02));

  const Data gray = c.outputs["image"];
  c.inputs["image"] = gray;
  REQUIRE(c.run("image.to_gray").ok);
  CHECK(c.image().pixels.get() == gray.asImage()->pixels.get());

  c.inputs["image"] = Data::image(makeRgb(100, 60));
  REQUIRE(c.run("image.resize", {{"scale", Value::number(0.5)}}).ok);
  CHECK(c.image().width == 50);
  CHECK(c.image().height == 30);
  REQUIRE(c.run("image.resize", {{"mode", Value::text("size")}, {"width", Value::integer(7)},
                                 {"height", Value::integer(3)}}).ok);
  CHECK(c.image().width == 7);

  REQUIRE(c.run("image.crop", {{"roi", Value::vec({10, 5, 30, 13})}}).ok);
  CHECK(c.image().width == 20);
  CHECK(c.image().at(0, 0, 2) == test::image::testImageValue(PixelDepth::U8, 10, 5, 2));
  Box2D box;
  box.min[0] = 90;
  box.min[1] = 50;
  box.max[0] = 200;  // 超出图像的部分截掉
  box.max[1] = 55;
  box.unit = Unit2D::Pixel;
  c.inputs["box"] = Data::box2d(box);
  REQUIRE(c.run("image.crop").ok);
  CHECK(c.image().width == 10);
  CHECK(c.image().height == 5);
  box.unit = Unit2D::Meter;
  c.inputs["box"] = Data::box2d(box);
  CHECK(c.run("image.crop").code == "bad_input");

  // 全宽 / 单行的子矩阵在 OpenCV 里是连续的：修前 fromMat 不拷，输出指着输入的缓冲却不持有它，
  // 输入一放掉就读到已释放的内存（review 修正，PR #1）。输出必须有自己的一份
  c.inputs.erase("box");
  for (const auto& roi : {std::vector<double>{0, 10, 100, 30}, std::vector<double>{5, 7, 60, 8}}) {
    Data in = Data::image(makeRgb(100, 60));
    const std::uint8_t* lo = in.asImage()->pixels.get();
    const std::uint8_t* hi = lo + in.asImage()->byteSize();
    c.inputs["image"] = in;
    REQUIRE(c.run("image.crop", {{"roi", Value::vec(roi)}}).ok);
    const Data out = c.outputs["image"];
    const std::uint8_t* p = out.asImage()->pixels.get();
    CHECK_MESSAGE((p < lo || p >= hi), "裁剪输出借着输入的像素：roi = ", roi[0], ",", roi[1], ",", roi[2], ",", roi[3]);
    c.inputs.clear();
    in = Data();
    c.outputs.clear();
    const int x0 = static_cast<int>(roi[0]), y0 = static_cast<int>(roi[1]);
    CHECK(out.asImage()->at(3, 0, 1) == test::image::testImageValue(PixelDepth::U8, x0 + 3, y0, 1));
  }
}

TEST_CASE("image.threshold：Otsu 分开两块灰度；单通道契约拦住 RGB；Otsu 不收 u16") {
  ensureRegistry();
  lyflow::test::ensureTestOps();
  Image g = Image::allocate(20, 10, 1, PixelDepth::U8);
  for (int y = 0; y < 10; ++y) {
    for (int x = 0; x < 20; ++x) g.mutablePixels()[y * 20 + x] = x < 8 ? 40 : 210;
  }
  Call c;
  c.inputs["image"] = Data::image(g);
  REQUIRE(c.run("image.threshold").ok);
  const Image& mask = c.image("mask");
  CHECK(mask.depth == PixelDepth::U8);
  CHECK(mask.at(3, 3, 0) == 0.0);
  CHECK(mask.at(15, 3, 0) == 255.0);

  c.inputs["image"] = Data::image(test::image::makeTestImage(8, 8, 1, PixelDepth::U16));
  CHECK(c.run("image.threshold").code == "bad_input");
  CHECK(c.run("image.threshold", {{"method", Value::text("fixed")}, {"thresh", Value::number(1000)}}).ok);

  // 契约在执行器里查：接一张 RGB 图报 contract_violation，期望 / 实际形状都记下
  const Json doc = test::makeGraph(
      {test::N{"src", "test.make_image", Json{{"width", 6}, {"height", 4}, {"channels", 3}}},
       test::N{"bin", "image.threshold", Json::object()}},
      {test::E{"src.image", "bin.image"}});
  test::Session s(doc);
  test::RunLog& log = s.wait();
  CHECK(log.nodeEvent("bin", "error")["error"]["code"] == "contract_violation");
}

TEST_CASE("image.find_circle：画一个圆找回来（像素坐标、unit = px 进 valueJson）；没有圆报 circle_not_found") {
  cv::Mat m(200, 240, CV_8UC1, cv::Scalar(0));
  cv::circle(m, cv::Point(130, 90), 40, cv::Scalar(255), 3);
  cv::GaussianBlur(m, m, cv::Size(5, 5), 1.5);
  Image img;
  REQUIRE(cvx::fromMat(m, img));
  Call c;
  c.inputs["image"] = Data::image(img);
  REQUIRE(c.run("image.find_circle", {{"minRadius", Value::number(20)}, {"maxRadius", Value::number(60)}}).ok);
  const Circle2D* circle = c.outputs["circle"].asCircle2D();
  REQUIRE(circle != nullptr);
  CHECK(circle->center[0] == doctest::Approx(130).epsilon(0.03));
  CHECK(circle->center[1] == doctest::Approx(90).epsilon(0.03));
  CHECK(circle->radius == doctest::Approx(40).epsilon(0.08));
  CHECK(circle->unit == Unit2D::Pixel);
  CHECK(Json::parse(c.outputs["circle"].valueJson())["unit"] == "px");
  // 预览把半径缩小之后可能不足 1（4 倍预览里的 3 → 0.75）：截断成 0 对 OpenCV 是「不限」，会找回这个半径 40 的圆。
  // 四舍五入、正数至少 1 —— 半径上限 1 找不到它（review 第二轮）
  CHECK(c.run("image.find_circle", {{"maxRadius", Value::number(0.75)}}).code == "circle_not_found");

  c.inputs["image"] = Data::image(Image::allocate(64, 64, 1, PixelDepth::U8));
  const Status none = c.run("image.find_circle");
  CHECK(none.code == "circle_not_found");
}

TEST_CASE("image.region_stats：掩膜内的均值、面积、外接框（像素，可直接接 crop）；掩膜尺寸不对或多通道报 bad_input；f32 掩膜的小正值也算前景") {
  const Image g = test::image::makeTestImage(30, 20, 1, PixelDepth::U8);
  Image mask = Image::allocate(30, 20, 1, PixelDepth::U8);
  for (int y = 5; y < 9; ++y) {
    for (int x = 10; x < 16; ++x) mask.mutablePixels()[y * 30 + x] = 255;
  }
  double sum = 0;
  for (int y = 5; y < 9; ++y) {
    for (int x = 10; x < 16; ++x) sum += test::image::testImageValue(PixelDepth::U8, x, y, 0);
  }
  Call c;
  c.inputs["image"] = Data::image(g);
  c.inputs["mask"] = Data::image(mask);
  REQUIRE(c.run("image.region_stats").ok);
  CHECK(c.outputs["area"].asMeasurement()->value == 24.0);
  CHECK(c.outputs["mean"].asMeasurement()->value == doctest::Approx(sum / 24.0));
  const Box2D* b = c.outputs["bbox"].asBox2D();
  CHECK(b->unit == Unit2D::Pixel);
  CHECK(std::vector<float>{b->min[0], b->min[1], b->max[0], b->max[1]} ==
        std::vector<float>{10, 5, 16, 9});

  // 不接掩膜：区域就是整张图（不造全 255 的掩膜、直接算的那条路），与接一张全 255 的掩膜逐位相同
  for (const auto& [channels, depth] : {std::pair{1, PixelDepth::U8}, std::pair{3, PixelDepth::U16}}) {
    CAPTURE(channels);
    Call whole;
    whole.inputs["image"] = Data::image(test::image::makeTestImage(30, 20, channels, depth));
    Image all = Image::allocate(30, 20, 1, PixelDepth::U8);
    std::fill(all.mutablePixels(), all.mutablePixels() + 600, std::uint8_t{255});
    whole.inputs["mask"] = Data::image(all);
    const std::unordered_map<std::string, Value> ch{{"channel", Value::integer(channels - 1)}};
    REQUIRE(whole.run("image.region_stats", ch).ok);
    const double maskedMean = whole.outputs["mean"].asMeasurement()->value;
    whole.inputs.erase("mask");
    REQUIRE(whole.run("image.region_stats", ch).ok);
    CHECK(whole.outputs["area"].asMeasurement()->value == 600.0);
    CHECK(whole.outputs["mean"].asMeasurement()->value == maskedMean);
    const Box2D* wb = whole.outputs["bbox"].asBox2D();
    CHECK(std::vector<float>{wb->min[0], wb->min[1], wb->max[0], wb->max[1]} == std::vector<float>{0, 0, 30, 20});
  }

  c.inputs["mask"] = Data::image(Image::allocate(3, 3, 1, PixelDepth::U8));
  CHECK(c.run("image.region_stats").code == "bad_input");
  c.inputs["mask"] = Data::image(Image::allocate(30, 20, 3, PixelDepth::U8));
  CHECK(c.run("image.region_stats").code == "bad_input");  // 修前 OpenCV 抛异常、报成 internal

  Image fmask = Image::allocate(30, 20, 1, PixelDepth::F32);
  float* fp = reinterpret_cast<float*>(fmask.mutablePixels());
  fp[0] = 0.3f;  // 修前先转 u8 被舍成 0、当成背景
  fp[1] = std::numeric_limits<float>::quiet_NaN();  // NaN 不算
  c.inputs["mask"] = Data::image(fmask);
  REQUIRE(c.run("image.region_stats").ok);
  CHECK(c.outputs["area"].asMeasurement()->value == 1.0);
}

TEST_CASE("image.to_tensor ↔ tensor.to_image：NCHW 的 (v·scale − mean)/std；CHW 张量读回 f32 原值；u8 按范围拉伸") {
  const Image rgb = makeRgb(5, 3);
  Call c;
  c.inputs["image"] = Data::image(rgb);
  REQUIRE(c.run("image.to_tensor", {{"mean", Value::vec({0.5, 0.0, 0.0})}, {"std", Value::vec({0.5, 1.0, 1.0})}}).ok);
  const Tensor* t = c.outputs["tensor"].asTensor();
  REQUIRE(t != nullptr);
  CHECK(t->shape == std::vector<std::int64_t>{1, 3, 3, 5});
  // 第 0 通道 (x=2, y=1)：((v / 255) − 0.5) / 0.5
  const double v = test::image::testImageValue(PixelDepth::U8, 2, 1, 0);
  CHECK(t->data[(0 * 3 + 1) * 5 + 2] == doctest::Approx((v / 255.0 - 0.5) / 0.5));
  // 第 1 通道只除 1：原值 / 255
  CHECK(t->data[(1 * 3 + 1) * 5 + 2] ==
        doctest::Approx(test::image::testImageValue(PixelDepth::U8, 2, 1, 1) / 255.0));

  REQUIRE(c.run("image.to_tensor", {{"layout", Value::text("CHW")}, {"scale", Value::number(1.0)}}).ok);
  c.inputs.clear();
  c.inputs["tensor"] = c.outputs["tensor"];
  REQUIRE(c.run("tensor.to_image").ok);
  const Image& back = c.image();
  CHECK(back.depth == PixelDepth::F32);
  CHECK(back.channels == 3);
  CHECK(back.at(4, 2, 2) == test::image::testImageValue(PixelDepth::U8, 4, 2, 2));
  REQUIRE(c.run("tensor.to_image", {{"depth", Value::text("u8")}}).ok);
  CHECK(c.image().depth == PixelDepth::U8);

  Tensor bad;
  bad.shape = {2, 5, 5};  // 首维 2 不是通道数
  bad.data.assign(50, 0.0f);
  c.inputs["tensor"] = Data::tensor(bad);
  CHECK(c.run("tensor.to_image").code == "bad_input");
}

TEST_CASE("image.normalize / blur：u16 拉伸到 u8 满量程；不拉伸按 scale 截断；大核中值滤波不收 u16") {
  Image g = Image::allocate(4, 1, 1, PixelDepth::U16);
  const std::uint16_t vals[4] = {1000, 2000, 3000, 5000};
  std::memcpy(g.mutablePixels(), vals, sizeof vals);
  Call c;
  c.inputs["image"] = Data::image(g);
  REQUIRE(c.run("image.normalize").ok);
  CHECK(c.image().depth == PixelDepth::U8);
  CHECK(c.image().at(0, 0, 0) == 0.0);
  CHECK(c.image().at(3, 0, 0) == 255.0);
  REQUIRE(c.run("image.normalize", {{"stretch", Value::boolean(false)}, {"scale", Value::number(0.1)}}).ok);
  CHECK(c.image().at(1, 0, 0) == 200.0);
  CHECK(c.image().at(3, 0, 0) == 255.0);  // 500 截断到 255

  CHECK(c.run("image.blur", {{"method", Value::text("median")}, {"ksize", Value::integer(7)}}).code ==
        "bad_param");
  CHECK(c.run("image.blur", {{"ksize", Value::integer(4)}}).code == "bad_param");
  REQUIRE(c.run("image.blur").ok);
  CHECK(c.image().depth == PixelDepth::U16);
}

// ------------------------------------------------------------ 深度图 ↔ 点云（阶段 4，image-plan §4.1）

namespace {

Image depthU16(int w, int h, const std::function<std::uint16_t(int, int)>& value) {
  Image d = Image::allocate(w, h, 1, PixelDepth::U16);
  auto* p = reinterpret_cast<std::uint16_t*>(d.mutablePixels());
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) p[y * w + x] = value(x, y);
  }
  return d;
}

std::unordered_map<std::string, Value> intrinsics(double fx, double fy, double cx, double cy) {
  return {{"fx", Value::number(fx)}, {"fy", Value::number(fy)}, {"cx", Value::number(cx)}, {"cy", Value::number(cy)}};
}

}  // namespace

TEST_CASE("cloud.from_depth：针孔反投影（x 右、y 下、z 前）；0 与范围外不出点；step 隔点取；彩色图按像素上色；尺寸不对 / 内参非法报错") {
  // 4×3，(u, v) 处深度 = 1000 + 100u + 10v 毫米，(0, 0) 是 0（没测到）
  const Image d = depthU16(4, 3, [](int x, int y) { return x == 0 && y == 0 ? 0 : 1000 + 100 * x + 10 * y; });
  Call c;
  c.inputs["depth"] = Data::image(d);
  auto k = intrinsics(2.0, 4.0, 1.5, 1.0);
  REQUIRE(c.run("cloud.from_depth", k).ok);
  const PointCloud* cloud = c.outputs["cloud"].asCloud();
  REQUIRE(cloud != nullptr);
  CHECK(cloud->pointCount() == 11);
  CHECK_FALSE(cloud->hasRgb());
  // 第一个出来的点是 (u=1, v=0)：z = 1.1 m，x = (1 − 1.5)·1.1 / 2，y = (0 − 1)·1.1 / 4
  CHECK(cloud->xyz[0] == doctest::Approx((1 - 1.5) * 1.1 / 2));
  CHECK(cloud->xyz[1] == doctest::Approx((0 - 1.0) * 1.1 / 4));
  CHECK(cloud->xyz[2] == doctest::Approx(1.1));

  auto ranged = k;
  ranged["minDepth"] = Value::number(1.15);
  ranged["maxDepth"] = Value::number(1.305);
  REQUIRE(c.run("cloud.from_depth", ranged).ok);
  // 1.15 ≤ z ≤ 1.3：u=2（1.2 / 1.21 / 1.22）与 u=3（1.3 那一个）
  CHECK(c.outputs["cloud"].asCloud()->pointCount() == 4);

  auto stepped = k;
  stepped["step"] = Value::integer(2);
  REQUIRE(c.run("cloud.from_depth", stepped).ok);
  CHECK(c.outputs["cloud"].asCloud()->pointCount() == 3);  // (0,0) 无效，(2,0) (0,2) (2,2)

  // 彩色图：u8 原样；u16 取高 8 位
  Image rgb = Image::allocate(4, 3, 3, PixelDepth::U8);
  for (int i = 0; i < 12; ++i) {
    rgb.mutablePixels()[i * 3] = static_cast<std::uint8_t>(i * 10);
    rgb.mutablePixels()[i * 3 + 1] = 7;
    rgb.mutablePixels()[i * 3 + 2] = 200;
  }
  c.inputs["color"] = Data::image(rgb);
  REQUIRE(c.run("cloud.from_depth", k).ok);
  const PointCloud* colored = c.outputs["cloud"].asCloud();
  REQUIRE(colored->hasRgb());
  CHECK(std::vector<std::uint8_t>(colored->rgb.begin(), colored->rgb.begin() + 3) ==
        std::vector<std::uint8_t>{10, 7, 200});  // 第一个点是像素 1
  Image rgb16 = Image::allocate(4, 3, 3, PixelDepth::U16);
  reinterpret_cast<std::uint16_t*>(rgb16.mutablePixels())[3] = 0xAB12;  // 像素 1 的 R
  c.inputs["color"] = Data::image(rgb16);
  REQUIRE(c.run("cloud.from_depth", k).ok);
  CHECK(c.outputs["cloud"].asCloud()->rgb[0] == 0xAB);

  // f32 的 RGBA 彩色图：0..1 映射到 0..255，alpha 不用
  Image rgbaF = Image::allocate(4, 3, 4, PixelDepth::F32);
  float* fp = reinterpret_cast<float*>(rgbaF.mutablePixels());
  fp[4] = 0.5f;  // 像素 1 的 R
  fp[5] = 2.0f;  // 超出 1 截到 255
  c.inputs["color"] = Data::image(rgbaF);
  REQUIRE(c.run("cloud.from_depth", k).ok);
  const auto& rgbF = c.outputs["cloud"].asCloud()->rgb;
  CHECK(std::vector<int>{rgbF[0], rgbF[1], rgbF[2]} == std::vector<int>{128, 255, 0});

  c.inputs["color"] = Data::image(Image::allocate(5, 3, 3, PixelDepth::U8));
  CHECK(c.run("cloud.from_depth", k).code == "bad_input");
  c.inputs["color"] = Data::image(Image::allocate(4, 3, 1, PixelDepth::U8));
  CHECK(c.run("cloud.from_depth", k).code == "bad_input");  // 单通道不是彩色图
  c.inputs.erase("color");
  auto bad = k;
  bad["fx"] = Value::number(0);
  CHECK(c.run("cloud.from_depth", bad).code == "bad_param");
  auto flipped = k;
  flipped["minDepth"] = Value::number(2.0);
  flipped["maxDepth"] = Value::number(1.0);
  CHECK(c.run("cloud.from_depth", flipped).code == "bad_param");  // 修前：静默出一片空云（review 修正 PR #2）
}

TEST_CASE("深度图 → cloud.from_depth → cloud.to_depth_image 来回一趟：u16 与 f32 都逐像素相同（无效像素仍是 0）；z 缓冲取最近") {
  const int W = 64, H = 48;
  const auto k = intrinsics(80.0, 82.0, 31.5, 23.0);
  SUBCASE("u16 毫米") {
    const Image d = depthU16(W, H, [](int x, int y) {
      return (x * 7 + y * 3) % 11 == 0 ? 0 : static_cast<std::uint16_t>(500 + x * 25 + y * 11);
    });
    Call c;
    c.inputs["depth"] = Data::image(d);
    REQUIRE(c.run("cloud.from_depth", k).ok);
    c.inputs.clear();
    c.inputs["cloud"] = c.outputs["cloud"];
    auto back = k;
    back["width"] = Value::integer(W);
    back["height"] = Value::integer(H);
    REQUIRE(c.run("cloud.to_depth_image", back).ok);
    const Image& out = c.image("depth");
    REQUIRE(out.depth == PixelDepth::U16);
    CHECK(std::memcmp(out.pixels.get(), d.pixels.get(), d.byteSize()) == 0);
  }
  SUBCASE("f32 米") {
    Image d = Image::allocate(W, H, 1, PixelDepth::F32);
    auto* p = reinterpret_cast<float*>(d.mutablePixels());
    for (int i = 0; i < W * H; ++i) p[i] = i % 13 == 0 ? 0.0f : 0.4f + 0.001f * static_cast<float>(i % 500);
    Call c;
    c.inputs["depth"] = Data::image(d);
    auto m = k;
    m["depthScale"] = Value::number(1.0);
    REQUIRE(c.run("cloud.from_depth", m).ok);
    c.inputs.clear();
    c.inputs["cloud"] = c.outputs["cloud"];
    auto back = k;
    back["width"] = Value::integer(W);
    back["height"] = Value::integer(H);
    back["depth"] = Value::text("f32");
    REQUIRE(c.run("cloud.to_depth_image", back).ok);
    const Image& out = c.image("depth");
    for (int i = 0; i < W * H; ++i) {
      CHECK_MESSAGE(out.at(i % W, i / W, 0) == doctest::Approx(p[i]).epsilon(1e-6), "像素 ", i);
    }
  }
  SUBCASE("同一个像素落两个点取近的；z ≤ 0 与投影到图外的点丢掉") {
    PointCloud cloud;
    cloud.push(0.0f, 0.0f, 2.0f);   // 投到主点 (31.5→32, 23)
    cloud.push(0.0f, 0.0f, 1.5f);   // 同一像素、更近
    cloud.push(0.0f, 0.0f, -1.0f);  // 相机后面
    cloud.push(100.0f, 0.0f, 1.0f); // 图外
    Call c;
    c.inputs["cloud"] = Data::cloud(std::move(cloud));
    auto back = k;
    back["width"] = Value::integer(W);
    back["height"] = Value::integer(H);
    REQUIRE(c.run("cloud.to_depth_image", back).ok);
    const Image& out = c.image("depth");
    CHECK(out.at(32, 23, 0) == 1500.0);
    double nonzero = 0;
    for (int y = 0; y < H; ++y) {
      for (int x = 0; x < W; ++x) nonzero += out.at(x, y, 0) != 0 ? 1 : 0;
    }
    CHECK(nonzero == 1);
  }
  SUBCASE("投影超出 32 位的点丢掉，不落进第 0 列；u16 量程外的点不占像素（review 修正 PR #2）") {
    PointCloud cloud;
    cloud.push(0.0f, 0.0f, 1.2f);     // 主点，1.2 m
    cloud.push(0.0f, 0.0f, 0.0003f);  // 同一像素，0.3 mm：u16 取整成 0，修前抢下像素写成 0
    cloud.push(100.0f, 0.0f, 1e-6f);  // u = 80·100/1e-6 = 8e9：修前 MSVC 的 lround 返回 0，落进 (0, 23)
    cloud.push(0.0f, 5.0f, 80.0f);    // 80 m = 80000 mm > 65535：修前截成 65535 冒充合法深度
    Call c;
    c.inputs["cloud"] = Data::cloud(std::move(cloud));
    auto back = k;
    back["width"] = Value::integer(W);
    back["height"] = Value::integer(H);
    REQUIRE(c.run("cloud.to_depth_image", back).ok);
    const Image& u16 = c.image("depth");
    CHECK(u16.at(32, 23, 0) == 1200.0);
    CHECK(u16.at(0, 23, 0) == 0.0);
    CHECK(u16.at(32, 28, 0) == 0.0);  // 80 m 那个点投到 v = 82·5/80 + 23 ≈ 28
    back["depth"] = Value::text("f32");
    REQUIRE(c.run("cloud.to_depth_image", back).ok);
    const Image& f32 = c.image("depth");
    CHECK(f32.at(0, 23, 0) == 0.0);  // 修前这里是 1e-6 的假深度
    CHECK(f32.at(32, 23, 0) == doctest::Approx(0.0003));  // f32 没有量程问题：最近的就是它
    CHECK(f32.at(32, 28, 0) == doctest::Approx(80.0));
  }
}

TEST_CASE("推理链路：图像 → image.to_tensor（CHW）→ ml.onnx_run → tensor.to_image，形状一路对上（要 LYFLOW_TEST_ONNX_MODEL）") {
  // 模型是 [N, 6, 1280] → [N, 8, 1280] 的剖面模型（std-ml 的测试同一个前提）：拿一张 1280×6 的单通道图当 [1, 6, 1280] 喂进去。
  // 验的是链路通、形状对，不是视觉语义（image-plan §4.1）
  const char* model = std::getenv("LYFLOW_TEST_ONNX_MODEL");
  if (model == nullptr || *model == '\0') {
    MESSAGE("跳过：没设 LYFLOW_TEST_ONNX_MODEL（指向一个 [N,6,1280] → [N,8,1280] 的 onnx 模型）");
    return;
  }
  if (ensureRegistry().find("ml.onnx_run") == nullptr) {
    // 点名构建（LYFLOW_STD_PACKS=0 LYFLOW_PACKS=std-image）没有 std-ml
    MESSAGE("跳过：这次构建没编 std-ml，没有 ml.onnx_run");
    return;
  }
  lyflow::test::ensureTestOps();
  const Json doc = test::makeGraph(
      {test::N{"img", "test.make_image", Json{{"width", 1280}, {"height", 6}, {"channels", 1}, {"depth", "f32"}}},
       test::N{"tensor", "image.to_tensor", Json{{"layout", "CHW"}, {"scale", 0.001}}},
       test::N{"infer", "ml.onnx_run", Json{{"modelPath", std::string(model)}}},
       test::N{"back", "tensor.to_image", Json::object()}},
      {test::E{"img.image", "tensor.image"}, test::E{"tensor.tensor", "infer.input"},
       test::E{"infer.output", "back.tensor"}});
  test::Session s(doc);
  test::RunLog& log = s.wait();
  REQUIRE_MESSAGE(log.runStatus() == "ok", log.events.back().dump());
  Data tensor;
  REQUIRE(exec::ResultStore::instance().get(s.runId(), "infer", "output", tensor));
  CHECK(tensor.asTensor()->shape == std::vector<std::int64_t>{1, 8, 1280});
  Data image;
  REQUIRE(exec::ResultStore::instance().get(s.runId(), "back", "image", image));
  CHECK(std::vector<int>{image.asImage()->width, image.asImage()->height, image.asImage()->channels} ==
        std::vector<int>{1280, 8, 1});
}

TEST_CASE("大图预览（ADR-0028）：源头缩一级，找圆与区域统计换回原图坐标后与正式运行对得上；resize 到指定宽高后的张量形状不变") {
  test::ensureTestOps();
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / std::filesystem::u8path("lyflow-大图预览");
  std::filesystem::create_directories(dir);
  // 4100 × 2050 的灰度图，圆心 (2000, 1000)、半径 500 的亮圆。8.4 MP 超过 4 MP → 预览缩一级（scale 2）
  cv::Mat canvas(2050, 4100, CV_8UC1, cv::Scalar(20));
  cv::circle(canvas, cv::Point(2000, 1000), 500, cv::Scalar(230), cv::FILLED);
  Image disk;
  REQUIRE(cvx::fromMat(canvas, disk));
  Call save;
  save.base = dir;
  save.inputs["image"] = Data::image(disk);
  REQUIRE(save.run("io.save_image", {{"path", Value::text("圆.png")}}).ok);

  const Json doc = test::makeGraph(
      {test::N{"load", "io.load_image", Json{{"path", "圆.png"}, {"mode", "gray"}}},
       test::N{"blur", "image.blur", Json{{"ksize", 9}}},
       test::N{"circle", "image.find_circle", Json{{"minRadius", 300}, {"maxRadius", 700}, {"minDist", 400}}},
       test::N{"bin", "image.threshold", Json::object()},
       test::N{"stats", "image.region_stats", Json::object()},
       test::N{"fit", "image.resize", Json{{"mode", "size"}, {"width", 640}, {"height", 480}}},
       test::N{"tensor", "image.to_tensor", Json::object()},
       // 动态尺寸推理那一类：掩膜 → 张量 →（模型）→ 张量 → 图像，再与原图一起进区域统计
       test::N{"maskT", "image.to_tensor", Json::object()},
       test::N{"back", "tensor.to_image", Json::object()},
       test::N{"stats2", "image.region_stats", Json::object()}},
      {test::E{"load.image", "blur.image"}, test::E{"blur.image", "circle.image"},
       test::E{"blur.image", "bin.image"}, test::E{"load.image", "stats.image"},
       test::E{"bin.mask", "stats.mask"}, test::E{"load.image", "fit.image"},
       test::E{"fit.image", "tensor.image"}, test::E{"bin.mask", "maskT.image"},
       test::E{"maskT.tensor", "back.tensor"}, test::E{"load.image", "stats2.image"},
       test::E{"back.image", "stats2.mask"}});
  const auto run = [&](exec::RunMode mode) {
    exec::ResultStore::instance().clear();
    exec::RunOptions options;
    options.runId = mode == exec::RunMode::Preview ? "big-image-preview" : "big-image-full";
    options.baseDir = dir;
    options.mode = mode;
    test::RunLog log;
    log.runId = options.runId;
    exec::Run r(doc.dump(), options, &test::detail::collect, &log);
    r.join();
    return log;
  };
  // 先把事件留住：nodeEvent 返回的是临时 Json，range-for 里直接对它取下标会悬空
  const auto value = [](const test::RunLog& log, const char* node, const char* port) {
    const Json done = log.nodeEvent(node, "done");
    const auto stats = done.find("stats");
    if (stats == done.end()) return Json();
    for (const Json& o : stats->value("outputs", Json::array())) {
      if (o.value("port", "") == port) return o.value("value", Json());
    }
    return Json();
  };
  const test::RunLog full = run(exec::RunMode::Full);
  const test::RunLog preview = run(exec::RunMode::Preview);
  REQUIRE(full.runStatus() == "ok");
  REQUIRE(preview.runStatus() == "ok");
  CHECK(value(preview, "load", "image")["scale"] == 2);
  CHECK_FALSE(value(full, "load", "image").contains("scale"));

  // 正式运行找回了画上去的圆；预览在一半大的图上找，换回原图坐标后差在几个像素以内
  // （一个预览像素是 2 个原图像素，霍夫的累加分辨率也跟着粗一倍）
  const Json fc = value(full, "circle", "circle");
  const Json pc = value(preview, "circle", "circle");
  const auto near = [](const Json& a, const Json& b, double tol) {
    return std::abs(a.get<double>() - b.get<double>()) <= tol;
  };
  CHECK(std::abs(fc["center"][0].get<double>() - 2000) <= 3);
  CHECK(std::abs(fc["radius"].get<double>() - 500) <= 3);
  CHECK_MESSAGE(near(pc["center"][0], fc["center"][0], 4), pc.dump(), " vs ", fc.dump());
  CHECK_MESSAGE(near(pc["center"][1], fc["center"][1], 4), pc.dump(), " vs ", fc.dump());
  CHECK_MESSAGE(near(pc["radius"], fc["radius"], 4), pc.dump(), " vs ", fc.dump());

  // 区域统计：外接框按 s 换回、面积按 s² 换回
  const Json fb = value(full, "stats", "bbox");
  const Json pb = value(preview, "stats", "bbox");
  for (const char* corner : {"min", "max"}) {
    for (int k = 0; k < 2; ++k) {
      CHECK_MESSAGE(near(pb[corner][k], fb[corner][k], 4), pb.dump(), " vs ", fb.dump());
    }
  }
  const double fa = value(full, "stats", "area")["value"].get<double>();
  const double pa = value(preview, "stats", "area")["value"].get<double>();
  CHECK(fa == doctest::Approx(3.14159265 * 500 * 500).epsilon(0.01));
  CHECK(pa == doctest::Approx(fa).epsilon(0.02));
  CHECK(value(preview, "stats", "area")["unit"] == "px²");

  // 经过张量转回来的掩膜也记着比例（review 第二轮）：修前它被当成原图比例、又缩一遍，与原图「不一样大」
  CHECK(value(preview, "back", "image")["scale"] == 2);
  REQUIRE_MESSAGE(preview.finalState("stats2") == "done", preview.nodeEvent("stats2", "error").dump());
  CHECK(value(preview, "stats2", "area")["value"].get<double>() == doctest::Approx(fa).epsilon(0.02));

  // resize 到指定宽高是绝对尺寸：预览里照旧 640 × 480，推理的输入形状不变
  CHECK(value(preview, "tensor", "tensor")["shape"] == value(full, "tensor", "tensor")["shape"]);
  CHECK(value(full, "tensor", "tensor")["shape"] == Json{1, 1, 480, 640});

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}
