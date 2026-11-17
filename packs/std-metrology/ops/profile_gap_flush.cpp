#include <utility>

#include "lyflow/kernel/profile_analysis.h"
#include "lyflow/kernel/metrology.h"
#include "lyflow/operator.h"
#include "lyflow/registry.h"

namespace lyflow::packs::std_metrology {
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
Measurement measurement(double value) {
  Measurement m;
  m.ok = true;
  m.value = value;
  m.unit = "m";
  return m;
}
}

void registerProfileGapFlush(Registry& r) {
  OperatorDesc op =
      descriptor("profile.gap_flush", "单剖面间隙与面差", "single line scan gap flush measurement");
  op.inputs = {{"cloud", "PointCloud", "单扫描点云", "同一参考帧的米制单剖面。", true}};
  op.outputs = {{"gap", "Measurement", "间隙", "右区最小横坐标减左区最大横坐标。", true},
                {"flush", "Measurement", "有符号面差", "referenceX处右拟合面减左拟合面。", true},
                {"left", "Line2D", "左拟合线", "米制剖面坐标。", true},
                {"right", "Line2D", "右拟合线", "米制剖面坐标。", true},
                {"quality", "Record", "质量", "profile.GapFlush.v1。", true}};
  Param left;
  left.name = "leftRange";
  left.label = "左拟合区间";
  left.type = ParamType::Vec2f;
  left.def = Value::vec({-0.02, -0.005});
  left.unit = "m";
  Param right = left;
  right.name = "rightRange";
  right.label = "右拟合区间";
  right.def = Value::vec({0.005, 0.02});
  op.params = {left,
               right,
               choice("plane", "剖面坐标平面", "xz", {{"xy", "XY"}, {"xz", "XZ"}, {"yz", "YZ"}}),
               number("referenceX", "面差横坐标", 0, -1e6, 1e6, "m"),
               number("maxResidual", "最大正交RMS", 0.001, 1e-12, 1e3, "m"),
               number("maxExtrapolation", "最大外推距离", 0.05, 0, 1e3, "m"),
               text("frame", "输入参考帧", "profile")};
  op.doc =
      "使用两个显式不相交横坐标区间拟合单扫描左右表面，分别报告采样边缘间距和同横坐标有符号面差。拒绝少点、竖直线、残差超限和超限外推；圆角/槽/斜投影缝型另用专用算法。";
  op.compute = [](const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
    const auto* cloud = in.get("cloud").asCloud();
    Status s = validate(*cloud, 6);
    if (!s.ok)
      return s;
    const auto l = p.vec2("leftRange"), r = p.vec2("rightRange");
    const double x = p.number("referenceX"), maximum = p.number("maxResidual"),
                 extrapolation = p.number("maxExtrapolation");
    if (l[0] >= l[1] || l[1] >= r[0] || r[0] >= r[1] || !frameValid(p.text("frame")))
      return error("剖面区间或参考帧非法", "leftRange", "bad_param");
    const auto indices = axes(p.choice("plane"));
    LineFit a, b;
    if (!fitLine(*cloud, indices.first, indices.second, l[0], l[1], a,
                 [&ctx] { return ctx.cancelled(); }) ||
        !fitLine(*cloud, indices.first, indices.second, r[0], r[1], b,
                 [&ctx] { return ctx.cancelled(); }))
      return ctx.cancelled() ? Status::Ok()
                             : error("左右拟合区间需要至少3个非退化点", "cloud", "no_result");
    kernel::metrology::SampledLine left, right;
    left.center = a.center;
    left.direction = a.direction;
    left.minimum = a.minimum;
    left.maximum = a.maximum;
    left.rms = a.rms;
    right.center = b.center;
    right.direction = b.direction;
    right.minimum = b.minimum;
    right.maximum = b.maximum;
    right.rms = b.rms;
    kernel::metrology::SampledGapFlush result;
    std::string why;
    if (!kernel::metrology::sampledGapFlush(left, right, {x, maximum, extrapolation}, result, why))
      return error(why, "cloud", why == "剖面计算超出范围" ? "bad_input" : "no_result");
    const double gap = result.gap, flush = result.flush;
    out.set("gap", Data::measurement(measurement(gap)));
    out.set("flush", Data::measurement(measurement(flush)));
    out.set("left", Data::line2d(nativeLine(a)));
    out.set("right", Data::line2d(nativeLine(b)));
    out.set("quality", Data::record(Record{"profile.GapFlush.v1",
                                           {{"frame", p.text("frame")},
                                            {"plane", p.choice("plane")},
                                            {"unit", "m"},
                                            {"referenceX", x},
                                            {"leftCount", a.count},
                                            {"rightCount", b.count},
                                            {"leftRms", a.rms},
                                            {"rightRms", b.rms}}}));
    return Status::Ok();
  };
  r.addOperator(std::move(op));
}
} // namespace lyflow::packs::std_metrology
