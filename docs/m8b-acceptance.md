# M8b 验收记录 —— 自动连线、片段库、2D 拖框、实时校验、Bundle 的 Edge Peek、read_scan 直接注入

逐条对着 [m8-plan.md](m8-plan.md) §4 的 M8b 验收 7–10 走，外加补充验收 11（L18：宿主把两片云直接注入
`gap.read_scan`）：**怎么跑 + 实际输出 + 通过/未通过/未验证**。范围是 L13–L18，对着
[m8a-acceptance.md](m8a-acceptance.md)「冻结接口」做。基于 main `fdd5603`。
所有命令都在 `LYFLOW_PACKS=gap;dts` 下跑，长输出落盘到 `%TEMP%` 再 grep。

## 结论

| # | 验收项 | 状态 |
|---|---|---|
| 7 | 空白画布只靠拖入节点、插「测点骨架」、2D 拖四个框、改基础组参数建出模板路径测点，跑出 flush 与 gap，全程不手连一条边 | ✅ 通过 |
| 8 | 唯一候选自动连上；两个候选时不连、候选端口高亮 | ✅ 通过 |
| 9 | Edge Peek 在 ScanPair 边上列出三个字段，点进 merged 显示点云 | ✅ 通过 |
| 10 | 编辑时把 datum 框拖到错误一侧，节点立即标红并显示诊断 | ✅ 通过 |
| 11 | 宿主经 C ABI 的 run inputs / CLI 的 `--input` 把两片云直接喂给 `read_scan.primary/secondary`，flush / gap 与从目录读取相同 | ✅ 通过（C ABI：夹具逐位相同；CLI：39 个样本 × 模板路径 / 模型 + 回退各 39/39 逐位相同） |

## 门禁

| 命令 | 结果 |
|---|---|
| `$env:LYFLOW_PACKS="gap;dts"; pnpm check > $env:TEMP\m8b-check2.log 2>&1` | 退出码 0，末行「全链路绿」。core doctest **271/271**（M8a 265；+3 输入注入、+3 片段 / roi 标记 / read_scan 注入），新增一步「片段文件 vs schema」6 份全过，`cargo test` **136/136**（M8a 131；+1 `input_injects_a_cloud_into_an_input_port`、+1 `pcd_reader_agrees_with_io_load_pcd_on_all_three_formats`、+3 `pcd::tests`），lyflow-client 单独构建、CLI `--no-default-features`、嵌入 SDK、编辑器单测 **9/9**（+5 自动连线 / 片段插入）、前端构建、MCP 45/45 |
| `$env:LYFLOW_PACKS="gap;dts"; pnpm e2e > $env:TEMP\m8b-e2e.log 2>&1` | 退出码 0，**480/480 项通过**（M8a 424；+56 是 `scripts/e2e/m8b.mjs` 的四组，老分组一条未改、全过） |
| `pnpm e2e:http`（顺手跑的，不是门槛） | 29/29：HTTP 宿主上实时校验每次改图都会走一次 `validate`，没有把老断言带坏 |
| `pnpm core:dump`（带 `LYFLOW_PACKS`） | 重新生成 `app/public/manifest.dev.json`：71 个算子、14 个端口类型、`snippets` 段 6 项（文件被 `app/.gitignore` 忽略） |

（2026-09-26 起 e2e 断言做过合并精简，这里的输出与条数是当时的快照；见 test/prune 精简提交）

grep 落盘日志：

```
Select-String m8b-e2e.log -Pattern "跳过|未验|✗|FAIL"
  55:   ✓ 下游的原因是 upstream_failed        （老断言的标题，PowerShell 不分大小写才命中；不是失败）
Select-String m8b-e2e.log -Pattern "^── M8b"   → 四组都在跑：验收 7 / 9 / 10 / 8，没有一组走「未验」分支
Select-String m8b-check2.log -Pattern "test cases|ℹ fail"
  [doctest] test cases:   271 |   271 passed | 0 failed | 0 skipped
  ℹ fail 0   （编辑器单测）
  ℹ fail 0   （MCP）
```

