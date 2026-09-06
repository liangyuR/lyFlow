# 接入 xyz-gap-inspector 验收记录

逐条对着 [gap-integration-plan.md](gap-integration-plan.md) 走一遍：**怎么跑 + 实际输出 + 通过/未通过/未验证**。
第一部分（配置/模板 + ICP 路径）对 §5，第二部分（模型 ROI 路径）对 §10。
与计划不同的地方全部记在最后的「偏离与决策」里，编号连续。

两个仓库：`D:\project\LyFlow`（main）与 `D:\project\xyz-gap-inspector`（分支 `lyflow-ops`）。
数据在 `C:\Users\11601\OneDrive\Documents\DTS\tianmu_0904`，
基线在 `%TEMP%\lyflow-gap-baseline`（模板路径）与 `%TEMP%\lyflow-gap-baseline-model`（模型路径），
模型在 `C:\Users\11601\OneDrive\Documents\DTS\models\v12s0.onnx`。

六个 commit（都没 push）：

| 仓库 | 分支 | commit |
|---|---|---|
| LyFlow | `main` | `feat: 算子包机制、2D 几何类型与剖面叠画 —— 为接入 gap-inspector` |
| xyz-gap-inspector | `lyflow-ops` | `1aca0b9  LyFlow 算子包：配置/模板路径的拆分算子、图生成器与 A/B 脚本` |
| LyFlow | `main` | `feat: gap 模型 ROI 路径的验收与文档` |
| xyz-gap-inspector | `lyflow-ops` | `7865848  LyFlow 算子包：ONNX 模型 ROI 路径` |
| xyz-gap-inspector | `lyflow-ops` | `a3cce54  LyFlow 算子包：模型四框与着色剖面共用测量帧底图` |
| LyFlow | `main` | `test: 模型图底图同帧断言` |

## 复现命令

```powershell
# 1. 不带算子包：通用侧行为不变的证据
cd D:\project\LyFlow
$env:LYFLOW_OP_PACKS = ""
pnpm check

# 2. 带算子包
$env:LYFLOW_OP_PACKS = "D:\project\xyz-gap-inspector\lyflow"
pnpm check

# 3. 39 个样本的 A/B（会顺手把 39 张图生成到 %TEMP%\lyflow-gap-ab）
cd D:\project\LyFlow\bridge
cargo build --bin lyflow --no-default-features       # A/B 脚本要用这个 exe
cd D:\project\LyFlow
python D:\project\xyz-gap-inspector\lyflow\tools\lyflow_ab.py `
    --dataset "C:\Users\11601\OneDrive\Documents\DTS\tianmu_0904\dataset.yml" `
    --baseline "$env:TEMP\lyflow-gap-baseline" `
    --json "$env:TEMP\lyflow-gap-ab\ab.json"

# 4. CDP 验收（第二条环境变量是可选的真实 gap 图那一组）
$env:LYFLOW_GAP_GRAPH = "$env:TEMP\lyflow-gap-ab\KUN10_HXMK2A12XTA237802_R5_11.lyflow.json"
pnpm e2e
```

R1 / R5 两张图（A/B 脚本生成，也可以单独生成）：

```
%TEMP%\lyflow-gap-ab\KUN10_HXMK2A12XTA237802_R1_10.lyflow.json
%TEMP%\lyflow-gap-ab\KUN10_HXMK2A12XTA253787_R1_39.lyflow.json   （另一个 gap 失败样本）
%TEMP%\lyflow-gap-ab\KUN10_HXMK2A12XTA237802_R5_11.lyflow.json
```

单独生成一张：

```powershell
python D:\project\xyz-gap-inspector\lyflow\tools\lyflow_graph_from_config.py `
    "<...>\database\KUN10\device_0\R5\StandardGap.yml" `
    --primary "<...>\LaserProfile_L0_Master_R5_*.pcd" `
    --secondary "<...>\LaserProfile_R1_Slave_R5_*.pcd" `
    -o R5.lyflow.json
```

---

## §5 逐条

### ✅ 1. `pnpm check` 全绿（不带包行为不变；带包 manifest 自检干净、schema 校验过）

**带包**（`LYFLOW_OP_PACKS=D:\project\xyz-gap-inspector\lyflow`）：

```
=== C++ core ===         ok: 32 operator(s), 11 port type(s)
                         [doctest] test cases: 94 | 94 passed | 0 failed
