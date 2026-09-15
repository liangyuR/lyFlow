# M5 验收记录 —— 能被 Agent 用

逐条对着 [m5-plan.md](m5-plan.md) §6 走：**怎么跑 + 实际输出 + 通过/未通过/未验证**。
所有验收由计划的作者独立执行，不是实现者自报；实现分三个 Opus 子代理完成，
每步落地后先跑 `LYFLOW_PACKS=gap pnpm check`，再用真实数据复现，再提交。

分支 `feat/m5-agent-tooling`，基于 `c7d5022`，四个提交：

| commit | 内容 |
|---|---|
| `e6bb715` | manifest 加 `preconditions`，事件加 `outputsAvailable`，`output_not_written` 错误码；gap 26 算子填适用前提，`load_profile_pair` 加 `layout` |
| `c03678a` | `lyflow eval`，`sweep` 改为薄壳，ADR-0020 |
| `a0e52cc` | `lyflow perturb` 与 `edit.translate_region`，`docs/agent-tuning.md` |
| `816fd80` | `@lyflow/mcp`，ADR-0021 |

数据：`luoshi/cloud/KUN10` 51 帧（同一 VIN，只证明重复性），19 张图来自
2026-09-15 真实调参任务的 scratchpad（已转成传感器布局的 PCD 在同目录 `sensor/`）。

## 门禁

| 命令 | 结果 |
|---|---|
| `LYFLOW_PACKS=gap pnpm check`（每步之后各跑一次，共 3 次） | 全绿。最终：core doctest 173/173（33767 断言）、manifest 50 算子符合 schema、execution-event 20 条、graph-doc、`cargo test` 88/88、CLI `--no-default-features` 构建、嵌入 SDK、前端、**新增 MCP 一步 35/35** |
| `LYFLOW_PACKS=gap pnpm e2e` | **332/332 全绿** |
| `pnpm check:gap` | **未跑**。两条 A/B 需要 `tianmu_0904` 数据集与模型基线，本轮没碰 A/B 路径 |
| `pnpm e2e:http` | **未跑**。盲测期间 8787 被 test-server 占着；MCP 的集成冒烟走的是同一个桩服务器 |

## §6 逐条

### ✅ 1. `lyflow eval` 零脚本复现 19 张图 × 51 帧的 std 表

每个点一份 samples.jsonl（`n_load.source=files` + 两个绝对路径），指标 `outputs.gap`：

```
point      n  ok  std_all    ref   std_a   std_b  mean_all
1         51  51   0.0627  0.062  0.0578  0.0682   -0.0017
1_1       51  51   0.0444  0.044  0.0496  0.0393   -0.0002
2         51  51   0.0702  0.070  0.0696  0.0720    0.1520
2_2       51  51   0.0726  0.072  0.0684  0.0739    0.1540
3         51  51   0.0308  0.031  0.0355  0.0259    0.3004
3_3       51  51   0.0330  0.033  0.0304  0.0354    0.3002
4         51  51   0.0669  0.066  0.0751  0.0532    1.0669
4_4       51  51   0.1051  0.104  0.1044  0.1048    0.8956
5         51  51   0.0419  0.041  0.0450  0.0386    0.1504
6         51  51   0.0505  0.050  0.0455  0.0558    0.1503
7         51  51   0.0658  0.065  0.0546  0.0766    0.1495
Audio_1   51  51   0.0349  0.035  0.0345  0.0339    0.1495
Audio_2   51  51   0.0657  0.065  0.0547  0.0761    0.1501
Audio_5   51  51   0.0441  0.044  0.0512  0.0363    0.1504
Audio_6   51  51   0.0866  0.086  0.0836  0.0911    0.1498
HUD_1     51  51   0.0647  0.064  0.0706  0.0588    2.9997
HUD_2     51  51   0.1424  0.141  0.1449  0.1415    3.0005
HUD_5     51  51   0.2721  0.269  0.2855  0.2525    3.0005
HUD_6     51  51   0.1016  0.101  0.0876  0.1163    3.0002
```

`ref` 是真实任务问卷里的数。19 个点全部一致；唯一差异是问卷用总体标准差（除 n），
`eval` 用样本标准差（除 n−1），比例正好 √(50/51)。计划要求「逐值一致到 1e-6」
在这个意义下成立：同一批值、两种归一化。

### ✅ 2. `--holdout` 按时间前后各半分别报数

