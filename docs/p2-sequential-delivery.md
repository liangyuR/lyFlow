# P2 后续六阶段交付跟踪

[文档中心](README.md) · [补齐计划](operator-completion-plan.md) · [198项能力对照](operator-coverage.md)

用户已授权按下表 **1→6** 顺序实现，每阶段独立分支和 PR；当前阶段的实际功能、CI 与 Codex 审查全部通过并合并后，才开始下一阶段。P2 A01 的受限表达式已经由 [PR #53](https://github.com/liangyuR/lyFlow/pull/53) 合并，不再重复实施。

## 阶段与状态

| 顺序 | 任务族与完整交付范围 | 原P2条目数 | 状态 | PR |
|---|---|---:|---|---|
| 1 | A05：高光直线检测、碎片分组、有限线段、旋转区域及质量输出和叠图 | 1 | 进行中 | 待创建 |
| 2 | B01：相对/分位/顶层高度、平面规则；B02：全局配准、三维模板、边缘和可见面采样，并补既有拟合/局部搜索的高级模式 | 11 | 待开始 | — |
| 3 | B03：Marker识别/位姿、工作空间标定文件；B04：轮廓/梯度形状匹配 | 3 | 待开始 | — |
| 4 | B05：分割、OCR检测/校正/识别、条码/二维码、标注加载与对应后处理 | 5 | 待开始 | — |
| 5 | C01：深度/正交投影、索引关联、滤波填洞与缺陷/截面；B07：高阶位姿、对象裁云、几何投影和一致性；B06：合成盒云 | 19 | 待开始 | — |
| 6 | C02：Python类型适配与可移植示例；C03：结果叠加、实例/云集合查看；D01：FIFO会话、录制回放、通用结果适配；统筹B07位姿文件日志/发布 | 15 | 待开始 | — |
| 合计 | 原P2的55项减去已完成A01一项 | 54 | 逐项核验 | — |

顺序和阶段范围依据本次用户授权；B06的盒云归阶段5，B07日志/发布归阶段6，均保留原任务族与初始P2优先级，不因为归组改变算法或协议要求。阶段1的实现与边界见[高光分析](p2-highlights-analysis.md)。

## 每阶段的合并门禁

1. 逐项阅读可获得的 `xyz-vision-lib` 参考实现，区分已读实现、仅有声明、外部不可得依赖和需要业务样本的判据；只分析语义并独立实现，不复制 proprietary 函数体。本机目录没有 Git 元数据，不声称等同清单指定提交。
2. 以该阶段实际范围定义输入/输出、单位/帧、类型、资源上限、空结果、失败、取消和副作用；复用已有算子或宿主能力时，交付可运行的完整组合。高级模式未实现时必须继续列为未完成，不能以数据容器或注册成功代替算法。
3. 使用可追踪真值和适用的真实/公开样本验证几何误差、质量门限、误检漏检、遮挡/退化/无目标及非法输入；交付的模型、字典、格式或外部协议有明确支持矩阵。合成回归与公开模型图不能单独证明现场质量。
4. 实际运行适用的桌面/CLI/嵌入入口或共享执行链，核对参数覆写、结果、导出/回放及跨边界一致性；大数组仍保留原生载荷，不使用JSON/base64填补数据契约。变更ABI时先按[发布规则](deployment/packaging.md)完成兼容门禁。
5. 运行完整适用回归与必要格式/schema/打包布局检查；编辑器测试保持100项以下、无新增跳过。以审查所对应的最新提交为准，修复所有有效审查意见。
6. 确认当前阶段的功能证据、最新提交CI全部成功、Codex审查无未解决问题、包含最新main且无冲突，再以head修订保护合并。仅在确认GitHub已合并后将状态改为已合并，并开始下一阶段。

CI绿灯、manifest通过和测试数量只是证据的一部分；没有覆盖上述实际功能的窄测试不能证明整个阶段完成。不得为降低验收难度缩减已授权范围，也不把有意保留的协议或模式标成已支持。

## 54项范围归属

本表逐一保留原P2标识与任务族；每个标识仅出现一次。当前覆盖状态来自阶段启动时的对照表，用于保留已有子能力和缺口，不将“部分覆盖”重命名为“完成”。各行原有模式与证据以[逐项能力对照](operator-coverage.md)为准，后续PR必须同时维护该表的具体边界。

| 阶段 | 原任务族 | 外部标识 | 阶段启动时覆盖状态 |
|---|---|---|---|
| 1 | A05 | `DetectHighLights` | 缺失 |
| 2 | B01 | `CalculateHeightByNeighborCloud` | 缺失 |
| 2 | B01 | `CalculateTopLayerHeight` | 缺失 |
| 2 | B01 | `CloudBasedEmptyToteChecker` | 缺失 |
| 2 | B01 | `CloudToGeometry` | 部分覆盖 |
| 2 | B01 | `HeightExtraction` | 缺失 |
| 2 | B02 | `BestFit` | 部分覆盖 |
| 2 | B02 | `CloudEdgeExtract` | 缺失 |
| 2 | B02 | `CloudViewSampling` | 缺失 |
| 2 | B02 | `SearchPoseLocally` | 部分覆盖 |
| 2 | B02 | `SurfaceBasedEstimator` | 缺失 |
| 2 | B02 | `SurfaceBasedEstimatorV2` | 缺失 |
| 3 | B03 | `LoadWorkspaceInfo` | 缺失 |
| 3 | B03 | `MarkerPoseEstimation` | 缺失 |
| 3 | B04 | `SBMEstimator` | 缺失 |
| 4 | B05 | `BarcodeDetector` | 缺失 |
| 4 | B05 | `LoadAnnotations` | 缺失 |
| 4 | B05 | `OCR` | 部分覆盖 |
| 4 | B05 | `QrCodeRefine` | 缺失 |
| 4 | B05 | `SemanticSegmentor` | 部分覆盖 |
| 5 | B06 | `GenerateBoxCloud` | 缺失 |
| 5 | B07 | `CloudCutByPrimitives` | 缺失 |
| 5 | B07 | `CloudRemovalByPrimitives` | 缺失 |
| 5 | B07 | `Compare2Primitive3D` | 缺失 |
| 5 | B07 | `Extend2DInformation` | 缺失 |
| 5 | B07 | `FlipPrimitiveAxis` | 缺失 |
| 5 | B07 | `ProjectPrim3DTo2D` | 缺失 |
| 5 | B07 | `ProjectToPlane` | 部分覆盖 |
| 5 | B07 | `RigidDistanceCheck` | 部分覆盖 |
| 5 | B07 | `RotatePrimitiveAxis` | 缺失 |
| 5 | C01 | `CloudFillHoles` | 缺失 |
| 5 | C01 | `CloudROIExtract` | 缺失 |
| 5 | C01 | `ConstructHeightMap` | 缺失 |
| 5 | C01 | `ConstructOrthographicProjectionMap` | 缺失 |
| 5 | C01 | `DefectDetector` | 缺失 |
| 5 | C01 | `DepthFilter` | 部分覆盖 |
| 5 | C01 | `FindCircles` | 缺失 |
| 5 | C01 | `ProfileSlicer` | 缺失 |
| 5 | C01 | `ProjectCloudToCamera` | 部分覆盖 |
| 6 | B07 | `LogPrimitive2D` | 缺失 |
| 6 | B07 | `LogPrimitive3D` | 缺失 |
| 6 | B07 | `Primitive2DPub` | 缺失 |
| 6 | B07 | `Primitive3DPub` | 缺失 |
| 6 | C02 | `CustomPythonScript` | 部分覆盖 |
| 6 | C03 | `DataShow` | 部分覆盖 |
| 6 | C03 | `SVGShowModel` | 部分覆盖 |
| 6 | C03 | `VisualizeCloudVector` | 部分覆盖 |
| 6 | C03 | `VisualizePrimitives2D` | 部分覆盖 |
| 6 | C03 | `VisualizePrimitives3D` | 缺失 |
| 6 | D01 | `BasicDatatypesPub` | 部分覆盖 |
| 6 | D01 | `CacheData` | 缺失 |
| 6 | D01 | `LogString` | 部分覆盖 |
| 6 | D01 | `OutputPub` | 部分覆盖 |
| 6 | D01 | `RecordReplayer` | 部分覆盖 |

## 不得丢失的已有部分模式

- 阶段2的 `BestFit` / `SearchPoseLocally` 继续保留混合加权3-2-1、完整轴组合/分级搜索与外部工件批处理边界；已有刚体拟合或9初值ICP不等于这些模式完成。平面系数也不代替平面片几何或清框业务判据。
- 阶段3须区分相机K/D、像素平面H、Marker空间位姿与工作空间帧树/边界；NCC不等于轮廓形状匹配。原协议字段和缺少的标定来源需逐项验证。
- 阶段4须区分目标检测、实例掩膜、语义掩膜、旋转框与可抓区域；单Tensor ONNX执行不等于分割/OCR/条码完整链。原有未覆盖模式在相应任务中继续列明，不能默默删掉。
- 阶段5保留organized索引、遮挡、无效深度和补值标记；填洞不是实测深度。三维对称/跨帧比较、几何投影和刚体一致性要有各自定义，不能以矩阵或Pose容器代替。盒云不等于Mesh拓扑。
- 阶段6区分引擎结果缓存与有会话/顺序/容量的FIFO、图级结果与发布协议、当前SceneSpec图形与任意SVG文档。外部录制格式、端点、时间戳、容量/重置/取消和副作用均需实测；Python SDK可扩展不表示原16脚本已移植。

P0/P1仍保留的范围外模式与P3设备/机器人专项继续保留原优先级；本次不把设备协议、现场节拍、机器人安全或生产判定顺带宣告完成。阶段范围内尚未完成的模式必须继续工作并保留证据，不能仅凭“限定用途”措辞越过本交付门禁。
