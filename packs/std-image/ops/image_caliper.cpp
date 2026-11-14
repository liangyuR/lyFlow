#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "ops.h"
#include "params.h"

namespace lyflow::ops {
namespace {

struct Scan {
  std::vector<double> values;
  int begin = 0;
  double across = 0;
};

struct Edge {
  double coordinate = 0;
  double contrast = 0;
};

Param roiParam() {
  Param roi;
  roi.name = "roi";
  roi.label = "ROI";
  roi.type = ParamType::Vec4f;
  roi.def = Value::vec({0, 0, 100, 100});
  roi.unit = "px";
  roi.doc = "半开矩形 [x0,y0,x1,y1)，超出图像的部分裁切。";
  return roi;
}

std::vector<Param> commonParams() {
  Param band = img::intParam("band", "平均带宽", 1, 1, 31, "每条扫描沿横向平均的奇数宽度。");
  band.step = 2;
  band.unit = "px";
  return {
      roiParam(),
      img::enumParam("axis", "扫描轴", "x", {{"x", "X", "沿行扫描"}, {"y", "Y", "沿列扫描"}}, ""),
      img::intParam("samples", "扫描数量", 16, 2, 256, "等间隔分布在 ROI 内的扫描条数。"),
      band,
      img::floatParam("minContrast", "最小梯度", 10, "相邻中心差分强度阈值，单位为输入原值/px。"),
      img::floatParam("minValidFraction", "最小有效比例", 0.5,
                      "有效扫描数与总扫描数的最低比例，0..1。")};
}

Status prepare(const Image& image, const ParamView& params, bool& xAxis, cv::Rect& roi) {
  if (!image.consistent() || image.channels != 1 || image.pixelCount() > 16u * 1024u * 1024u)
    return img::badInput("卡尺需要不超过 16 MP 的单通道图像");
  const auto bounds = params.vec4("roi");
  for (float value : bounds)
    if (!std::isfinite(value))
      return img::badParam("ROI 坐标必须有限", "roi");
  if (bounds[2] <= bounds[0] || bounds[3] <= bounds[1])
    return img::badParam("ROI 必须具有正宽高", "roi");
  const int x0 = static_cast<int>(std::floor(std::clamp<double>(bounds[0], 0, image.width)));
  const int y0 = static_cast<int>(std::floor(std::clamp<double>(bounds[1], 0, image.height)));
  const int x1 = static_cast<int>(std::ceil(std::clamp<double>(bounds[2], 0, image.width)));
  const int y1 = static_cast<int>(std::ceil(std::clamp<double>(bounds[3], 0, image.height)));
  roi = cv::Rect(x0, y0, x1 - x0, y1 - y0);
  xAxis = params.choice("axis") == "x";
  const int length = xAxis ? roi.width : roi.height;
  const int count = static_cast<int>(params.integer("samples"));
  const int band = static_cast<int>(params.integer("band"));
  if (length < 5 || (xAxis ? roi.height : roi.width) < 2)
    return img::badParam("裁切后 ROI 的扫描长度至少 5 像素、横向至少 2 像素", "roi");
  if (band < 1 || band % 2 == 0)
    return img::badParam("平均带宽必须是正奇数", "band");
  if (static_cast<std::size_t>(length) * count * band > 32u * 1024u * 1024u)
    return Status::Error(Phase::Execute, "resource_limit",
                         "扫描预算超过 32 M 样本，请减少 ROI/扫描数量/带宽");
  const double contrast = params.number("minContrast");
  const double fraction = params.number("minValidFraction");
  if (!std::isfinite(contrast) || contrast <= 0)
    return img::badParam("最小梯度必须大于零且有限", "minContrast");
  if (!std::isfinite(fraction) || fraction <= 0 || fraction > 1)
    return img::badParam("最小有效比例必须在 (0,1]", "minValidFraction");
  return Status::Ok();
}

Scan profile(const Image& image, const ParamView& params, const cv::Rect& roi, bool xAxis,
             int index) {
  const int count = static_cast<int>(params.integer("samples"));
  const int length = xAxis ? roi.width : roi.height;
  const int crossStart = xAxis ? roi.y : roi.x;
  const int crossLength = xAxis ? roi.height : roi.width;
  const double cross = crossStart + static_cast<double>(crossLength - 1) * index / (count - 1);
  const int center = static_cast<int>(std::lround(cross));
  const int half = static_cast<int>(params.integer("band")) / 2;
  Scan out;
  out.begin = xAxis ? roi.x : roi.y;
  out.across = center;
  out.values.reserve(static_cast<std::size_t>(length));
  for (int i = 0; i < length; ++i) {
    double sum = 0;
    int valid = 0;
    for (int j = std::max(crossStart, center - half);
         j <= std::min(crossStart + crossLength - 1, center + half); ++j) {
      const double value = xAxis ? image.at(out.begin + i, j, 0) : image.at(j, out.begin + i, 0);
      if (std::isfinite(value)) {
        sum += value;
        ++valid;
      }
    }
    out.values.push_back(valid ? sum / valid : std::numeric_limits<double>::quiet_NaN());
  }
  return out;
}

std::vector<Edge> edges(const Scan& scan, int sign, double threshold) {
  std::vector<double> gradient(scan.values.size(), 0);
  for (std::size_t i = 1; i + 1 < scan.values.size(); ++i) {
    if (std::isfinite(scan.values[i - 1]) && std::isfinite(scan.values[i]) &&
        std::isfinite(scan.values[i + 1])) {
      const double delta = (scan.values[i + 1] - scan.values[i - 1]) / 2;
      gradient[i] = sign == 0 ? std::abs(delta) : sign * delta;
    }
  }
  std::vector<Edge> result;
  for (std::size_t i = 1; i + 1 < gradient.size(); ++i) {
    const double g = gradient[i];
    if (g < threshold || g < gradient[i - 1] || g <= gradient[i + 1])
      continue;
    const double curvature = gradient[i - 1] - 2 * g + gradient[i + 1];
    const double offset =
        curvature < 0 ? std::clamp((gradient[i - 1] - gradient[i + 1]) / (2 * curvature), -0.5, 0.5)
                      : 0;
    result.push_back({scan.begin + static_cast<double>(i) + offset, g});
  }
  return result;
}

Measurement measured(double value, const char* message = "") {
  Measurement out;
  out.value = value;
  out.ok = std::isfinite(value);
  out.unit = "px";
  out.message = message;
  return out;
}

Port recordOutput(const char* type) {
  return withContract(Port{"samples", "Record", "扫描结果", "原图坐标及逐扫描质量。", true},
                      {{"recordType", type}});
}

Status lineCompute(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                   ExecContext& ctx) {
  const Image& image = *inputs.get("image").asImage();
  bool xAxis = true;
  cv::Rect roi;
  const Status valid = prepare(image, params, xAxis, roi);
  if (!valid.ok)
    return valid;
  const double tolerance = params.number("fitTolerance");
  if (!std::isfinite(tolerance) || tolerance <= 0)
    return img::badParam("拟合容差必须是正有限数", "fitTolerance");
  const std::string polarity = params.choice("polarity");
  const bool reverse = params.choice("direction") == "negative";
  const int sign = (polarity == "rising" ? 1 : polarity == "falling" ? -1 : 0) * (reverse ? -1 : 1);
  const std::string selection = params.choice("selection");
  const int count = static_cast<int>(params.integer("samples"));
  std::vector<cv::Point2f> points;
  nlohmann::json scans = nlohmann::json::array();
  for (int i = 0; i < count; ++i) {
    if (ctx.cancelled())
      return Status::Ok();
    const Scan scan = profile(image, params, roi, xAxis, i);
    auto candidates = edges(scan, sign, params.number("minContrast"));
    if (candidates.empty())
      continue;
    if (reverse)
      std::reverse(candidates.begin(), candidates.end());
    Edge edge = selection == "last" ? candidates.back() : candidates.front();
    if (selection == "strongest")
      edge =
          *std::max_element(candidates.begin(), candidates.end(),
                            [](const Edge& a, const Edge& b) { return a.contrast < b.contrast; });
    const cv::Point2f point =
        xAxis ? cv::Point2f(static_cast<float>(edge.coordinate), static_cast<float>(scan.across))
              : cv::Point2f(static_cast<float>(scan.across), static_cast<float>(edge.coordinate));
    points.push_back(point);
    scans.push_back({{"scan", i},
                     {"point", {point.x * image.scale, point.y * image.scale}},
                     {"contrast", edge.contrast}});
  }
  const auto enough = [&](std::size_t n) {
    return n >= 2 && static_cast<double>(n) / count >= params.number("minValidFraction");
  };
  if (!enough(points.size()))
    return Status::Error(Phase::Execute, "edge_not_found",
                         "满足极性/对比度的有效卡尺不足，未生成直线");
  cv::Vec4f fit;
  cv::fitLine(points, fit, cv::DIST_HUBER, 0, 0.01, 0.01);
  std::vector<cv::Point2f> inliers;
  for (std::size_t i = 0; i < points.size(); ++i) {
    const auto& p = points[i];
    const double distance = std::abs((p.x - fit[2]) * fit[1] - (p.y - fit[3]) * fit[0]);
    scans[i]["inlier"] = distance <= tolerance;
    if (distance <= tolerance)
      inliers.push_back(p);
  }
  if (!enough(inliers.size()))
    return Status::Error(Phase::Execute, "edge_not_found", "拟合后有效卡尺比例不足，未生成直线");
  cv::fitLine(inliers, fit, cv::DIST_L2, 0, 0.01, 0.01);
  double lo = std::numeric_limits<double>::infinity(), hi = -lo, sum = 0;
  for (const auto& p : inliers) {
    const double projection = (p.x - fit[2]) * fit[0] + (p.y - fit[3]) * fit[1];
    lo = std::min(lo, projection);
    hi = std::max(hi, projection);
    sum += xAxis ? p.x : p.y;
  }
  if (!(hi - lo > 1e-6))
    return Status::Error(Phase::Execute, "edge_not_found", "边缘点退化，不能构造直线");
  Line2D line;
  line.point[0] = fit[2];
  line.point[1] = fit[3];
  line.dir[0] = fit[0];
  line.dir[1] = fit[1];
  line.hasSegment = true;
  line.unit = Unit2D::Pixel;
  for (int c = 0; c < 2; ++c) {
    line.start[c] = static_cast<float>(fit[c + 2] + lo * fit[c]);
    line.end[c] = static_cast<float>(fit[c + 2] + hi * fit[c]);
  }
  outputs.set("line", Data::line2d(line));
  outputs.set("position", Data::measurement(measured(sum / inliers.size())));
  outputs.set("samples",
              Data::record(Record{"image.CaliperSamples.v1",
                                  {{"unit", "px"},
                                   {"coordinateSpace", "source"},
                                   {"scale", image.scale},
                                   {"validFraction", static_cast<double>(inliers.size()) / count},
                                   {"points", std::move(scans)}}}));
  return Status::Ok();
}

Status widthCompute(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                    ExecContext& ctx) {
  const Image& image = *inputs.get("image").asImage();
  bool xAxis = true;
  cv::Rect roi;
  const Status valid = prepare(image, params, xAxis, roi);
  if (!valid.ok)
    return valid;
  const double minWidth = params.number("minWidth"), maxWidth = params.number("maxWidth");
  if (!std::isfinite(minWidth) || !std::isfinite(maxWidth) || minWidth < 0 || maxWidth < minWidth)
    return img::badParam("宽度必须有限且 0 ≤ minWidth ≤ maxWidth", "minWidth");
  const bool inner = params.choice("dimension") == "inner";
  const int sign = (params.choice("polarity") == "bright" ? 1 : -1) * (inner ? -1 : 1);
  const int count = static_cast<int>(params.integer("samples"));
  std::vector<double> widths;
  nlohmann::json scans = nlohmann::json::array();
  for (int i = 0; i < count; ++i) {
    if (ctx.cancelled())
      return Status::Ok();
    const Scan scan = profile(image, params, roi, xAxis, i);
    const auto opening = edges(scan, sign, params.number("minContrast"));
    const auto closing = edges(scan, -sign, params.number("minContrast"));
    if (opening.empty() || closing.empty())
      continue;
    double first = opening.front().coordinate, second = closing.back().coordinate;
    bool pair = true;
    if (inner) {
      const double center = scan.begin + (scan.values.size() - 1) / 2.0;
      auto left = std::find_if(opening.rbegin(), opening.rend(),
                               [&](const Edge& e) { return e.coordinate < center; });
      auto right = std::find_if(closing.begin(), closing.end(),
                                [&](const Edge& e) { return e.coordinate > center; });
      pair = left != opening.rend() && right != closing.end();
      if (pair) {
        first = left->coordinate;
        second = right->coordinate;
      }
    }
    const double width = second - first;
    if (pair && width >= minWidth && width <= maxWidth) {
      widths.push_back(width);
      scans.push_back({{"scan", i},
                       {"across", scan.across * image.scale},
                       {"first", first * image.scale},
                       {"second", second * image.scale},
                       {"width", width * image.scale}});
    }
  }
  const bool found = !widths.empty() && static_cast<double>(widths.size()) / count >=
                                            params.number("minValidFraction");
  const double nan = std::numeric_limits<double>::quiet_NaN();
  double min = nan, max = nan, mean = nan;
  if (found) {
    const auto bounds = std::minmax_element(widths.begin(), widths.end());
    min = *bounds.first;
    max = *bounds.second;
    double sum = 0;
    for (double value : widths)
      sum += value;
    mean = sum / widths.size();
  }
  const char* message = found ? "" : "没有足够满足极性、对比度和宽度范围的成对边缘";
  outputs.set("minimum", Data::measurement(measured(min, message)));
  outputs.set("maximum", Data::measurement(measured(max, message)));
  outputs.set("mean", Data::measurement(measured(mean, message)));
  outputs.set("samples",
              Data::record(Record{"image.WidthSamples.v1",
                                  {{"valid", found},
                                   {"unit", "px"},
                                   {"coordinateSpace", "source"},
                                   {"scale", image.scale},
                                   {"axis", xAxis ? "x" : "y"},
                                   {"validFraction", static_cast<double>(widths.size()) / count},
                                   {"samples", std::move(scans)}}}));
  return Status::Ok();
}

Status positionsCompute(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                        ExecContext& ctx) {
  const Image& image = *inputs.get("image").asImage();
  if (!image.consistent() || image.channels != 1 || image.pixelCount() > 16u * 1024u * 1024u)
    return img::badInput("边缘投影需要不超过 16 MP 的单通道图像");
  const auto bounds = params.vec4("roi");
  for (float value : bounds)
    if (!std::isfinite(value))
      return img::badParam("ROI 坐标必须有限", "roi");
  if (bounds[2] <= bounds[0] || bounds[3] <= bounds[1])
    return img::badParam("ROI 必须具有正宽高", "roi");
  const int x0 = static_cast<int>(std::floor(std::clamp<double>(bounds[0], 0, image.width)));
  const int y0 = static_cast<int>(std::floor(std::clamp<double>(bounds[1], 0, image.height)));
  const int x1 = static_cast<int>(std::ceil(std::clamp<double>(bounds[2], 0, image.width)));
  const int y1 = static_cast<int>(std::ceil(std::clamp<double>(bounds[3], 0, image.height)));
  const bool xAxis = params.choice("axis") == "x";
  const int length = xAxis ? x1 - x0 : y1 - y0;
  const int crossLength = xAxis ? y1 - y0 : x1 - x0;
  if (length < 5 || crossLength < 1)
    return img::badParam("ROI 扫描长度至少 5 像素且横向非空", "roi");
  const double contrast = params.number("minContrast");
  if (!std::isfinite(contrast) || contrast <= 0)
    return img::badParam("最小梯度必须为正有限数", "minContrast");
  Scan scan;
  scan.begin = xAxis ? x0 : y0;
  scan.values.reserve(length);
  for (int i = 0; i < length; ++i) {
    if (ctx.cancelled())
      return Status::Ok();
    double sum = 0;
    int count = 0;
    for (int j = 0; j < crossLength; ++j) {
      const double v = xAxis ? image.at(x0 + i, y0 + j, 0) : image.at(x0 + j, y0 + i, 0);
      if (std::isfinite(v)) {
        sum += v;
        ++count;
      }
    }
    scan.values.push_back(count ? sum / count : std::numeric_limits<double>::quiet_NaN());
  }
  const bool reverse = params.choice("direction") == "negative";
  const std::string polarity = params.choice("polarity");
  const int sign = (polarity == "rising" ? 1 : polarity == "falling" ? -1 : 0) * (reverse ? -1 : 1);
  const std::string mode = params.choice("mode");
  auto candidates = edges(scan, sign, contrast);
  std::vector<Edge> selected;
  if (mode == "double") {
    const bool inner = params.choice("dimension") == "inner";
    const int firstSign = sign * (inner ? -1 : 1);
    auto first = edges(scan, firstSign, contrast), second = edges(scan, -firstSign, contrast);
    if (reverse) {
      std::reverse(first.begin(), first.end());
      std::reverse(second.begin(), second.end());
    }
    const double direction = reverse ? -1.0 : 1.0;
    if (!first.empty() && !second.empty()) {
      if (inner) {
        const double center = scan.begin + (scan.values.size() - 1) / 2.0;
        const auto left = std::find_if(first.rbegin(), first.rend(), [&](const Edge& e) {
          return direction * e.coordinate < direction * center;
        });
        const auto right = std::find_if(second.begin(), second.end(), [&](const Edge& e) {
          return direction * e.coordinate > direction * center;
        });
        if (left != first.rend() && right != second.end() &&
            direction * (right->coordinate - left->coordinate) > 0)
          selected = {*left, *right};
      } else if (direction * (second.back().coordinate - first.front().coordinate) > 0) {
        selected = {first.front(), second.back()};
      }
    }
  } else {
    if (reverse)
      std::reverse(candidates.begin(), candidates.end());
    const auto limit = static_cast<std::size_t>(mode == "single" ? 1 : params.integer("maxEdges"));
    if (candidates.size() > limit)
      candidates.resize(limit);
    selected = std::move(candidates);
  }
  nlohmann::json positions = nlohmann::json::array();
  for (const auto& edge : selected)
    positions.push_back({{"position", edge.coordinate * image.scale}, {"contrast", edge.contrast}});
  outputs.set("positions", Data::record(Record{"image.EdgePositions.v1",
                                               {{"unit", "px"},
                                                {"coordinateSpace", "source"},
                                                {"scale", image.scale},
                                                {"axis", xAxis ? "x" : "y"},
                                                {"count", positions.size()},
                                                {"positions", std::move(positions)}}}));
  return Status::Ok();
}

} // namespace

void registerImageCalipers(Registry& registry) {
  {
    OperatorDesc op;
    op.id = "image.line_caliper";
    op.version = "1.0.0";
    op.label = "直线卡尺";
    op.category = "图像/量测";
    op.doc =
        "矩形 ROI 等间隔扫描，中心差分极性边缘与抛物线亚像素定位，Huber 拟合后按容差筛选。无目标返回 edge_not_found，不构造假直线。";
    op.inputs = {img::grayIn()};
    op.outputs = {Port{"line", "Line2D", "拟合直线", "像素单位，含有限边缘范围。", true},
                  Port{"position", "Measurement", "平均边缘位置", "沿扫描轴的平均位置，px。", true},
                  recordOutput("image.CaliperSamples.v1")};
    op.params = commonParams();
    op.params.push_back(img::enumParam("direction", "扫描方向", "positive",
                                       {{"positive", "正方向", ""}, {"negative", "负方向", ""}},
                                       "边缘极性相对于此方向定义。"));
    op.params.push_back(
        img::enumParam("polarity", "边缘极性", "rising",
                       {{"rising", "暗到亮", ""}, {"falling", "亮到暗", ""}, {"any", "任意", ""}},
                       "沿正扫描轴定义。"));
    op.params.push_back(img::enumParam(
        "selection", "候选选择", "strongest",
        {{"strongest", "最强", ""}, {"first", "第一个", ""}, {"last", "最后一个", ""}}, ""));
    Param tolerance =
        img::floatParam("fitTolerance", "拟合容差", 2, "边缘点到拟合直线的距离阈值。");
    tolerance.unit = "px";
    op.params.push_back(tolerance);
    op.keywords = {"line edge caliper", "edge position", "亚像素边缘",
                   "直线边缘",          "卡尺测量",      "边缘位置"};
    op.capabilities = {true, false, true};
    op.compute = lineCompute;
    registry.addOperator(std::move(op));
  }
  {
    OperatorDesc op;
    op.id = "image.width_range";
    op.version = "1.0.0";
    op.label = "平行卡尺宽度";
    op.category = "图像/量测";
    op.doc =
        "矩形 ROI 沿 X/Y 扫描，外尺寸选择最外侧开/闭边对，内尺寸选择 ROI 中心两侧最近边对，输出采样宽度 min/max/mean。不是任意旋转形状的全局最窄宽度。";
    op.inputs = {img::grayIn()};
    op.outputs = {Port{"minimum", "Measurement", "最小宽度", "px，无目标为无效测量。", true},
                  Port{"maximum", "Measurement", "最大宽度", "px。", true},
                  Port{"mean", "Measurement", "平均宽度", "px。", true},
                  recordOutput("image.WidthSamples.v1")};
    op.params = commonParams();
    op.params.push_back(img::enumParam(
        "dimension", "量测模式", "outer",
        {{"outer", "外尺寸", "最外侧边对"}, {"inner", "内尺寸", "中心邻近边对"}}, ""));
    op.params.push_back(
        img::enumParam("polarity", "工件极性", "bright",
                       {{"bright", "亮条", "先上升后下降"}, {"dark", "暗条", "先下降后上升"}}, ""));
    Param min = img::floatParam("minWidth", "最小候选宽度", 1, "边对宽度闭区间下界。");
    Param max = img::floatParam("maxWidth", "最大候选宽度", 10000, "边对宽度闭区间上界。");
    min.unit = max.unit = "px";
    op.params.push_back(min);
    op.params.push_back(max);
    op.keywords = {"calculate width range",
                   "parallel caliper",
                   "最小宽度",
                   "最大宽度",
                   "条带量测",
                   "宽度范围"};
    op.capabilities = {true, false, true};
    op.compute = widthCompute;
    registry.addOperator(std::move(op));
  }
  {
    OperatorDesc op;
    op.id = "image.edge_positions";
    op.version = "1.0.0";
    op.label = "投影边缘定位";
    op.category = "图像/量测";
    op.doc =
        "全 ROI 沿横向均值投影后，按方向和极性选择单边、双边或多个梯度峰。空目标有效返回 count=0，不返回 -1 哨兵坐标。";
    op.inputs = {img::grayIn()};
    op.outputs = {
        withContract(Port{"positions", "Record", "边缘位置", "原图像素坐标的有界位置列表。", true},
                     {{"recordType", "image.EdgePositions.v1"}})};
    op.params = {
        roiParam(),
        img::enumParam("axis", "扫描轴", "x", {{"x", "X", ""}, {"y", "Y", ""}}, ""),
        img::enumParam("direction", "扫描方向", "positive",
                       {{"positive", "正方向", ""}, {"negative", "负方向", ""}}, ""),
        img::enumParam("mode", "模式", "single",
                       {{"single", "单边", ""}, {"double", "双边", ""}, {"multiple", "多边", ""}},
                       ""),
        img::enumParam("polarity", "边缘极性", "rising",
                       {{"rising", "暗到亮", ""}, {"falling", "亮到暗", ""}, {"any", "任意", ""}},
                       ""),
        img::enumParam("dimension", "双边模式", "outer",
                       {{"outer", "外尺寸", ""}, {"inner", "内尺寸", ""}}, ""),
        img::floatParam("minContrast", "最小梯度", 10, "原始强度单位的正阈值。"),
        img::intParam("maxEdges", "最大边数", 16, 1, 256, "多边模式按扫描顺序选择。")};
    op.keywords = {"calculate edge position", "projection gradient peaks", "边缘位置", "多边定位",
                   "投影卡尺"};
    op.capabilities = {true, false, true};
    op.compute = positionsCompute;
    registry.addOperator(std::move(op));
  }
}

} // namespace lyflow::ops