## 逐条

### ✅ 7. 从空白画布建出模板路径测点，全程不手连一条边

`scripts/e2e/m8b.mjs` 的 `suiteBuildFromBlank`。数据是在临时工作区里现写的合成剖面（与
`packs/gap/tests/test_blocks.cpp` 的夹具同形：左板 y=165、右板 y=164、缝两侧各一段 R1 圆角；Master 每 97 个点插一个
NaN 槽），模板文件名就是 `locate_template` 槽 1 的默认值，人只填目录。动作序列（每一步都是界面动作）：

1. 从面板把 `gap.read_scan` 拖到画布 → 没有可连的；
2. 拖入 `gap.locate_template` → `scan` 唯一候选，**自动连上**（1 条边）；
3. 把片段「测点骨架」拖到画布 → 8 个节点 + 11 条内部边，8 个对外输入**自动接到 locate_template 那唯一的一对输出上**；
4. 改基础组参数：`read_scan.dir`、`locate_template.templateDir`、两个判定的 `nominal`；
5. 这时 `locate_template` 已经**标红**（四个框没填，实时校验）；选中它、视图切「2D 剖面」→ 画出槽 1 的模板云与四个
   占位框（四种颜色、标角色名、「未设置」）；用**真实鼠标**逐个拖框身、拖右下角、拖左上角；
6. 红框消失 → F5。

关键输出（`m8b-e2e.log`）：

```
✓ locate_template.scan 自动接到 read_scan.scan
✓ 一共 10 个节点（计划 §3 的模板路径测点）
✓ 一共 20 条边：1 条拖入时自动连的 + 11 条片段内部的 + 8 条插入时自动连的
✓ scan 接 scan、rois 接 rois（没有接到 read_scan 的原始云上）
✓ 没有留下歧义（没有端口要人挑）
✓ 四个框都没填时 locate_template 立即标红
✓ 2D 视图画出了槽 1 的模板云和四个框
✓ seamLeftRoi 拖到了 [-4.5,164,-1.5,167]（误差 < 0.35 mm）      （四个都过；拖动吸附 0.1 mm）
✓ 框拖好之后校验干净、红框消失
✓ 整张图跑通
✓ 跑出了 flush 数值（合成剖面上是 1 mm）
✓ 跑出了 gap 数值（合成剖面上约 √37−2 ≈ 4.08 mm）
✓ 全程没有手连一条边（connect / 改接 / 插到线上一次都没调）
✓ 跑完之后样本云上叠着变换后的四个框（只读）
```

「不手连」的证据不是看边数：这一组开始时把 graph store 的 `connect` / `reconnectEdge` / `insertOnEdge` 三个动作
包一层计数（画布上手动连线、改接、拖到线上插入都只能走它们），结束时计数为 `[]`。自动连线与片段插入走的是
新的 `addNodeAuto` / `insertSnippet`，各记一条撤销。

最终画布截图：[m8b-canvas.png](m8b-canvas.png)（`LYFLOW_E2E_SCREENSHOT=docs/m8b-canvas.png` 时这一组跑完用 CDP 的
`Page.captureScreenshot` 截整个窗口，截之前点一下「整理」布局与适配视图；不设这个变量就不写文件）。右上是 2D 剖面：
灰色是槽 1 的模板云与四个可拖框，蓝色是样本云与 locate_template 输出的四个变换后的框（只读），Inspector 里列着
`rois.datum` 等字段的值。

### ✅ 8. 自动连线：唯一候选连上；两个候选不连、候选高亮

`suiteAutoConnect`：拖入 read_scan#1、locate_template#1（自动连上），再拖入 read_scan#2、locate_template#2：

