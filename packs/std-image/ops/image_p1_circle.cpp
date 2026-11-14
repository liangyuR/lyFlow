#include <cstdint>

#include "p1_common.h"

namespace lyflow::ops {
namespace {

constexpr double kPi = 3.1415926535897932384626433832795;

struct Edge {
  double radius = 0, gradient = 0;
};
struct Probe {
  double angle = 0;
  std::vector<Edge> edges;
};
struct Settings {
  double x = 0, y = 0, lo = 0, hi = 0, step = 1, start = 0, sweep = 0, gradient = 0, fraction = 0;
  int samples = 0, band = 1, length = 0;
  bool reverse = false;
};

bool bilinear(const cv::Mat& image, double x, double y, double& value) {
  if (x < 0 || y < 0 || x > image.cols - 1 || y > image.rows - 1)
    return false;
  const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
  const int x1 = std::min(x0 + 1, image.cols - 1), y1 = std::min(y0 + 1, image.rows - 1);
  const double dx = x - x0, dy = y - y0;
  value = (1 - dy) * ((1 - dx) * image.at<double>(y0, x0) + dx * image.at<double>(y0, x1)) +
          dy * ((1 - dx) * image.at<double>(y1, x0) + dx * image.at<double>(y1, x1));
  return std::isfinite(value);
}

Status scan(const Inputs& in, const ParamView& p, ExecContext& ctx, const Image*& input,
            Settings& settings, std::vector<Probe>& probes) {
  input = in.get("image").asImage();
  auto s = image_p1::image(input);
  if (!s.ok)
    return s;
  const auto center = p.vec2("center");
  settings.x = center[0];
  settings.y = center[1];
  settings.lo = p.number("radiusMin");
  settings.hi = p.number("radiusMax");
  settings.step = p.number("radialStep");
  const double sourceStep = settings.step * input->scale;
  if (!std::isfinite(sourceStep) || sourceStep < 0.25 || sourceStep > 4)
    return img::badParam("径向采样步长必须在原图[0.25,4]像素范围内", "radialStep");
  settings.start = p.number("angleStart") * kPi / 180;
  settings.sweep = (p.number("angleEnd") - p.number("angleStart")) * kPi / 180;
  settings.gradient = p.number("minGradient");
  settings.fraction = p.number("minValidFraction");
  settings.samples = static_cast<int>(p.integer("samples"));
  settings.band = static_cast<int>(p.integer("band"));
  settings.reverse = p.choice("direction") == "outside_in";
  if (!std::isfinite(settings.x) || !std::isfinite(settings.y) || !std::isfinite(settings.lo) ||
      !std::isfinite(settings.hi) || settings.lo < 0 || settings.hi <= settings.lo ||
      settings.hi > 1000000 || (settings.hi - settings.lo) / settings.step > 4096 ||
      !std::isfinite(settings.start) || !std::isfinite(settings.sweep) ||
      std::abs(settings.start) > 4 * kPi || std::abs(settings.sweep) < kPi / 180 ||
      std::abs(settings.sweep) > 2 * kPi + 1e-12 || !std::isfinite(settings.gradient) ||
      settings.gradient <= 0 || !std::isfinite(settings.fraction) || settings.fraction <= 0 ||
      settings.fraction > 1 || settings.samples < 8 || settings.samples > 720 ||
      settings.band < 1 || settings.band > 31 || settings.band % 2 != 1)
    return img::badParam("径向范围、角度、扫描数量或梯度阈值非法", "radiusMin");
  settings.length = int(std::floor((settings.hi - settings.lo) / settings.step)) + 1;
  if (settings.length < 5 ||
      std::uint64_t(settings.length) * settings.samples * settings.band > 67108864)
    return img::badParam("径向采样不足5点或超过64Mi采样预算", "radialStep");
  cv::Mat gray;
  image_p1::gray(*input).convertTo(gray, CV_64F);
  const bool complete = std::abs(std::abs(settings.sweep) - 2 * kPi) <= 1e-12;
  for (int n = 0; n < settings.samples; ++n) {
    if (ctx.cancelled())
      return Status::Ok();
    const double angle =
        settings.start + settings.sweep * n / (complete ? settings.samples : settings.samples - 1);
    const double cs = std::cos(angle), sn = std::sin(angle);
    std::vector<double> values(settings.length, std::numeric_limits<double>::quiet_NaN());
    for (int i = 0; i < settings.length; ++i) {
      const double radius = settings.lo + i * settings.step;
      double sum = 0;
      bool valid = true;
      for (int t = -settings.band / 2; t <= settings.band / 2; ++t) {
        double sample = 0;
        if (!bilinear(gray, settings.x + cs * radius - sn * t, settings.y + sn * radius + cs * t,
                      sample)) {
          valid = false;
          break;
        }
        sum += sample;
      }
      if (valid)
        values[i] = sum / settings.band;
    }
    std::vector<double> gradients(settings.length, 0);
    for (int i = 1; i + 1 < settings.length; ++i)
      if (std::isfinite(values[i - 1]) && std::isfinite(values[i]) && std::isfinite(values[i + 1]))
        gradients[i] = (values[i + 1] - values[i - 1]) / (2 * settings.step);
    Probe probe;
    probe.angle = angle;
    for (int i = 2; i + 2 < settings.length; ++i) {
      const double magnitude = std::abs(gradients[i]);
      const double epsilon = magnitude * 1e-6;
      if (magnitude < settings.gradient || magnitude + epsilon < std::abs(gradients[i - 1]))
        continue;
      int end = i;
      while (end + 3 < settings.length && gradients[i] * gradients[end + 1] > 0 &&
             std::abs(std::abs(gradients[end + 1]) - magnitude) <= epsilon)
        ++end;
      if (magnitude + epsilon < std::abs(gradients[end + 1])) {
        i = end;
        continue;
      }
      const double previous = std::abs(gradients[i - 1]), next = std::abs(gradients[end + 1]);
      const double curvature = previous - 2 * magnitude + next;
      const double subpixel = end == i && curvature != 0
                                  ? std::clamp(0.5 * (previous - next) / curvature, -0.5, 0.5)
                                  : 0;
      Edge edge{settings.lo + ((i + end) * 0.5 + subpixel) * settings.step,
                gradients[i] * (settings.reverse ? -1 : 1)};
      i = end;
      if (!probe.edges.empty() && edge.radius - probe.edges.back().radius <= settings.step * 1.5) {
        if (magnitude > std::abs(probe.edges.back().gradient))
          probe.edges.back() = edge;
      } else {
        probe.edges.push_back(edge);
      }
    }
    if (settings.reverse)
      std::reverse(probe.edges.begin(), probe.edges.end());
    probes.push_back(std::move(probe));
  }
  return Status::Ok();
}

bool fitCircle(const std::vector<cv::Point2d>& points, cv::Point2d& center, double& radius) {
  if (points.size() < 3)
    return false;
  cv::Point2d origin;
  for (const auto& point : points)
    origin += point;
  origin *= 1.0 / points.size();
  cv::Mat a(int(points.size()), 3, CV_64F), b(int(points.size()), 1, CV_64F);
  for (int i = 0; i < a.rows; ++i) {
    const auto point = points[i] - origin;
    a.at<double>(i, 0) = point.x;
    a.at<double>(i, 1) = point.y;
    a.at<double>(i, 2) = 1;
    b.at<double>(i) = point.x * point.x + point.y * point.y;
  }
  cv::SVD decomposition(a, cv::SVD::NO_UV);
  if (decomposition.w.at<double>(2) <= decomposition.w.at<double>(0) * 1e-9)
    return false;
  cv::Mat solution;
  if (!cv::solve(a, b, solution, cv::DECOMP_SVD))
    return false;
  center = origin + cv::Point2d(solution.at<double>(0) / 2, solution.at<double>(1) / 2);
  const double r2 = solution.at<double>(2) + std::pow(solution.at<double>(0), 2) / 4 +
                    std::pow(solution.at<double>(1), 2) / 4;
  radius = std::sqrt(std::max(0.0, r2));
  return std::isfinite(center.x) && std::isfinite(center.y) && std::isfinite(radius) && radius > 0;
}

Status circle(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  const Image* input = nullptr;
  Settings settings;
  std::vector<Probe> probes;
  auto s = scan(in, p, ctx, input, settings, probes);
  if (!s.ok || ctx.cancelled())
    return s;
  const auto& polarity = p.choice("polarity");
  std::vector<cv::Point2d> points;
  for (const auto& probe : probes) {
    std::vector<Edge> candidates;
    for (const auto& edge : probe.edges)
      if (polarity == "either" || (polarity == "rising" && edge.gradient > 0) ||
          (polarity == "falling" && edge.gradient < 0))
        candidates.push_back(edge);
    if (candidates.empty())
      continue;
    const auto& selection = p.choice("selection");
    const Edge* chosen = selection == "last" ? &candidates.back() : &candidates.front();
    if (selection == "strongest")
      chosen = &*std::max_element(
          candidates.begin(), candidates.end(),
          [](const Edge& a, const Edge& b) { return std::abs(a.gradient) < std::abs(b.gradient); });
    points.emplace_back(settings.x + std::cos(probe.angle) * chosen->radius,
                        settings.y + std::sin(probe.angle) * chosen->radius);
  }
  if (points.size() < 6 || points.size() < settings.samples * settings.fraction)
    return Status::Error(Phase::Execute, "no_target", "圆弧有效边缘比例或边缘数不足");
  const double tolerance = p.number("fitTolerance"), maxRms = p.number("maxRms");
  const double maxCenterOffset = p.number("maxCenterOffset");
  const double curvatureRatio = p.number("minCurvatureRatio");
  const double inlierFraction = p.number("minInlierFraction");
  const int iterations = static_cast<int>(p.integer("fitIterations"));
  if (!std::isfinite(tolerance) || tolerance <= 0 || !std::isfinite(maxRms) || maxRms <= 0 ||
      !std::isfinite(inlierFraction) || inlierFraction <= 0 || inlierFraction > 1 ||
      !std::isfinite(maxCenterOffset) || maxCenterOffset <= 0 || maxCenterOffset > 1000000 ||
      !std::isfinite(curvatureRatio) || curvatureRatio < 1 || curvatureRatio > 100 ||
      iterations < 1 || iterations > 1024)
    return img::badParam("圆拟合容差、质量或迭代上限非法", "fitTolerance");
  std::vector<std::size_t> best;
  double bestSquared = std::numeric_limits<double>::infinity();
  std::uint32_t state = 0x5eeda11u;
  const auto index = [&]() {
    state = state * 1664525u + 1013904223u;
    return std::size_t(state) % points.size();
  };
  for (int iteration = 0; iteration < iterations; ++iteration) {
    if (ctx.cancelled())
      return Status::Ok();
    const auto i = index(), j = index(), k = index();
    if (i == j || i == k || j == k)
      continue;
    cv::Point2d center;
    double radius;
    if (!fitCircle({points[i], points[j], points[k]}, center, radius))
      continue;
    if (cv::norm(center - cv::Point2d(settings.x, settings.y)) > maxCenterOffset ||
        radius < settings.lo - tolerance || radius > settings.hi + tolerance)
      continue;
    std::vector<std::size_t> inliers;
    double squared = 0;
    for (std::size_t n = 0; n < points.size(); ++n) {
      const double residual = std::abs(cv::norm(points[n] - center) - radius);
      if (residual <= tolerance) {
        inliers.push_back(n);
        squared += residual * residual;
      }
    }
    if (inliers.size() > best.size() || (inliers.size() == best.size() && squared < bestSquared)) {
      best = std::move(inliers);
      bestSquared = squared;
    }
  }
  if (best.size() < 6 || best.size() < points.size() * inlierFraction)
    return Status::Error(Phase::Execute, "quality_rejected", "圆弧拟合内点比例不足");
  std::vector<cv::Point2d> accepted;
  for (auto acceptedIndex : best)
    accepted.push_back(points[acceptedIndex]);
  cv::Point2d center;
  double radius;
  if (!fitCircle(accepted, center, radius))
    return Status::Error(Phase::Execute, "degenerate_geometry", "圆弧采样几何退化");
  double squared = 0;
  for (const auto& point : accepted)
    squared += std::pow(cv::norm(point - center) - radius, 2);
  const double rms = std::sqrt(squared / accepted.size());
  cv::Point2d mean;
  for (const auto& point : accepted)
    mean += point;
  mean *= 1.0 / accepted.size();
  double xx = 0, xy = 0, yy = 0;
  for (const auto& point : accepted) {
    const auto delta = point - mean;
    xx += delta.x * delta.x;
    xy += delta.x * delta.y;
    yy += delta.y * delta.y;
  }
  const double lineRms =
      std::sqrt(std::max(0.0, 0.5 * (xx + yy - std::hypot(xx - yy, 2 * xy))) / accepted.size());
  if (lineRms <= std::max(1e-6, rms * curvatureRatio))
    return Status::Error(Phase::Execute, "quality_rejected", "边缘更符合直线，圆弧曲率证据不足");
  if (!std::isfinite(rms) || rms > maxRms ||
      cv::norm(center - cv::Point2d(settings.x, settings.y)) > maxCenterOffset ||
      radius < settings.lo - tolerance || radius > settings.hi + tolerance ||
      std::max({std::abs(center.x), std::abs(center.y), radius}) >
          std::numeric_limits<float>::max())
    return Status::Error(Phase::Execute, "quality_rejected", "圆拟合残差或输出范围不满足要求");
  Circle2D result;
  result.center[0] = static_cast<float>(center.x);
  result.center[1] = static_cast<float>(center.y);
  result.radius = static_cast<float>(radius);
  result.unit = Unit2D::Pixel;
  out.set("circle", Data::circle2d(result));
  nlohmann::json edges = nlohmann::json::array();
  for (const auto& point : points)
    edges.push_back({point.x * input->scale, point.y * input->scale});
  return image_p1::record(out, "quality", "image.CircleCaliper.v1",
                          {{"unit", "px"},
                           {"coordinateSpace", "source"},
                           {"scale", input->scale},
                           {"center", {center.x * input->scale, center.y * input->scale}},
                           {"points", std::move(edges)},
                           {"inliers", best},
                           {"samples", settings.samples},
                           {"validScans", points.size()},
                           {"validFraction", double(points.size()) / settings.samples},
                           {"rms", rms * input->scale},
                           {"lineRms", lineRms * input->scale},
                           {"radius", radius * input->scale}});
}

Status ring(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  const Image* input = nullptr;
  Settings settings;
  std::vector<Probe> probes;
  auto s = scan(in, p, ctx, input, settings, probes);
  if (!s.ok || ctx.cancelled())
    return s;
  std::vector<double> widths;
  nlohmann::json pairs = nlohmann::json::array();
  const bool bright = p.choice("region") == "bright";
  for (const auto& probe : probes) {
    const Edge* first = nullptr;
    const Edge* second = nullptr;
    for (const auto& edge : probe.edges) {
      const bool opening = bright ? edge.gradient > 0 : edge.gradient < 0;
      if (!first && opening)
        first = &edge;
      else if (first && !opening) {
        second = &edge;
        if (p.choice("pairing") == "adjacent")
          break;
      }
    }
    if (!first || !second)
      continue;
    const double width = std::abs(second->radius - first->radius);
    if (!(width > 0))
      continue;
    widths.push_back(width);
    pairs.push_back({{"angle", probe.angle},
                     {"width", width * input->scale},
                     {"radii", {first->radius * input->scale, second->radius * input->scale}}});
  }
  const bool valid = !widths.empty() && widths.size() >= settings.samples * settings.fraction;
  double low = 0, high = 0, mean = 0;
  if (!widths.empty()) {
    low = *std::min_element(widths.begin(), widths.end());
    high = *std::max_element(widths.begin(), widths.end());
    for (double width : widths)
      mean += width;
    mean /= widths.size();
  }
  for (const auto& value :
       {std::pair<const char*, double>{"minimum", low}, {"maximum", high}, {"mean", mean}}) {
    Measurement measurement;
    measurement.unit = "px";
    measurement.ok = valid;
    measurement.value = valid ? value.second : std::numeric_limits<double>::quiet_NaN();
    if (!valid)
      measurement.message = "圆环配对边缘有效比例不足";
    out.set(value.first, Data::measurement(std::move(measurement)));
  }
  return image_p1::record(out, "quality", "image.RingWidth.v1",
                          {{"unit", "px"},
                           {"coordinateSpace", "source"},
                           {"scale", input->scale},
                           {"center", {settings.x * input->scale, settings.y * input->scale}},
                           {"valid", valid},
                           {"samples", settings.samples},
                           {"validScans", widths.size()},
                           {"validFraction", double(widths.size()) / settings.samples},
                           {"pairs", std::move(pairs)}});
}

std::vector<Param> common() {
  auto center = image_p1::vector("center", "扫描中心", {100, 100}, "px");
  auto lo = img::floatParam("radiusMin", "最小扫描半径", 20, "像素。");
  auto hi = img::floatParam("radiusMax", "最大扫描半径", 80, "像素。");
  auto step =
      img::floatParam("radialStep", "径向采样步长", 1, "[0.25,4]原图像素；预览随图像比例换算。");
  lo.unit = hi.unit = step.unit = "px";
  return {center,
          lo,
          hi,
          step,
          img::floatParam("angleStart", "圆弧起始角(°)", 0, "图像x轴向y轴为正。"),
          img::floatParam("angleEnd", "圆弧结束角(°)", 360, "扫角绝对值1..360度。"),
          img::intParam("samples", "周向扫描数", 64, 8, 720, "完整圆周不重复首尾采样。"),
          img::intParam("band", "切向平均带宽", 1, 1, 31, "奇数，像素。"),
          img::enumParam("direction", "径向扫描方向", "inside_out",
                         {{"inside_out", "由内向外", ""}, {"outside_in", "由外向内", ""}},
                         "极性沿扫描方向定义。"),
          img::floatParam("minGradient", "最小边缘梯度", 10, "输入原值/像素；float图需相应调低。"),
          img::floatParam("minValidFraction", "最小有效比例", 0.5, "(0,1]。")};
}

} // namespace

void registerImageP1Circles(Registry& r) {
  auto op = image_p1::descriptor("image.circle_caliper", "圆弧边缘卡尺",
                                 "radial circle arc edge caliper subpixel robust fitting RANSAC");
  op.inputs = {img::imageIn()};
  op.outputs = {Port{"circle", "Circle2D", "Circle", "像素圆。", true},
                image_p1::recordPort("quality", "image.CircleCaliper.v1")};
  op.params = common();
  op.params.push_back(
      img::enumParam("polarity", "边缘极性", "either",
                     {{"either", "任意", ""}, {"rising", "暗到亮", ""}, {"falling", "亮到暗", ""}},
                     "沿径向扫描方向。"));
  op.params.push_back(
      img::enumParam("selection", "边缘选择", "strongest",
                     {{"strongest", "最强", ""}, {"first", "首个", ""}, {"last", "末个", ""}}, ""));
  auto tolerance = img::floatParam("fitTolerance", "拟合内点容差", 1.5, "像素。");
  auto rms = img::floatParam("maxRms", "最大径向RMS", 1, "像素。");
  tolerance.unit = rms.unit = "px";
  auto offset = img::floatParam("maxCenterOffset", "最大拟合圆心偏移", 10,
                                "相对扫描中心的像素距离，拒绝局部直边拟合成远处大圆。");
  offset.unit = "px";
  op.params.push_back(tolerance);
  op.params.push_back(rms);
  op.params.push_back(offset);
  op.params.push_back(img::floatParam("minCurvatureRatio", "最小曲率证据比", 1.5,
                                      "直线RMS须大于圆RMS的此倍数，[1,100]。"));
  op.params.push_back(img::floatParam("minInlierFraction", "最小拟合内点比例", 0.6, "(0,1]。"));
  op.params.push_back(img::intParam("fitIterations", "确定性RANSAC轮数", 128, 1, 1024,
                                    "固定种子，最终内点最小二乘。"));
  op.doc =
      "圆环ROI径向扫描、切向平均、抛物线亚像素边缘和确定性鲁棒圆拟合；无目标、退化弧或质量不足报告诊断。";
  op.compute = &circle;
  r.addOperator(std::move(op));

  op = image_p1::descriptor("image.ring_width", "圆环周向宽度量测",
                            "annulus ring width radial edge pairs minimum maximum mean");
  op.inputs = {img::imageIn()};
  op.outputs = {Port{"minimum", "Measurement", "Minimum", "像素宽度。", true},
                Port{"maximum", "Measurement", "Maximum", "像素宽度。", true},
                Port{"mean", "Measurement", "Mean", "像素宽度。", true},
                image_p1::recordPort("quality", "image.RingWidth.v1")};
  op.params = common();
  op.params.push_back(img::enumParam("region", "环区域极性", "bright",
                                     {{"bright", "亮环", "暗-亮-暗"}, {"dark", "暗环", "亮-暗-亮"}},
                                     "沿扫描方向配对相反边缘。"));
  op.params.push_back(img::enumParam("pairing", "边缘配对", "adjacent",
                                     {{"adjacent", "首对相邻", ""}, {"outermost", "首末包络", ""}},
                                     "首个进入边缘与相邻或最后离开边缘配对。"));
  op.doc =
      "沿明确径向ROI配对内外边缘，输出周向最小/最大/平均宽度和配对质量；有效比例不足时Measurement.ok=false，不伪造零宽。";
  op.compute = &ring;
  r.addOperator(std::move(op));
}

} // namespace lyflow::ops
