# M8a 验收记录 —— Bundle 类型、gap 积木算子、导入器默认积木图

逐条对着 [m8-plan.md](m8-plan.md) §4 的 M8a 验收 1–6 走：**怎么跑 + 实际输出 + 通过/未通过/未验证**。
范围是 L1–L12；M8b（L13–L17，编辑器）不做，它要对着的接口写在文末「冻结接口」。
基于 main `499fb76`。所有命令都在 `LYFLOW_PACKS=gap;dts` 下跑，长输出落盘到 `%TEMP%` 再 grep。

## 结论

| # | 验收项 | 状态 |
|---|---|---|
| 1 | 夹具与 39 个样本上积木图 vs `--fine` 细粒度图的 flush、gap 逐帧相同 | ✅ 通过（夹具 1/1；模板路径 39/39，模型 + 回退 39/39，逼出回退 39/39，全部逐位相同） |
| 2 | 模板路径积木图节点数 ≤ 12；积木算子参数里没有 side / toward / baseSide / datumSide | ✅ 通过（12 份配置的 39 张图都是 10 个节点） |
| 3 | Bundle：字段缺失/类型不符报 contract_violation；`scan.merged` 取得到点云；图输出指向字段；ScanPair 接 RoiSet 报 type_mismatch | ✅ 通过 |
| 4 | `locate_template` 把 datum 拖到 target 同侧，`lyflow validate` 执行前报错 | ✅ 通过 |
| 5 | 细粒度算子既有测试全过；同一张图混用积木与细粒度算子能跑 | ✅ 通过 |
| 6 | `pnpm check`、`pnpm e2e` 全绿，日志 grep 无「跳过 / 未验」 | ✅ 通过 |

## 门禁

| 命令 | 结果 |
|---|---|
| `$env:LYFLOW_PACKS="gap;dts"; pnpm check > $env:TEMP\m8a-check.log 2>&1` | 退出码 0，末行「全链路绿」。core doctest **265/265**（M7 是 247；新增 `test_bundle.cpp` 6 条、`test_blocks.cpp` 11 条，`test_import_bundle.cpp` 的回退用例拆成积木 / 细粒度两条），三份 schema、`cargo test` **131/131**（+1 `import_defaults_to_blocks_and_fine_flag_gives_the_fine_graph`）、lyflow-client 单独构建、CLI `--no-default-features`、嵌入 SDK、编辑器单测 4/4、前端构建、MCP 45/45 |
| `$env:LYFLOW_PACKS="gap;dts"; pnpm e2e > $env:TEMP\m8a-e2e.log 2>&1` | 退出码 0，**424/424 项通过**（与 M7 相同：M8a 没有前端行为改动） |
| `pnpm core:dump`（带 `LYFLOW_PACKS`） | 重新生成 `app/public/manifest.dev.json`：71 个算子（+11：七个积木、两对 make/split）、14 个端口类型（+`Bundle`）、`bundles` 段两项。该文件被 `app/.gitignore` 忽略，不进 commit |

grep 落盘日志：

```
grep -nE "跳过|未验|FAILED|skip" m8a-check.log
  153: [doctest] test cases:   265 |   265 passed | 0 failed | 0 skipped
  251: -- Check for working CXX compiler: ... - skipped            （CMake 自己的探测）
  283: ℹ skipped 0                                                  （编辑器单测）
  340: ✔ stderr 掺进 stdout 的行被跳过而不是让解析崩掉              （MCP 现有用例的标题）
  375: ℹ skipped 0                                                  （MCP）
grep -nE "跳过|未验|✗|FAIL" m8a-e2e.log                  → 零命中
```

（命中的类别与 M7 相同，都不是跳过。）

## 逐条

### ✅ 1. 积木图与细粒度图逐帧相同

**夹具**（仓库内、合成数据）：`packs/gap/tests/test_blocks.cpp`「夹具上积木图与 --fine 细粒度图的 flush、gap
逐位相同（M8a 验收 1）」。在临时目录写一对传感器帧 PCD（左高右低、缝两侧各一段 R1 mm 的圆角，Master 里每 97 个点
插一个 NaN 槽，Slave 错开半个点距）和一对模板，配置带 `auto_center` 整体框与半径去噪；同一份 YAML 分别用
`StandardGap.yml:template` 与 `StandardGap.yml:template:fine` 导入、各跑一遍，`CHECK(a.flush == b.flush)`、
`CHECK(a.gap == b.gap)` 是 double 的 `==`。读数 flush 0.999972、gap 4.05212（几何真值 1 与 √37−2≈4.083）。
另有「基准件的侧由框推出：base_side 与几何不符时……」一条在翻了 `base_side` 的配置上再比一次，也逐位相同。

`test_import_bundle.cpp` 的 `kConfig` 夹具（没有点云，测导入结构）的六种 kind 都过 core 的 validate。

**39 个样本**（`tianmu_0904`，同 M7 的对照方法）：脚本
`%TEMP%\claude\…\scratchpad\m8a_compare.py`（不进仓库）对 `dataset.yml` 的每个样本：

