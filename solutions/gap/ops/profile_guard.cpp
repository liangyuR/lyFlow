#include "profile_migration.h"
#include "profile_guard.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

#include "lyflow/profile.h"
#include "lyflow_profile/context.h"

namespace lyflow::packs::gap::profile_scene {
namespace {
Status bad(const std::string& message, const char* parameter = "", const char* code = "bad_param") {
  return Status::Error(Phase::Execute, code, message, parameter);
}
Status cancelled() { return bad("相机一致性检查已取消", "", "cancelled"); }
double median(std::vector<double>& values) {
  std::sort(values.begin(), values.end());
  const auto n = values.size();
  return n % 2 == 1 ? values[n / 2] : .5 * (values[n / 2 - 1] + values[n / 2]);
}
std::string mmText(double value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.3f", value);
  return buffer;
}
struct Working {
  std::vector<float> x;
  std::vector<float> height;
};
bool working(const Profile& profile, Working& result, ExecContext& ctx, std::string& why) {
  Profile metric;
  if (!convertProfileUnit(profile, "m", metric, why, [&ctx] { return ctx.cancelled(); }))
    return false;
  const int x = metric.descriptor.plane == "YZ" ? 1 : 0;
  const int h = metric.descriptor.plane == "XY" ? 1 : 2;
  result.x.reserve(metric.pointCount());
  result.height.reserve(metric.pointCount());
  for (std::size_t i = 0; i < metric.pointCount(); ++i) {
    if ((i & 1023U) == 0 && ctx.cancelled())
      return false;
    if (!metric.validity[i])
      continue;
    result.x.push_back(metric.cloud->xyz[3 * i + x]);
    result.height.push_back(metric.cloud->xyz[3 * i + h]);
  }
  return true;
}
bool medianAt(const Working& profile, float x, float half, double& height, ExecContext& ctx) {
  std::vector<double> values;
  for (std::size_t i = 0; i < profile.x.size(); ++i) {
    if ((i & 4095U) == 0 && ctx.cancelled())
      return false;
    if (profile.x[i] > x - half && profile.x[i] < x + half)
      values.push_back(profile.height[i]);
  }
  if (values.empty())
    return false;
  height = median(values);
  return true;
}
Param number(const char* name, double value, const char* unit) {
  Param parameter;
  parameter.name = name;
  parameter.label = name;
  parameter.type = ParamType::Float;
  parameter.unit = unit;
  parameter.def = Value::number(value);
  return parameter;
}
}
Status guardProfiles(const Data& primary, const Data& secondary, const CameraGuardOptions& options,
                     GuardResult& out, ExecContext& ctx, const Data* box,
                     const Record* boxContext) {
  const auto* a = primary.asProfile();
  const auto* b = secondary.asProfile();
  std::string why;
  if (!profile_contract::sameSpace(a->descriptor, b->descriptor))
    return bad("两路轮廓必须声明相同坐标帧、分析平面和手性", "", "space_mismatch");
  const auto& action = options.onDisagree;
  const double metricStep = options.sampleStepMm / 1000.0;
  const double metricHalf = options.halfWindowMm / 1000.0;
  if (options.maxDeltaMm < 0)
    return bad("高度差上限必须为非负毫米值", "maxDeltaMm");
  if (metricStep <= 0 || metricStep > std::numeric_limits<float>::max())
    return bad("采样步长必须为可表示的正米制float", "sampleStep");
  if (metricHalf <= 0 || metricHalf > std::numeric_limits<float>::max())
    return bad("窗口半宽必须为可表示的正米制float", "halfWindow");
  const float step = static_cast<float>(metricStep);
  const float half = static_cast<float>(metricHalf);
  if (step <= 0 || half <= 0)
    return bad("步长或窗口半宽换算到float后下溢", "sampleStep");
  if (static_cast<std::uint64_t>(options.minSamples) > options.maxSamples)
    return bad("最少有效采样不能超过采样预算", "minSamples");
  if ((box != nullptr) != (boxContext != nullptr))
    return bad("ROI与上下文须成对提供", "", "bad_input");
  const Box2D* roi = nullptr;
  if (box) {
    if (!profile_contract::geometryInSpace(*box, boxContext, a->descriptor, why))
      return bad(why, "", "space_mismatch");
    roi = box->asBox2D();
  }
  if (ctx.cancelled())
    return cancelled();
  Record quality{
      "gap.CameraGuard.v3",
      {{"schemaVersion", 3}, {"onDisagree", action}, {"maxDeltaMm", options.maxDeltaMm}}};
  const auto emit = [&](const Data& left, const Data& right, const char* taken) {
    GuardResult result;
    result.primary = left;
    result.secondary = right;
    result.quality = quality;
    result.quality.data["taken"] = taken;
    result.quality.data["inputPrimarySourceId"] = a->descriptor.sourceId;
    result.quality.data["inputSecondarySourceId"] = b->descriptor.sourceId;
    result.quality.data["primarySourceId"] = left.asProfile()->descriptor.sourceId;
    result.quality.data["secondarySourceId"] = right.asProfile()->descriptor.sourceId;
    if (ctx.cancelled())
      return cancelled();
    out = std::move(result);
    return Status::Ok();
  };
  if (action == "off") {
    quality.data["evaluated"] = false;
    quality.data["exceeded"] = false;
    return emit(primary, secondary, "both");
  }
  Working left, right;
  if (!working(*a, left, ctx, why) || !working(*b, right, ctx, why))
    return ctx.cancelled() ? cancelled() : bad(why, "", "bad_input");
  std::vector<double> differences;
  if (!left.x.empty() && !right.x.empty()) {
    const float lo = std::max(*std::min_element(left.x.begin(), left.x.end()),
                              *std::min_element(right.x.begin(), right.x.end()));
    const float hi = std::min(*std::max_element(left.x.begin(), left.x.end()),
                              *std::max_element(right.x.begin(), right.x.end()));
    const float from = roi ? std::max(lo, roi->min[0]) : lo;
    const float to = roi ? std::min(hi, roi->max[0]) : hi;
    std::size_t attempted = 0;
    for (float x = from; x < to;) {
      if (ctx.cancelled())
        return cancelled();
      if (attempted++ >= options.maxSamples)
        return bad("相机一致性采样超过明确预算", "sampleStep", "budget_exceeded");
      const float next = x + step;
      if (!std::isfinite(next) || next <= x)
        return bad("采样步长在当前坐标量级无法推进有限float", "sampleStep", "sample_step_stalled");
      double ha = 0, hb = 0;
      if (medianAt(left, x, half, ha, ctx) && medianAt(right, x, half, hb, ctx))
        differences.push_back((ha - hb) * 1000.0);
      if (ctx.cancelled())
        return cancelled();
      x = next;
    }
  }
  quality.data["sampleCount"] = differences.size();
  if (differences.size() < static_cast<std::size_t>(options.minSamples)) {
    quality.data["evaluated"] = false;
    quality.data["exceeded"] = false;
    quality.data["reason"] = "insufficient_overlap";
    ctx.log(LogLevel::Info,
            "camera_guard: 重叠采样只有 " + std::to_string(differences.size()) + " 个，判不了");
    return emit(primary, secondary, "both");
  }
  std::vector<double> magnitude;
  magnitude.reserve(differences.size());
  for (const double difference : differences)
    magnitude.push_back(std::fabs(difference));
  const double delta = median(magnitude);
  auto signedCopy = differences;
  const double signedDelta = median(signedCopy);
  const bool exceeded = delta > options.maxDeltaMm;
  quality.data["evaluated"] = true;
  quality.data["exceeded"] = exceeded;
  quality.data["deltaMedianMm"] = delta;
  quality.data["signedMedianMm"] = signedDelta;
  const std::string summary = "两台在重叠段上的高度差中位 " + mmText(delta) + " mm（带符号 " +
                              mmText(signedDelta) + "，" + std::to_string(differences.size()) +
                              " 个采样），上限 " + mmText(options.maxDeltaMm) + " mm";
  ctx.log(exceeded ? LogLevel::Warn : LogLevel::Info, "camera_guard: " + summary);
  if (!exceeded || action == "record")
    return emit(primary, secondary, "both");
  if (action == "keepPrimary")
    return emit(primary, primary, "primary");
  if (action == "keepSecondary")
    return emit(secondary, secondary, "secondary");
  return bad("两台相机看到的不是同一个面：" + summary, "", "camera_disagree");
}
void registerProfileCameraGuard(Registry& registry) {
  OperatorDesc op;
  op.id = "gap.camera_guard";
  op.version = "3.0.0";
  declareGapGraphUpgrade(op);
  op.migrations.push_back({2, nullptr, &profile_contract::keepInputs});
  op.label = "双相机轮廓一致性校验";
  op.category = "间隙/质量";
  op.keywords = {"camera guard", "profile", "consistency", "双相机", "轮廓", "一致性"};
  op.inputs = {{"primary", "Profile", "Primary", "第一逻辑角色所选的真实物理源。", true},
               {"secondary", "Profile", "Secondary", "第二逻辑角色所选的真实物理源。", true},
               {"box", "Box2D", "ROI", "可选米制横向范围，与上下文成对。", false},
               {"boxContext", "Record", "ROI上下文", "profile.Geometry.v3，与轮廓同空间。", false}};
  op.outputs = {
      {"primary", "Profile", "Primary", "共享所选原Profile，保留原槽和真实来源。", true},
      {"secondary", "Profile", "Secondary", "共享所选原Profile，保留原槽和真实来源。", true},
      {"quality", "Record", "质量", "实际评估/分歧/选择与来源，不代表生产就绪。", true}};
  op.outputs[2].contract = {{"recordType", "gap.CameraGuard.v3"}};
  Param action;
  action.name = "onDisagree";
  action.label = "分歧处置";
  action.type = ParamType::Enum;
  action.def = Value::text("record");
  for (const auto* value : {"off", "record", "keepPrimary", "keepSecondary", "fail"})
    action.options.push_back({value, value});
  Param minimum;
  minimum.name = "minSamples";
  minimum.label = "最少有效采样";
  minimum.type = ParamType::Int;
  minimum.def = Value::integer(20);
  minimum.min = 1;
  minimum.max = 1000000;
  Param budget = minimum;
  budget.name = "maxSamples";
  budget.label = "采样预算";
  budget.def = Value::integer(1000000);
  budget.advanced = true;
  op.params = {action,
               number("maxDeltaMm", .5, "mm"),
               number("sampleStep", .5, "mm"),
               number("halfWindow", .25, "mm"),
               minimum,
               budget};
  op.capabilities.cancellable = true;
  op.capabilities.deterministic = true;
  op.capabilities.previewable = true;
  op.doc =
      "按明确Profile平面读取高度，在两路重叠范围内保留float步进、开边界窗口和中位统计；只读有效槽。"
      "选择单一路时两输出共享该真实Profile，不改名伪造另一物理相机。";
  op.compute = [](const Inputs& inputs, const ParamView& parameters, Outputs& outputs,
                  ExecContext& ctx) {
    CameraGuardOptions options;
    options.onDisagree = parameters.choice("onDisagree");
    options.maxDeltaMm = parameters.number("maxDeltaMm");
    options.sampleStepMm = parameters.number("sampleStep");
    options.halfWindowMm = parameters.number("halfWindow");
    options.minSamples = parameters.integer("minSamples");
    options.maxSamples = static_cast<std::size_t>(parameters.integer("maxSamples"));
    GuardResult result;
    const auto* box = inputs.has("box") ? &inputs.get("box") : nullptr;
    const auto* context = inputs.has("boxContext") ? inputs.get("boxContext").asRecord() : nullptr;
    if (auto status = guardProfiles(inputs.get("primary"), inputs.get("secondary"), options, result,
                                    ctx, box, context);
        !status.ok)
      return status;
    if (ctx.cancelled())
      return cancelled();
    outputs.set("primary", result.primary);
    outputs.set("secondary", result.secondary);
    outputs.set("quality", Data::record(std::move(result.quality)));
    return Status::Ok();
  };
  registry.addOperator(std::move(op));
}
}
