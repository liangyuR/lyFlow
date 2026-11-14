#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <limits>

#include <opencv2/imgproc.hpp>

#include "helpers.h"
#include "lyflow_cv/adapter.h"

namespace {

using namespace lyflow;

Image imageOf(const cv::Mat& matrix) {
  Image image;
  REQUIRE(cvx::fromMat(matrix, image));
  return image;
}

const nlohmann::json& record(const test::OpCall& call, const char* name) {
  const auto it = call.outputs.find(name);
  REQUIRE(it != call.outputs.end());
  REQUIRE(it->second.asRecord() != nullptr);
  return it->second.asRecord()->data;
}

Image barImage(int depth = CV_8U) {
  cv::Mat matrix(32, 64, CV_MAKETYPE(depth, 1), cv::Scalar(0));
  matrix(cv::Rect(20, 0, 20, 32)).setTo(200);
  return imageOf(matrix);
}

ParamMap roiParams() { return {{"roi", Value::vec({0, 0, 64, 32})}}; }

} // namespace

TEST_CASE("P0 image channel conversion preserves RGB order and u16/f32 depth") {
  for (const int type : {CV_8UC3, CV_16UC3, CV_32FC3}) {
    test::OpCall call;
    const Image input = imageOf(cv::Mat(3, 4, type, cv::Scalar(100, 20, 5)));
    call.inputs["image"] = Data::image(input);
    REQUIRE(call.run("image.convert_channels", {{"channels", Value::text("rgba")}}).ok);
    const Image& rgba = *call.out("image").asImage();
    CHECK(rgba.channels == 4);
    CHECK(rgba.depth == input.depth);
    CHECK(rgba.at(0, 0, 0) == 100);
    CHECK(rgba.at(0, 0, 2) == 5);
    CHECK(rgba.at(0, 0, 3) == cvx::fullScale(CV_MAT_DEPTH(type)));
    REQUIRE(call.run("image.convert_channels", {{"channels", Value::text("gray")}}).ok);
    CHECK(call.out("image").asImage()->at(0, 0, 0) == doctest::Approx(42.73).epsilon(0.02));
    CHECK(input.at(0, 0, 0) == 100);
  }
}

TEST_CASE("P0 image arithmetic saturates integers and preserves signed floating differences") {
  test::OpCall call;
  call.inputs["a"] = Data::image(imageOf(cv::Mat(2, 3, CV_8U, cv::Scalar(250))));
  call.inputs["b"] = Data::image(imageOf(cv::Mat(2, 3, CV_8U, cv::Scalar(20))));
  REQUIRE(call.run("image.arithmetic", {{"operation", Value::text("add")}}).ok);
  CHECK(call.out("image").asImage()->at(0, 0, 0) == 255);
  CHECK(call.inputs["a"].asImage()->at(0, 0, 0) == 250);
  call.inputs["a"] = Data::image(imageOf(cv::Mat(2, 3, CV_32F, cv::Scalar(0.1))));
  call.inputs["b"] = Data::image(imageOf(cv::Mat(2, 3, CV_32F, cv::Scalar(0.7))));
  REQUIRE(call.run("image.arithmetic", {{"operation", Value::text("subtract")}}).ok);
  CHECK(call.out("image").asImage()->at(0, 0, 0) == doctest::Approx(-0.6));
  call.inputs["b"] = Data::image(imageOf(cv::Mat(3, 3, CV_32F)));
  CHECK(call.run("image.arithmetic").code == "bad_input");
}

