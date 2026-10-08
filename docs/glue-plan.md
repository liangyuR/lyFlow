# 涂胶算子设计与验收范围

2026-10-08：从 `feat/glue`（`d65de45`）迁移 G2 / G4 到主线 `362a6d5`。图像域以主线 ABI v15 / ADR-0026 为准；以下保留领域算法的设计约定。

当前实现、复现命令及剩余范围见 [glue-integration.md](glue-integration.md)。原 G1 平台实现和 G2 历史验收保留在原分支，本次不迁入旧版图像平台或编辑器。

客户数据不进仓库；使用 `LYFLOW_GLUE_DATA` 指定仓库外的演示数据目录。G3 的真实三目选相机、现场标定及真实 NG 仍待现场资料。

## 2. G2 —— 涂胶积木（`packs/glue`）

### 2.1 约定

- **图像坐标系**：像素，x 向右、y 向下，像素中心在整数坐标。G2 的所有位置、长度默认都是 px。
- **胶路参数 s**：从喷嘴出胶点沿胶路的弧长（px）。**检测区** `zone = [start, end]` 是 s 的一段。
- **站（station）**：检测区内每隔 `stationStep` 取一个 s，站上的切向 `t` 指向远离喷嘴的方向，
  法向 `n = (−t.y, t.x)`，在图像里指向行进方向的右侧。卡尺沿 n 布。
- **单位**：没有接标定时一律 px（`Measurement.unit = "px"`）；接了 `calib`（G3）后宽度、距离、长度改出 mm。
  这是 G2 唯一要留的标定口子：量测积木都带一个可选输入 `calib: Record<image.PlaneCalib>`，
  格式在 G2 定下（写进 std-image 的 README）：`{H: [9 个数，行主序，图像 px → 工作平面 mm], unit: "mm"}`。
  G2 只实现「不接」与「按单应映射」两条路（宽度、距离、长度都按映射后的两点距离算），用合成的单应做 doctest；
  产出单应的算子在 G3。

### 2.2 定死的决定

