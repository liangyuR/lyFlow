# 节点运行按钮 验收记录

对应 [node-run-plan.md](node-run-plan.md) §4。2026-09-24，Windows 11，WebView2，`LYFLOW_PACKS=gap;dts`。

- `pnpm check`：退出码 0，「全链路绿」。doctest **282/282**（其中新文件 `core/tests/test_noderun.cpp` 8 个用例、
  85 条断言），`cargo test`（bridge）**139/139**（新增 3 条），执行事件样例 **28 条**符合 schema（新增 7 条单节点运行的样例），
  `@lyflow/editor` 单测 15/15，两个前端构建，MCP 45/45，`lyflow-client` 单独构建。
- `pnpm e2e`：退出码 0，**672/672 项通过**，其中新分组 `scripts/e2e/noderun.mjs` **69 项**。完整输出落盘后
  grep「未验」「跳过」「FAIL」「✗」均为 0 行；gap 的张量组、M8b / M8c 的分组都真的跑了。
- `pnpm e2e:http`：`LYFLOW_E2E_HEADLESS=1` 下退出码 0，**42/42**，其中新增「只运行此节点」一组 7 项（验桩服务器的 R6 最小语义）。
- **R7（计划外节点挂结果，验收后补，第二个 commit）落地后重跑**：`pnpm check` 退出码 0、全链路绿，doctest **285/285**
  （`test_noderun.cpp` 11 个用例、130 条断言），`cargo test` 139/139，事件样例 28 条；`pnpm e2e` 退出码 0，**688/688**，
  `noderun.mjs` **85 项**（新增 11b 一组 16 项），grep「未验」「跳过」「FAIL」「✗」0 行；`pnpm e2e:http` headless 退出码 0，**44/44**。

| # | 验收项 | 结果 |
|---|---|---|
| 1 | 全跑后 `isolate: [b]`：只有 b 执行，a 命中缓存且计数不变，c 不在计划里 | ✅ 通过 |
| 2 | 改 a 参数后 `isolate: [b]`：失败、`upstream_not_ready` 指向 a、零执行 | ✅ 通过 |
| 3 | 从没跑过时同 2；`isolate: [a]`（源节点）照常执行 | ✅ 通过 |
| 4 | 连续两次 `isolate: [b]`，计数 +2 | ✅ 通过 |
| 5 | 子图节点作 isolate：内部全部强制执行，子图外上游只取缓存 | ✅ 通过 |
| 6 | isolate + preview 参数错误；`run_started.isolate` 存在且符合 schema | ✅ 通过 |
| 6b | （R7）全跑后 isolate b：c、d 不执行但按新 runId 取得到，`attached` 含 c、d；c 没有当前结果时不挂 | ✅ 通过（口径见下） |
| 7 | 按钮位置、折叠仍在、真鼠标点击不选中/不拖动/不改名、不遮挡标题 | ✅ 通过 |
| 8 | 全图跑过后点中间节点：只有它 running → done，其余状态与耗时不变，下游不进计划 | ✅ 通过 |
| 9 | 改上游参数 → disabled + reason；绕过预判 → `upstream_not_ready` toast、零 running | ✅ 通过 |
| 10 | 运行中 running、点击取消；全图运行里点 running 节点的按钮是抢占 | ✅ 通过 |
| 11 | hover 实心 accent；关动效时进度环不转但显示；端点对齐 ≤ 1 px | ✅ 通过 |
| 11b | （R7）只运行中间节点后下游仍 done，Edge Peek 与 3D 视图取得到数据；从没跑过的下游保持 idle | ✅ 通过 |
| 12 | 右键「只运行此节点」与按钮一致（成功一次、disabled 一次） | ✅ 通过 |
| 13 | 不回归：`pnpm check`、带包的 `pnpm e2e`、headless `pnpm e2e:http` 全绿（R7 之后重跑同样全绿） | ✅ 通过 |

## 1–6. core（doctest）与 bridge（cargo test）

执行计数靠新加的测试算子 `test.tally`（`core/tests/test_ops.h`）：确定性（能命中缓存）、按 `tag` 参数分开计数，
所以一张图里 a / b / c 各记各的。现有的 `test.counted` 是全局一个计数器，数不出「a 没动、b +1」。

```
$ build/core/bin/lyflow-core-tests.exe -tc="*isolate*,验收*" -s
TEST CASE:  验收 1：全跑一遍后 isolate [b] —— 只有 b 执行，a 命中缓存，c 不在计划里
TEST CASE:  验收 2：改 a 的参数后 isolate [b] —— 开跑前失败，upstream_not_ready 指向 a，零执行
TEST CASE:  验收 3：从没跑过时 isolate [b] 同样失败；isolate [a]（源节点）照常执行
TEST CASE:  验收 4：b 连续两次 isolate [b]，两次都真执行
TEST CASE:  验收 5：子图节点作 isolate —— 内部节点全部强制执行，子图外的上游只取缓存
TEST CASE:  验收 6：isolate + preview 是参数错误；run_started.isolate 按原样带出
TEST CASE:  isolate 的节点不存在：整图级失败，零执行
TEST CASE:  isolate 节点静音：照静音语义透传，上游仍只取缓存
[doctest] test cases:  8 |  8 passed | 0 failed | 274 skipped
[doctest] assertions: 85 | 85 passed | 0 failed |
```

各条的关键断言：

1. `tallyOf(a)==1、tallyOf(b)==2、tallyOf(c)==1`；b 的 `done` 事件 `stats` 不带 `cached`，a 是 `skipped` + `cached: true`；
   `run_started.plan` 有 a、b，没有 c；c 一条事件都没有；seq 连续。
2. 三个计数都不变；`run_finished.error.code == "upstream_not_ready"`，消息含「上游 a 还没有可用结果」；
   `diagnostics` 的 nodeId 集合恰是 `{a}`；**一条 `node_state` 都没有**（没有 running，也没有把 a 标成 error）。
