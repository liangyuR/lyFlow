# M8c 验收记录 —— 多模板的框分开处理

逐条对着 [m8-plan.md](m8-plan.md) §5 的 M8c 验收 11–14 走：**怎么跑 + 实际输出 + 通过/未通过/未验证**。
范围是 L19–L22（2026-09-24 试用反馈：槽 1 开了覆盖时 2D 视图同时画出公共四框与槽 1 的四个覆盖框，八个框叠在一起）。
基于 main `0c4cbc0`。所有命令都在 `LYFLOW_PACKS=gap;dts` 下跑，长输出落盘到 `%TEMP%` 再 grep。

## 结论

| # | 验收项 | 状态 |
|---|---|---|
| 11 | 39 个样本导入积木图，flush / gap 与 M8a/M8b 逐位相同；图里没有 `datumRoi` 等公共框参数、没有 `Override` 参数 | ✅ 通过（模板路径 39/39、模型 + 模板回退 39/39 与 M8b 全精度逐位相同；39/39 张图没有公共框 / Override） |
| 12 | 三模板的图，2D 视图切到每个槽恰好 4 个框；拖槽 2 的框只改槽 2；「复制到其它槽」后其余启用槽四框与当前槽相同 | ✅ 通过 |
| 13 | 两个框相邻时，标签包围盒不相交 | ✅ 通过 |
| 14 | 槽 3 的 datum 拖到 target 同侧，诊断标明「模板 3」 | ✅ 通过 |

## 门禁

| 命令 | 结果 |
|---|---|
| `$env:LYFLOW_PACKS="gap;dts"; pnpm check > $env:TEMP\m8c-check2.log 2>&1` | 退出码 0，末行「全链路绿」。core doctest **273/273**（M8b 271；+2：「每个模板槽各有自己的四框」「v1 → v2 迁移」），`cargo test` **136/136**，lyflow-client 单独构建、CLI `--no-default-features`、嵌入 SDK、编辑器单测 **15/15**（M8b 9；+6 `test/roiframes.test.mjs`）、前端构建、MCP 45/45 |
| `$env:LYFLOW_PACKS="gap;dts"; $env:LYFLOW_E2E_M8C_SHOT="docs/m8c-slot2.png"; pnpm e2e > $env:TEMP\m8c-e2e.log 2>&1` | 退出码 0，**527/527 项通过**（M8b 480；+47 是新的 `scripts/e2e/m8c.mjs` 三组；M8b 的四组只改了参数名，全过） |
| `pnpm core:dump`（带 `LYFLOW_PACKS`） | 重新生成 `app/public/manifest.dev.json`：71 个算子、14 个端口类型；`gap.locate_template` v2.0.0、48 个参数（文件被 `app/.gitignore` 忽略） |

grep 落盘日志：

```
grep -nE "跳过|未验|✗|FAIL" m8c-e2e.log          → 零命中
grep -n "^── M8" m8c-e2e.log                        → M8b 验收 7 / 9 / 10 / 8，M8c 验收 12 / 13 / 14，七组都在跑
grep -nE "test cases|ℹ tests|test result: ok. 1" m8c-check2.log
  [doctest] test cases:   273 |   273 passed | 0 failed | 0 skipped
  test result: ok. 136 passed; 0 failed; ...
  ℹ tests 15    （编辑器单测）
  ℹ tests 45    （MCP）
```

## 逐条

### ✅ 11. 39 个样本：读数逐位相同，图里没有公共框与 Override

脚本 `%TEMP%\claude\…\scratchpad\m8c_compare.py`（不进仓库，复用 M8a 的 `m8a_compare.py` 的导入 / 指帧 / 取数函数）对
`tianmu_0904/dataset.yml` 的每个样本：用 `pnpm check` 刚构建的 `bridge\target\debug\lyflow.exe` 导入积木图与 `--fine`
细粒度图，读剖面节点改 `source=files` 指向那一帧，`lyflow run --outputs --no-cache`，取 flush / gap。
**基准是 M8b 验收时落盘的全精度读数**（`%TEMP%\m8b-inject\rows-template.json` / `rows-model.json` 的 `files` 列，double 的 `==`），
不是表里的六位小数。数据集目录只读，所有产物写在 `%TEMP%\m8c-cmp`。

