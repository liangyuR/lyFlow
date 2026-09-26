# KUN10 19 张线上图迁移 —— 验收记录

对象：`luoshi/database/KUN10/device_0/*/*.lyflow.json`（2026-09-15 手搭的 19 个测点，M5 的证据图），
在当前 core 下 `lyflow validate` 全部失败。本轮让它们重新校验通过、读数与 [m5-acceptance.md](m5-acceptance.md) 一致。
每一条都写**怎么跑 + 实际输出 + 通过/未通过**，命令都由我自己跑、输出落盘后再读。

分支 `fix/result-bundle-v2-migration`（worktree `D:\project\LyFlow-kun10`，基于 `0720efd`）。主工作区当时有另一个会话在改
`bridge/` 并开着 `tauri dev`（core-watch 会被这边的改动触发重编），所以改在独立 worktree 里做，一个提交，没有合回 main、没有 push。

## 结论

| # | 验收项 | 结果 |
|---|---|---|
| 1 | 19 张图 `lyflow validate` 全部通过 | ✅ 19/19，诊断 0 条 |
| 2 | 至少 3 帧 flush / gap 与记录一致 | ✅ 点 2 前 3 帧 gap、flush 与 P4 记录逐位相同；19 点 × 51 帧的 std / 均值 18 点与 M5 表四位小数全同 |
| 2' | 点 4_4 | ⚠️ 与 M5 不同（std 0.1384 vs 0.1051），**原因是 M7 改了 `gap.fit_line` 取棱边点的判据**，不是迁移；用旧判据在同一批内点上重算，逐项等于 M5（§4） |
| 3 | `LYFLOW_PACKS=gap;dts pnpm check` | ✅ 全链路绿（§6） |
| 4 | `LYFLOW_PACKS=gap;dts pnpm e2e`（落盘 grep 未验 / 跳过 / FAIL / ✗ / 中断） | ⚠️ 原样跑 1072/1088，16 条全是动效断言，原因是本机 Windows「动画效果」关着；把 `prefers-reduced-motion` 模拟成 `no-preference` 重跑 **1088/1088 全绿**（§6） |

## 1. 为什么失败：两处，第二处被第一处挡住了

1. **`gap.result_bundle` 的散端口**。m8a（`1f58af5`，m8-plan L5）把七个散端口 `roiFlushBase` / `roiFlushRef` / `roiGapLeft` /
   `roiGapRight`、`cloudPrimary` / `cloudSecondary` / `cloudMerged` 换成 `rois: Bundle<gap.RoiSet>` 与 `scan: Bundle<gap.ScanPair>`，
   版本却只从 1.0.0 升到 1.1.0，没有迁移。19 张图都连着三片云（点 4、4_4、7 另连着 `roiFlushBase` / `roiFlushRef`）→ `unknown_port`。
2. **`gap.fit_line` 的 `side`**。M7（`34412a8`，m7-plan J9c）删掉 `side` 参数、新增必接输入 `toward`，版本只升 minor，明确「不留旧参数」。
   点 4、4_4、7 用了 `fit_line` → `unknown_param side` + `missing_input toward`。第一处修好之后才露出来。

## 2. 改了什么

### core：迁移可以改连线（[ADR-0025](adr/0025-topology-migration.md)，修订 ADR-0008）

ADR-0008 的迁移只碰参数，它自己写了复议条件「一次迁移要动图的拓扑」—— 这次成立：`scan` 是 Bundle，三片云是 `PointCloud`，
等价接法要**多插一个 `gap.make_scan_pair` 节点**。

- `Migration` 加可选的 `topology`：拿该步之前的参数与本节点的全部入边，还 `TopologyEdit{dropInputs, addNodes, addEdges, notes}`。
  `apply` 与 `topology` 至少一个（`Registry::validate` 同步放宽）。
- `buildPlan` 一开头跑完所有节点的迁移链，拓扑改动落在图的副本上，插入的节点照常校验、进计划（`core/src/exec/plan.cpp`）。
- 迁移诊断多一个 `edits`（`removeEdges` / `addNodes` / `addEdges`，id 由 core 分配：`<节点>_<key>`，撞了加 `_2`）。
  写回两处同一语义：编辑器 `applyMigrations`（同一条撤销记录，`packages/editor/src/store/graph.ts`）、
  `lyflow migrate --write`（`GraphDoc::apply_migration`，`bridge/src/graph.rs`；stderr 逐条打出 notes）。
