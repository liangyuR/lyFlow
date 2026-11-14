#include <doctest/doctest.h>
#include <cmath>
#include <limits>
#include <opencv2/core.hpp>

#include "helpers.h"
#include "lyflow_cv/adapter.h"

namespace lyflow::ops {
void registerP2HeightExtraction(Registry&);
}
namespace {
using namespace lyflow;
Registry depthRegistry() {
  Registry r;
  ops::registerP2HeightExtraction(r);
  return r;
}
Data depthImage(const cv::Mat& matrix, int scale = 1) {
  Image image;
  REQUIRE(cvx::fromMat(matrix.clone(), image));
  image.scale = scale;
  return Data::image(std::move(image));
}
cv::Mat mat(test::OpCall& call, const char* port) { return cvx::view(*call.out(port).asImage()); }
}

TEST_CASE("P2 depth height planes preserve signed meters and genuine observation validity") {
  auto registry = depthRegistry();
  test::OpCall call(registry);
  cv::Mat depth(5, 6, CV_32F, cv::Scalar(1));
  depth.at<float>(2, 2) = 1.02f;
  depth.at<float>(2, 3) = .98f;
  depth.at<float>(0, 0) = 0;
  depth.at<float>(0, 1) = std::numeric_limits<float>::quiet_NaN();
  call.inputs["image"] = depthImage(depth);
  REQUIRE(call.run("image.height_extract", {{"gain", Value::number(2)}}).ok);
  CHECK(std::abs(mat(call, "height").at<float>(2, 2) - .02) < 1e-6);
  CHECK(std::abs(mat(call, "height").at<float>(2, 3) + .02) < 1e-6);
  CHECK(mat(call, "mask").at<unsigned char>(0, 0) == 0);
  CHECK(mat(call, "mask").at<unsigned char>(0, 1) == 0);
  CHECK(mat(call, "image").at<unsigned char>(1, 1) == 128);
  CHECK(mat(call, "image").at<unsigned char>(2, 2) == 168);
  CHECK(mat(call, "image").at<unsigned char>(2, 3) == 88);
  REQUIRE(call.run("image.height_extract",
                   {{"invalid", Value::text("custom")}, {"customDepth", Value::number(1.1)}})
              .ok);
  CHECK(std::abs(mat(call, "height").at<float>(0, 0) - .1) < 1e-6);
  CHECK(mat(call, "mask").at<unsigned char>(0, 0) == 0);
  REQUIRE(call.run("image.height_extract", {{"invalid", Value::text("farthest")}}).ok);
  CHECK(std::abs(mat(call, "height").at<float>(0, 0) - .02) < 1e-6);
  CHECK(mat(call, "mask").at<unsigned char>(0, 0) == 0);
  REQUIRE(call.run("image.height_extract",
                   {{"direction", Value::text("high")}, {"gain", Value::number(2)}})
              .ok);
  CHECK(mat(call, "image").at<unsigned char>(2, 2) == 40);
  CHECK(mat(call, "image").at<unsigned char>(2, 3) == 0);
  REQUIRE(call.run("image.height_extract",
                   {{"direction", Value::text("low")}, {"gain", Value::number(2)}})
              .ok);
  CHECK(mat(call, "image").at<unsigned char>(2, 3) == 40);
  CHECK(mat(call, "image").at<unsigned char>(2, 2) == 0);
  CHECK(call.inputs["image"].asImage()->depth == PixelDepth::F32);
  CHECK(cvx::view(*call.inputs["image"].asImage()).at<float>(2, 2) == depth.at<float>(2, 2));
}