3. 结果仓清空后 `isolate [b]` 同 2；`isolate [a]` 的计划只有 a，状态 `pending → running → done`；之后 `isolate [b]` 就能跑。
4. 全图 1 次 + 单独 2 次 = 3；之后的普通运行里 b 是 `skipped/cached` —— 强制重算的结果照常写回了结果仓。
5. `g → s(x → y) → t`，`isolate [s]`：x、y 各 +1 且 `done` 不带 cached，g 是 cached，t 不在计划里。
6. `isolate + Preview`：`run_finished.error.code == "bad_input"`、零执行；普通的 `isolate [b]`：`run_started.isolate == ["b"]`、
   `targets == ["b"]`、`mode == "full"`；普通运行的 `run_started.isolate == []`。
   schema：`schema/execution-event.schema.json` 给 `run_started` 加了 `isolate: string[]`，给 `run_finished` 加了
   `diagnostics[]`，error.code 清单加了 `upstream_not_ready`；样例追加一次成功的单节点运行与一次上游不齐，`pnpm check` 校验 28 条通过。
   另外把 CLI 真跑一张图的 10 条事件落盘对着 schema 校验，也通过（`isolate: []` 那一版）。

bridge（经 C ABI v11 的 `isolate` 字段，`bridge/src/execution.rs`）：

```
test execution::tests::isolate_with_preview_is_a_bad_input ... ok
test execution::tests::isolate_without_upstream_results_fails_before_running_anything ... ok
test execution::tests::isolate_reruns_only_that_node_over_the_abi ... ok
test result: ok. 139 passed; 0 failed; 0 ignored; 0 measured; 0 filtered out
```

## 6b. R7：计划外节点挂结果（doctest + cargo test）

```
$ build/core/bin/lyflow-core-tests.exe -tc="*isolate*,验收*,R7*" -s
TEST CASE:  验收 6b：计划外的 c、d 不执行，但按新 runId 取得到输出，run_finished.attached 列出它们
TEST CASE:  验收 6b：结果仓里没有 c 当前 cacheKey 的结果（改了 c 的参数 / c 从没跑过）→ 不挂
TEST CASE:  R7：上游不齐、开跑前就失败时，已有的结果照样挂上
[doctest] test cases:  11 |  11 passed | 0 failed | 274 skipped
[doctest] assertions: 130 | 130 passed | 0 failed |
```

- `a → b → c`、`a → d` 全跑后 `isolate [b]`：`tally(c) == tally(d) == 1`（不执行），c、d 一条节点事件都没有、不在计划里；
  `run_finished.attached` 含 c、d、不含 a、b；Run 活着时 `ResultStore::get(新 runId, c/d, "cloud")` 取得到、点数 4，
  `outputsOf` 非空。普通运行的 `run_finished` 不带 `attached`。
- 「c 没有当前结果」用了两种办法：改 c 自己的参数（键变了，仓里那份对不上）→ c 不挂、d 照挂、按新 runId 取 c 失败；
  只跑到 b（`targets [b]`，c、d 从没跑过）再 isolate b → c、d 都不挂。**没有用「逐出」**：结果仓的 LRU 预算是进程级的，
  在测试里逼它恰好逐出 c 一个会牵连别的用例；键对不上与逐出在 `attach` 看来是同一件事（仓里没有那个键）。
- 开跑前就失败（只跑过 d 那一支，再 isolate c，上游 b 缺）：`attached` 含 a、d，不含 b、c；按新 runId 取得到 d。
  isolate 一个不存在的 id（整图编译失败）不带 `attached`。
- `cargo test`：`isolate_reruns_only_that_node_over_the_abi` 追加断言 `run_finished.attached == ["p"]`、
  `output_cloud(新 runId, "p")` 取得到点。

## 7–12. `scripts/e2e/noderun.mjs` 的实际输出

> 这一节是第二个 commit（R7）时的输出。§6 修订一把 8、9、10、12 按新语义改写了，改写后的输出见文末「修订一」。