- **没打 `opVersion` 的节点按 v1 去试**，只有真改动了才出迁移诊断。这 19 张图的节点全都没有 `opVersion`
  （导入器从 `9bc7071` 才开始写），不这样做迁移永远触发不了。现有两条迁移（`filter.random_sample`、`gap.locate_template`）本就幂等。

### gap 包：`gap.result_bundle` 2.0.0

`Migration{1, nullptr, &migrateResultBundleFromV1}`（`packs/gap/ops/result_bundle.cpp`）：

| v1 的接法 | 迁移成 | 等价性 |
|---|---|---|
| 三片云齐全 | 插 `gap.make_scan_pair`（标题「组剖面对（迁移）」），三条边改接它，它接 `scan` | 点数各项与 v1 相同（v1 数的就是这三片云） |
| 四个框齐全 | 插 `gap.make_roi_set`（`source` = 接了 `cropStatus` 是 `model`，否则 `template`），接 `rois` | `effective_roi` 四框相同；`roi_source` 按 v1 的推断写死 |
| 凑不齐 | 删边，notes 写明从此不再记录哪几项 | 不等价，**不造数据补齐** |
| `scan` / `rois` 已经接了 | 删旧边，以新端口为准 | —— |

### 19 张图的数据（先备份）

备份：`luoshi/database/KUN10/backup-2026-09-25-before-result-bundle-v2/<点>/<点>.lyflow.json`，同目录 `SHA256SUMS`
（`sha256sum -c` 19/19 OK）。

1. `lyflow migrate <图> --write`，19 张各 1 个节点：
   - 16 张（1、1_1、2、2_2、3、3_3、5、6、Audio_*、HUD_*）：三片云收进新插的 `n_bundle_scan`（`gap.make_scan_pair`）接 `scan`。
   - 3 张（4、4_4、7）只有两片云（没有合并云）、两个框：四条边删掉。**汇总里从此没有** `input_*` / `preprocess_*` 点数、
     `input_point_count`，`effective_roi` 里没有 `flush_base` / `flush_ref`。这三张图测的是 gap（`point_offset` / `flush`），
     bundle 只是记录，读数不受影响。
2. `n_load.layout = "profile"`，19 张（理由见 §3）。
3. `gap.fit_line` 手工接 `toward`（点 4、4_4、7；2026-09-25 用户选定「只手工接这 3 张，不写 fit_line 迁移」）。
   旧 `side` 的实际含义是「按扫描顺序取靠缝那一头」，而这批扫描是 x 递增的：left = 取 +x 端，right = 取 −x 端。
   - 点 4、4_4：新加 `n_toward_l` / `n_toward_r` 两个 `gap.overall_roi`（fixed，只用中心），分别放在左件面框右边 40 mm、
     右件面框左边 40 mm 处，标题写明用途。**第一版接的是对面那块面的框，点 4 的均值变成 1.9065**：棱边线约 45° 倾斜，
     对面框的中心投影到直线上落在内点中间，`innerEnd` 取到了远离缝的那一头。改成远处锚框后与 M5 逐项相同。
   - 点 7：缓坡线 `toward` 接缝底邻域框 `n_box_pick_roi`（它本来就在缓坡末端的缝侧），与 M5 逐项相同。
   - 这几个 `fit_line` 节点与新节点打了 `opVersion`。

## 3. `n_load.layout`：图本身就该是 profile，算子默认值没变

- **默认值没变过**：`layout` 是 `e6bb715`（09-16，m5-plan G11）新加的参数，默认 `sensor` 就是加它之前唯一的行为。
- **盘上的剖面都是 profile 布局**：xyz-gap-inspector 2.1.0 起 `ReadProfilePcd` 只认 x=u、y=h、z=0，旧的传感器布局直接拒读
  （`src/infrastructure/point_cloud/profile_pcd_io.hpp`）；`cloud/KUN10` 51 帧就是这种。09-15 调参时另转成传感器布局的
  `scratchpad/sensor/` 目录还在，但**里面 0 个文件**（09-23 清掉了），P4 验收里「那一份已经不在了」属实。
- **线上不受影响**：宿主不让 `load_profile_pair` 读盘，而是先 `SensorCloudFromProfile` 转成传感器帧再注入它的两个输出端口
  （`process_controller.cpp:2882`、`recalculation_service.cpp:965`、`lyflow_measurer.cpp` 的 `ProfilePairInputs`），
  被注入的节点不调 compute，`layout` 不起作用。
