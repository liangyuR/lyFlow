#include "profile_migration.h"
#include "profile_circles.h"

#include <algorithm>
#include <limits>
#include <set>

#include "lyflow/profile.h"
#include "lyflow_profile/provenance.h"

namespace lyflow::packs::gap::fine {
std::vector<Issue> validateFitGapCircles(const ParamView&, const std::set<std::string>&);
}
namespace lyflow::packs::gap::profile_scene {
namespace {
using Json = nlohmann::json;
Status bad(const std::string& message, const char* port = "merged",
           const char* code = "bad_input") {
  return Status::Error(Phase::Execute, code, message, {}, port);
}
Status cancelled() { return bad("间隙圆拟合已取消", "merged", "cancelled"); }
bool present(const Data& data) { return data.kind() != Data::Kind::None; }
bool joinSources(Json& into, const Json& from, std::string& why) {
  for (const auto& source : from)
    if (std::find(into.begin(), into.end(), source) == into.end())
      into.push_back(source);
  if (into.size() > 64) {
    why = "圆拟合几何来源超过64项预算";
    return false;
  }
  return true;
}
Status physical(const Data& geometry, const Data& context, const ProfileDescriptor& expected,
                Json& sources, const char* port) {
  ProfileDescriptor space;
  Json parents;
  std::string why;
  const auto* record = context.asRecord();
  if (!record || record->type != "profile.Geometry.v2" ||
      !profile_contract::geometryMatchesPhysical(geometry, record, space, why, &parents))
    return bad(why.empty() ? "几何需要严格v2内容绑定上下文" : why, port);
  if (!profile_contract::sameSpace(expected, space))
    return bad("圆拟合几何与轮廓的物理空间不一致", port);
  if (!joinSources(sources, parents, why))
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
  const std::array<const Data*, 3> provenanceData{
      &inputs.mergedProvenance, &inputs.primaryProvenance, &inputs.secondaryProvenance};
  std::array<const Record*, 3> provenance{};
  Json geometrySources = Json::array();
  std::string why;
  for (std::size_t i = 0; i < sources.size(); ++i) {
    const auto* profile = sources[i]->asProfile();
    if (profile->pointCount() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
      return bad("输入轮廓超过原槽索引范围", "merged", "budget_exceeded");
    if (i && !profile_contract::sameSpace(sources[0]->asProfile()->descriptor, profile->descriptor))
      return bad("三路轮廓必须在同一明确物理空间");
    provenance[i] = provenanceData[i]->asRecord();
    Json parents;
    if (!profile_contract::profileSources(*sources[i], provenance[i], parents, why) ||
        !joinSources(geometrySources, parents, why))
      return bad(why, "mergedProvenance");
  }
  const auto& space = inputs.merged.asProfile()->descriptor;
  for (std::size_t i = 0; i < 2; ++i) {
    if (auto status = physical(inputs.boxes[i], inputs.boxContexts[i], space, geometrySources,
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
      if (auto status = physical(*lines[i], *lineContexts[i], space, geometrySources,
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
  result.quality.type = "gap.CircleFitPair.v2";
  result.quality.data["schemaVersion"] = 2;
  const std::array<const char*, 2> sideNames{"left", "right"};
  for (std::size_t side = 0; side < 2; ++side) {
    if (ctx.cancelled())
      return cancelled();
    const auto sourceIndex = trace.sides[side].source == CircleSource::Primary     ? 1U
                             : trace.sides[side].source == CircleSource::Secondary ? 2U
                                                                                   : 0U;
    const auto& original = *sources[sourceIndex];
    const auto* profile = original.asProfile();
    const auto& slots = trace.sides[side].slots;
    Profile selected = *profile;
    selected.validity.clear();
    selected.sourceIndices.clear();
    for (std::size_t i = 0; i < slots.size(); ++i) {
      if ((i & 1023U) == 0 && ctx.cancelled())
        return cancelled();
      const auto slot = slots[i];
      if (slot < 0 || static_cast<std::size_t>(slot) >= profile->pointCount() ||
          !profile->validity[slot])
        return bad("拟合云原槽追踪非法", sideNames[side], "invalid_geometry");
      selected.validity.push_back(profile->validity[slot]);
      selected.sourceIndices.push_back(profile->sourceIndices[slot]);
    }
    selected.cloud = std::make_shared<const PointCloud>(profile->cloud->select(slots));
    result.profiles[side] = Data::profile(std::move(selected));
    Record rebound, extended;
    if (!profile_contract::rebindProvenance(original, result.profiles[side],
                                            provenance[sourceIndex], rebound, why) ||
        !profile_contract::extendProvenanceSources(result.profiles[side], rebound, geometrySources,
                                                   extended, why))
      return bad(why, "mergedProvenance");
    result.provenances[side] = Data::record(std::move(extended));
    const auto& circle = legacyResults.at(sideNames[side]);
    if (!profile_contract::validGeometryV2(circle, why))
      return bad(why, sideNames[side], "invalid_geometry");
    result.circles[side] = circle;
    Json allSources = geometrySources;
    if (!joinSources(allSources, Json::array({profile_contract::context(result.profiles[side])}),
                     why))
      return bad(why);
    try {
      result.contexts[side] =
          Data::record(Record{"profile.Geometry.v2",
                              profile_contract::geometryContextV2FromSources(circle, allSources)});
    } catch (const std::invalid_argument& error) {
      return bad(error.what());
    }
    const auto* inliers = legacyResults.at(side == 0 ? "leftInliers" : "rightInliers").asIndices();
    if (!inliers)
      return bad("拟合未返回内点索引");
    for (auto slot : inliers->values)
      if (slot < 0 || static_cast<std::size_t>(slot) >= slots.size())
        return bad("内点索引越界");
    result.inliers[side] =
        Data::indices(Indices{result.profiles[side].asProfile()->cloud->id, inliers->values});
    auto& quality = result.quality.data[sideNames[side]];
    quality["logicalRole"] = sideNames[side];
    quality["selectedInput"] = names[sourceIndex];
    quality["selectedSourceId"] = profile->descriptor.sourceId;
    quality["profileContext"] = profile_contract::context(result.profiles[side]);
    quality["actualSourceIds"] = Json::array();
    for (const auto& segment : rebound.data.at("recipe").at("segments")) {
      const auto& identity = segment.at("sourceContext").at("descriptor").at("sourceId");
      auto& identities = quality["actualSourceIds"];
      if (std::find(identities.begin(), identities.end(), identity) == identities.end())
        identities.push_back(identity);
    }
  }
  if (ctx.cancelled())
    return cancelled();
  out = std::move(result);
  return Status::Ok();
}
void registerProfileGapCircles(Registry& registry) {
  OperatorDesc op;
  op.id = "gap.fit_gap_circles";
  op.version = "2.0.0";
  declareGapGraphUpgrade(op);
  op.label = "原生轮廓间隙双侧圆拟合";
  op.category = "间隙/拟合";
  op.keywords = {"circle pair", "profile", "camera fallback", "间隙", "圆弧", "双相机"};
  for (const auto* name : {"merged", "primary", "secondary"})
    op.inputs.push_back({name, "Profile", name, "同空间明确单位及原槽来源。", true});
  op.inputs.push_back({"mergedProvenance", "Record", "合并来源", "严格绑定merged Profile。", true});
  for (const auto* name : {"primaryProvenance", "secondaryProvenance"})
    op.inputs.push_back({name, "Record", name, "可选输入来源。", false});
  for (const auto* side : {"Left", "Right"}) {
    op.inputs.push_back({std::string("box") + side, "Box2D", side, "米制业务ROI。", true});
    op.inputs.push_back(
        {std::string("box") + side + "Context", "Record", "ROI上下文", "严格v2内容绑定。", true});
  }
  for (const auto* name : {"refLine", "refLineRight"}) {
    op.inputs.push_back({name, "Line2D", name, "可选约束参考线。", false});
    op.inputs.push_back(
        {std::string(name) + "Context", "Record", "参考线上下文", "与参考线成对提供。", false});
  }
  for (const auto* side : {"left", "right"}) {
    op.outputs.push_back({side, "Circle2D", side, "米制所选圆。", true});
    op.outputs.push_back({std::string(side) + "Context", "Record", "圆上下文",
                          "全部实际测量父、ROI及参考线来源。", true});
    op.outputs.push_back({std::string(side) + "Profile", "Profile", "拟合轮廓",
                          "所选ROI的原生采样与完整通道。", true});
    op.outputs.push_back(
        {std::string(side) + "Provenance", "Record", "拟合来源", "绑定所选Profile的来源。", true});
    op.outputs.push_back(
        {std::string(side) + "Inliers", "Indices", "内点", "绑定所选Profile实际cloud.id。", true});
  }
  op.outputs.push_back({"quality", "Record", "质量", "保留旧数值与明确实际来源。", true});
  op.outputs.back().contract = {{"recordType", "gap.CircleFitPair.v2"}};
  op.params = circlePairParams();
  op.validate = &validateCirclePairParams;
  op.capabilities.cancellable = true;
  op.capabilities.deterministic = true;
  op.compute = [](const Inputs& in, const ParamView& parameters, Outputs& out, ExecContext& ctx) {
    CirclePairInputs inputs;
    inputs.merged = in.get("merged");
    inputs.primary = in.get("primary");
    inputs.secondary = in.get("secondary");
    inputs.mergedProvenance = in.get("mergedProvenance");
    if (in.has("primaryProvenance"))
      inputs.primaryProvenance = in.get("primaryProvenance");
    if (in.has("secondaryProvenance"))
      inputs.secondaryProvenance = in.get("secondaryProvenance");
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
      out.set(side + "Provenance", result.provenances[i]);
      out.set(side + "Inliers", result.inliers[i]);
    }
    out.set("quality", Data::record(std::move(result.quality)));
    return Status::Ok();
  };
  registry.addOperator(std::move(op));
}
}