```
── 单节点运行 验收 7：按钮在标题与徽标之后、折叠仍在；真鼠标点它不选中、不拖动、不改名
  ✓ 每个节点的 .node__head 里都有自己的 node-run-<id>
  ✓ 标题的右边界 ≤ 按钮左边界（不遮挡标题文字）
  ✓ 命中区 20 px、视觉圆 14 px（画布缩放下按比例）
  ✓ 顺序是 标题 → 徽标 → 按钮
  ✓ 折叠之后按钮仍在标题栏里
  ✓ 缺失算子的节点没有运行按钮
  ✓ （前提）单击按钮发起了只跑 a 的运行
  ✓ 那次运行 ok
  ✓ 单击按钮：节点没有被选中
  ✓ 单击按钮：没有进入改名
  ✓ 单击按钮：节点位置没动
  ✓ （前提）c 的按钮不可用（上游 b 还没有结果）
  ✓ 按住按钮拖动：节点不跟着走、也没被选中
  ✓ 双击按钮：没有进入改名、节点没被选中
  ✓ 点不可用的按钮（双击）：没有发起运行

── 单节点运行 验收 8：全图跑过后点中间节点 —— 只有它 running → done，其余状态与耗时不变，下游不进计划
  ✓ （前提）全图运行 ok
  ✓ （前提）b 的按钮可点（上游 a 已有结果）
  ✓ 这次运行 ok
  ✓ 只有 b 经历了状态变化
  ✓ b 经历了 running → done
  ✓ a、c 的状态与耗时不变
  ✓ run_started.isolate = [b]、mode 仍是 full
  ✓ 下游 c 不在计划里，上游 a 在（只取缓存）
  ✓ b 的按钮回到 done（绿圈）

── 单节点运行 验收 9：改上游参数 → 按钮 disabled 且写明缺谁；绕过预判直接跑 → upstream_not_ready toast、零 running
  ✓ （前提）全图运行 ok
  ✓ （前提）a 被标为 stale
  ✓ b 的按钮 data-run-state="disabled"
  ✓ data-run-reason 含 a 的 id
  ✓ title 写明上游还没有可用结果
  ✓ 不可用时光标 not-allowed
  ✓ 源节点 a 自己的按钮永远可点
  ✓ 点不可用的按钮不发起运行
  ✓ 运行状态 error
  ✓ run_finished.diagnostics 指向 a、code 是 upstream_not_ready
  ✓ warn 级 toast，文案是「上游 … 还没有可用结果」
  ✓ 没有任何节点进入 running
  ✓ 缺结果的上游 a 闪了一下定位光（data-flash="locate"）
  ✓ a 没有被标红（它没有失败）
  ✓ 定位闪光不抖动（标题栏没有位移）

── 单节点运行 验收 10：运行中按钮 running、点它取消；全图运行里 running 的节点上点按钮是抢占，不是停止
  ✓ （前提）全图运行 ok
  ✓ （前提）slow 节点够慢（1677 ms ≥ 300）
  ✓ 点了之后按钮 data-run-state="running"、带 ■（data-run-own=1）
  ✓ 运行中 title 是「停止」
  ✓ （前提）这时跑的是 isolate=[slow] 的那一次
  ✓ 再点一下：这次运行被取消（cancelled），没有发起新的
  ✓ 取消后按钮不再是 running
  ✓ （前提）slow 在全图运行里 running，按钮也显示 running 但没有 ■
  ✓ 点击发起了新的单节点运行（isolate=[slow]）
  ✓ 被抢占的全图运行以 cancelled 收场
  ✓ 抢占它的那次跑完了（ok），而不是被当成「停止」

── 单节点运行 验收 11：hover 实心 accent、关动效时进度环不转但仍显示、端点对齐 ≤ 1 px
  ✓ （前提）全图运行 ok
  ✓ （对照）没 hover 时是空心圈
  ✓ hover 按钮：圆的计算后 fill 是 accent（实心）
  ✓ hover 按钮：白色 ▶ 显出来、SVG 放大（按钮不含端口，A6 允许）
  ✓ hover 按钮期间：2 条边端点与锚点最大偏差 0 px ≤ 1
  ✓ （前提）慢图全图运行 ok
  ✓ （对照）动效开着时进度环在转（animation-name）
  ✓ 运行中（进度环在画）：端点与锚点最大偏差 0 px ≤ 1
  ✓ 关动效：进度环仍显示（有弧、有描边）
  ✓ 关动效：进度环不转（animation-name 为 none）

── 单节点运行 验收 11b：只运行中间节点后，下游仍是 done、Edge Peek 与 3D 视图照样取得到数据；从没跑过的下游保持 idle
  ✓ （前提）全图运行 ok
  ✓ （前提）只运行 b 的那次 ok
  ✓ run_finished.attached 含计划外的 c、d，不含从没跑过的 e
  ✓ c、d 没有执行（没有任何状态迁移，更没有 running）
  ✓ 下游 c、d 仍是 done
  ✓ 从没跑过的 e 保持 idle
  ✓ getOutputInfo(新 runId, c) 有 cloud 输出
  ✓ 选中 c：3D 视图画出了点云（11230/11230）
  ✓ d 的入边（源是 c）：在这条边上找到了真能点中的落点
  ✓ 双击 d 的入边开出了 Edge Peek
  ✓ Edge Peek 里是 c 的点云，总点数与 c 报的一致
  ✓ Edge Peek 没有「未运行 / 取不到」之类的占位
  ✓ （前提）这次只运行 b 也 ok
  ✓ c、d 的键变了、仓里没有 → 不在 attached 里
  ✓ 编辑器把 c、d 退回 idle
  ✓ a、b 不受影响（a 命中缓存、b 刚跑完）

── 单节点运行 验收 12：右键「只运行此节点」—— 与按钮同一动作、同一可用性
  ✓ （前提）全图运行 ok
  ✓ 右键菜单里有「只运行此节点」且可点
  ✓ 成功路径：运行 ok
  ✓ 成功路径：与按钮一样是 isolate=[b]
  ✓ 成功路径：只有 b 变了
  ✓ 成功路径：a、c 的状态与耗时不变
  ✓ 菜单点完就收起
  ✓ 不可用路径：菜单项 disabled，data-run-reason 含 a
  ✓ 不可用路径：与按钮的判定一致
```

口径说明：

- 「只有 b 变了」看的是 `window.__lyflow.transitions`（来自事件，见 README）。「a、c 的状态与耗时不变」比的是
  单节点运行前后 `snapshot().run.nodes` 里的 `{state, durationMs}`，逐字段相等。
- 「下游不进计划」读的是这次运行的 `run_started.plan`：分组在页面里另挂了一个 `transport.onExecutionEvent`
  记下 run_started / run_finished —— 单节点运行不清节点表，从 store 读不出计划。
- 「绕过预判」是 `window.__lyflow.run({ isolate: [b] })`：同一条 `startRun`，只是跳过了按钮上的 disabled 判定。
- 停止 / 抢占用的慢节点是三百万点过一道 0.0006 的体素栅格，本机约 1.7 s。
- 11b 用 `a → b → c → d` 只运行 b：c、d 都在计划外。Edge Peek 双击的是 **d 的入边**（c→d），它显示的是源 c 的输出 ——
  c 正是被挂上的那一个；c 的入边 b→c 显示的是刚重算的 b，验不出 R7。3D 视图选的也是 c。双击与读浮窗复用
  `peek.mjs` 的 `openByDoubleClick` / `waitPeek`（从那里导出），3D 视图用 `page.mjs` 的 `selectAndReadViewer`。
  「从没跑过的下游」e 是全图跑完之后才接到 c 上的节点。