=== manifest vs schema ===   符合 schema/operator-manifest.schema.json
=== execution-event vs schema === ok: 16 条
=== graph-doc vs schema ===  ok: 4 node(s), 3 edge(s)
=== Rust bridge ===      test result: ok. 53 passed; 0 failed
=== headless CLI ===     算子描述自检干净
=== frontend ===         built in 2.54s
全链路绿
```

**不带包**：`ok: 16 operator(s), 11 port type(s)`，其余各段同样通过 ——
算子数从 32 回到 16、类型表仍是 11 个（六种 2D 载荷是 core 的，不随包来去）。

带包时新增的 16 个算子全部通过 `Registry::validate()` 与 schema 校验，
这一条正是「包作者的笔误挡在启动自检里」的兑现（ADR-0013）。

> **一个已知的既有不稳定**（**已修**，见「偏离与决策」第 8 条末尾）：`cargo test` 有大约 1/6 的概率在
> `execution::tests::cloud_payload_carries_normals_when_the_op_produces_them`
> 上报 `NoSuchOutput`。原因是 `commands`/`cli` 里的测试会调 `lyflow_cache_clear()`，
> 而结果仓是进程级的，它会把并行跑着的另一个测试的结果一起清掉。
> 与本次改动无关（那条路径一个字都没动），详见「偏离与决策」第 8 条。

### ✅ 2. core doctest：新类型 JSON 往返；`gap.crop_box` 开区间；`gap.fit_line` 截取方向四种组合

| 断言 | 在哪 |
|---|---|
| 六种载荷的类型名往返、Box2D/Line2D/Circle2D/Point2D/Measurement/Record 的 `valueJson` | `core/tests/test_data.cpp`，8 个 TEST_CASE |
| Measurement 的非有限值写成 `null` 而不是把整份 JSON 弄坏 | 同上 |
| 点云与 Indices 不走 `valueJson` | 同上 |
| `gap.crop_box` 四边严格开区间（四条边上的点、NaN 点各一条） | `xyz-gap-inspector/lyflow/tests/test_gap_ops.cpp` |
| `gap.crop_box` 裁空报 `roi_empty` | 同上 |
| `gap.fit_line` 截取方向 **ascend × side 四种组合** | 同上（一个 TEST_CASE 里循环四次，断言留下的十个内点是头还是尾） |
| `gap.fit_line` 内点少于 segmentPoints 时不截取 | 同上 |
| `gap.flush` 取绝对值、符号不参与 | 同上 |
| `gap.gap` definition A 的几何 + 端点顺序不影响 + 切线交叉报错 | 同上 |
| `gap.judge` 五种判定 | 同上 |
| `gap.business_rois` 的 base_side 互换 | 同上 |
| `gap.overall_roi` 的 auto_center | 同上 |

**实际输出**（带包）：`[doctest] test cases: 94 | 94 passed | 0 failed`，
其中 core 自己 77 个、新增类型 8 个、包里 9 个。

### ✅ 3. A/B 脚本：39 个样本全部一致

**怎么跑**：见上面的复现命令第 3 条。

**实际输出**：

```
一致 39 / 39；最大 |Δ| = 0.000007 mm
```

- 成功/失败状态：39/39 一致（28 成功、11 失败）
- 成功样本的 gap、flush：最大 |Δ| = 6.7e-6 mm，远小于 0.002 mm
- `gap.measure_reference` 与拆分算子之差：同样在 1e-5 mm 量级（脚本对这一项单独断言）

失败样本的**失败原因**也逐个对上了（脚本只断言「有没有值」，这一栏是人工核对的）：

| sample | 基线 stage/code | LyFlow 的红框节点 |
|---|---|---|
| …237775_R2_1 | icp / icp_score_low | `n_select`：所有候选低于 80（最好 0.00） |
| …237796_L2_7 | roi / roi_empty | `n_crop_flushBase` 等四个 crop 全空 |
| …237802_R1_10 | fitting / circle_fit_failed | `n_circles`：gap_left 圆拟合失败 |
| …237805_L3_12 | icp / icp_score_low | `n_select`（最好 57.28） |
| …253649_L3_20 | icp / icp_score_low | `n_select`（最好 74.89） |
| …253661_L6_21 | roi / roi_empty | 四个 crop 全空 |
| …253664_L6_23 | roi / roi_empty | 四个 crop 全空 |
| …253680_L3_28 | icp / icp_score_low | `n_select`（最好 73.33） |
| …253709_R2_32 | icp / icp_score_low | `n_select`（最好 23.87） |
| …253733_L2_34 | icp / icp_score_low | `n_select`（最好 0.00） |
| …253787_R1_39 | fitting / circle_fit_failed | `n_circles`：gap_left 圆拟合失败 |

完整表格见本文末尾。

### ✅ 4. R1、R5 两个测点的图在桌面端打开、运行，3D 视图能看到有效 ROI 框、基准线、圆；R1 的 gap 节点红框指向 `gap_left` 圆拟合失败

**怎么跑**：`pnpm e2e` 带上 `LYFLOW_OP_PACKS` 与 `LYFLOW_GAP_GRAPH`（复现命令第 4 条），
`scripts/e2e/gap.mjs` 的「真实 gap 图」那一组会打开图、F5、选中 `gap.business_rois` 节点，
断言 3D 视图的 `data-overlay` 是 4（四个业务 ROI 框都叠上了）。

R1 的红框：A/B 脚本的事件流里，`n_circles` 报
`circle_fit_failed / gap_left 圆拟合失败 / portName=boxLeft`，
与基线的 `fitting / circle_fit_failed / gap` 对应。
前端把 `portName` 标到那个输入端口上，`errors[]` 完整显示在 Inspector 里（M2 起就有的机制）。

### ✅ 5. CDP：叠画几何、2D 剖面模式、Inspector 数值显示各至少一条断言

`scripts/e2e/gap.mjs` 的「量测输出」那一组（**不需要算子包**，所以它在默认门禁里）：

| 断言 | 内容 |
|---|---|
| 叠画几何 | 灌一条带 Box2D/Line2D/Circle2D/Point2D 的 `node_state` 事件，断言 `.viewer[data-overlay] === 4`；再断言叠画之后画布仍在渲染 |
| 2D 剖面模式 | 把 `[data-testid="viewer-camera"]` 切到 `2d`，断言 `.viewer[data-camera] === "2d"`，且画布没有被重建掉 |
| Inspector 数值 | 断言 `[data-testid="inspector-outputs"]` 有五行；`output-value` 显示 `6.4138 mm`；`data-verdict="ok"`；Box2D 显示成两个角点 |

值是从**执行事件**灌进去的而不是靠某个具体算子 —— 这一组验的是前端那一段，
而前端不该知道任何算子的名字（ADR-0003）。C++ 那一段由 core doctest 与 A/B 脚本覆盖。

### ❌ 6. 热重载：改包里一个算子 label，app 5 s 内换代 —— **实测 5.1 s，差 0.1 s**

机制成立，时间差一点点。

**怎么跑**：一个终端 `$env:LYFLOW_OP_PACKS=…; pnpm dev`（它会同时起 core-watch 与 app），
另一个终端把 `lyflow/ops/measure.cpp` 里 `gap.judge` 的 `op.label` 从 `"Judge"`
改成 `"Judge (hot)"`，看节点面板里那个算子的名字什么时候变。
我这边是用一个临时脚本量的：起 core-watch → 起 app → 改文件 → 每 150 ms 读一次
`manifest store` 里 `gap.judge` 的 label，直到它变。

**实际输出**：

```
label Judge -> Judge (hot)，耗时 5.1 s，generation=1
```

`generation` 从 0 变成 1 说明真的换代了（ADR-0009 那条路径），
manifest 里的算子名也跟着换了。5.1 s 的构成大致是：

| 段 | 大约 |
|---|---|
| core-watch 轮询发现改动 | 0–0.5 s（带包时轮询收紧到 0.5 s） |
| core-watch 的 400 ms 静默去抖 | 0.4 s |
| 增量编译 + 重链 5 MB 的 DLL | ≈3.5 s |
| app 侧 watcher 的 400 ms 静默 + 复制 + 加载 + 自检 + 推 manifest | ≈0.8 s |

**为了这一条加了包级 PCH**：一开始是 **10.5 s**（只到 DLL 就绪那一步就 10.5 s）。
包里每个 TU 都要拖一遍 PCL + Eigen + 领域头，
所以 `lyflow_op_pack()` 加了 `PCH` 关键字，gap 包给了 `ops/gap_pch.h`，
增量编译从 ~9 s 降到 ~3 s。

剩下的 0.1 s 没有再追：继续压只能动去抖窗口（400 ms × 2），
而那两个窗口各自有理由（链接器边写边改，缩短会加载到半截 DLL）。
**如实标未通过。**

---

## §7 后续改动：几何节点叠画时用上游最近的点云做底图

**问题**：接入之后选中 `gap.business_rois` / `gap.fit_line` / `gap.selected_point`
这类只输出 Box2D/Line2D/Circle2D/Point2D 的节点，视图里只有几条细线，
中间还写着「该节点无点云输出」；2D 剖面的取景又只按几何的跨度框，
**用户看不出 ROI 框压在剖面的哪个位置** —— 而这恰恰是要看的东西。
钉住（§2.6）解决不了：`activeId = pinnedId ?? selectedId` 是整体替换显示节点，
拼不出「A 的云 + B 的几何」。

### 规则

1. **底图**：显示节点自己没有 PointCloud 输出时，沿**输入边**往上游广度优先找第一片云。
   - 「最近」是硬性的：队列先进先出，同一深度全部试完才往上走一层。
   - 同深度的先后来自**输入端口的声明顺序**（`OperatorDesc.inputs` 的次序），
     不是边在 JSON 里的次序 —— 后者是生成器的实现细节，不该影响用户看到什么。
   - 找到的节点若是子图节点，用 `resolveOutput` 展开成叶子的**路径 id**（F2/M4）；
     解不开的（库算子的定义在库文件里）不算数，继续往上找。
   - 显示节点在子图**内部**、而某个入口是从外面喂进来的，搜索会翻过边界到父层继续
     （`SubgraphDef.inputs[].to` 反查端口名，再在父层找喂它的边）。
   - 上游一片云都没有时才是空态，文案改成
     「该节点无点云输出，上游也没有可当底图的点云」。
2. **标注**：栏上出现「底图：<节点名>」（`[data-testid="viewer-base"]`，
   `.viewer[data-base]` 是那个节点的本地 id），不再显示「该节点无点云输出」。
   名字取节点标题，没改过标题就取算子 label。
3. **取景**：2D 剖面与 3D 的 fit 都按「底图云 + 叠画几何」的**联合**包围盒。
   取并集而不是二选一：ROI 框只有几毫米时不会把整片剖面挤出画面，
   反过来云再大也不会把框推到画外。⤢ 按钮走同一条路。
4. **钉住语义不变**：仍然是 `pinnedId ?? selectedId` 决定显示哪个节点；
   钉住的那个节点没有云时，同样走底图规则。
5. **数据通路不变**：底图云还是既有的二进制 IPC `getOutputCloud`，
   缓存键仍是 `runId|node|port|maxPoints`（键里的 node 是底图节点，
   所以共用同一片云的几个几何节点之间来回切是命中缓存的）。**没有新增 C ABI**，
   上游查找全部在前端用 GraphDoc 的 `edges` 做（`app/src/lib/basecloud.ts`）。

### 为什么上游查找放在前端

图的连接关系本来就在前端手里（GraphDoc 是唯一数据模型，ADR-0002），
而底图是**展示决策**不是执行语义 —— 放进 C++ 就得让执行器知道「谁给谁当背景」，
那是把 UI 的口味写进内核。前端也不需要知道任何算子的名字：
判据只有「输出端口的类型是不是 PointCloud」（ADR-0003）。

### 改了哪些文件

| 文件 | 改动 |
|---|---|
| `app/src/lib/basecloud.ts` | 新增。`firstCloudPort` + `findBaseCloud`（BFS，跨子图边界） |
| `app/src/components/Viewer3D.tsx` | 取云时按底图规则挑端口；`Display` 多带一个 `base`；`unionBounds` 联合取景；栏上的「底图：」标签与 `data-base` |
| `app/src/styles.viewer.css` | `.viewer__base` |
| `scripts/e2e/page.mjs` | `selectAndReadViewer` 顺带回 `base` / `baseText` |
| `scripts/e2e/run.mjs` | ransac_plane 那条断言从「提示无点云输出」改成「底图取自上游的 sor + 画出了点」 |
| `scripts/e2e/gap.mjs` | 合成图那组加 `gen.synthetic → segment.ransac_plane` 的底图断言；真实 gap 图那组加「底图落在 `filter.radius_outlier` 上」 |

### CDP 断言

**默认门禁里也覆盖**（`suiteMeasurementOutputs`，不需要算子包）：
`gen.synthetic → segment.ransac_plane`，把四个几何输出灌到 `ransac_plane`（它只输出
Indices/Plane）上，断言 `.viewer[data-base]` 是 `gen` 那个节点、
底图标签以「底图：」开头、`.viewer__count` 的点数 > 0、`data-overlay` 仍是 4。
`suiteDemoPipeline` 里那条老断言也顺势变成同一个形状（底图取自 `sor`）。

**带算子包时**（`suiteRealGapGraph`，`LYFLOW_GAP_GRAPH` 未设时整组照旧跳过）：
选中 `gap.business_rois` 断言 `data-overlay === 4`、
`data-base` 等于图里 `filter.radius_outlier` 那个节点的 id、底图点数 > 0。

那条链路是 `n_rois ←alignment― n_select ←a― n_align_f2 ←cloud― n_filter`：
深度 3 上同时有 `n_filter`（`cloud` 口）和 `n_tpl_f2`（`tplLeft`/`tplRight` 口，
它也输出 PointCloud），端口声明顺序把 `cloud` 排在前面，所以底图是滤波后的测量云
而不是模板 —— 这正是想看的那一片。

### 验收

| 场景 | 结果 |
|---|---|
| 不带包 `pnpm check` | 绿（`cargo test` 的既有并发不稳定重跑一次，见「偏离与决策」第 8 条；该不稳定其后已修） |
| 不带包 `pnpm e2e` | 绿 |
| 带包 `pnpm check` | 绿 |
| 带包 + `LYFLOW_GAP_GRAPH`（R5）`pnpm e2e` | 绿 |

---

## A/B 全表（39 样本）

单位毫米。Δ 是 LyFlow 的拆分算子与基线 `results.csv` 之差。
「—」表示这一项没有值：基线里是空单元格，LyFlow 里是该节点没跑出结果。

| sample | 基线 gap | 基线 flush | LyFlow gap | LyFlow flush | Δgap | Δflush | 状态一致 |
|---|---|---|---|---|---|---|---|
| KUN10_HXMK2A120TA237775_R2_1 | — | — | — | — | — | — | 是 |
| KUN10_HXMK2A127TA237787_R4_2 | 5.7031 | 0.5648 | 5.7031 | 0.5648 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2A120TA237789_R4_3 | 6.0970 | 1.0817 | 6.0970 | 1.0817 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2A127TA237790_R5_4 | 6.0404 | 4.4108 | 6.0404 | 4.4108 | 0.000001 | 0.000000 | 是 |
| KUN10_HXMK2A128TA237796_R2_5 | 9.2217 | 4.1528 | 9.2217 | 4.1528 | 0.000001 | 0.000000 | 是 |
| KUN10_HXMK2A128TA237796_R3_6 | 10.1656 | 2.6033 | 10.1656 | 2.6033 | 0.000001 | 0.000000 | 是 |
| KUN10_HXMK2A128TA237796_L2_7 | — | — | — | — | — | — | 是 |
| KUN10_HXMK2A128TA237796_L3_8 | 7.8426 | 3.5084 | 7.8426 | 3.5084 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2A126TA237800_L3_9 | 7.7864 | 5.7061 | 7.7864 | 5.7061 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2A12XTA237802_R1_10 | — | 2.3737 | — | 2.3737 | — | 0.000000 | 是 |
| KUN10_HXMK2A12XTA237802_R5_11 | 6.4138 | 3.8751 | 6.4138 | 3.8751 | 0.000002 | 0.000000 | 是 |
| KUN10_HXMK2A125TA237805_L3_12 | — | — | — | — | — | — | 是 |
| KUN10_HXMK2A129TA237810_R3_13 | 7.8961 | 3.5646 | 7.8961 | 3.5646 | 0.000002 | 0.000000 | 是 |
| KUN10_HXMK2F112TA237815_R1_14 | 3.7116 | 2.6118 | 3.7116 | 2.6118 | 0.000002 | 0.000000 | 是 |
| KUN10_HXMK2F112TA237815_R4_15 | 5.3581 | 0.7866 | 5.3581 | 0.7866 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2F116TA237820_R4_16 | 4.0701 | 0.5436 | 4.0701 | 0.5436 | 0.000001 | 0.000000 | 是 |
| KUN10_HXMK2F116TA237820_R4_17 | 5.5817 | 0.7222 | 5.5817 | 0.7222 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2F116TA237820_L1_18 | 4.4753 | 0.7841 | 4.4753 | 0.7841 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2F119TA253638_R1_19 | 0.8755 | 2.4209 | 0.8755 | 2.4209 | 0.000001 | 0.000000 | 是 |
| KUN10_HXMK2F113TA253649_L3_20 | — | — | — | — | — | — | 是 |
| KUN10_HXMK2F114TA253661_L6_21 | — | — | — | — | — | — | 是 |
| KUN10_HXMK2F11XTA253664_R2_22 | 9.4304 | 2.9682 | 9.4304 | 2.9682 | 0.000002 | 0.000000 | 是 |
| KUN10_HXMK2F11XTA253664_L6_23 | — | — | — | — | — | — | 是 |
| KUN10_HXMK2F110TA253673_R2_24 | 4.8701 | 3.0761 | 4.8701 | 3.0761 | 0.000007 | 0.000000 | 是 |
| KUN10_HXMK2F118TA253677_R1_25 | 3.0535 | 2.3739 | 3.0535 | 2.3739 | 0.000001 | 0.000000 | 是 |
| KUN10_HXMK2F118TA253677_R4_26 | 5.3994 | 0.9078 | 5.3994 | 0.9078 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2F118TA253680_R1_27 | 3.5100 | 2.4660 | 3.5100 | 2.4660 | 0.000001 | 0.000000 | 是 |
| KUN10_HXMK2F118TA253680_L3_28 | — | — | — | — | — | — | 是 |
| KUN10_HXMK2F118TA253680_L4_29 | 5.3150 | 1.0424 | 5.3150 | 1.0424 | 0.000002 | 0.000000 | 是 |
| KUN10_HXMK2F117TA253685_R6_30 | 6.1218 | 4.8603 | 6.1218 | 4.8603 | 0.000002 | 0.000000 | 是 |
| KUN10_HXMK2F112TA253688_L3_31 | 7.5439 | 5.1879 | 7.5439 | 5.1879 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2A121TA253709_R2_32 | — | — | — | — | — | — | 是 |
| KUN10_HXMK2A12XTA253711_R4_33 | 5.4564 | 1.3037 | 5.4564 | 1.3037 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2F113TA253733_L2_34 | — | — | — | — | — | — | 是 |
| KUN10_HXMK2F114TA253742_R4_35 | 5.3041 | 1.2366 | 5.3041 | 1.2366 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2F117TA253749_R1_36 | 3.3177 | 2.6641 | 3.3177 | 2.6641 | 0.000001 | 0.000000 | 是 |
| KUN10_HXMK2A126TA253768_L5_37 | 8.2411 | 5.3090 | 8.2411 | 5.3090 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2A125TA253776_R4_38 | 5.2897 | 0.9245 | 5.2897 | 0.9245 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2A12XTA253787_R1_39 | — | 2.1444 | — | 2.1444 | — | 0.000000 | 是 |

**没有不一致的样本。** 最大 |Δ| 出现在 `…253673_R2_24` 的 gap 上，6.7e-6 mm，
是 float 与 double 在两条不同调用顺序上的舍入差 —— 拆分算子把
`u = (col(1) − col(0)).normalized()` 换成了从 Line2D 的两个端点现算，
两者数学上恒等，二进制上差最后一两位。

---

## 第二部分（§10）逐条

模型 ROI 路径是现场真正在用的那条：ONNX 逐槽分割 → 四个业务 ROI → 跟随零件的裁剪窗，
**无模板、无 ICP**。基线是 `%TEMP%\lyflow-gap-baseline-model\`
（`gap_batch_runner run --manifest dataset.yml --repeat 1 --dump-ml-export --roi-model <onnx>`，
cwd 是 `C:\Users\11601\OneDrive\Documents\DTS\tianmu_0904`）。
模型：`C:\Users\11601\OneDrive\Documents\DTS\models\v12s0.onnx`。

### 复现命令（第二部分）

```powershell
$env:LYFLOW_OP_PACKS = "D:\project\xyz-gap-inspector\lyflow"
$model = "C:\Users\11601\OneDrive\Documents\DTS\models\v12s0.onnx"
$data  = "C:\Users\11601\OneDrive\Documents\DTS\tianmu_0904"

