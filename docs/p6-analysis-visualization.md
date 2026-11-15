# P2 阶段6：结果查看与叠加

参考本机 `xyz-vision-lib` 的 `src/xyz_vision_lib/models/Visualization/{DataShow,SVGShowModel,VisualizeCloudVector,VisualizePrimitives2D,VisualizePrimitives3D}.cpp`、对应头文件、`include/xyz_vision_lib/FlowData.hpp`、`internal/signal_utils.cpp` 和 `property_settings.yml`。参考目录没有 Git 元数据，未把它认定为清单指定修订；实现只复用可核验的语义，未复制 Qt 控件、私有函数体和外部 xyz-viz 协议。

| 原条目 | 实际输入与模式 | LyFlow 实现与明确边界 |
|---|---|---|
| DataShow | 实际端口是 String，由 QTextBrowser 显示；并非通用工件计算节点 | 既有 Measurement/Record 检查器增加标准标量文本、工件身份、位置、方向、尺寸、置信度、pickable 与三维 Z 尺寸专用摘要；文本按普通文本显示。每页100字段，原始 JSON 可展开，不把高度尺寸当作现场测得高度。 |
| SVGShowModel | `flow_data::SVG` 包装 `xyz::viz::SvgPlot`；内部调用 addPoint/addLine/addCircle，是结构化绘图对象 | `visualization.SceneSpec.v1` Record 适配共享 SceneSpec，支持内嵌图像、点、线、圆、文本、帧与单位、SVG/PNG 导出及缩放。不是任意 SVG XML 文档入口；外部脚本、网络资源、Mesh、Rerun 与 xyz-viz 线协议仍属计划 P3 边界。 |
| VisualizeCloudVector | 全部/单云选择、固定随机颜色、显式输入帧、可选相机图像投影 | 复用原生 PointCloud + Indices + `cloud.Groups.v1`。主查看器按真实来源 ID 找父云，按稳定字符串组 ID 着色和选择，图像查看器使用显式相机/帧/外参投影；不靠相同点数或第一个输出猜分组来源。 |
| VisualizePrimitives2D | 图像与二维实例；旋转框、掩膜透明度、线宽、标签、方向箭头 | 共享工件适配器生成旋转框和方向；图像可选择实例 ID、标签、方向及二值 NHW 掩膜透明度，数值视图可独立查看和导出二维场景。掩膜只接受同一节点的唯一成对元数据/Tensor；歧义有诊断，不拼错数组。 |
| VisualizePrimitives3D | 图像与三维实例；位姿轴长度为各尺寸一半；STL mask/edge_mask/depth/normal；颜色、线宽、标签 | 查看器支持位姿轴、旋转尺寸盒、可选半透明尺寸实体与身份选择；图像通过标定投影盒/轴并可导出。既有 `image.render_model_3d` 增加 overlay/mask/edge_mask/depth/normal 主图模式及独立科学图像输出，STL 经 `io.load_cloud` 固定种子表面采样后送入；缺有效模型法线时不产 normal，normal 模式报诊断。尺寸实体和采样点投影不冒充 STL Mesh；不在 UI 新增显示型计算节点。 |

## 数据与坐标约束

- 源 PointCloud 本身没有 frame。只有 `cloud.Groups.v1` 明确绑定同一个实际 `sourceCloudId` 时，才能从该记录获取帧；否则工件与云的叠加需要用户明确选择点云坐标系。无云的工件场景一次只显示一个帧，不把不同帧实例拼在同一原点。
- PointCloud 元数据外层的 `sourceCloudId` 和原生 Indices 的 uint64 来源 ID 均按十进制字符串比较，保留大于 JavaScript 安全整数的身份。没有该元数据的旧宿主仍可看原生云，但不能可靠执行组索引着色。
- 分组映射仅在宿主声明 `cloudSampling: ordered-stride-v1` 后启用：请求 cap 对应 `stride=max(1,ceil(total/cap))`，第 n 个显示点来自原始第 n×stride 点。按请求上限记录 stride，不从返回点数反推；cap=0 的原生合同为全量。查看器主动限制组显示为200万点。
- 帧、米/像素、四元数、数组长度、唯一 ID、连续 offset/count 和所有索引范围严格校验。重叠组按声明顺序给全部视图的首个成员着色，单组选择仍保留该组的全部成员；空组显示为空，不回退全云。
- 图像坐标采用像素中心 `(0,0)`；SVG/canvas 像素中心在 `(0.5,0.5)`，新叠加统一加半像素。像素掩膜图像边界仍为 `[0,width]×[0,height]`，不额外偏移。
- 三维图像投影使用 `vision.CameraCalib.v1` 的行主序 K、0/4/5/8/12/14 项 OpenCV 畸变、相机分辨率与 frame。异帧需要 `{fromFrame,toFrame,matrix:[16]}` 的显式右手刚体；不能拿单位阵默默补缺失外参。相机后方或无效点不显示；穿越近裁面但端点不全可见的边整条略过，不伪造可见边界。
- 显示图像的 frame 需要用户明确确认。内参分辨率必须匹配原图名义像素尺寸；同尺寸不能证明图像来源，用户选择的是显式视觉叠加，不改工作流中的配准关系。NHW 掩膜只在正式原图分辨率一致时显示，预览缩放不合适时给诊断。
- 三维小尺寸线框先减去实例局部原点再存 float32 GPU 顶点，平移保存在对象变换；避免1024m位置把毫米尺寸量化。SVG 导出重新应用世界变换并保留工件 frame/ID。

