# std-ml 逐算子迁移清单

状态：实施中；当前 1 项。原251/7规划基线通过机器清单baselineReference追溯。返回[清单总览](../operator-package-audit.md)。

下列顺序是迁移/验收顺序，不是执行图调度顺序。精确端口和参数见[机器清单](../operator-package-audit.json)。

## 229 · ml.onnx_run

- 当前：`std-ml` / `1.0.0` / ONNX 推理。目标：`std-ml`；保留并验边界；**M6**。
- 源码：[packs/std-ml/ops/ml_onnx_run.cpp:43](../../packs/std-ml/ops/ml_onnx_run.cpp#L43)。
- 端口：`input:Tensor` → `output:Tensor`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前：ONNX Runtime 1.32.0 CPU与core Tensor；目标：ml-runtime内部后端模块（可选），adapters-only不链接ORT
- 当前契约：float32 Tensor单输入/输出；按模型指纹及线程预算复用会话；serial模式有限批次，取消不能中断已进入ORT的调用
- 目标契约：float32 Tensor单输入/输出；按模型指纹及线程预算复用会话；serial模式有限批次，取消不能中断已进入ORT的调用；运行会话由内部ml-runtime服务提供；纯张量适配器可独立构建，不让std-ml反向依赖std-image注册
- 迁移前置：M0；M1；M2；M4；M5；共享Tensor/检测/OCR协议公开化
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：动态/固定batch、空批次、模型变化、会话隔离与只读输入回归；adapters-only构建不加载ORT
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID；原ORT缓存/线程/异常/取消边界均是兼容门槛，不假定能够强制中断ORT