TEST_CASE("P0 exact gray statistics handle median ties, u16, nonfinite pixels and empty masks") {
  test::OpCall call;
  cv::Mat values =
      (cv::Mat_<float>(2, 4) << 1, 5, 5, 9, 1, 9, std::numeric_limits<float>::quiet_NaN(),
       std::numeric_limits<float>::infinity());
  call.inputs["image"] = Data::image(imageOf(values));
  REQUIRE(call.run("image.statistics").ok);
  const auto stats = record(call, "stats");
  CHECK(stats["sampleCount"] == 6);
  CHECK(stats["invalidSamples"] == 2);
  CHECK(stats["mean"] == 5);
  CHECK(stats["median"] == 5);
  CHECK(stats["mode"] == 1);
  call.inputs["mask"] = Data::image(imageOf(cv::Mat::zeros(2, 4, CV_8U)));
  REQUIRE(call.run("image.statistics").ok);
  CHECK(record(call, "stats")["valid"] == false);
  CHECK(record(call, "stats")["mean"].is_null());
  call.inputs.erase("mask");
  call.inputs["image"] =
      Data::image(imageOf((cv::Mat_<unsigned short>(1, 4) << 1000, 2000, 3000, 4000)));
  REQUIRE(call.run("image.statistics").ok);
  CHECK(record(call, "stats")["median"] == 2500);
  CHECK(record(call, "stats")["min"] == 1000);
  const float large = 3e38f;
  call.inputs["image"] = Data::image(imageOf((cv::Mat_<float>(1, 2) << large, large)));
  REQUIRE(call.run("image.statistics").ok);
  CHECK(record(call, "stats")["valid"] == true);
  CHECK(record(call, "stats")["median"].is_number());
  CHECK(record(call, "stats")["median"].get<double>() == static_cast<double>(large));
}

TEST_CASE("P0 color range supports HSV red wrap, gray intervals and nonfinite exclusion") {
  test::OpCall call;
  cv::Mat rgb(1, 3, CV_16UC3);
  rgb.at<cv::Vec<unsigned short, 3>>(0, 0) = {65535, 0, 0};
  rgb.at<cv::Vec<unsigned short, 3>>(0, 1) = {0, 65535, 0};
  rgb.at<cv::Vec<unsigned short, 3>>(0, 2) = {0, 0, 65535};
  call.inputs["image"] = Data::image(imageOf(rgb));
  REQUIRE(call.run("image.range_mask", {{"space", Value::text("hsv")},
                                        {"lower", Value::vec({350, 0.5, 0.5})},
                                        {"upper", Value::vec({10, 1, 1})}})
              .ok);
  CHECK(call.out("mask").asImage()->at(0, 0, 0) == 255);
  CHECK(call.out("mask").asImage()->at(1, 0, 0) == 0);
  CHECK(call.out("mask").asImage()->at(2, 0, 0) == 0);
  REQUIRE(call.run("image.range_mask", {{"space", Value::text("hsv")},
                                        {"lower", Value::vec({0, 1, 1})},
                                        {"upper", Value::vec({0, 1, 1})}})
              .ok);
  CHECK(call.out("mask").asImage()->at(0, 0, 0) == 255);
  call.inputs["image"] = Data::image(
      imageOf((cv::Mat_<float>(1, 4) << 0.1, 0.5, 0.9, std::numeric_limits<float>::quiet_NaN())));
  REQUIRE(call.run("image.range_mask", {{"space", Value::text("gray")},
                                        {"lower", Value::vec({0.3, 0, 0})},
                                        {"upper", Value::vec({0.7, 0, 0})},
                                        {"invert", Value::boolean(true)}})
              .ok);
  CHECK(call.out("mask").asImage()->at(0, 0, 0) == 255);
  CHECK(call.out("mask").asImage()->at(1, 0, 0) == 0);
  CHECK(call.out("mask").asImage()->at(3, 0, 0) == 0);
}