# 1. 带包 / 不带包
pnpm check                       # 带包：37 算子、101 doctest
$env:LYFLOW_OP_PACKS = ""; pnpm check   # 不带包：16 算子、84 doctest

# 2. 模型 A/B（顺手把 39 张模型图生成到 %TEMP%\lyflow-gap-ab-model）
$env:LYFLOW_OP_PACKS = "D:\project\xyz-gap-inspector\lyflow"
cd D:\project\LyFlow\bridge; cargo build --bin lyflow --no-default-features; cd ..
python D:\project\xyz-gap-inspector\lyflow\tools\lyflow_ab.py `
    --dataset "$data\dataset.yml" --baseline "$env:TEMP\lyflow-gap-baseline-model" `
    --model $model --out "$env:TEMP\lyflow-gap-ab-model" `
    --json "$env:TEMP\lyflow-gap-ab-model\ab.json"

# 3. 模板 A/B 回归（不带 --model）
python D:\project\xyz-gap-inspector\lyflow\tools\lyflow_ab.py `
    --dataset "$data\dataset.yml" --baseline "$env:TEMP\lyflow-gap-baseline" `
    --out "$env:TEMP\lyflow-gap-ab"

# 4. CDP：两组一起跑
$env:LYFLOW_GAP_GRAPH       = "$env:TEMP\lyflow-gap-ab\KUN10_HXMK2A12XTA237802_R5_11.lyflow.json"
$env:LYFLOW_GAP_GRAPH_MODEL = "$env:TEMP\lyflow-gap-ab-model\KUN10_HXMK2A12XTA237802_R1_10.lyflow.json"
pnpm e2e

# 5. 安装包：onnxruntime 随包
pnpm tauri build
pnpm e2e:packaged
```

