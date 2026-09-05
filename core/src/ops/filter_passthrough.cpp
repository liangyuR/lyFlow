#include <limits>

#include "lyflow/json_writer.h"
#include "ops.h"

namespace lyflow::ops {
namespace {

Status compute(const Inputs& inputs, const ParamView& params, Outputs& outputs,
               ExecContext& ctx) {
  const PointCloud& in = *inputs.get("cloud").asCloud();
  const std::string& field = params.choice("field");
  const double lo = params.number("min");
  const double hi = params.number("max");
  const bool invert = params.flag("invert");
  const bool keepOrganized = params.flag("keepOrganized");

  // 跨参数的语义约束 —— 这类错误算子必须自己报，因为执行器只看单个参数的
  // 声明范围，看不出 min 和 max 之间的关系。带上 paramPath 前端才能标红框。
  if (lo >= hi) {
    return Status::Error(Phase::Execute, "bad_param",
                         "Min 必须小于 Max（当前 " + jsonNumber(lo) + " ≥ " + jsonNumber(hi) +
                             "）",
                         "min");
  }
  if (field == "intensity" && !in.hasIntensity()) {
    return Status::Error(Phase::Execute, "bad_input", "输入点云没有 intensity 通道", "field",
                         "cloud");
  }

  const std::size_t n = in.pointCount();
  int axis = 2;
  if (field == "x") axis = 0;
  else if (field == "y") axis = 1;
  else if (field == "z") axis = 2;
  else axis = -1;  // intensity

  std::vector<std::int32_t> keep;
  keep.reserve(n);
  Ticker ticker(ctx, n);
  for (std::size_t i = 0; i < n; ++i) {
    if (ticker.tick(i)) return Status::Ok();
    const float v = axis >= 0 ? in.xyz[i * 3 + static_cast<std::size_t>(axis)] : in.intensity[i];
    const bool inside = v >= lo && v <= hi;
    if (inside != invert) keep.push_back(static_cast<std::int32_t>(i));
  }

  PointCloud out;
  if (keepOrganized) {
    // 有序结构：删掉的点填 NaN 而不是移除，点数与输入一一对应。
    // 下游要能对得上下标时（比如再接一个 extract_indices）这是唯一正确的做法。
    // 逐通道拷贝而不是 out = in：拷贝赋值会连 id 一起搬过来，
    // 而这是一片**新**点云，老的 Indices 不该还能对得上它。
    out.xyz = in.xyz;
    out.intensity = in.intensity;
    out.normals = in.normals;
    out.rgb = in.rgb;

    std::vector<bool> kept(n, false);
    for (std::int32_t idx : keep) kept[static_cast<std::size_t>(idx)] = true;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (std::size_t i = 0; i < n; ++i) {
      if (kept[i]) continue;
      out.xyz[i * 3] = nan;
      out.xyz[i * 3 + 1] = nan;
      out.xyz[i * 3 + 2] = nan;
    }
  } else {
    out = in.select(keep);
  }

  Indices indices;
  indices.sourceCloudId = in.id;
  indices.values = std::move(keep);

  outputs.set("cloud", Data::cloud(std::move(out)));
  outputs.set("indices", Data::indices(std::move(indices)));
  return Status::Ok();
}

}  // namespace

void registerFilterPassthrough(Registry& r) {
  OperatorDesc op;
  op.id = "filter.passthrough";
  op.version = "1.0.0";
  op.label = "Passthrough";
  op.category = "Filter/Crop";
  op.keywords = {"crop", "clip", "range", "limit", "裁剪", "直通", "范围"};
  op.doc = "沿某个字段做区间裁剪，保留（或排除）落在 [min, max] 内的点。";

  op.inputs = {Port{"cloud", "PointCloud", "Cloud", "待裁剪的点云。", true}};
  op.outputs = {
      Port{"cloud",   "PointCloud", "Cloud",   "保留下来的点。", true},
      Port{"indices", "Indices",    "Indices", "保留点在输入点云中的下标。", true},
  };

  Param field;
  field.name = "field";
  field.type = ParamType::Enum;
  field.label = "Field";
  field.doc = "沿哪个字段裁剪。";
  field.def = Value::text("z");
  field.options = {
      EnumOption{"x", "X", ""},
      EnumOption{"y", "Y", ""},
      EnumOption{"z", "Z", ""},
      EnumOption{"intensity", "Intensity", "仅当输入点云带强度字段时可用。"},
  };

  Param lo;
  lo.name = "min";
  lo.type = ParamType::Float;
  lo.label = "Min";
  lo.def = Value::number(0.0);
  lo.softMin = -5.0;
  lo.softMax = 5.0;
  lo.step = 0.001;
  lo.unit = "m";

  Param hi;
  hi.name = "max";
  hi.type = ParamType::Float;
  hi.label = "Max";
  hi.def = Value::number(1.0);
  hi.softMin = -5.0;
  hi.softMax = 5.0;
  hi.step = 0.001;
  hi.unit = "m";

  Param invert;
  invert.name = "invert";
  invert.type = ParamType::Bool;
  invert.label = "Invert";
  invert.doc = "反向：保留区间之外的点。";
  invert.def = Value::boolean(false);

  Param keepOrganized;
  keepOrganized.name = "keepOrganized";
  keepOrganized.type = ParamType::Bool;
  keepOrganized.label = "Keep Organized";
  keepOrganized.doc = "保持有序点云结构，被滤掉的点置为 NaN 而不是删除。";
  keepOrganized.def = Value::boolean(false);
  keepOrganized.advanced = true;

  // 单位标注只对空间字段成立，intensity 是无量纲的。
  // 这里用 visibleWhen 的兄弟条件 enabledWhen 演示参数联动：
  // 选了 intensity 时把 keepOrganized 禁用（有序结构是空间概念）。
  keepOrganized.enabledWhen.param = "field";
  keepOrganized.enabledWhen.in = {Value::text("x"), Value::text("y"), Value::text("z")};

  op.params = {field, lo, hi, invert, keepOrganized};
  op.capabilities = {/*cancellable=*/true, /*previewable=*/true, /*deterministic=*/true};
  op.compute = &compute;

  r.addOperator(std::move(op));
}

}  // namespace lyflow::ops