上表 `std_a` / `std_b` 就是 `--holdout half=b` 的 train / holdout 两组。
点 4 前半 0.075、后半 0.053；HUD_5 前半 0.286、后半 0.253 —— 两组差异本身就是留出集存在的理由。

### ✅ 3. `lyflow perturb` 抓出两种失效

三条都是验收者用拷出的二进制独立复跑，与实现者报告逐字节一致。

**点 1 单侧张开**（基准在左，推右侧，`--after n_frame_s:cloud`，刀口 x=13.45 mm，`x=0:0.0006:5`）：

```json
{"kind":"perturb_summary","metric":"outputs.gap","expect":1000.0,"tolerance":100.0,"samples":51,"pass":47,"slopeMean":913.4,"slopeStd":297.0,"slopeMin":-336.4,"slopeMax":1000.0002,"nonResponsive":3,"signFold":0}
```

47/51 帧 `slopePos` 精确到 1000.0002，即 1.000 mm/mm。对称扫只有 23/51 通过，因为这条缝闭合，
负位移是让两件互相穿透，读数在那一侧饱和 —— 失败几乎全在 `slopeNeg`。

**点 7 的 `gap.flush` 取绝对值折叠**（刀口 x=13.5 mm，`x=-0.0003:0.0003:5`）：

```json
signed=false {"samples":51,"pass":0,"slopeMean":-251.0,"nonResponsive":3,"signFold":51}
signed=true  {"samples":51,"pass":49,"slopeMean":983.1,"slopeStd":39.4,"nonResponsive":0,"signFold":0}
```

51/51 报 `signFold`，对照组 0/51。这是 `slopeNeg` / `slopePos` 存在的理由。

**Audio_1**（基准在右，推左侧，`--after n_frame_p:cloud`，刀口 x=9.80 mm，`--expect -1000`）：

```json
{"samples":51,"pass":0,"slopeMean":29.3,"slopeStd":373.3,"nonResponsive":31,"signFold":16}
```

「读数不响应」复现了，**但归因与计划预期不同**。计划写的是「两台相机都被挡住右壁」，
实测是：Audio_1 的缝只有 0.04~0.15 mm 宽，位置却在 51 帧里游走 0.9 mm，
任何固定的几何刀都切不对多数帧（最好的刀口 15/51）。刀口切对的帧 `slopeNeg` 精确到 −1000。
这条限制写进了 ADR-0020 G7 与 agent-tuning.md。**没有调任何参数把它掰成预期。**

### ✅ 4. manifest 26/26 gap 算子有 `preconditions`

`lyflow manifest` 倒出核对：26 个 gap 算子全部有 2~3 条非空 `preconditions`，schema 校验过；
`gap.load_profile_pair` 有 `layout` 参数（default `sensor`，options `sensor|profile`）。
Inspector 在 doc 下方列出「适用前提」，`skipped` 节点按 `outputsAvailable` 区分「已缓存」与「未被需要」。

### ⚠️ 5. `outputsAvailable` 进 schema 与事件；业务侧 harvest 改为认它

schema、core、编辑器三处已落地，事件样例 20 条校验过。
**xyz-gap-inspector 那一处未改** —— 那是另一个仓库（`release/2.1.0` 的 `a5ba0e7` 现在同时认
`done` 与 `skipped`），改成认 `outputsAvailable` 要动业务仓库，留给阶段 B 切换时一并做。
另有一条实现者主动交代的缺口：`app/src/devbridge.ts` 的 e2e 快照没透出 `outputsAvailable`，
所以 e2e 断言不到这个字段。

### ✅ 6. 子代理盲测：点 4 / 4_4 的 `distThresh`

