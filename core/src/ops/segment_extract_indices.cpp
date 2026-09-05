#include "ops.h"

namespace lyflow::ops {
namespace {

Status compute(const Inputs& inputs, const ParamView&, Outputs& outputs, ExecContext&) {
  const PointCloud& cloud = *inputs.get("cloud").asCloud();
  const Indices& indices = *inputs.get("indices").asIndices();

  // 下标对账。这是 PointCloud::id 存在的全部理由：
  // 把「一片云的下标喂给另一片云」这种错误变成一句人话，而不是一堆
  // 越界跳过之后「结果少了一半点，但没人报错」。
  if (indices.sourceCloudId != cloud.id) {
    return Status::Error(Phase::Execute, "bad_input",
                         "这组下标不是从当前输入点云上取的（中间可能插了一个会重建点云的算子）",
                         {}, "indices");
  }

  outputs.set("selected", Data::cloud(cloud.select(indices.values)));
  outputs.set("rest", Data::cloud(cloud.selectInverse(indices.values)));
  return Status::Ok();
}

}  // namespace

void registerSegmentExtractIndices(Registry& r) {
  OperatorDesc op;
  op.id = "segment.extract_indices";
  op.version = "1.0.0";
  op.label = "Extract Indices";
  op.category = "Segment";
  op.keywords = {"extract", "indices", "split", "select", "提取", "下标", "分离"};
  op.doc = "按下标把点云一分为二。两个输出端口同时给出，不需要 negative 参数。";

  op.inputs = {
      Port{"cloud",   "PointCloud", "Cloud",   "源点云。", true},
      Port{"indices", "Indices",    "Indices", "要提取的点下标。", true},
  };
  // 两个输出而不是一个「negative」开关：调试时你几乎总想同时看两边，
  // 一个开关会逼用户复制一份节点、反着设一次参数，还得记住它们要同步改。
  op.outputs = {
      Port{"selected", "PointCloud", "Selected", "下标选中的点。", true},
      Port{"rest",     "PointCloud", "Rest",     "其余的点。", true},
  };

  op.capabilities = {/*cancellable=*/true, /*previewable=*/true, /*deterministic=*/true};
  op.compute = &compute;

  r.addOperator(std::move(op));
}

}  // namespace lyflow::ops