```
lyflow import <config>/StandardGap.yml --kind StandardGap.yml:template        -o <s>-blocks.lyflow.json
lyflow import <config>/StandardGap.yml --kind StandardGap.yml:template --fine -o <s>-fine.lyflow.json
# 读剖面节点改成 source=files 指向那一帧的两片 PCD（积木图是 gap.read_scan，细粒度图是 gap.load_profile_pair）
lyflow run <graph> --base-dir <config 所在目录> --outputs --no-cache
```

从 `--outputs` 那一行取 flush / gap（`to_chars` 最短往返表示，文本相同 ⇔ double 相同）。模型模式另在导入的
`--base-dir` 放一份 `setting.yml`（`model_roi.enabled: true`）得到**带模板回退**的图，运行时
`--param modelPath=<v12s0.onnx>`；「逼出回退」把 `modelPath` 指到不存在的文件，积木图的 `n_model` 与细粒度图的
`n_infer` 都报 `bad_param`，两张图都走模板备用闭包。CLI 用 `pnpm check` 刚构建的 `bridge\target\debug\lyflow.exe`。

```
template:        39/39 逐位相同
model:           39/39 逐位相同
model-fallback:  39/39 逐位相同
```

| 样本 | 模板 gap（积木 = 细粒度） | 模板 flush | 同 | M7 gap | M7 flush | 模型+回退 gap | 模型+回退 flush | 同 | 逼出回退 同 |
|---|---|---|---|---|---|---|---|---|---|
| R2_1 | —（icp_score_low） | — | ✓ | — | — | 27.700485 | -5.932908 | ✓ | ✓ |
| R4_2 | 5.699679 | 0.564443 | ✓ | 5.700 | 0.713 | 5.776925 | 0.676074 | ✓ | ✓ |
| R4_3 | 6.088255 | 0.968213 | ✓ | 6.088 | 1.038 | 6.055248 | 0.141562 | ✓ | ✓ |
| R5_4 | 6.143392 | -6.718779 | ✓ | 6.143 | -6.719 | 6.093440 | -7.278044 | ✓ | ✓ |
| R2_5 | 9.249416 | -6.013714 | ✓ | 9.249 | -6.014 | 9.484759 | -6.532842 | ✓ | ✓ |
| R3_6 | 9.616839 | -5.203312 | ✓ | 9.617 | -5.203 | 9.658230 | -6.529852 | ✓ | ✓ |
| L2_7 | —（roi_empty） | — | ✓ | — | — | 5.462724 | 4.058199 | ✓ | ✓ |
| L3_8 | 7.842608 | 3.470490 | ✓ | 7.843 | 3.470 | 5.092493 | 3.877840 | ✓ | ✓ |
| L3_9 | 7.812856 | 5.521365 | ✓ | 7.813 | 5.521 | 7.564572 | 5.917732 | ✓ | ✓ |
| R1_10 | —（circle_fit_failed） | -2.373703 | ✓ | — | -2.374 | 3.783406 | -2.378815 | ✓ | ✓ |
| R5_11 | 6.499484 | -6.475078 | ✓ | 6.499 | -6.475 | 6.470677 | -6.117328 | ✓ | ✓ |
| L3_12 | —（icp_score_low） | — | ✓ | — | — | 7.911205 | 6.587292 | ✓ | ✓ |
| R3_13 | 7.906184 | -6.421351 | ✓ | 7.906 | -6.421 | 5.720321 | -6.789222 | ✓ | ✓ |
| R1_14 | 3.755395 | -2.762496 | ✓ | 3.755 | -2.762 | 3.032188 | -2.576966 | ✓ | ✓ |
| R4_15 | 5.344849 | 0.713304 | ✓ | 5.345 | 0.787 | 5.349201 | 1.094732 | ✓ | ✓ |
| R4_16 | 3.616913 | 0.500760 | ✓ | 3.617 | 0.469 | 5.327485 | 1.363438 | ✓ | ✓ |
| R4_17 | 5.541333 | 0.700770 | ✓ | 5.541 | 0.701 | 5.502379 | 0.154169 | ✓ | ✓ |
| L1_18 | 4.380924 | 0.538841 | ✓ | 4.381 | 0.539 | 4.527664 | 0.755485 | ✓ | ✓ |
| R1_19 | 1.049134 | -2.398955 | ✓ | 1.049 | -2.399 | 3.645082 | -2.349557 | ✓ | ✓ |
| L3_20 | —（icp_score_low） | — | ✓ | — | — | 7.275567 | 6.211828 | ✓ | ✓ |
| L6_21 | —（roi_empty） | — | ✓ | — | — | 8.706555 | 3.543462 | ✓ | ✓ |
| R2_22 | 9.430362 | -4.968199 | ✓ | 9.430 | -4.968 | 9.381395 | -4.122803 | ✓ | ✓ |
| L6_23 | —（roi_empty） | — | ✓ | — | — | 7.582750 | 6.364131 | ✓ | ✓ |
| R2_24 | 4.870131 | -5.076068 | ✓ | 4.870 | -5.076 | 2.451944 | -5.126814 | ✓ | ✓ |
| R1_25 | 3.054148 | -2.422547 | ✓ | 3.054 | -2.423 | 3.059347 | -2.264467 | ✓ | ✓ |
| R4_26 | 5.400936 | 0.946991 | ✓ | 5.401 | 0.947 | 5.363951 | 1.228767 | ✓ | ✓ |
| R1_27 | 3.510453 | -2.465052 | ✓ | 3.510 | -2.465 | 3.714707 | -2.323437 | ✓ | ✓ |
| L3_28 | —（icp_score_low） | — | ✓ | — | — | 7.607230 | 6.144964 | ✓ | ✓ |
| L4_29 | 5.318827 | 0.073212 | ✓ | 5.319 | 0.073 | 5.325021 | -0.568530 | ✓ | ✓ |
| R6_30 | 6.117538 | -5.894053 | ✓ | 6.118 | -5.894 | 6.195873 | -6.147455 | ✓ | ✓ |
| L3_31 | 7.555259 | 5.264531 | ✓ | 7.555 | 5.265 | 7.448915 | 5.365737 | ✓ | ✓ |
| R2_32 | —（icp_score_low） | — | ✓ | — | — | 9.664622 | -4.044776 | ✓ | ✓ |
| R4_33 | 5.483583 | 1.239884 | ✓ | 5.484 | 1.240 | 5.505868 | 0.142660 | ✓ | ✓ |
| L2_34 | —（icp_score_low） | — | ✓ | — | — | 5.544205 | 4.420482 | ✓ | ✓ |
| R4_35 | 5.304131 | 1.199189 | ✓ | 5.304 | 1.199 | 5.418651 | 0.114737 | ✓ | ✓ |
| R1_36 | 3.312383 | -2.405742 | ✓ | 3.312 | -2.406 | 3.312989 | -2.216987 | ✓ | ✓ |
| L5_37 | 8.236421 | -4.589374 | ✓ | 8.236 | -4.589 | 8.248722 | 3.623311 | ✓ | ✓ |
| R4_38 | 5.217093 | 0.868035 | ✓ | 5.217 | 0.893 | 5.332923 | -0.043381 | ✓ | ✓ |
| R1_39 | —（circle_fit_failed） | -2.131313 | ✓ | — | -2.131 | 3.591483 | -2.451444 | ✓ | ✓ |

