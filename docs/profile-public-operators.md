# 公共 Profile 选择、稳健拟合与度量

M4b 随产品 1.4.4 增加 10 项公共算子：6 项 Profile 能力、3 项度量及 1 项 Box2D 构造。C ABI 布局保持 1.4 系列兼容。`std-profile`、`std-metrology` 为 0.3.0，`std-geometry` 为 0.6.0；旧 Gap 适配包为 0.2.1。

## 注册与依赖

| 算子                         | 契约                                                                                |
| ---------------------------- | ----------------------------------------------------------------------------------- |
| `geometry.box_2d`            | 构造单位明确的轴对齐框；边界须有限且 min≤max，不自动交换。                          |
| `profile.bind_geometry`      | 调用方显式确认 frame、plane，将米制点/线/圆/框绑定到 Profile 来源；不做缩放或换轴。 |
| `profile.nearest_sample`     | 全部有效槽中按锚点欧氏距离选样本；等距保留原槽序。                                  |
| `profile.nearest_to_line`    | 按无限直线垂距选有效样本，不限幅到显示线段。                                        |
| `profile.filter_validity`    | 按掩码及 XYZ 有限性保槽或压缩，保持所有通道和原源索引。                             |
| `profile.fit_line_robust`    | 双迹线 RANSAC、近端选段重拟合、方向约束及中位回退；保留最终多重内点权重。           |
| `profile.fit_circle_robust`  | 固定种子 RANSAC/LM，自由、固定半径、软先验及半径夹限明确分开。                      |
| `metrology.flush_point_line` | 有符号点线面差、垂足及显示段，显式正法向和补偿顺序。                                |
| `metrology.circle_gap`       | 圆心连线边界距离或沿基准段方向的切线距离。                                          |
| `metrology.point_offset`     | 同空间两点的两轴位移和未补偿欧氏距离，支持零位移。                                  |

选择、几何和度量不需要 PCL/OpenCV/ONNX Runtime。两项稳健拟合仅在 `profile-robust-kernel` 进入实际模块闭包后注册；默认轻量配置不创建占位节点。标准点云或 Gap 已请求此公共核时，共享相同实现；独立使用可显式选择：

