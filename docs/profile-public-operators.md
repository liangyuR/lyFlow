# 公共 Profile 选择、稳健拟合与度量

M4b 随产品 1.4.4 增加 10 项公共算子：6 项 Profile 能力、3 项度量及 1 项 Box2D 构造。M4c 随产品 1.4.5 增加下列五项公开图节点，并把 Gap 消费者切换到原生契约；C ABI 布局保持 1.4 系列兼容。当前 `std-profile` 为 0.5.0，`std-geometry` 为 0.7.0，`std-metrology` 为 0.4.0；场景边界见 [Gap 原生迁移](gap-profile-migration.md)。

| M4c 新 ID                | 契约                                                                                        |
| ------------------------ | ------------------------------------------------------------------------------------------- |
| `geometry.box_anchor`    | 从明确的 Box2D 得到指定锚点，传递完整几何来源，不依赖工件角色。                             |
| `profile.select_indices` | 按实际输入轮廓上的索引选择，保留原槽身份和通道，不把原始索引当作当前数组下标。              |
| `profile.merge`          | 显式合并顺序与可选通道策略，输出新 Profile 及不重叠的原始索引区间。                         |
| `profile.crop_box`       | 米制 ROI 与同空间上下文成对输入，选择已有观测，输出实际索引。                               |
| `profile.radius_outlier` | 显式 PCL 半径滤波，复用标准点云数值实现并保持 Profile 采样槽；没有后端时不注册占位入口。    |

std-profile 0.5.0 / std-metrology 0.4.0 删除了来源集合、`profile.CompositeSources.v1`、全部 `provenance` 端口及内容/配方/绑定指纹。涉及的算子升为 2.0.0，旧图经 `lyflow migrate` 删除连到已退役输入（`provenance`、`aProvenance`/`bProvenance`、`referenceGeometry`/`referenceContext`）的边，`profile.merge` 同时删除 `roleA`/`roleB` 参数。信息 Record 去掉内嵌轮廓上下文与指纹并升版本，例如 `profile.LineFit.v2`、`profile.Selection.v2`、`profile.SampleSelection.v2`、`profile.LineFitRobust.v2`、`metrology.FlushPointLine.v2`。

## 注册与依赖

| 算子                         | 契约                                                                                |
| ---------------------------- | ----------------------------------------------------------------------------------- |
| `geometry.box_2d`            | 构造单位明确的轴对齐框；边界须有限且 min≤max，不自动交换。                          |
| `profile.bind_geometry`      | 调用方显式确认 frame、plane，输出米制点/线/圆/框所在空间的上下文；不做缩放或换轴。  |
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

轻量配置不注册需要 PCL 的半径滤波及稳健拟合；显式选择相应模块并配置后端后才提供这些节点。M4b 的完整审计组合为 271 项原生算子，这是该阶段的历史快照；当前构建的可用集合以实际 manifest 为准。`LYFLOW_MODULES` 不代替算子包选择，且不会隐式注册模型推理或场景节点。

## 单位、坐标与来源

Profile 仍使用[完整原生契约](profile-data-contract.md)：m/mm、XY/XZ/YZ、frame、handedness、sourceId、sourceCount、validity 和 sourceIndices。计算时按声明映射分析平面并换算到米，输出几何真实为米。图像像素几何不能直接送入这些物理度量节点。

几何上下文统一为 `profile.Geometry.v3`，只含 `schemaVersion`、`frame`、`plane`、`handedness`，供消费者判定几何与轮廓或其他几何处于同一物理空间。几何本身须为米制、坐标有限且非退化（零长线段、零方向、非正半径、min>max 的框均拒绝）；原始载荷的 m/mm 可以不同，因为输出几何已统一为米。上下文不绑定几何内容，也不记录来源；`profile.bind_geometry` 只表示调用方确认外部几何与该轮廓共用物理空间，不能当作标定正确或由该轮廓拟合得到的证明。混用帧/轴/手性或缺上下文均拒绝。

`profile.merge` 输出的 `sourceId` 为 `derived:composite/` 后按 a、b 顺序以 `+` 连接的父来源；父来源本身是复合轮廓时展开其后缀，例如 a 与（b+c）合并得到 `derived:composite/a+b+c`，`sourceCount` 为两者之和。