「—」是两张图在同一处之前失败（括号里是积木图报的码；细粒度图报在对应的细粒度节点上，码相同，例如
`n_locate:icp_score_low` ↔ `n_select:icp_score_low`）。M7 列取自 M7 验收的 `%TEMP%\m7r-ab\compare.csv`（三位小数）。
逼出回退的 39 行读数与模板路径逐位相同（备用闭包就是模板路径），表里只列了「同」。

**与 M7 的差别只在 R4 的五个 flush 上**（gap 全部 39 行与 M7 相同）。定位办法：把 M7 验收时导入的 R4 细粒度图
（`%TEMP%\m7r-ab\R4_*-new.lyflow.json`，M7 的接法：`fit_line.toward` 接同侧 gap 框、去噪在裁剪之后）原样放到 M8a 的
CLI 上跑，读数**与 M7 验收表一致**（三位小数）；只把两条线的 `toward` 改接 `n_rois.seam`，就**逐位等于 M8a 的读数**
（全精度比，脚本 `m7_isolate.py`）：

| 样本 | M7 接法 @ M8a CLI | toward → seam @ M8a CLI | M8a 积木 = 细粒度 |
|---|---|---|---|
| R4_2 | 0.7131048531737179 | 0.5644426327198744 | 0.5644426327198744 |
| R4_3 | 1.038495893124491 | 0.968212616397068 | 0.968212616397068 |
| R4_15 | 0.7873659250326455 | 0.7133035741280764 | 0.7133035741280764 |
| R4_16 | 0.4687901581637562 | 0.5007601046263517 | 0.5007601046263517 |
| R4_38 | 0.8925205382984132 | 0.8680354857351631 | 0.8680354857351631 |
| R4_17 / R4_26 / L5_37 / R1_14 / R3_6（对照） | 三列相同 | 三列相同 | 三列相同 |

原因是 R4 的右缝框中心 x = 36.57 mm 落在参考面框 [35.57, 39.07] **里面**：M7 的 innerEnd 取的是参考面中段离
36.57 最近的点，「靠缝那一端」的截取也围着框中段转；L7 的朝向是两个缝框中心的中点（33.10），落在参考面框外、
靠缝一侧，innerEnd 回到真正靠缝的那一头。其余点位的缝框中心都在线段投影范围之外，两种朝向排出来的内点序一样。
这也同时说明另外两处改动在这批数据上没有改读数：去噪挪到裁剪之前（取舍 3）—— 上表十个样本上「M7 接法（去噪在后）+ seam」
与 M8a（去噪在前）逐位相同，其余样本与 M7 在三位小数上相同；L5 的 datumSide 从配置的 left 变成几何推出的 right
（取舍 4）—— L5_37 的读数与 M7 逐位相同（左右两侧 ICP 给出的是同一个变换）。