- 所以这张图唯一会自己读盘的场景（`lyflow run` / `eval` / 编辑器离线调参）读到的一定是 profile 布局 —— 图里该写 `profile`。
  下面 §4 的 18 点逐项一致，就是「profile 读入 = 当时手工转的传感器布局」的证据。

## 4. 读数

### 4.1 19 点 × 51 帧，对 M5 表

**怎么跑**（每个点一次，样本是原始归档，不带任何布局覆盖）：

```
lyflow eval database/KUN10/device_0/<点>/<点>.lyflow.json --samples-dir cloud/KUN10 --sample-subdir device_0/<点>_0 \
  --bind-pair n_load.primaryFile,n_load.secondaryFile --pattern '*Master*.pcd,*Slave*.pcd' \
  --set 'n_load.source="files"' --metric outputs.gap [--metric outputs.flush] --split-half half --holdout half=b --no-cache
```

19 × 51 = 969 次运行全部 ok。std 是样本标准差（除 n−1，与 M5 的 `eval` 同口径），a / b 是时间前后两半：

| 点 | n | ok | std_all | M5 | std_a | M5 | std_b | M5 | mean_all | M5 | 一致 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 51 | 51 | 0.0627 | 0.0627 | 0.0578 | 0.0578 | 0.0682 | 0.0682 | -0.0017 | -0.0017 | ✓ |
| 1_1 | 51 | 51 | 0.0444 | 0.0444 | 0.0496 | 0.0496 | 0.0393 | 0.0393 | -0.0002 | -0.0002 | ✓ |
| 2 | 51 | 51 | 0.0702 | 0.0702 | 0.0696 | 0.0696 | 0.0720 | 0.0720 | 0.1520 | 0.1520 | ✓ |
| 2_2 | 51 | 51 | 0.0726 | 0.0726 | 0.0684 | 0.0684 | 0.0739 | 0.0739 | 0.1540 | 0.1540 | ✓ |
| 3 | 51 | 51 | 0.0308 | 0.0308 | 0.0355 | 0.0355 | 0.0259 | 0.0259 | 0.3004 | 0.3004 | ✓ |
| 3_3 | 51 | 51 | 0.0330 | 0.0330 | 0.0304 | 0.0304 | 0.0354 | 0.0354 | 0.3002 | 0.3002 | ✓ |
| 4 | 51 | 51 | 0.0669 | 0.0669 | 0.0751 | 0.0751 | 0.0532 | 0.0532 | 1.0669 | 1.0669 | ✓ |
| 4_4 | 51 | 51 | 0.1384 | 0.1051 | 0.1517 | 0.1044 | 0.1212 | 0.1048 | 0.9055 | 0.8956 | ✗（§4.3） |
| 5 | 51 | 51 | 0.0419 | 0.0419 | 0.0450 | 0.0450 | 0.0386 | 0.0386 | 0.1504 | 0.1504 | ✓ |
| 6 | 51 | 51 | 0.0505 | 0.0505 | 0.0455 | 0.0455 | 0.0558 | 0.0558 | 0.1503 | 0.1503 | ✓ |
| 7 | 51 | 51 | 0.0658 | 0.0658 | 0.0546 | 0.0546 | 0.0766 | 0.0766 | 0.1495 | 0.1495 | ✓ |
| Audio_1 | 51 | 51 | 0.0349 | 0.0349 | 0.0345 | 0.0345 | 0.0339 | 0.0339 | 0.1495 | 0.1495 | ✓ |
| Audio_2 | 51 | 51 | 0.0657 | 0.0657 | 0.0547 | 0.0547 | 0.0761 | 0.0761 | 0.1501 | 0.1501 | ✓ |
| Audio_5 | 51 | 51 | 0.0441 | 0.0441 | 0.0512 | 0.0512 | 0.0363 | 0.0363 | 0.1504 | 0.1504 | ✓ |
| Audio_6 | 51 | 51 | 0.0866 | 0.0866 | 0.0836 | 0.0836 | 0.0911 | 0.0911 | 0.1498 | 0.1498 | ✓ |
| HUD_1 | 51 | 51 | 0.0647 | 0.0647 | 0.0706 | 0.0706 | 0.0588 | 0.0588 | 2.9997 | 2.9997 | ✓ |
| HUD_2 | 51 | 51 | 0.1424 | 0.1424 | 0.1449 | 0.1449 | 0.1415 | 0.1415 | 3.0005 | 3.0005 | ✓ |
| HUD_5 | 51 | 51 | 0.2721 | 0.2721 | 0.2855 | 0.2855 | 0.2525 | 0.2525 | 3.0005 | 3.0005 | ✓ |
| HUD_6 | 51 | 51 | 0.1016 | 0.1016 | 0.0876 | 0.0876 | 0.1163 | 0.1163 | 3.0002 | 3.0002 | ✓ |

