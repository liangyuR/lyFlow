#include "profile_migration.h"
#include "profile_circles.h"

#include <unordered_map>
#include <limits>
#include <set>

#include "lyflow/profile.h"
#include "lyflow_profile/context.h"

namespace lyflow::packs::gap::fine {
std::vector<Issue> validateFitGapCircles(const ParamView&, const std::set<std::string>&);
}
namespace lyflow::packs::gap::profile_scene {
namespace {
Status bad(const std::string& message, const char* port = "merged",
           const char* code = "bad_input") {
  return Status::Error(Phase::Execute, code, message, {}, port);
}
Status cancelled() { return bad("间隙圆拟合已取消", "merged", "cancelled"); }
bool present(const Data& data) { return data.kind() != Data::Kind::None; }
Status physical(const Data& geometry, const Data& context, const ProfileDescriptor& expected,
                const char* port) {
  std::string why;
  if (!profile_contract::geometryInSpace(geometry, context.asRecord(), expected, why))
    return bad(why, port);
  return Status::Ok();
}
Status parameterValuesValid(const ParamView& parameters) {
  for (const auto* name : {"distThresh", "retryDistance"}) {
    const double value = parameters.number(name) / 1000.0;
    if (value < 0 || (std::string(name) == "distThresh" && value == 0) ||
        value > std::numeric_limits<float>::max() || (value > 0 && static_cast<float>(value) == 0))
      return Status::Error(Phase::Execute, "bad_param", "拟合门限必须可表示为米制float", name);
  }
  for (const auto* side : {"left", "right"}) {
    const std::string prefix = side;
    const auto count = parameters.integer(prefix + "MinInliers");
    if (count < 0 || count > std::numeric_limits<std::int32_t>::max())
      return Status::Error(Phase::Execute, "bad_param", "最少内点数超出范围",
                           prefix + "MinInliers");
    const double minimum = parameters.number(prefix + "RadiusMin");
    const double maximum = parameters.number(prefix + "RadiusMax");
    if (minimum <= 0 || maximum / 1000.0 > std::numeric_limits<float>::max())
      return Status::Error(Phase::Execute, "bad_param", "半径范围必须为可表示的正米制范围",
                           prefix + "RadiusMin");
  }

  return Status::Ok();
}
bool working(const Profile& input, PointCloud& result, ExecContext& ctx, std::string& why) {
  Profile metric;
  if (!convertProfileUnit(input, "m", metric, why, [&ctx] { return ctx.cancelled(); }))
    return false;
  const auto& cloud = *metric.cloud;
  result = cloud;
  PointCloud identity;
  result.id = identity.id;
  const int a = input.descriptor.plane == "YZ" ? 1 : 0;
  const int b = input.descriptor.plane == "XY" ? 1 : 2;
  const float nan = std::numeric_limits<float>::quiet_NaN();
  for (std::size_t i = 0; i < input.pointCount(); ++i) {
    if ((i & 1023U) == 0 && ctx.cancelled())
      return false;
    result.xyz[3 * i] = metric.validity[i] ? cloud.xyz[3 * i + a] : nan;
    result.xyz[3 * i + 1] = metric.validity[i] ? cloud.xyz[3 * i + b] : nan;
    result.xyz[3 * i + 2] = metric.validity[i] ? 0 : nan;
  }
  return true;
}
}
std::vector<Issue> validateCirclePairParams(const ParamView& parameters,
                                            const std::set<std::string>& connected) {
  if (const auto status = parameterValuesValid(parameters); !status.ok)
    return {Issue::error(status.code, status.message, status.paramPath, status.portName)};
  auto issues = fine::validateFitGapCircles(parameters, connected);
  for (const auto* line : {"refLine", "refLineRight"}) {
    const auto context = std::string(line) + "Context";
    if (connected.count(line) != connected.count(context))
      issues.push_back(
          Issue::error("bad_input", "圆约束参考线与其严格几何上下文必须成对连接", {}, context));
  }
  return issues;
}
Status fitProfileGapCircles(const CirclePairInputs& inputs, const ParamView& parameters,
                            CirclePairResult& out, ExecContext& ctx) {
  if (ctx.cancelled())
    return cancelled();
  const std::array<const Data*, 3> sources{&inputs.merged, &inputs.primary, &inputs.secondary};
  std::string why;
  for (std::size_t i = 0; i < sources.size(); ++i) {
    const auto* profile = sources[i]->asProfile();
    if (profile->pointCount() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
      return bad("输入轮廓超过原槽索引范围", "merged", "budget_exceeded");
    if (i && !profile_contract::sameSpace(sources[0]->asProfile()->descriptor, profile->descriptor))
      return bad("三路轮廓必须在同一明确物理空间");
  }
  const auto& space = inputs.merged.asProfile()->descriptor;
  for (std::size_t i = 0; i < 2; ++i) {
    if (auto status = physical(inputs.boxes[i], inputs.boxContexts[i], space,
                               i ? "boxRightContext" : "boxLeftContext");
        !status.ok)
      return status;
  }
  const std::array<const Data*, 2> lines{&inputs.refLine, &inputs.refLineRight};
  const std::array<const Data*, 2> lineContexts{&inputs.refLineContext,
                                                &inputs.refLineRightContext};
  for (std::size_t i = 0; i < 2; ++i) {
    if (present(*lines[i]) != present(*lineContexts[i]))
      return bad("参考线与上下文须成对提供", "refLineContext");
    if (present(*lines[i]))
      if (auto status = physical(*lines[i], *lineContexts[i], space,
                                 i ? "refLineRightContext" : "refLineContext");
          !status.ok)
        return status;
  }
  std::unordered_map<std::string, Data> cloudInputs;
  const std::array<const char*, 3> names{"merged", "primary", "secondary"};
  for (std::size_t i = 0; i < 3; ++i) {
    PointCloud cloud;
    if (!working(*sources[i]->asProfile(), cloud, ctx, why))
      return ctx.cancelled() ? cancelled() : bad(why);
    cloudInputs.emplace(names[i], Data::cloud(std::move(cloud)));
  }
  cloudInputs.emplace("boxLeft", inputs.boxes[0]);
  cloudInputs.emplace("boxRight", inputs.boxes[1]);
  if (present(inputs.refLine))
    cloudInputs.emplace("refLine", inputs.refLine);
  if (present(inputs.refLineRight))
    cloudInputs.emplace("refLineRight", inputs.refLineRight);
  std::unordered_map<std::string, Data> legacyResults;
  Outputs pending(legacyResults);
  CirclePairTrace trace;
  if (auto status =
          detail::fitCirclePairClouds(Inputs(cloudInputs), parameters, pending, ctx, &trace);
      !status.ok)
    return status;
  CirclePairResult result;
  result.quality = *legacyResults.at("quality").asRecord();
  result.quality.type = "gap.CircleFitPair.v3";
  result.quality.data["schemaVersion"] = 3;
  const auto context = Data::record(profile_contract::geometryContext(space));
  const std::array<const char*, 2> sideNames{"left", "right"};
  for (std::size_t side = 0; side < 2; ++side) {
    if (ctx.cancelled())
      return cancelled();
    const auto sourceIndex = trace.sides[side].source == CircleSource::Primary     ? 1U
                             : trace.sides[side].source == CircleSource::Secondary ? 2U
                                                                                   : 0U;
    const auto* profile = sources[sourceIndex]->asProfile();
    const auto& slots = trace.sides[side].slots;
    Profile selected = *profile;
    selected.validity.clear();
    selected.sourceIndices.clear();
    for (const auto slot : slots) {
      selected.validity.push_back(profile->validity[slot]);
      selected.sourceIndices.push_back(profile->sourceIndices[slot]);
    }
    selected.cloud = std::make_shared<const PointCloud>(profile->cloud->select(slots));
    result.profiles[side] = Data::profile(std::move(selected));
    result.circles[side] = legacyResults.at(sideNames[side]);
    result.contexts[side] = context;
    const auto* inliers = legacyResults.at(side == 0 ? "leftInliers" : "rightInliers").asIndices();
    result.inliers[side] =
        Data::indices(Indices{result.profiles[side].asProfile()->cloud->id, inliers->values});
    result.quality.data[sideNames[side]]["selectedInput"] = names[sourceIndex];
  }
  if (ctx.cancelled())
    return cancelled();
  out = std::move(result);
  return Status::Ok();
}
void registerProfileGapCircles(Registry& registry) {
  OperatorDesc op;
  op.id = "gap.fit_gap_circles";
  op.version = "3.0.0";
  declareGapGraphUpgrade(op);
  op.migrations.push_back(
      {2, nullptr, [](const nlohmann::json&, const std::vector<MigrationInput>& inputs) {
         return profile_contract::retireInputs(
             inputs, {"mergedProvenance", "primaryProvenance", "secondaryProvenance"});
       }});
  op.label = "原生轮廓间隙双侧圆拟合";
  op.category = "间隙/拟合";
  op.keywords = {"circle pair", "profile", "camera fallback", "间隙", "圆弧", "双相机"};
  for (const auto* name : {"merged", "primary", "secondary"})
    op.inputs.push_back({name, "Profile", name, "同空间明确单位及原槽来源。", true});
  for (const auto* side : {"Left", "Right"}) {
    op.inputs.push_back({std::string("box") + side, "Box2D", side, "米制业务ROI。", true});
    op.inputs.push_back({std::string("box") + side + "Context", "Record", "ROI上下文",
                         "ROI所在的profile.Geometry.v3空间。", true});
  }
  for (const auto* name : {"refLine", "refLineRight"}) {
    op.inputs.push_back({name, "Line2D", name, "可选约束参考线。", false});
    op.inputs.push_back(
        {std::string(name) + "Context", "Record", "参考线上下文", "与参考线成对提供。", false});
  }
  for (const auto* side : {"left", "right"}) {
    op.outputs.push_back({side, "Circle2D", side, "米制所选圆。", true});
    op.outputs.push_back({std::string(side) + "Context", "Record", "圆上下文",
                          "圆所在的profile.Geometry.v3空间。", true});
    op.outputs.push_back({std::string(side) + "Profile", "Profile", "拟合轮廓",
                          "所选ROI的原生采样与完整通道。", true});
    op.outputs.push_back(
        {std::string(side) + "Inliers", "Indices", "内点", "绑定所选Profile实际cloud.id。", true});
  }
  op.outputs.push_back({"quality", "Record", "质量", "保留旧数值与各侧所选输入路。", true});
  op.outputs.back().contract = {{"recordType", "gap.CircleFitPair.v3"}};
  op.params = circlePairParams();
  op.validate = &validateCirclePairParams;
  op.capabilities.cancellable = true;
  op.capabilities.deterministic = true;
  op.compute = [](const Inputs& in, const ParamView& parameters, Outputs& out, ExecContext& ctx) {
    CirclePairInputs inputs;
    inputs.merged = in.get("merged");
    inputs.primary = in.get("primary");
    inputs.secondary = in.get("secondary");
    inputs.boxes = {in.get("boxLeft"), in.get("boxRight")};
    inputs.boxContexts = {in.get("boxLeftContext"), in.get("boxRightContext")};
    if (in.has("refLine"))
      inputs.refLine = in.get("refLine");
    if (in.has("refLineContext"))
      inputs.refLineContext = in.get("refLineContext");
    if (in.has("refLineRight"))
      inputs.refLineRight = in.get("refLineRight");
    if (in.has("refLineRightContext"))
      inputs.refLineRightContext = in.get("refLineRightContext");
    CirclePairResult result;
    if (auto status = fitProfileGapCircles(inputs, parameters, result, ctx); !status.ok)
      return status;
    if (ctx.cancelled())
      return cancelled();
    for (std::size_t i = 0; i < 2; ++i) {
      const std::string side = i ? "right" : "left";
      out.set(side, result.circles[i]);
      out.set(side + "Context", result.contexts[i]);
      out.set(side + "Profile", result.profiles[i]);
      out.set(side + "Inliers", result.inliers[i]);
    }
    out.set("quality", Data::record(std::move(result.quality)));
    return Status::Ok();
  };
  registry.addOperator(std::move(op));
}
}