TEST_CASE("P2 three-point orthographic plane uses calibrated coordinates and rejects degeneracy") {
  auto registry = depthRegistry();
  test::OpCall call(registry);
  cv::Mat depth(9, 9, CV_32F);
  for (int y = 0; y < 9; ++y)
    for (int x = 0; x < 9; ++x)
      depth.at<float>(y, x) = 1 + .01f * x + .02f * y;
  depth.at<float>(4, 4) += .03f;
  call.inputs["image"] = depthImage(depth);
  const ParamMap params{
      {"reference", Value::text("three_points")}, {"pixelSizeX", Value::number(.01)},
      {"pixelSizeY", Value::number(.02)},         {"point0", Value::vec({0, 0, 0})},
      {"point1", Value::vec({8, 0, 0})},          {"point2", Value::vec({0, 8, 0})}};
  REQUIRE(call.run("image.height_extract", params).ok);
  CHECK(std::abs(mat(call, "height").at<float>(4, 4) - .03 / std::sqrt(3)) < 1e-6);
  CHECK(std::abs(mat(call, "reference").at<float>(4, 4) - 1.12) < 1e-6);
  auto bad = params;
  bad["point2"] = Value::vec({4, 0, 0});
  CHECK_FALSE(call.run("image.height_extract", bad).ok);
  bad = params;
  bad["point2"] = Value::vec({99, 0, 0});
  CHECK_FALSE(call.run("image.height_extract", bad).ok);
  call.inputs["image"] = depthImage(depth, 2);
  CHECK_FALSE(call.run("image.height_extract", params).ok);
  CHECK(call.outputs.empty());
}

TEST_CASE(
    "P2 local polynomial surface reconstructs independent quadratic truth including boundaries") {
  auto registry = depthRegistry();
  test::OpCall call(registry);
  cv::Mat depth(13, 13, CV_32F);
  for (int y = 0; y < depth.rows; ++y)
    for (int x = 0; x < depth.cols; ++x) {
      double u = (x - 6) * .01, v = (y - 6) * .01;
      depth.at<float>(y, x) = static_cast<float>(1 + .2 * u - .3 * v + 2 * u * u + u * v + v * v);
    }
  depth.at<float>(6, 6) = 0;
  call.inputs["image"] = depthImage(depth);
  const ParamMap params{{"reference", Value::text("surface")},
                        {"pixelSizeX", Value::number(.01)},
                        {"pixelSizeY", Value::number(.01)},
                        {"radius", Value::number(.05)},
                        {"degree", Value::integer(2)}};
  REQUIRE(call.run("image.height_extract", params).ok);
  for (const auto xy : {cv::Point(0, 0), cv::Point(12, 12), cv::Point(4, 8)}) {
    CHECK(mat(call, "mask").at<unsigned char>(xy) == 255);
    CHECK(std::abs(mat(call, "height").at<float>(xy)) < 1e-6);
    CHECK(std::abs(mat(call, "reference").at<float>(xy) - depth.at<float>(xy)) < 1e-6);
  }
  CHECK(mat(call, "mask").at<unsigned char>(6, 6) == 0);
  CHECK(std::abs(mat(call, "reference").at<float>(6, 6) - 1) < 1e-6);
  auto bad = params;
  bad["radius"] = Value::number(100);
  CHECK_FALSE(call.run("image.height_extract", bad).ok);
  CHECK(call.outputs.empty());
}

TEST_CASE("P2 metric depth supports u16 scale and rejects unsupported images") {
  auto registry = depthRegistry();
  test::OpCall call(registry);
  cv::Mat depth(4, 4, CV_16U, cv::Scalar(1200));
  call.inputs["image"] = depthImage(depth);
  REQUIRE(call.run("image.height_extract", {{"depthScale", Value::number(.001)}}).ok);
  CHECK(std::abs(mat(call, "height").at<float>(2, 2) - .2) < 1e-6);
  call.inputs["image"] = depthImage(cv::Mat(4, 4, CV_8U, cv::Scalar(100)));
  CHECK_FALSE(call.run("image.height_extract").ok);
  call.inputs["image"] = depthImage(cv::Mat(4, 4, CV_32FC3, cv::Scalar(1, 1, 1)));
  CHECK_FALSE(call.run("image.height_extract").ok);
}