- 端点对齐复用 `motion.mjs` 的 `align()`（从那里导出 `installMotionProbe` / `alignOf` / `worst`），口径同 motion 验收 4：
  路径起止点与 React Flow 锚点（圆点外缘）的距离，画布坐标。

## 13. 不回归

```
第一个 commit：
$ pnpm check            → 全链路绿（CHECK-EXIT=0）
$ pnpm e2e              → 672/672 项通过，全绿（E2E-EXIT=0）
$ grep 未验|跳过|FAIL|✗  → 0 行
$ LYFLOW_E2E_HEADLESS=1 pnpm e2e:http → 42/42 项通过，全绿（exit=0）

R7 之后（第二个 commit）：
$ pnpm check            → 全链路绿（CHECK-EXIT=0），doctest 285/285
$ pnpm e2e              → 688/688 项通过，全绿（E2E-EXIT=0）
$ grep 未验|跳过|FAIL|✗  → 0 行
$ LYFLOW_E2E_HEADLESS=1 pnpm e2e:http → 44/44 项通过，全绿（exit=0）
```

## 实现上的取舍（计划没写死、我自己定的纯实现细节）

- **C ABI 升到 v11。** `lyflow_run_options` 末尾加 `isolate` / `isolate_count`，结构体变长了，按惯例加 ABI 号
  （`c_api.h`、`lyflow-client::ABI_VERSION`、CMake 的安装版本号，顺手把停在 9 的那个也改对）。`client.hpp` 与
  `lyflow-client` 都是零初始化整个结构体，不用 isolate 的宿主不必改。`client.hpp` 的 `RunOptions` 没加这个字段（R6 只要求编辑器用得到的路径）。
- **upstream_not_ready 的形状。** 挂在 `run_finished` 上：`error` 是第一条，新字段 `diagnostics[]` 每个缺的上游一条
  （`nodeId / severity / phase / code / message`，与 `lyflow_validate` 的诊断同形）。**不发任何 `node_state`**：
  那些上游没有失败，发 error 会让编辑器把它们标红。phase 用 `execute`。
- **开跑前的探测跳过哪些节点。** isolate 自己、惰性闭包（开跑前不知道会不会被 demand）、静音与注入的节点（不调 compute，
  不算「重跑上游」）、校验没过的节点（交给执行期的老规则报它自己的错）。被 demand 的惰性上游在执行期撞上时，那个上游节点以
  `upstream_not_ready` 报 error（编辑器把它并进同一条 toast）。`noReuse` + isolate 会让所有上游都判成缺；不确定性算子的
  cacheKey 带 runId，永远判成缺 —— 两者都是「只许命中缓存」的直接推论。
- **强制重算的写回。** `ResultStore::put` 加了 `replace` 参数：isolate 节点的结果顶掉同 cacheKey 的旧 Data（原来的规则是「键相同即内容相同，保留旧的」）。
- **`run_started.isolate` 普通运行也发**（空数组），与 `targets` 同一个约定。
- **编辑器的执行 store。** 单节点运行不清节点表、不重置「过时」标记；`run_started` 只追加 cacheKey（`extendRanWith`）；
  不在 isolate 里的节点的 `node_state` 不落库、不进流水账。这是验收 8「其余节点的状态与耗时不变」的实现方式。
- **按钮五态的优先级**：自己发起的运行或节点正在 running → `running`；否则上游不齐 → `disabled`；否则上一次的结果 done/skipped(可取) → `done`，error → `error`；其余 `idle`。
  别的运行里 running 的节点显示进度环但没有 ■（U3）。`pending` 不算 running。进度 > 0 画弧长，否则转 3/4 弧。
- **title 里的上游写节点的显示名**（改过名用改的，否则算子 label），`data-run-reason` 写 id。toast 文案在前端按 R2 的同一模板拼（多个上游时「先运行它们」）。
- **定位闪光**：`lib/motion.ts` 加了一条 `flashNodesLocate` 通道，节点上挂 `data-flash="locate"`，光晕用 error 的红，不抖；关动效时不播（toast 已写明是谁）。
- **标题栏改成 flex**，标题自己省略；按钮命中区 20×20 用负外边距收回，标题栏不变高。按钮布局在 `styles.editor.css`，颜色、hover、进度环与 keyframes 在 `styles.motion.css`。
- **桩服务器（R6）**：按事件里的 cacheKey 记账判「上游有没有结果」，缺就不起 CLI、直接回一对 run_started / run_finished；
  齐了就按 `--to` 跑 CLI（CLI 没有 isolate，进程之间也不共享缓存，所以上游在桩里其实会被重算），并把 `isolate` 补回 run_started。
  isolate + preview 在桩里直接回 400。

## R7 的实现取舍

- **全图 cacheKey 怎么来**：isolate 运行另编一份不带 targets 的全图计划，只取它的 cacheKey（诊断丢弃、不执行）。cacheKey
  只依赖上游，所以计划里的节点两份键逐字相同。
- **挂谁**：全图里这次**没有收场事件**的节点，只要结果仓里有它当前 cacheKey 的**全部**输出端口（与缓存复用同一条「全有才算」）。
  跳过注入的节点（输出来自宿主这一次的数据）与不确定性算子（键带 runId，本来就挂不上）。静音节点照挂 —— 它透传的结果本来就存在自己的键下。
- **挂法**：`ResultStore::attach` 只写这次运行的索引、刷一下 LRU，不记 hits / misses（这不是一次缓存命中）。挂在 summary 之前，
  挂上的节点声明了图级输出的话 summary 里是 value。
