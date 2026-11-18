#include <utility>

#include "lyflow/kernel/geometry.h"
#include "lyflow/op_params.h"
#include "lyflow/operator.h"
#include "lyflow/registry.h"
#include "lyflow_profile/context.h"

namespace lyflow::packs::std_geometry {
namespace {
Status anchoredWindow(const Inputs& in, const ParamView& p, Outputs& out, ExecContext&) {
  const auto& anchor = in.get("anchor");
  const auto& height = in.has("heightAnchor") ? in.get("heightAnchor") : anchor;
  ProfileDescriptor space;
  std::string why;
  if (in.has("anchorContext")) {
    if (const auto status = profile_contract::geometrySpace(anchor, in.get("anchorContext"), space,
                                                            "anchorContext");
        !status.ok)
      return status;
  } else {
    if (!profile_contract::validGeometry(anchor, why))
      return op_params::badInput(why, "anchor");
    space.frame = p.text("frame");
    if (space.frame.empty())
      return op_params::badParam("请明确指定锚框所在的坐标帧", "frame");
    space.plane = p.choice("plane");
    space.handedness = p.choice("handedness");
  }
  if (in.has("heightContext")) {
    if (!in.has("heightAnchor"))
      return op_params::badInput("高度上下文需要对应的高度锚框", "heightContext");
    if (!profile_contract::geometryInSpace(height, in.get("heightContext").asRecord(), space, why))
      return Status::Error(Phase::Execute, "space_mismatch", why, {}, "heightContext");
  } else if (in.has("heightAnchor")) {
    if (in.has("anchorContext"))
      return op_params::badInput("高度锚框需要自己的空间上下文", "heightContext");
    if (!profile_contract::validGeometry(height, why))
      return op_params::badInput(why, "heightAnchor");
  }
  Box2D box;
  if (!kernel::anchoredWindow(*anchor.asBox2D(), *height.asBox2D(), p.choice("side") == "left",
                              p.number("offset"), p.number("length"), p.number("heightPadding"),
                              box, why))
    return op_params::badParam(why, "length");
  out.set("box", Data::box2d(box));
  out.set("context", Data::record(profile_contract::geometryContext(space)));
  return Status::Ok();
}
}

void registerAnchoredWindow(Registry& registry) {
  using namespace op_params;
  OperatorDesc op;
  op.id = "geometry.anchored_window";
  op.version = "1.0.0";
  op.label = "锚框方向窗口";
  op.category = "几何/构造";
  op.doc =
      "从水平锚框的左边或右边推出米制窗口，可按独立锚框取高度范围。直接输入矩形时明确声明空间。";
  op.inputs = {{"anchor", "Box2D", "水平锚框", "米制矩形。", true},
               withContract({"anchorContext", "Record", "水平上下文", "锚框所在空间。", false},
                            {{"recordType", "profile.Geometry.v3"}}),
               {"heightAnchor", "Box2D", "高度锚框", "省略时沿用水平锚框。", false},
               withContract({"heightContext", "Record", "高度上下文", "与水平锚框同空间。", false},
                            {{"recordType", "profile.Geometry.v3"}})};
  op.outputs = {{"box", "Box2D", "窗口", "米制矩形。", true},
                {"context", "Record", "窗口上下文", "profile.Geometry.v3。", true}};
  op.outputs.back().contract = {{"recordType", "profile.Geometry.v3"}};
  auto distance = [](const char* name, const char* label, double value, const char* doc) {
    auto param = floatParam(name, label, value, doc);
    param.unit = "m";
    param.min = 0;
    return param;
  };
  op.params = {
      enumParam("side", "方向", "left", {{"left", "左"}, {"right", "右"}}, "选择水平锚框的边。"),
      distance("offset", "偏移", 0, "锚框边到窗口近边的距离。"),
      distance("length", "长度", .013, "窗口水平方向的长度。"),
      distance("heightPadding", "高度扩展", .0025, "高度范围向上下各扩展的距离。"),
      text("frame", "坐标帧", ""),
      enumParam("plane", "分析平面", "XY", {{"XY", "XY"}, {"XZ", "XZ"}, {"YZ", "YZ"}},
                "直接输入锚框时的分析平面。"),
      enumParam("handedness", "手性", "right", {{"right", "右手"}, {"reflected", "镜像"}},
                "直接输入锚框时的手性。")};
  op.capabilities.deterministic = true;
  op.compute = anchoredWindow;
  registry.addOperator(std::move(op));
}
}