| # | 决定 | 理由 |
|---|---|---|
| D1 | **路径与测量解耦，两遍做**：`glue.bead_path` 先在检测区里粗找胶点，稳健拟合一条光滑胶路（能跨过断口，覆盖整个检测区）；`glue.bead_width` 再沿这条胶路布站、逐站卡尺精测。**粗找防跟丢**（G2 实现时定下，2026-09-25 验收采纳）：找到三个以上胶点后，一步接一步横向不超过半个胶宽（此前中位，至少 8 px）；滑过一段没找到胶之后重新接上的点横向不限，但要像此前的胶（峰值 ≥ 中位一半、直胶的宽度 ≥ 中位一半、边缘陡度 ≥ `sharpMin`）；跟丢了退回扇形搜索选中的射线；胶点离喷嘴 ≥ 0.75·s、方位不出搜索范围 20°；`maxGap`（默认 120 px）之后不再接新胶点、按已有胶点外推；s 是平滑后胶路（σ = 12 px）的真实弧长 | 教训 2。断胶 = 胶路上连续若干站无胶；胶路本身不因为没胶而中断。没有这几条时，断口后面常接上阴影或纹理，胶路被拽出一个弯（人造断胶里螺旋胶 1 例误报、2 例误差 38 / 36.6 px） |
| D2 | **检测区离开喷嘴**：`zone` 默认 `[100, 400]` px，粗找从 `zone.start` 开始，不从喷嘴开始 | 教训 1 |
| D3 | 胶的响应图用**黑顶帽**（闭运算 − 原图，结构元直径 > 期望最大胶宽），不用固定灰度阈值；`polarity: bright` 时用白顶帽。结构元用**正八边形**（不是椭圆），只算检测区外扩的那一块 | 背景从全黑到过曝都有，胶只在「比局部背景暗」这一点上稳定。91 px 的椭圆核整幅闭运算 118 ms，八边形 4 ms，结果在真实帧上不变 |
| D4 | **胶边取响应的半高处**（每站按本站峰值自适应）；暗段之间的缝小于 `mergeGap` 且合并后宽度仍在 `widthRange` 内就合并 | 教训 3：高光条不能把一条胶劈成两条 |
| D5 | **螺旋胶取外包络**：`form: swirl` 时，每站的胶两边取 `[s − window/2, s + window/2]` 内所有暗段的并集；有无胶也按窗口判 | 用户认可的定义；否则螺圈之间的空隙会被当成断胶 |
| D6 | **方向**：`headingSource = param` 时（G3 由机器人经顶层图参数喂 `heading`）只在 `heading ± headingTol` 里找；否则在 `sector` 扇区里扇形搜索，取前三个相距 ≥15° 的峰各试一遍，留得分最高的；射线本身的响应不到最强那条四分之一的候选得分打对折 | 原型里单峰搜索会被喷嘴处的卷丝带偏；靠卡尺从旁边「蹭」上胶的候选，起点其实落在胶外 |
| D7 | **逐站分类 + 连续段出缺陷**：每站得出 有胶 / 无胶、宽度、边距；断胶、窄胶、宽胶、边距超差都是「连续 ≥ 最短长度的同类站」 | 与商用胶路检测一致；一根卡尺偶发失手不报缺陷 |
| D8 | **边距默认从胶的近边量**（`reference: bead_edge`），可切到 `bead_center`；零件边在哪一侧**自动判**（两侧都找，取多数站找到强边的一侧），不设必须人填的 side | 用户认可的定义；M8 的规矩：人能填反的参数一个都不留 |
| D9 | 零件边的判据：沿法向第一个满足极性（默认 亮→暗）、对比度 ≥ `contrastMin`、且远侧持续 ≥ `farRun` px 的跳变；逐站结果再沿 s 做中值 + MAD 剔野 | 原型里只取最大梯度，常被翻边上的划痕、反光抢走 |
| D10 | 端口上的逐站数据用 Record（`glue.Stations`，数组形式），统计量用 `Measurement`；可视化另走 G1 的叠画通道 | 统计量要能进 `lyflow eval` 的值路径；逐站数组给宿主与叠画 |
| D11 | 算法全在 C++（OpenCV 只在包里）；**不写第二份算法**：积木之间共用包内函数，doctest 与 CLI 走同一份 | 与 gap 包 L6 一致 |
| D12 | 真实帧不进仓库。doctest 用**合成图**（已知宽度、断口、边距）；真实帧的评估走 `lyflow eval`，数据路径由 `LYFLOW_GLUE_DATA` 给 | 客户数据；合成图才有真值 |
| D13 | `packs/glue` 是 `DEFAULT OFF` 的领域包，`LINK lyflow_opencv_support`，configure 时没有这个目标就 FATAL；加 `scripts/check-glue.ps1` 与 `pnpm check:glue`（照 `check-gap.ps1`），e2e 的 glue 组在 `LYFLOW_PACKS` 含 glue 却找不到 glue 算子时**判失败**，不是静默跳过 | 领域包惯例（ADR-0015）；记忆里踩过的坑：缺包时分组自跳过还报全绿 |
| D14 | 真实帧的批量评估、人造断胶数据集、叠画缩略图，由 `packs/glue/tools/` 下的脚本生成，脚本只调 `lyflow` CLI（`eval`、`run --input`、`dump`），不自己实现检测 | 与 gap 的 tools 同地位；评估用的是产品本身的代码 |
| D15 | **胶路要像胶**：候选胶路上各暗段的边缘陡度 = min(左右两边各 ±3 px 的灰度差) / 胶的暗度（两侧 8 px 外的均值 − 暗段中间一半的均值），整条候选取第 25 百分位，≥ `sharpMin`（默认 0.4）才算找到胶；D6 的三个候选方向按「覆盖率 ×（像胶 ? 1 : 0.1）」选。**逐站不卡陡度** | 零件上的压痕、阴影也是暗带，但边缘是缓的。参考实现里没有这一条时，Glue2 #1–5 的无胶帧全被当成有胶（覆盖率 1.0）—— 胶枪断料时就是「没胶却放行」。实测：直胶 P25 0.84–1.20，螺旋胶（逐圈算）0.64–0.80，无胶帧的压痕 ≤ −0.01。逐站卡陡度反而多出 3 帧误报 |
| D16 | **逐站判有无胶的一致性**：参考宽 `w_ref` = 全检测区候选暗段宽度的中位数（不用局部窗口：断口里的局部窗口会被零件表面的拉丝纹主导）。直胶：宽 ≥ `presentRatio`（0.5）× `w_ref` 且中心偏离胶路 ≤ max(6, `centerRatio`（0.3）× `w_ref`)，否则判无胶；**对比度一致**：一站的峰值不到全区候选峰值中位数的 `contrastRatio`（0.3）倍也判无胶（断口两头模糊出来的残影宽度、位置都对，只是淡）；螺旋胶：本站**自己**要有一个宽 ≥ 0.5 × `w_ref`（绳宽中位）、落在包络半宽内的暗段才算有胶，窗口（±`window`/2）**只用来算外包络宽度**，不用来判有无 | 人造断胶里，断口内的站把偏离胶路约 12 px、宽 8–20 px 的拉丝纹（胶本身约 37 px 宽）当成胶；螺旋胶按窗口判有无会把比窗口短的断口整个抹掉。加上这两条后参考实现从 35–50% 检出升到 94–100% |