## 查看与导出

1. 主查看器选择含 Pose3D 或米制 Pose2D 的节点，自动进入几何场景。通过“工件身份”选单体，切换位姿轴、尺寸实体与标签；多个坐标系需要选择当前场景帧。工件名称只用于显示，实际身份始终是 id。
2. 主查看器的“点云分组来源”列出运行结果中的 Groups；多个 Indices 时明确选择配套端口，再选择组 ID。云的 RGB/强度/法线与交互测量都使用过滤后的同一显示子集，不让不可见点继续被选中。
3. 图像查看器可切换当前节点多个原生 Image 输出，例如模型渲染的叠图、掩膜、深度或法线。既有 ImageCanvas 保留灰度/RGB/RGBA、u8/u16/f32、色带、缩放、原始像素读数和级别提示。
4. 图像叠加工具选择工件端口、图像帧、实例身份；三维工件选择相机标定，异帧填写明确外参。点云组投影使用绑定的父云与完整 Indices，并按独立云组身份着色；多个 Indices 时必须明确选择配套端口。叠加配置收在浮层中，避免占满窄预览栏；Image/Indices 混合输出可明确切换到图像视图。
5. 图像 PNG 将当前 ImageCanvas 的实际显示图与共享像素 SVG 叠加后导出；保持图像的现有色带与缩略级别，不声称是未经显示映射的原始 f32/u16 文件。结构化结果的 SVG/PNG 与界面使用同一 SceneSpec；三维 PNG 是当前原生 Three.js 画面，XY 导出保留显式工件帧和 ID。

## 资源上限与兼容

- 每组最多4096实例，分组索引最多16777216成员，组显示最多200万点，图像点云投影请求上限10万点；界面标注显示/原云点数。投影属于调试显示，无全量可见性或像素表面覆盖承诺。
- SceneSpec 最多16384图层、200万点/线端点；像素叠加/掩膜最多16777216像素/元素。三维标签最多显示前256实例，每条32字符；完整名称、身份和数值仍在检查器中。
- 全运行相关元数据最多16384端口、256节点、16MiB Record；超过预算显示明确问题，不拉取大型数组进入 JSON。点、索引、掩膜仍通过既有原生二进制接口获取。
- `getRuntimeCapabilities`、`getRunBindings` 均为可选能力。旧宿主或静态宿主能力未知时不猜分组抽样、不伪造云身份；原有原生云、图像与值预览继续可用。不需要联网、外部可视化服务或 CDN。
- 不自动修改工作流，不增加只负责显示的算子；工作流继续保存原生计算输出，查看选择属于编辑器当前会话。

## 回归证据

`packages/visualization/test/results.test.mjs` 按4个功能契约覆盖二维旋转真值、三维边/轴、局部原点精度、身份/帧/单位、各畸变长度、显式外参、后方剔除、lossless uint64 云来源、固定 stride、重叠/空组、非法索引、NHW有序身份、非二值拒绝、半像素对齐、文本转义与外部资源拒绝。`packages/editor/test/view-rule.test.mjs` 扩展既有功能组，保留主线回归，不增加顶层测试或跳过。

交叉审查补充了两类回归：N=0 的空掩膜仍单独受宽×高像素预算限制，RGBA 分配前拒绝大尺寸；异步图像契约受控延迟新运行的元信息与二进制载荷，旧元信息只携带对应运行的输出快照。新元信息已经到达、像素尚未更新时保留旧像素画面，暂停结果叠加、科学编码重绘和PNG导出；实际绘制的 epoch 与元信息一致后才恢复。工件参数拖框保持独立交互，不把待更新的计算结果套到上一幅图上。

真实组合例为 [p6-model-viewer](../examples/p6-model-viewer.lyflow.json)：自有 [STL立方体](../examples/p6-viewer-cuboid.stl)→4096固定种子表面采样→两个相机帧工件→原生 renderer，复用已有 p3 相机和图像。已实际 CLI 跑通，6115个命中像素，mask/edge_mask/depth/normal及quality全部可取；normal含负分量，Z深度为米，背景由mask区分。点模型深度不是封闭三角网格表面深度。

`scripts/e2e/p6.mjs` 复用项目 CDP 窗口桥，在真实 Edge、React StrictMode 和 HTTP/CLI 后端验证：原生 Pose3D 场景；上述STL例的4个科学图像端口；合成原生载荷的 uint64 父云绑定与组选择；结构化 SceneSpec 的缩放/SVG下载；二维旋转框与NHW掩膜/PNG下载；法线 `[0,0,-1]→RGB(128,128,0)` 且背景黑；跨帧三维叠图缺外参有诊断、显式外参后正常显示。合成传输夹具不冒充真实后端能力；HTTP桩本身保持 resultBindings=false，因为其重跑PCD路径无法证明原云ID和原生抽样。

浏览器实际发现并修复了 StrictMode 首次挂载清理撤销 SVG Blob URL 的破图问题；复验图像 naturalWidth、SVG与PNG文件实际落盘，并检查零运行时异常。证据位于本地 `build/p6-browser-evidence/evidence.json` 与对应截图，不作为运行库资产。浏览器专用 profile 在退出时清理；所有测试服务仅监听本机回环地址。上述软件验证不代替相机标定、工件遮挡、对称件匹配或现场视觉质量验收。