```
python m8c_compare.py --lyflow bridge\target\debug\lyflow.exe --out %TEMP%\m8c-cmp --mode template
  template: 积木图 vs M8b 读数 39/39 逐位相同；积木 vs 细粒度 39/39；积木图没有公共框 / Override 参数 39/39
python m8c_compare.py --lyflow bridge\target\debug\lyflow.exe --out %TEMP%\m8c-cmp --mode model
  model: 积木图 vs M8b 读数 39/39 逐位相同；积木 vs 细粒度 39/39；积木图没有公共框 / Override 参数 39/39
```

「没有公共框 / Override」查的是图里**所有** `gap.locate_template` 节点（模型路径的备用闭包 `b_n_locate` 也算）的参数键：
不含 `datumRoi` / `targetRoi` / `seamLeftRoi` / `seamRightRoi`、不含任何 `*Override`、也不含 `template1Enabled`。
这批配置里 13 张单槽、7 张两槽、18 张三槽、1 张四槽；**26 张图至少有一个槽的四框与槽 1 不同**（M8b 时它们是
`templateNOverride=true`），L19 改的正是这批 —— 读数照样逐位相同。

| 样本 | 槽数 | 模板 gap | 模板 flush | 同 | 模型+回退 gap | 模型+回退 flush | 同 |
|---|---|---|---|---|---|---|---|
| R2_1 | 3 | —（icp_score_low） | — | ✓ | 27.700485 | -5.932908 | ✓ |
| R4_2 | 3 | 5.699679 | 0.564443 | ✓ | 5.776925 | 0.676074 | ✓ |
| R4_3 | 3 | 6.088255 | 0.968213 | ✓ | 6.055248 | 0.141562 | ✓ |
| R5_4 | 1 | 6.143392 | -6.718779 | ✓ | 6.093440 | -7.278044 | ✓ |
| R2_5 | 3 | 9.249416 | -6.013714 | ✓ | 9.484759 | -6.532842 | ✓ |
| R3_6 | 1 | 9.616839 | -5.203312 | ✓ | 9.658230 | -6.529852 | ✓ |
| L2_7 | 3 | —（roi_empty） | — | ✓ | 5.462724 | 4.058199 | ✓ |
| L3_8 | 1 | 7.842608 | 3.470490 | ✓ | 5.092493 | 3.877840 | ✓ |
| L3_9 | 1 | 7.812856 | 5.521365 | ✓ | 7.564572 | 5.917732 | ✓ |
| R1_10 | 2 | —（circle_fit_failed） | -2.373703 | ✓ | 3.783406 | -2.378815 | ✓ |
| R5_11 | 1 | 6.499484 | -6.475078 | ✓ | 6.470677 | -6.117328 | ✓ |
| L3_12 | 1 | —（icp_score_low） | — | ✓ | 7.911205 | 6.587292 | ✓ |
| R3_13 | 1 | 7.906184 | -6.421351 | ✓ | 5.720321 | -6.789222 | ✓ |
| R1_14 | 2 | 3.755395 | -2.762496 | ✓ | 3.032188 | -2.576966 | ✓ |
| R4_15 | 3 | 5.344849 | 0.713304 | ✓ | 5.349201 | 1.094732 | ✓ |
| R4_16 | 3 | 3.616913 | 0.500760 | ✓ | 5.327485 | 1.363438 | ✓ |
| R4_17 | 3 | 5.541333 | 0.700770 | ✓ | 5.502379 | 0.154169 | ✓ |
| L1_18 | 3 | 4.380924 | 0.538841 | ✓ | 4.527664 | 0.755485 | ✓ |
| R1_19 | 2 | 1.049134 | -2.398955 | ✓ | 3.645082 | -2.349557 | ✓ |
| L3_20 | 1 | —（icp_score_low） | — | ✓ | 7.275567 | 6.211828 | ✓ |
| L6_21 | 1 | —（roi_empty） | — | ✓ | 8.706555 | 3.543462 | ✓ |
| R2_22 | 3 | 9.430362 | -4.968199 | ✓ | 9.381395 | -4.122803 | ✓ |
| L6_23 | 1 | —（roi_empty） | — | ✓ | 7.582750 | 6.364131 | ✓ |
| R2_24 | 3 | 4.870131 | -5.076068 | ✓ | 2.451944 | -5.126814 | ✓ |
| R1_25 | 2 | 3.054148 | -2.422547 | ✓ | 3.059347 | -2.264467 | ✓ |
| R4_26 | 3 | 5.400936 | 0.946991 | ✓ | 5.363951 | 1.228767 | ✓ |
| R1_27 | 2 | 3.510453 | -2.465052 | ✓ | 3.714707 | -2.323437 | ✓ |
| L3_28 | 1 | —（icp_score_low） | — | ✓ | 7.607230 | 6.144964 | ✓ |
| L4_29 | 3 | 5.318827 | 0.073212 | ✓ | 5.325021 | -0.568530 | ✓ |
| R6_30 | 4 | 6.117538 | -5.894053 | ✓ | 6.195873 | -6.147455 | ✓ |
| L3_31 | 1 | 7.555259 | 5.264531 | ✓ | 7.448915 | 5.365737 | ✓ |
| R2_32 | 3 | —（icp_score_low） | — | ✓ | 9.664622 | -4.044776 | ✓ |
| R4_33 | 3 | 5.483583 | 1.239884 | ✓ | 5.505868 | 0.142660 | ✓ |
| L2_34 | 3 | —（icp_score_low） | — | ✓ | 5.544205 | 4.420482 | ✓ |
| R4_35 | 3 | 5.304131 | 1.199189 | ✓ | 5.418651 | 0.114737 | ✓ |
| R1_36 | 2 | 3.312383 | -2.405742 | ✓ | 3.312989 | -2.216987 | ✓ |
| L5_37 | 1 | 8.236421 | -4.589374 | ✓ | 8.248722 | 3.623311 | ✓ |
| R4_38 | 3 | 5.217093 | 0.868035 | ✓ | 5.332923 | -0.043381 | ✓ |
| R1_39 | 2 | —（circle_fit_failed） | -2.131313 | ✓ | 3.591483 | -2.451444 | ✓ |

