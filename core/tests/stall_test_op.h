#pragma once
// 停不下来的测试算子 test.stall（docs/large-image-plan.md L1）：不理取消，睡满 ms 毫秒。
// 模拟一次进去就出不来的第三方调用（OpenCV 的霍夫找圆、PCL 的滤波），给「抢占不阻塞」的验收用。
// 与 image_test_op.h 同一个身份：编进 core，只在进程环境里 LYFLOW_TEST_OPS=1 时注册（e2e）。
#include <chrono>
#include <thread>

#include "lyflow/operator.h"
#include "lyflow/registry.h"

namespace lyflow::test {
namespace stall {

inline Status stallCompute(const Inputs&, const ParamView& params, Outputs& outputs,
                           ExecContext&) {
  // 故意不看 ctx.cancelled()：这正是它要模拟的
  std::this_thread::sleep_for(std::chrono::milliseconds(params.integer("ms")));
  Measurement slept;
  slept.value = static_cast<double>(params.integer("ms"));
  slept.ok = true;
  slept.unit = "ms";
  outputs.set("slept", Data::measurement(slept));
  return Status::Ok();
}

}  // namespace stall

/// 注册 test.stall。已经注册过就什么都不做。
inline void registerStallTestOp(Registry& r) {
  if (r.find("test.stall")) return;
  OperatorDesc op;
  op.id = "test.stall";
  op.version = "1.0.0";
  op.label = "停不下来";
  op.category = "Test";
  op.keywords = {"test", "stall", "cancel", "取消"};
  op.doc = "只在测试里注册：不理取消、睡满 ms 毫秒，模拟一次进去就出不来的第三方调用。";
  op.outputs = {Port{"slept", "Measurement", "Slept", "睡了多少毫秒。", true}};
  Param ms;
  ms.name = "ms";
  ms.type = ParamType::Int;
  ms.label = "ms";
  ms.def = Value::integer(1000);
  ms.min = 0;
  ms.max = 600000;
  ms.unit = "ms";
  op.params = {ms};
  // 不可取消、不确定：每次都真睡、不吃缓存 —— 验收要的就是「它正在算、停不下来」
  op.capabilities = {false, false, false};
  op.compute = &stall::stallCompute;
  r.addOperator(std::move(op));
}

}  // namespace lyflow::test
