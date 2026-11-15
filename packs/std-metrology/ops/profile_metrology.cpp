#include <cmath>
#include <limits>
#include <utility>

#include "lyflow/kernel/metrology.h"
#include "lyflow/operator.h"
#include "lyflow/registry.h"
#include "lyflow_profile/context.h"

namespace lyflow::packs::std_metrology {
namespace {
namespace metrology = kernel::metrology;

Status failure(std::string message, std::string port, std::string code = "bad_input") {
  return Status::Error(Phase::Execute, code, std::move(message), {}, std::move(port));
}
Param scalar(const char* name, const char* label, double value, double minimum, double maximum) {
  Param p;
  p.name = name;
  p.label = label;
  p.type = ParamType::Float;
  p.def = Value::number(value);
  p.min = minimum;
  p.max = maximum;
  p.unit = "m";
  return p;
}
struct FittedLine {
  metrology::SampledLine line;
  ProfileDescriptor descriptor;
  std::uint64_t count = 0;
  nlohmann::json context;
};

bool fittedLine(const Data& geometry, const Record* statistics, FittedLine& output,
                std::string& why) {
  const auto* line = geometry.asLine2D();
  if (!line || line->unit != Unit2D::Meter || !profile_contract::validLine(geometry, why)) {
    why = "量测需要米制Line2D";
    return false;
  }
  if (!statistics || statistics->type != "profile.LineFit.v1" || !statistics->data.is_object()) {
    why = "量测需要profile.LineFit.v1拟合统计";
    return false;
  }
  try {
    const auto& stats = statistics->data;
    if (!stats.at("schemaVersion").is_number_integer() || stats.at("schemaVersion") != 1 ||
        !stats.at("unit").is_string() || stats.at("unit") != "m" ||
        !stats.at("geometryHash").is_string() ||
        stats.at("geometryHash") != profile_contract::geometryHash(geometry)) {
      why = "拟合统计版本、米制单位或几何指纹不匹配";
      return false;
    }
    FittedLine result;
    if (!profile_contract::parse(stats.at("context"), result.descriptor, why))
      return false;
    if (stats.at("frame") != result.descriptor.frame ||
        stats.at("plane") != result.descriptor.plane || !stats.at("count").is_number_integer() ||
        stats.at("count").get<double>() < 3 ||
        stats.at("count").get<double>() > result.descriptor.sourceCount ||
        !stats.at("rms").is_number() || !std::isfinite(stats.at("rms").get<double>()) ||
        stats.at("rms").get<double>() < 0) {
      why = "拟合统计坐标声明、点数或残差非法";
      return false;
    }
    const auto& range = stats.at("abscissaRange");
    if (!range.is_array() || range.size() != 2 || !range[0].is_number() || !range[1].is_number() ||
        !std::isfinite(range[0].get<double>()) || !std::isfinite(range[1].get<double>()) ||
        range[0].get<double>() > range[1].get<double>()) {
      why = "拟合统计采样横坐标范围非法";
      return false;
    }
    result.line.center = {line->point[0], line->point[1]};
    result.line.direction = {line->dir[0], line->dir[1]};
    result.line.minimum = range[0].get<double>();
    result.line.maximum = range[1].get<double>();
    result.line.rms = stats.at("rms").get<double>();
    result.count = stats.at("count").get<std::uint64_t>();
    result.context = stats.at("context");
    output = std::move(result);
    why.clear();
    return true;
  } catch (const nlohmann::json::exception&) {
    why = "拟合统计字段缺失或类型非法";
    return false;
  }
}
Measurement measurement(double value) {
  Measurement result;
  result.value = value;
  result.ok = true;
  result.unit = "m";
  return result;
}
Status gapFlush(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  const double x = p.number("referenceX"), maximum = p.number("maxResidual"),
               extrapolation = p.number("maxExtrapolation");
  if (!std::isfinite(x) || !std::isfinite(maximum) || maximum <= 0 ||
      !std::isfinite(extrapolation) || extrapolation < 0)
    return Status::Error(Phase::Execute, "bad_param", "面差横坐标、残差或外推参数非法",
                         "referenceX");
  FittedLine left, right;
  std::string why;
  if (!fittedLine(in.get("left"), in.get("leftStatistics").asRecord(), left, why))
    return failure(why, "leftStatistics");
  if (!fittedLine(in.get("right"), in.get("rightStatistics").asRecord(), right, why))
    return failure(why, "rightStatistics");
  if (left.descriptor.frame != right.descriptor.frame ||
      left.descriptor.plane != right.descriptor.plane ||
      left.descriptor.handedness != right.descriptor.handedness)
    return failure("左右轮廓必须声明相同坐标帧、轴平面和手性", "rightStatistics");
  if (left.line.maximum >= right.line.minimum)
    return failure("左右实际采样范围必须按横坐标严格分离", "rightStatistics");
  if (ctx.cancelled())
    return Status::Ok();
  metrology::SampledGapFlush result;
  if (!metrology::sampledGapFlush(left.line, right.line, {x, maximum, extrapolation}, result, why))
    return failure(why, "left", "no_result");
  out.set("gap", Data::measurement(measurement(result.gap)));
  out.set("flush", Data::measurement(measurement(result.flush)));
  out.set("quality", Data::record(Record{"metrology.ProfileGapFlush.v1",
                                         {{"schemaVersion", 1},
                                          {"frame", left.descriptor.frame},
                                          {"plane", left.descriptor.plane},
                                          {"handedness", left.descriptor.handedness},
                                          {"unit", "m"},
                                          {"referenceX", x},
                                          {"leftCount", left.count},
                                          {"rightCount", right.count},
                                          {"leftRms", left.line.rms},
                                          {"rightRms", right.line.rms},
                                          {"leftContext", left.context},
                                          {"rightContext", right.context}}}));
  return Status::Ok();
}

Status signedPointLine(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  const auto& pointData = in.get("point");
  const auto& lineData = in.get("line");
  const auto* point = pointData.asPoint2D();
  const auto* line = lineData.asLine2D();
  if (!point || !line)
    return failure("需要Point2D与Line2D", "point");
  ProfileDescriptor pointDescriptor, lineDescriptor;
  std::string why;
  if (!profile_contract::geometryMatches(pointData, in.get("pointContext").asRecord(),
                                         pointDescriptor, why))
    return failure(why, "pointContext");
  if (!profile_contract::geometryMatches(lineData, in.get("lineContext").asRecord(), lineDescriptor,
                                         why))
    return failure(why, "lineContext");
  if (pointDescriptor.frame != lineDescriptor.frame ||
      pointDescriptor.plane != lineDescriptor.plane ||
      pointDescriptor.handedness != lineDescriptor.handedness)
    return failure("点线必须声明相同坐标帧、轴平面和手性", "lineContext");
  const auto reference = p.vec2("positiveNormal");
  if (!std::isfinite(reference[0]) || !std::isfinite(reference[1]) ||
      (reference[0] == 0 && reference[1] == 0))
    return Status::Error(Phase::Execute, "bad_param", "正法向参考必须为有限非零向量",
                         "positiveNormal");
  if (ctx.cancelled())
    return Status::Ok();
  metrology::SignedPointLine result;
  if (!metrology::signedPointLine({point->p[0], point->p[1]}, {line->point[0], line->point[1]},
                                  {line->dir[0], line->dir[1]}, {reference[0], reference[1]},
                                  metrology::DirectionPolicy::Normalize, result, why))
    return failure(why, "line", "no_result");
  Point2D foot;
  for (int i = 0; i < 2; ++i) {
    if (std::abs(result.foot[i]) > std::numeric_limits<float>::max())
      return failure("垂足超出Point2D的float32范围", "line", "no_result");
    foot.p[i] = static_cast<float>(result.foot[i]);
  }
  foot.unit = Unit2D::Meter;
  out.set("distance", Data::measurement(measurement(result.distance)));
  out.set("foot", Data::point2d(foot));
  return Status::Ok();
}
}

void registerProfileMetrology(Registry& registry) {
  OperatorDesc op;
  op.id = "metrology.profile_gap_flush";
  op.version = "1.0.0";
  op.label = "轮廓拟合结果间隙与面差";
  op.category = "计量/轮廓";
  op.keywords = {"profile", "gap", "flush", "间隙", "面差"};
  op.capabilities = {true, false, true};
  op.doc = "接收已拟合的米制直线及其profile.LineFit.v1统计，核对几何指纹与声明坐标语义。"
           "不重新拟合，不推断标定正确性；允许不同源身份，但坐标帧、轴平面及手性必须相同。"
           "间隙为右采样最小横坐标减左采样最大横坐标，面差为共同referenceX处右减左。";
  op.inputs = {{"left", "Line2D", "左拟合直线", "米制直线，与leftStatistics配套。", true},
               {"right", "Line2D", "右拟合直线", "米制直线，与rightStatistics配套。", true},
               {"leftStatistics", "Record", "左拟合统计", "profile.LineFit.v1。", true},
               {"rightStatistics", "Record", "右拟合统计", "profile.LineFit.v1。", true}};
  op.outputs = {{"gap", "Measurement", "间隙", "采样边缘横坐标间距，m。", true},
                {"flush", "Measurement", "有符号面差", "共同横坐标的右减左纵差，m。", true},
                {"quality", "Record", "质量", "metrology.ProfileGapFlush.v1。", true}};
  op.params = {scalar("referenceX", "面差横坐标", 0, -1e6, 1e6),
               scalar("maxResidual", "最大正交RMS", 0.001, 1e-12, 1e3),
               scalar("maxExtrapolation", "最大外推距离", 0.05, 0, 1e3)};
  op.compute = &gapFlush;
  registry.addOperator(std::move(op));

  op = {};
  op.id = "metrology.signed_point_line";
  op.version = "1.0.0";
  op.label = "有符号点线距离";
  op.category = "计量/二维几何";
  op.keywords = {"signed", "point", "line", "distance", "垂足", "有符号距离"};
  op.capabilities = {true, false, true};
  op.doc = "核对点线的米制单位、几何内容指纹及声明坐标帧、轴平面和手性；允许不同采样源。"
           "将直线方向归一化，以positiveNormal指定分析平面中的距离正法向，参考不能与法向正交。"
           "输出米制有符号垂距和垂足，不推断业务方向、物理标定或判定阈值。";
  op.inputs = {{"point", "Point2D", "点", "米制点。", true},
               {"line", "Line2D", "直线", "米制无限直线；端点不限制垂足。", true},
               {"pointContext", "Record", "点坐标上下文", "profile.Geometry.v1。", true},
               {"lineContext", "Record", "直线坐标上下文", "profile.Geometry.v1。", true}};
  op.outputs = {{"distance", "Measurement", "有符号距离", "按指定正法向的米制垂距。", true},
                {"foot", "Point2D", "垂足", "米制Point2D。", true}};
  Param reference;
  reference.name = "positiveNormal";
  reference.label = "距离正法向参考";
  reference.type = ParamType::Vec2f;
  reference.def = Value::vec({0, 1});
  op.params = {reference};
  op.compute = &signedPointLine;
  registry.addOperator(std::move(op));
}
} // namespace lyflow::packs::std_metrology
