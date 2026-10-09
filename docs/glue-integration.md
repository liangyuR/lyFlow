# glue 主线集成验收

本轮基于主线 `362a6d5`，从原 `feat/glue`（`d65de45`）迁移领域算子。保留主线 Image ABI v15、共享图像缓冲、u8/u16/f32 数据域及现有编辑器；不合入旧 G1 平台实现。

## 本轮实现

- `packs/glue` 默认关闭，包含胶路、胶宽、断胶、边距、判定、人造断胶，以及飞拍 `glue.locate` / `glue.station_calipers` 共 8 个算子。
- `std-image` 增加 `image.board_calib`、`image.load_calib`，统一经 `lyflow_opencv_support` 链接 OpenCV。`io.load_image` 默认文件行为保留，新增 `source=inputs` 接收上游或宿主内存注图。
- 纯色及标准差不足 1 灰度级的模板返回失败位姿，避免 OpenCV 对常量模板给出 NCC=1。胶边超出卡尺窗口返回 `incomplete_bead`，不再返回带空胶宽的 `ok` 或把窗口边缘当胶边。
- 旋转精调先初始化粗扫分数，避免首次比较读取未初始化的栈内存；增加 ±0.12° 的小角度回归。
- 空/非法站点文件、错误位姿、错误标定 Record 类型、非法棋盘尺寸有定位到字段/端口的错误。图像需要全分辨率 u8（灰度、RGB、RGBA）；不将 u16/f32 内存误读为 u8，不把缩小预览当正式测量。
- 实际随包飞拍图通过 C ABI 注入带行填充的相机缓冲、绑定模板/测量点/标定参数，读取毫米点表。连续的有工件/无工件帧验证缓存不会串帧。

## 复现

```powershell
$env:LYFLOW_PACKS = 'glue'
pnpm core:build
& .\build\core\bin\lyflow-core-tests.exe '--source-file=*test_flyshot.cpp,*test_calib.cpp,*test_glue.cpp,*test_image_ops.cpp'
pnpm check:glue
```

`pnpm check:glue` 会显式检查 9 个 glue 算子（含下面的示教胶路）和 2 个标定算子都在 manifest 中。给了 `LYFLOW_GLUE_DATA` 才跑仓库外的客户帧评估；缺少现场数据时不会声称已验证真实 NG。

文件输入使用 `--set 'n_load.source="file"' --set 'n_load.path="C:/frames/k0.png"'`；宿主注图则保持 `source=inputs`，传 `RunImageInput`。当前 CLI 的 `--input` 仍用于点云。叠画导出是 `lyflow.overlay2d` Record，人造断胶工具按主线 `.lyim` 协议取图。

## 验证记录（2026-10-08）

- 本地 OpenCV 4.12.0；算子自检 49 个算子通过。
- glue、飞拍、标定、std-image 专项：41 个用例、5775 条断言通过。包括 `io.load_image source=inputs` 的 u8/u16/f32 转换及缺失输入错误。ONNX 外部模型用例未配置 `LYFLOW_TEST_ONNX_MODEL`，该用例按原测试约定跳过。
- 全量 core：214 个用例中 212 通过、2 失败；42700 条断言中 42676 通过、24 失败。失败全部在未改动的 ASCII PCD/PCL 对照测试；独立导出主线 `362a6d5` 并使用同一 C:/vcpkg 构建后，复现相同的 2 个用例 / 24 条断言失败。因此 `pnpm check:glue` 的全量门禁尚非全绿。
- `check-glue.ps1 -Steps manifest,schema` 通过，包含 8 个 glue / 2 个标定算子存在性检查、随包图和片段、叠画正负例；实际测试产出的叠画也通过 schema。
- 前端 strict typecheck、编辑器 161 个单测、app 与 host-react 生产构建通过；MCP typecheck / build 和 32 个测试通过（包含 CLI 与 test-server 集成）。
- Rust 客户端独立构建、headless CLI 构建与 manifest 自检通过。本机使用已安装的 Rust 1.96.1、Node 24.16.0，前端直接执行对应 package scripts，因锁定版 pnpm 12.4.2 本地启动器失败、Rust 1.97.1 下载未完成。没有改动版本锁定文件；锁定环境的结果以 PR CI 为准。
- Python 工具编译检查通过。未运行客户数据评估、桌面 e2e；没有把这些记录为通过。