「同」= 积木图与 M8b 的读数（全精度）相同、且积木图与 `--fine` 细粒度图相同。表与 M8a 验收第 1 条、M8b 验收第 11 条的表逐位一致。

**v1 图的迁移（取舍 1 的实证）**：脚本 `m8c_migrate.py` 拿 M8b 验收时用**老 CLI** 导入、已指向那一帧的 39 张 v1 积木图
（`%TEMP%\m8b-inject\<s>-template-files.lyflow.json`，其中 32 张带 `templateNOverride`）交给 M8c 的 CLI：

```
v1 图（其中 32 张带 Override）：不补 opVersion 报 unknown_param 39/39；补了之后迁移且校验干净 39/39；读数与 M8b 逐位相同 39/39
```

`lyflow validate` 给出一条 `kind=migration` 的诊断（`gap.locate_template v1.0.0 → v2.0.0`，`params` 是改写后的完整参数），
迁移后的图跑出与 M8b 相同的读数。core 侧的同一件事在 `test_blocks.cpp`「locate_template v1 → v2 迁移」里钉着。

### ✅ 12. 三模板的图：切槽各 4 个框、拖槽 2 只改槽 2、复制到其它槽

`scripts/e2e/m8c.mjs` 的 `suiteSlotSwitch`。临时工作区里现写合成剖面与**三对模板**（槽 1 `left_template.pcd` / `right_template.pcd`，
槽 2 `f2_*`，槽 3 `f3_*`，都是各槽参数的默认文件名；三对在 x 上各错开 0 / +2 / −2 mm，好断言底图真的换了），
经 store 搭 `read_scan → locate_template`，`template2Enabled` / `template3Enabled` 打开，12 个框各有各的值。
选中 locate_template、视图切「2D 剖面」，然后**真实鼠标**逐个点切换条上的标签页、拖框、点「复制到其它槽」：