TEST_CASE("P0 ROI masks support clipped rectangles, circles, rotated boxes and polygons") {
  test::OpCall call;
  call.inputs["image"] = Data::image(imageOf(cv::Mat::zeros(20, 20, CV_8U)));
  REQUIRE(call.run("image.roi_mask", {{"roi", Value::vec({-10, 2, 5, 6})}}).ok);
  CHECK(cv::countNonZero(cvx::view(*call.out("mask").asImage())) == 20);
  REQUIRE(call.run("image.roi_mask", {{"shape", Value::text("circle")},
                                      {"center", Value::vec({10, 10})},
                                      {"radius", Value::number(3)}})
              .ok);
  CHECK(call.out("mask").asImage()->at(10, 10, 0) == 255);
  CHECK(call.out("mask").asImage()->at(14, 10, 0) == 0);
  REQUIRE(call.run("image.roi_mask", {{"shape", Value::text("rotated_rectangle")},
                                      {"center", Value::vec({10, 10})},
                                      {"size", Value::vec({8, 2})},
                                      {"angle", Value::number(90)}})
              .ok);
  CHECK(call.out("mask").asImage()->at(10, 13, 0) == 255);
  CHECK(call.out("mask").asImage()->at(13, 10, 0) == 0);
  REQUIRE(call.run("image.roi_mask", {{"shape", Value::text("polygon")},
                                      {"points", Value::text("[[2,2],[10,2],[2,10]]")}})
              .ok);
  CHECK(call.out("mask").asImage()->at(3, 3, 0) == 255);
  CHECK(call.out("mask").asImage()->at(15, 15, 0) == 0);
  Box2D box;
  call.inputs["box"] = Data::box2d(box);
  CHECK(call.run("image.roi_mask").code == "bad_input");
}

TEST_CASE("P0 fractional zero-width and zero-height rectangles remain empty before rasterization") {
  test::OpCall call;
  call.inputs["image"] = Data::image(imageOf(cv::Mat::zeros(8, 8, CV_8U)));
  for (const auto& roi :
       {std::vector<double>{2.5, 1, 2.5, 3}, std::vector<double>{1, 2.5, 3, 2.5}}) {
    REQUIRE(call.run("image.roi_mask", {{"roi", Value::vec(roi)}}).ok);
    CHECK(cv::countNonZero(cvx::view(*call.out("mask").asImage())) == 0);
    REQUIRE(call.run("image.roi_mask", {{"roi", Value::vec(roi)}, {"invert", Value::boolean(true)}})
                .ok);
    CHECK(cv::countNonZero(cvx::view(*call.out("mask").asImage())) == 64);
  }
}

TEST_CASE("P0 component analysis preserves holes and uses pixel area, explicit order and budgets") {
  test::OpCall call;
  cv::Mat mask = cv::Mat::zeros(30, 40, CV_8U);
  mask(cv::Rect(2, 2, 10, 10)).setTo(255);
  mask(cv::Rect(5, 5, 4, 4)).setTo(0);
  mask(cv::Rect(20, 3, 3, 4)).setTo(255);
  call.inputs["image"] = Data::image(imageOf(mask));
  REQUIRE(call.run("image.components",
                   {{"contours", Value::boolean(true)}, {"order", Value::text("area_desc")}})
              .ok);
  const auto regions = record(call, "regions");
  CHECK(regions["count"] == 2);
  CHECK(regions["regions"][0]["area"] == 84);
  CHECK(regions["regions"][1]["area"] == 12);
  CHECK(regions["regions"][0]["contours"].size() == 2);
  CHECK(call.out("mask").asImage()->at(6, 6, 0) == 0);
  CHECK(call.inputs["image"].asImage()->at(6, 6, 0) == 0);
  call.threads = 4;
  REQUIRE(call.run("image.components",
                   {{"contours", Value::boolean(true)}, {"order", Value::text("area_desc")}})
              .ok);
  CHECK(record(call, "regions") == regions);
  REQUIRE(call.run("image.components",
                   {{"filterAspect", Value::boolean(true)}, {"minAspect", Value::number(0.9)}})
              .ok);
  CHECK(record(call, "regions")["count"] == 1);
  CHECK(call.run("image.components", {{"maxRegions", Value::integer(1)}}).code == "resource_limit");
  CHECK(call.run("image.components", {{"connectivity", Value::integer(5)}}).code == "bad_param");
  call.inputs["image"] = Data::image(imageOf(cv::Mat::zeros(4, 4, CV_8U)));
  REQUIRE(call.run("image.components").ok);
  CHECK(record(call, "regions")["count"] == 0);
}