M5 表只记 gap。flush（6 个有面差的点）M5 没有记录，逐帧对照见 4.2。

### 4.2 逐帧：点 2 的 gap 与 flush，与 P4 记录逐位相同

[param-recipe-p4-acceptance.md](param-recipe-p4-acceptance.md) §26 用同一张图（当时在 e2e 副本里手工删边、改 layout）记了前 3 帧「基础」下的值。
这次用迁移后的线上图原样跑：

| 帧 | gap（本次） | gap（P4） | flush（本次） | flush（P4） |
|---|---|---|---|---|
| 14-09-2026-03-44-38 | 0.17514934028347556 | 0.17514934028347556 | -0.39045428135984867 | -0.39045428135984867 |
| 15-09-2026-07-52-22 | 0.1220759759448678 | 0.1220759759448678 | -0.4085120928229669 | -0.4085120928229669 |
| 15-09-2026-07-53-17 | 0.182924012999698 | 0.182924012999698 | -0.388735962025612 | -0.388735962025612 |

另外两组对照，都是 51 帧逐行 `metrics` 文本相同：备份里**没迁移**的点 2、HUD_5（执行器在内存里迁移）加 `--set n_load.layout="profile"`，
对迁移写回后的文件。即「内存里迁移」与「写回之后」是同一张图。

### 4.3 点 4_4：M7 改了 `fit_line` 的判据

两种 `toward` 接法（对面框、远处锚框）在 4_4 上结果**完全相同**（std 0.1384、均值 0.9055），所以不是接法问题。

M7 J9c 之前 `innerEnd` = 内点里**按扫描顺序**的最后一个（`getEndPointofCloud`，`ascend` 取裁剪云首尾 x 比较）；
之后 = 内点里**沿直线方向投影**离 `toward` 中心最近的那个。4_4 左侧棱边是 Slave 在 1 mm 高的框里只剩的 11 个点，散。

**怎么核实的**：拷一份 4_4 图，加 `segment.extract_indices`（`n_crop_*` 云 × `n_fit_*.inliers`）与 `io.save_pcd`（ascii），
51 帧逐帧存下裁剪云与内点，按旧判据重取两端的点、算 dx：

```
old(pre-M7 scan order)   std_all 0.1051 std_a 0.1044 std_b 0.1048 mean 0.8956
new(M7 projection)       std_all 0.1384 std_a 0.1517 std_b 0.1212 mean 0.9055
M5 表                    std_all 0.1051 std_a 0.1044 std_b 0.1048 mean 0.8956
frames 51  左侧内点 x 不单调的帧 51  old≠new 的帧 15
```

旧判据在同一批内点上**逐项等于 M5**；51 帧左侧内点的 x 都不单调，其中 15 帧两种判据取到不同的点（差得最多的两帧：07-54-12 0.9413 → 1.4851、08-26-58 0.7811 → 1.2987）。
M7 有意改成与点序无关，本轮不改回去，**4_4 在当前算子下需要重新调**（框或 `distThresh`），留给调参。

## 5. 测试

| 位置 | 新增 |
|---|---|
| `core/tests/test_cache.cpp` | 拓扑迁移：删旧端口的边、插节点（id 撞了换 `_2`）、接到新端口，插入的节点进计划；没打 opVersion 的 v1 写法迁移、v2 写法不算迁移；只有 `topology` 的一步过注册表自检，两样都没有不过 |
| `packs/gap/tests/test_blocks.cpp` | 从 `--fine` 导出的图反推 v1 接法（七根散线直接接 bundle）：迁移出 2 个插入节点、7 删 9 加，执行器当场跑、写回后再跑，bundle **逐字段**等于 m8a 的细粒度图；凑不齐（点 4 的接法、无 opVersion）只删边、notes 写明；v2 接法去掉所有 opVersion 不出迁移 |
| `bridge/src/graph.rs` | `apply_migration`：删边、插节点（摆在旁边、标题进 ui）、加边，子图路径不写回 |
| `packages/editor/test/migrations.test.mjs` | `applyMigrations` 带 edits：一次撤销全部回去、边 id 撞了加后缀；不带 edits 照旧 |
| `scripts/e2e/params_p4.mjs` | 不再替线上图删边、改 layout，改为断言线上图已是 `layout=profile`、`n_bundle` v2（这两条断言 2026-09-26 精简时删了，见 §6 末） |

