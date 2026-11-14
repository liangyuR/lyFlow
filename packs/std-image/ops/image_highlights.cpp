#include <deque>
#include <numeric>
#include <tuple>

#include "lyflow/pose.h"
#include "p1_common.h"

namespace lyflow::ops {
namespace {

using Json = nlohmann::json;
constexpr std::size_t kMaxPixels = 8 * 1024 * 1024;
constexpr std::size_t kMaxPoints = 262144;
constexpr std::size_t kMaxCandidates = 4096;
constexpr std::size_t kMaxChecks = 64 * 1024 * 1024;
constexpr std::size_t kMaxResults = 256;
constexpr double kPi = poses::kPi;

struct Failure {
  Status status;
};

void cancelled(ExecContext& ctx) {
  if (ctx.cancelled())
    throw Failure{Status::Error(Phase::Execute, "cancelled", "高光检测已取消")};
}

[[noreturn]] void resource(const char* message) {
  throw Failure{Status::Error(Phase::Execute, "resource_limit", message)};
}

struct Budget {
  std::size_t checks = 0;
  ExecContext& ctx;
  void add(std::size_t count) {
    if (count > kMaxChecks - checks)
      resource("高光候选/支持点检查超过64Mi预算");
    checks += count;
    cancelled(ctx);
  }
};

struct Settings {
  double brightness = 0, contrast = 0, sigma = 0, length = 0, gap = 0, residual = 0;
  double boxHeight = 0, angle = 0, tolerance = 0, coverage = 0, mergeDistance = 0;
  double mergeAngle = 0;
  int radius = 0, support = 0;
  std::string frame;
};

Settings settings(const ParamView& p, int scale) {
  Settings s;
  s.frame = p.text("frame");
  if (s.frame.empty() || s.frame.size() > values::kMaxStringBytes)
    throw Failure{img::badParam("参考帧不能为空或超过64KiB", "frame")};
  auto scalar = [&](const char* name, double low, double high, bool pixels = false) {
    const double value = p.number(name);
    const double source = pixels ? value * scale : value;
    if (!std::isfinite(value) || !std::isfinite(source) || source < low || source > high)
      throw Failure{img::badParam("参数超出声明的原图范围", name)};
    return value;
  };
  s.brightness = scalar("minBrightness", 0, 1);
  s.contrast = scalar("minContrast", 0, 1);
  if (s.contrast == 0)
    throw Failure{img::badParam("最小双侧对比度必须大于零", "minContrast")};
  const double radius = scalar("ridgeRadius", 1, 64, true);
  s.radius = std::max(1, static_cast<int>(std::lround(radius)));
  s.sigma = scalar("smoothSigma", 0, 16, true);
  s.length = scalar("minLength", 2, 1000000, true);
  s.gap = scalar("maxGap", 0, 1000000, true);
  s.residual = scalar("maxResidual", 0.1, 64, true);
  s.boxHeight = scalar("boxHeight", 0.25, 1000000, true);
  s.angle = scalar("referenceAngle", -360, 360) * kPi / 180;
  s.tolerance = scalar("angleTolerance", 0, 90) * kPi / 180;
  s.coverage = scalar("minCoverage", 0, 1);
  s.mergeDistance = scalar("mergeDistance", 0, 64, true);
  s.mergeAngle = scalar("mergeAngle", 0, 90) * kPi / 180;
  const auto support = p.integer("minSupport");
  if (support < 2 || support > 4096)
    throw Failure{img::badParam("最少支持点必须为2..4096", "minSupport")};
  s.support = static_cast<int>(support);
  return s;
}

cv::Mat regionMask(const Inputs& in, const Image& image, const Settings& s, ExecContext& ctx) {
  if (in.has("roi") && in.has("region"))
    throw Failure{img::badInput("roi和region不可同时提供", "region")};
  cv::Mat mask(image.height, image.width, CV_8U, cv::Scalar(255));
  const Box2D* box = nullptr;
  const Record* region = nullptr;
  double cx = 0, cy = 0, width = 0, height = 0, cosine = 1, sine = 0;
  if (in.has("roi")) {
    box = in.get("roi").asBox2D();
    if (!box || box->unit != Unit2D::Pixel || !std::isfinite(box->min[0]) ||
        !std::isfinite(box->min[1]) || !std::isfinite(box->max[0]) || !std::isfinite(box->max[1]) ||
        box->max[0] <= box->min[0] || box->max[1] <= box->min[1])
      throw Failure{img::badInput("ROI必须为有限、正面积的像素Box2D", "roi")};
  }
  if (in.has("region")) {
    region = in.get("region").asRecord();
    std::string why;
    if (!region || region->type != poses::kPose2DType || !poses::validate(*region, why))
      throw Failure{img::badInput("旋转ROI需要有效vision.Pose2D.v1：" + why, "region")};
    const auto& data = region->data;
    if (data["unit"] != "px" || data["frame"] != s.frame)
      throw Failure{img::badInput("旋转ROI的像素单位与参考帧必须匹配", "region")};
    cx = data["position"][0].get<double>() / image.scale;
    cy = data["position"][1].get<double>() / image.scale;
    width = data["size"][0].get<double>() / image.scale;
    height = data["size"][1].get<double>() / image.scale;
    if (width <= 0 || height <= 0)
      throw Failure{img::badInput("旋转ROI尺寸无法在工作图像中表示", "region")};
    cosine = std::cos(data["angle"].get<double>());
    sine = std::sin(data["angle"].get<double>());
  }
  if (!box && !region)
    return mask;
  std::size_t count = 0;
  const double normalizer = std::max(width, height);
  for (int y = 0; y < mask.rows; ++y) {
    cancelled(ctx);
    auto* row = mask.ptr<std::uint8_t>(y);
    for (int x = 0; x < mask.cols; ++x) {
      bool inside = false;
      if (box)
        inside = x >= box->min[0] && x <= box->max[0] && y >= box->min[1] && y <= box->max[1];
      else {
        const double dx = (x - cx) / normalizer, dy = (y - cy) / normalizer;
        inside = std::abs(cosine * dx + sine * dy) <= width / normalizer / 2 &&
                 std::abs(-sine * dx + cosine * dy) <= height / normalizer / 2;
      }
      row[x] = inside ? 255 : 0;
      count += inside ? 1 : 0;
    }
  }
  if (!count)
    throw Failure{img::badInput("ROI与图像没有有效像素交集", box ? "roi" : "region")};
  return mask;
}

cv::Mat normalizedGray(const Image& image, const cv::Mat& mask, double sigma) {
  cv::Mat gray;
  const double divisor = image.depth == PixelDepth::U8    ? 255
                         : image.depth == PixelDepth::U16 ? 65535
                                                          : 1;
  image_p1::gray(image).convertTo(gray, CV_32F, 1 / divisor);
  if (sigma == 0)
    return gray;
  cv::Mat weight, weighted;
  mask.convertTo(weight, CV_32F, 1.0 / 255);
  cv::multiply(gray, weight, weighted);
  cv::GaussianBlur(weight, weight, {}, sigma, sigma, cv::BORDER_CONSTANT);
  cv::GaussianBlur(weighted, weighted, {}, sigma, sigma, cv::BORDER_CONSTANT);
  cv::max(weight, 1e-20, weight);
  cv::divide(weighted, weight, gray);
  return gray;
}

struct Ridge {
  cv::Point2d point;
  double contrast = 0;
};

void extrema(const std::vector<float>& line, int width, bool maximum, std::vector<float>& output) {
  std::deque<std::size_t> queue;
  output.resize(line.size() - static_cast<std::size_t>(width) + 1);
  for (std::size_t i = 0; i < line.size(); ++i) {
    while (!queue.empty() &&
           (maximum ? line[queue.back()] <= line[i] : line[queue.back()] >= line[i]))
      queue.pop_back();
    queue.push_back(i);
    if (i >= static_cast<std::size_t>(width) && queue.front() == i - width)
      queue.pop_front();
    if (i + 1 >= static_cast<std::size_t>(width))
      output[i + 1 - width] = line[queue.front()];
  }
}

std::vector<Ridge> ridges(const cv::Mat& gray, const cv::Mat& valid, const Settings& s,
                          cv::Mat& mask, ExecContext& ctx) {
  std::vector<Ridge> result;
  std::vector<float> line, minimum, maximum;
  for (int axis = 0; axis < 2; ++axis) {
    const int scans = axis == 0 ? gray.cols : gray.rows;
    const int span = axis == 0 ? gray.rows : gray.cols;
    auto value = [&](int scan, int position) {
      return axis == 0 ? gray.at<float>(position, scan) : gray.at<float>(scan, position);
    };
    auto allowed = [&](int scan, int position) {
      return axis == 0 ? valid.at<std::uint8_t>(position, scan) != 0
                       : valid.at<std::uint8_t>(scan, position) != 0;
    };
    for (int scan = 0; scan < scans; ++scan) {
      cancelled(ctx);
      int start = 0;
      while (start < span) {
        while (start < span && !allowed(scan, start))
          ++start;
        int end = start;
        while (end < span && allowed(scan, end))
          ++end;
        const int length = end - start;
        if (length >= 2 * s.radius + 1) {
          line.resize(static_cast<std::size_t>(length));
          for (int i = 0; i < length; ++i)
            line[static_cast<std::size_t>(i)] = value(scan, start + i);
          extrema(line, s.radius, false, minimum);
          extrema(line, 2 * s.radius + 1, true, maximum);
          auto contrast = [&](int i) {
            return double(line[static_cast<std::size_t>(i)]) -
                   std::max(minimum[static_cast<std::size_t>(i - s.radius)],
                            minimum[static_cast<std::size_t>(i + 1)]);
          };
          auto peak = [&](int i) {
            return line[static_cast<std::size_t>(i)] >= s.brightness &&
                   line[static_cast<std::size_t>(i)] >=
                       maximum[static_cast<std::size_t>(i - s.radius)] - 1e-6 &&
                   contrast(i) >= s.contrast && contrast(i) > 1e-6;
          };
          for (int i = s.radius; i < length - s.radius; ++i) {
            if (!peak(i))
              continue;
            int last = i;
            while (last + 1 < length - s.radius && peak(last + 1) &&
                   std::abs(line[static_cast<std::size_t>(last + 1)] -
                            line[static_cast<std::size_t>(i)]) <= 1e-6)
              ++last;
            double position = (i + last) / 2.0;
            if (last == i) {
              const double before = line[static_cast<std::size_t>(i - 1)];
              const double middle = line[static_cast<std::size_t>(i)];
              const double after = line[static_cast<std::size_t>(i + 1)];
              const double denominator = before - 2 * middle + after;
              if (denominator < -1e-12)
                position += std::clamp(0.5 * (before - after) / denominator, -0.5, 0.5);
            }
            const int pixel = static_cast<int>(std::lround(start + position));
            const int x = axis == 0 ? scan : pixel, y = axis == 0 ? pixel : scan;
            if (!mask.at<std::uint8_t>(y, x)) {
              if (result.size() == kMaxPoints)
                resource("候选亮脊点超过262144个");
              result.push_back({axis == 0 ? cv::Point2d(scan, start + position)
                                          : cv::Point2d(start + position, scan),
                                (contrast(i) + contrast(last)) / 2});
              mask.at<std::uint8_t>(y, x) = 255;
            }
            i = last;
          }
        }
        start = end + 1;
      }
    }
  }
  std::sort(result.begin(), result.end(), [](const Ridge& a, const Ridge& b) {
    return std::tie(a.point.x, a.point.y, a.contrast) < std::tie(b.point.x, b.point.y, b.contrast);
  });
  return result;
}

double cross(const cv::Point2d& a, const cv::Point2d& b) { return a.x * b.y - a.y * b.x; }
double dot(const cv::Point2d& a, const cv::Point2d& b) { return a.x * b.x + a.y * b.y; }
double angleDistance(double a, double b) { return std::abs(std::remainder(a - b, kPi)); }

struct Segment {
  cv::Point2d center, direction, first, last;
  double length = 0, rms = 0, maxError = 0, coverage = 0, contrast = 0, score = 0, angle = 0;
  std::vector<std::size_t> members;
};

bool fit(const std::vector<Ridge>& points, const std::vector<std::size_t>& members, Segment& result,
         Budget& budget) {
  budget.add(members.size());
  if (members.size() < 2)
    return false;
  cv::Point2d center;
  for (const auto i : members)
    center += points[i].point;
  center *= 1.0 / members.size();
  double xx = 0, xy = 0, yy = 0;
  for (const auto i : members) {
    const auto delta = points[i].point - center;
    xx += delta.x * delta.x;
    xy += delta.x * delta.y;
    yy += delta.y * delta.y;
  }
  if (xx + yy < 1e-12)
    return false;
  const double theta = 0.5 * std::atan2(2 * xy, xx - yy);
  cv::Point2d direction(std::cos(theta), std::sin(theta));
  if (direction.x < 0 || (std::abs(direction.x) < 1e-12 && direction.y < 0))
    direction *= -1;
  double lo = std::numeric_limits<double>::infinity(), hi = -lo, squared = 0, maximum = 0,
         contrast = 0;
  std::vector<double> positions;
  positions.reserve(members.size());
  for (const auto i : members) {
    const auto delta = points[i].point - center;
    const double t = dot(delta, direction), distance = cross(delta, direction);
    lo = std::min(lo, t);
    hi = std::max(hi, t);
    squared += distance * distance;
    maximum = std::max(maximum, std::abs(distance));
    contrast += points[i].contrast;
    positions.push_back(t);
  }
  const double length = hi - lo;
  if (length <= 0)
    return false;
  std::sort(positions.begin(), positions.end());
  const double halfPixel = (std::abs(direction.x) + std::abs(direction.y)) / 2;
  double covered = 0, begin = lo, end = lo;
  for (double position : positions) {
    const double a = std::max(lo, position - halfPixel), b = std::min(hi, position + halfPixel);
    if (a > end) {
      covered += end - begin;
      begin = a;
    }
    end = std::max(end, b);
  }
  covered += end - begin;
  result.center = center + direction * ((lo + hi) / 2);
  result.direction = direction;
  result.first = center + direction * lo;
  result.last = center + direction * hi;
  result.length = length;
  result.rms = std::sqrt(squared / members.size());
  result.maxError = maximum;
  result.coverage = std::clamp(covered / length, 0.0, 1.0);
  result.contrast = contrast / members.size();
  result.angle = std::atan2(direction.y, direction.x);
  result.members = members;
  std::sort(result.members.begin(), result.members.end());
  return true;
}

std::vector<std::vector<std::size_t>> split(const std::vector<Ridge>& points,
                                            std::vector<std::size_t> members,
                                            const cv::Point2d& origin, const cv::Point2d& direction,
                                            double maxGap) {
  std::sort(members.begin(), members.end(), [&](std::size_t a, std::size_t b) {
    const double at = dot(points[a].point - origin, direction),
                 bt = dot(points[b].point - origin, direction);
    return at == bt ? a < b : at < bt;
  });
  const double pixel = std::abs(direction.x) + std::abs(direction.y);
  std::vector<std::vector<std::size_t>> groups;
  double previous = 0;
  for (const auto index : members) {
    const double position = dot(points[index].point - origin, direction);
    if (groups.empty() || position - previous > maxGap + pixel + 1e-8)
      groups.emplace_back();
    groups.back().push_back(index);
    previous = position;
  }
  return groups;
}

std::vector<std::vector<std::size_t>> normalBands(const std::vector<Ridge>& points,
                                                  const std::vector<std::size_t>& members,
                                                  const cv::Point2d& origin,
                                                  const cv::Point2d& direction, Budget& budget) {
  budget.add(members.size());
  std::vector<std::pair<double, std::size_t>> offsets;
  offsets.reserve(members.size());
  for (const auto index : members)
    offsets.emplace_back(cross(points[index].point - origin, direction), index);
  std::sort(offsets.begin(), offsets.end());
  std::vector<std::vector<std::size_t>> groups;
  const double separation = 0.5 * (std::abs(direction.x) + std::abs(direction.y));
  double previous = 0;
  for (const auto& item : offsets) {
    if (groups.empty() || item.first - previous > separation)
      groups.emplace_back();
    groups.back().push_back(item.second);
    previous = item.first;
  }
  return groups;
}

cv::Point2d robustDirection(const std::vector<Ridge>& points,
                            const std::vector<std::size_t>& members, const cv::Point2d& origin,
                            const cv::Point2d& direction, double seedLength, Budget& budget) {
  if (members.size() < 3)
    return direction;
  const double initial = std::atan2(direction.y, direction.x);
  const double range = std::atan2(2.0, seedLength);
  std::vector<std::pair<double, std::size_t>> offsets;
  offsets.reserve(members.size());
  std::vector<std::size_t> best;
  double bestSquared = std::numeric_limits<double>::infinity();
  for (int sample = -16; sample <= 16; ++sample) {
    budget.add(members.size());
    const double theta = initial + range * sample / 16;
    const cv::Point2d trial(std::cos(theta), std::sin(theta));
    offsets.clear();
    for (const auto i : members)
      offsets.emplace_back(cross(points[i].point - origin, trial), i);
    std::sort(offsets.begin(), offsets.end());
    const double width = 0.8 * (std::abs(trial.x) + std::abs(trial.y));
    std::size_t begin = 0, sampleBegin = 0, sampleEnd = 0, sampleCount = 0;
    double sampleVariance = std::numeric_limits<double>::infinity();
    double sum = 0, squared = 0;
    for (std::size_t end = 0; end < offsets.size(); ++end) {
      sum += offsets[end].first;
      squared += offsets[end].first * offsets[end].first;
      while (offsets[end].first - offsets[begin].first > width) {
        sum -= offsets[begin].first;
        squared -= offsets[begin].first * offsets[begin].first;
        ++begin;
      }
      const auto count = end - begin + 1;
      const double variance = std::max(0.0, squared - sum * sum / count);
      if (count > sampleCount || (count == sampleCount && variance < sampleVariance)) {
        sampleBegin = begin;
        sampleEnd = end;
        sampleCount = count;
        sampleVariance = variance;
      }
    }
    if (sampleCount > best.size() || (sampleCount == best.size() && sampleVariance < bestSquared)) {
      best.clear();
      for (auto j = sampleBegin; j <= sampleEnd; ++j)
        best.push_back(offsets[j].second);
      bestSquared = sampleVariance;
    }
  }

  Segment fitted;
  if (fit(points, best, fitted, budget) && angleDistance(fitted.angle, initial) <= range + 1e-8)
    return fitted.direction;
  return direction;
}

std::vector<Segment> segments(const std::vector<Ridge>& points, const cv::Mat& mask,
                              const Settings& s, Budget& budget) {
  if (points.size() < static_cast<std::size_t>(s.support))
    return {};
  cancelled(budget.ctx);
  std::vector<cv::Vec4i> raw;
  cv::HoughLinesP(mask, raw, 1, kPi / 360, s.support, std::max(2.0, s.length / 2), s.gap);
  if (raw.size() > kMaxCandidates)
    resource("Hough高光候选超过4096条");
  if (points.size() <= 64) {
    for (std::size_t i = 0; i < points.size(); ++i)
      for (std::size_t j = i + 1; j < points.size(); ++j) {
        if (cv::norm(points[j].point - points[i].point) < std::max(1.0, s.length / 2))
          continue;
        if (raw.size() == kMaxCandidates)
          resource("短线补充候选超过4096条");
        raw.emplace_back(cvRound(points[i].point.x), cvRound(points[i].point.y),
                         cvRound(points[j].point.x), cvRound(points[j].point.y));
      }
  }
  for (auto& line : raw)
    if (std::tie(line[2], line[3]) < std::tie(line[0], line[1])) {
      std::swap(line[0], line[2]);
      std::swap(line[1], line[3]);
    }
  std::sort(raw.begin(), raw.end(), [](const cv::Vec4i& a, const cv::Vec4i& b) {
    return std::lexicographical_compare(a.val, a.val + 4, b.val, b.val + 4);
  });
  std::vector<Segment> candidates;
  std::size_t storedIndices = 0;
  for (const auto& line : raw) {
    budget.add(points.size());
    const cv::Point2d origin(line[0], line[1]);
    cv::Point2d direction(line[2] - line[0], line[3] - line[1]);
    const double norm = std::hypot(direction.x, direction.y);
    if (norm == 0)
      continue;
    direction *= 1 / norm;
    std::vector<std::size_t> selected;
    for (std::size_t i = 0; i < points.size(); ++i)
      if (std::abs(cross(points[i].point - origin, direction)) <= s.residual + 1)
        selected.push_back(i);
    direction = robustDirection(points, selected, origin, direction, norm, budget);
    selected.clear();
    budget.add(points.size());
    for (std::size_t i = 0; i < points.size(); ++i)
      if (std::abs(cross(points[i].point - origin, direction)) <= s.residual + 2)
        selected.push_back(i);
    for (auto band : normalBands(points, selected, origin, direction, budget)) {
      for (auto group : split(points, std::move(band), origin, direction, s.gap)) {
        if (group.size() < static_cast<std::size_t>(s.support))
          continue;
        Segment fitted;
        if (!fit(points, group, fitted, budget))
          continue;
        std::vector<std::size_t> retained;
        budget.add(group.size());
        for (const auto i : group)
          if (std::abs(cross(points[i].point - fitted.center, fitted.direction)) <= s.residual)
            retained.push_back(i);
        for (const auto& finalGroup :
             split(points, std::move(retained), fitted.center, fitted.direction, s.gap)) {
          if (finalGroup.size() < static_cast<std::size_t>(s.support) ||
              !fit(points, finalGroup, fitted, budget))
            continue;
          if (fitted.length < s.length || fitted.rms > s.residual || fitted.maxError > s.residual ||
              fitted.coverage < s.coverage ||
              angleDistance(fitted.angle, s.angle) > s.tolerance + 1e-12)
            continue;
          fitted.score = fitted.coverage * std::clamp(fitted.contrast, 0.0, 1.0) /
                         (1 + fitted.rms / s.residual);
          if (candidates.size() == kMaxCandidates)
            resource("拟合分组超过4096条预算");
          if (fitted.members.size() > 4 * 1024 * 1024 - storedIndices)
            resource("高光拟合支持索引超过32MiB预算");
          storedIndices += fitted.members.size();
          candidates.push_back(std::move(fitted));
        }
      }
    }
  }
  std::sort(candidates.begin(), candidates.end(), [](const Segment& a, const Segment& b) {
    if (a.length != b.length)
      return a.length > b.length;
    return std::tie(a.rms, a.first.x, a.first.y, a.last.x, a.last.y, a.members) <
           std::tie(b.rms, b.first.x, b.first.y, b.last.x, b.last.y, b.members);
  });
  std::vector<Segment> kept;
  for (auto& candidate : candidates) {
    cancelled(budget.ctx);
    bool duplicate = false;
    for (const auto& other : kept) {
      if (angleDistance(candidate.angle, other.angle) > s.mergeAngle + 1e-12 ||
          std::abs(cross(candidate.center - other.center, other.direction)) > s.mergeDistance)
        continue;
      budget.add(candidate.members.size() + other.members.size());
      std::size_t common = 0, i = 0, j = 0;
      while (i < candidate.members.size() && j < other.members.size()) {
        const auto a = candidate.members[i], b = other.members[j];
        if (a == b)
          ++common;
        if (a <= b)
          ++i;
        if (b <= a)
          ++j;
      }
      if (double(common) >= 0.8 * std::min(candidate.members.size(), other.members.size())) {
        duplicate = true;
        break;
      }
    }
    if (!duplicate) {
      if (kept.size() == kMaxResults)
        resource("合格高光线段超过256条结果预算");
      kept.push_back(std::move(candidate));
    }
  }
  return kept;
}

Status detect(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  try {
    const Image* input = in.get("image").asImage();
    if (input && input->consistent() && input->pixelCount() > kMaxPixels)
      resource("高光图像超过8Mi像素预算");
    auto status = image_p1::image(input, "image", false, kMaxPixels);
    if (!status.ok)
      return status;
    if (input->channels != 1 && input->channels != 3 && input->channels != 4)
      return img::badInput("高光检测仅支持灰度、RGB或RGBA");
    if (input->depth == PixelDepth::F32 &&
        !cv::checkRange(cvx::view(*input), true, nullptr, 0, std::nextafter(1.0, 2.0)))
      return img::badInput("浮点图像必须为有限0..1强度");
    const std::uint64_t angles = 360;
    const std::uint64_t rhoBins =
        2ULL * (static_cast<std::uint64_t>(input->width) + input->height) + 1;
    if (rhoBins > static_cast<std::uint64_t>(std::numeric_limits<int>::max()) ||
        rhoBins >
            std::numeric_limits<std::uint64_t>::max() / angles / (sizeof(cv::Vec4i) + sizeof(int)))
      resource("Hough累加器维度超出安全范围");
    const std::uint64_t houghBytes = angles * rhoBins * (sizeof(cv::Vec4i) + sizeof(int));
    const std::uint64_t estimated = 2 * static_cast<std::uint64_t>(input->byteSize()) +
                                    input->pixelCount() * 28ULL +
                                    std::max(input->width, input->height) * 28ULL +
                                    kMaxPoints * 96ULL + 32ULL * 1024 * 1024 + houghBytes;
    if (estimated > 512ULL * 1024 * 1024)
      resource("高光检测预计内存超过512MiB预算");
    const Settings s = settings(p, input->scale);
    cancelled(ctx);
    const cv::Mat valid = regionMask(in, *input, s, ctx);
    const cv::Mat gray = normalizedGray(*input, valid, s.sigma);
    cv::Mat mask = cv::Mat::zeros(gray.size(), CV_8U);
    const auto points = ridges(gray, valid, s, mask, ctx);
    Budget budget{0, ctx};
    const auto fitted = segments(points, mask, s, budget);
    Json poseItems = Json::array(), items = Json::array();
    cv::Mat overlay = cvx::view(*input).clone();
    const double full = input->depth == PixelDepth::U8    ? 255
                        : input->depth == PixelDepth::U16 ? 65535
                                                          : 1;
    for (std::size_t i = 0; i < fitted.size(); ++i) {
      cancelled(ctx);
      const auto& line = fitted[i];
      const std::string id = "highlight-" + std::to_string(i);
      const double scale = input->scale;
      Record pose = poses::make2D(id, s.frame, "px", {line.center.x * scale, line.center.y * scale},
                                  line.angle, {line.length * scale, s.boxHeight * scale});
      pose.data["confidence"] = line.score;
      pose.data["pickable"] = false;
      pose.data["attributes"] = {{"method", "dual-axis-bright-ridge-tls-v1"},
                                 {"rms", line.rms * scale},
                                 {"coverage", line.coverage}};
      poseItems.push_back(std::move(pose.data));
      items.push_back({{"id", id},
                       {"endpoints",
                        {{line.first.x * scale, line.first.y * scale},
                         {line.last.x * scale, line.last.y * scale}}},
                       {"length", line.length * scale},
                       {"rms", line.rms * scale},
                       {"maxError", line.maxError * scale},
                       {"coverage", line.coverage},
                       {"support", line.members.size()},
                       {"meanContrast", line.contrast},
                       {"score", line.score},
                       {"angle", line.angle}});
      const cv::Scalar color = input->channels == 1 ? cv::Scalar(0) : cv::Scalar(full, 0, 0, full);
      cv::line(overlay, line.first, line.last, color, 1, cv::LINE_8);
      const cv::Point2d normal(-line.direction.y, line.direction.x);
      const std::array<cv::Point2d, 4> corners{
          {line.first + normal * (s.boxHeight / 2), line.last + normal * (s.boxHeight / 2),
           line.last - normal * (s.boxHeight / 2), line.first - normal * (s.boxHeight / 2)}};
      for (int j = 0; j < 4; ++j)
        cv::line(overlay, corners[static_cast<std::size_t>(j)],
                 corners[static_cast<std::size_t>((j + 1) % 4)],
                 input->channels == 1 ? cv::Scalar(full) : cv::Scalar(0, full, 0, full), 1,
                 cv::LINE_8);
    }
    if (input->channels == 4) {
      const int channels[] = {3, 3};
      const cv::Mat original = cvx::view(*input);
      cv::mixChannels(&original, 1, &overlay, 1, channels, 1);
    }
    Record posesRecord = poses::make2DList(std::move(poseItems), s.frame, "px");
    Record segmentsRecord{"image.Highlights.v1",
                          {{"frame", s.frame}, {"unit", "px"}, {"items", std::move(items)}}};
    std::string why;
    if (!poses::validate(posesRecord, why) || !values::boundedJson(segmentsRecord.data, why))
      return Status::Error(Phase::Execute, "resource_limit", why);
    Image maskOutput, overlayOutput;
    if (!cvx::fromMat(std::move(mask), maskOutput) ||
        !cvx::fromMat(std::move(overlay), overlayOutput))
      return Status::Error(Phase::Execute, "internal", "高光输出图像格式无效");
    maskOutput.scale = overlayOutput.scale = input->scale;
    cancelled(ctx);
    out.set("instances", Data::record(std::move(posesRecord)));
    out.set("segments", Data::record(std::move(segmentsRecord)));
    out.set("mask", Data::image(std::move(maskOutput)));
    out.set("image", Data::image(std::move(overlayOutput)));
    return Status::Ok();
  } catch (const Failure& failure) {
    return failure.status;
  } catch (const cv::Exception& error) {
    return img::fromCvError(error);
  }
}

}

void registerImageHighlights(Registry& registry) {
  auto op =
      image_p1::descriptor("image.highlights", "高光直线检测",
                           "highlight bright ridge line segment detection grouping rotated region");
  op.doc =
      "双轴亮脊与双侧对比度检测，按真实支持点拟合和分组高光直线。返回原图像素旋转区域、线段质量及只读源图叠画；confidence为算法质量分数。";
  op.inputs = {img::imageIn(), Port{"roi", "Box2D", "像素矩形ROI", "与region互斥。", false},
               image_p1::recordPort("region", poses::kPose2DType, false)};
  op.outputs = {image_p1::recordPort("instances", poses::kPoses2DType),
                image_p1::recordPort("segments", "image.Highlights.v1"),
                img::imageOut("mask", "u8候选亮脊，不是最终旋转区域填充。"),
                img::imageOut("image", "保留源位深/通道/alpha的高光线段与旋转框叠图。")};
  auto px = [](const char* name, const char* label, double value, const char* doc) {
    auto param = img::floatParam(name, label, value, doc);
    param.unit = "px";
    return param;
  };
  op.params = {
      image_p1::text("frame", "图像参考帧", "image"),
      img::floatParam("minBrightness", "最小亮度", 0.3, "归一化[0,1]。"),
      img::floatParam("minContrast", "最小双侧对比度", 0.05, "归一化(0,1]，平坦亮面不构成亮脊。"),
      px("ridgeRadius", "亮脊邻域半径", 4, "[1,64]原图像素；工作半径四舍五入且最小1。"),
      px("smoothSigma", "平滑标准差", 1, "[0,16]原图像素，0禁用。ROI内归一化，不借用外部亮度。"),
      px("minLength", "最小线段长度", 30, "[2,1e6]原图像素。"),
      px("maxGap", "最大缺失间隙", 10, "[0,1e6]原图像素；超限分成独立线段。"),
      px("maxResidual", "最大拟合残差", 1, "[0.1,64]原图像素；支持点距离及RMS门限。"),
      px("boxHeight", "旋转区域高度", 8, "[0.25,1e6]原图像素。"),
      img::floatParam("referenceAngle", "参考方向(°)", 0, "[-360,360]，无向直线模180°。"),
      img::floatParam("angleTolerance", "方向容差(°)", 90, "[0,90]，含等号。"),
      img::intParam("minSupport", "最少支持点", 12, 2, 4096, "实际工作亮脊点数量，不是概率。"),
      img::floatParam("minCoverage", "最小支持覆盖率", 0.5,
                      "[0,1]，像素投影支持区间并集长度/线段长度。"),
      px("mergeDistance", "重复线段距离", 2, "[0,64]原图像素，还需共享至少80%较短支持集。"),
      img::floatParam("mergeAngle", "重复线段角度(°)", 2, "[0,90]，重复去除不跨间隙合并。")};
  op.compute = &detect;
  registry.addOperator(std::move(op));
}

} // namespace lyflow::ops