R1 / R5 的模型图（第 2 步生成）：

```
%TEMP%\lyflow-gap-ab-model\KUN10_HXMK2A12XTA237802_R1_10.lyflow.json
%TEMP%\lyflow-gap-ab-model\KUN10_HXMK2A12XTA237802_R5_11.lyflow.json
```

单独生成一张：

```powershell
python D:\project\xyz-gap-inspector\lyflow\tools\lyflow_graph_from_config.py `
    "$data\database\KUN10\device_0\R1\StandardGap.yml" `
    --primary  "$data\pointclouds\KUN10\HXMK2A12XTA237802_04-09-2026-09-11-57\device_0\R1\LaserProfile_L0_Master_R1_04-09-2026-09-11-59_0.pcd" `
    --secondary "$data\pointclouds\KUN10\HXMK2A12XTA237802_04-09-2026-09-11-57\device_0\R1\LaserProfile_R1_Slave_R1_04-09-2026-09-11-59_0.pcd" `
    --model $model -o R1_model.lyflow.json
```

---

### ✅ 1. 带包 / 不带包 `pnpm check` 全绿；`onnxruntime*.dll` 随 core `bin/` 走，干净目录里也有

**带包**（`LYFLOW_OP_PACKS=D:\project\xyz-gap-inspector\lyflow`）：

```
=== C++ core ===         ok: 37 operator(s), 11 port type(s)
                         [doctest] test cases: 101 | 101 passed | 0 failed
=== manifest vs schema ===   符合 schema/operator-manifest.schema.json
=== execution-event vs schema === ok: 16 条
=== graph-doc vs schema ===  ok: 4 node(s), 3 edge(s)
=== Rust bridge ===      test result: ok. 53 passed; 0 failed
=== headless CLI ===     算子描述自检干净
=== frontend ===         built
全链路绿
```

**不带包**：`ok: 16 operator(s), 11 port type(s)`、84 个 doctest 用例，其余各段同样通过 ——
算子数从第一部分的 32 涨到 37（新增 §9 的五个），不带包时仍然是 16，通用侧一个字没动。

两边**各有一次**撞上偏离第 8 条那个既有的 `cargo test` 并发不稳定
（带包一次 `cloud_payload_carries_normals_when_the_op_produces_them`，
不带包两次 `sweep_reuses_the_upstream_across_the_grid` + 同一个），重跑即绿。
补了一条佐证：`cargo test --lib -- --test-threads=1` 一次就是 `53 passed; 0 failed`。
（这条不稳定其后已修，见「偏离与决策」第 8 条末尾；现在默认并行下连跑 10 次也全绿。）

**DLL 随包**：`build/core/bin/` 与 `bridge/target/release/` 下都有
`onnxruntime.dll`（11 234 848 B）与 `onnxruntime_providers_shared.dll`（22 048 B）。
`pnpm e2e:packaged` 的干净目录里也有 —— 新加的断言就盯着这一条：

```
── 安装包（干净目录）
  ✓ CLI 也随包
  ✓ onnxruntime 两个 DLL 都在干净目录里
308/308 项通过，全绿
```

不带算子包跑 `e2e:packaged` 时同一条断言反过来断言「没有 onnxruntime」（按设计）。

### ✅ 2. 模型 A/B：39/39 状态一致，|Δ| ≤ 0.002 mm；现场值 41/41 在 0.006 mm 内

```
一致 39 / 39；最大 |Δ| = 0.000000 mm
现场值（manifest.csv，容差 0.006 mm）：41 / 41 个数值一致；
最大 |Δ| = 0.00499 mm（KUN10_HXMK2F118TA253680_L4_29 gap）
```

78 个可比数值（39 样本 × gap/flush）里最大 |Δ| = **4.77e-7 mm**，
`gap.measure_reference`（带 `modelPath`）与拆分算子之差同样在 4.77e-7 mm 以内。
39 个样本的 `crop_status` 也逐个对上：38 个 `applied`、1 个 `reverted:min_points`
（`KUN10_HXMK2A120TA237775_R2_1`，1278+950 个点一个没裁掉），与基线
`diagnostics.jsonl` 的分布一模一样。

**§10 里「40/41、R4_16 例外」这一条没有复现 —— 实测是 41/41。**
原因见偏离第 27 条：`manifest.csv` 与 `dataset.yml` 按 **`left_cloud` 路径** join 之后，
`R4_16` / `R4_17` 这两次相隔 50 秒的测量各自对上自己那一行（5.330 与 5.500），
不再撞在一起。计划 §7 写「两行都记的是第二次的值」，是按
（序列号, 测点, 维度）join 出来的假象。A/B 脚本用的是路径 join。

### ✅ 3. R1、R5 的模型路径图在桌面端跑出基线值

| 测点 | 期望（§10） | 实测（LyFlow 拆分算子） | 黑盒对照 `gap.measure_reference` |
|---|---|---|---|
| R1_10 gap | 3.7838 | 3.7838001545518636 | 3.7838002681732177 |
| R1_10 flush | 2.3504 | 2.3504302371293306 | 2.3504302501678467 |
| R5_11 gap | 6.4685 | 6.46850885823369 | 6.468508720397949 |
| R5_11 flush | 3.5173 | 3.5173279501497747 | 3.517327976226807 |

R1 的模型图**整张跑通、一个红框都没有** —— 与模板路径不同：模板路径上 R1 的
`gap_left` 圆拟合失败（第一部分 §5 第 4 条），模型给的 ROI 让它拟合成功了。

选中三个节点看到的东西（CDP 断言见下一条，肉眼确认同样成立）：

- `gap.roi_from_labels`：四个框叠在剖面底图上（`data-overlay=4`，底图来自上游最近的那片云）；
- `gap.labels_to_cloud`：1280 个槽都在，按类着色；
- `gap.roll_anchored_crop`：窗画出来，Inspector 里 `status` 一行写着
  `GapRollCrop {"afterPrimary":212,"afterSecondary":196,"applied":true,"beforePrimary":1231,
  "beforeSecondary":1276,"boxMm":[…],"minPointsKept":50,"status":"applied"}`。

### ✅ 4. CDP：模型路径图的三个节点各一条断言

`scripts/e2e/gap.mjs` 新增一组「模型 ROI 图：四框 / 按类着色 / 裁剪窗状态」，
`LYFLOW_GAP_GRAPH_MODEL` 指向 R1 的模型图；未设时整组跳过（与第一部分那组同样的门）。
带包 + 两个环境变量都设上跑 `pnpm e2e`：

```
── 模型 ROI 图：四框 / 按类着色 / 裁剪窗状态
  ✓ 打开图
  ✓ 模型路径整张图跑通（一个红框都不该有）
  ✓ 图里有 roi_from_labels 节点
  ✓ 四个模型 ROI 框都叠上了
  ✓ 框叠在一片真实的剖面上
  ✓ 图里有 labels_to_cloud 节点
  ✓ 着色后的剖面有点
  ✓ 逐点的类别通道到了前端（强度可选）
  ✓ 图里有 roll_anchored_crop 节点
  ✓ Inspector 里有 status 这一行
  ✓ status 是个 Record
  ✓ status 写明了裁剪窗的结局与前后点数
  ✓ 窗本身也列出来了
  ✓ 拆分算子与黑盒对照给出同一对数（差 ≤ 0.002 mm）

326/326 项通过，全绿
```

`labels_to_cloud` 那条断言的是**强度通道**而不是 rgb 通道，原因见偏离第 20 条：
LyFlow 的二进制点云载荷根本不带 rgb 位，前端看不见它。rgb 色表本身由包内 doctest 锁死。

### ✅ 5. 第一部分的 39/39 模板路径 A/B 仍然通过（回归）

```
一致 39 / 39；最大 |Δ| = 0.000000 mm
```

比第一部分记的 0.000007 mm 还小了一档 —— 偏离第 23 条那个 ROI 换算的 ULP 修正
顺带把模板路径也拉齐了（`gap.business_rois` 走的是同一个换算）。
这一趟的现场值那一列对模板路径是 0/31 一致，**这正是计划 §7 的论点**：
现场跑的不是模板路径。

---

## 模型 A/B 全表（39 样本）

单位毫米。`Δgap`/`Δflush` 是与模型基线 `results.csv` 之差，
`Δ现场` 是与数据目录 `manifest.csv`（现场值，两位小数）之差。
「—」表示这一项没有值：现场 CSV 里这个测点只记了另一个维度。