### ✅ 2. 节点数与参数名

- 12 份真实配置导入的 39 张模板路径积木图节点数全是 **10**：`n_scan`（read_scan）、`n_locate`（locate_template）、
  `n_line`（role_line）、`n_ref_point`（ref_point）、`n_flush`、`n_circles`（seam_circles）、`n_gap`、`n_judge_0`、
  `n_judge_1`、`n_bundle`（对照脚本的 `rows-template.json`；细粒度图 25–31 个）。计划正文说「共 9 个」，
  数的时候两个 judge 只算了一个，按它列出的节点实际是 10 个，≤ 12。
- `test_blocks.cpp`「导入的模板路径积木图：10 个节点……」：节点数 == 10、每个节点的 op 只能是七个积木或
  flush / gap / judge / result_bundle、节点参数里没有四个禁用名、图里一条 Box2D 边都没有，且过 validate。
  配了方向基准的是 11 个（「方向基准：写得成积木就是一个 gap.datum_direction……」）。
- `test_blocks.cpp`「积木算子的参数里没有 side / toward / baseSide / datumSide（m8-plan L7）」：七个积木算子的全部
  参数声明逐个比对。manifest 核对：

```
python -c "... ops = [o for o in m['operators'] if o['category']=='间隙/积木'] ..."   → 七个算子、135 个参数，
  名字里含 side / toward / baseside / datumside（不分大小写、子串）的 0 个
  （datumSide 只出现在 RoiSet.info 的数据里，不是参数）
```

### ✅ 3. Bundle

| 要求 | 用例 | 位置 |
|---|---|---|
| 字段缺失、类型不符 → `contract_violation` | 「字段齐全、类型对得上才放行，否则 contract_violation」：缺字段、字段类型错、kind 错、多一个未声明字段四种；节点 `error.code=contract_violation`、`portName=pair`；summary `contractViolations` 一条，`expected.bundle=test.Pair`、`expected.fields.box=Box2D`；下游 `cancelled` | `core/tests/test_bundle.cpp` |
| `lyflow_output_cloud(run, node, "scan.merged")` | core：`lyflow_output_cloud(run, "n_pair", "pair.cloud")` 返回 0、`total_points=5`；`pair.nope` / `pair.box` 返回 1；gap：「gap 的两种 Bundle：scan.merged 取得到点云……」在合成夹具上 `lyflow_output_cloud(run, "n_scan", "scan.merged")` 返回 0（>1000 点），`n_locate` 的 `scan.merged` 点数不多于它 | `test_bundle.cpp` / `test_blocks.cpp` |
| `lyflow_output_info` 对 Bundle 端口列出每个字段 | `[pair (Bundle<test.Pair>, elementCount=2, value.fields), pair.cloud, pair.box]`；事件 `stats.outputs` 同样三项，缓存命中（skipped）时也是 | `test_bundle.cpp` |
| 图输出指向字段 | `outputs: {box: pair.box, cloud: pair.cloud, whole: pair}`：validate 干净、`lyflow_run_outputs` 与 summary 给字段自己的类型与值；`pair.nope`、`nope.box`、非 Bundle 端口的 `cloud.x` 都是 `unknown_port`。gap 上 `rois.datum`（Box2D）与 `rois.info`（`datumSide=left`、`source=template`） | 同上 |
| `Bundle<gap.ScanPair>` 接 `Bundle<gap.RoiSet>` → `type_mismatch` | 把 `n_scan.scan` 接到 `n_line.rois`：`type_mismatch`，「类型不匹配：Bundle<gap.ScanPair> → Bundle<gap.RoiSet>（端口 rois）」；core 另测 Bundle 接普通类型也报，`flow.fallback` 的 Any 端口推成 Bundle 照常跑 | `test_blocks.cpp` / `test_bundle.cpp` |
| 声明写错被自检拒掉 | 字段类型 Any / 嵌套 Bundle / 字段名带点 / 未知类型、端口引用未声明 kind、裸 `Bundle` | `test_bundle.cpp` |

### ✅ 4. datum 拖到 target 一侧

- core：`test_blocks.cpp`「locate_template 把 datum 框拖到 target 那一侧：validate 在执行前就报错（M8a 验收 4）」：
  `datumRoi = [16, 162, 17.5, 166]` → 唯一一条 error，`code=bad_param phase=validate nodeId=n_locate paramPath=datumRoi`，
  「模板槽 1：datum 与 target 落在缝的同一侧（都在右边）—— 段差要两侧各取一个面」；直接跑时 `n_locate` 从未进
  `running`。同一用例还覆盖框退化、缝框颠倒或重叠、两个模板槽的基准件不同侧（报在 `template2DatumRoi`）。
