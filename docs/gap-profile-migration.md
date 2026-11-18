# Gap 原生轮廓场景与旧图重搭

[分包迁移计划](operator-package-migration-plan.md) · [Profile 契约](profile-data-contract.md) · [公共算子](profile-public-operators.md)

本页说明 Gap 原生场景的运行契约与旧图重搭方式。模板、模型的合成夹具不能替代现场工件验收。

## 包与能力

场景源码位于 `solutions/gap`，包名为 `solution-gap`。它直接依赖 `std-profile`、`std-geometry`、`std-metrology` 和声明式内部模块，不通过注册表执行另一个包的算子。旧 `gap` 包名不再作为隐式别名接受。

| 能力           | 内容与依赖                                                                                                                                |
| -------------- | ----------------------------------------------------------------------------------------------------------------------------------------- |
| 基础场景       | 原生双头轮廓、相机选择、角色区域、定位结果及量测特征；`gap-geometry` 复用 PCL 几何，`gap-model-adapters` 处理模型特征与标签，不要求 ORT。 |
| `gap-template` | 模板读取、ICP、候选选择和业务 ROI；对实际模板 Profile 应用变换后才发布量测帧几何。                                                        |
| `gap-model`    | 常驻 ONNX 推理能力，经 `ml-runtime` 使用 ORT；没有此能力时不注册推理执行入口。                                                            |
| `gap-import`   | StandardGap YAML 导入，需要 yaml-cpp；选定模式的全部算子必须可用，否则导入失败并说明缺少的能力，不返回悬空工作流。                        |

SDK 随构建能力交付片段：基础场景为五份，模板与模型能力同时存在时增加原子定位回退片段；正式完整组合为六份。安装文件与 manifest 的能力集合保持一致。

例如只选择原生场景与模板能力：

```powershell
cmake -S core -B build/gap-template -G Ninja -DLYFLOW_STD_PACKS=OFF "-DLYFLOW_PACKS=solution-gap;std-profile;std-geometry;std-metrology" "-DLYFLOW_MODULES=gap-template" -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

PCL、Eigen 等依赖需先按[环境准备](getting-started.md)配置。这个命令说明能力选择，不代表该配置已经通过本机安装包验收。`packaging/release.json` 分别列出正式 `packs` 和 `modules`；自定义组合不能自称正式运行库。

## 数据与单位

原始传感器轮廓为原生 Profile：XYZ、可选通道、有效性掩码和原槽索引继续保存在数组载荷中。双头模型严格要求每头 1280 槽，不以压缩后的有限点补齐槽数。模型六通道中的颜色特征采用原 RGB.R/255，不能替换为原生 intensity。

文件单位、坐标系、轴布局及手性必须明确声明。传感器 XZ 到量测 XY 的换轴会改变手性；这是显式轴映射，不能伪装为右手刚体标定。`alreadyAligned` 仅记录调用方对现有坐标关系的声明，不能证明硬件已经标定。

| 原生组合值                | 数据契约                                                                                                               |
| ------------------------- | ---------------------------------------------------------------------------------------------------------------------- |
| `gap.ScanPair.v3`         | 原始两路传感器 Profile、量测帧两路 Profile、合并 Profile 和小型处理元数据。raw/processed 阶段明确区分。                  |
| `gap.RoiSet.v3`           | datum、target、seamLeft、seamRight 四个米制 Box2D，各自带 `profile.Geometry.v3` 空间上下文；info 保存定位方式及处理状态。 |
| `gap.LineFeature.v3`      | 轮廓、直线、端点、几何上下文、唯一内点、多重权重内点和质量一起传递。                                                     |
| `gap.ReferenceFeature.v3` | 参考点与选点方法；非拟合方法的显示线携带不可量测的 GeometryUnavailable，不冒充真实拟合直线。                             |
| `gap.CirclePair.v3`       | 实际选中的双侧圆、对应观测轮廓、内点及候选质量。                                                                         |
| `gap.Location.v3`         | 定位后的轮廓与四角色区域作为一个原子值选择，避免不同回退路径混配。                                                       |

组合字段的 schema 版本与算子版本分别管理。端口或数据契约改变的节点升主版本：上一主版本的图由 `lyflow migrate` 删除已退役端口的连线（如各类 `provenance`、`mergedProvenance`、`leftProvenance`）后原样升级；更早的裸点云主版本仍要求图级升级。

几何上下文 `profile.Geometry.v3` 只记录坐标帧、分析平面和手性，用于判断几何与轮廓是否处于同一空间；不携带内容指纹或来源集合。组合值不再保存 profile/binding/geometry 哈希，读取方不重算、不比对已产出的结果。

合并轮廓保持 secondary 在前、primary 在后，`sourceId` 为 `derived:composite/` 后按顺序以 `+` 连接的父来源，原槽索引保存在原生数组中。半径滤波之后的随动裁剪继续裁已有 merged，不能重新合并以恢复已删除样本。共享可选通道按实际有效贡献取交集，不给缺失通道填零。

## 编排与结果

StandardGap 提供自动、模板、模型三种模式，各有高层与细粒度入口。细粒度图使用公共 Profile 选择/拟合与公共度量节点；高层场景入口调用相同的 typed helper，不在内部查找、运行注册节点。距离、半径等旧毫米参数在明确适配处换算，公共物理几何采用米，最终读数可显式输出毫米。

模型与模板定位通过 Location 原子回退，直线与参考点通过各自的原子特征回退。圆拟合保持原候选、弱拟合地板与相机选择策略；不能额外引入一次模板圆重试。惰性备用路径只有在真实需求发生时才运行。旧细粒度图若实际选出的 scan/ROI 路径不一致，升级后明确报 `location_path_mismatch`，不偷偷重选路径。

`gap.result_bundle` 算子 4.0.0 输出 `GapResultBundle` schemaVersion 4，不再附带 `source_evidence`。缺失统计、时序和一致性信息使用 null/不可用状态，不能合成为 0 或“已通过”。唯一内点和最终多重内点分别报告；旧 CSV `inlier_count` 保持最终多重权重口径。局部特征回退与定位回退分开记录，不能让一次直线重试改变 `roi_source`。

ROI 底图可引用原生 Profile 或 Bundle 的单层采样字段，例如 `templates.left`、`templates.right`；显示按声明的分析平面、单位及有效性掩码取数。计算失败的部分输出不进入正式结果仓，不能把诊断候选描述为可发布量测。

## 旧图重搭与策略节点

当前发行版通过 StandardGap 导入器生成可运行的原生图。早期裸点云图可作为业务与参数参考。重新搭图时，从已知资料确认传感器和模板的单位、采样槽序、分析平面、手性及坐标帧。旧入口诊断会定位节点并给出替代路径。

- 细粒度方向基准窗口使用 `geometry.anchored_window`，将配置中的起点、长度和高度扩展由毫米换算为米。几何节点输出 `box/context`。选用哪个角色窗口由 StandardGap 图决定。
- 原始轮廓经 `profile.compare_overlap` 生成独立观测。Gap 的 `gap.select_camera` 应用记录、择相机或报错策略，检测上限仍以毫米配置，保留实际传感器身份。
- 段差和间隙读数经 `metrology.compare_tolerance`、`gap.apply_patrol` 后进入命名输出与结果汇总。顶层 `patrol` 默认关闭，打开时已超差的读数输出 `margin`。

模型与模板定位仍按各自能力注册。细粒度模型的 `gap.infer_profile` 保留双头有限输入、类数与固定槽序的错误契约。六组 Bundle 的组装与拆分入口都由当前 StandardGap 导入图或片段实际消费，各角色始终取自选中的原子定位结果。
