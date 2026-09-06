#include "algo/crop2d.h"
#include "ops.h"

namespace lyflow::ops {
namespace {

Status compute(const Inputs& inputs, const ParamView& params, Outputs& outputs,
               ExecContext& ctx) {
  const PointCloud& in = *inputs.get("cloud").asCloud();
  const Box2D& box = *inputs.get("box").asBox2D();
  const std_pc::Bounds2D bounds =
      params.choice("bounds") == "open" ? std_pc::Bounds2D::Open : std_pc::Bounds2D::Closed;

  const Eigen::Vector2f lo(box.min[0], box.min[1]);
  const Eigen::Vector2f hi(box.max[0], box.max[1]);
  std::vector<std::int32_t> keep;
  keep.reserve(in.pointCount());
  Ticker ticker(ctx, in.pointCount());
  for (std::size_t i = 0; i < in.pointCount(); ++i) {
    if (ticker.tick(i)) return Status::Ok();
    if (std_pc::insideBox2D(in.xyz[i * 3], in.xyz[i * 3 + 1], lo, hi, bounds)) {
      keep.push_back(static_cast<std::int32_t>(i));
    }
  }
  if (keep.empty()) {
    return Status::Error(Phase::Execute, "roi_empty", "框里一个点都没有", {}, "box");
  }
  outputs.set("cloud", Data::cloud(in.select(keep)));
  return Status::Ok();
}

}  // namespace

void registerFilterCropBox2D(Registry& r) {
  OperatorDesc op;
  op.id = "filter.crop_box2d";
  op.version = "1.0.0";
  op.label = "Crop Box 2D";
  op.category = "Filter/Crop";
  op.keywords = {"crop", "box", "roi", "2d", "裁剪", "感兴趣区域"};
  op.doc =
      "按 Box2D 在 XY 平面上裁剪（z 不参与）。\n"
      "Bounds 选 open 就是四边严格不等 —— 线扫量测域的既有语义，"
      "落在边上的点会被丢掉，非有限点也因为比较恒假而一并丢掉。裁空报 roi_empty。";
  op.inputs = {
      Port{"cloud", "PointCloud", "Cloud", "待裁剪的点云。", true},
      Port{"box", "Box2D", "Box", "裁剪框。", true},
  };
  op.outputs = {Port{"cloud", "PointCloud", "Cloud", "框内的点。", true}};

  Param bounds;
  bounds.name = "bounds";
  bounds.type = ParamType::Enum;
  bounds.label = "Bounds";
  bounds.doc = "边界怎么算。";
  bounds.def = Value::text("closed");
  bounds.options = {EnumOption{"closed", "Closed", "四边闭区间，边上的点留下。"},
                    EnumOption{"open", "Open", "四边严格不等，边上的点丢掉。"}};

  op.params = {bounds};
  op.capabilities = {/*cancellable=*/true, /*previewable=*/true, /*deterministic=*/true};
  op.compute = &compute;

  r.addOperator(std::move(op));
}

}  // namespace lyflow::ops