### 2.3 积木一览

`Bundle` 两种（manifest 里声明，M8a L2）：

- **`glue.Path`** = `line`（Record `glue.Polyline`：`points`（px）、`s`、`tangents`，间隔 ≤ 2 px）+
  `info`（Record：`heading`、`headingSource: param|search`、`coverage`、`residual`、`zone`、`nozzle`、`ok`、`message`）
- **`glue.Bead`** = `line`（同上，Bundle 不嵌套，所以把折线本身带过来）+ `stations`（Record `glue.Stations`：
  每站 `s`、中心、左右边、宽度、有无胶、对比度，数组形式）+ `info`（`form`、`stationStep`、`unit`）

| 积木 | 输入 → 输出 | 人要填的参数（其余在「高级」组折叠） |
|---|---|---|
| `io.load_image` 读图（G1） | （路径，或宿主注入）→ `image: Image` | 路径 |
| `glue.bead_path` 定胶路 | `image` → `path: Bundle<glue.Path>` | `nozzle`（在图上点选）；`zone`；`sector`；高级：`heading`、`headingTol`、`widthMax`、`polarity`、`maxGap`、`minCoverage` |
| `glue.bead_width` 量胶宽 | `image`、`path`、`calib?` → `bead: Bundle<glue.Bead>`、`widthMean`、`widthMin`、`widthMax`、`coverage`（Measurement） | `form: straight\|swirl`；`widthRange`；高级：`stationStep`（默认 4）、`searchHalf`、`mergeGap`、`window`、`contrastMin` |
| `glue.bead_breaks` 查断胶 | `bead` → `breaks: Record`（每段 `sStart`、`sEnd`、`length`、两端点）、`count`、`longest`（Measurement） | `minLength`（默认 20 px）；高级：`countAtZoneEnds`（默认 true） |
| `glue.edge_distance` 量边距 | `image`、`bead`、`calib?` → `edge: Record`（每站零件边点与距离）、`distanceMean`、`distanceMin`、`distanceMax`（Measurement） | `searchLength`（默认 200）；高级：`reference`、`edgePolarity`、`contrastMin`、`farRun` |
| `glue.judge` 判定 | `bead`、`breaks`、`edge?` → `verdict: Record`（`ok`、缺陷列表：类型 / 起止 s / 长度 / 值）、`ok: Measurement` | 胶宽上下限、边距上下限（0 = 不判那一侧）；允许的断口长度 `maxBreak`（**0 = 一处断口都不许，负数 = 不判断胶**）；缺陷最短长度（0 = 一站就算） |