最近样本输出小型 `profile.SampleSelection.v2`：原 slotIndex、物理 sourceIndex、原 cloud.id 和米制距离；完整数组仍从透传的原 Profile 读取。有效性筛选的 valid/invalid 是原始 cloud.id 上的槽号，不能直接应用到压缩后的新点云。默认 `preserve_slots` 保持输入共享且 removedCount=0；显式 `compact` 按原序选择有效槽，保留 sourceCount、sourceIndices、强度、法线与 RGB。

## 方法与测量定义

`profile.fit_line` 继续采用 TLS。`profile.fit_line_robust` 使用独立的 RANSAC 方法与方向策略；不能仅因都输出直线而互换。稳健直线的 box 决定端点口径，输入轮廓应已按需要选区，不能把该参数当作隐式裁剪。

固定方向回退可产生重复的最终内点权重。`inliers` 是去重后的观察槽号，`weightedInliers` 保留最终拟合及质量计算的权重；它不表示 RANSAC 内部抽样轨迹。`innerEnd` 仍取自真实原槽。圆的固定半径与软先验分别处理，不用 TLS 或代数圆替代 RANSAC/LM。

面差的 `positive_normal` 按显式正法向定符号；`canonical_axis` 采用方向朝 +x、竖直朝 +y 后的右法向。关闭 signed 时先取距离绝对值，再乘 gain、加 offsetMm；最后按 outputUnit 输出 m 或 mm。几何和补偿的单位不能互相替代。

圆间隙的 `center_line` 沿圆心连线计算边界端点；保留原 left/right 与分析横轴口径，左端点 x 大于右端点时给无效 NaN。`datum_tangents` 需要非退化基准段与上下文，切线交叉明确拒绝。左右角色不自动排序。位移分量使用 b−a，先增益、再按开关取绝对值、再加毫米偏置；两点欧氏距离不使用这些补偿。所有结果保持有效性和原因，不附加业务公差或合格判定。

## 旧 Gap 行为与迁移边界

M4b 曾保留旧节点的端口和参数并改为调用同一公开 C++ 核；下表记录该阶段的缓存隔离版本，不能作为当前原生场景可直接使用旧节点的说明：

| 原 ID              | 本轮版本 | 变化                                           |
| ------------------ | -------- | ---------------------------------------------- |
| `gap.fit_line`     | 1.1.1    | NaN 无效样本不参与直线流程；原槽号仍对应输入。 |
| `gap.flush`        | 1.1.1    | 米制/有限几何和补偿校验，非法及溢出明确拒绝。  |
| `gap.gap`          | 1.0.1    | 重合圆心、像素和无法表示的几何明确拒绝。       |
| `gap.point_offset` | 1.0.1    | 明确米制及有限结果，非法增益/偏置和溢出拒绝。  |

冻结旧运行库对照中，42 个已有定义直线场景的字段及浮点位一致；量测对照 293 个场景中，281 个已有定义场景一致，另有 12 个预期变化：8 个重合圆心、3 个像素单位、1 个 float 溢出。已有定义场景包含预期失败边界，不能把 281 解释为全部成功的测量，也不能宣称所有历史输入兼容。这些是软件数值与错误契约证据，不是现场精度或吞吐验收。

M4c 将旧 8 个通用 Gap ID 从正式注册中移除。历史图需要明确来源声明和 `gapProfile.v1` 整图升级，不能静默重标为新端口。双头 `[2,6,1280]` 槽协议、相机来源/逻辑角色、模板/模型模块、六种导入器和六个片段的验收状态见[完整计划](operator-package-migration-plan.md)。

公开静态接口为 `profile_features.h`（`lyflow::profile_robust_kernel`）和 `profile_dimensions.h`（`lyflow::metrology_kernel`），无需 Registry、执行图或加载 core DLL；安装 SDK 用法和隔离消费门禁见[公共内核](public-kernels.md)。

可执行的[原生Profile示例](../examples/m4-native-profile.md)随附GraphDoc、CSV和独立C++宿主，演示原生源、外部几何声明、稳健拟合及量测的同一条链。