| sample | 基线 gap | 基线 flush | LyFlow gap | LyFlow flush | Δgap | Δflush | Δ现场 gap | Δ现场 flush | 状态一致 |
|---|---|---|---|---|---|---|---|---|---|
| KUN10_HXMK2A120TA237775_R2_1  | 27.7005     | 3.9329      | 27.7005     | 3.9329      | 0.00000     | 0.00000     | 0.00049    | —            | 是        |
| KUN10_HXMK2A127TA237787_R4_2  | 5.7769      | 0.7240      | 5.7769      | 0.7240      | 0.00000     | 0.00000     | 0.00307    | —            | 是        |
| KUN10_HXMK2A120TA237789_R4_3  | 6.0552      | 0.8584      | 6.0552      | 0.8584      | 0.00000     | 0.00000     | 0.00475    | —            | 是        |
| KUN10_HXMK2A127TA237790_R5_4  | 6.0934      | 4.6780      | 6.0934      | 4.6780      | 0.00000     | 0.00000     | 0.00344    | —            | 是        |
| KUN10_HXMK2A128TA237796_R2_5  | 9.4848      | 4.5328      | 9.4848      | 4.5328      | 0.00000     | 0.00000     | 0.00476    | —            | 是        |
| KUN10_HXMK2A128TA237796_R3_6  | 9.6589      | 3.9299      | 9.6589      | 3.9299      | 0.00000     | 0.00000     | 0.00110    | —            | 是        |
| KUN10_HXMK2A128TA237796_L2_7  | 5.4651      | 4.0582      | 5.4651      | 4.0582      | 0.00000     | 0.00000     | 0.00487    | —            | 是        |
| KUN10_HXMK2A128TA237796_L3_8  | 5.0925      | 3.8778      | 5.0925      | 3.8778      | 0.00000     | 0.00000     | 0.00249    | —            | 是        |
| KUN10_HXMK2A126TA237800_L3_9  | 7.5646      | 5.6202      | 7.5646      | 5.6202      | 0.00000     | 0.00000     | —          | 0.00020      | 是        |
| KUN10_HXMK2A12XTA237802_R1_10 | 3.7838      | 2.3504      | 3.7838      | 2.3504      | 0.00000     | 0.00000     | —          | 0.00043      | 是        |
| KUN10_HXMK2A12XTA237802_R5_11 | 6.4685      | 3.5173      | 6.4685      | 3.5173      | 0.00000     | 0.00000     | —          | 0.00267      | 是        |
| KUN10_HXMK2A125TA237805_L3_12 | 7.9112      | 6.6624      | 7.9112      | 6.6624      | 0.00000     | 0.00000     | —          | 0.00239      | 是        |
| KUN10_HXMK2A129TA237810_R3_13 | 5.7223      | 4.1202      | 5.7223      | 4.1202      | 0.00000     | 0.00000     | 0.00231    | —            | 是        |
| KUN10_HXMK2F112TA237815_R1_14 | 3.0337      | 2.4930      | 3.0337      | 2.4930      | 0.00000     | 0.00000     | —          | 0.00299      | 是        |
| KUN10_HXMK2F112TA237815_R4_15 | 5.3492      | 1.0947      | 5.3492      | 1.0947      | 0.00000     | 0.00000     | 0.00080    | —            | 是        |
| KUN10_HXMK2F116TA237820_R4_16 | 5.3283      | 1.3491      | 5.3283      | 1.3491      | 0.00000     | 0.00000     | 0.00169    | —            | 是        |
| KUN10_HXMK2F116TA237820_R4_17 | 5.5024      | 0.8458      | 5.5024      | 0.8458      | 0.00000     | 0.00000     | 0.00238    | —            | 是        |
| KUN10_HXMK2F116TA237820_L1_18 | 4.5277      | 0.7555      | 4.5277      | 0.7555      | 0.00000     | 0.00000     | 0.00234    | —            | 是        |
| KUN10_HXMK2F119TA253638_R1_19 | 3.6492      | 2.3496      | 3.6492      | 2.3496      | 0.00000     | 0.00000     | —          | 0.00044      | 是        |
| KUN10_HXMK2F113TA253649_L3_20 | 7.2756      | 6.2118      | 7.2756      | 6.2118      | 0.00000     | 0.00000     | —          | 0.00183      | 是        |
| KUN10_HXMK2F114TA253661_L6_21 | 8.7066      | 3.5435      | 8.7066      | 3.5435      | 0.00000     | 0.00000     | —          | 0.00346      | 是        |
| KUN10_HXMK2F11XTA253664_R2_22 | 9.3811      | 2.1252      | 9.3811      | 2.1252      | 0.00000     | 0.00000     | 0.00111    | 0.00481      | 是        |
| KUN10_HXMK2F11XTA253664_L6_23 | 7.6117      | 6.3641      | 7.6117      | 6.3641      | 0.00000     | 0.00000     | —          | 0.00413      | 是        |
| KUN10_HXMK2F110TA253673_R2_24 | 2.4519      | 3.1268      | 2.4519      | 3.1268      | 0.00000     | 0.00000     | 0.00194    | —            | 是        |
| KUN10_HXMK2F118TA253677_R1_25 | 3.0586      | 2.3105      | 3.0586      | 2.3105      | 0.00000     | 0.00000     | —          | 0.00047      | 是        |
| KUN10_HXMK2F118TA253677_R4_26 | 5.3640      | 1.2288      | 5.3640      | 1.2288      | 0.00000     | 0.00000     | 0.00395    | —            | 是        |
| KUN10_HXMK2F118TA253680_R1_27 | 3.7177      | 2.3234      | 3.7177      | 2.3234      | 0.00000     | 0.00000     | —          | 0.00344      | 是        |
| KUN10_HXMK2F118TA253680_L3_28 | 7.6072      | 6.2134      | 7.6072      | 6.2134      | 0.00000     | 0.00000     | —          | 0.00340      | 是        |
| KUN10_HXMK2F118TA253680_L4_29 | 5.3250      | 1.7619      | 5.3250      | 1.7619      | 0.00000     | 0.00000     | 0.00499    | —            | 是        |
| KUN10_HXMK2F117TA253685_R6_30 | 6.1937      | 5.1475      | 6.1937      | 5.1475      | 0.00000     | 0.00000     | 0.00365    | —            | 是        |
| KUN10_HXMK2F112TA253688_L3_31 | 7.4489      | 5.3657      | 7.4489      | 5.3657      | 0.00000     | 0.00000     | —          | 0.00426      | 是        |
| KUN10_HXMK2A121TA253709_R2_32 | 9.6650      | 2.0010      | 9.6650      | 2.0010      | 0.00000     | 0.00000     | 0.00495    | 0.00098      | 是        |
| KUN10_HXMK2A12XTA253711_R4_33 | 5.5059      | 0.8573      | 5.5059      | 0.8573      | 0.00000     | 0.00000     | 0.00413    | —            | 是        |
| KUN10_HXMK2F113TA253733_L2_34 | 5.5506      | 4.4205      | 5.5506      | 4.4205      | 0.00000     | 0.00000     | 0.00058    | —            | 是        |
| KUN10_HXMK2F114TA253742_R4_35 | 5.4187      | 0.8853      | 5.4187      | 0.8853      | 0.00000     | 0.00000     | 0.00135    | —            | 是        |
| KUN10_HXMK2F117TA253749_R1_36 | 3.3133      | 2.4422      | 3.3133      | 2.4422      | 0.00000     | 0.00000     | —          | 0.00218      | 是        |
| KUN10_HXMK2A126TA253768_L5_37 | 8.2487      | 3.6233      | 8.2487      | 3.6233      | 0.00000     | 0.00000     | —          | 0.00331      | 是        |
| KUN10_HXMK2A125TA253776_R4_38 | 5.3298      | 1.1012      | 5.3298      | 1.1012      | 0.00000     | 0.00000     | 0.00020    | —            | 是        |
| KUN10_HXMK2A12XTA253787_R1_39 | 3.5897      | 2.3587      | 3.5897      | 2.3587      | 0.00000     | 0.00000     | —          | 0.00126      | 是        |

**没有不一致的样本**，也没有一个现场值超出 0.006 mm。

---

## 偏离与决策

计划没覆盖或与计划不同的地方，全部记在这里。

**1. `nearest point` 类型做了，尽管 §6 说不做。**
数据集里 R2 那份配置（5 个样本）的 `flush.ref_type` 就是 `nearest point`。
不做它，§5 的「全部 39 个样本」就不可能成立 —— §5 是可机器断言的验收条款，
§6 是范围声明，冲突时按 §5。实现是一个十几行的算子 `gap.nearest_to_line`：
取离基准线垂距最小的那个云点，复刻 `lineCloudDistance` 的选点，之后交给
`gap.flush` 算距离，与原路径逐位相同。
`2-points line` 与 `circle tangent` 仍然不做（数据集里没有），生成器遇到会直接报错。