```
✓ gap.locate_template 是 v2（每槽各自四框）
✓ 切换条列出三个启用的槽                       ["模板 1 · f1","模板 2 · f2","模板 3 · f3"]
✓ 切到模板 k：恰好 4 个框                      （k = 1、2、3 各一条；.roi-box 的个数）
✓ 切到模板 k：拖框层与视图都记 4               （roi-layer 的 data-count、viewer 的 data-roi-edit）
✓ 切到模板 k：画的是它自己的四个参数           （template<k>DatumRoi / TargetRoi / SeamLeftRoi / SeamRightRoi）
✓ 切到模板 k：框的值就是这个槽的值
✓ 切到模板 k：Inspector 里只有「模板槽 k」这一节展开
✓ 三个槽的底图各不相同（换槽就换了模板云）
✓ 拖槽 2 的 datum：只有 template2DatumRoi 变了  （拖动前后整份节点参数逐键比，变了的键只有这一个）
✓ 拖动量对得上（x +1.5、y −0.5 mm，误差 < 0.35）
✓ 复制之后模板 1 的四框与模板 2 相同
✓ 复制之后模板 3 的四框与模板 2 相同
✓ 模板 2 自己没变
✓ 切到模板 3 看：画的就是复制过来的四个框
✓ 撤销一次就回到复制之前（整次复制是一条撤销）
✓ 关掉槽 3 之后切换条只列两个槽
✓ 所选的槽被关掉就退回槽 1
```

切到槽 2 时的 2D 视图：[m8c-slot2.png](m8c-slot2.png)（`LYFLOW_E2E_M8C_SHOT=docs/m8c-slot2.png` 时这一组用 CDP 的
`Page.captureScreenshot` + `clip` 只截视图那一块；不设就不写）。顶上是切换条（`模板 2 · f2` 高亮）与「复制到其它槽」，
下面是槽 2 的模板云与它自己的四个框；Seam Right 的标签被挪到了上一行，Target 的标签放在框内侧（见第 13 条）。

### ✅ 13. 相邻两个框的标签包围盒不相交

`suiteLabelsApart`：把槽 1 的 Seam Right 设成 `[1.5, 163, 4.5, 166]`、Target 设成 `[4.6, 162.5, 15, 166]`（间隔 0.1 mm，
即截图里的情形：Seam Right 窄、标签比框宽，右边紧挨着 Target），读四个标签的 `getBoundingClientRect()`：

```
✓ Seam Right 与 Target 两个框是相邻的（屏幕上间隔 < 4 px）
✓ 对照：两个标签都放在框上方的话会相交          （用框的矩形算出老摆法的标签位置 —— 证明这条断言不是白给的）
✓ Seam Right 与 Target 的标签包围盒不相交
✓ 这一组四个标签两两都不相交
```

摆法是纯函数 `placeLabels`（`packages/editor/src/lib/roiFrames.ts`），编辑器单测另有两条（两个挨着的框、四个挤成一排的框）。

### ✅ 14. 槽 3 的 datum 拖到 target 同侧，诊断标明「模板 3」

`suiteSlot3WrongSide`：切到模板 3，真实鼠标把 `template3DatumRoi` 拖到缝的右边（target 旁边）：

```
✓ 松手后 locate_template 标红
✓ 诊断指到 template3DatumRoi
✓ 诊断标明「模板 3」          「模板 3 · f3：datum 与 target 落在缝的同一侧（都在右边）—— 段差要两侧各取一个面」
✓ 诊断说的是同一侧
✓ 节点上贴的诊断也标明「模板 3」
✓ 其它槽没被牵连（诊断只有这一条）
✓ Inspector 里「模板槽 3」这一节标红
✓ 撤销之后回到干净
```

core 侧 `test_blocks.cpp`「每个模板槽各有自己的四框」：导入的三槽图把槽 3 的 datum 挪到 target 一侧，唯一一条 error
`paramPath=template3DatumRoi`、message 以 `模板 3 · f3：` 开头；把 `template3Enabled` 关掉之后校验干净（关着的槽不查）。
CLI `import_defaults_to_blocks_and_fine_flag_gives_the_fine_graph` 改成拖 `template1DatumRoi`，`lyflow validate` 照样退出 1。

## 参数结构（L19，`gap.locate_template` 2.0.0）

| 组 | 参数 | 说明 |
|---|---|---|
| 基础 | `templateDir`、`minScore` | 不变 |
| 模板槽 1（基础，恒启用） | `template1Id`、`template1Left`、`template1Right`、`template1DatumRoi`、`template1TargetRoi`、`template1SeamLeftRoi`、`template1SeamRightRoi` | 没有 `template1Enabled` |
| 模板槽 2–4（高级） | `template<k>Enabled`（默认 false）+ 同上七个 | 七个都 `visibleWhen template<k>Enabled=true`；启用后四框必填（validate 按槽查） |
| 整体框 / ICP（高级） | 不变 | |

