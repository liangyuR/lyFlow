# glue 接入指南

本页说明宿主如何选择涂胶流程、注入图像和消费量测结果。算子参数与 Record / Bundle 字段见 [glue 算子包](../packs/glue/README.md)，低层测量和彩色流程见 [涂胶测量算子](glue-measure-ops.md)。部署与 ABI 兼容规则统一见 [运行库与 SDK 发包](deployment/packaging.md)。

## 选择流程

| 场景 | 参考图 | 宿主职责 |
|---|---|---|
| 随动相机，逐帧搜索胶路 | [glue1](../packs/glue/graphs/glue1.lyflow.json)、[glue2](../packs/glue/graphs/glue2.lyflow.json) | 给定喷嘴、搜索方向和检测区，核对胶路是否跟到了目标胶条。 |
| 固定拍照点，按示教折线检测 | [taught](../packs/glue/graphs/taught.lyflow.json) | 按拍照点提供示教线、检测区、容差、胶宽范围和标定。 |
| 固定相机飞拍，模板定位后逐点量测 | [flyshot](../packs/glue/graphs/flyshot.lyflow.json) | 管理模板、站点和标定文件；按站点身份消费结果，完成判定及跨帧缺陷合并。 |
| 显式配置方向、卡尺与测量策略 | [measure](../packs/glue/graphs/measure.lyflow.json) | 消费原始测量与质量信号，定义有无胶、方向回退、候选选择和产品判定。 |
| 彩色胶条 | [color](../packs/glue/graphs/color.lyflow.json) | 示教颜色模型，冻结生产配置，并用真实好件、缺陷件与干扰背景验收。 |

高层 `glue.bead_path → glue.bead_width → glue.bead_breaks / glue.edge_distance → glue.judge` 提供单帧检测。测量算子与飞拍图只负责定位、量测及质量信号，宿主不能把它们的执行成功直接当作产品合格。

## 图像输入与单位

- 图像坐标为像素，x 向右、y 向下，像素中心在整数坐标。位置与未标定长度均为 px，叠画始终使用像素坐标。
- 完整测量接受 u8 灰度、RGB 或 RGBA 图像；其他位深先经 `image.normalize`。相机 BGR 缓冲先转为 RGB。示教模板、站点和标定对应完整分辨率，不能把缩小预览当作正式测量。
- 图像采集属于宿主。入口 `io.load_image` 配 `source=inputs`，通过 C ABI / C++ / Rust 的图像注入接口传入 `n_load.image`。接口允许带行填充，core 将输入复制为自己的数据；具体字段与缓冲生命周期见 [嵌入指南](embedding.md)。
- 文件调试将入口切成 `source=file` 并设置 `path`。CLI 的 `--input` 用于点云，不能代替图像注入；图像文件通过入口参数加载。
- 图内 Path 参数相对图文件目录解析。模板、站点、标定和图应作为一套配置发布，不依赖进程当前目录。
- `calib` 使用 `Record<image.PlaneCalib>`，单应矩阵 H 将图像 px 映射到工作平面 mm。宽度和距离按映射后两点的距离计算，曲线长度按映射后的路径累计，不能只乘一个全局比例。
- 高层胶路中，`zone` 是沿胶路的弧长区间。切向指向远离喷嘴的方向，法向为 `(-t.y, t.x)`，卡尺 `lo / hi` 是沿法向的像素偏移。示教胶路的弧长从第一个示教点开始。
- 接标定后，量测及依赖量测单位的断口、判定阈值按 mm 解释；`tolerance`、图像搜索窗、站点位置等像素参数仍为 px。应以各参数声明的 unit 为准。

标定属于相机工位与工作平面。`image.board_calib` 从棋盘格生成标定，生产图通过 `image.load_calib` 读取宿主保存的文件；换型不应无条件重标，也不能把不同高度平面的单应混用。

## 示教胶路

`glue.taught_path` 不从图像里找胶，示教折线经重采样生成 `glue.Path`，下游与搜索胶路使用同一份量测实现。