```
✓ 唯一候选：locate_template.scan 自动接到 read_scan.scan
✓ 两个候选（read_scan#2.scan、locate_template#1.scan）：不连
✓ 没连上的输入高亮成待选                 （port 上 data-auto-hint="target"）
✓ 候选 read_scan#2.scan 高亮             （data-auto-hint="candidate"）
✓ 候选 locate_template#1.scan 高亮
✓ 已被 locate_template#1 取代的 read_scan#1.scan 不算候选   （见取舍 1）
✓ 类型不兼容的输出不亮
✓ 从 ScanPair 输出拖线：role_line.scan 可落
✓ RoiSet 输入置灰
✓ Line2D 输入置灰
✓ 另一个 locate_template.scan 可落
```

后四条是 L13 的「从输出拖线时只高亮兼容的输入」：真实鼠标从 read_scan#2 的输出端口按下、拖到半路读端口上的
`data-port-verdict`（已有的 P1 #20 机制，Bundle 按 kind 精确匹配）。纯逻辑另有编辑器单测
`packages/editor/test/autoconnect.test.mjs`（唯一候选 + 一条撤销、两个候选、遮蔽规则、Any 不算候选、片段插入）。

### ✅ 9. Edge Peek 在 ScanPair 边上列出三个字段，点进 merged 显示点云

`suiteBundlePeek`（用验收 7 跑完的那张图）：

```
✓ 端口类型是 Bundle<gap.ScanPair>
✓ 默认视图是字段表
✓ 列出三个字段                  ["primary","secondary","merged"]
✓ 三个字段都是点云、都有点
✓ 点进 merged 显示点云          view=cloud3d type=PointCloud
✓ 窗里的总点数 = merged 字段的点数
✓ 「‹ 字段」回到字段表
```

实现：`PeekWindow` 多一个 `field`；Bundle 端口的默认视图是新的 `fields`（字段名、类型、点数或值摘要，取自
`stats.outputs` 里 `<port>.<field>` 那一项）；点进字段后 `resolved.port` 变成 `<port>.<field>`，点云 / 2D / 张量 /
下标 / 文本各视图原样复用（取数函数本来就认这种写法，M8a L3）。

### ✅ 10. 编辑时把 datum 框拖到错误一侧，立即标红并显示诊断

`suiteDragWrongSide`：2D 视图里把 datum 框拖到缝的右边（target 旁边）：

```
✓ 松手后 locate_template 标红
✓ 节点上显示诊断：datum 与 target 落在缝的同一侧
✓ 诊断指到 datumRoi
✓ 从松手到标红在 3 秒内（619 ms，含拖动本身）      （门禁那一次 e2e 的数；debounce 200 ms + 一次 validate IPC；
                                                     2026-09-26 起改成从 mouseReleased 起算、门限就是 3 s，
                                                     原先含拖动本身、放宽到 5 s）
✓ Inspector 里 datumRoi 那一行标红
✓ 错误消息贴在控件下面
✓ Inspector 顶部列出校验诊断
✓ 撤销一次就回到干净（整段拖动只记了一条撤销）
```

诊断全部来自 core 的 `lyflow_validate`（M7 的 `OperatorDesc::validate`，M8a 冻结接口第 5 条），前端不重做任何一条校验。

### ✅ 11. 宿主把两片云直接注入 `read_scan` 的 primary / secondary（L18）

**C ABI**：`packs/gap/tests/test_blocks.cpp`「宿主把两片云直接注入 read_scan 的 primary / secondary：与从目录读取逐位相同」。
在夹具上导入积木图、先从目录跑一遍；再用 `gap.load_profile_pair`（`dropNonFinite=false`）读出同一对传感器帧的云，
经 `lyflow_run_start` 的 `lyflow_run_options.inputs`（`node_id="n_scan"`、`port="primary"/"secondary"`，xyz + rgb）跑：

- 图原样（`source=dir`、目录也在）：注入的两片云压过目录，`CHECK(injected.flush == fromDir.flush)`、gap 同（double 的 `==`）；
- `source=inputs`、删掉 `dir`：`lyflow validate` 干净，跑出来同样逐位相同；不注入就跑，`n_scan` 报 `missing_input`；
- 两种情况下 `n_scan` 的 `node_state: done` 都**没有** `provided`（是输入注入、compute 真跑了），前面没有别的读盘节点。

