# 接入 xyz-gap-inspector 验收记录

逐条对着 [gap-integration-plan.md](gap-integration-plan.md) §5 走一遍：**怎么跑 + 实际输出 + 通过/未通过/未验证**。
与计划不同的地方全部记在最后的「偏离与决策」里。

两个仓库：`D:\project\LyFlow`（main）与 `D:\project\xyz-gap-inspector`（分支 `lyflow-ops`）。
数据在 `C:\Users\11601\OneDrive\Documents\DTS\tianmu_0904`，基线在 `%TEMP%\lyflow-gap-baseline`。

两个 commit（都没 push）：

| 仓库 | 分支 | commit |
|---|---|---|
| LyFlow | `main` | `feat: 算子包机制、2D 几何类型与剖面叠画 —— 为接入 gap-inspector` |
| xyz-gap-inspector | `lyflow-ops` | `1aca0b9  LyFlow 算子包：配置/模板路径的拆分算子、图生成器与 A/B 脚本` |

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

> **一个已知的既有不稳定**：`cargo test` 有大约 1/6 的概率在
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

**8. `cargo test` 有一个既有的并发不稳定，不是这次引入的。**
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