**怎么做的**：一个 Opus 子代理，工作目录在仓库外（`%TEMP%\m5blind`），
只给它 mcporter 配置（指向 `@lyflow/mcp`，后端是 test-server）、`agent-tuning.md` 的副本、
**脱敏后的**图副本（去掉了 `meta.note` —— 原图备注里写着 `distThresh 0.8` 与 std，会泄露答案）
与 51 帧的目录。`4.lyflow.json` / `4_4.lyflow.json` 的 `distThresh` 事先改回真实任务的起始猜测 0.1。
禁止读 `D:\project\` 下任何东西、禁止直接跑 `lyflow.exe`、禁止写解析或汇总 LyFlow 输出的代码；
每次 mcporter 调用前把命令原文追加到 `calls.log`。

**边界**：`calls.log` 21 行与它的自报一致；它写的唯一脚本 `gen_samples.ps1`（24 行）只生成
samples.jsonl；`work/` 下没有任何引用 `D:/project` 的东西。它主动交代了两点：mcporter 配置里
能看见 `D:/project/LyFlow-m5/packages/mcp/dist/index.js` 这个路径（没打开）；Claude Code 自动往
上下文注入了主仓库的 CLAUDE.md 与 git status（不是它读的）。看 LyFlow 输出时用了 `grep`/`sed`/`head`
分页显示，没做算术。**判定：边界成立。**

**结论**：8 个取值 × 51 帧 × 2 张图，816 次运行全部 ok，`failCodes` 全空。

| distThresh | 点 4 train | 点 4 holdout | 4_4 train | 4_4 holdout |
|---|---|---|---|---|
| 0.1 | 0.3330 | 0.2845 | 0.1435 | 0.3267 |
| 0.2 | 0.1662 | 0.1944 | 0.1151 | 0.1607 |
| 0.3 | **0.0762** | **0.0523** | 0.1362 | 0.1560 |
| 0.4~0.8 | 0.0765 | 0.0532 | 0.1107 → **0.0989**(0.6) | 0.1293 → **0.1047**(0.8) |

它得出「放宽更稳」，且比真实任务更细：点 4 在 0.3~0.4 就到平台（`inlierRatio` 到 1.0，
再放宽等于关掉外点剔除），4_4 的左面散布大，要到 0.6~0.8。它还指出 `distThresh` 把绝对读数整体
搬了 0.38 mm，比公差带还宽，并按文档把这一条标为「待实物标定的假设」。**它没有去调 `perturb`
先确认点 4 测的是那条缝**，自己在报告里承认了。

**成本**：21 次 MCP 调用（有效 15，5 次是自己的 shell 转义失误，1 次故意拼错指标换可用路径清单），
1 个输入数据生成脚本，0 个解析/统计脚本。真实任务里同一件事是 12 个脚本中的 3 个核心件。

### ✅ 7. 子代理盲测：Audio_1 用 `perturb` 独立判断

它先跑一帧、用 `summarize_output` 读 `n_notch:quality`，拿到锚点 9.858 mm、两侧穿出点
9.839 / 9.927 mm、两个拟合窗口 [5.3, 8.8] 与 [10.9, 14.4]，据此把刀口放在 9.857 mm、推左侧、
`--expect -1000`。固定刀切 51 帧：**36 帧不响应**。

然后它用 `eval` 取 `nodes.n_notch.quality.cameras.primary.midXMm` 逐帧看缝的位置，
发现缝游走 0.89 mm 而缝宽 0.025~0.169 mm，按文档 §4 那条硬限制判断问题在选区不在读数；
再手挑刀口确实落进缝里的 6 帧重跑：

```json
{"samples":6,"pass":4,"slopeMean":-879.3,"slopeStd":319.7,"nonResponsive":0,"signFold":0}
```

**它的判定与计划预期相反，而且更对**：Audio_1 的读数**确实在量那条缝**，刀口落进缝里时
0/6 不响应、斜率均值 −879；51 帧整体不响应是固定选区跟不上游走的缝。它还指出两帧增益只有
0.4~0.56 且 rmse≈0，推测是刀口切在相邻两个剖面点之间（点距 ≈0.09 mm，与缝宽同量级），
这条缝已经细到接近剖面采样极限。真实任务里「Audio_1 不响应」的结论，很可能是那个 Agent
自己的切分方式造成的假象 —— 这正是 G7 把「切在哪」显式化的价值。

## 盲测暴露的缺口（按它耽误时间的多少排序，工具面据此再加）

1. **`perturb` 的逐样本斜率没有落盘**。MCP 的 `failures` 截到 20 条，`rows.jsonl` 里只有原始值没有
   `perturb_sample`。文档教的诊断法（看不响应的帧是不是缝位置偏得最远的那些）因此做不完。
   补：CLI 与 MCP 都把 `perturb_sample` 落盘；MCP `failures` 加 `limit`。
2. **样本集的配对与打 tag 每个测点都要重做一遍**。补 `eval --samples-dir <root> --bind-pair
   <node>.<primary>,<secondary> --pattern <a>,<b>`，加「按目录名时间戳排序、前后各半打 tag」的开关。
   这是盲测里唯一那个脚本存在的原因。
3. **MCP `eval` 返回太大**（8 组 × 5 指标 = 80 个 summary，31 KB）。加 `compact` 只回
   params / metric / group / n / ok / mean / std。文档提到的 `--csv` 在 MCP 工具里没有对应参数，文档与工具对不上。
4. **文档没说 `--after` 该插在哪台相机后面**。`camera=primary` 时要插 `n_frame_p`，例子全写的 `n_frame_s`；
   `camera=both` 时怎么办没写。
5. **选区跟着缝走**。`--region` 允许 `pointFrom: "<node>:<port>"` 引用某个节点输出的 2D/3D 点，
   刀口逐帧跟锚点。这是平台级的通用引用，不是领域规则，不违背 G7；有了它 Audio_1 是一条命令。
6. **`list_metrics(graphPath)`**：现在要靳 `eval` 拼错一次换可用路径清单。
7. region / axis 的单位（米）只在正文里，参数 `unit` 里没有。

三次「想看源码」：perturb 截断阈值可不可配、`notch_width` 沿轮廓走到刀口的插值怎么写、
`inlierRatio=1` 时第二次拟合是否退化成整段最小二乘。第三条靠 manifest 的 `doc` 推出来了，前两条没有。

## 偏离与决策

1. 第二个合成任务（从 StandardGap.yml 零起点设计图）没有单独跑：真实任务本身就是 19 个点从零设计加调参，两类需求都覆盖了。
2. 「参数单位没标」不成立，没做（26 算子里缺 `unit` 的 19 个参数全是计数、比例、增益）。
3. `perturb` 的 `--expect` 写 ±1000 而不是 ±1：测量帧仍是米，只有 Measurement 是 mm。文档已写明。
4. `edit.translate_region` 的 category 用中文「编辑」，与仓库其他 44 个算子一致。
5. 实现在独立 worktree 上进行，与 `feat/gap-notch-width` 上未提交的 Edge Peek 工作隔离；
   因此 MCP 的 `summarize_output` 拿不到 tensor / indices 二进制端点（ADR-0019 在那个分支上未提交），
   张量只能给 `OutputInfo` 里的 shape/min/max/mean。两边合并后改一处函数即可。
6. `outputsAvailable` 的业务侧改动留给阶段 B。
7. 零注释规则在前两步各漏了几处，第三、四步顺手删掉；实现者与验收者都没发现前先交代的一处
   （`core/tests/test_ops.h` 里一行）由验收者删掉。

## 盲测缺口的补齐（第 1–4 条）

用户决定前四条立刻补，第五条与 `list_metrics` 留待单独决定。实现一个 Opus 子代理，验收如下：

| 项 | 怎么验 | 结果 |
|---|---|---|
| `perturb_sample` 落盘、`failuresLimit` | MCP `perturb` 点 7 `signed=true`，`samplesPath` 文件 51 行，`failuresLimit:2` 时 `failures` 2 条 | ✅ summary 与上文 49/51、983.1 一致 |
| 样本集目录模式 | `--samples-dir sensor --sample-subdir 4 --bind-pair n_load.primaryFile,n_load.secondaryFile --pattern '*Master*.pcd,*Slave*.pcd' --split-half half` 对点 4，与手写 `4.jsonl` 的 summary `diff` | ✅ **逐字节相同**（train n=26 std 0.0751、holdout n=25 std 0.0532）。这份数据字典序与时间戳序恰好一致，两者不一致的分支只有 cargo test 覆盖 |
| MCP `eval` 的 `compact` 与 `csv` | sdk Client 走 stdio 跑真实数据 | ✅ compact 1188 B 一组一行；`compact:false` 回原样 |
| 文档三处 | 逐条对照 `mcp.md` / `agent-tuning.md` §7 | ✅ 每条 CLI 选项有字段或明写「MCP 不提供」（`--parallel`、`--samples-jsonl-out`） |
| 门禁 | `LYFLOW_PACKS=gap pnpm check` 由验收者独立复跑 | ✅ core 173/173、`cargo test` 96/96、MCP 39/39 |

顺手修了一处文档错误：`edit.translate_region` 的 `doc` 原写「测量帧里就是毫米」，与 ADR-0020 G6 矛盾，测量帧仍是米。