core 层另有三条（`core/tests/test_flow.cpp`）：port 只是输入端口时 compute 照常跑、端口值取注入数据、必填输入不报
`missing_input`；端口已有连线 / 名字不存在 / 与输出注入混用在校验期分别报 `bad_input` / `unknown_port` / `bad_input`；
输入注入的数据也进 cacheKey。

**CLI**：`lyflow run <graph> --input n_scan.primary=<a.pcd> --input n_scan.secondary=<b.pcd>`。`bridge/src/cli.rs` 的
`input_injects_a_cloud_into_an_input_port`（`util.merge` 的两个必填输入由 `--input` 喂：校验不报缺线、compute 真跑、
`--outputs` 点数 5；端口名写错退出码 2 且报 `unknown_port`；写法不对、文件不存在退出码 4）。
39 个样本（`tianmu_0904`，同 M8a 的对照方法）：脚本 `%TEMP%\claude\…\scratchpad\m8b_inject.py`（不进仓库）对每个样本导入积木图，
A = `read_scan` 改 `source=files` 指向那一帧的两片 PCD；B = 同一张图改 `source=inputs`、删掉目录与文件参数，
CLI 用 `--input` 注入同两个文件。比 `--outputs` 里的 flush / gap（最短往返表示，文本相同 ⇔ double 相同）：

```
python m8b_inject.py --lyflow bridge\target\debug\lyflow.exe --out %TEMP%\m8b-inject --mode template
  template: 39/39 逐位相同
python m8b_inject.py --lyflow bridge\target\debug\lyflow.exe --out %TEMP%\m8b-inject --mode model
  model: 39/39 逐位相同            （导入时旁边放 setting.yml → 模型定位 + 模板回退；--param modelPath=<v12s0.onnx>）
```

全部 78 次注入运行里 `n_scan` 都是 `done` 且没有 `provided`；这批配置都没配双相机闸（`guard=0`），注入全部落在 `n_scan` 本身。