删掉的：`datumRoi`、`targetRoi`、`seamLeftRoi`、`seamRightRoi`、`template{1..4}Override`、`template1Enabled`。
框的 label 是 `Datum ROI` / `Target ROI` / `Seam Left ROI` / `Seam Right ROI`，每个框的 `roiBackdrop` 是
`{ dir: templateDir, files: [template<k>Left, template<k>Right], label: "模板 <k>", labelParam: "template<k>Id" }`。

导入器：每个候选写进自己的槽、四框**总是显式写**（候选自带 rois 的用自己的，没带的在解析时已用配置的全局 rois 填好），
槽 2–4 写 `Enabled=true`，不再写任何公共框或 Override。`Candidate::sameAsTop` 随之删掉。

manifest 的 `roiBackdrop` 多两个可选字段 `label`、`labelParam`（schema、C++ `RoiBackdrop`、TS `RoiBackdrop`、
`docs/operator-manifest.md` 一并更新；`lyflow manifest --check` 查 `labelParam` 指向本算子的一个 string 参数）。

## 取舍（计划没覆盖、又影响方向的）

1. **`gap.locate_template` 升主版本到 2.0.0，带一条 v1 → v2 迁移。** M8a 取舍 14 只升次版本，是因为那几处改动老图照样能跑；
   L19 删掉了参数，v1 图在 v2 上会报 `unknown_param`，所以按 ADR-0008 给迁移链。规则（`migrateLocateTemplateFromV1`）：
   没开覆盖的槽把公共四框抄进自己的四框，开了的保留自己的覆盖框；关着的槽也抄一份（打开就能用）；
   v1 槽 1 关着时把第一个启用的槽换到槽 1、原槽 1 挪到它的位置并关着 —— 启用槽的先后不变，`select_alignment` 打平时的
   次序（order）因此不变；换了槽号的 Id / 文件名把旧槽的默认值显式写出来。参数里没有任何 v1 才有的键时原样返回
   （改写对 v2 参数不幂等）。**限制**：core 只对带 `opVersion` 的节点走迁移，而导入器从来不写 `opVersion` ——
   M8a / M8b 导入、之后没在编辑器里另存过的图打开会报 `unknown_param datumRoi`（第 11 条实测 39/39），
   要重新导入，或者在 locate_template 节点上补 `"opVersion": "1.0.0"`。在编辑器里新拖出来的节点都带 `opVersion`，打开时自动迁移。
2. **槽 1 整组都是「人要填的」基础参数**（含 `template1Id` / `Left` / `Right`），§3 原写「高级：四个模板槽」。槽 1 恒启用之后
   它就是那个模板，四个框本来就在基础组里（§3「人要填的：四个角色框」），文件名与框分在两处反而拆散了「一个模板一组」。
   槽 2–4 照旧在高级里。
3. **切换条是通用机制，不认算子名**：可见的 roi 参数按 `roiBackdrop`（`dir` + `files`）分组，一组一个标签页，名字取
   `roiBackdrop.label` + `labelParam` 的当前值（「模板 2 · f2」）。视图一次只画选中的那一组，其余组的框不画。
   有带底图的组时数据坐标系里的框（`overallRoi`）不进 2D 拖框（与 M8b 取舍 7 相同）。只有一个启用槽时切换条照样显示
   （告诉人现在画的是哪个模板），「复制到其它槽」置灰。
4. **选中哪一组存在 ui store**（`roiFrame[nodeId]`，不进 doc、不进撤销）：切换条与 Inspector 共用。选中的槽被关掉就退回第一组。
   颜色按组内位置取，每个槽的 datum 都是同一种颜色，切槽时不换色。
5. **Inspector 里「同步展开」做成手风琴**：各槽的参数组变成可折叠的一节（`<details>`，开合由 `roiFrame` 决定），
   选中的槽那一节展开、其余收起；反过来**展开另一节也就切了视图里的槽**。没启用的槽那一节照样能展开（去勾 Enabled），
   这时视图仍画第一组；再点一下展开的标题可以收起。收起的一节里有校验错误时标题标红；抽屉里点了指向某个槽参数的诊断，
   自动切到那个槽（这一条没有专门的 e2e）。别的算子（只有一组框或没有框）的 Inspector 不变。
6. **「复制到其它槽」按组内声明顺序一一对应**（每个槽都是 datum、target、seamLeft、seamRight），只写**启用的**槽，
   整次复制一条撤销；写的是当前槽的有效值（没填过的 `[0,0,0,0]` 也照写）。框数对不上的组跳过（locate_template 不会出现）。