**2. `gap.select_alignment` 有四个候选输入，不是计划说的三个。**
R6 与 L4 两份配置各有 4 个 `template_candidates`。生成器遇到超过 4 个会报一句人话。

**3. `gap.fit_gap_circles` 多出 `leftCloud` / `rightCloud` 两个输出。**
计划只列了两个 `Indices` 输出，但内点可能指向「合并云的 ROI」也可能指向
「某台相机的 ROI」，那是算子内部才有的一片云。不把它输出来的话，
`Indices.sourceCloudId` 就指向一个图上不存在的点云，LyFlow 的对账约定就破了。
顺带也让人能看见相机回退到底选了哪一台。

**4. `gap.load_profile_pair` 加了「直接指两个文件」的模式。**
计划只说了目录 + 前缀。A/B 脚本要按 `dataset.yml` 里写死的路径取样本，
而同一个测点目录下可能有多次采集。用一个 `source` 枚举在两种模式之间切，
两组路径参数互相 `visibleWhen` 藏起来 —— LyFlow 的 Path 参数只要可见就是必填。
`gap.measure_reference` 的 `templateDir` 同理，用 `deriveTemplateDir` 开关藏起来。

**5. `robustCloudCenter` 与信赖域是照抄的，不是链接的。**
`robustCloudCenter` 在 `Alignment.cpp` 的匿名命名空间里，
`applyTrustRegion` / `decomposeDelta` / `composeDelta` 在 `GapDetection.cpp` 的匿名命名空间里，
两者都没有导出。G2 说「不复制算法源码」，但这四个函数根本拿不到 ——
所以照抄了这不到六十行，并在注释里指明了出处。其余（ICP、拟合、距离、模板选择、
换轴、`filterCloudByRoi`）全部是直接调用。

**6. `registerCloud2DICPOutcome` 靠派生一个子类拿到。**
它是 `detection::Alignment` 的 `protected` 成员，`Alignment` 又是抽象类。
包里派生了一个 `PackAlignment`，`run()` 直接返回 0（永远不会被调到），
用 `using` 把那个成员提到 public。比复制两百行 ICP 包装安全得多。

**7. 一致性门（Layer 3）只实现了 off / shadow，`enforce` 会报错。**
`ConsistencyMode` 的默认值是 `kShadow`，而数据集里 12 份配置一个都没写
`align.robustness`，所以 39 个样本走的都是 shadow —— shadow 只记录不改行为。
`enforce` 需要在**模板选择之后**拿两侧的点云重跑一次 ICP，
那要求 `gap.select_alignment` 也持有点云，会把两个算子的职责搅在一起。
配置里真的出现 `enforce` 时，`gap.align_template` 报 `bad_param` 而不是静默降级。
（这一条没有样本覆盖，标**未验证**。）

**8. `cargo test` 有一个既有的并发不稳定，不是这次引入的。**（**已修**，见本条末尾）
`execution::tests::cloud_payload_carries_normals_when_the_op_produces_them`
（有时还有 `output_cloud_binary_header_is_correct`、`cli::tests::*`）会报
`NoSuchOutput`。原因是 `commands`/`cli` 里的测试会调 `lyflow_cache_clear()`，
而结果仓是进程级的，会把并行跑着的另一个测试的结果一并清掉
（`bridge/src/commands.rs:200`、`bridge/src/cli.rs:566`）。

**对照实验**：在 `HEAD`（b8b5c1b，本次改动之前）新开一个 worktree
（`git worktree add %TEMP%\lyflow-head HEAD`），构建后连跑 8 次 `cargo test`：

```
HEAD cargo-test failures = 4 / 8
  run 5: cli::tests::subgraph_expands_into_path_ids / preview_decimates_the_source 等 4 个
  run 6: execution::tests::cloud_payload_carries_normals_when_the_op_produces_them
  run 7: 上面那个 + output_cloud_binary_header_is_correct
  run 8: 上面那个
```

同样的失败、同样的测试名。**这条路径本次一个字都没动。**
**没有顺手修**：它是 LyFlow 通用侧的测试隔离问题，与本次接入无关，
在这个 commit 里改会把两件事搅在一起。修法很清楚（给测试各自的 runId 命名空间，
或者别在单测里调进程级的 `cache_clear`），但那是另一个 commit 的事。

> **已修**（`fix: cargo 测试不再共享进程级缓存清理；注释收敛`）。
> 修法是把「不吃缓存」从进程级动作降成 run 级选项：C ABI 升 v6，
> `lyflow_run_options` 加 `no_reuse`，执行器据此跳过 `ResultStore::reuse()`
> 而**不动别人的结果**；CLI 的 `--no-cache` 改走这条路，
> `commands` 那个测试改成靠专属 seed 拿 `cached=false`，不再调 `clear_cache()`。
> 现在没有任何测试路径会调 `lyflow_cache_clear()`。
>
> 修的过程中翻出**第二个同族的根因**，本条原来没记：
> `core_ffi::tests::hot_reload_swaps_in_a_fresh_generation` 偶发
> `复制 … → lyflow_core.gen1.dll 失败: 另一个程序正在使用此文件 (os error 32)`。
> 换代 DLL 的落地名只带代数，而代数每个进程都从 0 起，
> 于是所有 `cargo test` 进程都往 `deps/lyflow_core.gen1.dll` 这一个名字上拷 ——
> 上一个进程还没把它 unmap，拷贝就撞上 sharing violation。
> 名字里加了 pid（`lyflow_core.gen1.p<pid>.dll`），并把「扫 `deps/`」这个
> 进程外可见的动作从那个命名测试里挪走（改成扫它自己的临时目录）。
>
> 连跑 `cargo test`：10/10 全绿（`53 passed; 0 failed`），另跑两轮 30 次同样 0 失败。

**9. `gap.judge` 的 `patrol` 语义是自己定的。**
计划只列了参数名。这里定成「巡检模式：超差也只标 `margin`，不判 `high`/`low`」。
它不影响 A/B —— 生成器不给 `patrol` 赋值，用默认的 `false`。

**10. 生成的图里有两个「只为了看」的节点。**
`n_crop_gapLeft` / `n_crop_gapRight` 的输出没有下游：`gap.fit_gap_circles`
按计划拿 `merged` + 两个 Box2D 自己裁。留着它们是为了能点开看间隙 ROI 里到底有什么，
代价是两次很便宜的重复裁剪。

**11. `gap.business_rois` 的端口按语义给，不是按槽位。**
原算法在 `base_side == "right"` 时先把 ROI 槽 0/2 互换，之后又把两片云换了回来。
等价于「端口 `flushBase` 永远是基准面那一格」。
按槽位给端口的话，生成器要在两种 `base_side` 下接不同的线，那是更容易接错的一边。
数据集里 12 份配置全是 `base_side: left`，所以这一条**只有单测覆盖，没有样本覆盖**。

**12. 全局粗配的目标云被加了两遍。**
原算法在 seg_mode ROI 下做 `merged_target = *left_cloud_ + *right_cloud_`，
而这两个指针指着同一片合并云 —— 目标点数因此翻倍。
这是 G8 说的「复刻行为而不是修正它」，照做了，并在代码里写明了原因。
**这是原算法的一处可疑之处**，记在这里：它让全局粗配的 fitness 计算基于一份重复的点集。

**13. 另外三处原算法的可疑之处，一并记下，都没有改。**
- `flushMeasurementMm()` 先按 `flush_pts_[0][1] > flush_pts_[1][1]` 定符号，
  再 `std::fabs()` —— 符号是死代码。
- `getLinefrom2Points()` 里 `Eigen::Vector2f center = (pt2 + pt2) / 2;`
  写的是 `pt2 + pt2` 而不是 `pt1 + pt2`。这条路径上没有走到它（`2-points line` 不做）。
- 相机分开拟合的候选打分永远用 definition B 的圆心距，即使最终按 definition A 算。
  39 个样本里没有一个走到这条分支（都是合并云一次拟合成功，或者直接失败）。

**14. `yaml-cpp.dll` 由包的 cmake 拷进 core 的 `bin/`。**
它是动态三元组的 DLL，而且不在 `C:\vcpkg` 里，vcpkg 的 applocal 那一步看不见它。
`bridge/build.rs` 整目录搬走 `bin/`，所以拷进去就够了。

**15. 为热重载给算子包加了 PCH（`lyflow_op_pack(PCH …)`）。**
计划没提。理由见 §5 第 6 条：不加的话一次增量重建 10.5 s，热重载跟不上手。
所有包共用一个对象库，所以 PCH 只能有一份；两个包给了不同的 PCH 会直接 FATAL_ERROR。

**16. 一个构建期的缺陷，已经修掉（`fix: 开发构建按自己的配置加载 core DLL…`）。**
症状：设了 `LYFLOW_OP_PACKS` 跑 `pnpm tauri dev`，状态栏显示「16 算子」，
`gap.*` 全部「未知算子」；而同一环境下带包的 `pnpm check` 是绿的、32 个算子。
本条原先归因成「DLL 被占用时 `copy_dir_contents` 静默跳过」——**那是错的**：
一个 app 都没在跑的时候照样复现。