| 样本 | 模板 gap（文件 = 注入） | 模板 flush | 同 | 模型+回退 gap | 模型+回退 flush | 同 |
|---|---|---|---|---|---|---|
| R2_1 | —（icp_score_low） | — | ✓ | 27.700485 | -5.932908 | ✓ |
| R4_2 | 5.699679 | 0.564443 | ✓ | 5.776925 | 0.676074 | ✓ |
| R4_3 | 6.088255 | 0.968213 | ✓ | 6.055248 | 0.141562 | ✓ |
| R5_4 | 6.143392 | -6.718779 | ✓ | 6.093440 | -7.278044 | ✓ |
| R2_5 | 9.249416 | -6.013714 | ✓ | 9.484759 | -6.532842 | ✓ |
| R3_6 | 9.616839 | -5.203312 | ✓ | 9.658230 | -6.529852 | ✓ |
| L2_7 | —（roi_empty） | — | ✓ | 5.462724 | 4.058199 | ✓ |
| L3_8 | 7.842608 | 3.470490 | ✓ | 5.092493 | 3.877840 | ✓ |
| L3_9 | 7.812856 | 5.521365 | ✓ | 7.564572 | 5.917732 | ✓ |
| R1_10 | —（circle_fit_failed） | -2.373703 | ✓ | 3.783406 | -2.378815 | ✓ |
| R5_11 | 6.499484 | -6.475078 | ✓ | 6.470677 | -6.117328 | ✓ |
| L3_12 | —（icp_score_low） | — | ✓ | 7.911205 | 6.587292 | ✓ |
| R3_13 | 7.906184 | -6.421351 | ✓ | 5.720321 | -6.789222 | ✓ |
| R1_14 | 3.755395 | -2.762496 | ✓ | 3.032188 | -2.576966 | ✓ |
| R4_15 | 5.344849 | 0.713304 | ✓ | 5.349201 | 1.094732 | ✓ |
| R4_16 | 3.616913 | 0.500760 | ✓ | 5.327485 | 1.363438 | ✓ |
| R4_17 | 5.541333 | 0.700770 | ✓ | 5.502379 | 0.154169 | ✓ |
| L1_18 | 4.380924 | 0.538841 | ✓ | 4.527664 | 0.755485 | ✓ |
| R1_19 | 1.049134 | -2.398955 | ✓ | 3.645082 | -2.349557 | ✓ |
| L3_20 | —（icp_score_low） | — | ✓ | 7.275567 | 6.211828 | ✓ |
| L6_21 | —（roi_empty） | — | ✓ | 8.706555 | 3.543462 | ✓ |
| R2_22 | 9.430362 | -4.968199 | ✓ | 9.381395 | -4.122803 | ✓ |
| L6_23 | —（roi_empty） | — | ✓ | 7.582750 | 6.364131 | ✓ |
| R2_24 | 4.870131 | -5.076068 | ✓ | 2.451944 | -5.126814 | ✓ |
| R1_25 | 3.054148 | -2.422547 | ✓ | 3.059347 | -2.264467 | ✓ |
| R4_26 | 5.400936 | 0.946991 | ✓ | 5.363951 | 1.228767 | ✓ |
| R1_27 | 3.510453 | -2.465052 | ✓ | 3.714707 | -2.323437 | ✓ |
| L3_28 | —（icp_score_low） | — | ✓ | 7.607230 | 6.144964 | ✓ |
| L4_29 | 5.318827 | 0.073212 | ✓ | 5.325021 | -0.568530 | ✓ |
| R6_30 | 6.117538 | -5.894053 | ✓ | 6.195873 | -6.147455 | ✓ |
| L3_31 | 7.555259 | 5.264531 | ✓ | 7.448915 | 5.365737 | ✓ |
| R2_32 | —（icp_score_low） | — | ✓ | 9.664622 | -4.044776 | ✓ |
| R4_33 | 5.483583 | 1.239884 | ✓ | 5.505868 | 0.142660 | ✓ |
| L2_34 | —（icp_score_low） | — | ✓ | 5.544205 | 4.420482 | ✓ |
| R4_35 | 5.304131 | 1.199189 | ✓ | 5.418651 | 0.114737 | ✓ |
| R1_36 | 3.312383 | -2.405742 | ✓ | 3.312989 | -2.216987 | ✓ |
| L5_37 | 8.236421 | -4.589374 | ✓ | 8.248722 | 3.623311 | ✓ |
| R4_38 | 5.217093 | 0.868035 | ✓ | 5.332923 | -0.043381 | ✓ |
| R1_39 | —（circle_fit_failed） | -2.131313 | ✓ | 3.591483 | -2.451444 | ✓ |

读数与 M8a 验收第 1 条的表逐位一致（M8a 以来算法没动）。「—」是两种读法在同一处失败，码相同。

**一个过程中的发现**（取舍 12 的由来）：`--input` 最初借 core 的 `io.load_pcd` 读文件、再用 `lyflow_output_cloud(…, 0)`
全量取回。模板路径 39/39 逐位相同，**模型路径 0/39**：取数视图 `lyflow_cloud_view` 只有 xyz / intensity / normals，
没有 rgb，而模型定位把 rgb 的 R 当强度特征（`SensorXzProfile`）。改成 CLI 自己读 PCD（`bridge/src/pcd.rs`）带上 rgb 之后
39/39。宿主经 C ABI 注入时同理要把 rgb 一起给（`docs/embedding.md` 已写明）。

## 片段文件的格式与位置

- **格式**：`*.lyflow-snippet.json`，schema 在 [schema/snippet.schema.json](../schema/snippet.schema.json)：
  `{ schemaVersion: 1, id, label, category?, doc?, nodes: [{ id, op, params?, ui? }], edges: [{ from, to }],
  ports?: { inputs: [{ node, port, hint? }], outputs: [...] } }`。节点与边的写法与 GraphDoc 相同，id 只在片段内唯一
  （插入时重新分配）。`ports.inputs` 是插入时要自动连线的输入（可以含可选输入）；没给就接片段里所有没连的必需输入。
