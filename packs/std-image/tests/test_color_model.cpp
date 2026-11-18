#include <doctest/doctest.h>

#include <cmath>

#include "lyflow_cv/color_model.h"

namespace color = lyflow::std_image::color;

// 防：三态混成一态 —— 暗而明显比胶暗的像素当无信息（漏判断胶）、与胶一样亮却看不清颜色的当暗背景（误判断胶）、高光当有信息；
// 以及得分按色度而不是按面积：亮胶压暗底时 25 % 面积的混合像素就过了 0.5，胶宽每边多量出 1 px 多。
TEST_CASE("图像颜色模型：像素三态 —— 高光 NaN、暗区按胶亮度 P5 分成 dark（NaN）与 dark_bg（0）；"
          "有信息的像素得分是胶的面积占比，离群颜色压低") {
  const auto feature = [](cv::Vec3d rgb, double* c1, double* c2, double* luma) {
    *c1 = std::log((rgb[0] + 1) / (rgb[1] + 1));
    *c2 = std::log((rgb[2] + 1) / (rgb[1] + 1));
    *luma = 0.299 * rgb[0] + 0.587 * rgb[1] + 0.114 * rgb[2];
  };
  const auto score = [](const color::ColorModel& model, double c1, double c2, double luma,
                        bool saturated, color::ColorState* state) {
    const double r = std::exp(c1), b = std::exp(c2);
    const double sum = r + 1.0 + b;
    return color::ColorScorer(model).score(
        color::ColorSample{c1, c2, luma, 0, r / sum, b / sum, saturated}, state);
  };
  const cv::Vec3d glueRgb(230, 120, 130), castRgb(52, 46, 43);
  color::ColorModel m;
  m.spec.noiseK = 4;
  m.spec.noiseMax = 0.1; // 亮度 < 40 是暗区
  m.spec.darkRatio = 0.5;
  double luma;
  feature(glueRgb, &m.glue.mean[0], &m.glue.mean[1], &luma);
  m.glue.cov = {0.002, 0, 0.002};
  m.glue.intensity = 480;
  m.glueLuma = {60, 120, 180}; // 暗背景：亮度 < 30
  color::Gauss2 bg;
  feature(castRgb, &bg.mean[0], &bg.mean[1], &luma);
  bg.cov = {0.004, 0, 0.004};
  bg.intensity = 141;
  m.background = {bg};
  const double g1 = m.glue.mean[0], g2 = m.glue.mean[1];
  color::ColorState st;
  CHECK(std::isnan(score(m, g1, g2, 120, true, &st)));
  CHECK(st == color::ColorState::Highlight);
  CHECK(score(m, g1, g2, 20, false, &st) == 0.0);
  CHECK(st == color::ColorState::DarkBackground);
  CHECK(std::isnan(score(m, g1, g2, 35, false, &st)));
  CHECK(st == color::ColorState::Dark);
  // 暗而不噪（噪声门限放宽到亮度 < 8）的暗褐背景：比胶的 P5 暗一半以上，照样是暗背景，色度像不像胶都不算
  color::ColorModel quiet = m;
  quiet.spec.noiseMax = 0.5;
  CHECK(score(quiet, g1, g2, 20, false, &st) == 0.0);
  CHECK(st == color::ColorState::DarkBackground);
  CHECK(score(m, g1, g2, 120, false, &st) > 0.99);
  CHECK(st == color::ColorState::Info);
  CHECK(score(m, bg.mean[0], bg.mean[1], 47, false, &st) < 0.01);
  // 胶与铸件按面积 25 % / 50 % / 75 % 混合的像素（线性 RGB 里混）：得分就是面积占比
  for (const double a : {0.25, 0.5, 0.75}) {
    CAPTURE(a);
    double c1, c2;
    feature(glueRgb * a + castRgb * (1 - a), &c1, &c2, &luma);
    CHECK(std::fabs(score(m, c1, c2, luma, false, &st) - a) <= 0.03);
  }
  // 离胶与连线都远的颜色（偏紫）不算胶
  CHECK(score(m, 0.9, 0.9, 120, false, &st) < 0.01);
}
