#include <cmath>
#include <set>
#include <utility>

#include "lyflow/kernel/profile_dimensions.h"
#include "lyflow/operator.h"
#include "lyflow/registry.h"
#include "lyflow_profile/context.h"

namespace lyflow::packs::std_metrology {
namespace {
namespace dimensions = kernel::profile_dimensions;

Status failure(const std::string& why, const char* port = "", const char* code = "bad_input") {
  return Status::Error(Phase::Execute, code, why, {}, port);
}

Data contextOf(const ProfileDescriptor& space) {
  return Data::record(profile_contract::geometryContext(space));
}

Data measured(double valueMm, bool valid, const std::string& unit,
              const std::string& message = {}) {
  Measurement value;
  value.value = unit == "mm" ? valueMm : valueMm * 0.001;
  value.ok = valid && std::isfinite(value.value);
  value.unit = unit;
  value.message = message;
  return Data::measurement(std::move(value));
}

Data quality(const char* type, const ProfileDescriptor& space, nlohmann::json values) {
  values["schemaVersion"] = 2;
  values["frame"] = space.frame;
  values["plane"] = space.plane;
  values["handedness"] = space.handedness;
  return Data::record(Record{type, std::move(values)});
}

Status flushPointLine(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                      ExecContext& ctx) {
  if (ctx.cancelled())
    return Status::Ok();
  const auto* base = inputs.get("baseLine").asLine2D();
  const auto* point = inputs.get("refPoint").asPoint2D();
  ProfileDescriptor space;
  auto status = profile_contract::geometrySpace(
      inputs.get("baseLine"), inputs.get("baseLineContext"), space, "baseLineContext");
  if (!status.ok)
    return status;
  status = profile_contract::geometryInSpace(inputs.get("refPoint"), inputs.get("refPointContext"),
                                             space, "refPointContext");
  if (!status.ok)
    return status;
  dimensions::FlushOptions options;
  options.signedDistance = params.flag("signed");
  options.sign = params.choice("signConvention") == "canonical_axis"
                     ? dimensions::FlushSign::CanonicalAxis
                     : dimensions::FlushSign::PositiveNormal;
  options.positiveNormal = params.vec2("positiveNormal");
  options.gain = params.number("gain");
  options.offsetMm = params.number("offsetMm");
  dimensions::FlushResult result;
  std::string why;
  if (!dimensions::flushPointLine(*base, *point, options, result, why))
    return failure(why, "baseLine", "invalid_geometry");
  const auto context = contextOf(space);
  auto diagnostic = quality("metrology.FlushPointLine.v2", space,
                            {{"valueMm", result.valueMm},
                             {"signed", options.signedDistance},
                             {"signConvention", params.choice("signConvention")},
                             {"gain", options.gain},
                             {"offsetMm", options.offsetMm}});
  if (ctx.cancelled())
    return Status::Ok();
  outputs.set("value", measured(result.valueMm, true, params.choice("outputUnit")));
  outputs.set("foot", Data::point2d(result.foot));
  outputs.set("segment", Data::line2d(result.segment));
  outputs.set("baseLine", Data::line2d(result.baseLine));
  outputs.set("footContext", context);
  outputs.set("segmentContext", context);
  outputs.set("baseLineContext", context);
  outputs.set("quality", std::move(diagnostic));
  return Status::Ok();
}

Status circleGap(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                 ExecContext& ctx) {
  if (ctx.cancelled())
    return Status::Ok();
  const auto* left = inputs.get("left").asCircle2D();
  const auto* right = inputs.get("right").asCircle2D();
  ProfileDescriptor space;
  auto status = profile_contract::geometrySpace(inputs.get("left"), inputs.get("leftContext"),
                                                space, "leftContext");
  if (!status.ok)
    return status;
  status = profile_contract::geometryInSpace(inputs.get("right"), inputs.get("rightContext"), space,
                                             "rightContext");
  if (!status.ok)
    return status;
  dimensions::CircleGapOptions options;
  options.definition = params.choice("definition") == "datum_tangents"
                           ? dimensions::CircleGapDefinition::DatumTangents
                           : dimensions::CircleGapDefinition::CenterLine;
  options.offsetMm = params.number("offsetMm");
  const Line2D* base = nullptr;
  if (options.definition == dimensions::CircleGapDefinition::DatumTangents ||
      inputs.has("baseLine") || inputs.has("baseLineContext")) {
    base = inputs.get("baseLine").asLine2D();
    if (!base)
      return failure("基准线与其上下文必须成对提供", "baseLine");
    status = profile_contract::geometryInSpace(
        inputs.get("baseLine"), inputs.get("baseLineContext"), space, "baseLineContext");
    if (!status.ok)
      return status;
  }
  dimensions::CircleGapResult result;
  std::string why;
  if (!dimensions::circleGap(*left, *right, base, options, result, why))
    return failure(why, "left", "invalid_geometry");
  auto diagnostic =
      quality("metrology.CircleGap.v2", space,
              {{"valid", result.valid},
               {"crossed", result.crossed},
               {"valueMm",
                std::isfinite(result.valueMm) ? nlohmann::json(result.valueMm) : nlohmann::json()},
               {"definition", params.choice("definition")},
               {"offsetMm", options.offsetMm}});
  if (ctx.cancelled())
    return Status::Ok();
  outputs.set("value", measured(result.valueMm, result.valid, params.choice("outputUnit"),
                                result.crossed ? "两侧交叉，间隙无效" : ""));
  outputs.set("segment", Data::line2d(result.segment));
  outputs.set("segmentContext", contextOf(space));
  outputs.set("quality", std::move(diagnostic));
  return Status::Ok();
}

Status pointOffset(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                   ExecContext& ctx) {
  if (ctx.cancelled())
    return Status::Ok();
  const auto* a = inputs.get("a").asPoint2D();
  const auto* b = inputs.get("b").asPoint2D();
  ProfileDescriptor space;
  auto status =
      profile_contract::geometrySpace(inputs.get("a"), inputs.get("aContext"), space, "aContext");
  if (!status.ok)
    return status;
  status =
      profile_contract::geometryInSpace(inputs.get("b"), inputs.get("bContext"), space, "bContext");
  if (!status.ok)
    return status;
  dimensions::PointOffsetOptions options;
  options.absDx = params.flag("absDx");
  options.absDy = params.flag("absDy");
  options.gainDx = params.number("gainDx");
  options.gainDy = params.number("gainDy");
  options.dxOffsetMm = params.number("dxOffsetMm");
  options.dyOffsetMm = params.number("dyOffsetMm");
  dimensions::PointOffsetResult result;
  std::string why;
  if (!dimensions::pointOffset(*a, *b, options, result, why))
    return failure(why, "a", "invalid_geometry");
  auto diagnostic = quality("metrology.PointOffset.v2", space,
                            {{"dxMm", result.dxMm},
                             {"dyMm", result.dyMm},
                             {"distanceMm", result.distanceMm},
                             {"gainDx", options.gainDx},
                             {"gainDy", options.gainDy},
                             {"dxOffsetMm", options.dxOffsetMm},
                             {"dyOffsetMm", options.dyOffsetMm},
                             {"absDx", options.absDx},
                             {"absDy", options.absDy}});
  if (ctx.cancelled())
    return Status::Ok();
  const auto& unit = params.choice("outputUnit");
  outputs.set("dx", measured(result.dxMm, true, unit));
  outputs.set("dy", measured(result.dyMm, true, unit));
  outputs.set("distance", measured(result.distanceMm, true, unit));
  outputs.set("segment", Data::line2d(result.segment));
  outputs.set("segmentContext", contextOf(space));
  outputs.set("quality", std::move(diagnostic));
  return Status::Ok();
}

Param scalar(const char* name, const char* label, double value, const char* unit = "") {
  Param param;
  param.name = name;
  param.label = label;
  param.type = ParamType::Float;
  param.def = Value::number(value);
  param.unit = unit;
  return param;
}
Param boolean(const char* name, const char* label, bool value) {
  Param param;
  param.name = name;
  param.label = label;
  param.type = ParamType::Bool;
  param.def = Value::boolean(value);
  return param;
}
Param choice(const char* name, const char* label, const char* value,
             std::initializer_list<EnumOption> options) {
  Param param;
  param.name = name;
  param.label = label;
  param.type = ParamType::Enum;
  param.def = Value::text(value);
  param.options = options;
  return param;
}
Param outputUnit() {
  return choice("outputUnit", "结果单位", "m", {{"m", "米", ""}, {"mm", "毫米", ""}});
}
Port contextPort(const char* name, const char* label, bool required = true) {
  return {name, "Record", label, "profile.Geometry.v3坐标空间。", required};
}
} // namespace

void registerProfileDimensions(Registry& registry) {
  OperatorDesc op;
  op.id = "metrology.flush_point_line";
  op.version = "2.0.1";
  op.migrations = {{1, nullptr, &profile_contract::keepInputs}};
  op.label = "点线面差测量";
  op.category = "计量/轮廓";
  op.keywords = {"flush", "point line", "signed distance", "面差", "垂足", "段差"};
  op.capabilities = {true, false, true};
  op.doc = "验证米制点线的坐标帧、分析平面和手性。显式正法向参考决定符号；"
           "canonical_axis采用方向朝+x、竖直朝+y后的右法向。关闭signed时先取绝对距离，"
           "再乘无量纲gain并加offsetMm；输出单位独立选择。输出垂足、显示线段与并入垂足的基准段。";
  op.inputs = {{"baseLine", "Line2D", "基准线", "米制无限直线；可带待扩展的端点。", true},
               {"refPoint", "Point2D", "参考点", "米制点。", true},
               contextPort("baseLineContext", "基准线上下文"),
               contextPort("refPointContext", "参考点上下文")};
  op.outputs = {{"value", "Measurement", "面差", "方向与补偿明确的面差，无公差判定。", true},
                {"foot", "Point2D", "垂足", "米制垂足。", true},
                {"segment", "Line2D", "显示线段", "从垂足指向参考点，米。", true},
                {"baseLine", "Line2D", "扩展基准段", "将垂足并入原基准段，米。", true},
                contextPort("footContext", "垂足上下文"),
                contextPort("segmentContext", "线段上下文"),
                contextPort("baseLineContext", "扩展基准段上下文"),
                {"quality", "Record", "测量信息", "metrology.FlushPointLine.v2。", true}};
  Param normal;
  normal.name = "positiveNormal";
  normal.label = "正法向参考";
  normal.type = ParamType::Vec2f;
  normal.def = Value::vec({0, -1});
  op.params = {
      boolean("signed", "有符号面差", true),
      choice("signConvention", "符号定义", "positive_normal",
             {{"positive_normal", "显式正法向", ""}, {"canonical_axis", "规范方向右法向", ""}}),
      normal,
      scalar("gain", "读数增益", 1),
      scalar("offsetMm", "毫米偏置", 0, "mm"),
      outputUnit()};
  op.compute = &flushPointLine;
  registry.addOperator(std::move(op));

  op = {};
  op.id = "metrology.circle_gap";
  op.version = "2.0.0";
  op.migrations = {{1, nullptr, &profile_contract::keepInputs}};
  op.label = "圆边界间隙测量";
  op.category = "计量/轮廓";
  op.keywords = {"circle gap", "tangent separation", "clearance", "圆间隙", "切线间隙"};
  op.capabilities = {true, false, true};
  op.doc = "center_line沿圆心连线测量两圆边界间距；左侧端点x大于右侧时输出无效NaN。"
           "datum_tangents沿基准段端点方向测量两圆切线距离，需要基准段与上下文，"
           "切线交叉返回invalid_geometry。left必须为分析平面横轴较小一侧；"
           "圆/基准段坐标为米，offsetMm为毫米，输出单位另行选择，不增加公差判定。";
  op.inputs = {{"left", "Circle2D", "左圆", "米制正半径圆。", true},
               {"right", "Circle2D", "右圆", "米制正半径圆。", true},
               contextPort("leftContext", "左圆上下文"),
               contextPort("rightContext", "右圆上下文"),
               {"baseLine", "Line2D", "基准段", "切线定义必须提供非退化米制线段。", false},
               contextPort("baseLineContext", "基准段上下文", false)};
  op.outputs = {{"value", "Measurement", "间隙", "无公差判定；交叉无效保持NaN。", true},
                {"segment", "Line2D", "显示线段", "测量方向上的米制线段。", true},
                contextPort("segmentContext", "线段上下文"),
                {"quality", "Record", "测量信息", "metrology.CircleGap.v2。", true}};
  op.params = {choice("definition", "测量定义", "center_line",
                      {{"center_line", "圆心连线上的边界距离", ""},
                       {"datum_tangents", "沿基准方向的切线距离", ""}}),
               scalar("offsetMm", "毫米偏置", 0, "mm"), outputUnit()};
  op.validate = [](const ParamView& params, const std::set<std::string>& connected) {
    std::vector<Issue> issues;
    if (params.choice("definition") == "datum_tangents")
      for (const char* port : {"baseLine", "baseLineContext"})
        if (!connected.count(port))
          issues.push_back(
              Issue::error("missing_input", "切线定义需要基准段及其上下文", "definition", port));
    return issues;
  };
  op.compute = &circleGap;
  registry.addOperator(std::move(op));

  op = {};
  op.id = "metrology.point_offset";
  op.version = "2.0.0";
  op.migrations = {{1, nullptr, &profile_contract::keepInputs}};
  op.label = "二维位移分量测量";
  op.category = "计量/轮廓";
  op.keywords = {"point offset", "displacement", "dx", "dy", "位移分解", "两点位移"};
  op.capabilities = {true, false, true};
  op.doc = "验证同帧、同分析平面和手性的米制点，沿平面两坐标轴分解b-a。"
           "每分量先乘无量纲增益，再按absDx/absDy取绝对值并加毫米偏置；"
           "两点欧氏距离独立于增益和偏置。支持同一点零位移，不输出公差或业务合格判定。";
  op.inputs = {{"a", "Point2D", "起点", "米制Point2D。", true},
               {"b", "Point2D", "终点", "米制Point2D。", true},
               contextPort("aContext", "起点上下文"),
               contextPort("bContext", "终点上下文")};
  op.outputs = {{"dx", "Measurement", "横向位移", "分析平面第一轴的位移。", true},
                {"dy", "Measurement", "纵向位移", "分析平面第二轴的位移。", true},
                {"distance", "Measurement", "两点距离", "未加补偿的欧氏距离。", true},
                {"segment", "Line2D", "显示线段", "从a指向b的米制线段。", true},
                contextPort("segmentContext", "线段上下文"),
                {"quality", "Record", "测量信息", "metrology.PointOffset.v2。", true}};
  op.params = {boolean("absDx", "横向取绝对值", false),
               boolean("absDy", "纵向取绝对值", false),
               scalar("gainDx", "横向增益", 1),
               scalar("gainDy", "纵向增益", 1),
               scalar("dxOffsetMm", "横向毫米偏置", 0, "mm"),
               scalar("dyOffsetMm", "纵向毫米偏置", 0, "mm"),
               outputUnit()};
  op.compute = &pointOffset;
  registry.addOperator(std::move(op));
}

} // namespace lyflow::packs::std_metrology