- **包随附的**：放在包目录的 `snippets/` 下（gap 的六份在 [packs/gap/snippets/](../packs/gap/snippets/)）。构建时由包的
  `lyflow_op_pack.cmake` 按字节编进包（`packs/gap/ops/snippets.cpp` 注册），经 manifest 顶层的 `snippets` 段给出 ——
  片段跟着包走：包没编进来，它的片段不出现。启动自检（`lyflow manifest --check`）查算子注册、参数名、边的端口与类型、
  对外输入没在片段里接边；`pnpm check` 另把每个文件对着 schema 校验。
- **用户的**：桌面宿主扫 app data 下的 `snippets/`（`%APPDATA%\com.lyflow.app\snippets`）与 `LYFLOW_SNIPPET_DIRS`
  （分号分隔），经新的 Tauri 命令 `list_snippets` 给编辑器；同 id 时用户的覆盖包里的。
- 文档：[docs/op-packs.md](op-packs.md)「随包的片段」、[docs/operator-manifest.md](operator-manifest.md)「片段」、
  [packs/gap/README.md](../packs/gap/README.md)「在编辑器里拼一个测点」。

gap 随附的六份：「测点骨架」`gap.measure_skeleton`、「段差 · 线端点」`gap.flush_line_end`、「段差 · 选点」
`gap.flush_selected_point`、「间隙 · 圆」`gap.gap_circles`、「模型定位 + 模板回退」`gap.locate_model_template_fallback`、
「备用相机回退」`gap.backup_camera`（内容见 gap README 的表）。

## roi 语义标记的定法（L15）

Param 上加两个可选字段，**只有编辑器读**，执行器与校验一概不看：

```jsonc
{ "name": "datumRoi", "type": "vec4f", "unit": "mm",
  "semantic": "roi",
  "roiBackdrop": { "dir": "templateDir", "files": ["template1Left", "template1Right"] } }
```

- `semantic`：目前只有 `"roi"` = `[xMin, yMin, xMax, yMax]`，XY 平面上的一个框；单位按 `unit`（`mm` 换成米乘 0.001）。
  C++ `Param::semantic`、`kParamSemantics`；只能标在 vec4f 上（`Registry::validate`）。
- `roiBackdrop`（可选）：框在 `<dir 参数>/<files 参数>` 那几个文件的坐标系里，编辑器把那几个文件读出来当底图；
  不给就是数据坐标系，画在节点显示的那片云上。`dir` / `files` 必须是本算子的 path / string 参数（自检查）。
- 标了的：`gap.locate_template` 的四个角色框（底图 = 槽 1 的左右模板）与四个槽的覆盖框（底图 = 各自槽的模板）、
  `overallRoi`（数据坐标系）；`gap.overall_roi.roi`（数据坐标系）。`gap.align_template` 的四个 roi 没标（它的模板是
  输入端口不是文件参数，底图说不清，见「未验证」）。
- manifest schema、C++ `manifest.h`、TS `types/manifest.ts` 一并更新；子图提升参数时 `semantic` 跟着走、`roiBackdrop` 不带。

## 取舍（计划没覆盖、又影响方向的）

1. **自动连线的候选不计「被取代」的输出。** L13 说「恰好有一个类型兼容的输出」。照字面，模板路径上一个都连不上：
   `read_scan.scan` 与 `locate_template.scan` 都是 `Bundle<gap.ScanPair>`，定位之后每一步都有两个候选。
   定义：一个输出已经连到某个节点上、而那个节点自己又产出同一种类型 —— 它被那个节点的输出取代，不算候选
   （`packages/editor/src/lib/autoconnect.ts` 的 `shadowedOutputs`）。这不引入任何隐式上下文：连出来的仍是普通的边，
   两个真正并列的候选照样不连、照样高亮（验收 8 里 read_scan#2 与 locate_template#1 就是这样）。