- CLI：`bridge/src/cli.rs` 的 `import_defaults_to_blocks_and_fine_flag_gives_the_fine_graph`：`lyflow import` 默认出积木图、
  `--fine` 出细粒度图，两张都 `lyflow validate` 退出 0；改 `datumRoi` 之后 `lyflow validate` **退出 1**，诊断
  `bad_param / validate / n_locate / datumRoi`；`lyflow run` 同样退出 1，stdout 里没有 `run_started`，也没有任何 `node_state`。

### ✅ 5. 细粒度算子照旧、混用能跑

- `packs/gap/tests/test_gap_ops.cpp`、`test_model_roi.cpp` **一行未改**，全部通过（在 265/265 里）。
  `test_import_bundle.cpp` 的结构用例改成对 `:fine` kind 断言（它们测的就是细粒度图的接线），另有两处按计划改了：
  `gap.result_bundle` 的用例改喂 RoiSet（L5 定死了去掉七个散端口），「toward 接同侧 gap 框」改成「接 business_rois 的
  seam」（L7）；`modelPath` 的 binds 去掉了 `n_ref`（L12 不再生成黑盒对照）。
- 旁证：M7 验收时导入的细粒度图原样在 M8a 的 CLI 上跑出 M7 的读数（见第 1 条的定位表），细粒度算子的行为没变。
- 混用：`test_blocks.cpp`「同一张图里混用积木与细粒度算子能跑，结果与纯积木图相同（M8a 验收 5）」两个方向：
  ① 积木定位 → `gap.split_roi_set` / `gap.split_scan_pair` → 细粒度 `filter.crop_box2d` + `gap.fit_line`（toward 接
  `seam`）→ `gap.flush`；② 细粒度 `business_rois` → `gap.make_roi_set` → 积木 `gap.role_line` → `gap.flush`。
  两张都过 validate、跑通，flush / gap 与纯积木图逐位相同。导入器本身也会产出混用图：方向基准的锚配置写不成积木时
  （例：`anchor: gap_left, height_anchor: flush_ref`）退回细粒度的「窗 → 裁 → 拟」，经 `split_*` 从 Bundle 取框和云。

### ✅ 6. 门禁 —— 见「门禁」

## 抽成共用函数的清单（L6）

细粒度算子的 compute 主体从匿名命名空间挪进 `namespace lyflow::packs::gap::fine`，声明在 `packs/gap/ops/gap_fine.h`，
**函数体一字未改**；细粒度算子注册的 `op.compute` 就是这些函数，积木算子用 `Step` 原样调同一个函数
（`test_blocks.cpp` 第一条用例逐个断言 `registry.find(id)->compute == &fine::X`）：

| 包内函数 | 细粒度算子 | 被哪些积木算子调 |
|---|---|---|
| `fine::loadPair`（+ `profilePairKey`） | `gap.load_profile_pair` | read_scan |
| `fine::toMeasurementFrame` | `gap.to_measurement_frame` | read_scan、locate_model（换回传感器帧） |
| `fine::dropNonFinite` | `gap.drop_non_finite` | read_scan、locate_model |
| `fine::overallRoi` | `gap.overall_roi` | locate_template |
| `fine::loadTemplate` | `gap.load_template` | locate_template |
| `fine::alignTemplate` | `gap.align_template` | locate_template |
| `fine::selectAlignment` | `gap.select_alignment` | locate_template |
| `fine::businessRois` | `gap.business_rois` | locate_template |
| `fine::profileTensor` / `labelsFromLogits` / `roiFromLabels` | 同名三个 | locate_model |
| `fine::rollAnchoredCrop` | `gap.roll_anchored_crop` | locate_model |
| `fine::fitLine`（+ `validateFitLine`） | `gap.fit_line` | role_line、ref_point（line_end）、datum_direction |
| `fine::selectedPoint` / `nearestToLine` | 同名两个 | ref_point |
| `fine::fitGapCircles`（+ `validateFitGapCircles`） | `gap.fit_gap_circles` | seam_circles |
| `fine::datumWindow`（+ `validateDatumWindow`） | `gap.datum_window` | datum_direction |

跨包的四个取注册表里那个算子注册的函数指针（同一份实现，不复制）：`filter.crop_box2d`、`util.merge`、
`filter.radius_outlier`（std-pointcloud）、`ml.onnx_run`（std-ml，会话缓存因此也共用）。

新写的只有两类「不是算法」的代码：Bundle 的装拆（`bundle_common.cpp`：`scanPairOf` / `readScanPair` /
`roiSetOf` / `readRoiSet` / `roiInfo`）与两个判据（`datumOnRight`：datum 框中心在两个缝框中心连线中点的哪一侧；
`seamTowardBox`：那个中点的零尺寸框）。细粒度图拿到同一个判据的路径：`business_rois` / `roi_from_labels` 的新输出
`seam`、`business_rois.datumSide` 与 `make_roi_set.datumSide` 的 `auto`，都调这两个函数。参数声明也共用：
积木算子的参数用 `paramOf(r, "<细粒度算子>", "<名字>")` 从已注册的细粒度算子拷，名字相同 = 语义相同，
`Step::copyAll` 按名字原样传。