- `points` 为 `[[x,y], …]`；至少两个有限点，总长不小于 8 px。`zone` 必须落在折线范围内。无效示教是加载期诊断，不是产品 NG。
- `PathInfo.lineSource = taught`、`headingSource = taught`、`ok = true`。搜索才有的角度、覆盖率、陡度、残差和喷嘴等字段为空，不能把它们当成实测质量。
- `glue.bead_width` 在 `tolerance` 内估计逐站偏移轨迹，优先选择有足够支持且靠近示教线的胶；参考宽度与峰值只取轨迹上的站。卡尺半长应满足 `searchHalf ≥ tolerance + widthRange[1] / 2`。
- 轨迹支持不足，或暗段边缘陡度不足时判为沿示教线没有胶。压痕、阴影和平行暗边可能与胶同宽同暗，不能仅凭宽度认胶。
- 连续横移段只有在偏移支持、邻域排他性及两端真实连接证据满足时才跟随，避免把断口旁的暗块接成胶；采样不足时仍可能报断口。
- `glue.judge` 对胶路未找到或一站都未量到胶统一报告 `missing`，不能因为关闭断胶长度判定而把整段无胶判为 OK。

每个拍照点分别保存示教线、检测区、允许偏移、胶宽范围、形态与标定配置。胶相对示教线的位置变化超过允许范围时，应重新核对采集、示教与配准策略，不能仅放宽容差来消除误报。零件刚体定位也不一定能补偿胶相对零件的独立位移。

## 飞拍定位与逐点卡尺

参考图由 `io.load_image` 向 `glue.locate` 和 `glue.station_calipers` 提供同一帧，卡尺另接定位结果及工位标定。宿主逐帧提供模板、`anchor`、`stations`、`calib`，不必为每帧改写图的结构。

### 模板和位姿

`glue.locate` 输出 `Record<glue.Pose2D>`，把示教图坐标变换到当前帧：

```text
T(p) = R(angle) · (p − teachCenter) + center
```

图像 y 向下，正角度在屏幕上为顺时针。`anchor` 是示教框左上角，不是带外圈模板文件的左上角。

新示教建议保存四边各带 4 px 外圈的模板，并设置 `templatePad=4`。外圈只为预平滑提供真实邻域，匹配、`teachCenter`、`templateSize` 和叠画均按内部示教框计算。`templatePad=0` 兼容无外圈模板，但框外紧邻强边时可能产生明显平移偏置；1–3 px 外圈会被加载期校验拒绝。示教框距图边至少为 `max(2, templatePad)` px，模板文件去除外圈后仍须有有效示教框。

定位失败以 `pose.ok=false` 和原因码表示，下游卡尺据此给出 `pose_fail`。无纹理模板、图像或搜索区域越界、低分、搜索贴边、转角超范围和歧义分别对应 `flat_template`、`out_of_image`、`low_score`、`at_search_edge`、`at_angle_edge`、`ambiguous`，按质量门顺序取先命中的原因。高分不能覆盖贴边或歧义失败。`peakRatio` 表示独立次峰与主峰的比值；`maxPeakRatio=1` 不启用歧义拒绝。

`angleObservable=false` 表示模板不能可靠约束角度，此时角度报 0、只补平移，不能解释为零件没有转动。模板离测量点较远的宿主必须核对未补偿转角带来的位移误差。`angleRange=0` 是主动关闭转角搜索，不能借其 `angleObservable=true` 证明实际转角为零。

角度搜索对各角度使用一致的预平滑、插值与亚像素相位比较；具体搜索护栏和细扫定义见 [定位内核](../packs/glue/algo/flyshot.h)。只找平移或角度不可观时仍存在亚像素峰值锁定误差，不能把合成测试中的容差承诺为所有现场模板的精度。

### 站点与结果

