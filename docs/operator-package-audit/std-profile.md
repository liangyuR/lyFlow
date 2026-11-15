# std-profile 逐算子迁移清单

状态：实施中；当前 3 项。原251/7规划基线通过机器清单baselineReference追溯。返回[清单总览](../operator-package-audit.md)。

下列顺序是迁移/验收顺序，不是执行图调度顺序。精确端口和参数见[机器清单](../operator-package-audit.json)。

## 020 · profile.from_cloud

- 当前：`std-profile` / `1.0.0` / 点云显式转换轮廓。目标：`std-profile`；保留并验边界；**M1**。
- 源码：[packs/std-profile/ops/register.cpp:167](../../packs/std-profile/ops/register.cpp#L167)。
- 端口：`cloud:PointCloud` → `profile:Profile`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core公开Profile/几何/Record契约与C++标准库；无PCL/OpenCV/ONNX Runtime直接依赖
- 当前契约：显式声明frame、m/mm、plane、handedness、sampleOrder与sourceId；共享只读PointCloud，保留全部槽与可选通道。有限XYZ标validity=1，NaN标0，来源索引0..N-1；不排序、不换轴、不压缩。
- 目标契约：原生Profile公共适配，8字段描述与validity/sourceIndices独立数组；调用方确认acquisition，不从点序猜测采集语义。
- 迁移前置：M0
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：1280槽/NaN/可选通道保留；缺单位/帧拒绝，Inf及非法通道拒绝；共享输入不变；轻量无PCL/CV/ORT。
- 说明：M1新增显式契约适配；不替代M3轮廓与DTS场景算法迁移。

## 021 · profile.remap_axes

- 当前：`std-profile` / `1.0.0` / 轮廓显式轴映射。目标：`std-profile`；保留并验边界；**M1**。
- 源码：[packs/std-profile/ops/register.cpp:214](../../packs/std-profile/ops/register.cpp#L214)。
- 端口：`profile:Profile` → `profile:Profile`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core公开Profile/几何/Record契约与C++标准库；无PCL/OpenCV/ONNX Runtime直接依赖
- 当前契约：按明确轴排列及+1/-1符号映射XYZ和法线，更新目标frame、plane与handedness；单位、有效性、槽序、sourceId和sourceIndices不变。
- 目标契约：反射与轴置换通过显式Profile适配，不伪装右手刚体Transform；不排序、不丢NaN或有限无效槽。
- 迁移前置：M0
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：6种轴排列及符号奇偶、反射往返、法线和源索引一致；非法符号或目标帧失败不发布输出；输入不变。
- 说明：M1新增显式契约适配；不替代M3轮廓与DTS场景算法迁移。

## 022 · profile.to_cloud

- 当前：`std-profile` / `1.0.0` / 轮廓显式导出点云。目标：`std-profile`；保留并验边界；**M1**。
- 源码：[packs/std-profile/ops/register.cpp:195](../../packs/std-profile/ops/register.cpp#L195)。
- 端口：`profile:Profile` → `cloud:PointCloud`, `descriptor:Record`, `profile:Profile`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core公开Profile/几何/Record契约与C++标准库；无PCL/OpenCV/ONNX Runtime直接依赖
- 当前契约：显式m/mm换算XYZ，法线不缩放且轴不旋转；裸PointCloud无效槽置NaN，同时输出完整Profile与仅含小元数据的profile.Descriptor.v1 Record。
- 目标契约：标准裸云消费者使用明确米制；完整Profile维持原始无效载荷、validity和sourceIndices，禁止宣称裸云+小Record可无损往返。
- 迁移前置：M0
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：mm↔m往返、有限无效槽仅裸云置NaN、通道/索引保留；溢出与取消不发布部分结果，上游缓冲不变。
- 说明：M1新增显式契约适配；不替代M3轮廓与DTS场景算法迁移。
