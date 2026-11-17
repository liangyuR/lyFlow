#include <array>
#include <cmath>
#include <limits>
#include <utility>

#include "lyflow/kernel/metrology.h"
#include "lyflow/operator.h"
#include "lyflow/profile.h"
#include "lyflow/registry.h"
#include "lyflow_profile/context.h"
#include "lyflow_profile/features.h"

namespace lyflow::packs::std_profile {
namespace {
Status failure(const std::string& why, const char* port = "profile",
               const char* code = "bad_input") {
  return Status::Error(Phase::Execute, code, why, {}, port);
}
Status cancelled() { return failure("轮廓选择已取消", "profile", "cancelled"); }
Param text(const char* name, const char* label, const char* value = "") {
  Param result;
  result.name = name;
  result.label = label;
  result.type = ParamType::String;
  result.def = Value::text(value);
  return result;
}
Param choice(const char* name, const char* label, const char* value,
             std::initializer_list<const char*> options) {
  auto result = text(name, label, value);
  result.type = ParamType::Enum;
  for (const auto* option : options)
    result.options.push_back({option, option});
  return result;
}
OperatorDesc description(const char* id, const char* label, const char* keywords) {
  OperatorDesc result;
  result.id = id;
  result.version = "2.0.0";
  result.label = label;
  result.category = "轮廓/选择与适配";
  result.keywords = {keywords, label};
  result.capabilities.deterministic = true;
  result.capabilities.cancellable = true;
  return result;
}

Status bindGeometry(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                    ExecContext& ctx) {
  std::string why;
  const auto* input = inputs.get("profile").asProfile();
  if (params.text("frame") != input->descriptor.frame)
    return Status::Error(Phase::Execute, "bad_param", "必须明确声明与轮廓相同的坐标帧", "frame");
  if (params.choice("plane") != input->descriptor.plane)
    return Status::Error(Phase::Execute, "bad_param", "必须明确声明与轮廓相同的分析平面", "plane");
  const auto& geometry = inputs.get("geometry");
  if (!profile_contract::validGeometry(geometry, why))
    return failure(why, "geometry");
  if (ctx.cancelled())
    return cancelled();
  outputs.set("geometry", geometry);
  outputs.set("context", Data::record(profile_contract::geometryContext(input->descriptor)));
  return Status::Ok();
}

Status nearestCore(const Inputs& inputs, Outputs& outputs, ExecContext& ctx, bool toLine) {
  std::string why;
  const auto* input = inputs.get("profile").asProfile();
  const char* geometryPort = toLine ? "line" : "anchor";
  const char* contextPort = toLine ? "lineContext" : "anchorContext";
  const auto& geometry = inputs.get(geometryPort);
  const auto* anchor = geometry.asPoint2D();
  const auto* line = geometry.asLine2D();
  if (auto status = profile_contract::geometryInSpace(geometry, inputs.get(contextPort),
                                                      input->descriptor, contextPort);
      !status.ok)
    return status;
  if (ctx.cancelled())
    return cancelled();
  Profile metric;
  if (!convertProfileUnit(*input, "m", metric, why, [&ctx] { return ctx.cancelled(); }))
    return ctx.cancelled() ? cancelled() : failure(why);
  const int a = metric.descriptor.plane == "YZ" ? 1 : 0;
  const int b = metric.descriptor.plane == "XY" ? 1 : 2;
  std::array<double, 2> linePoint{}, lineDirection{}, lineReference{};
  if (line) {
    linePoint = {line->point[0], line->point[1]};
    lineDirection = {line->dir[0], line->dir[1]};
    lineReference = kernel::metrology::canonicalNormalReference(lineDirection);
  }
  std::size_t selected = metric.pointCount();
  double best = std::numeric_limits<double>::infinity();
  for (std::size_t i = 0; i < metric.pointCount(); ++i) {
    if ((i & 1023U) == 0 && ctx.cancelled())
      return cancelled();
    if (!metric.validity[i])
      continue;
    const float x = metric.cloud->xyz[3 * i + a], y = metric.cloud->xyz[3 * i + b];
    double distance;
    if (line) {
      kernel::metrology::SignedPointLine projection;
      if (!kernel::metrology::signedPointLine({x, y}, linePoint, lineDirection, lineReference,
                                              kernel::metrology::DirectionPolicy::Normalize,
                                              projection, why))
        return failure(why, geometryPort);
      distance = std::fabs(projection.distance);
    } else {
      const float dx = x - anchor->p[0], dy = y - anchor->p[1];
      distance = std::sqrt(dx * dx + dy * dy);
      if (!std::isfinite(distance))
        distance = std::hypot(static_cast<double>(x) - anchor->p[0],
                              static_cast<double>(y) - anchor->p[1]);
    }
    if (distance < best) {
      best = distance;
      selected = i;
    }
  }
  if (selected == metric.pointCount())
    return failure("轮廓没有可选择的有效样本", "profile", "no_result");
  Point2D point;
  point.p[0] = metric.cloud->xyz[3 * selected + a];
  point.p[1] = metric.cloud->xyz[3 * selected + b];
  const auto selection = Data::record(Record{"profile.SampleSelection.v2",
                                             {{"schemaVersion", 2},
                                              {"sourceCloudId", std::to_string(input->cloud->id)},
                                              {"indexSpace", "profile_slots"},
                                              {"slotIndex", selected},
                                              {"sourceIndex", input->sourceIndices[selected]},
                                              {"distance", best},
                                              {"unit", "m"}}});
  if (ctx.cancelled())
    return cancelled();
  outputs.set("point", Data::point2d(point));
  outputs.set("pointContext", Data::record(profile_contract::geometryContext(input->descriptor)));
  outputs.set("selection", selection);
  outputs.set("profile", inputs.get("profile"));
  return Status::Ok();
}

Status filterValidity(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                      ExecContext& ctx) {
  const auto* input = inputs.get("profile").asProfile();
  if (input->pointCount() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
    return failure("轮廓槽数超过Indices范围", "profile", "budget_exceeded");
  const auto& mode = params.choice("mode");
  Indices valid, invalid;
  valid.sourceCloudId = invalid.sourceCloudId = input->cloud->id;
  for (std::size_t i = 0; i < input->pointCount(); ++i) {
    if ((i & 1023U) == 0 && ctx.cancelled())
      return cancelled();
    const bool accepted = input->validity[i] && std::isfinite(input->cloud->xyz[3 * i]) &&
                          std::isfinite(input->cloud->xyz[3 * i + 1]) &&
                          std::isfinite(input->cloud->xyz[3 * i + 2]);
    (accepted ? valid.values : invalid.values).push_back(static_cast<std::int32_t>(i));
  }
  Data result = inputs.get("profile");
  if (mode == "compact") {
    auto cloud = input->cloud->select(valid.values);
    std::vector<std::uint32_t> sourceIndices;
    sourceIndices.reserve(valid.values.size());
    for (const auto slot : valid.values) {
      if ((sourceIndices.size() & 1023U) == 0 && ctx.cancelled())
        return cancelled();
      sourceIndices.push_back(input->sourceIndices[static_cast<std::size_t>(slot)]);
    }
    result = Data::profile(
        Profile{std::make_shared<const PointCloud>(std::move(cloud)), input->descriptor,
                std::vector<std::uint8_t>(valid.values.size(), 1), std::move(sourceIndices)});
  }
  const auto info =
      Data::record(Record{"profile.ValiditySelection.v2",
                          {{"schemaVersion", 2},
                           {"mode", mode},
                           {"slotCount", input->pointCount()},
                           {"validCount", valid.values.size()},
                           {"invalidCount", invalid.values.size()},
                           {"removedCount", mode == "compact" ? invalid.values.size() : 0},
                           {"outputCount", result.asProfile()->pointCount()},
                           {"indexSpace", "input_profile_slots"},
                           {"sourceIndexMapping", "profile.sourceIndices"}}});
  const auto validData = Data::indices(std::move(valid)),
             invalidData = Data::indices(std::move(invalid));
  if (ctx.cancelled())
    return cancelled();
  outputs.set("profile", result);
  outputs.set("sourceProfile", inputs.get("profile"));
  outputs.set("valid", validData);
  outputs.set("invalid", invalidData);
  outputs.set("selection", info);
  return Status::Ok();
}

TopologyEdit retireProvenance(const nlohmann::json&, const std::vector<MigrationInput>& inputs) {
  return profile_contract::retireInputs(inputs, {"provenance"});
}
}

void registerProfileSelection(Registry& registry) {
  auto op = description("profile.bind_geometry", "轮廓几何坐标声明",
                        "profile geometry frame plane declare bind 坐标声明 几何上下文");
  op.migrations = {
      {1, nullptr, [](const nlohmann::json&, const std::vector<MigrationInput>& inputs) {
         return profile_contract::retireInputs(
             inputs, {"referenceGeometry", "referenceContext", "provenance"});
       }}};
  Port geometry{"geometry", "Any", "物理几何", "明确位于所声明帧和轴平面的米制点、线、圆或框。",
                true};
  geometry.anyGroup = 1;
  op.inputs = {{"profile", "Profile", "来源轮廓", "提供坐标帧、分析平面与手性。", true}, geometry};
  op.outputs = {geometry,
                {"context", "Record", "几何上下文",
                 "profile.Geometry.v3：轮廓的坐标帧、分析平面与手性。", true}};
  op.params = {text("frame", "已确认的坐标帧"),
               choice("plane", "已确认的分析平面", "", {"", "XY", "XZ", "YZ"})};
  op.doc = "显式确认外部米制几何与轮廓处于相同坐标帧和分析平面，输出其空间上下文。"
           "这是调用方声明，不证明标定正确；坐标不做缩放或换轴。";
  op.compute = &bindGeometry;
  registry.addOperator(std::move(op));
  for (const bool toLine : {false, true}) {
    op = description(toLine ? "profile.nearest_to_line" : "profile.nearest_sample",
                     toLine ? "轮廓直线最近样本" : "轮廓锚点最近样本",
                     "profile nearest sample point line 轮廓 最近点 选点");
    if (toLine)
      op.version = "2.0.1";
    op.migrations = {{1, nullptr, &retireProvenance}};
    op.inputs = {{"profile", "Profile", "轮廓", "按原槽序选择有效样本；保留物理源索引。", true},
                 {toLine ? "line" : "anchor", toLine ? "Line2D" : "Point2D", "参考几何",
                  "同帧同平面的米制几何。", true},
                 {toLine ? "lineContext" : "anchorContext", "Record", "参考上下文",
                  "profile.Geometry.v3。", true}};
    op.outputs = {
        {"point", "Point2D", "选中样本", "分析平面内的原始样本，明确换算为米。", true},
        {"pointContext", "Record", "点上下文", "profile.Geometry.v3。", true},
        {"selection", "Record", "选择信息",
         "profile.SampleSelection.v2：原槽号、物理源索引与米制距离。", true},
        {"profile", "Profile", "原始轮廓", "共享只读输入；完整源映射留在原生数组。", true}};
    op.doc = toLine ? "按到无限直线的垂距选有效样本，不限幅到线段；等距保留原槽序较前者。"
                    : "按到明确锚点的欧氏距离扫描全部有效样本；等距保留原槽序较前者。";
    op.compute = [toLine](const Inputs& in, const ParamView&, Outputs& out, ExecContext& ctx) {
      profile_contract::NearestInputs input{in.get("profile"), in.get(toLine ? "line" : "anchor"),
                                            in.get(toLine ? "lineContext" : "anchorContext")};
      profile_contract::NearestResult result;
      if (auto status =
              profile_contract::nearestSample(input,
                                              toLine ? profile_contract::NearestReference::Line
                                                     : profile_contract::NearestReference::Point,
                                              result, ctx);
          !status.ok)
        return status;
      if (ctx.cancelled())
        return cancelled();
      out.set("profile", result.profile);
      out.set("point", result.point);
      out.set("pointContext", result.pointContext);
      out.set("selection", result.selection);
      return Status::Ok();
    };
    registry.addOperator(std::move(op));
  }
  op = description("profile.filter_validity", "轮廓有效样本筛选",
                   "profile validity finite mask compact 轮廓 有效性 非有限 保槽 筛选");
  op.migrations = {{1, nullptr, &retireProvenance}};
  op.inputs = {{"profile", "Profile", "轮廓", "完整原生采样、有效掩码与源索引。", true}};
  op.outputs = {
      {"profile", "Profile", "筛选轮廓", "显式保槽或压缩；所有通道和物理源索引保持对应。", true},
      {"sourceProfile", "Profile", "原始轮廓", "valid/invalid的槽号属于此原始轮廓。", true},
      {"valid", "Indices", "保留槽", "绑定原始轮廓实际cloud.id的槽号。", true},
      {"invalid", "Indices", "无效槽", "绑定原始轮廓实际cloud.id的槽号。", true},
      {"selection", "Record", "筛选信息",
       "profile.ValiditySelection.v2计数；大数组保留在原生载荷。", true}};
  op.params = {choice("mode", "输出模式", "preserve_slots", {"preserve_slots", "compact"})};
  op.doc =
      "只按既有有效掩码与XYZ有限性筛选，不进行平滑、强度阈值或统计去噪。压缩保留样本顺序与全部可选通道。";
  op.compute = &filterValidity;
  registry.addOperator(std::move(op));
}
} // namespace lyflow::packs::std_profile

namespace lyflow::profile_contract {
Status nearestSample(const NearestInputs& input, NearestReference reference, NearestResult& out,
                     ExecContext& ctx) {
  if (reference != NearestReference::Point && reference != NearestReference::Line)
    return Status::Error(Phase::Execute, "bad_param", "未知最近样本参考类型");
  const bool toLine = reference == NearestReference::Line;
  std::unordered_map<std::string, Data> inputs{
      {"profile", input.profile},
      {toLine ? "line" : "anchor", input.reference},
      {toLine ? "lineContext" : "anchorContext", input.referenceContext}};
  std::unordered_map<std::string, Data> values;
  Outputs outputs(values);
  if (auto status = packs::std_profile::nearestCore(Inputs(inputs), outputs, ctx, toLine);
      !status.ok)
    return status;
  NearestResult result{values.at("profile"), values.at("point"), values.at("pointContext"),
                       values.at("selection")};
  if (ctx.cancelled())
    return Status::Error(Phase::Execute, "cancelled", "最近样本已取消");
  out = std::move(result);
  return Status::Ok();
}
}
