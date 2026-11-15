#include <cmath>
#include <limits>
#include <utility>

#include "lyflow/kernel/profile_analysis.h"
#include "lyflow/operator.h"
#include "lyflow/profile.h"
#include "lyflow/registry.h"
#include "lyflow_profile/context.h"

namespace lyflow::packs::std_profile {
namespace {
namespace analysis = kernel::profile_analysis;

Status failure(std::string message, std::string port = "profile", std::string code = "bad_input") {
  return Status::Error(Phase::Execute, code, std::move(message), {}, std::move(port));
}
Status badParam(std::string message, std::string key) {
  return Status::Error(Phase::Execute, "bad_param", std::move(message), std::move(key));
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
Param text(const char* name, const char* label, const char* value) {
  Param p;
  p.name = name;
  p.label = label;
  p.type = ParamType::String;
  p.def = Value::text(value);
  return p;
}
Param policy() {
  Param p = text("invalidPolicy", "无效槽策略", "reject");
  p.type = ParamType::Enum;
  p.options = {{"reject", "拒绝无效槽"}, {"valid_only", "仅选有效槽"}};
  return p;
}
std::pair<int, int> axes(const std::string& plane) {
  return plane == "XY"   ? std::make_pair(0, 1)
         : plane == "YZ" ? std::make_pair(1, 2)
                         : std::make_pair(0, 2);
}
bool samples(const Profile& input, const std::string& invalidPolicy, analysis::CloudChannels& out,
             ExecContext& ctx, std::string& why, std::size_t minimum = 3,
             Profile* converted = nullptr) {
  if (invalidPolicy == "reject") {
    for (auto validity : input.validity)
      if (!validity) {
        why = "轮廓含无效槽；需要显式选择valid_only策略";
        return false;
      }
  }
  Profile metric;
  if (!convertProfileUnit(input, "m", metric, why, [&ctx] { return ctx.cancelled(); }))
    return false;
  for (std::size_t i = 0; i < metric.pointCount(); ++i) {
    if ((i & 8191) == 0 && ctx.cancelled())
      return false;
    if (!metric.validity[i])
      continue;
    out.xyz.insert(out.xyz.end(), metric.cloud->xyz.begin() + i * 3,
                   metric.cloud->xyz.begin() + i * 3 + 3);
    if (metric.cloud->hasIntensity())
      out.intensity.push_back(metric.cloud->intensity[i]);
    if (metric.cloud->hasNormals())
      out.normals.insert(out.normals.end(), metric.cloud->normals.begin() + i * 3,
                         metric.cloud->normals.begin() + i * 3 + 3);
    if (metric.cloud->hasRgb())
      out.rgb.insert(out.rgb.end(), metric.cloud->rgb.begin() + i * 3,
                     metric.cloud->rgb.begin() + i * 3 + 3);
  }
  if (!analysis::validateCloud({out.xyz, out.intensity, out.normals, out.rgb}, minimum, why))
    return false;
  if (converted)
    *converted = std::move(metric);
  return true;
}

Status fitLine(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  const auto& data = in.get("profile");
  const auto* input = data.asProfile();
  std::string why;
  if (!input || !validateProfile(*input, why))
    return failure(why.empty() ? "需要有效Profile" : why);
  const auto selected = p.choice("invalidPolicy");
  if (selected != "reject" && selected != "valid_only")
    return badParam("无效槽策略非法", "invalidPolicy");
  const double maximum = p.number("maxResidual");
  if (!std::isfinite(maximum) || maximum <= 0)
    return badParam("拟合残差门限必须为正有限数", "maxResidual");
  double lower = -std::numeric_limits<double>::infinity();
  double upper = std::numeric_limits<double>::infinity();
  if (p.flag("rangeEnabled")) {
    const auto range = p.vec2("range");
    lower = range[0];
    upper = range[1];
    if (!std::isfinite(lower) || !std::isfinite(upper) || lower >= upper)
      return badParam("拟合闭区间非法", "range");
  }
  analysis::CloudChannels active;
  if (!samples(*input, selected, active, ctx, why))
    return ctx.cancelled() ? Status::Ok() : failure(why);
  const auto selectedAxes = axes(input->descriptor.plane);
  analysis::LineFit fit;
  if (!analysis::fitLine({active.xyz, active.intensity, active.normals, active.rgb},
                         selectedAxes.first, selectedAxes.second, lower, upper, fit,
                         [&ctx] { return ctx.cancelled(); }))
    return ctx.cancelled() ? Status::Ok()
                           : failure("剖面直线退化或点数不足", "profile", "no_result");
  if (fit.rms > maximum)
    return failure("剖面线拟合残差超过门限", "profile", "no_result");
  const auto line = Data::line2d(analysis::nativeLine(fit));
  Record statistics{"profile.LineFit.v1",
                    {{"schemaVersion", 1},
                     {"frame", input->descriptor.frame},
                     {"plane", input->descriptor.plane},
                     {"unit", "m"},
                     {"count", fit.count},
                     {"rms", fit.rms},
                     {"abscissaRange", {fit.minimum, fit.maximum}},
                     {"context", profile_contract::context(data)},
                     {"geometryHash", profile_contract::geometryHash(line)}}};
  out.set("line", line);
  out.set("statistics", Data::record(std::move(statistics)));
  out.set("lineContext", Data::record(Record{"profile.Geometry.v1",
                                             profile_contract::geometryContext(data, line)}));
  return Status::Ok();
}

Status transformPair(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  const auto commonFrame = p.text("commonFrame"), outputFrame = p.text("outputFrame");
  const auto targetPlane = p.choice("targetPlane"), selected = p.choice("invalidPolicy");
  if (selected != "reject" && selected != "valid_only")
    return badParam("无效槽策略非法", "invalidPolicy");
  analysis::Matrix4 common = analysis::identityMatrix();
  if (in.has("commonToOutput")) {
    const auto* transform = in.get("commonToOutput").asTransform();
    if (!transform || !analysis::rigidMatrix(*transform, common))
      return failure("公共帧变换必须为刚体", "commonToOutput");
  } else if (commonFrame != outputFrame) {
    return failure("不同输出帧需要commonToOutput", "commonToOutput");
  }
  Data master, slave;
  nlohmann::json metadata{{"schemaVersion", 1},
                          {"commonFrame", commonFrame},
                          {"outputFrame", outputFrame},
                          {"plane", targetPlane},
                          {"unit", "m"},
                          {"invalidPolicy", selected},
                          {"invalidSlots", "preserved_si_untransformed_not_observations"}};
  for (const auto& source :
       {std::make_pair("master", "masterToCommon"), std::make_pair("slave", "slaveToCommon")}) {
    const auto& sourceData = in.get(source.first);
    const auto* input = sourceData.asProfile();
    const auto* transform = in.get(source.second).asTransform();
    std::string why;
    if (!input || !validateProfile(*input, why))
      return failure(why.empty() ? "需要有效Profile" : why, source.first);
    ProfileDescriptor target = input->descriptor;
    target.frame = commonFrame;
    if (!validateProfileDescriptor(target, why))
      return badParam(why, "commonFrame");
    target.frame = outputFrame;
    target.plane = targetPlane;
    target.unit = "m";
    if (!validateProfileDescriptor(target, why))
      return badParam(why, "outputFrame");
    analysis::Matrix4 sensor;
    if (!transform || !analysis::rigidMatrix(*transform, sensor))
      return failure("已知刚体外参无效", source.second);
    analysis::CloudChannels active, moved;
    Profile metric;
    if (!samples(*input, selected, active, ctx, why, 0, &metric))
      return ctx.cancelled() ? Status::Ok() : failure(why, source.first);
    if (!analysis::transformCloud(
            {active.xyz, active.intensity, active.normals, active.rgb},
            analysis::compose(common, sensor), moved, [&ctx] { return ctx.cancelled(); },
            [&ctx](float value) { ctx.progress(value); }))
      return ctx.cancelled() ? Status::Ok() : failure("投影坐标或法线超出范围", source.first);
    PointCloud cloud = *metric.cloud;
    PointCloud identity;
    cloud.id = identity.id;
    std::size_t activeIndex = 0;
    for (std::size_t i = 0; i < metric.pointCount(); ++i) {
      if ((i & 8191) == 0 && ctx.cancelled())
        return Status::Ok();
      if (!metric.validity[i])
        continue;
      for (int j = 0; j < 3; ++j) {
        cloud.xyz[i * 3 + j] = moved.xyz[activeIndex * 3 + j];
        if (cloud.hasNormals())
          cloud.normals[i * 3 + j] = moved.normals[activeIndex * 3 + j];
      }
      ++activeIndex;
    }
    metric.cloud = std::make_shared<const PointCloud>(std::move(cloud));
    metric.descriptor = std::move(target);
    if (!validateProfile(metric, why))
      return failure(why, source.first);
    auto data = Data::profile(std::move(metric));
    const std::string prefix = source.first;
    metadata[prefix + "Frame"] = input->descriptor.frame;
    metadata[prefix + "Context"] = profile_contract::context(sourceData);
    metadata[prefix + "OutputContext"] = profile_contract::context(data);
    (prefix == "master" ? master : slave) = std::move(data);
  }
  out.set("master", std::move(master));
  out.set("slave", std::move(slave));
  out.set("metadata", Data::record(Record{"profile.PairFrames.v1", std::move(metadata)}));
  return Status::Ok();
}
}

void registerNativeProfileAnalysis(Registry& registry) {
  OperatorDesc op;
  op.id = "profile.fit_line";
  op.version = "1.0.0";
  op.label = "原生轮廓正交直线拟合";
  op.category = "轮廓/拟合";
  op.keywords = {"profile", "TLS", "line", "轮廓", "直线"};
  op.capabilities = {true, false, true};
  op.doc = "读取轮廓坐标声明，将m/mm显式归一到米后采用原Eigen TLS；不排序、不改写源槽。"
           "默认拒绝无效槽，valid_only仅在工作缓冲选择有效槽，并严格检查其全部坐标和附加通道。"
           "闭区间及残差参数均按米解释，质量基于实际float32输出直线。";
  op.inputs = {{"profile", "Profile", "轮廓", "明确坐标与有效性契约的轮廓。", true}};
  op.outputs = {
      {"line", "Line2D", "拟合直线", "米制输出直线。", true},
      {"statistics", "Record", "拟合统计", "profile.LineFit.v1，携带声明上下文与几何指纹。", true},
      {"lineContext", "Record", "直线坐标上下文", "profile.Geometry.v1。", true}};
  Param enabled;
  enabled.name = "rangeEnabled";
  enabled.label = "限制拟合横坐标区间";
  enabled.type = ParamType::Bool;
  enabled.def = Value::boolean(false);
  Param range;
  range.name = "range";
  range.label = "拟合闭区间";
  range.type = ParamType::Vec2f;
  range.def = Value::vec({-0.02, 0.02});
  range.unit = "m";
  op.params = {enabled, range, scalar("maxResidual", "最大正交RMS", 0.001, 1e-12, 1e3), policy()};
  op.compute = &fitLine;
  registry.addOperator(std::move(op));

  op = {};
  op.id = "profile.transform_pair";
  op.version = "1.0.0";
  op.label = "原生双轮廓已知外参变换";
  op.category = "轮廓/坐标适配";
  op.keywords = {"profile", "transform", "pair", "轮廓", "外参"};
  op.capabilities = {true, false, true};
  op.doc = "双轮廓先按各自声明单位换算到SI米制，再应用显式output←common←sensor刚体变换。"
           "输入帧取各自descriptor；输出帧和分析轴平面必须显式声明，不推断物理标定或共面性。"
           "保留来源、采集序、槽、索引、有效性与通道；默认拒绝无效槽。"
           "valid_only仅变换有效槽；无效槽保留SI换算后的原始载荷且mask=0，"
           "不参与刚体变换，不可按输出帧解释为观测，不会归零、复活或丢弃。";
  op.inputs = {
      {"master", "Profile", "主轮廓", "帧、单位、槽语义明确的轮廓。", true},
      {"slave", "Profile", "副轮廓", "来源与采样顺序独立的轮廓。", true},
      {"masterToCommon", "Transform", "主帧到公共帧", "平移按米声明的右手刚体。", true},
      {"slaveToCommon", "Transform", "副帧到公共帧", "平移按米声明的右手刚体。", true},
      {"commonToOutput", "Transform", "公共帧到输出帧", "帧名不同时必须显式提供。", false}};
  op.outputs = {{"master", "Profile", "主输出轮廓", "保留源槽和映射的米制轮廓。", true},
                {"slave", "Profile", "副输出轮廓", "与主输出声明同一帧及分析平面。", true},
                {"metadata", "Record", "双源坐标声明", "profile.PairFrames.v1。", true}};
  auto plane = text("targetPlane", "输出分析轴平面", "");
  plane.type = ParamType::Enum;
  plane.options = {{"", "请选择"}, {"XY", "XY"}, {"XZ", "XZ"}, {"YZ", "YZ"}};
  op.params = {text("commonFrame", "公共坐标帧", ""), text("outputFrame", "输出坐标帧", ""), plane,
               policy()};
  op.compute = &transformPair;
  registry.addOperator(std::move(op));
}
} // namespace lyflow::packs::std_profile