每个积木都多一个输出 `overlay: Record<lyflow.overlay2d>`（G1 I8），画的是这一步的结论：`bead_path` 画喷嘴点、扇区、
检测区两端的弧、胶路与粗找到的胶点；`bead_width` 画每站两边（有胶 / 无胶两种 role）；`bead_breaks` 画断口；
`edge_distance` 画零件边点与每站的距离线段；`judge` 把胶路、两边与全部缺陷画在一起。

一张检测图：`io.load_image → glue.bead_path → glue.bead_width → glue.bead_breaks → glue.edge_distance → glue.judge`，6 个节点。
包随附片段 `glue.bead_inspect`（「涂胶检测」）与两张示例图 `packs/glue/graphs/glue1.lyflow.json`（直胶）、
`glue2.lyflow.json`（螺旋胶），参数按演示数据调好。

**失败语义**（ADR-0016 错误即值）：`bead_path` 覆盖率低于 `minCoverage` 时 `info.ok=false`、`message` 写「检测区内没找到胶」，
下游照常跑出「全段无胶」—— 没有胶本身就是一个 NG 结果，不是执行错误。

### 2.4 不做

整条胶的拼接（按机器人里程把逐帧结果拼成一条）；深度学习分割（留 `ml.onnx_run` 的口子，有 ONNX 模型或标注数据再说）；
胶高 / 3D；起胶收胶段的自动屏蔽（现场由机器人程序告诉哪几帧不检）。

**已知限制**：收胶段胶已离开喷嘴时（Glue2 #69–70），旁边零件的锐利接缝可能被当成胶 —— 它的边缘和胶一样陡，D15 挡不住。
G3 的 `heading` 把方向钉在 ±`headingTol` 以内就不会再选到它；现场这几帧本来也由机器人程序屏蔽。

---

## 3. G3 —— 现场接入（只定接口）

| # | 决定 |
|---|---|
| F1 | **三路相机**：三路各走一遍 `bead_path → bead_width`（每路自己的 `nozzle`、`sector`），末端 `glue.pick_camera`（`bead1..3` → `bead`、`info`）：给了机器人方向就选扇区包含胶方向的那一路，没给就选覆盖率最高的 |
| F2 | **机器人方向**：顶层图参数 `heading`（该相机图像坐标系里的胶方向，度）由宿主逐帧给；每路相机一个固定偏角 `cameraYaw`，由现场数据拟合（胶方向实测值 vs 机器人方向） |
| F3 | **标定**：Record `image.PlaneCalib`（3×3 单应，工作平面 mm）；`image.scale_calib`（已知 mm/px）与 `image.board_calib`（标定板图 → 单应）两个来源；量测积木的 `calib` 输入接上即出 mm |
| F4 | **真实 NG 样本**进验收集：断胶 / 窄胶 / 宽胶 / 偏移各至少 5 帧，带人工标注（帧号、类型、大致位置）；另收一批「覆盖率够、边缘缓」的无胶帧（压痕、阴影、反光带）—— 演示数据里的无胶帧最终都是覆盖率先拦下的，D15 眼下只有 doctest 在钉 |

---

## 4. 原分支的 验收

每个实施包由子代理写进 `docs/glue-<包>-acceptance.md`，逐条标 通过 / 未通过 / 未验证，附命令与输出摘录；
以下为原分支的完整验收清单，其中 G1/G1b 平台与编辑器按主线实现替代；本轮实际覆盖、偏离及余项见 [集成记录](glue-integration.md)，不把下列清单视为本轮已全部通过。

**G1a**

1. `pnpm check` 全绿（默认包集，含 std-image 的 doctest）；`LYFLOW_STD_PACKS=0` 的纯平台构建 C++ 一层全绿
   （Image 在 core 里，但不带 OpenCV），`dumpbin /dependents lyflow_core.dll` 没有 opencv。纯平台下 bridge 的 70 条与
   MCP 的 2 条测试失败是**基线就有的**（测试图假定标准点云包在），另开任务处理，不算 G1a 的账（2026-09-25 验收时核对过基线）。