- **范围比 R7 字面上宽一点（计划只说「计划外节点」）**：开跑前就因 `upstream_not_ready` 失败时，一个节点都没跑，
  这时把**整张图**里有当前结果的节点都挂上（包括计划里的上游）。理由同 R7：这次运行接替了上一次，桌面端会放掉上一次的索引，
  不挂的话整张图的输出都取不到。执行中途失败 / 取消时同样照挂（只挂没有收场事件的）。`isolate` 了不存在的 id、与 preview
  同时给这两种整图级失败（编译都没过）不挂，也不带 `attached` 字段。
- **编辑器**：isolate 运行收场时，带了 `attached` 字段才处理 —— 节点表里既不在 isolate、这次也没命中缓存、又不在 attached 里
  的节点退回 idle（并记一条 idle 迁移进流水账）。副作用：上游不齐失败后，键已经变了的那些节点（比如验收 9 里改过参数的 a 及其下游）
  也会退回 idle，而不是保持上一次的 done —— 那些结果本来就对不上当前的图、按新 runId 也取不到，这正是 R7 要的。stale 判定不变。
- **桩服务器**：按记账算 attached（全图 `plan` 的 cacheKey 跑出过结果、这次又没有事件的节点），把它补进 run_finished；
  挂上的节点 `getOutputInfo` 回那一次记下的输出信息。桩取点云本来就按图重跑 CLI、不看 runId。桩合成的「取消」run_finished 不带 attached。

## 未决问题（第一个 commit 时提出，已由 R7 解决）

**单节点运行之后，计划外节点（下游、兄弟分支）的输出取不到了。** 编辑器按验收 8 保留它们的状态（仍显示 done），
但 3D 视图、Edge Peek、`getOutputInfo` 都按**当前** runId 取数：新一次运行的索引里没有它们（R4：下游不进计划），
而 `RunManager` 在新一次运行结束时把上一次的 `finished` 句柄放掉（`lyflow_run_free` → 索引没了，内容寻址层的数据还在）。
实测（全图跑 a→b→c，再单独跑 b）：

```
c 的状态 "done"；getOutputInfo(新 runId, c) → []；
getOutputCloud(新 runId, c) 与 getOutputCloud(旧 runId, c) 都是「core 没有该结果」
```

可选方向（需要拍板，都超出本计划）：① 编辑器按节点记「它的结果属于哪次运行」，`RunManager` 为单节点运行多留一份基准运行；
② core 在单节点运行时把计划外、cacheKey 未变且仍在结果仓里的节点也挂进本次索引（不执行、不发事件）；
③ 接受现状，但把计划外节点显示成「结果需重新运行」而不是 done。

2026-09-24 选了方案 ②，写进 [node-run-plan.md](node-run-plan.md) 的 R7；实现与验收见上面 6b、11b 与「R7 的实现取舍」。

---

## 修订一（§6，V1–V7，验收 14–21）

2026-09-24。单击改为智能运行，`force` 从 `isolate` 里拆出来，R7 推广到所有带 targets 的运行，右键三项。

- `pnpm check`：退出码 0、全链路绿。doctest **289/289**（`test_noderun.cpp` 15 个用例、186 条断言），
  `cargo test`（bridge）**140/140**（新增 `run_to_node_attaches_unplanned_downstream`），事件样例 28 条符合 schema，
  编辑器单测 15/15，MCP 45/45。
- `pnpm e2e`：退出码 0，**705/705**；`noderun.mjs` **102 项**。grep「未验」「跳过」「FAIL」「✗」0 行。
  **第一次跑卡住了**：停在 M8b 验收 7 分组的开头（画布已清空、`window.__lyflow` 仍能响应），二十多分钟没有新输出，
  我手动结束了进程树，原样重跑一遍得到上面的 705/705。卡住的位置在 M8b 分组里、我没有改那一组，
  重跑没有复现，原因没有查到，记在这里。
- `pnpm e2e:http`：headless 退出码 0，**46/46**（「只运行此节点」一组 9 项：isolate 的 upstream_not_ready、
  挂结果、智能运行 + force 之后下游仍取得到）。
- 演示截图：`scripts/e2e/record-noderun.mjs`，见下面「录屏」。

| # | 验收项 | 结果 |
|---|---|---|
| 14 | `targets:[b]` 全就绪零执行；加 `force:[b]` 只有 b +1、a 命中缓存；改 a 后 a、b 执行、c 不执行 | ✅ 通过 |
| 15 | `isolate:[b]` 不带 force 命中缓存、计数不变；带 force 执行；上游不齐仍 `upstream_not_ready`（原 2、3 照旧，原 4 改写） | ✅ 通过 |
| 16 | 运行到此之后 c、d 在 `attached`、按新 runId 取得到；`force` + preview 可组合、进预览命名空间 | ✅ 通过 |
| 17 | 改上游 a 后 b 可点、title 含 a、`data-run-upstream` 含 a；单击后 a、b 依次运行，c 不进计划 | ⚠️ 通过（c 的最终状态与字面不同，见下） |
| 18 | 全就绪单击 b：「已缓存」、没真执行、title 为「已是最新」；Shift+单击 b 真执行、a 不执行 | ✅ 通过 |
| 19 | 右键三项：运行到此 = 单击、强制重算 = Shift+单击、仅此节点过时时置灰写明缺谁、绕过得 `upstream_not_ready` | ✅ 通过 |
| 20 | 本节点有校验 error 时按钮置灰；运行中点自己停止、全图运行中点它抢占 | ✅ 通过 |
| 21 | 不回归：check、带包 e2e、e2e:http headless 全绿 | ✅ 通过 |

### 14–16（doctest）