**真正的根因**：cargo 对同一个 crate 会按 feature 集构建多次
（`lyflow-app` 带 `desktop`、lib 的 test、CLI `--no-default-features`），
每种配置有自己的 `OUT_DIR`，各自跑一遍 `bridge/build.rs`、各自编出一个
`lyflow_core.dll`，然后**都往共享的** `target/debug/` 与 `target/debug/deps/` 里拷。
实测的现场：带包那个配置的 OUT_DIR 是 5.2 MB，不带包的两个各 2.28 MB，
而 `target/debug/lyflow_core.dll` 是 2.28 MB —— 不带包那次 `pnpm check` 里
`cargo test` / CLI 构建拷过去的。之后带包跑 `tauri dev`，这个配置的输入一个没变，
`build.rs` 不重跑、也就不重拷，app 从 exe 同目录加载到的正是别人留下的那一份。
`rerun-if-env-changed=LYFLOW_OP_PACKS` 一直都在，不是它的问题；
「拷贝是 `build.rs` 的副作用，只在本配置的 `build.rs` 重跑时发生，而目的地是共享的」才是。
所以「再构建一次」并不能纠正它 —— 输入没变，就不会有第二次拷贝。

**修法**：`build.rs` 本来就发了
`cargo:rustc-env=LYFLOW_CORE_BIN=<本配置的 cmake bin 目录>`，此前没人读。
现在 `core_ffi::dll_path()` 在 `debug_assertions` 且该目录里有 DLL 时优先用它，
否则回落到 exe 同目录（release 与安装包走这条，行为不变）。
Windows 上加载改用 `LOAD_WITH_ALTERED_SEARCH_PATH`：PCL、`yaml-cpp` 这些依赖
跟着从**核心 DLL 自己的目录**解析，而不是 exe 目录。
热重载不变 —— 仍然盯 `build/core/bin/lyflow_core.dll`，仍然复制成 exe 目录下的
`lyflow_core.gen<N>.dll` 再加载；换代时依赖 DLL 已经在进程里，Windows 按模块名解析。
顺带把 `copy_dir_contents` 对 `PermissionDenied` 的静默跳过改成 `cargo:warning`，
「没能覆盖，那里还是旧的一份」这句话得有人看得见。

**实测**（状态栏算子数 + app 进程里 `lyflow_core` 那个模块的真实路径）：

| 场景 | `target/debug/` 里躺着的 | 修复前 | 修复后 |
|---|---|---|---|
| 不带包 `check` → 带包 `dev` | 不带包 2.28 MB | **16**，加载 `target/debug/` | **32**，加载本配置 OUT_DIR |
| 带包 `check` → 不带包 `dev` | 带包 5.2 MB | 同一机制，没单独复现 | **16**，加载本配置 OUT_DIR |

**17. A/B 脚本挑 `lyflow.exe` 时按 mtime 取最新的。**
第一次跑的时候它按「release 优先」挑中了一个几小时前构建的、不带算子包的 exe，
39 个样本全部静默地跑成「算子不存在」，而表格里只显示成一片「—」。
改成取最新的，并在没有任何事件时把 `lyflow run` 的退出码与 stderr 记进 errors。

### 第二部分（模型 ROI 路径）的偏离

**18. `gap.roi_from_labels` 多了一个 `baseSide` 参数。**
§9 只列了「refine 开关与五个数值」。但 `GapDetection.cpp:234` 的 `apply_business_rois`
对模型 override 与模板 ROI 用的是**同一个** base_side 互换，不加这个参数，
`base_side: right` 的点位会把基准面与参考面接反。端口按语义给（与偏离第 11 条同一条规矩）。
数据集里 12 份配置全是 `base_side: left`，所以这一条**只有单测覆盖，没有样本覆盖**。

**19. `gap.labels_to_cloud` 多了一个 `row` 参数，并且同时写 intensity。**
§9 写的是「cloud, labels → cloud」，但 labels 是两行，算子必须知道这片云是哪一行。
`row` 默认 primary（row0）。
`intensity` 是额外写的：类 id 逐点存一份。理由见下一条。

**20. LyFlow 的 3D 视图没有 rgb 着色模式，所以 §9 那句「只为了在 3D 视图里看分割结果」
靠 rgb 一个通道达不成。**
`ShadingMode` 只有 `intensity | height | normal | flat`（`Viewer3D.tsx:18`），
而二进制点云载荷的通道位只有 `LYFLOW_CLOUD_HAS_INTENSITY` 与 `..._HAS_NORMALS`
（`core/src/c_api.cpp:226`）—— **rgb 根本没进 IPC**。
所以：算子按 §9 把八类色表写进 rgb（包内 doctest 逐值锁死那三种颜色），
同时把类 id 写进 intensity，视图靠强度色带才真的看得见分割结果。
CDP 那一条断言的因此是「强度这一项没被禁用」，而不是「rgb 通道存在」——
后者在当前 ABI 下前端无从知道。**没有为此改 core**：加一个 rgb 通道位是 C ABI 与
点云载荷格式的改动，与「接一条领域路径」不是一件事，该单独提。

**21. `gap.fit_line` 多了一个 `endpoints` 参数。**
原算法按 `align_cloud_` 分两支写 `lines_["flush_base"]`（`GapDetection.cpp:861`）：
真取直线与 ROI 框的两个交点，假取**第一个与最后一个内点的真实云点**。
而模型路径上 `align_cloud_ = configuration_.align_cloud && !override_short_circuit` 恒为假
（`GapDetection.cpp:218`）。这条线的两个端点是 gap definition A 的方向 `u` 的唯一来源，
所以它不是可视化细节，是数值。生成器按路径给：模板 `roi_intersection`、模型 `inlier_ends`。
**这一条是模型 A/B 从 21/39 走到 32/39 的那一步。**

**22. `gap.flush` 多了一个 `baseLine` 输出，`gap.gap` 的 definition A 改接它。**
原算法在算完段差之后、算间隙之前，把段差的垂足并进基准线段
（`GapDetection.cpp:940`，`insertPoint2Segment`），而 definition A 的 `u` 取的是
**并过之后**的那条线段。模板路径上端点本来就在直线上，并一个同样在直线上的点不改方向，
所以第一部分没暴露；模型路径上端点是真实云点（离直线最远 `distThresh/3`），并进去就改方向。
生成器两条路径都改接 `n_flush.baseLine` —— 这是原算法的语义，不是模型路径的特例。

**23. ROI 框的毫米→米必须「先窄化成 float 再除」，不能「先除再窄化」。**
原算法是 `Matrix2f roi; roi << 双精度…; roi /= scale_;`，Eigen 的
`operator/=(const Scalar&)` 里 Scalar 是 float，所以 `scale_`（double 1000.0）也被窄化，
整个除法是 `float / float`。而包里原本写的是 `static_cast<float>(mm / 1000.0)`。
两者差最后一个 ULP，而 `filterCloudByRoi` 是**四边严格开区间**——
正好卡在这一位上：`KUN10_HXMK2F113TA253649_L3_20` 的 `flush_ref` ROI 因此少了一个点
（173 vs 基线 174），第二次直线拟合的方向从 (0.98350, 0.18093) 变成 (0.99190, 0.12704)，
段差差了 0.4 mm。
修法是新加一个 `mmToMRoi()`，只给 ROI 框用（距离与半径那些走的是 `auto d = mm / scale_`，
double 除完再窄化，仍然是 `mmToM`）。**这一条是模型 A/B 从 32/39 走到 39/39 的那一步**，
顺带把模板路径的最大 |Δ| 从 7e-6 mm 压到 0。

**24. `gap.roll_anchored_crop` 的点数保护把 `allow_single_camera` 定死为真。**
原算法的判据是四选一（`Alignment.cpp:378`），第四支要看
`preprocess(..., allow_single_camera = configuration_.common.roi_guided_segmentation, ...)`。
这条路径上 `seg_mode` 恒为 `ROI`（生成器在入口就拦掉别的），所以它恒为真，
`Both` 看的就是两片之和。算子只暴露 `usingCamera` 三选一，与 §9 的参数表一致。
`Left`/`Right` 两支有单测覆盖，没有样本覆盖（12 份配置全是 `Both`）。

**25. 窗没生效时 `window` 输出的是剩余点的包围盒，不是 ±1e7 mm 的无界框。**
原算法在 `skip_roi_crop && !roll_crop_applied_` 时也是这么记 `resolved_overall_roi_mm_` 的
（`Alignment.cpp:402`「honest diagnostics」），而且一个 2 万米宽的框画在 3D 视图里毫无意义。
`status` 里的 `boxMm` 仍然是模型真正推出来的那个窗（有的话），两者不混。