2. core doctest：Image 的 `valueJson` / `elementCount` / `byteSize`；`lyflow_output_image` 取整幅、取中间几行、
   越界得空切片、非图像端口或找不到返回 1、`out` 为空返回 2（与 tensor 相同，I3）；两次取的 `data` 指针相同（零拷贝）；`shape` 契约对 Image 生效。
3. 注入：同一张图注入两次第二次 `skipped`；换一张图（只改一个像素）不命中缓存；未知 kind 报 `bad_input`。
4. std-image doctest：合成图 → `io.save_image` 写 png → `io.load_image` 读回，逐像素相等；jpg 能读；
   中文路径能读写；`mode: gray` 对三通道图出单通道；`image.to_gray`；BGR/RGB 没反（纯红像素读回是 `[255,0,0]`）。
5. CLI：`lyflow run g --input n_load.image=<png>` 跑通；`lyflow dump g n_gray:image out.png` 的像素与源图逐像素相等。
6. `lyflow.overlay2d`：构造器产出的 Record 过 `schema/overlay2d.schema.json`；`point` 标记用在非 vec2f 上加载期报错。

**G1b**

7. e2e（tauri dev）：建图 `io.load_image → image.to_gray`，运行后选中节点，主视图切到图像视图
   （`data-testid="image-view"`，`data-w` / `data-h` 与图一致）；悬停已知像素读数正确；滚轮缩放改变 `data-zoom`，
   拖动平移改变 `data-pan`；Edge Peek 打开 Image 边默认是图像视图。
8. e2e 叠画：用 `gap.mjs` 的 `feedOutputs` 手法注入一条带 `lyflow.overlay2d` 的假事件，叠画项数与包围盒对得上。
9. e2e `point`：`LYFLOW_TEST_OPS` 下一个带 `point` 参数的测试算子，拖柄写回参数（误差 ≤ 0.5 px）、只占一个撤销步；
    「在图上点选」后单击一次即设值。
10. `pnpm e2e:http`：经桩服务器取到图像，尺寸对、抽查像素对；MCP `summarize_output` 对 Image 返回 I15 的字段（MCP 自己的测试）。
11. `pnpm e2e:packaged`：干净目录里有 `opencv_core4.dll`、`opencv_imgproc4.dll`、`opencv_imgcodecs4.dll`，
    打包后的 app 能读 jpg。
12. 文档：新 ADR（图像数据域与叠画契约）、`docs/op-packs.md` 包表、`docs/http-transport.md`（端点与 LYIM 帧）、
    `docs/mcp.md`、`bridge/README.md`、README 快速开始里的 OpenCV 安装命令。

**G2**

13. **合成图 doctest**：直胶与螺旋胶，宽度 `{8, 20, 40, 70}` px，叠渐变背景、高斯噪声、模糊、零件边：
   胶宽中位误差 ≤ 1.0 px，边距中位误差 ≤ 1.0 px；断口长度 `{10, 20, 40, 80}` px 放在已知 s：
   `minLength=20` 时恰好报出 ≥ 20 的那几段，起止误差 ≤ `stationStep`，其余位置零误报。
14. **真实帧，满胶段零误报**：`lyflow eval` 跑 Glue1 #15–80 与 Glue2 #13–66（示例图的参数），Glue1 **0 帧**报断胶，
   Glue2 **≤ 1 帧**（列出帧号与位置）；两组都 100% 找到胶路（`info.ok`）。（Glue1 #14 是起胶帧：胶头还在检测区里，不算满胶段。）