TEST_CASE(
    "P0 component coordinates and area scale once and contour limits reject rather than truncate") {
  test::OpCall call;
  cv::Mat mask = cv::Mat::zeros(10, 10, CV_8U);
  mask(cv::Rect(2, 3, 4, 5)).setTo(255);
  Image image = imageOf(mask);
  image.scale = 2;
  call.inputs["image"] = Data::image(image);
  REQUIRE(
      call.run("image.components", {{"minArea", Value::number(80)}, {"maxArea", Value::number(80)}})
          .ok);
  CHECK(record(call, "regions")["regions"][0]["area"] == 80);
  CHECK(record(call, "regions")["regions"][0]["bbox"] == nlohmann::json({4, 6, 12, 16}));
  CHECK(call.run("image.components",
                 {{"contours", Value::boolean(true)}, {"maxContourPoints", Value::integer(3)}})
            .code == "resource_limit");
}

TEST_CASE("P0 image change combines morphology and strict minimum changed area") {
  test::OpCall call;
  cv::Mat a = cv::Mat::zeros(20, 20, CV_8U), b = a.clone();
  a.at<unsigned char>(2, 2) = 200;
  a(cv::Rect(10, 10, 5, 5)).setTo(200);
  call.inputs["a"] = Data::image(imageOf(a));
  call.inputs["b"] = Data::image(imageOf(b));
  REQUIRE(call.run("image.change_stats",
                   {{"openRadius", Value::integer(1)}, {"minArea", Value::number(20)}})
              .ok);
  CHECK(record(call, "stats")["changedSamples"] == 26);
  CHECK(record(call, "stats")["filteredChangedSamples"] == 21);
  CHECK(record(call, "stats")["different"] == true);
  CHECK(call.out("mask").asImage()->at(2, 2, 0) == 0);
  REQUIRE(call.run("image.change_stats",
                   {{"openRadius", Value::integer(1)}, {"minArea", Value::number(21)}})
              .ok);
  CHECK(record(call, "stats")["different"] == false);
}

TEST_CASE(
    "P0 contour-tolerant difference rejects shared edge jitter but keeps a new interior defect") {
  test::OpCall call;
  cv::Mat source = cv::Mat::zeros(64, 64, CV_8U), target = source.clone();
  source(cv::Rect(10, 10, 40, 40)).setTo(200);
  target(cv::Rect(11, 10, 40, 40)).setTo(200);
  source(cv::Rect(27, 27, 5, 5)).setTo(0);
  call.inputs["a"] = Data::image(imageOf(source));
  call.inputs["b"] = Data::image(imageOf(target));
  REQUIRE(call.run("image.difference_mask",
                   {{"ignoreEdges", Value::boolean(true)}, {"edgeDistance", Value::number(3)}})
              .ok);
  CHECK(call.out("mask").asImage()->at(10, 20, 0) == 0);
  CHECK(call.out("mask").asImage()->at(29, 29, 0) == 255);
  REQUIRE(call.run("image.difference_mask", {{"change", Value::text("positive")}}).ok);
  CHECK(call.out("mask").asImage()->at(29, 29, 0) == 0);
  call.inputs["a"] = Data::image(imageOf(cv::Mat::zeros(32, 32, CV_8U)));
  call.inputs["b"] = call.inputs["a"];
  CHECK(call.run("image.difference_mask", {{"alignment", Value::text("translation")}}).code ==
        "alignment_failed");
}