```
$ build/core/bin/lyflow-core-tests.exe -tc="验收*,isolate*,R7*,force*" -s
TEST CASE:  验收 1 / 15：全跑一遍后 isolate [b]（不带 force）—— b 命中缓存；再加 force [b] 才只有 b 执行
TEST CASE:  验收 2：改 a 的参数后 isolate [b] —— 开跑前失败，upstream_not_ready 指向 a，零执行
TEST CASE:  验收 3：从没跑过时 isolate [b] 同样失败；isolate [a]（源节点）照常执行
TEST CASE:  验收 4 / 15（按修订一改写）：isolate [b] 两次都命中缓存；isolate + force [b] 两次都真执行
TEST CASE:  验收 5：子图节点作 isolate + force —— 内部节点全部强制执行，子图外的上游只取缓存
TEST CASE:  验收 6：isolate + preview 是参数错误；run_started.isolate 按原样带出
TEST CASE:  isolate 的节点不存在：整图级失败，零执行
TEST CASE:  isolate 节点静音：照静音语义透传，上游仍只取缓存
TEST CASE:  验收 6b：计划外的 c、d 不执行，但按新 runId 取得到输出，run_finished.attached 列出它们
TEST CASE:  验收 6b：结果仓里没有 c 当前 cacheKey 的结果（改了 c 的参数 / c 从没跑过）→ 不挂
TEST CASE:  R7：上游不齐、开跑前就失败时，已有的结果照样挂上
TEST CASE:  验收 14：targets [b]（智能运行）—— 全就绪时零执行；加 force 只有 b 执行；改 a 参数后 a、b 执行、c 不执行
TEST CASE:  验收 15：isolate 上游不齐时仍是 upstream_not_ready，带 force 也一样（force 只管本节点）
TEST CASE:  验收 16：运行到此 targets [b] 之后 c、d 在 attached 里、按新 runId 取得到；force + preview 进预览命名空间
TEST CASE:  force 给了不存在的 id：整图级失败
[doctest] test cases:  15 |  15 passed | 0 failed | 274 skipped
[doctest] assertions: 186 | 186 passed | 0 failed |
```

- 14：全跑后 `targets:[b]` → a、b 计数不变、没有 running、b `skipped/cached`；`targets:[b], force:[b]` → b +1、a cached、
  b 的 done 与 summary 都是 `cached:false`、`run_started.force == ["b"]`；改 a 的 pointCount 后 `targets:[b]` → a、b 各
  `pending → running → done`、c 计数不变且不在计划里。
- 15：原验收 1、4、5 按 V1 改写 —— 不带 force 的 isolate 一律命中缓存（计数不变），带 force 才执行（原 4 的「两次 +2」
  改成 isolate+force 两次 +2）；上游不齐时 isolate + force 仍是 `upstream_not_ready`（force 只管本节点，不替 isolate 补上游）。
  原 2、3 一字未改照过。
- 16：`targets:[b]`（Run 活着时）→ `attached` 含 c、d 不含 a、b，`ResultStore::get(新 runId, c/d)` 取得到；全图运行不带 `attached`；
  `preview + targets:[b] + force:[b]` → b 真跑一次，之后不带 force 的预览运行命中它（预览命名空间），正式运行仍命中正式那一份，计数不再变。
- cargo test 追加：`isolate_reruns_only_that_node_over_the_abi` 改为 isolate + force（并断言 `run_started.force`），
  新增 `run_to_node_attaches_unplanned_downstream`（只给 targets 的运行 `attached == ["p"]`、按新 runId 取得到 p 的点云）。

### 17–20（`scripts/e2e/noderun.mjs` 改写后的实际输出）