7. **标签避让（L21）**：每帧按屏幕位置从左到右贪心摆，依次试「框上方」「框内左上（框放得下标签才用）」「上方错开一到三行」
   「框下方」，取第一个与已摆好的标签都不相交的。只保证标签之间互不遮挡，不保证标签不压别的框身（标签
   `pointer-events: none`，不挡拖动；`z-index` 抬到框之上）。错开的标签与框靠颜色对应，没有连线。
8. **诊断前缀「模板 k · <Id>：」**（L22 说「写明是哪个槽」，验收 14 要「模板 3」）：与切换条上的写法一致；
   「槽之间基准件不同侧」那条写成「……与模板 1 · f1 不一致」。M8a 的「四个模板槽一个都没启用」那条随 L19（槽 1 恒启用）删掉。
9. **e2e `m8b.mjs` 只改了参数名**（`datumRoi` → `template1DatumRoi` 等），并导出几个辅助函数给 `m8c.mjs` 复用；断言一条没改。

## 未验证

- **真实工件上手工切槽、拖框、复制**：e2e 用合成剖面；天幕 R 系列三槽的真实模板上切槽的手感没有人看过。
- **`pnpm e2e:http`（不是门槛）没过**：「HttpTransport：搭图 + 快捷键运行」那一组在 F5 之后等运行结束超时（24/25）。
  把 `packages/editor` 暂存回 HEAD 再跑，同一步同样超时 —— 不是 M8c 引入的，没有继续查。
- 导入器不写 `opVersion`，所以老的导入图不会自动迁移（取舍 1）。要不要让导入器给每个节点写上 `opVersion` 留给后续决定。
- HTTP 传输下的底图与 MCP 对 `roiBackdrop.label` 的展示（MCP 读 manifest 是宽松的，没有坏）。
- e2e 的两组真实 gap 图（`LYFLOW_GAP_GRAPH*`）照旧未设、整组不跑（与 M7 / M8a / M8b 相同）。

## commit

见 `git log`：gap 的 locate_template v2（每槽四框、按槽校验、v1 迁移）+ 导入器 + manifest 的 `roiBackdrop.label` +
编辑器（模板切换条、复制到其它槽、Inspector 手风琴、标签避让）+ e2e 与本验收记录，一个 commit。
`docs/m7-plan.md`、`docs/m8-plan.md`、`.claude/launch.json` 未改动。

## 补：导入器写 opVersion（验收后的小改，fix(m8c)）

取舍 1 的限制（导入的图没有 `opVersion`，算子升主版本后不会自动迁移）在导入器里补上：`import_standard_gap.cpp` 的
`Builder::node` 给每个节点写 `opVersion` = 当前注册表（`ensureRegistry()`）里该算子的 `version`，积木图与 `--fine`
细粒度图、模型路径的回退闭包都一样 —— 与编辑器新建节点（`packages/editor/src/store/graph.ts` 写 `opVersion: op.version`）一致。
以后算子升主版本，M8c 起导入的图打开时按 ADR-0008 走迁移；M8c 之前导入的图仍然没有 `opVersion`，照取舍 1 的办法处理。

- 测试：`test_import_bundle.cpp`「导入的图每个节点都写了 opVersion，等于 manifest 里该算子的版本」——
  `StandardGap.yml:template` / `:template:fine` / `:model` / `:model:fine`（带 `setting.yml`，含 `flow.fallback` 回退）四种导入，
  每个节点都有 `opVersion`，且等于 `toManifestJson()` 里同一算子的 `version`（`gap.locate_template` 是 2.0.0）。
- 门禁：`$env:LYFLOW_PACKS="gap;dts"; pnpm check > $env:TEMP\m8c-check3.log 2>&1` 退出码 0、「全链路绿」；
  core doctest **274/274**（+1），`cargo test` 136/136，编辑器单测 15/15，MCP 45/45。e2e 按要求没有重跑。
- 旁证：CLI 导入一份 tianmu_0904 的 StandardGap.yml，10 个节点都带 `opVersion`（`gap.locate_template` 2.0.0、
  `gap.read_scan` 1.1.0 ……），`lyflow validate` 零诊断（版本与当前一致，不出 `version_mismatch`）。
