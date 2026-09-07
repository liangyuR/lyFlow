#include "ops.h"

namespace lyflow::ops {
namespace {

Status compute(const Inputs& inputs, const ParamView&, Outputs& outputs, ExecContext&) {
  outputs.set("out", inputs.get("in"));
  return Status::Ok();
}

}  // namespace

void registerUtilReroute(Registry& r) {
  OperatorDesc op;
  op.id = "util.reroute";
  op.version = "1.0.0";
  op.label = "路由";
  op.category = "工具";
  op.keywords = {"reroute", "pipe", "wire", "整理", "转接", "布线"};
  // 不是特殊节点类型，就是一个 Any → Any 的普通算子（E5）。实际类型由连线推导。
  op.doc = "把输入原样传给输出。只为整理连线，端口类型跟着连上的那一端走。";

  op.inputs = {Port{"in", "Any", "In", "任意类型的输入。", true}};
  op.outputs = {Port{"out", "Any", "Out", "与输入同一份数据，零拷贝。", true}};
  op.capabilities = {/*cancellable=*/false, /*previewable=*/true, /*deterministic=*/true};
  op.compute = &compute;

  r.addOperator(std::move(op));
}

}  // namespace lyflow::ops