2. **类型还推不出来的 Any 输出不当候选、Any 输入不自动接。** 一个没接的 reroute「和谁都兼容」，算进去等于让所有
   端口都有歧义；`flow.fallback` 的 a / b 接谁是结构决定，猜就是瞎猜。
3. **只有界面上的三种加节点方式自动连线**（拖到画布、面板双击、画布双击唤起的搜索面板）：新的 store 动作
   `addNodeAuto`，整个算一条撤销。`addNode` 保持「只加节点」—— 脚本、验收、宿主用它精确搭图，老 e2e 一条没改。
   从端口拖线中途松手弹出的搜索面板保留原有语义（接上拖出的那个端口）。只接**必需**输入；片段按 `ports.inputs` 接
   （可以含可选输入，例如结果汇总的 `rois` / `scan`）。
4. **歧义的高亮**放在 ui store 的 `autoHint`（不进 doc、不进撤销）：没连上的输入 `data-auto-hint="target"`、候选输出
   `"candidate"`；手动连上一条、点空白处、下一次自动连线时清掉。
5. **「测点骨架」是定位之后的八个节点**（两条量测链、两个判定、结果汇总），不含 read_scan 与定位：这样同一个骨架
   接在模板定位、模型定位、「模型定位 + 模板回退」任何一个后面都对（遮蔽规则保证候选唯一）。验收 7 的 10 个节点 =
   拖入的 2 个 + 骨架的 8 个，正好是计划 §3 列的模板路径测点。
6. **包随附的片段编进包、经 manifest 下发**，而不是运行时扫包目录：打包后的 app 里没有包目录；片段跟着包的开关走；
   自检能在启动时拦住坏片段；MCP / HTTP 宿主读同一份 manifest。用户片段才扫目录（见上）。
7. **2D 拖框只画「与第一个可见 roi 参数同一底图」的那一组**：不同坐标系的框（模板坐标系的角色框 vs 数据坐标系的整体框）
   画在同一片云上是错的。没填过的框（退化）画成虚线占位，在底图包围盒里一字排开，第一次拖动就落成真值；拖动吸附
   0.1 mm；最近动过的框压在最上面（框挨着框时它的把手不会被邻居盖住）。框是叠在 WebGL 画布上的 DOM
   （`components/RoiLayer.tsx`），每帧按正交相机重新摆位。「样本云上叠画变换后的框（只读）」复用已有的几何叠画：
   跑过之后 locate_template 的 `rois.*` 字段就是 Box2D 输出，画在它的 `scan.merged` 上。
8. **底图不属于任何一次运行**：新的 Tauri 命令 `load_cloud_file` 用 core 的 `io.load_pcd` 读文件（相对路径按图文件目录）。
   框没填好时 locate_template 过不了校验、根本不会跑，底图却必须先看得见。Transport 上它与 `listSnippets` 是**可选**方法：
   HTTP / 静态传输没实现，宿主不给时只画框不画底图、片段只有包里的。
9. **`locate_template.overallRoi` 的默认值改成一个大到不裁的框** `[-1000, -1000, 1000, 1000]`（细粒度 `gap.overall_roi` 的
   默认 `[-1, -1, 1, 1]` 会把整片剖面裁没）：空白画布上拼出来的图不填高级参数也得能跑。导入器总是显式写这一项，
   导入的图、M8a 的 39 样本读数都不受影响（上面第 11 条的表逐位等于 M8a）。
10. **`gap.read_scan` 1.1.0**（L18）：两个输入接上或被注入就用它们，`source` 只决定「没给输入时去哪读」；validate 只查
    「成对给」。`source=inputs` 而两个都没给不在 validate 里报（`lyflow validate` 看不见注入），执行期报 `missing_input`。
    `layout` 不作用于输入（输入约定就是传感器帧）。版本号按 M8a 取舍 14 只升次版本。
