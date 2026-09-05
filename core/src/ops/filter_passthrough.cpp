#include "ops.h"

namespace lyflow::ops {

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

  r.addOperator(std::move(op));
}

}  // namespace lyflow::ops