## 冻结接口（M8b 对着它做）

**1. Bundle 在 manifest 里的形状**（`schema/operator-manifest.schema.json` 的 `bundles` / `$defs.bundle`）：

```jsonc
"types":   [ ..., { "name": "Bundle", "color": "#c8a86b", "doc": "..." } ],   // 所有 Bundle 端口共用的颜色
"bundles": [ { "kind": "gap.RoiSet", "label": "角色框", "doc": "...", "pack": "gap@0.2.0",
               "fields": [ { "name": "datum", "type": "Box2D", "doc": "..." }, ... ] } ]
```

- 端口类型字符串 `Bundle<kind>`，正则 `^Bundle<[^<> ]+>$`；类型检查只认**字符串相等**（与 Any 照常兼容），
  没有 castableTo。编辑器的 `typesByName` 已经为每个 kind 登记了一个 `Bundle<kind>` 项（颜色取 `Bundle`），
  `typecheck.ts` 的精确匹配因此不用改。
- 字段有序；字段名不含 `.`；字段类型是 `types` 里的具体类型（非 Any / Error / Bundle）。
- 值的 `valueJson`：`{ "kind": "Bundle", "bundleKind": "<kind>", "fields": [ { "name", "type", "elementCount", "value"? } ] }`，
  点云 / Indices 字段没有 `value`（走二进制）。

**2. `<port>.<field>` 寻址**：按**最后一个点**拆（字段名不含点）。认的地方：`ResultStore::get / outputInfo`
（因此 `lyflow_output_cloud / tensor / indices / save` 不改签名）、`lyflow_output_info` 与事件 `stats.outputs`
（Bundle 端口那一项之后紧跟 `<port>.<field>` 各一项，类型是字段的类型）、缓存命中的 `skipped` 事件、summary 与
`lyflow_run_outputs`、GraphDoc 顶层 `outputs[].port`（校验期查端口是 Bundle 且字段已声明，否则 `unknown_port`）。
**边不按字段接**：`edges[].from.port` 仍只能是端口名；要把字段接给普通端口用 `gap.split_*`。C ABI 仍是 v10。

**3. gap 的两种 kind**：

| kind | 字段（有序） |
|---|---|
| `gap.ScanPair` | `primary`、`secondary`、`merged`（PointCloud，测量帧、米） |
| `gap.RoiSet` | `datum`、`target`、`seamLeft`、`seamRight`（Box2D，米）、`info`（Record，type `GapRoiInfo`） |

`GapRoiInfo.data` = `{ datumSide: "left"|"right", source: "template"|"model", alignment: <GapAlignment.data>|null,
overallMm: [x0,y0,x1,y1]|null, cropStatus: <GapRollCrop.data>|null }`。

**4. 积木算子的端口与参数名**（「人要填的」= 不带 advanced；其余 advanced）：

| 算子 | 输入 | 输出 | 人要填的参数 | 高级 |
|---|---|---|---|---|
| `gap.read_scan` | `primary?`、`secondary?`（PointCloud，source=inputs 时用） | `scan: Bundle<gap.ScanPair>` | `source`（dir \| files \| inputs）、`dir`、`primaryPrefix`、`secondaryPrefix`、`primaryFile`、`secondaryFile`、`removeOutliers`、`outlierRadiusMm`、`outlierNeighbors` | `layout` |
| `gap.locate_template` | `scan` | `rois: Bundle<gap.RoiSet>`、`alignment: Record`、`scan: Bundle<gap.ScanPair>` | `templateDir`、`datumRoi`、`targetRoi`、`seamLeftRoi`、`seamRightRoi`（Vec4f，模板坐标系 mm，`[xMin,yMin,xMax,yMax]`，group `ROI`）、`minScore` | `overallRoi`、`overallMode`、`overallCamera`；`template{1..4}Enabled / Id / Left / Right / Override / DatumRoi / TargetRoi / SeamLeftRoi / SeamRightRoi`；ICP：`initialPoseMode`、`maxMatchingDist`、`maxFitnessDist`、`maxIterations`、`normalKnn`、`bidirection`、`globalCoarse`、`successGuide`、`segRoi`、`trustTranslation`、`trustRotation`、`degenerateRatio` |
| `gap.locate_model` | `scan` | `rois`、`scan` | `modelPath` | `refine`、`splitStepMm`、`splitSlotGap`、`linkMm`、`minComponentSlots`、`anchorGapMm`；`cropEnabled`、`cropHalfWidth`、`cropHalfHeight`、`cropMaxRollBoxHeight`、`cropMinPointsKept`、`cropUsingCamera` |
| `gap.role_line` | `scan`、`rois`、`refLine?` | `line`、`innerEnd`、`quality` | `role`（datum \| target）、`distThresh`、`segmentPoints` | `cloud`（merged \| primary \| secondary）、`endpoints`、`lineType`、`dirMode`、`dirNominalDeg`、`dirTolDeg`、`minInliers` |
| `gap.ref_point` | `scan`、`rois`、`baseLine?` | `point`、`line`、`quality` | `method`（line_end \| selected_point \| nearest_point）、`role`（默认 target）、`distThresh`、`segmentPoints` | `cloud`、`endpoints`、`lineType`、`minInliers` |
| `gap.seam_circles` | `scan`、`rois`、`refLine?`、`refLineRight?` | `left`、`right`、`quality` | `distThresh`、`nominal`、`leftRadiusMin`、`leftRadiusMax`、`rightRadiusMin`、`rightRadiusMax` | `gap.fit_gap_circles` 其余 25 个，同名 |
| `gap.datum_direction` | `scan`、`rois` | `line`、`quality` | `startMm`、`lengthMm`、`heightMm`、`distThresh` | `role`、`minInliers` |