**26. roll 裁剪窗的输入框走了一次 float 往返。**
`computeRollAnchoredCropMm` 在原算法里吃的是模型输出的**双精度毫米**；
在图里它吃的是 `gap.roi_from_labels` 出的 `Box2D`，而 `Box2D` 是 float。
所以两个 roll 框的坐标被窄化过一次再乘回毫米。窗是 ±10~35 mm 的框，
往返误差在 1e-6 mm 量级，39 个样本的 `crop_status` 与前后点数全部与基线一致，
所以**没有为此改端口类型**。真要消掉，得让 `roi_from_labels` 额外吐一份双精度的 Record，
那是为一个观察不到的差异付端口复杂度。

**27. §10 的「现场值 40/41、R4_16 例外」没有复现 —— 实测 41/41。**
计划 §7 说 `R4_16` 与 `R4_17` 是同一测点相隔 50 秒的两次测量、现场 CSV 两行都记第二次的值。
按 `manifest.csv` 的 `left_cloud` 路径 join 之后并不是这样：两行分别是 5.330 与 5.500，
而基线是 5.328309 与 5.502378，各自 Δ = 0.0017 / 0.0024，都在 0.006 mm 内。
把两行撞在一起的是「按（序列号, 测点, 维度）join」——那个键在这份数据里不唯一。
`lyflow_ab.py` 用的是路径 join，`read_field_values()` 里写明了原因。

**28. A/B 脚本对现场值「只报不判」。**
现场值是两位小数、另一套构建、另一次运行，不该决定脚本的退出码。
脚本照样打一行「N / 41 个数值一致；最大 |Δ| = …」并列出超出的样本，
退出码只看基线那三条（§5 / §10 的可机器断言部分）。

**29. `gap.measure_reference` 的模型开关是 `useModel` + `modelPath` 两个参数，不是一个可空路径。**
§9 写「加参数 modelPath（可空）」，但 LyFlow 的 `Path` 参数**只要可见就是必填**
（`plan.cpp`，与偏离里 `templateDir` 那一处同一个坑）。所以加一个 Bool 开关，
`modelPath` 用 `visibleWhen` 挂在它下面 —— 与同一个算子里 `deriveTemplateDir` / `templateDir`
的写法一致。

**30. `gap.drop_non_finite` 的关键词写成 `NaN` 而不是 `nan`。**
`core/tests/test_executor.cpp:419` 断言导出的 manifest 里不含子串 `"nan"`（防止非有限
默认值漏进 JSON）。那是**子串**匹配，一个小写的关键词就能把它打红。
没有改那条测试 —— 它守的是别的东西，改判据是另一件事；包这边换个大小写就行，
并在代码里写明了为什么。

### 看图体验：模型四框与着色剖面共用测量帧底图

**31. `gap.roi_from_labels` 加了一个可选输入 `backdrop:PointCloud` 与同名输出。**
症状（截图核实）：选中 `gap.roi_from_labels` 时，底图规则（§7）取到的是它直接上游的
**原始传感器帧**云（y≡0 的 XZ 剖面），而四个框已经按 H3 换到了**测量帧**（x, y=z）。
2D 剖面俯视 XY，云退化成一条线、框飘在别处，两者根本对不上 ——
而「框压在剖面的哪里」正是这个节点唯一值得看的东西。

底图规则本身没错：它只知道「这个节点没有 PointCloud 输出」，不知道帧。
所以修法是让节点**自己带一片同帧的云**：`backdrop` 输入原样透传到同名输出
（同一个 `shared_ptr`，零拷贝），有它时 `firstCloudPort` 直接命中，BFS 不再往上游找。
没接时输出一片空云 —— 声明过的输出端口必须填（执行器的 `checkOutputs`）。
**没有改 LyFlow 的底图规则**：那条规则是通用的展示决策，
「哪一片云与我同帧」是领域知识，该由图的连接关系表达。

**32. `gap.labels_to_cloud` 改接测量帧的云。**
原先生成器把它接在 `n_load` 的原始传感器帧云上，同样俯视成一条线，
按类着色看不出剖面形状。改接 `gap.to_measurement_frame` 的输出：
换轴只换轴不删点（`pcl::transformPointCloud` 对 `is_dense=false` 的云保留 NaN 槽），
槽位与标签仍然一一对应。**必须排在 `gap.drop_non_finite` 之前** ——
删点会让槽位错位；上游已经删过点时槽数不再是 1280，算子报 `bad_input` 并说明这一点。
生成器把它的输出再接进 `roi_from_labels.backdrop`，所以选中四框时看到的
就是按类着色的测量帧剖面。`gap.roll_anchored_crop` 已自带点云输出，没有动。

**33. 为验收在 `Viewer3D` 上加了两个只读的 `data-` 属性。**
`data-cloud-bounds` / `data-overlay-bounds`（各六个数，`minXYZ,maxXYZ`）。
CDP 要断言「底图与四框在同一平面」就得读到两个包围盒，而既有的 `data-*` 只有
点数与 `data-overlay` 的个数。属性是纯只读的展示派生值，不进任何存档格式。
新增断言（`scripts/e2e/gap.mjs` 模型图那组）：四框与底图的包围盒在 XY 上相交；
底图来自节点自己的输出（`data-base` 为空）；`labels_to_cloud` 的槽数是 1280
且包围盒 y 跨度 > 0（不是一条线）。
按 CLAUDE.md 的规矩**没有写 UI 单元测试**，这一条只有 CDP 覆盖。

**验证**：带包 `pnpm check` 全绿（37 算子、**102** 个 doctest —— 多的那个是
`backdrop` 零拷贝透传与「没接就是空云」）；模型 A/B **39/39**、最大 |Δ| = 0.000000 mm、
现场值 41/41 —— 数值一个没动，这一改只动了图的连线与一个旁路输出；
带包 + 两个图的 `pnpm e2e` **331/331 通过**（比上一轮多 5 条，就是上面那几条断言）。

---

## 实测数据

（本节记录几条需要「跑一次才知道」的数字，机器：本机，MSVC 14.51 + vcpkg PCL 1.15.1。）

- 带包 `pnpm check`：全链路绿，32 个算子、11 个端口类型、94 个 doctest 用例。
- 不带包 `pnpm check`：16 个算子、11 个端口类型（除 `cargo test` 的既有不稳定外全绿）。
- A/B：39/39 一致，最大 |Δ| = 0.000007 mm，单样本平均约 0.25 s（debug 版 CLI）。
- `pnpm e2e`（不带包，默认门禁）：**297/297 通过**（「真实 gap 图」那一组按设计跳过）。
- `pnpm e2e`（带包 + `LYFLOW_GAP_GRAPH` 指 R5）：302/302 通过。
  单独跑 gap 那两组（R5 与 R1 各一次）：16/16 通过。
- 热重载：改包里一个 label → app 里的算子名换掉，**5.1 s**（generation 0 → 1）。
- 一次 A/B 全跑约 40 s（39 个样本 × 一次 `lyflow run`）。

第二部分（模型 ROI 路径）跑出来的数：

- 带包 `pnpm check`：全链路绿，**37** 个算子、11 个端口类型、**101** 个 doctest 用例。
- 不带包 `pnpm check`：16 个算子、11 个端口类型、84 个 doctest 用例
  （两边各撞一次偏离第 8 条那个既有并发不稳定，重跑即绿；
  `cargo test --lib -- --test-threads=1` 一次就 53 passed。该不稳定其后已修）。
- 模型 A/B：39/39 一致，78 个可比数值的最大 |Δ| = **4.77e-7 mm**；
  与 `gap.measure_reference`（带 modelPath）之差同样 ≤ 4.77e-7 mm。
- 模型 A/B 对现场值 `manifest.csv`：**41/41** 在 0.006 mm 内，最大 0.00499 mm。
- 模板 A/B 回归：39/39，最大 |Δ| = **0.000000 mm**（ULP 修正之后比原来还紧一档）。
- 模板 A/B 对现场值：0/31 —— 与计划 §7 的论断一致，现场跑的不是模板路径。
- `crop_status` 分布：38 个 `applied` + 1 个 `reverted:min_points`，与基线逐个对上。
- `pnpm e2e`（带包 + `LYFLOW_GAP_GRAPH` 指 R5 模板图 + `LYFLOW_GAP_GRAPH_MODEL` 指 R1 模型图）：
  **326/326 通过**。
- `pnpm tauri build` + `pnpm e2e:packaged`（带包）：**308/308 通过**，
  干净目录里有 `onnxruntime.dll` 与 `onnxruntime_providers_shared.dll`。
- 一次模型 A/B 全跑约 2 分钟（39 个样本，每个样本一次 ONNX 推理 + 一次 `lyflow run`）。

底图同帧那一轮（偏离 31–33）之后：

- 带包 `pnpm check`：全链路绿，37 个算子、**102** 个 doctest 用例。
- 模型 A/B：**39/39**，最大 |Δ| = 0.000000 mm；现场值 41/41 在 0.006 mm 内。
- `pnpm e2e`（带包 + R5 模板图 + R1 模型图）：**331/331 通过**。
