#include "ops.h"

namespace lyflow::ops {
namespace {

Status compute(const Inputs& inputs, const ParamView&, Outputs& outputs, ExecContext& ctx) {
  const PointCloud& a = *inputs.get("a").asCloud();
  const PointCloud& b = *inputs.get("b").asCloud();

  // 通道求交：一方没有的通道整体丢弃。
  //
  // 备选做法是给缺失的一方补零，但那是在**编造数据** —— 一半点的强度是真值、
  // 另一半是 0，下游的强度着色会显示出一条根本不存在的分界。丢掉并 log warn
  // 至少是诚实的，用户能立刻看见发生了什么。
  //
  // 空点云要单独放过：hasIntensity() 就是 !intensity.empty()，所以一片
  // 0 个点的云在这里会「什么通道都没有」，于是上游一个恰好裁空的 crop_box
  // 会把另一侧完好的强度和颜色一起带走。空的一方对通道没有意见。
  auto agrees = [](const PointCloud& c, bool (PointCloud::*has)() const) {
    return c.pointCount() == 0 || (c.*has)();
  };
  const bool anyIntensity = a.hasIntensity() || b.hasIntensity();
  const bool anyNormals = a.hasNormals() || b.hasNormals();
  const bool anyRgb = a.hasRgb() || b.hasRgb();
  const bool intensity =
      anyIntensity && agrees(a, &PointCloud::hasIntensity) && agrees(b, &PointCloud::hasIntensity);
  const bool normals =
      anyNormals && agrees(a, &PointCloud::hasNormals) && agrees(b, &PointCloud::hasNormals);
  const bool rgb = anyRgb && agrees(a, &PointCloud::hasRgb) && agrees(b, &PointCloud::hasRgb);

  if (anyIntensity && !intensity) {
    ctx.log(LogLevel::Warn, "只有一侧带 intensity 通道，合并后该通道被丢弃");
  }
  if (anyNormals && !normals) {
    ctx.log(LogLevel::Warn, "只有一侧带 normals 通道，合并后该通道被丢弃");
  }
  if (anyRgb && !rgb) {
    ctx.log(LogLevel::Warn, "只有一侧带 rgb 通道，合并后该通道被丢弃");
  }

  PointCloud out;
  const std::size_t total = a.pointCount() + b.pointCount();
  out.xyz.reserve(total * 3);
  if (intensity) out.intensity.reserve(total);
  if (normals) out.normals.reserve(total * 3);
  if (rgb) out.rgb.reserve(total * 3);

  for (const PointCloud* src : {&a, &b}) {
    out.xyz.insert(out.xyz.end(), src->xyz.begin(), src->xyz.end());
    if (intensity) out.intensity.insert(out.intensity.end(), src->intensity.begin(), src->intensity.end());
    if (normals) out.normals.insert(out.normals.end(), src->normals.begin(), src->normals.end());
    if (rgb) out.rgb.insert(out.rgb.end(), src->rgb.begin(), src->rgb.end());
  }

  outputs.set("cloud", Data::cloud(std::move(out)));
  return Status::Ok();
}

}  // namespace

void registerUtilMerge(Registry& r) {
  OperatorDesc op;
  op.id = "util.merge";
  op.version = "1.0.0";
  op.label = "Merge Clouds";
  op.category = "Util";
  op.keywords = {"merge", "concat", "combine", "join", "合并", "拼接"};
  op.doc = "把两片点云拼成一片。通道求交：只有一方带的通道会被丢弃并告警。";

  // 两个显式端口而不是一个可变长端口：往一个端口连多条边的话，
  // 拼接顺序是隐式的（docs/graph-doc.md）。
  op.inputs = {
      Port{"a", "PointCloud", "A", "第一片点云。", true},
      Port{"b", "PointCloud", "B", "第二片点云。", true},
  };
  op.outputs = {Port{"cloud", "PointCloud", "Cloud", "拼接结果。", true}};

  op.capabilities = {/*cancellable=*/false, /*previewable=*/true, /*deterministic=*/true};
  op.compute = &compute;

  r.addOperator(std::move(op));
}

}  // namespace lyflow::ops