15. **真实帧，无胶判得出**：Glue1 #1–10、Glue2 #1–10 都判 NG，理由是「检测区内没找到胶」（D15 的边缘陡度拦住零件上的压痕）。
16. **人造断胶**：随包工具从满胶帧生成断口 —— 从胶路两侧挑源带更干净的一侧平移过来盖住（源带里黑顶帽响应 > `contrastMin`
   的像素超过 2% 就换一侧，两侧都不干净就跳过这个样本），羽化约 3 px；不用 inpaint，它会把胶色补回去。
   长度 `{15, 30, 60}` px、位置随机落在检测区内，直胶与螺旋胶各 ≥ 20 例：
   直胶 ≥ 30 px 检出 **100%**、起止误差 ≤ 5 px；螺旋胶 ≥ 30 px 检出 ≥ 90%、≥ 60 px 检出 100%、起止误差 ≤ 32 px
   （螺圈回卷，约一个螺距）；15 px 短于 `minLength` 不报；断口之外零误报。
17. **目检**：验收文档附 Glue1、Glue2 全部帧的叠画缩略图（胶路、两边、零件边点、缺陷），我逐张过。
18. **性能**：Release 下 `bead_path → judge` 五个积木单帧 ≤ 50 ms（本机）。
19. **e2e**：空白画布插入「涂胶检测」片段，在图像视图里点一下设 `nozzle`，运行，叠画出现胶路与各站；截图进验收文档。
20. `LYFLOW_PACKS="gap;dts;glue"` 下 `pnpm check`、`pnpm e2e` 全绿，日志落盘 grep 无「跳过 / 未验」。

---

## 5. G4 —— 固定相机飞拍后检（ly-TuJiaoVision 接入）

**场景**：打胶之后，机器人持工件在**固定**相机下连续运动，机器人控制器按路径位置硬触发相机，一件分 N 帧（拍照点 k）。
判的是**胶条到实物内边**的距离（名义 0.75 mm，公差 ±0.75 mm），不是胶枪跟随。宿主（ly-TuJiaoVision）持有名义胶路、
拍照点划分与每个测量点归哪一帧，逐帧把「这一帧负责的测量点」交给图；图只做**定位修正 + 逐点卡尺**，
判定、断胶按弧长跨帧合并都在宿主（与 gap 一致：图只量不判）。宿主侧的设计见 ly-TuJiaoVision「飞拍模式设计」§9。

### 5.1 定死的决定

| # | 决定 | 理由 |
|---|---|---|
| K1 | **名义胶路逐点卡尺**，不从图像里找胶路：测量点由宿主给（示教图坐标下的位置 + 法向），`glue.station_calipers` 逐点量 | 一件分多帧，断胶要按弧长跨帧合并，站必须是宿主结果表里的格子；D1 的找胶路在飞拍里既不需要也不稳（胶条贴着内边，内边本身是强边） |
| K2 | **定位 = 模板归一化互相关 + 小角度扫描**（`glue.locate`），输出刚体位姿 Record `glue.Pose2D`（示教图 → 当前图：`T(p) = R(angle)·(p − teachCenter) + center`）；分数低于 `minScore` 时 `ok = false`（错误即值），下游全部站判 `pose_fail`，宿主据此报定位失败 | 机器人重复定位误差只有几毫米、零点几度，不需要形状匹配；角度按抛物线插值到步长以下 |
| K3 | **站的约定**：点在名义胶条中线上，法向从内边（开口一侧）指向翻边，卡尺上 `t < 0` 是内边一侧。内边在 `innerSearch` 区间里按极性找跳变（±3 px 两侧均值差 ≥ `contrastMin`），有几条时按 `innerSelect` 取（`nearest` = 离胶最近，对应现场「内边取上沿」的约定；`strongest` = 最强）；胶在内边之后、`beadSearch` 之内取**离站点最近的暗谷**（窗口里可能还有安装孔、阴影这类更暗的东西），按本站的半高定两条边（与 D4 同一思路） | 铸件内壁有拔模斜度时俯视能看到两条边，取哪条由宿主按 CMM 对齐后定，不写死；半高边对模糊对称，不偏 |
| K4 | **输出三条边沿法向的偏移**（px）与每站状态 `ok / no_inner / no_bead / pose_fail`；接了 `calib` 再出 内边→近边、内边→胶中线、胶宽 三个距离（按映射后两点的距离，§2.1 的尺子）。中心线 / 近边两种测量模式由宿主判定时选，图里不分支 | 模式切换不重跑视觉；宿主的离线重判要能换模式 |
| K5 | **测量点经文件传**：参数 `stations`（Path，JSON：`points[[x,y]]`、`normals[[nx,ny]]`、可选 `ids[]`），宿主发布配方时每个拍照点写一份，逐帧经顶层图参数换路径；`externalKey` 取文件大小与 mtime | 几百个点不适合塞进参数；文件静态、改了就重读 |
| K6 | **标定**：std-image 加 `image.board_calib`（棋盘格 → `image.PlaneCalib`，`findChessboardCornersSB` + 全点单应，报残差与 mm/px）与 `image.load_calib`（读 PlaneCalib 的 JSON 文件）。标定属于**相机工位**（用户 2026-09-27 定），宿主存一份文件，产线图里用 `load_calib` 读；棋盘放在内边所在高度的平面上 | 换型不重标；图里拿到的是和 G2 同一种 Record，量测积木的 `calib` 口原样接 |
| K7 | 示例图 `packs/glue/graphs/flyshot.lyflow.json`：`io.load_image`（source=inputs）→ `glue.locate` → `glue.station_calipers` ← `image.load_calib`；顶层参数 `template`、`anchor`、`stations`、`calib` 由宿主逐帧给 | 宿主只换参数、注入图像，不改图 |
| K8 | doctest 用**合成图**：暗开口 + 亮翻边（渐变）+ 暗胶条，内边距离已知；平移、旋转后的定位误差、距离误差、断口、内边缺失、位姿失败都有真值；标定用已知单应画出来的棋盘 | D12：没有客户真实帧，合成图才有真值 |

