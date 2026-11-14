#include <opencv2/imgproc.hpp>

#include <array>
#include <algorithm>
#include <bitset>
#include <cmath>
#include <limits>

#include "MarkerIDs.h"
#include "lyflow_cv/compat.h"
#include "p3_stag.h"

namespace lyflow::ops {
namespace {
constexpr std::size_t kCodes = 766, kRotations = 4;
constexpr std::size_t kMaxCandidates = 2048, kMaxContours = 8192;
constexpr std::uint64_t kMaxDecodes = 8000000;
static_assert(sizeof(HD15) / sizeof(HD15[0]) == kCodes * kRotations);
using Quad = std::array<cv::Point2f, 4>;

Status resource() {
  return Status::Error(Phase::Execute, "resource_limit", "STag图像、轮廓、候选或字典比较超过预算");
}

std::array<cv::Point2d, 72> samplingLayout() {
  const std::array<double, 12> radius{{.088363142525988, .206935928182607, .206935928182607,
                                       .313672146827381, .327493143484516, .327493143484516,
                                       .313672146827381, .437421957035861, .437226762361658,
                                       .430628029742607, .437226762361658, .437421957035861}};
  const double halfPi = CV_PI / 2;
  const std::array<double, 12> angle{
      {CV_PI / 4, .459275804122858, halfPi - .459275804122858, .200579720495241, .591687617505840,
       halfPi - .591687617505840, halfPi - .200579720495241, .145724938287167, .433363129825345,
       CV_PI / 4, halfPi - .433363129825345, halfPi - .145724938287167}};
  std::array<cv::Point2d, 72> result;
  for (int quadrant = 0; quadrant < 4; ++quadrant) {
    for (int i = 0; i < 12; ++i) {
      const double theta = angle[i] + quadrant * halfPi;
      result[12 * quadrant + i] = {.5 + .72 * radius[i] * std::cos(theta),
                                   .5 - .72 * radius[i] * std::sin(theta)};
    }
    for (int i = 0; i < 3; ++i) {
      cv::Point2d black{.045 * (i + 1), .045 * (3 - i)}, white{.25 * (i + 1), -.045};
      for (int turn = 0; turn < quadrant; ++turn) {
        black = {1 - black.y, black.x};
        white = {1 - white.y, white.x};
      }
      result[48 + quadrant * 3 + i] = black;
      result[60 + quadrant * 3 + i] = white;
    }
  }
  return result;
}

bool similar(const Quad& a, const Quad& b) {
  double distance = 0;
  for (int i = 0; i < 4; ++i)
    distance += cv::norm(a[i] - b[i]);
  return distance < 12;
}

bool quadrilateral(const std::vector<cv::Point>& contour, const cv::Mat& gray, Quad& result) {
  const double perimeter = cv::arcLength(contour, true);
  if (perimeter < 40)
    return false;
  std::vector<cv::Point> polygon;
  cv::approxPolyDP(contour, polygon, perimeter * .025, true);
  if (polygon.size() != 4 || !cv::isContourConvex(polygon) ||
      std::abs(cv::contourArea(polygon)) < 100)
    return false;
  cv::Point2f center;
  for (const auto& p : polygon)
    center += cv::Point2f(p.x, p.y) * .25f;
  std::sort(polygon.begin(), polygon.end(), [&](const auto& a, const auto& b) {
    return std::atan2(a.y - center.y, a.x - center.x) < std::atan2(b.y - center.y, b.x - center.x);
  });
  auto start = std::min_element(polygon.begin(), polygon.end(), [](const auto& a, const auto& b) {
    return a.y != b.y ? a.y < b.y : a.x < b.x;
  });
  std::rotate(polygon.begin(), start, polygon.end());
  double shortest = std::numeric_limits<double>::infinity();
  std::vector<cv::Point2f> corners;
  for (int i = 0; i < 4; ++i) {
    if (polygon[i].x < 2 || polygon[i].y < 2 || polygon[i].x >= gray.cols - 2 ||
        polygon[i].y >= gray.rows - 2)
      return false;
    shortest = std::min(shortest, cv::norm(polygon[i] - polygon[(i + 1) % 4]));
    corners.emplace_back(static_cast<float>(polygon[i].x), static_cast<float>(polygon[i].y));
  }
  if (shortest < 8)
    return false;
  const int window = std::clamp(static_cast<int>(shortest / 12), 1, 5);
  cv::cornerSubPix(gray, corners, {window, window}, {-1, -1},
                   {cv::TermCriteria::COUNT | cv::TermCriteria::EPS, 25, .01});
  if (!cv::isContourConvex(corners) || std::abs(cv::contourArea(corners)) < 100)
    return false;
  for (int i = 0; i < 4; ++i) {
    if (!std::isfinite(corners[i].x) || !std::isfinite(corners[i].y) ||
        cv::norm(corners[i] - cv::Point2f(polygon[i])) > window * 2)
      return false;
    result[i] = corners[i];
  }
  return true;
}

bool sample(const cv::Mat& gray, const cv::Mat& h, const cv::Point2d& p, double& value) {
  const double denominator =
      h.at<double>(2, 0) * p.x + h.at<double>(2, 1) * p.y + h.at<double>(2, 2);
  if (!std::isfinite(denominator) || std::abs(denominator) < 1e-12)
    return false;
  const double x =
      (h.at<double>(0, 0) * p.x + h.at<double>(0, 1) * p.y + h.at<double>(0, 2)) / denominator;
  const double y =
      (h.at<double>(1, 0) * p.x + h.at<double>(1, 1) * p.y + h.at<double>(1, 2)) / denominator;
  if (!std::isfinite(x) || !std::isfinite(y) || x < 0 || y < 0 || x >= gray.cols - 1 ||
      y >= gray.rows - 1)
    return false;
  const int column = static_cast<int>(std::floor(x)), row = static_cast<int>(std::floor(y));
  const double fx = x - column, fy = y - row;
  value = (1 - fy) * ((1 - fx) * gray.at<unsigned char>(row, column) +
                      fx * gray.at<unsigned char>(row, column + 1)) +
          fy * ((1 - fx) * gray.at<unsigned char>(row + 1, column) +
                fx * gray.at<unsigned char>(row + 1, column + 1));
  return true;
}

bool readWord(const cv::Mat& gray, const Quad& quad, std::uint64_t& word) {
  const std::vector<cv::Point2f> square{{0, 0}, {1, 0}, {1, 1}, {0, 1}};
  const std::vector<cv::Point2f> target(quad.begin(), quad.end());
  const cv::Mat h = cv::getPerspectiveTransform(square, target);
  if (!cv::checkRange(h))
    return false;
  int boundarySupport = 0;
  for (int edge = 0; edge < 4; ++edge) {
    const double delta =
        std::min(.03, std::max(.005, 1 / cv::norm(quad[edge] - quad[(edge + 1) % 4])));
    for (int position = 1; position <= 4; ++position) {
      const double t = position * .2;
      cv::Point2d inner{t, delta}, outer{t, -delta};
      for (int turn = 0; turn < edge; ++turn) {
        inner = {1 - inner.y, inner.x};
        outer = {1 - outer.y, outer.x};
      }
      double insideValue, outsideValue;
      if (!sample(gray, h, inner, insideValue) || !sample(gray, h, outer, outsideValue))
        return false;
      boundarySupport += outsideValue - insideValue >= 30;
    }
  }
  if (boundarySupport < 14)
    return false;
  static const auto layout = samplingLayout();
  cv::Mat samples(1, 72, CV_8U);
  double blackMean = 0, whiteMean = 0;
  for (int i = 0; i < 72; ++i) {
    double value;
    if (!sample(gray, h, layout[i], value))
      return false;
    samples.at<unsigned char>(i) =
        static_cast<unsigned char>(std::clamp(std::round(value), 0.0, 255.0));
    if (i >= 60)
      whiteMean += value / 12;
    else if (i >= 48)
      blackMean += value / 12;
  }
  if (whiteMean - blackMean < 30)
    return false;
  cv::Mat binary;
  cv::threshold(samples, binary, 0, 255, cv::THRESH_BINARY_INV | cv::THRESH_OTSU);
  int black = 0, white = 0;
  for (int i = 0; i < 12; ++i) {
    black += binary.at<unsigned char>(48 + i) != 0;
    white += binary.at<unsigned char>(60 + i) == 0;
  }
  if (black < 10 || white < 10)
    return false;
  word = 0;
  for (int bit = 0; bit < 48; ++bit)
    if (binary.at<unsigned char>(bit))
      word |= std::uint64_t{1} << bit;
  return true;
}
} // namespace

Status detectSTagHD15(const cv::Mat& gray, std::vector<std::vector<cv::Point2f>>& corners,
                      std::vector<int>& ids, ExecContext& ctx) {
  if (gray.empty() || gray.type() != CV_8UC1 || std::min(gray.cols, gray.rows) < 32)
    return Status::Error(Phase::Execute, "bad_input", "STag需要短边至少32px的u8灰度图", {},
                         "image");
  if (gray.total() > 2097152)
    return resource();
  std::vector<Quad> attempted, decoded;
  std::vector<int> decodedIds;
  std::size_t contourCount = 0, contourPoints = 0;
  std::uint64_t comparisons = 0;
  for (int pass = 0; pass < 3; ++pass) {
    if (ctx.cancelled())
      return Status::Ok();
    cv::Mat binary;
    if (pass == 0)
      cv::threshold(gray, binary, 0, 255, cv::THRESH_BINARY_INV | cv::THRESH_OTSU);
    else {
      int window = std::min(pass == 1 ? 31 : 71, std::min(gray.cols, gray.rows) - 1);
      if (window % 2 == 0)
        --window;
      cv::adaptiveThreshold(gray, binary, 255, cv::ADAPTIVE_THRESH_GAUSSIAN_C,
                            cv::THRESH_BINARY_INV, window, 7);
    }
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(binary, contours, cv::RETR_LIST, cv::CHAIN_APPROX_SIMPLE);
    contourCount += contours.size();
    if (contourCount > kMaxContours)
      return resource();
    for (const auto& contour : contours) {
      if (ctx.cancelled())
        return Status::Ok();
      contourPoints += contour.size();
      if (contourPoints > 8388608)
        return resource();
      Quad quad;
      if (!quadrilateral(contour, gray, quad))
        continue;
      if (std::any_of(attempted.begin(), attempted.end(),
                      [&](const Quad& previous) { return similar(previous, quad); }))
        continue;
      if (attempted.size() >= kMaxCandidates)
        return resource();
      attempted.push_back(quad);
      std::uint64_t word;
      if (!readWord(gray, quad, word))
        continue;
      if (comparisons + kCodes * kRotations > kMaxDecodes)
        return resource();
      comparisons += kCodes * kRotations;
      int best = 49, runner = 49;
      std::size_t match = 0;
      for (std::size_t code = 0; code < kCodes * kRotations; ++code) {
        const int distance = static_cast<int>(std::bitset<48>(word ^ HD15[code]).count());
        if (distance < best) {
          runner = best;
          best = distance;
          match = code;
        } else if (distance < runner)
          runner = distance;
      }
      if (best > 7 || runner <= 7)
        continue;
      std::rotate(quad.begin(), quad.begin() + match / kCodes, quad.end());
      if (std::any_of(decoded.begin(), decoded.end(),
                      [&](const Quad& previous) { return similar(previous, quad); }))
        continue;
      if (decoded.size() >= 256)
        return resource();
      decoded.push_back(quad);
      decodedIds.push_back(static_cast<int>(match % kCodes));
    }
  }
  if (ctx.cancelled())
    return Status::Ok();
  std::vector<std::vector<cv::Point2f>> result;
  for (const auto& quad : decoded)
    result.emplace_back(quad.begin(), quad.end());
  corners = std::move(result);
  ids = std::move(decodedIds);
  return Status::Ok();
}
} // namespace lyflow::ops