```
── 单节点运行 验收 7：按钮在标题与徽标之后、折叠仍在；真鼠标点它不选中、不拖动、不改名
  ✓ 每个节点的 .node__head 里都有自己的 node-run-<id>
  ✓ 标题的右边界 ≤ 按钮左边界（不遮挡标题文字）
  ✓ 命中区 20 px、视觉圆 14 px（画布缩放下按比例）
  ✓ 顺序是 标题 → 徽标 → 按钮
  ✓ 折叠之后按钮仍在标题栏里
  ✓ 缺失算子的节点没有运行按钮
  ✓ （前提）单击按钮发起了智能运行 targets=[a]（不是 isolate）
  ✓ 那次运行 ok
  ✓ 单击按钮：节点没有被选中
  ✓ 单击按钮：没有进入改名
  ✓ 单击按钮：节点位置没动
  ✓ （20）b 有校验错误：按钮 data-run-state="disabled"，title 说明原因
  ✓ （20）不可用时光标 not-allowed
  ✓ （20）上游不齐不再让按钮置灰：c 的上游 b 从没跑过，c 的按钮仍可点
  ✓ 按住按钮拖动：节点不跟着走、也没被选中
  ✓ 双击按钮：没有进入改名、节点没被选中
  ✓ （20）点置灰的按钮（双击）：没有发起运行
  ✓ （20）改回合法参数：按钮恢复可点

── 修订一 验收 18：全部就绪时单击 = 已缓存、不真跑；Shift+单击 = 只有本节点真跑
  ✓ （前提）全图运行 ok
  ✓ 全部就绪：b 的 title 是「已是最新」那条
  ✓ 全部就绪：没有要一并运行的上游、按钮可点
  ✓ 单击：运行 ok
  ✓ 单击：是智能运行 targets=[b]、没有 force
  ✓ 单击：b 显示「已缓存」（stats.cached=true），没有真执行
  ✓ 单击：节点上的标签是「已缓存」
  ✓ 单击：没有任何节点进入 running
  ✓ Shift+单击：运行 ok
  ✓ Shift+单击：targets=[b]、force=[b]
  ✓ Shift+单击：b 真执行（running → done，cached=false）
  ✓ Shift+单击：a 不执行（命中缓存，没有 running）
  ✓ Shift+单击：节点没有被加进选区（Shift 是多选键，按钮吞掉了它）
  ✓ 下游 c 两次都不进计划，仍是 done

── 修订一 验收 17：改上游参数后 b 仍可点，hover 提示预告「将一并运行上游」，单击后 a、b 依次运行
  ✓ （前提）全图运行 ok
  ✓ b 的按钮可点（不再因上游过时置灰）
  ✓ data-run-upstream 含 a
  ✓ title 预告「将一并运行上游 合成点云」
  ✓ 运行 ok
  ✓ a、b 都经历了 running → done
  ✓ 依次：a 的 done 早于 b 的 running
  ✓ c 不进计划
  ✓ c 的键变了、不在 attached 里 → 退回 idle（不显示「完成」却取不到输出）
  ✓ 不受影响的 d 挂上了、仍是 done、按新 runId 取得到输出
  ✓ getOutputInfo(新 runId, d) 有 cloud
  ✓ 跑完之后 b 回到「已是最新」

── 修订一 验收 19：「仅此节点」—— 上游过时时置灰写明缺谁；绕过预判 → upstream_not_ready toast、零 running
  ✓ （前提）全图运行 ok
  ✓ （前提）a 被标为 stale
  ✓ 「仅此节点」置灰，data-run-reason 含 a，title 写明缺谁
  ✓ 同一时刻按钮本身仍可点（智能运行会把 a 一起跑）
  ✓ 运行状态 error
  ✓ run_finished.diagnostics 指向 a、code 是 upstream_not_ready
  ✓ warn 级 toast，文案是「上游 … 还没有可用结果」
  ✓ 没有任何节点进入 running
  ✓ 缺结果的上游 a 闪了一下定位光（data-flash="locate"）
  ✓ a 没有被标红（它没有失败）
  ✓ 定位闪光不抖动（标题栏没有位移）

── 单节点运行 验收 10 / 修订一 20：Shift+单击发起的运行中按钮 running、点它取消；全图运行里 running 的节点上点按钮是抢占，不是停止
  ✓ （前提）全图运行 ok
  ✓ （前提）slow 节点够慢（1684 ms ≥ 300）
  ✓ 点了之后按钮 data-run-state="running"、带 ■（data-run-own=1）
  ✓ 运行中 title 是「停止」
  ✓ （前提）这时跑的是按钮发起的 targets=[slow] 那一次
  ✓ 再点一下：这次运行被取消（cancelled），没有发起新的
  ✓ 取消后按钮不再是 running
  ✓ （前提）slow 在全图运行里 running，按钮也显示 running 但没有 ■
  ✓ 点击发起了新的智能运行（targets=[slow]）
  ✓ 被抢占的全图运行以 cancelled 收场
  ✓ 抢占它的那次跑完了（ok），而不是被当成「停止」

── 单节点运行 验收 11：hover 实心 accent、关动效时进度环不转但仍显示、端点对齐 ≤ 1 px
  ✓ （前提）全图运行 ok
  ✓ （对照）没 hover 时是空心圈
  ✓ hover 按钮：圆的计算后 fill 是 accent（实心）
  ✓ hover 按钮：白色 ▶ 显出来、SVG 放大（按钮不含端口，A6 允许）
  ✓ hover 按钮期间：2 条边端点与锚点最大偏差 0 px ≤ 1
  ✓ （前提）慢图全图运行 ok
  ✓ （对照）动效开着时进度环在转（animation-name）
  ✓ 运行中（进度环在画）：端点与锚点最大偏差 0 px ≤ 1
  ✓ 关动效：进度环仍显示（有弧、有描边）
  ✓ 关动效：进度环不转（animation-name 为 none）

── 单节点运行 验收 11b：单击中间节点后，下游仍是 done、Edge Peek 与 3D 视图照样取得到数据；从没跑过的下游保持 idle
  ✓ （前提）全图运行 ok
  ✓ （前提）只运行 b 的那次 ok
  ✓ run_finished.attached 含计划外的 c、d，不含从没跑过的 e
  ✓ c、d 没有执行（没有任何状态迁移，更没有 running）
  ✓ 下游 c、d 仍是 done
  ✓ 从没跑过的 e 保持 idle
  ✓ getOutputInfo(新 runId, c) 有 cloud 输出
  ✓ 选中 c：3D 视图画出了点云（11302/11302）
  ✓ d 的入边（源是 c）：在这条边上找到了真能点中的落点
  ✓ 双击 d 的入边开出了 Edge Peek
  ✓ Edge Peek 里是 c 的点云，总点数与 c 报的一致
  ✓ Edge Peek 没有「未运行 / 取不到」之类的占位
  ✓ （前提）这次只运行 b 也 ok
  ✓ c、d 的键变了、仓里没有 → 不在 attached 里
  ✓ 编辑器把 c、d 退回 idle
  ✓ a、b 命中缓存（skipped/cached），不受 c 的改动影响

── 修订一 验收 19：右键三项 —— 运行到此 = 单击、强制重算此节点 = Shift+单击、仅此节点（用现有上游）
  ✓ （前提）全图运行 ok
  ✓ 三项都在、文案对
  ✓ 三项放在一起、依次排列
  ✓ 全部就绪时三项都可点
  ✓ 「运行到此」：targets=[b]、不带 force、b 命中缓存（与单击一致）
  ✓ 「强制重算此节点」：targets=[b]、force=[b]，b 真跑、a 命中缓存（与 Shift+单击一致）
  ✓ 「仅此节点」：isolate=[b]、运行 ok
  ✓ 「仅此节点」：只有 b 的显示变了
  ✓ 「仅此节点」：a、c 的状态与耗时不变
```

### 与计划字面不同、需要知会的地方