细粒度侧的四个装拆算子：`gap.make_scan_pair`（primary, secondary, merged → scan）、`gap.split_scan_pair`（反过来）、
`gap.make_roi_set`（datum, target, seamLeft, seamRight, alignment?, overall?, cropStatus? → rois；参数 `source`、
高级 `datumSide`=auto|left|right）、`gap.split_roi_set`（rois → datum, target, seamLeft, seamRight, seam, info）。
`gap.result_bundle` 的输入是 `gap, flush?, rois?, scan?, roiOverall?, fits?, fitBase?, fitRef?, cropStatus?, alignment?, fallback?`。

**5. 加载期诊断**（L16 实时校验直接显示）：都是 `code=bad_param phase=validate`，`paramPath` 指到具体参数 ——
`locate_template`：`datumRoi` 等四个 / `template{k}{Role}Roi`（退化、缝框颠倒或重叠、datum 与 target 同侧、槽间不一致）、
`template1Enabled`（一个槽都没开）；`role_line`：`dirMode`（非 free 没接 refLine）；`ref_point`：`method`
（nearest_point 没接 baseLine，code 是 `missing_input`）；`read_scan`：`source`；`seam_circles` / `datum_direction`
与对应细粒度算子同一组。

**6. 导入器**：kind `StandardGap.yml[:template|:model]` 出积木图，后面加 `:fine` 出细粒度图；CLI `lyflow import --fine`
等价于在 kind 后面加 `:fine`。积木图的节点 id：`n_scan`、`n_locate`（模板）/ `n_model`（模型）、`b_n_locate`（回退的模板备用）、
`n_datum`、`n_line`、`n_ref_point`、`n_flush`、`n_circles`、`n_gap`、`n_judge_0/1`、`n_bundle`、`n_fb_rois` / `n_fb_scan` /
`n_fb_line` / `n_fb_ref_point` / `n_fb_quality_base` / `n_fb_quality_ref` / `n_fb_ref_line`；顶层参数 `gapOffset`
（binds `n_gap.offset`、`n_circles.offset`）、`modelPath`（binds `n_model.modelPath`）。`meta.style` 是 `blocks` / `fine`，
导入时的取舍写在 `meta.importNotes`。

没冻结、留给 M8b 定的：ROI 参数的「roi 语义标记」（L15）—— M8a 的四个角色框只能按名字与 `componentLabels`
认出来，manifest 里没有加通用标记字段。

## 取舍（计划没覆盖、又影响方向的）

1. **`locate_template` 多一个 `scan` 输出。** §3 写的是 `scan → rois, alignment`，但定位这一步里做了「整体框裁」，
   下游量测（细粒度图里是整体框裁过、去噪过的合并云）必须用裁过的那一份才能逐帧相同。与 L9 给 `locate_model`
   的 `scan` 输出同一个道理，于是两个定位算子形状对称：都出 `rois` + `scan`。
2. **ScanPair 的语义随产出者分两种。** `read_scan` 出的 primary / secondary **保留原始 1280 槽**（NaN 槽不剔），
   因为模型定位靠槽号与标签对齐、而它的输入只有 `scan`（L9）；merged 只含有限点。模板定位的整体框中位数与
   `filter.crop_box2d(open)` 本来就跳过非有限点，所以同一份 ScanPair 两条路都能吃。定位之后的 ScanPair 是裁过、
   有限的。`locate_model` 要传感器帧，就把测量帧再换一次轴（交换 y、z 对有限点是精确的，NaN 槽原样）——
   39 个样本的模型路径逐位相同是这条的实证。
3. **去噪挪到合并云上、裁剪之前**（两种图一致）。§3 把「离群滤波开关与半径」放进 `read_scan`，而它在整体框之前；
   细粒度图于是也改成「合并 → 去噪 → 裁」，`gap.roll_anchored_crop` 为此加了可选的 `merged` 输入/输出（跟同一个窗裁）。
   这批数据上零格读数变化（见第 1 条）。
