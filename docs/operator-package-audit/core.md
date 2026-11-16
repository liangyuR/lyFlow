# core 逐算子迁移清单

状态：实施中；当前 4 项。原251/7规划基线通过机器清单baselineReference追溯。返回[清单总览](../operator-package-audit.md)。

下列顺序是迁移/验收顺序，不是执行图调度顺序。精确端口和参数见[机器清单](../operator-package-audit.json)。

## 001 · flow.fallback

- 当前：`core` / `1.0.0` / 容错回退。目标：`core`；保留并验边界；**M1**。
- 源码：[core/src/ops/flow_ops.cpp:106](../../core/src/ops/flow_ops.cpp#L106)。
- 端口：`a:Any`, `b:Any` → `out:Any`, `choice:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：a成功透传，仅a失败或无产出时demand备用b的上游闭包
- 目标契约：a成功透传，仅a失败或无产出时demand备用b的上游闭包；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：备用分支未demand不执行，事件skipped/not_demanded一致
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID

## 002 · flow.select

- 当前：`core` / `1.0.0` / 条件选择。目标：`core`；保留并验边界；**M1**。
- 源码：[core/src/ops/flow_ops.cpp:134](../../core/src/ops/flow_ops.cpp#L134)。
- 端口：`cond:Any`, `a:Any`, `b:Any` → `out:Any`, `choice:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：按Measurement.ok或Record中path指向boolean选择惰性闭包
- 目标契约：按Measurement.ok或Record中path指向boolean选择惰性闭包；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：未选分支不进入执行；cond错误不猜值，事件/取消/缓存行为一致
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID

## 003 · util.reroute

- 当前：`core` / `1.0.0` / 连线转接。目标：`core`；保留并验边界；**M1**。
- 源码：[core/src/ops/util_reroute.cpp:15](../../core/src/ops/util_reroute.cpp#L15)。
- 端口：`in:Any` → `out:Any`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：Any原样透传，无采样/类型/单位转换
- 目标契约：Any原样透传，无采样/类型/单位转换；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：各原生数据类型/Bundle透明透传，缓存和连线语义不变
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID

## 147 · gen.synthetic

- 当前：`core` / `1.0.0` / 点云合成。目标：`std-pointcloud`；迁移归属；**M6**。
- 源码：[core/src/ops/gen_synthetic.cpp:106](../../core/src/ops/gen_synthetic.cpp#L106)。
- 端口：无 → `cloud:PointCloud`。
- 参数单位标注：noise=m。
- 依赖：当前：core与C++标准库；目标：std-pointcloud portable-generation模块，不依赖PCL/CV/ORT
- 当前契约：米制XYZ，按seed生成同一片云；可选intensity，不能改变随机序列和通道对齐
- 目标契约：米制XYZ，按seed生成同一片云；可选intensity，不能改变随机序列和通道对齐；移出core到可独立启用的portable-generation，纯core最终仅保留三个调度/透传节点
- 迁移前置：M0；M1；M2；纯平台测试与预设去除对core生成器的假设
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：固定seed逐点回归；无PCL的portable构建可独立启用；纯core测试改用内部fixture
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID；不得因迁移而让测试云生成要求安装PCL；纯平台的4→3算子契约需显式更新