### 5.2 积木

| 积木 | 输入 → 输出 | 参数 |
|---|---|---|
| `glue.locate` 定位 | `image` → `pose: Record<glue.Pose2D>`、`score`（Measurement）、`overlay` | `template`（Path，示教时从示教图上裁的一块）、`anchor`（point：模板左上角在示教图上的位置）、`searchRadius`（80 px）、`angleRange`（3°）、`angleStep`（0.5°）、`minScore`（0.6） |
| `glue.station_calipers` 逐点卡尺 | `image`、`pose?`、`calib?` → `measure: Record<glue.StationMeasure>`、`coverage`、`distanceMean`（Measurement）、`overlay` | `stations`（Path）、`searchHalf`（48 px）、`caliperWidth`（5 px）、`innerSearch`（[−40, −3] px）、`beadSearch`（[−15, 40] px）、`innerPolarity`、`innerSelect`、`beadPolarity`、`contrastMin`（25）、`widthMin`（3 px） |
| `image.board_calib` 标定板标定（std-image） | `image` → `calib: Record<image.PlaneCalib>`、`rms`（Measurement）、`overlay` | `pattern`（内角点数，[11, 8]）、`square`（格长 mm，5） |
| `image.load_calib` 读标定（std-image） | —— → `calib: Record<image.PlaneCalib>` | `path`（Path，JSON） |

### 5.3 验收

21. 合成图：平移 ±40 px、旋转 ±2° 时 `glue.locate` 位姿误差 ≤ 0.3 px / 0.1°（角度误差只让站沿边滑动，距离在同一根卡尺里量，不受它影响；0.1° 在 110 px 半径上约 0.2 px），分数 ≥ 0.9；模板在图上根本没有时 `ok = false`，下游全部 `pose_fail`。
22. 卡尺：内边→近边、内边→胶中线中位误差 ≤ 0.3 px；接合成单应出 mm，误差 ≤ 0.02 mm；断口处 `no_bead`、内边被抹掉处 `no_inner`，其余零误报。
23. `image.board_calib`：已知单应画出的棋盘，恢复出的单应量任意两点距离误差 ≤ 0.05 mm；图里没有棋盘是执行错误 `no_board`；`image.load_calib` 读回的与写入的逐位相同（中文路径可读）。
24. `LYFLOW_PACKS=glue` 下 `pnpm core:build` 的自检与 doctest 全绿。