## 示教胶路（2026-10-09，glue-plan §6 / G5）

GlueSight 一期（胶枪相机、逐拍照点单帧检测）的胶路改由示教给定。

### 本轮实现

- 新积木 `glue.taught_path`：示教折线（`points`，JSON 文本）→ 同形的 `glue.Path`，`lineSource = taught`；加载期校验指到 `points` / `zone`。
- `glue.bead_width`：见到示教胶路时逐站估「偏移轨迹」（T3），D16 的中心偏离、参考宽 / 峰值以轨迹为准；沿线没有胶（T4）或轨迹上暗段
  边缘太缓（T5）时全部判无胶。拟合胶路（`bead_path`）的期望中心恒为 0，原有 doctest 一条不改照样过。
- `glue.judge`：一站都没量到胶与胶路没找到同样处理，只报一条 `missing`，与 `maxBreak` 无关。
- 示例图 `packs/glue/graphs/taught.lyflow.json`；`check-glue.ps1` 的算子清单加上 `glue.taught_path`。

### 复现

```powershell
$env:LYFLOW_PACKS = 'glue'
pnpm core:build
& .\build\core\bin\lyflow-core-tests.exe '--source-file=*test_glue.cpp'
pwsh -NoProfile -File scripts/check-glue.ps1 -Steps manifest,schema
```

### 验证记录（2026-10-09）

- glue doctest 14 例 / 1824 条断言全过（新增 4 例：示教线上的宽度与偏移、没胶 / 压痕 / 平行暗线 / 偏出容差、断口旁一条更长的平行暗线、加载期校验与重采样）。
- 全量 core（`LYFLOW_PACKS=glue`）：218 例中 216 过；失败的 2 例 / 24 条断言仍是上面那两例 ASCII PCD 与 PCL 的逐字节对照，未改动。
- `check-glue.ps1 -Steps manifest,schema`：manifest 与 schema、9 个 glue 算子与 2 个标定算子都在、4 张随包图与片段合格。
- headless CLI（`cargo +stable build --bin lyflow --no-default-features`，本机 Rust 1.96.1；钉的 1.97.1 没装全）`manifest --check` 干净，manifest 里有 `glue.taught_path`。

### MX11 现场对照（仓库外，86 帧，两件、43 个拍照点）

每个拍照点的示教线取自好件（2032670000），两件都用它检；每点的 `widthMax` / `widthRange` / `form` 同可行性研究的逐点参数，0.112 mm/px 标定。

| 项目 | bead_path 逐点（P） | 示教线 tolerance 40 px |
|---|---|---|
| 结论与现场一致 | 79 | 78 |
| 断胶 4 帧判 NG / 断口位置对 | 4 / 2 | 4 / 1 |
| 误报 | 7 | 8 |
| 直线段胶宽误差中位（最小 / 最大值） | 0.07 / 0.19 mm | 0.07 / 0.19 mm |
| 积木耗时 p50 | 18 ms | 6 ms |

示教线在胶的位置件与件之间稳定的拍照点上与 P 一样准、快三倍，断口不会被零件暗边整条拽走。但这批数据的断胶都在两件之间
胶的位置差 35–107 px 的拍照点上（另有 4 个点差 20–35 px）：固定的示教线落不到另一件的胶上，放宽 tolerance 又会把零件折边的暗影
（与胶一样宽、一样暗，只是边缘缓）当成胶。按零件特征配准（`glue.locate`）更差 —— 胶粘在喷嘴后面，不随零件刚体移动。
这些点要可靠，需要逐帧对胶本身配准，并加边缘陡度 / 纹理判据，列入后续。

## 仍属后续范围

- 三目相机选择、机器人方向和真实 NG 的现场验收（G3）。本轮合成图验证不能替代它。
- 主线编辑器对 `lyflow.overlay2d` 的渲染和点选示教；本轮保留数据与离线叠画工具，不迁入旧编辑器。
- 示教胶路：两件之间胶的位置不稳定的拍照点（MX11 #2、7、19、45、47–50）要逐帧对胶本身配准，并加边缘陡度 / 纹理判据挡零件折边的暗影。
- ly-TuJiaoVision 更新依赖到合并后的提交，构建启用 glue 的 core，并移除 `IMAGE_INPUT_UNSUPPORTED`、恢复宿主 `RunImageInput`。其现有默认分支不在本 PR 中改动。