4. **基准件在哪一侧一律按框的几何推，`flush.base_side` 不再参与**（L7 的直接推论，两种图一致）。
   `business_rois.datumSide` 加了 `auto` 并设为默认，细粒度图不再写它。天幕 L5 / L6 的配置写 `left` 而 base 框在缝右侧，
   导入器在 `meta.importNotes` 里记一笔。**候选模板之间基准面不同侧就拒绝导入**（`bad_input`）—— 那是歧义，
   两种图没法给出同一个答案。
5. **「靠缝那一端」取两个缝框中心的中点**（L7），细粒度图为此由 `business_rois` / `roi_from_labels` 多出一个 `seam` 输出
   接给 `fit_line.toward`。这是 R4 五个 flush 变化的唯一来源，新读数才是真正靠缝的那一端（第 1 条）。
6. **四框全 0 的模板候选（没配框，天幕 L4 的 f1）两种图都跳过**，记进 `meta.importNotes`。不跳的话 L11 的校验会把
   整张积木图拦下来；而它被选中的那一帧本来就只会在裁剪处 `roi_empty`。
7. **方向基准的角色映射。** `datum_direction` 没有 side：窗朝角色框背离缝的那一侧推，x 锚同侧缝框、y 锚角色框。
   配置的 `anchor` / `height_anchor` / `side` 能对上 `role=datum` 或 `role=target` 就出积木，对不上就退回细粒度
   三节点经 `split_*` 混用（两种图结果仍相同）。模型路径的 datumSide 是运行期由模型框推的，细粒度图的
   `datum_window.side` 是导入期按配置框定的 —— 只在「模型框把基准面判到另一侧」的坏帧上两者可能不同，
   这批配置没有一份用 `base_direction`，没有数据可验。
8. **读剖面的注入入口**：`read_scan` 只有一个 Bundle 输出，宿主注入整个 compute 会被跳过、merged 算不出来，所以不能
   直接注入它。给了 `source=inputs` + 两个可选输入：宿主在前面接一个 `gap.load_profile_pair` 并注入那个节点；
   配了双相机闸时导入器就是这么接的（`n_load → n_camera_guard → n_scan`）。
9. **`ref_point.line` 永远写出**：执行器要求声明过的输出都写，而 selected_point / nearest_point 没有拟线 ——
   给一条过参考点的占位线，`quality.lineFitted=false`，doc 里写明要参考线就用 `role_line(role=target)`。
10. **Bundle 的字段校验放在执行器接管输出那一刻**（`checkOutputs`，与现有的输出类型检查同一处），不在
    `Outputs::set` 里：后者拿不到注册表。效果与 L2 一样 —— 算子一返回就判，下游一个字段都看不到。
    积木算子内部经 Step 调的细粒度算子不产出 Bundle，不需要这一层。
11. **ABI 号不变（仍是 v10）**：L3 要求取数函数不改签名，新增的只是 port 字符串的一种写法与 output_info 的多几项；
    在 c_api.h 注释里写明「加在 v10 里」，与 M6 在 v9 里加 `lyflow_effective_params` 的先例一致。
12. **manifest 的 `inf`/`nan` 用例改判据**：`test_executor.cpp` 原来按子串查 manifest 里没有 `inf`，而 L4 定死了 RoiSet 有个
    叫 `info` 的字段。改成只查值位置（`:`、`,`、`[` 之后）上的非有限数字字面量。
13. **导入器不再生成 `n_crop_gapLeft` / `n_crop_gapRight`**（细粒度图里两个没有下游的叶子节点，只为看框；
    圆拟合自己裁）。细粒度的回退图不再自己读一遍文件（`b_n_load` / `b_n_frame_*`），备用闭包与模型那一支共用
    剔过 NaN 的两片云与去噪之后的合并云 —— 与积木图的 `b_n_locate` 吃同一个 `n_scan` 同源；
    `n_fb_overall` 换成 `n_fb_roi_set` / `n_fb_scan_set`。
14. **版本号**：改了端口或默认值的细粒度算子只升次版本（business_rois / roi_from_labels 1.2.0、roll_anchored_crop /
    result_bundle 1.1.0），理由同 M7 取舍 13（升主版本要带完整迁移链，而老图不迁移）。

## 未验证

- **M8b（验收 7–10）**不在本轮。
- `pnpm check:gap` 的历史 A/B 对拍（`LYFLOW_GAP_AB=1`）没有跑：已不是门槛；`packs/gap/tools/` 的 Python 生成器与
  `lyflow_ab.py` 仍按 M7 之前的图形状工作，没有跟着改（J3 的「历史对拍工具」）。
- e2e 的两组真实 gap 图（`LYFLOW_GAP_GRAPH*`）照旧未设、整组不跑；文案已改成用 `--fine` 导入，黑盒对照那一条改成「两个读数都在」。
- `pnpm e2e:http` 没有跑（M8a 不涉及 HTTP 传输）。
- 取舍 7 的模型路径方向基准分支没有真实数据可验。

## commit

见 `git log`：core 的 Bundle（含 schema、编辑器与 MCP 的类型）一个；gap 积木算子、共用实现、导入器与 CLI `--fine` 一个；
本验收记录一个。`docs/m8-plan.md` 未改动。