## 6. 门禁

都在 worktree 里跑，`$env:LYFLOW_PACKS="gap;dts"`，输出 `*> log` 落盘后 grep。

| 命令 | 结果 |
|---|---|
| `pnpm check` | **exit 0，「全链路绿」**。core doctest 306/306（36917 断言，含本轮新增 5 条）；manifest 71 / 72（带测试算子）个算子符合 schema；`cargo test` 162 passed；前端单测 66/66（含 `migrations.test.mjs`）；MCP 47/47 |
| `pnpm e2e`（第 1 次，原样） | exit 1，**1072/1088**。16 条失败全在「动效 验收 2–7、9」「修订一 验收 19 的定位闪光」「单节点运行 验收 11 的进度环转动」，都是「动效开着时应当有动画」的断言。grep `未验` / `跳过` / `中断` / `FAIL`：**0 行** |
| 失败原因 | 本机 `SystemParametersInfo(SPI_GETCLIENTAREAANIMATION)` = **False**（设置 → 辅助功能 → 视觉效果 → 动画效果：关），WebView2 因此报 `prefers-reduced-motion: reduce`，编辑器按设计关掉全部动效（`LyFlowEditor.tsx` 的 `motionOn = animations !== false && !reducedMotion`）。系统设置没有动 |
| `pnpm e2e`（第 2 次） | 只在本地临时加三行：连上 CDP 后 `Emulation.setEmulatedMedia` 设 `prefers-reduced-motion: no-preference`，`motion.mjs` / `noderun.mjs` 里两处「撤掉模拟」改为回到 `no-preference`。**exit 0，1088/1088 全绿**；grep `✗` / `未验` / `跳过` / `中断` / `FAIL`：0 行。三行已撤回，不在提交里 |

e2e 另有两处本地环境处理，同样不在提交里：主工作区的另一个会话占着 vite 5173，这边临时改成 5174（`app/vite.config.ts`、
`bridge/tauri.conf.json`、`harness.mjs` 的 URL 匹配），CDP 用 `LYFLOW_CDP_PORT=9223`，WebView2 数据目录用
`WEBVIEW2_USER_DATA_FOLDER` 指到临时目录（否则会并进那边已开着的 WebView2 进程，调试端口不生效）。

P4 验收 26 这组（真实 gap 图 2、KUN10 3 帧）两次都跑了，新加的两条断言「线上图的 n_load.layout 是 profile」「线上图的
result_bundle 已是 v2」通过，3 帧编辑器与 CLI 逐位相同。这两条 2026-09-26 的 e2e 精简里删了：它们只查线上图文件本身的两个字段，
迁移结果以本文的 `lyflow validate`（结论表第 1 条）与 §3–§4 为准；图要是退回旧接法，验收 26 后面的运行与逐位比较照样会失败。

（2026-09-26 起 e2e 断言做过合并精简，这里的日志与条数是当时的快照；见 test/prune 精简提交）

## 没修的

- **其他老图**：`OneDrive/Desktop/DTS` 下另有 65 张图还在用 `gap.fit_line` 的 `side`，当前 core 里同样校验不过 ——
  fengdang 54 张（`database*`、`review_20260921_anomaly/*` 等 9 个目录 × 点 1..6）、tianmu 8 张（`database_v5_2026-09-16_r10`）、
  luoshi `legacy_graphs_from_zip_2026-09-15` 3 张。M7 没留迁移，它们的正确 `toward`
  要看各自的框（导入器图接同侧 gap 框，手搭图各不相同），本轮按用户决定只手工改了 KUN10 的 3 张。
- **宿主** `xyz-gap-inspector` 的 `RoiDecisionNodes` 仍按 `roiFlushBase` 等 v1 端口名找 ROI 决策节点
  （`lyflow_measurer.cpp:492`）。KUN10 这批图不走 fallback，不受影响；积木图 / m8a 之后的细粒度图上它本来就找不到。另一个仓库，没动。
- `packs/gap/tools/lyflow_graph_from_config.py` 还在产 v1 接法（`cloudPrimary` 等），现在会被自动迁移，但工具本身没改。
- 点 4、4_4、7 的 bundle 丢了点数与两个框的记录（§2），要恢复得在图里补一个合并云（`util.merge`）与缝框，那是改图的量测结构，没做。
