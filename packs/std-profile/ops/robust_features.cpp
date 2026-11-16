#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <utility>
#include <vector>

#include "lyflow/kernel/profile_features.h"
#include "lyflow/operator.h"
#include "lyflow/profile.h"
#include "lyflow/registry.h"
#include "lyflow_profile/context.h"

namespace lyflow::packs::std_profile {
namespace {
namespace robust = kernel::profile_robust;
namespace features = kernel::profile_features;
using Json = nlohmann::json;

Status bad(std::string message, std::string port = "profile", std::string code = "bad_input") {
  return Status::Error(Phase::Execute, std::move(code), std::move(message), {}, std::move(port));
}
Status parameter(std::string message, std::string name) {
  return Status::Error(Phase::Execute, "bad_param", std::move(message), std::move(name));
}
Status cancelled() { return bad("轮廓拟合已取消", "profile", "cancelled"); }
Json finiteOrNull(double value) { return std::isfinite(value) ? Json(value) : Json(); }
Param scalar(const char* name, const char* label, double value, const char* unit = "m") {
  Param out;
  out.name = name;
  out.label = label;
  out.type = ParamType::Float;
  out.def = Value::number(value);
  out.unit = unit;
  return out;
}
Param count(const char* name, const char* label, std::int64_t value, double minimum) {
  Param out;
  out.name = name;
  out.label = label;
  out.type = ParamType::Int;
  out.def = Value::integer(value);
  out.min = minimum;
  return out;
}
Param choice(const char* name, const char* label, const char* value,
             std::initializer_list<EnumOption> options) {
  Param out;
  out.name = name;
  out.label = label;
  out.type = ParamType::Enum;
  out.def = Value::text(value);
  out.options = options;
  return out;
}

struct Working {
  std::vector<robust::Point> points;
  std::size_t validCount = 0;
};
bool working(const Profile& input, Working& out, ExecContext& ctx, std::string& why) {
  Profile metric;
  if (!convertProfileUnit(input, "m", metric, why, [&ctx] { return ctx.cancelled(); }))
    return false;
  const int a = input.descriptor.plane == "YZ" ? 1 : 0;
  const int b = input.descriptor.plane == "XY" ? 1 : 2;
  const float nan = std::numeric_limits<float>::quiet_NaN();
  out.points.reserve(metric.pointCount());
  for (std::size_t i = 0; i < metric.pointCount(); ++i) {
    if ((i & 1023U) == 0 && ctx.cancelled())
      return false;
    if (metric.validity[i]) {
      out.points.push_back({metric.cloud->xyz[3 * i + a], metric.cloud->xyz[3 * i + b], 0});
      ++out.validCount;
    } else {
      out.points.push_back({nan, nan, nan});
    }
  }
  return true;
}

Status samePhysical(const Inputs& inputs, const char* port, const char* contextPort,
                    const ProfileDescriptor& input, Json& sources) {
  ProfileDescriptor descriptor;
  Json geometrySources;
  std::string why;
  if (!profile_contract::geometryMatchesPhysical(
          inputs.get(port), inputs.get(contextPort).asRecord(), descriptor, why, &geometrySources))
    return bad(why, contextPort);
  if (!profile_contract::sameSpace(descriptor, input))
    return bad("拟合几何与轮廓的坐标帧、分析平面或手性不一致", contextPort);
  for (const auto& source : geometrySources) {
    if (std::find(sources.begin(), sources.end(), source) == sources.end())
      sources.push_back(source);
  }
  if (sources.size() > 64)
    return bad("拟合几何来源不能超过64项", contextPort);
  return Status::Ok();
}

struct Selection {
  Data inliers;
  Data samples;
  Data map;
  std::size_t uniqueCount = 0;
};
Selection selection(const Data& source, const Json& context, const std::vector<int>& samples,
                    ExecContext& ctx) {
  const auto* input = source.asProfile();
  std::vector<std::int32_t> weighted;
  weighted.reserve(samples.size());
  for (std::size_t i = 0; i < samples.size(); ++i) {
    if ((i & 1023U) == 0 && ctx.cancelled())
      throw robust::Cancelled();
    const int slot = samples[i];
    if (slot < 0 || static_cast<std::size_t>(slot) >= input->pointCount() || !input->validity[slot])
      throw std::invalid_argument("拟合返回了无效轮廓槽");
    weighted.push_back(static_cast<std::int32_t>(slot));
  }
  auto unique = weighted;
  std::sort(unique.begin(), unique.end());
  unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
  Selection out;
  out.uniqueCount = unique.size();
  out.inliers = Data::indices(Indices{input->cloud->id, std::move(unique)});
  out.samples = Data::indices(Indices{input->cloud->id, std::move(weighted)});
  out.map = Data::record(Record{"profile.Selection.v1",
                                {{"schemaVersion", 1},
                                 {"context", context},
                                 {"sourceCloudId", std::to_string(input->cloud->id)},
                                 {"indexSpace", "profile_slots"},
                                 {"indexCount", out.uniqueCount},
                                 {"weightedInlierCount", samples.size()},
                                 {"sourceIndexMapping", "profile.sourceIndices"}}});
  return out;
}

Status lineParameters(const ParamView& params, features::LineOptions& options) {
  const double threshold = params.number("distanceThreshold");
  if (!std::isfinite(threshold) || threshold <= 0 ||
      threshold > std::numeric_limits<float>::max() || static_cast<float>(threshold) == 0)
    return parameter("内点距离门限必须为可表示的正米制值", "distanceThreshold");
  options.fit.distThresh = static_cast<float>(threshold);
  const auto segment = params.integer("segmentPoints"), minimum = params.integer("minInliers");
  if (segment < 0)
    return parameter("局部重拟合样本数不能为负", "segmentPoints");
  if (minimum < 0)
    return parameter("最少内点数不能为负", "minInliers");
  options.segmentPoints = static_cast<std::size_t>(segment);
  options.minInliers = static_cast<std::size_t>(minimum);
  const auto type = params.choice("lineType");
  if (type != "fit" && type != "vertical" && type != "horizontal")
    return parameter("直线拟合方向模式非法", "lineType");
  options.type = type == "fit"        ? features::LineType::Free
                 : type == "vertical" ? features::LineType::Vertical
                                      : features::LineType::Horizontal;
  const auto mode = params.choice("dirMode");
  if (mode != "free" && mode != "band" && mode != "fixed")
    return parameter("方向约束模式非法", "dirMode");
  options.directionMode = mode == "free"   ? features::DirectionMode::Free
                          : mode == "band" ? features::DirectionMode::Band
                                           : features::DirectionMode::Fixed;
  options.nominalDeg = params.number("dirNominalDeg");
  options.toleranceDeg = params.number("dirTolDeg");
  if (!std::isfinite(options.nominalDeg))
    return parameter("相对标称角必须有限", "dirNominalDeg");
  if (!std::isfinite(options.toleranceDeg) || options.toleranceDeg < 0)
    return parameter("方向带宽必须为非负有限角度", "dirTolDeg");
  const auto ends = params.choice("endpoints");
  if (ends != "roi_intersection" && ends != "inlier_ends")
    return parameter("端点模式非法", "endpoints");
  options.endpoints = ends == "inlier_ends" ? features::Endpoints::InlierEnds
                                            : features::Endpoints::RoiIntersection;
  return Status::Ok();
}

Status fitLine(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext& ctx) {
  if (ctx.cancelled())
    return cancelled();
  const auto& source = inputs.get("profile");
  const auto* input = source.asProfile();
  std::string why;
  if (!input || !validateProfile(*input, why))
    return bad(why.empty() ? "需要有效Profile" : why);
  features::LineOptions options;
  if (auto status = lineParameters(params, options); !status.ok)
    return status;
  if (!inputs.get("box").asBox2D())
    return bad("需要米制拟合ROI", "box");
  if (!inputs.get("toward").asPoint2D())
    return bad("需要米制局部选择参考点", "toward");
  if (inputs.has("refLine") != inputs.has("refLineContext") ||
      (options.directionMode != features::DirectionMode::Free && !inputs.has("refLine")))
    return bad("方向约束需要成对连接refLine与refLineContext", "refLineContext");
  try {
    const auto context = profile_contract::context(source);
    Json sources = Json::array({context});
    if (auto status = samePhysical(inputs, "box", "boxContext", input->descriptor, sources);
        !status.ok)
      return status;
    if (auto status = samePhysical(inputs, "toward", "towardContext", input->descriptor, sources);
        !status.ok)
      return status;
    if (inputs.has("refLine")) {
      if (!inputs.get("refLine").asLine2D())
        return bad("需要方向参考直线", "refLine");
      if (auto status =
              samePhysical(inputs, "refLine", "refLineContext", input->descriptor, sources);
          !status.ok)
        return status;
      const auto* reference = inputs.get("refLine").asLine2D();
      options.hasReference = true;
      options.referenceDirection = {reference->dir[0], reference->dir[1]};
    }
    const auto* box = inputs.get("box").asBox2D();
    options.roi = {{box->min[0], box->min[1]}, {box->max[0], box->max[1]}};
    const auto* toward = inputs.get("toward").asPoint2D();
    options.toward = {toward->p[0], toward->p[1]};
    Working work;
    if (!working(*input, work, ctx, why))
      return ctx.cancelled() ? cancelled() : bad(why);
    features::LineFeature fit;
    features::LineFailure failure;
    if (!features::fitLine({work.points.data(), work.points.size(), false}, options, fit, &failure,
                           [&ctx] { return ctx.cancelled(); }))
      return bad(failure.stage == features::LineFailureStage::MinInliers
                     ? "拟合内点未达到minInliers"
                     : "轮廓直线拟合失败或有效点不足",
                 "profile",
                 failure.stage == features::LineFailureStage::MinInliers ? "insufficient_points"
                                                                         : "line_fit_failed");
    Line2D line;
    std::copy(fit.point.begin(), fit.point.end(), line.point);
    std::copy(fit.direction.begin(), fit.direction.end(), line.dir);
    line.hasSegment = fit.hasSegment;
    std::copy(fit.start.begin(), fit.start.end(), line.start);
    std::copy(fit.end.begin(), fit.end.end(), line.end);
    const auto geometry = Data::line2d(line);
    Point2D inner;
    inner.p[0] = work.points[static_cast<std::size_t>(fit.innerSlot)].x;
    inner.p[1] = work.points[static_cast<std::size_t>(fit.innerSlot)].y;
    const auto point = Data::point2d(inner);
    const auto lineContext = Data::record(Record{
        "profile.Geometry.v2", profile_contract::geometryContextV2FromSources(geometry, sources)});
    const auto pointContext = Data::record(Record{
        "profile.Geometry.v2", profile_contract::geometryContextV2FromSources(point, sources)});
    auto selected = selection(source, context, fit.inliers, ctx);
    const auto quality = Data::record(
        Record{"profile.LineFitRobust.v1",
               {{"schemaVersion", 1},
                {"context", context},
                {"geometryHash", profile_contract::geometryHashV2(geometry)},
                {"unit", "m"},
                {"method", "ransac_dual_trace_segment_direction"},
                {"inputSlotCount", input->pointCount()},
                {"validSlotCount", work.validCount},
                {"uniqueInlierCount", selected.uniqueCount},
                {"weightedInlierCount", fit.inliers.size()},
                {"uniqueInlierRatio", static_cast<double>(selected.uniqueCount) / work.validCount},
                {"residualAggregation", "final_inlier_multiset"},
                {"weightedRmsResidual", fit.quality.rmsResidualMm / 1000.0},
                {"maxResidual", fit.quality.maxResidualMm / 1000.0},
                {"segmentApplied", fit.segmentApplied},
                {"directionPinned", fit.directionPinned}}});
    if (ctx.cancelled())
      return cancelled();
    outputs.set("line", geometry);
    outputs.set("lineContext", lineContext);
    outputs.set("innerEnd", point);
    outputs.set("pointContext", pointContext);
    outputs.set("profile", source);
    outputs.set("inliers", std::move(selected.inliers));
    outputs.set("weightedInliers", std::move(selected.samples));
    outputs.set("indexMap", std::move(selected.map));
    outputs.set("quality", quality);
    return Status::Ok();
  } catch (const robust::Cancelled&) {
    return cancelled();
  } catch (const std::invalid_argument& error) {
    return bad(error.what());
  }
}

Status circleParameters(const ParamView& params, robust::Circle2DFitOptions& options) {
  const double threshold = params.number("distanceThreshold");
  if (!std::isfinite(threshold) || threshold <= 0 ||
      threshold > std::numeric_limits<float>::max() || static_cast<float>(threshold) == 0)
    return parameter("内点距离门限必须为可表示的正米制值", "distanceThreshold");
  options.distThresh = static_cast<float>(threshold);
  options.minRadius = params.number("minRadius");
  options.maxRadius = params.number("maxRadius");
  if (!std::isfinite(options.minRadius) || options.minRadius <= 0)
    return parameter("最小半径必须为正有限米制值", "minRadius");
  if (!std::isfinite(options.maxRadius) || options.maxRadius < options.minRadius ||
      options.maxRadius > std::numeric_limits<float>::max())
    return parameter("最大半径必须有限且不小于最小半径", "maxRadius");
  const auto mode = params.choice("radiusMode");
  if (mode != "free" && mode != "fixed" && mode != "prior")
    return parameter("半径拟合模式非法", "radiusMode");
  if (mode == "fixed" &&
      (!std::isfinite(params.number("fixedRadius")) || params.number("fixedRadius") <= 0 ||
       params.number("fixedRadius") > std::numeric_limits<float>::max()))
    return parameter("固定半径必须为正有限米制值", "fixedRadius");
  if (mode == "prior") {
    options.radiusPrior = params.number("radiusPrior");
    options.radiusPriorSigma = params.number("radiusPriorSigma");
    if (!std::isfinite(options.radiusPrior) || options.radiusPrior <= 0 ||
        options.radiusPrior > std::numeric_limits<float>::max())
      return parameter("先验半径必须为正有限米制值", "radiusPrior");
    if (!std::isfinite(options.radiusPriorSigma) || options.radiusPriorSigma <= 0)
      return parameter("半径先验标准差必须为正有限米制值", "radiusPriorSigma");
  }
  if (params.integer("minInliers") < 3)
    return parameter("圆拟合至少需要三个内点", "minInliers");
  return Status::Ok();
}
const char* outcome(robust::Circle2DRadiusOutcome value) {
  switch (value) {
  case robust::Circle2DRadiusOutcome::Prior:
    return "prior";
  case robust::Circle2DRadiusOutcome::ClampedMin:
    return "clamped_min";
  case robust::Circle2DRadiusOutcome::ClampedMax:
    return "clamped_max";
  case robust::Circle2DRadiusOutcome::Unrefined:
    return "unrefined";
  default:
    return "free";
  }
}
Status fitCircle(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                 ExecContext& ctx) {
  if (ctx.cancelled())
    return cancelled();
  const auto& source = inputs.get("profile");
  const auto* input = source.asProfile();
  std::string why;
  if (!input || !validateProfile(*input, why))
    return bad(why.empty() ? "需要有效Profile" : why);
  robust::Circle2DFitOptions options;
  if (auto status = circleParameters(params, options); !status.ok)
    return status;
  try {
    Working work;
    if (!working(*input, work, ctx, why))
      return ctx.cancelled() ? cancelled() : bad(why);
    robust::CircleFit fit;
    const robust::Samples samples{work.points.data(), work.points.size(), false};
    const auto mode = params.choice("radiusMode");
    const auto check = [&ctx] { return ctx.cancelled(); };
    const bool ok = mode == "fixed"
                        ? robust::fitCircleFixedRadius(samples, params.number("fixedRadius"), fit,
                                                       options, check)
                        : robust::fitCircle(samples, fit, options, check);
    if (!ok)
      return bad("轮廓圆拟合失败或有效点不足", "profile", "circle_fit_failed");
    if (fit.inliers.size() < static_cast<std::size_t>(params.integer("minInliers")))
      return bad("圆拟合内点未达到minInliers", "profile", "insufficient_points");
    Circle2D circle;
    circle.center[0] = fit.coefficients[0];
    circle.center[1] = fit.coefficients[1];
    circle.radius = fit.coefficients[2];
    const auto geometry = Data::circle2d(circle);
    const auto context = profile_contract::context(source);
    const auto geometryContext = Data::record(
        Record{"profile.Geometry.v2",
               profile_contract::geometryContextV2FromSources(geometry, Json::array({context}))});
    auto selected = selection(source, context, fit.inliers, ctx);
    double sum = 0;
    double worst = 0;
    for (std::size_t i = 0; i < fit.inliers.size(); ++i) {
      if ((i & 1023U) == 0 && ctx.cancelled())
        return cancelled();
      const auto& point = work.points[static_cast<std::size_t>(fit.inliers[i])];
      const double dx = point.x - fit.coefficients[0];
      const double dy = point.y - fit.coefficients[1];
      const double residual = std::fabs(std::hypot(dx, dy) - fit.coefficients[2]);
      sum += residual * residual;
      worst = std::max(worst, residual);
    }
    const auto quality = Data::record(
        Record{"profile.CircleFitRobust.v1",
               {{"schemaVersion", 1},
                {"context", context},
                {"geometryHash", profile_contract::geometryHashV2(geometry)},
                {"unit", "m"},
                {"method", "ransac_circle2d_lm"},
                {"radiusMode", mode},
                {"radiusOutcome", mode == "fixed" ? "fixed" : outcome(fit.report.radius)},
                {"seedRadiusOutcome", outcome(fit.report.radius)},
                {"unclampedRadius", finiteOrNull(fit.report.unclampedRadius)},
                {"inputSlotCount", input->pointCount()},
                {"validSlotCount", work.validCount},
                {"uniqueInlierCount", selected.uniqueCount},
                {"weightedInlierCount", fit.inliers.size()},
                {"uniqueInlierRatio", static_cast<double>(selected.uniqueCount) / work.validCount},
                {"rmsResidual", std::sqrt(sum / fit.inliers.size())},
                {"maxResidual", worst}}});
    if (ctx.cancelled())
      return cancelled();
    outputs.set("circle", geometry);
    outputs.set("circleContext", geometryContext);
    outputs.set("profile", source);
    outputs.set("inliers", std::move(selected.inliers));
    outputs.set("weightedInliers", std::move(selected.samples));
    outputs.set("indexMap", std::move(selected.map));
    outputs.set("quality", quality);
    return Status::Ok();
  } catch (const robust::Cancelled&) {
    return cancelled();
  } catch (const std::invalid_argument& error) {
    return bad(error.what());
  }
}
std::vector<Port> commonOutputs() {
  return {{"profile", "Profile", "原轮廓", "共享只读输入轮廓，保留完整槽和sourceIndices。", true},
          {"inliers", "Indices", "唯一内点槽", "原轮廓cloud.id上的有序唯一槽索引。", true},
          {"weightedInliers", "Indices", "最终内点权重",
           "原轮廓槽的最终内点与质量多重权重；不是RANSAC内部求解抽样轨迹。", true},
          {"indexMap", "Record", "索引来源声明",
           "profile.Selection.v1；完整映射读取profile.sourceIndices。", true},
          {"quality", "Record", "拟合质量", "区分唯一观测内点数与最终内点/质量权重数。", true}};
}
} // namespace

void registerRobustProfileFeatures(Registry& registry) {
  OperatorDesc op;
  op.id = "profile.fit_line_robust";
  op.version = "1.0.0";
  op.label = "轮廓鲁棒直线特征拟合";
  op.category = "轮廓/拟合";
  op.keywords = {"profile", "robust line", "RANSAC",   "direction",
                 "轮廓",    "直线",        "鲁棒拟合", "方向约束"};
  op.doc =
      "按Profile声明将m/mm转换到米，并将选定分析轴映射到XY；无效槽在工作副本中标NaN。"
      "采用公共RANSAC双迹线选择、toward近段阈值1/3重拟合及free/band/fixed方向约束。"
      "固定方向回退保留旧多重样本权重；weightedInliers是最终内点和质量权重，不是RANSAC内部求解抽样轨迹。"
      "inliers唯一化只用于观察索引，不会重拟合模型。"
      "box/toward/refLine必须与Profile处于相同物理空间，并提供绑定几何内容的上下文。";
  op.capabilities = {true, false, true};
  op.inputs = {{"profile", "Profile", "轮廓", "包含帧、轴、m/mm单位、有效性与原索引。", true},
               {"box", "Box2D", "拟合ROI", "米制端点ROI。输入轮廓需已按需要选区。", true},
               {"boxContext", "Record", "ROI坐标声明", "profile.Geometry.v2。", true},
               {"toward", "Point2D", "局部选择参考点", "米制，按沿线投影选择邻近样本。", true},
               {"towardContext", "Record", "参考点坐标声明", "profile.Geometry.v1或v2。", true},
               {"refLine", "Line2D", "方向参考线", "band/fixed时必须提供。", false},
               {"refLineContext", "Record", "参考线坐标声明", "必须与refLine成对连接。", false}};
  op.outputs = commonOutputs();
  op.outputs.insert(op.outputs.end(),
                    {{"line", "Line2D", "拟合直线", "米制输出。", true},
                     {"lineContext", "Record", "直线坐标上下文", "profile.Geometry.v2。", true},
                     {"innerEnd", "Point2D", "局部参考端点", "选中原槽的米制分析平面坐标。", true},
                     {"pointContext", "Record", "端点坐标上下文", "profile.Geometry.v2。", true}});
  op.params = {scalar("distanceThreshold", "内点距离门限", 0.0001),
               count("segmentPoints", "近端重拟合样本数", 500, 0),
               count("minInliers", "最少最终内点权重", 0, 0),
               choice("lineType", "初始方向", "fit",
                      {{"fit", "自由"}, {"horizontal", "水平"}, {"vertical", "竖直"}}),
               choice("endpoints", "端点口径", "roi_intersection",
                      {{"roi_intersection", "ROI交点"}, {"inlier_ends", "内点端点"}}),
               choice("dirMode", "方向约束", "free",
                      {{"free", "自由"}, {"band", "偏离带宽时固定"}, {"fixed", "固定相对方向"}}),
               scalar("dirNominalDeg", "相对标称角", 0, "°"),
               scalar("dirTolDeg", "方向带宽", 12, "°")};
  op.params[1].doc = "0表示使用全部拟合内点，不进行近端选段。";
  op.params[2].doc =
      "0关闭附加内点计数门禁；底层直线拟合仍至少需要两个有效点。固定方向回退计数为拟合多重样本权重。";
  op.compute = &fitLine;
  op.validate = [](const ParamView& params, const std::set<std::string>& connected) {
    features::LineOptions options;
    const auto status = lineParameters(params, options);
    if (!status.ok)
      return std::vector<Issue>{
          Issue::error(status.code, status.message, status.paramPath, status.portName)};
    if (connected.count("refLine") != connected.count("refLineContext") ||
        (options.directionMode != features::DirectionMode::Free && !connected.count("refLine")))
      return std::vector<Issue>{Issue::error(
          "bad_input", "方向约束需要成对连接refLine与refLineContext", "dirMode", "refLineContext")};
    return std::vector<Issue>{};
  };
  registry.addOperator(std::move(op));
  op = {};
  op.id = "profile.fit_circle_robust";
  op.version = "1.0.0";
  op.label = "轮廓鲁棒圆拟合";
  op.category = "轮廓/拟合";
  op.keywords = {"profile", "circle", "RANSAC",   "LM",      "radius prior",
                 "轮廓",    "圆",     "固定半径", "半径先验"};
  op.doc = "按Profile声明转换到米制分析平面，使用公共固定种子Circle2D RANSAC与LM。"
           "free、固定半径和软半径先验为显式独立模式；细化越界保留原半径边界钉住重定圆心策略。"
           "输入不排序、不改写；无效槽不参与计算。原轮廓和原槽索引共同提供完整来源映射。";
  op.capabilities = {true, false, true};
  op.inputs = {{"profile", "Profile", "轮廓", "包含m/mm单位、分析轴、有效性与源槽映射。", true}};
  op.outputs = commonOutputs();
  op.outputs.insert(op.outputs.end(),
                    {{"circle", "Circle2D", "拟合圆", "米制圆心与半径。", true},
                     {"circleContext", "Record", "圆坐标上下文", "profile.Geometry.v2。", true}});
  op.params = {scalar("distanceThreshold", "内点距离门限", 0.00003),
               scalar("minRadius", "最小半径", 0.0005),
               scalar("maxRadius", "最大半径", 0.01),
               choice("radiusMode", "半径模式", "free",
                      {{"free", "自由半径"}, {"fixed", "固定半径"}, {"prior", "软半径先验"}}),
               scalar("fixedRadius", "固定半径", 0.003),
               scalar("radiusPrior", "先验半径", 0.003),
               scalar("radiusPriorSigma", "先验标准差", 0.0003),
               count("minInliers", "最少内点数", 3, 3)};
  op.params[4].visibleWhen.param = "radiusMode";
  op.params[4].visibleWhen.eq = Value::text("fixed");
  for (const auto index : {5, 6}) {
    op.params[index].visibleWhen.param = "radiusMode";
    op.params[index].visibleWhen.eq = Value::text("prior");
  }
  op.compute = &fitCircle;
  op.validate = [](const ParamView& params, const std::set<std::string>&) {
    robust::Circle2DFitOptions options;
    const auto status = circleParameters(params, options);
    if (status.ok)
      return std::vector<Issue>{};
    return std::vector<Issue>{
        Issue::error(status.code, status.message, status.paramPath, status.portName)};
  };
  registry.addOperator(std::move(op));
}
} // namespace lyflow::packs::std_profile
