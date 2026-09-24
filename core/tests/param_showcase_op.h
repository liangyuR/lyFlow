#pragma once
// 参数面板的全类型示例算子 test.param_showcase（param-recipe P2.10）：14 种参数类型各至少一个，
// 带 group、advanced、visibleWhen、enabledWhen 与 roi。它是 P2 / P3 的验收素材（examples/param-showcase.lyflow.json）。
//
// 与 test_ops.h 的 test.* 同一个身份：只在测试里注册。doctest 经 ensureTestOps() 注册；
// 编辑器的 e2e 跑的是真的 app，所以 core 也编进了这一份，但只在进程环境里 LYFLOW_TEST_OPS=1 时
// 才注册（core/tests/e2e/register_e2e_ops.cpp）—— 正常启动的 app 与 CLI 里没有它。
#include <nlohmann/json.hpp>

#include "lyflow/operator.h"
#include "lyflow/registry.h"

namespace lyflow::test {
namespace showcase {

/// 参数值原样回显成 JSON：transform 是 16 个数，curve 还原成对象。验收脚本据此断言
/// 「core 收到的就是编辑器写下去的」。curve 在 core 里是 JSON 文本，按名字认出来再解析。
inline nlohmann::json echoValue(const std::string& name, const Value& v) {
  switch (v.kind()) {
    case Value::Kind::Null:     return nullptr;
    case Value::Kind::Bool:     return v.boolValue();
    case Value::Kind::Int:      return v.intValue();
    case Value::Kind::Float:    return v.floatValue();
    case Value::Kind::FloatVec: return v.vecValue();
    case Value::Kind::String:
      if (name == "response" || name == "falloff") {
        return nlohmann::json::parse(v.stringValue(), nullptr, false);
      }
      return v.stringValue();
  }
  return nullptr;
}

inline Status compute(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                      ExecContext&) {
  // 有输入就原样透传（2D 拖框要一片底图），没接就给一片空云
  if (inputs.has("cloud")) {
    outputs.set("cloud", inputs.get("cloud"));
  } else {
    outputs.set("cloud", Data::cloud(PointCloud{}));
  }
  Record echo;
  echo.type = "ParamShowcase";
  for (const auto& [name, value] : params.raw()) echo.data["params"][name] = echoValue(name, value);
  // 曲线在几个 x 上的取值：core 的 evaluateCurve 与编辑器画的线同一套算法
  const nlohmann::json response = echo.data["params"].value("response", nlohmann::json());
  for (double x : {0.0, 0.25, 0.5, 0.75, 1.0}) {
    echo.data["responseAt"].push_back(evaluateCurve(response, x));
  }
  outputs.set("echo", Data::record(std::move(echo)));
  return Status::Ok();
}

inline Param make(const char* name, ParamType type, const char* label, Value def,
                  const char* group) {
  Param p;
  p.name = name;
  p.type = type;
  p.label = label;
  p.def = std::move(def);
  p.group = group;
  return p;
}

inline Condition when(const char* param, Value eq, bool negate = false) {
  Condition c;
  c.param = param;
  (negate ? c.ne : c.eq) = std::move(eq);
  return c;
}

inline EnumOption option(const char* value, const char* label, const char* doc = "") {
  return EnumOption{value, label, doc};
}

}  // namespace showcase

/// 注册 test.param_showcase。已经注册过就什么都不做（doctest 与 LYFLOW_TEST_OPS 可能都来一次）。
inline void registerParamShowcase(Registry& r) {
  using showcase::make;
  using showcase::option;
  using showcase::when;
  if (r.find("test.param_showcase")) return;

  OperatorDesc op;
  op.id = "test.param_showcase";
  op.version = "1.0.0";
  op.label = "参数全类型示例";
  op.category = "Test";
  op.keywords = {"showcase", "params", "test", "参数", "示例"};
  op.doc = "只在测试里注册：14 种参数类型各一个以上，参数面板与配方的验收素材。"
           "点云原样透传，echo 回显 core 收到的全部参数值。";
  op.inputs = {Port{"cloud", "PointCloud", "Cloud", "可选：有就原样透传，给 2D 拖框当底图。", false}};
  op.outputs = {Port{"cloud", "PointCloud", "Cloud", "输入的点云（没接输入时是空云）。", true},
                Port{"echo", "Record", "Echo", "core 收到的参数值（ParamShowcase）。", true}};

  // -- 基础
  Param enabled = make("enabled", ParamType::Bool, "Enabled", Value::boolean(true), "基础");
  enabled.doc = "关掉之后 Tolerance 置灰（enabledWhen）。";
  Param mode = make("mode", ParamType::Enum, "Mode", Value::text("fast"), "基础");
  mode.options = {option("fast", "快速"), option("precise", "精确"),
                  option("custom", "自定义", "露出 Custom Factor")};
  Param iterations = make("iterations", ParamType::Int, "Iterations", Value::integer(10), "基础");
  iterations.min = 1.0;
  iterations.max = 1000.0;
  iterations.softMax = 100.0;
  Param gain = make("gain", ParamType::Float, "Gain", Value::number(1.0), "基础");
  gain.min = 0.0;
  gain.max = 10.0;
  gain.softMin = 0.0;
  gain.softMax = 2.0;
  gain.step = 0.01;
  gain.unit = "×";
  Param custom = make("customFactor", ParamType::Float, "Custom Factor", Value::number(0.5), "基础");
  custom.doc = "只在 Mode = 自定义时出现（visibleWhen）。";
  custom.visibleWhen = when("mode", Value::text("custom"));
  Param tolerance = make("tolerance", ParamType::Float, "Tolerance", Value::number(0.001), "基础");
  tolerance.min = 0.0;
  tolerance.step = 0.0001;
  tolerance.unit = "m";
  tolerance.enabledWhen = when("enabled", Value::boolean(true));

  // -- 几何
  Param offset = make("offset", ParamType::Vec2f, "Offset", Value::vec({0.0, 0.0}), "几何");
  offset.unit = "m";
  offset.step = 0.001;
  Param scale = make("scale", ParamType::Vec3f, "Scale", Value::vec({1.0, 1.0, 1.0}), "几何");
  scale.min = 0.001;
  scale.step = 0.01;
  Param weights =
      make("weights", ParamType::Vec4f, "Weights", Value::vec({0.25, 0.25, 0.25, 0.25}), "几何");
  weights.min = 0.0;
  weights.max = 1.0;
  weights.componentLabels = {"A", "B", "C", "D"};
  Param roi = make("roi", ParamType::Vec4f, "ROI", Value::vec({-0.3, -0.2, 0.3, 0.2}), "几何");
  roi.semantic = "roi";
  roi.unit = "m";
  roi.doc = "XY 平面上的框 [xMin, yMin, xMax, yMax]，2D 剖面视图里可拖。";
  roi.componentLabels = {"X0", "Y0", "X1", "Y1"};
  Param pose = make("pose", ParamType::Transform, "Pose",
                    Value::vec({1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}), "几何");
  pose.unit = "m";
  pose.doc = "4×4 行主序刚体变换；旋转是内旋 X→Y→Z（R = Rz·Ry·Rx），与 transform.make 同一约定。";

  // -- 外观
  Param tint = make("tint", ParamType::Color, "Tint", Value::vec({0.29, 0.62, 1.0}), "外观");
  Param overlay =
      make("overlay", ParamType::Color, "Overlay", Value::vec({1.0, 0.5, 0.1, 0.6}), "外观");
  overlay.alpha = true;
  Param response = make("response", ParamType::Curve, "Response",
                        Value::text(R"({"interp":"smooth","points":[[0,0],[0.5,0.35],[1,1]]})"),
                        "外观");
  response.doc = "输入强度 → 输出强度。x、y 都是 0–1。";
  response.min = 0.0;
  response.max = 1.0;

  // -- 输出
  Param tag = make("tag", ParamType::String, "Tag", Value::text(""), "输出");
  tag.placeholder = "给这次运行起个名字";
  Param note = make("note", ParamType::Text, "Note", Value::text(""), "输出");
  note.rows = 3;
  Param writeFile = make("writeFile", ParamType::Bool, "Write File", Value::boolean(false), "输出");
  Param exportPath = make("exportPath", ParamType::Path, "Export Path", Value::text(""), "输出");
  exportPath.mode = "save";
  exportPath.filters = {FileFilter{"PCD", {"pcd"}}};
  exportPath.doc = "只在 Write File 打开时出现（visibleWhen）；不真的写文件。";
  exportPath.visibleWhen = when("writeFile", Value::boolean(true));

  // -- 高级（advanced：面板里默认折叠）
  Param features = make("features", ParamType::Flags, "Features", Value::integer(3), "");
  features.advanced = true;
  features.options = {EnumOption{"1", "法线", ""}, EnumOption{"2", "强度", ""},
                      EnumOption{"4", "颜色", ""}, EnumOption{"8", "时间戳", ""}};
  Param seed = make("seed", ParamType::Int, "Seed", Value::integer(0), "");
  seed.advanced = true;
  seed.min = 0.0;
  Param precision = make("precision", ParamType::Enum, "Precision", Value::text("single"), "");
  precision.advanced = true;
  precision.options = {option("single", "单精度"), option("double", "双精度")};
  precision.doc = "Mode = 快速时置灰（enabledWhen 的 ne）。";
  precision.enabledWhen = when("mode", Value::text("fast"), /*negate=*/true);
  Param falloff = make("falloff", ParamType::Curve, "Falloff",
                       Value::text(R"({"interp":"linear","points":[[0,1],[1,0]]})"), "");
  falloff.advanced = true;
  falloff.min = 0.0;
  falloff.max = 1.0;

  op.params = {enabled, mode,    iterations, gain,   custom,    tolerance,  offset,
               scale,   weights, roi,        pose,   tint,      overlay,    response,
               tag,     note,    writeFile,  exportPath, features, seed, precision, falloff};
  op.capabilities = {/*cancellable=*/false, /*previewable=*/false, /*deterministic=*/true};
  op.compute = &showcase::compute;
  r.addOperator(std::move(op));
}

}  // namespace lyflow::test