- **验收 17「c 不进计划且仍 done、取得到输出」与 V2 冲突，我按 V2 做了。** c 是 b 的下游，a 的参数一改，c 的 cacheKey
  跟着变，结果仓里没有 c 的当前结果 —— 按 V2（R7 推广）的定死决定它「没挂上 → 退回 idle」。要让 c 仍显示 done 并取得到输出，
  只能挂 c 的**旧键**结果，那与 R7「当前 cacheKey」相矛盾（旧结果对不上改过的 a）。e2e 里对 c 断言的是「不在 attached、退回 idle」，
  「仍 done、取得到输出」改由一个与这条链无关的源节点 d 验（它挂上了、按新 runId 取得到）。验收 11b（改的是下游自己之外的东西、
  键不变）里下游仍 done、取得到，照计划字面过。演示录屏第 3 步同理：改了源头之后，下游 d、e 在第 3 步结束时退回 idle。
  如果用户要的是「过时但可看旧结果」，那是 V2 之外的新语义（显示旧键结果并标过时），需要重新决定。
- **「运行到此」菜单项运行中仍是灰的**（既有行为 `disabled={running}`），而按钮与新加的两项运行中都可点（抢占）。V6 说它「与单击同一动作」，
  动作确实相同（都是 `targets:[id]`），只是这条既有的可用性没改。
- **按钮的「自己的运行」按 `targets` 恰好是 `[本节点]` 判**：所以从右键「运行到此」「强制重算此节点」「仅此节点」、Shift+F5 选中单个节点发起的运行，
  按钮上也显示 ■、点了是停止。V7 只说「由按钮发起」；编辑器不单独记来源，同义动作一并算。
- **预判的祖先只看当前层**（子图里从子图输入口进来的上游不在这一层的边上），判「会不会真算」优先用 `plan_graph` 的 `cached`（排除惰性、静音节点），
  拿不到时（HTTP 桩、图编不过）退回执行状态。每次正式运行收场立刻重编一次计划（不等 150 ms debounce），否则刚跑完的节点仍被当成「没缓存」。
- **C ABI 仍是 v11**：`force` / `force_count` 与 `isolate` 同一版加（v11 尚未发布，只在本地这几个 commit 里）。
- **V2 覆盖了预览**：live preview 发的是 `targets:[拖动的节点]` + preview，按 V2 也挂结果、不清节点表 —— 计划外、预览命名空间里没有结果的节点
  收场时退回 idle，与以前「预览运行清空整张节点表」的最终效果一致。`m4.mjs` 的 live preview 组（含 < 100 ms 的跟手延迟）照过。
- **既有分组里依赖「运行到此会清空节点表」的断言**：逐个查了带 targets 的三处 ——
  `run.mjs` 的「Run to node」（`run.nodes[tail] === undefined`）、`m3.mjs` 的「Shift+F5 跑到选中节点」（`run.nodes[tail] === undefined`）、
  `m4.mjs` 的 preview run（`targets:[sample]` 之后读源头点数）。前两处在同一组里先 `newDoc`（节点表清空）再做第一次运行，下游从来没跑过，
  按 V2 也不在表里，断言原样成立；第三处只读计划内节点。**三处都不用改，已随 705/705 跑过。** 真正按 V2 / V1 改写的是本分组自己：
  原验收 8（isolate 只重算 b）→ 18（单击命中缓存、Shift 真跑）；原 9（按钮因上游过时置灰）→ 17（按钮可点、预告上游）+ 19 的「仅此节点」置灰；
  原 10 的「单击发起、再点停止」→ Shift+单击发起（全图刚跑过，单击只会命中缓存、根本进不了 running）；原 12（右键一项）→ 19（三项）；
  11b 的「a、b 不受影响」由 `done` 改为 `skipped/cached`（智能运行复用缓存）；11 的进度环采样改用 `targets + force`。
- 桩服务器：`force` 天然成立（每次都是新进程，没有跨运行缓存），只补回 `run_started.force`；`attached` 对所有带 targets 的运行按 cacheKey 记账覆盖
  （进程里的 core 自己也会算，但那个进程的结果仓永远是空的）。

### 录屏

本机没有 ffmpeg，按约定输出每步一张 PNG（没有 mp4 / gif）。脚本 `scripts/e2e/record-noderun.mjs`（复用 harness / page.mjs，
CDP 驱动真实 Tauri app，带 gap;dts 包；不接进 run.mjs），5 节点链（合成点云 → 体素网格 → 直通滤波 → 体素网格 → 路由，排成两行），
画布缩放 1、只截画布那一块、1.5 倍像素：

| 步 | 文件 | 大小 |
|---|---|---|
| 1 全图运行 | `docs/noderun-step-1.png` | 64 KB |
| 2 改上游参数、hover 下游按钮 | `docs/noderun-step-2.png` | 83 KB |
| 3 单击：上游与本节点依次运行（截在体素网格 running 的那一刻） | `docs/noderun-step-3.png` | 77 KB |
| 4 再单击：已是最新、命中缓存 | `docs/noderun-step-4.png` | 76 KB |
| 5 Shift+单击强制重算 | `docs/noderun-step-5.png` | 70 KB |
| 6 右键菜单三项 | `docs/noderun-step-6.png` | 103 KB |
| 7a 运行中：进度环 + ■ | `docs/noderun-step-7a.png` | 66 KB |
| 7b 点 ■ 之后：cancelled | `docs/noderun-step-7b.png` | 69 KB |

合计约 620 KB。hover 提示用的是**画面顶部的一行字幕**（二选一里的字幕方案）：原生 title 气泡与系统鼠标指针都截不到，
脚本在页面上临时叠了字幕（写当前步骤，第 2、4 步把按钮 title 的原文抄进去）和一个跟着 CDP 真鼠标走的黄色指针标记，演示结束就撤掉。
第 7 步拆成 7a / 7b 两张（停止之前、之后）。