`stations` 是宿主发布的 JSON 文件，包含 `points[[x,y]]`、`normals[[nx,ny]]` 和可选 `ids[]`。法向从内边开口一侧指向翻边，负偏移为内边一侧。站点经位姿变换后，卡尺在 `innerSearch` 查内边、在 `beadSearch` 查目标胶条；`innerSelect` 由宿主按测量基准选择最近或最强内边。

结果提供逐站状态和内边、近胶边、远胶边的像素偏移；接标定后另给内边到近胶边、到胶中心及胶宽的物理距离。中心线或近边两种测量口径由宿主选择，不必重复运行视觉。胶边超出完整卡尺范围时是 `incomplete_bead`，不能把窗口边缘当胶边或补出合格胶宽。字段和状态以 [算子实现](../packs/glue/ops/station_calipers.cpp) 与 manifest 为准。

站点身份与跨帧弧长归属由宿主管理。宿主负责产品阈值、缺失站点处理、重复站位去重、跨帧断口合并，以及 PLC、数据库和 UI 的结果表达。

## 未通过站位的候选胶宽

`glue.bead_width` 的 `bead.stations` 提供与 `count` 等长的 `candidateLo`、`candidateHi`、`candidateWidthPx` 数组。前两项是相对本站 `center` 沿 `normal` 的像素偏移，后一项为两者之差，始终为 px。

通过站位与正式 `lo`、`hi`、`widthPx` 一致。未通过站位只有在完整半高双边落入 `searchHalf`、包围期望中心且中心偏差不超过 `max(6 px, centerRatio × 参考宽)` 时才保留最近候选；可信示教轨迹存在时以轨迹中心与参考宽为准，否则使用示教中心和 `widthRange` 上限。宽度超限也可作为候选保留，没有可信双边则三项为 null。

候选仅用于诊断显示，不能当作合格胶宽，也不能保证排除全部阴影。它不参与参考宽度、coverage、present、断胶或 NG 判定，不通过邻站插值制造宽度；未通过站位的正式宽度仍为 null。

## 构建与验证入口

```powershell
$env:LYFLOW_PACKS = 'glue'
pnpm core:build
pnpm check:glue
```

专项原生测试可在构建后执行：

```powershell
& .\build\core\bin\lyflow-core-tests.exe '--source-file=*test_flyshot.cpp,*test_calib.cpp,*test_glue.cpp,*test_color.cpp'
```

[scripts/check-glue.ps1](../scripts/check-glue.ps1) 检查构建、manifest、契约和参考图；给定 `LYFLOW_GLUE_DATA` 才执行仓库外的真实帧评估，人造断胶另行报告。当前修订是否通过以实际输出为准，命令存在不代表已运行。完整门禁、运行库交付与宿主消费验收分别见 [测试地图](testing.md) 和 [发包指南](deployment/packaging.md)。

单帧失败定位和批量评估可用 [MCP 涂胶调参流程](mcp-glue-tuning.md)。先区分执行错误、未量到和产品 NG，再比较阈值；冻结图、参数、输入与标定身份，避免把缓存命中耗时当作冷启动节拍。

## 现场接入边界

- 三目相机选择、机器人方向到各相机图像方向的转换，以及各相机的现场标定仍需真实采集与几何资料。相机扇区、方向偏角或覆盖率选择策略必须按现场数据定义，不把方案讨论当作已交付的自动选相机能力。
- 真实断胶、窄胶、宽胶、偏移及无胶干扰样本需要人工标注，并覆盖压痕、阴影、反光和位置变化。合成图与人造断口不能证明现场误检率、漏检率或量测准确率。
- 同一标定只适用于约定的相机、分辨率和工作平面。相机姿态、焦距或工件高度改变后，应重新验证标定及物理量误差。
- 胶本身相对零件移动时，需单独验证逐帧胶路配准与干扰排除；模板定位成功不代表胶已经正确对齐。
- 采集节拍、帧与工件关联、示教/配方发布、数据库迁移、报警和 PLC 写回属于宿主。LyFlow 与 GlueSight 等业务仓库独立发布和验收，不因本库测试通过而认定现场接入完成。