TEST_CASE("P0 bounded translation alignment reports direction and does not mutate source pixels") {
  test::OpCall call;
  cv::Mat source = cv::Mat::zeros(64, 64, CV_8U);
  source(cv::Rect(14, 11, 17, 21)).setTo(180);
  source(cv::Rect(39, 39, 7, 9)).setTo(90);
  cv::Mat target;
  const cv::Mat affine = (cv::Mat_<double>(2, 3) << 1, 0, 3, 0, 1, -2);
  cv::warpAffine(source, target, affine, source.size());
  call.inputs["a"] = Data::image(imageOf(source));
  call.inputs["b"] = Data::image(imageOf(target));
  REQUIRE(call.run("image.difference_mask",
                   {{"alignment", Value::text("translation")}, {"threshold", Value::number(30)}})
              .ok);
  const auto shift = record(call, "stats")["shift"];
  CHECK(shift[0].get<double>() == doctest::Approx(3).epsilon(0.08));
  CHECK(shift[1].get<double>() == doctest::Approx(-2).epsilon(0.08));
  CHECK(record(call, "stats")["changedSamples"].get<int>() < 40);
  CHECK(call.inputs["a"].asImage()->at(14, 11, 0) == 180);
  CHECK(call.run("image.difference_mask",
                 {{"alignment", Value::text("translation")}, {"maxShift", Value::number(1)}})
            .code == "alignment_failed");
}

TEST_CASE("P0 subpixel alignment requires a fully finite interpolation footprint") {
  cv::RNG random(203);
  cv::Mat clean(64, 64, CV_32F);
  random.fill(clean, cv::RNG::UNIFORM, 1, 200);
  clean.at<float>(30, 30) = 0;
  cv::Mat source = clean.clone();
  source.at<float>(30, 30) = std::numeric_limits<float>::quiet_NaN();
  cv::Mat target;
  cv::Mat affine = (cv::Mat_<double>(2, 3) << 1, 0, 0.03, 0, 1, 0.03);
  cv::warpAffine(clean, target, affine, clean.size());
  target.at<float>(31, 31) += 1;
  test::OpCall call;
  call.inputs["a"] = Data::image(imageOf(source));
  call.inputs["b"] = Data::image(imageOf(target));
  REQUIRE(call.run("image.difference_mask", {{"alignment", Value::text("translation")},
                                             {"minResponse", Value::number(0)},
                                             {"threshold", Value::number(0)}})
              .ok);
  const auto stats = record(call, "stats");
  affine.at<double>(0, 2) = stats["shift"][0].get<double>();
  affine.at<double>(1, 2) = stats["shift"][1].get<double>();
  cv::Mat sourceCoverage(64, 64, CV_32F, cv::Scalar(1));
  sourceCoverage.at<float>(30, 30) = 0;
  cv::Mat expectedCoverage;
  cv::warpAffine(sourceCoverage, expectedCoverage, affine, sourceCoverage.size());
  const cv::Mat fullyFinite = expectedCoverage == 1.0f;
  CHECK(stats["validSamples"] == cv::countNonZero(fullyFinite));
  REQUIRE(fullyFinite.at<unsigned char>(31, 31) == 0);
  CHECK(call.out("mask").asImage()->at(31, 31, 0) == 0);
  CHECK(source.at<float>(30, 30) != source.at<float>(30, 30));
}

TEST_CASE("P0 line calipers recover half-pixel edges in each depth and diagnose missing edges") {
  test::OpCall call;
  for (int depth : {CV_8U, CV_16U, CV_32F}) {
    call.inputs["image"] = Data::image(barImage(depth));
    REQUIRE(call.run("image.line_caliper", roiParams()).ok);
    CHECK(call.out("line").asLine2D()->unit == Unit2D::Pixel);
    CHECK(std::abs(call.out("line").asLine2D()->dir[0]) < 1e-5);
    CHECK(call.out("position").asMeasurement()->value == doctest::Approx(19.5));
    auto params = roiParams();
    params["direction"] = Value::text("negative");
    params["selection"] = Value::text("first");
    REQUIRE(call.run("image.line_caliper", params).ok);
    CHECK(call.out("position").asMeasurement()->value == doctest::Approx(39.5));
  }
  call.inputs["image"] = Data::image(imageOf(cv::Mat::zeros(32, 64, CV_8U)));
  CHECK(call.run("image.line_caliper", roiParams()).code == "edge_not_found");
}

