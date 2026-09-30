// std-image 包的算子测试（docs/image-plan.md 阶段 2）：adapter 的零拷贝与 RGB 顺序、读写往返（含中文路径）、
// 每个算子的数值、像素单位的几何、单通道契约。图像一律现造（合成渐变 / cv::circle 画的圆），仓库不进图片。
#include <doctest/doctest.h>

#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <unordered_map>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include "helpers.h"
#include "image_test_op.h"
#include "lyflow/operator.h"
#include "lyflow/registry.h"
#include "lyflow_cv/adapter.h"
#include "test_ops.h"

namespace {

using namespace lyflow;
using lyflow::test::Json;

class NullContext final : public ExecContext {
 public:
  explicit NullContext(std::filesystem::path base = {}) : baseDir_(std::move(base)) {}
  bool cancelled() const override { return false; }
  void progress(float, std::string_view) override {}
  void log(LogLevel, std::string) override {}
  const std::filesystem::path& baseDir() const override { return baseDir_; }
  int threadBudget() const override { return 1; }

 private:
  std::filesystem::path baseDir_;
};

/// 直接调一个算子的 compute（不经执行器）。参数先填默认值再覆盖。
struct Call {
  std::unordered_map<std::string, Data> inputs;
  std::unordered_map<std::string, Data> outputs;
  ParamMap params;
  std::filesystem::path base;

  Status run(const std::string& opId, const std::unordered_map<std::string, Value>& overrides = {}) {
    const OperatorDesc* op = ensureRegistry().find(opId);
    REQUIRE_MESSAGE(op != nullptr, opId);
    params.clear();
    outputs.clear();
    for (const Param& p : op->params) params[p.name] = p.def;
    for (const auto& [k, v] : overrides) params[k] = v;
    NullContext ctx(base);
    ParamView view(params, base);
    Inputs in(inputs);
    Outputs out(outputs);
    return op->compute(in, view, out, ctx);
  }
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
