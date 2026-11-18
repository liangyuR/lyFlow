#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

#include "lyflow/op_params.h"
#include "lyflow/operator.h"
#include "lyflow/profile.h"
#include "lyflow/registry.h"
#include "lyflow_profile/context.h"

namespace lyflow::packs::std_profile {
namespace {
Status bad(const std::string& message, const char* param, const char* code = "bad_param") {
  return Status::Error(Phase::Execute, code, message, param);
}
Status cancelled() { return bad("轮廓重叠比较已取消", "", "cancelled"); }
double median(std::vector<double>& values) {
  std::sort(values.begin(), values.end());
  const auto n = values.size();
  return n % 2 == 1 ? values[n / 2] : .5 * (values[n / 2 - 1] + values[n / 2]);
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

Status compareOverlap(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                      ExecContext& ctx) {
  const auto* a = inputs.get("primary").asProfile();
  const auto* b = inputs.get("secondary").asProfile();
  if (!profile_contract::sameSpace(a->descriptor, b->descriptor))
    return bad("两条轮廓必须处于相同坐标帧、分析平面和手性", "", "space_mismatch");
  const auto budget = static_cast<std::size_t>(params.integer("maxSamples"));
  const auto minimum = static_cast<std::size_t>(params.integer("minSamples"));
  if (minimum > budget)
    return bad("最少有效采样不能超过采样预算", "minSamples");
  const auto stepValue = params.number("sampleStep"), halfValue = params.number("halfWindow");
  if (stepValue > std::numeric_limits<float>::max() || !(static_cast<float>(stepValue) > 0))
    return bad("采样步长必须为可表示的正米制float", "sampleStep");
  if (halfValue > std::numeric_limits<float>::max() || !(static_cast<float>(halfValue) > 0))
    return bad("窗口半宽必须为可表示的正米制float", "halfWindow");
  const auto step = static_cast<float>(stepValue), half = static_cast<float>(halfValue);
  if (inputs.has("box") != inputs.has("boxContext"))
    return op_params::badInput("ROI 与上下文须成对提供", "boxContext");
  std::string why;
  const Box2D* roi = nullptr;
  if (inputs.has("box")) {
    if (!profile_contract::geometryInSpace(inputs.get("box"), inputs.get("boxContext").asRecord(),
                                           a->descriptor, why))
      return Status::Error(Phase::Execute, "space_mismatch", why, {}, "boxContext");
    roi = inputs.get("box").asBox2D();
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
      if (attempted++ >= budget)
        return bad("相机一致性采样超过明确预算", "sampleStep", "budget_exceeded");
      const float next = x + step;
      if (!std::isfinite(next) || next <= x)
        return bad("采样步长在当前坐标量级无法推进有限float", "sampleStep", "sample_step_stalled");
      double ha = 0, hb = 0;
      if (medianAt(left, x, half, ha, ctx) && medianAt(right, x, half, hb, ctx))
        differences.push_back(ha - hb);
      if (ctx.cancelled())
        return cancelled();
      x = next;
    }
  }
  Record observation{"profile.OverlapComparison.v1",
                     {{"unit", "m"},
                      {"evaluated", differences.size() >= minimum},
                      {"sampleCount", differences.size()},
                      {"primarySourceId", a->descriptor.sourceId},
                      {"secondarySourceId", b->descriptor.sourceId}}};
  if (differences.size() < minimum) {
    observation.data["reason"] = "insufficient_overlap";
  } else {
    observation.data["signedMedian"] = median(differences);
    for (auto& difference : differences)
      difference = std::fabs(difference);
    observation.data["absoluteMedian"] = median(differences);
  }
  if (ctx.cancelled())
    return cancelled();
  outputs.set("observation", Data::record(std::move(observation)));
  return Status::Ok();
}
}

void registerCompareOverlap(Registry& r) {
  using namespace op_params;
  OperatorDesc op;
  op.id = "profile.compare_overlap";
  op.version = "1.0.0";
  op.label = "轮廓重叠区高度比较";
  op.category = "轮廓/分析";
  op.doc =
      "在两条轮廓的共享横向区间取样。按分析平面计算每站的局部高度中位数，只统计两路均有有效槽的站。";
  op.inputs = {{"primary", "Profile", "第一轮廓", "高度差的被减轮廓。", true},
               {"secondary", "Profile", "第二轮廓", "高度差的减数轮廓，与第一轮廓同空间。", true},
               {"box", "Box2D", "横向范围", "可选米制范围。", false},
               withContract({"boxContext", "Record", "范围上下文", "与范围成对。", false},
                            {{"recordType", "profile.Geometry.v3"}})};
  op.outputs = {withContract(
      {"observation", "Record", "重叠观测", "有效采样数、带符号与绝对高度差的米制中位数。", true},
      {{"recordType", "profile.OverlapComparison.v1"}})};
  auto step = floatParam("sampleStep", "采样步长", .0005, "横向等距取样的间隔。");
  step.unit = "m";
  step.min = 0;
  auto window = floatParam("halfWindow", "窗口半宽", .00025, "不含左右边界的局部统计半宽。");
  window.unit = "m";
  window.min = 0;
  auto budget = intParam("maxSamples", "采样预算", 1000000, 1, 1000000, "采样循环的最大站数。");
  budget.advanced = true;
  op.params = {step, window,
               intParam("minSamples", "最少有效采样", 20, 1, 1000000, "有效站数不足时输出未评估。"),
               budget};
  op.capabilities = {true, true, true};
  op.compute = compareOverlap;
  r.addOperator(std::move(op));
}
}