```powershell
python scripts/fetch-profile-math-deps.py --output-dir build/profile-math-deps
cmake -S core -B build/profile-public -G Ninja -DLYFLOW_STD_PACKS=OFF "-DLYFLOW_PACKS=std-profile;std-metrology;std-geometry" -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

上述配置启用 14 项 Profile 算子。另加 `-DLYFLOW_MODULES=profile-robust-kernel` 并提供已安装的 PCL 后端，才包含两项稳健拟合，共 16 项。当前完整审计组合包含所有场景包，共 271 项原生算子；不同构建的可用集合以实际 manifest 为准。`LYFLOW_MODULES` 不代替算子包选择，且不会隐式注册模型推理或场景节点。

## 单位、坐标与来源

Profile 仍使用[完整原生契约](profile-data-contract.md)：m/mm、XY/XZ/YZ、frame、handedness、sourceId、sourceCount、validity 和 sourceIndices。计算时按声明映射分析平面并换算到米，输出几何真实为米。图像像素几何不能直接送入这些物理度量节点。

`profile.Geometry.v1` 的点线约束和严格校验保持原样。新增 `profile.Geometry.v2` 支持点、线、圆、框，绑定几何类型、米制单位、实际内容指纹及 1–64 项唯一来源上下文。所有来源须属于同一 frame、plane 和 handedness；原始载荷的 m/mm 可以不同，因为输出几何已统一为米。来源上下文包含小型描述和内容 hash，大数组继续保留在原生 Profile。

`relation=declared` 表示调用方确认外部几何与该轮廓共用物理空间，不能当作标定正确或由该轮廓拟合得到的证明。拟合和派生测量使用 `relation=derived`，保留参与输入的全部来源，合并去重后超过 64 项明确失败。改几何内容后继续使用旧上下文、混用帧/轴/手性或缺上下文均拒绝。

最近样本输出小型 `profile.SampleSelection.v1`：原 slotIndex、物理 sourceIndex、原 cloud.id 和米制距离；完整数组仍从透传的原 Profile 读取。有效性筛选的 valid/invalid 是原始 cloud.id 上的槽号，不能直接应用到压缩后的新点云。默认 `preserve_slots` 保持输入共享且 removedCount=0；显式 `compact` 按原序选择有效槽，保留 sourceCount、sourceIndices、强度、法线与 RGB。

## 方法与测量定义

`profile.fit_line` 继续采用 TLS。`profile.fit_line_robust` 使用独立的 RANSAC 方法与方向策略；不能仅因都输出直线而互换。稳健直线的 box 决定端点口径，输入轮廓应已按需要选区，不能把该参数当作隐式裁剪。

固定方向回退可产生重复的最终内点权重。`inliers` 是去重后的观察槽号，`weightedInliers` 保留最终拟合及质量计算的权重；它不表示 RANSAC 内部抽样轨迹。`innerEnd` 仍取自真实原槽。圆的固定半径与软先验分别处理，不用 TLS 或代数圆替代 RANSAC/LM。

面差的 `positive_normal` 按显式正法向定符号；`canonical_axis` 采用方向朝 +x、竖直朝 +y 后的右法向。关闭 signed 时先取距离绝对值，再乘 gain、加 offsetMm；最后按 outputUnit 输出 m 或 mm。几何和补偿的单位不能互相替代。

圆间隙的 `center_line` 沿圆心连线计算边界端点；保留原 left/right 与分析横轴口径，左端点 x 大于右端点时给无效 NaN。`datum_tangents` 需要非退化基准段与上下文，切线交叉明确拒绝。左右角色不自动排序。位移分量使用 b−a，先增益、再按开关取绝对值、再加毫米偏置；两点欧氏距离不使用这些补偿。所有结果保持有效性和原因，不附加业务公差或合格判定。

## 旧 Gap 行为与迁移边界

旧节点保留现有端口和参数，并调用同一公开 C++ 核。以下算子 patch 版本使新的输入约束进入缓存身份：

| 原 ID              | 本轮版本 | 变化                                           |
| ------------------ | -------- | ---------------------------------------------- |
| `gap.fit_line`     | 1.1.1    | NaN 无效样本不参与直线流程；原槽号仍对应输入。 |
| `gap.flush`        | 1.1.1    | 米制/有限几何和补偿校验，非法及溢出明确拒绝。  |
| `gap.gap`          | 1.0.1    | 重合圆心、像素和无法表示的几何明确拒绝。       |
| `gap.point_offset` | 1.0.1    | 明确米制及有限结果，非法增益/偏置和溢出拒绝。  |

冻结旧运行库对照中，42 个已有定义直线场景的字段及浮点位一致；量测对照 293 个场景中，281 个已有定义场景一致，另有 12 个预期变化：8 个重合圆心、3 个像素单位、1 个 float 溢出。已有定义场景包含预期失败边界，不能把 281 解释为全部成功的测量，也不能宣称所有历史输入兼容。这些是软件数值与错误契约证据，不是现场精度或吞吐验收。

M4b 不删除旧 8 个通用 Gap ID。`gap.fit_line`、`gap.selected_point`、`gap.nearest_to_line`、`gap.drop_non_finite`、`gap.to_measurement_frame`、`gap.flush`、`gap.gap`、`gap.point_offset` 的图、Bundle 和场景消费者切换放到 M4c；本轮不把历史图静默重标为新端口。`solution-gap`、双头 `[2,6,1280]` 槽协议、相机来源/逻辑角色、模板/模型模块、六种导入器和六个片段仍须独立迁移验收，见[完整计划](operator-package-migration-plan.md)。

公开静态接口为 `profile_features.h`（`lyflow::profile_robust_kernel`）和 `profile_dimensions.h`（`lyflow::metrology_kernel`），无需 Registry、执行图或加载 core DLL；安装 SDK 用法和隔离消费门禁见[公共内核](public-kernels.md)。

可执行的[原生Profile示例](../examples/m4-native-profile.md)随附GraphDoc、CSV和独立C++宿主，演示原生源、外部几何声明、稳健拟合及量测的同一条链。