11. **输入注入按端口名区分，ABI 号不变（仍是 v10）**：`lyflow_run_input.port` 是这个算子的输出端口 → 老语义（整节点注入）；
    只是输入端口 → 输入注入。同名输入输出按输出算（老宿主不变）。注入的输入算「已连」（validate 钩子看得见）；
    端口同时有连线、同一节点既注入输出又注入输入报 `bad_input`，名字都不是报 `unknown_port`；注入到图里没有的节点
    保持老行为（不起作用、不报错）。结构体与函数签名都没变，与 M8a 取舍 11 同一个理由。
12. **CLI `--input` 自己读 PCD**（`bridge/src/pcd.rs`：ascii / binary / binary_compressed，点序、NaN 槽、intensity、rgb 原样），
    其余格式（PLY）才借 core 的 `io.load_pcd`。原因见第 11 条的「发现」：取数视图没有 rgb，给它加字段是 ABI 破坏。
    与 `io.load_pcd` 三种格式读出同一批点有 `cargo test` 钉着。被 `--input` 喂了的必填输入在运行前那次 `lyflow validate`
    里不算 `missing_input`（执行期的校验知道注入）。
13. **实时校验**（L16）：doc 或图路径一变，debounce 200 ms 调一次 `validate_graph`（新 store `store/validation.ts`）；
    error 标到节点上（红边框 + 第一条诊断贴在节点底部，全部在 title 里）与 Inspector 的参数上（与上一次运行的错误合并，
    同一个参数两边都有时取校验的那条 —— 它对着当前的值）；warning 只进 Inspector 的「校验」小节。刚拖入、必需输入还没接的
    节点也会因为 `missing_input` 标红 —— 这是有意的，错误在拼的时候就看见。静态传输不校验。
14. **Bundle 里的点云算节点「自己的云」**：主 3D 视图与 Peek 的底图规则认 Bundle 的点云字段（`<port>.<field>`），给了本次运行
    的统计就挑点最多的字段（ScanPair 是 merged），没给取声明里最后一个点云字段。`resolveOutput` 认 `<port>.<field>`
    （子图边界只按端口名走，字段接回去）。
15. **e2e 的 HTML5 拖放用 `DragEvent` + 真的 `DataTransfer` 驱动**：CDP 的鼠标事件不触发 HTML5 拖放。面板行自己的
    `dragstart` 写 MIME、落点处的元素收 `dragover` / `drop`，走的是画布真实的 onDrop；2D 拖框与连线拖拽仍是真实鼠标。
    写进了 `scripts/e2e/README.md`「踩过的坑」。

## 未验证

- **在真实工件上手工拖框**：e2e 用的是合成剖面，真实模板（R 系列十几毫米宽的缝）上拖框的手感没有人看过；
  视图面板只有 380 px 宽，拖几毫米的缝框时一个像素约 0.1 mm。
- `gap.align_template` 的四个 roi 参数没标 roi（它的模板是输入端口，要做就得给 `roiBackdrop` 加「按输入端口取底图」一种）。
- HTTP 传输（`examples/host-react`）的 2D 拖框底图与用户片段：`loadCloudFile` / `listSnippets` 没有 HTTP 实现，
  `docs/http-transport.md` 没加对应的端点。`pnpm e2e:http` 29/29 只说明没带坏老功能。
- 宿主侧的 `sceneId` 注入（xyz-gap-inspector 的 HTTP 服务把场景注入 `gap.load_profile_pair`）在仓库外，没有改成注入
  `read_scan`；新语义对它是兼容的。
- MCP 服务（`packages/mcp`）没有暴露片段与 `semantic` 字段（它读 manifest 的类型是宽松的，没有坏）。
- e2e 的两组真实 gap 图（`LYFLOW_GAP_GRAPH*`）照旧未设、整组不跑（与 M7 / M8a 相同）。

## commit

见 `git log`：core + gap（输入注入、片段、roi 标记、read_scan 1.1.0）+ CLI / 桥接一个；编辑器（自动连线、片段库、
2D 拖框、实时校验、Bundle 的 Edge Peek）+ e2e 一个；文档与本验收记录一个。`docs/m7-plan.md`、`docs/m8-plan.md` 未改动。