TEST_CASE("P0 width distinguishes outer extremal pairs from center-nearest inner pairs") {
  test::OpCall call;
  cv::Mat bars = cv::Mat::zeros(32, 64, CV_8U);
  bars(cv::Rect(5, 0, 10, 32)).setTo(200);
  bars(cv::Rect(35, 0, 10, 32)).setTo(200);
  call.inputs["image"] = Data::image(imageOf(bars));
  REQUIRE(call.run("image.width_range", roiParams()).ok);
  CHECK(call.out("minimum").asMeasurement()->value == doctest::Approx(40));
  CHECK(call.out("maximum").asMeasurement()->value == doctest::Approx(40));
  auto params = roiParams();
  params["dimension"] = Value::text("inner");
  REQUIRE(call.run("image.width_range", params).ok);
  CHECK(call.out("mean").asMeasurement()->value == doctest::Approx(20));
  call.inputs["image"] = Data::image(imageOf(cv::Mat::zeros(32, 64, CV_8U)));
  REQUIRE(call.run("image.width_range", roiParams()).ok);
  CHECK_FALSE(call.out("mean").asMeasurement()->ok);
  CHECK(std::isnan(call.out("mean").asMeasurement()->value));
}

TEST_CASE(
    "P0 projection edge positions select single, paired and multiple peaks without line fitting") {
  test::OpCall call;
  cv::Mat bars = cv::Mat::zeros(32, 64, CV_8U);
  bars(cv::Rect(5, 0, 10, 32)).setTo(200);
  bars(cv::Rect(35, 0, 10, 32)).setTo(200);
  call.inputs["image"] = Data::image(imageOf(bars));
  auto params = roiParams();
  params["mode"] = Value::text("multiple");
  REQUIRE(call.run("image.edge_positions", params).ok);
  CHECK(record(call, "positions")["count"] == 2);
  CHECK(record(call, "positions")["positions"][0]["position"] == 4.5);
  CHECK(record(call, "positions")["positions"][1]["position"] == 34.5);
  params["mode"] = Value::text("double");
  params["dimension"] = Value::text("inner");
  REQUIRE(call.run("image.edge_positions", params).ok);
  CHECK(record(call, "positions")["positions"][0]["position"] == 14.5);
  CHECK(record(call, "positions")["positions"][1]["position"] == 34.5);
}

TEST_CASE("P0 paired projection edges preserve scan-relative polarity and ordering on both axes") {
  for (const bool xAxis : {true, false})
    for (const bool inner : {true, false})
      for (const bool reverse : {true, false}) {
        CAPTURE(xAxis);
        CAPTURE(inner);
        CAPTURE(reverse);
        cv::Mat horizontal = cv::Mat::zeros(8, 32, CV_8U);
        if (inner) {
          horizontal(cv::Rect(3, 0, 7, 8)).setTo(200);
          horizontal(cv::Rect(20, 0, 7, 8)).setTo(200);
        } else {
          horizontal(cv::Rect(10, 0, 10, 8)).setTo(200);
        }
        cv::Mat input;
        if (xAxis)
          input = horizontal;
        else
          cv::transpose(horizontal, input);
        test::OpCall call;
        call.inputs["image"] = Data::image(imageOf(input));
        REQUIRE(call.run("image.edge_positions",
                         {{"roi", Value::vec({0, 0, static_cast<double>(input.cols),
                                              static_cast<double>(input.rows)})},
                          {"axis", Value::text(xAxis ? "x" : "y")},
                          {"direction", Value::text(reverse ? "negative" : "positive")},
                          {"dimension", Value::text(inner ? "inner" : "outer")},
                          {"mode", Value::text("double")},
                          {"polarity", Value::text("rising")}})
                    .ok);
        const auto positions = record(call, "positions");
        REQUIRE(positions["count"] == 2);
        CHECK(positions["positions"][0]["position"] == (reverse ? 19.5 : 9.5));
        CHECK(positions["positions"][1]["position"] == (reverse ? 9.5 : 19.5));
      }
}
