#include <cmath>
#include <algorithm>
#include <array>
#include <limits>
#include <numeric>

#include "lyflow/operator.h"
#include "lyflow/profile.h"
#include "lyflow/registry.h"

namespace lyflow::packs::std_profile {
void registerProfileProcessing(Registry&);
void registerProfileInput(Registry&);
void registerNativeProfileAnalysis(Registry&);
void registerProfileAnalysis(Registry&);
void registerProfileSelection(Registry&);
void registerProfileMerge(Registry&);
void registerProfileRegionSelection(Registry&);
#ifdef LYFLOW_PROFILE_PCL_FILTER
void registerProfileRadiusOutlier(Registry&);
#endif
#ifdef LYFLOW_PROFILE_ROBUST
void registerRobustProfileFeatures(Registry&);
#endif
namespace {

Param text(const char* name, const char* label) {
  Param p;
  p.name = name;
  p.label = label;
  p.type = ParamType::String;
  p.def = Value::text("");
  return p;
}

Param choice(const char* name, const char* label, const char* initial,
             std::initializer_list<const char*> options) {
  Param p = text(name, label);
  p.type = ParamType::Enum;
  p.def = Value::text(initial);
  for (const auto* value : options)
    p.options.push_back({value, value});
  return p;
}

ProfileDescriptor descriptor(const ParamView& p) {
  ProfileDescriptor d;
  d.frame = p.text("frame");
  d.unit = p.choice("unit");
  d.plane = p.choice("plane");
  d.handedness = p.choice("handedness");
  d.sampleOrder = p.choice("sampleOrder");
  d.sourceId = p.text("sourceId");
  return d;
}

Status fromCloud(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  const auto cloud = in.get("cloud").cloudPtr();
  if (!cloud || cloud->xyz.size() % 3 != 0 || !cloud->channelsConsistent())
    return Status::Error(Phase::Execute, "bad_input", "点云坐标或通道长度不一致", {}, "cloud");
  const auto n = cloud->pointCount();
  if (n > std::numeric_limits<std::uint32_t>::max())
    return Status::Error(Phase::Execute, "bad_input", "轮廓槽数超过uint32范围", {}, "cloud");
  ProfileDescriptor d = descriptor(p);
  d.sourceCount = n;
  std::vector<std::uint8_t> valid(n);
  std::vector<std::uint32_t> indices(n);
  for (std::size_t i = 0; i < n; ++i) {
    if ((i & 4095) == 0 && ctx.cancelled())
      return Status::Ok();
    valid[i] = std::isfinite(cloud->xyz[3 * i]) && std::isfinite(cloud->xyz[3 * i + 1]) &&
               std::isfinite(cloud->xyz[3 * i + 2]);
    indices[i] = static_cast<std::uint32_t>(i);
  }
  Profile result;
  std::string why;
  if (!makeProfile(cloud, std::move(d), std::move(valid), std::move(indices), result, why))
    return Status::Error(Phase::Execute, "bad_input", why, {}, "cloud");
  out.set("profile", Data::profile(std::move(result)));
  return Status::Ok();
}

Status toCloud(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  const auto* profile = in.get("profile").asProfile();
  std::string why;
  if (!profile || !validateProfile(*profile, why))
    return Status::Error(Phase::Execute, "bad_input", why.empty() ? "需要有效Profile" : why, {},
                         "profile");
  if (ctx.cancelled())
    return Status::Ok();
  Profile converted;
  if (!convertProfileUnit(*profile, p.choice("unit"), converted, why,
                          [&ctx] { return ctx.cancelled(); })) {
    if (ctx.cancelled())
      return Status::Ok();
    return Status::Error(Phase::Execute, "bad_input", why, {}, "profile");
  }
  Record metadata;
  metadata.type = "profile.Descriptor.v1";
  metadata.data = profileDescriptorJson(converted.descriptor);
  auto exported = converted.cloud;
  bool mask = false;
  for (std::size_t i = 0; i < converted.pointCount(); ++i) {
    if ((i & 4095) == 0 && ctx.cancelled())
      return Status::Ok();
    mask = mask || (!converted.validity[i] && (std::isfinite(exported->xyz[3 * i]) ||
                                               std::isfinite(exported->xyz[3 * i + 1]) ||
                                               std::isfinite(exported->xyz[3 * i + 2])));
  }
  if (mask) {
    PointCloud cloud = *exported;
    PointCloud identity;
    cloud.id = identity.id;
    for (std::size_t i = 0; i < converted.pointCount(); ++i) {
      if ((i & 4095) == 0 && ctx.cancelled())
        return Status::Ok();
      if (!converted.validity[i])
        std::fill_n(cloud.xyz.begin() + i * 3, 3, std::numeric_limits<float>::quiet_NaN());
    }
    exported = std::make_shared<const PointCloud>(std::move(cloud));
  }
  out.set("cloud", Data::cloud(std::move(exported)));
  out.set("descriptor", Data::record(std::move(metadata)));
  out.set("profile", Data::profile(std::move(converted)));
  return Status::Ok();
}

Status remapAxes(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  const auto* input = in.get("profile").asProfile();
  std::string why;
  if (!input || !validateProfile(*input, why))
    return Status::Error(Phase::Execute, "bad_input", why.empty() ? "需要有效Profile" : why, {},
                         "profile");
  const auto& order = p.choice("axes");
  const auto signs = p.vec3("signs");
  for (float sign : signs)
    if (sign != 1 && sign != -1)
      return Status::Error(Phase::Execute, "bad_param", "轴符号只能为+1或-1", "signs");
  Profile result;
  if (!remapProfileAxes(*input, order, signs, p.text("targetFrame"), result, why,
                        [&ctx] { return ctx.cancelled(); }))
    return ctx.cancelled() ? Status::Ok()
                           : Status::Error(Phase::Execute, "bad_param", why, "targetFrame");
  out.set("profile", Data::profile(std::move(result)));
  return Status::Ok();
}

} // namespace

void registerPackOps(Registry& r) {
  OperatorDesc op;
  op.id = "profile.from_cloud";
  op.version = "1.0.0";
  op.label = "点云显式转换轮廓";
  op.category = "轮廓/数据适配";
  op.keywords = {"profile", "point cloud", "ordered", "轮廓", "点云", "采样顺序", "单位"};
  op.doc =
      "显式声明帧、m/mm单位、轴平面、手性、采样顺序及源身份；共享只读点云，保留全部槽和可选通道。"
      "NaN槽标为无效，不排序、不压缩、不换轴；acquisition必须由调用者确认原点序即采样序。";
  op.inputs = {{"cloud", "PointCloud", "点云", "按声明单位解释的原始点云。", true}};
  op.outputs = {{"profile", "Profile", "轮廓", "共享只读缓冲及明确槽映射。", true}};
  op.params = {text("frame", "坐标帧"),
               choice("unit", "原始坐标单位", "", {"", "m", "mm"}),
               choice("plane", "轴平面", "", {"", "XY", "XZ", "YZ"}),
               choice("handedness", "手性", "", {"", "right", "reflected"}),
               choice("sampleOrder", "采样顺序", "unknown", {"unknown", "acquisition"}),
               text("sourceId", "源轮廓身份")};
  op.capabilities.cancellable = true;
  op.capabilities.deterministic = false;
  op.compute = &fromCloud;
  op.validate = [](const ParamView& p, const std::set<std::string>&) {
    std::string why;
    if (!validateProfileDescriptor(descriptor(p), why))
      return std::vector<Issue>{Issue::error("bad_param", why)};
    return std::vector<Issue>{};
  };
  r.addOperator(std::move(op));

  op = {};
  op.id = "profile.to_cloud";
  op.version = "1.0.0";
  op.label = "轮廓显式导出点云";
  op.category = "轮廓/数据适配";
  op.keywords = {"profile", "point cloud", "convert", "轮廓", "单位换算", "点云"};
  op.doc = "仅显式换算m/mm坐标；保持原XYZ轴、点序与可选通道，法线不缩放。"
           "裸云的无效槽置NaN以免被当作观测；同时输出保留原始无效载荷与完整映射的Profile。"
           "descriptor仅含小元数据；裸PointCloud不能单独往返轮廓的有效性和源索引。";
  op.inputs = {{"profile", "Profile", "轮廓", "原生轮廓。", true}};
  op.outputs = {
      {"cloud", "PointCloud", "点云", "明确单位的原XYZ点云。", true},
      {"descriptor", "Record", "轮廓描述", "profile.Descriptor.v1小元数据，不含数组。", true},
      {"profile", "Profile", "完整轮廓", "换算单位后保留所有槽与映射的原生Profile。", true}};
  op.params = {choice("unit", "输出坐标单位", "m", {"m", "mm"})};
  op.capabilities.cancellable = true;
  op.compute = &toCloud;
  r.addOperator(std::move(op));

  op = {};
  op.id = "profile.remap_axes";
  op.version = "1.0.0";
  op.label = "轮廓显式轴映射";
  op.category = "轮廓/数据适配";
  op.keywords = {"profile", "axes", "reflection", "轮廓", "轴映射", "反射", "手性"};
  op.doc = "按明确的轴排列及+1/-1符号映射XYZ与法线，并更新平面、目标帧和手性；"
           "保持单位、采样槽和源索引。这是轴映射，不假装为右手刚体Transform，不执行隐式排序。";
  op.inputs = {{"profile", "Profile", "轮廓", "显式坐标契约的原生轮廓。", true}};
  op.outputs = {{"profile", "Profile", "轮廓", "映射后轮廓，源身份不变。", true}};
  Param signs;
  signs.name = "signs";
  signs.label = "轴符号";
  signs.type = ParamType::Vec3f;
  signs.def = Value::vec({1, 1, 1});
  op.params = {text("targetFrame", "目标坐标帧"),
               choice("axes", "输出XYZ对应原轴", "XYZ", {"XYZ", "XZY", "YXZ", "YZX", "ZXY", "ZYX"}),
               signs};
  op.capabilities.cancellable = true;
  op.compute = &remapAxes;
  r.addOperator(std::move(op));
  registerProfileInput(r);
  registerProfileAnalysis(r);
  registerProfileProcessing(r);
  registerNativeProfileAnalysis(r);
  registerProfileSelection(r);
  registerProfileMerge(r);
  registerProfileRegionSelection(r);
#ifdef LYFLOW_PROFILE_PCL_FILTER
  registerProfileRadiusOutlier(r);
#endif
#ifdef LYFLOW_PROFILE_ROBUST
  registerRobustProfileFeatures(r);
#endif
}

} // namespace lyflow::packs::std_profile
