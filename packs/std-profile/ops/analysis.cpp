#include <limits>
#include <utility>

#include "lyflow/kernel/profile_analysis.h"
#include "lyflow/operator.h"
#include "lyflow/registry.h"

namespace lyflow::packs::std_profile {
namespace {
using namespace kernel::profile_analysis;
Status error(const std::string& message, const std::string& port = "cloud",
             const std::string& code = "bad_input") {
  const bool parameter = code == "bad_param" || code == "index_out_of_range";
  return Status::Error(Phase::Execute, code, message, parameter ? port : std::string{},
                       parameter ? std::string{} : port);
}
OperatorDesc descriptor(std::string id, std::string label, std::string english) {
  OperatorDesc op;
  op.id = std::move(id);
  op.label = std::move(label);
  op.version = "1.0.0";
  op.category = "点云/三维分析";
  op.keywords = {english, op.label};
  op.capabilities = {true, false, true};
  return op;
}
Param text(std::string name, std::string label, std::string def) {
  Param p;
  p.name = std::move(name);
  p.label = std::move(label);
  p.type = ParamType::String;
  p.def = Value::text(std::move(def));
  return p;
}
Param choice(std::string name, std::string label, std::string def,
             std::initializer_list<std::pair<const char*, const char*>> choices) {
  Param p = text(std::move(name), std::move(label), std::move(def));
  p.type = ParamType::Enum;
  for (const auto& item : choices)
    p.options.push_back({item.first, item.second});
  return p;
}
Param number(std::string name, std::string label, double def, double min, double max,
             std::string unit) {
  Param p;
  p.name = std::move(name);
  p.label = std::move(label);
  p.type = ParamType::Float;
  p.def = Value::number(def);
  p.min = min;
  p.max = max;
  p.unit = std::move(unit);
  return p;
}
bool frameValid(const std::string& frame) { return !frame.empty() && frame.size() <= 1024; }
std::pair<int, int> axes(const std::string& plane) {
  return plane == "xy"   ? std::make_pair(0, 1)
         : plane == "yz" ? std::make_pair(1, 2)
                         : std::make_pair(0, 2);
}
Status validate(const PointCloud& cloud, std::size_t minimum = 0) {
  std::string why;
  return validateCloud(cloud, minimum, why) ? Status::Ok() : error(why);
}
}

void registerProfileAnalysis(Registry& r) {
  OperatorDesc op = descriptor("profile.analyze", "单剖面直线分析",
                               "single scan profile line analysis signal evaluator");
  op.inputs = {{"cloud", "PointCloud", "单剖面点云", "明确选择输入帧的XY/XZ/YZ平面。", true}};
  op.outputs = {{"line", "Line2D", "剖面拟合直线", "所选平面的米制二维直线。", true},
                {"statistics", "Record", "分析", "profile.Line.v1，采样范围/点数/正交RMS。", true}};
  op.params = {choice("plane", "剖面坐标平面", "xz", {{"xy", "XY"}, {"xz", "XZ"}, {"yz", "YZ"}}),
               number("maxResidual", "最大正交RMS", 0.001, 1e-12, 1e3, "m"),
               text("frame", "输入参考帧", "profile")};
  op.doc =
      "单条扫描的二维正交最小二乘直线分析；重合/各向同性/少点或残差超限拒绝。无隐式双相机ScanPair、无业务缝型推断。多特征圆/圆弧和复杂SignalEvaluator模式另行组合。";
  op.compute = [](const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
    const auto* cloud = in.get("cloud").asCloud();
    Status s = validate(*cloud, 3);
    if (!s.ok)
      return s;
    if (!frameValid(p.text("frame")))
      return error("参考帧必须非空且有界", "frame", "bad_param");
    const auto indices = axes(p.choice("plane"));
    LineFit line;
    if (!fitLine(*cloud, indices.first, indices.second, -std::numeric_limits<double>::infinity(),
                 std::numeric_limits<double>::infinity(), line, [&ctx] { return ctx.cancelled(); }))
      return ctx.cancelled() ? Status::Ok() : error("剖面直线退化或点数不足", "cloud", "no_result");
    if (line.rms > p.number("maxResidual"))
      return error("剖面线拟合残差超过门限", "cloud", "no_result");
    Record stats{"profile.Line.v1",
                 {{"frame", p.text("frame")},
                  {"plane", p.choice("plane")},
                  {"unit", "m"},
                  {"count", line.count},
                  {"rms", line.rms},
                  {"abscissaRange", {line.minimum, line.maximum}}}};
    out.set("line", Data::line2d(nativeLine(line)));
    out.set("statistics", Data::record(std::move(stats)));
    return Status::Ok();
  };
  r.addOperator(std::move(op));

  op = descriptor("profile.project_pair", "双剖面已知外参变换",
                  "dual profile projection known extrinsics frame transforms");
  op.inputs = {{"master", "PointCloud", "主扫描", "主扫描帧的米制点云。", true},
               {"slave", "PointCloud", "副扫描", "副扫描帧的米制点云。", true},
               {"masterToCommon", "Transform", "主帧到公共帧", "已标定的刚体外参。", true},
               {"slaveToCommon", "Transform", "副帧到公共帧", "已标定的刚体外参。", true},
               {"commonToOutput", "Transform", "公共帧到输出帧",
                "可选，绕显式旋转中心的变换由调用者构造。", false}};
  op.outputs = {{"master", "PointCloud", "主输出", "输出参考帧。", true},
                {"slave", "PointCloud", "副输出", "同一输出参考帧。", true},
                {"metadata", "Record", "坐标帧", "profile.Frames.v1。", true}};
  op.params = {text("masterFrame", "主参考帧", "master"), text("slaveFrame", "副参考帧", "slave"),
               text("commonFrame", "公共参考帧", "world"),
               text("outputFrame", "输出参考帧", "world")};
  op.doc =
      "只应用调用者显式提供的外参：output←common←master/slave。PointCloud不自带pose元数据，不猜测外参或自动更改配方中心；保留属性并旋转法线。";
  op.compute = [](const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
    for (const char* key : {"masterFrame", "slaveFrame", "commonFrame", "outputFrame"})
      if (!frameValid(p.text(key)))
        return error("参考帧必须非空且有界", key, "bad_param");
    Matrix4 common = identityMatrix();
    if (in.has("commonToOutput")) {
      if (!rigidMatrix(*in.get("commonToOutput").asTransform(), common))
        return error("公共帧变换必须为刚体", "commonToOutput");
    } else if (p.text("commonFrame") != p.text("outputFrame")) {
      return error("不同输出帧需要commonToOutput", "commonToOutput");
    }
    PointCloud master, slave;
    for (const auto& pair :
         {std::make_pair("master", "masterToCommon"), std::make_pair("slave", "slaveToCommon")}) {
      const auto* cloud = in.get(pair.first).asCloud();
      const auto* value = in.get(pair.second).asTransform();
      Matrix4 matrix;
      if (!rigidMatrix(*value, matrix))
        return error("点云或已知刚体外参无效", pair.first);
      Status s = validate(*cloud);
      if (!s.ok)
        return s;
      CloudChannels channels;
      if (!transformCloud(
              *cloud, compose(common, matrix), channels, [&ctx] { return ctx.cancelled(); },
              [&ctx](float value) { ctx.progress(value); }))
        return ctx.cancelled() ? Status::Ok() : error("投影坐标或法线超出范围", pair.first);
      auto& result = std::string(pair.first) == "master" ? master : slave;
      result.xyz = std::move(channels.xyz);
      result.intensity = std::move(channels.intensity);
      result.normals = std::move(channels.normals);
      result.rgb = std::move(channels.rgb);
    }
    out.set("master", Data::cloud(std::move(master)));
    out.set("slave", Data::cloud(std::move(slave)));
    out.set("metadata", Data::record(Record{"profile.Frames.v1",
                                            {{"masterFrame", p.text("masterFrame")},
                                             {"slaveFrame", p.text("slaveFrame")},
                                             {"commonFrame", p.text("commonFrame")},
                                             {"outputFrame", p.text("outputFrame")},
                                             {"unit", "m"}}}));
    return Status::Ok();
  };
  r.addOperator(std::move(op));
}
} // namespace lyflow::packs::std_profile
