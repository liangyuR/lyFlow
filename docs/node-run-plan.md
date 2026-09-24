# 节点运行按钮计划 —— 标题栏右端的小圆圈，只运行当前节点

目标：**每个节点标题栏右端有一个小圆圈，点一下只重算这一个节点**：上游用已有结果，不重跑；下游不动。

2026-09-24 与用户确认：
- 设计用 **A 方案**：14px 空心圆，嵌在标题栏右端，常驻；
- 点击**只运行当前节点**，不像「运行到此」那样连带跑上游；
- 上游没有可用结果时的处理、命中缓存时要不要重算，用户没有偏好，按推荐定：**上游不齐就不跑并提示；点了就强制重算**。

---

## 1. 现状

- 「运行到此」（右键菜单，`CanvasActions.onRunToNode`）= `targets: [id]`，core 保留目标的上游闭包，有缓存的节点跳过、没缓存的照跑。
- core 没有「只跑这几个节点」「强制重算」的开关。`BuildOptions` 只有 `targets`、`cacheNamespace`、注入相关字段（`core/src/exec/plan.h`）。
- 结果仓按 cacheKey 内容寻址；cacheKey 依赖上游的 cacheKey，所以**上游的参数一变，当前节点能取到的上游结果就对不上了** —— 这正是「上游不齐」的判据。

## 2. 定死的决定

### 执行语义（core）

| # | 决定 | 理由 |
|---|---|---|
| R1 | 运行请求新增 **`isolate: string[]`**（与 `targets` 同样的 id 语义，子图节点按路径前缀展开为全部内部节点）。给了 `isolate` 时，`targets` 取同一组 id（不需要调用方重复传） | 一个字段表达「只跑这些」，子图节点照样能点 |
| R2 | 编译照常保留上游闭包（cacheKey 要靠它算），但执行时：**不在 `isolate` 里的节点只许命中缓存**。任何一个需要的上游（非惰性依赖，或被 demand 的惰性依赖）在结果仓里没有当前 cacheKey 的结果，**开跑前**整次运行失败，不执行任何算子，报诊断 `upstream_not_ready`（run 级 error，`nodeId` 为缺结果的上游节点，每个缺的上游一条），消息「上游 X 还没有可用结果，先运行它或运行到此」 | 严格只跑当前节点，结果永远和上游参数对得上；失败在开跑前，不留半截状态 |
| R3 | `isolate` 里的节点**跳过缓存查找，强制执行**，结果照常写回结果仓（覆盖同 cacheKey 的旧结果），`stats.cached` 为 false | 点了就真跑一遍：调试、读外部文件的算子都需要 |
| R4 | 下游一概不进本次计划（`targets` 已经保证）。静音（bypass）的 isolate 节点照静音语义透传；缺失算子、校验不过的节点照现有规则报错 | 不引入新的执行分支 |
| R5 | `run_started` 事件带上 `isolate`（schema 同步），`mode` 仍是 full；预览模式与 `isolate` 不组合（同时给时 core 报参数错误） | 编辑器与 MCP 能看出这次是单节点运行；预览缓存是另一个命名空间，混用会误判「上游不齐」 |
| R7 | **计划外节点挂结果，不执行**（2026-09-24 验收后补）：给了 `isolate` 时，core 仍对全图算 cacheKey；不在本次计划里的节点（下游、兄弟支路）若结果仓里有它**当前 cacheKey** 的结果，就挂进这次 run（输出可按新 runId 取，`getOutputInfo` / `getOutputCloud` / Edge Peek / 3D 视图照常），但不执行、不发 running；`run_finished` 带 `attached: string[]` 列出挂上的节点。结果仓里没有的节点不挂，编辑器把它们退回 idle（不能显示「完成」却取不到输出）。挂结果不改变它们的 stale 判定 | isolate 节点的强制重算不改变它的 cacheKey，下游与兄弟节点的旧结果在语义上仍然有效；内容寻址本来就允许按 key 复用。否则单节点运行后，计划外节点显示「完成」却取不到输出（验收时在 a→b→c 上实测复现） |
| R6 | 贯穿所有层：C ABI 的 run spec、`bridge`（Tauri 命令与 HTTP）、`Transport` 接口的 `runGraph` 选项、三个 transport 实现、`packages/editor/test-server` 桩服务器（桩里实现最小语义：上游没跑过就回 `upstream_not_ready`）。CLI 与 MCP **不加**这个开关 | 编辑器用得到的路径都要通；CLI/MCP 的需求没提，不扩面 |

### 编辑器

| # | 决定 |
|---|---|
| U1 | 按钮在 `.node__head` 右端，排在徽标（子图 ⧉、库 L、静音 M）之后，`nodrag nopan` 类；`pointerdown`/`click`/`dblclick` 都 `stopPropagation`，不选中、不拖动、不触发改名。折叠节点也有；缺失算子节点没有 |
| U2 | 外观照 A 方案（见本文 §3）：14px 空心圆，描边 `--lyflow-fg-2` 1.5px。**hover** 变 accent 实心圆 + 白色 ▶；**运行中**圆环变进度环（accent 弧长 = progress，没有 progress 时是转圈的 3/4 弧，CSS 循环），中心一个小方块 ■；**完成**绿圈；**出错**红圈；**不可用**描边降到 40% 不透明度，光标 `not-allowed`。颜色与时长全部走 `styles.motion.css` 的变量与 docs/motion-plan.md 的约定（A5：按钮本身在标题栏里，不含端口，hover 缩放可以用在按钮的 SVG 上） |
| U3 | 点击 = `startRun(doc, path, { isolate: [id] })`。**运行中**点它自己 = 停止（`cancelCurrentRun`），只在这次运行是由这个按钮发起的时候显示 ■ 并可停；别的运行进行中时，所有节点按钮照常显示状态，点击行为与现有「运行到此」一致（新运行抢占旧运行） |
| U4 | **可用性预判**（纯提示，core 的 R2 才是权威）：当前层里，当前节点的每个必需、非惰性输入所连的上游节点都满足「本会话跑过、`outputsAvailable` 为真、不 stale」才可点。不可点时 `title` 写明「上游 X、Y 还没有可用结果 —— 先运行它们，或右键『运行到此』」。没有输入的源节点永远可点 |
| U5 | core 回了 `upstream_not_ready`：不弹对话框，走现有的 toast（warn 级），文案同 R2；缺结果的上游节点用现有的诊断定位样式闪一下（复用 docs/motion-plan.md S2 的 error 闪光，不抖动） |
| U6 | 按钮 `title` 默认「只运行此节点（上游用已有结果）」；运行中「停止」。`data-testid="node-run-<id>"`，`data-run-state` = `idle / running / done / error / disabled`，`data-run-reason` 在 disabled 时写缺的上游 id（逗号分隔）|
| U7 | 快捷键：无（这次不加）。右键菜单增加一项「只运行此节点」，与按钮同一动作、同一可用性 |

### 不做

「运行到此」的行为不变；Shift+点击等变体；CLI/MCP 的 `--isolate`；按钮的位置/样式可配置。

## 3. 设计稿（A 方案）

五个状态：默认（空心灰圈）、hover（accent 实心 + 白 ▶）、运行中（进度环 + 中心 ■）、完成（绿圈）、出错（红圈）；另加不可用（淡灰圈）。
尺寸：圆 14px（视觉），命中区 20×20px（padding 补齐），与标题文字垂直居中，右侧留 `padding-right` 与现有标题栏一致。

## 4. 验收

子代理写进 `docs/node-run-acceptance.md`，逐条标 通过 / 未通过 / 未验证，附实际输出。

**core / bridge**（doctest 与 `cargo test`，进 `pnpm check`）

1. `a → b → c` 全跑一遍后 `isolate: [b]`：只有 b 执行（b 的 `stats.cached == false`，a 命中缓存且算子的执行计数不变），c 不在计划里。
2. 改 a 的参数后 `isolate: [b]`：运行失败，诊断 `upstream_not_ready` 指向 a，**没有任何算子被执行**（执行计数断言）。
3. 从没跑过时 `isolate: [b]` 同 2；`isolate: [a]`（源节点）照常执行。
4. b 连续两次 `isolate: [b]`，两次都真执行（计数 +2）。
5. 子图节点 id 作 `isolate`：内部节点全部强制执行，子图外的上游只取缓存。
6. `isolate` + preview 同时给：参数错误。`run_started.isolate` 存在且符合 schema。
6b. （R7）`a → b → c` 与兄弟支路 `a → d` 全跑后 `isolate: [b]`：c、d 不执行（计数不变），但按新 runId 取得到 c、d 的输出，`run_finished.attached` 含 c、d；把 c 的结果从结果仓逐出（或 c 从没跑过）后再 isolate b，c 不在 `attached` 里。

**编辑器**（新 e2e 分组 `scripts/e2e/noderun.mjs`，接进 `run.mjs`、登记进 README；带 `LYFLOW_PACKS=gap;dts` 跑）

7. 每个非缺失节点的标题栏有 `node-run-<id>`，位于标题与徽标之后；折叠后仍在。真鼠标点击按钮：节点不被选中、不开始拖动、不进入改名；按钮位置不遮挡标题文字（标题的右边界 ≤ 按钮左边界）。
8. 全图跑过后点中间节点：只有它经历 running → done，其余节点的状态与耗时不变；下游节点不进计划。
9. 改上游参数后，中间节点按钮 `data-run-state="disabled"`、`data-run-reason` 含该上游 id；绕过预判直接调用 isolate 运行，得到 `upstream_not_ready` toast，且没有节点进入 running。
10. 运行中（找一个耗时可控的算子）按钮 `data-run-state="running"`，点击后运行被取消；由全图运行导致的 running 节点上点按钮，不是停止而是发起新运行（抢占）。
11. hover 按钮时计算样式为实心 accent；关动效（`prefers-reduced-motion`）时进度环不转但仍显示；端口连线端点对齐（复用 motion 分组的对齐断言工具，误差 ≤ 1px）。
11b. （R7）全图跑过后只运行中间节点：下游节点仍是 done，双击其入边能打开 Edge Peek 并看到数据，选中它 3D 视图有点云；从没跑过的下游节点保持 idle。
12. 右键菜单「只运行此节点」与按钮行为一致（至少验证一次成功路径、一次 disabled）。
13. **不回归**：`pnpm check` 全过；带 `LYFLOW_PACKS=gap;dts` 的 `pnpm e2e` 全绿，输出落盘、grep「未验」「跳过」；`pnpm e2e:http`（headless）全绿。

## 5. 实施顺序建议

core（BuildOptions / executor / C ABI / 事件 schema + doctest）→ bridge（Tauri、HTTP、`cargo test`）→ Transport 接口与三个实现、test-server 桩 → 编辑器按钮与可用性预判 → toast 与右键菜单 → e2e → 文档（`docs/embedding.md` 的 Transport 选项、`docs/interaction-checklist.md` 登记、`packages/editor/README.md`）。

---

## 6. 修订一（2026-09-24）：单击改为「智能运行」

用户看过业界对比后确认：**主操作是「让这个节点的结果变成最新」，由系统算出最少要跑哪些节点**（KNIME 的 Execute、Dataiku 的智能构建、Houdini/Nuke 的按需拉取）。
「严格只跑本节点」「强制重算」降为次级操作。本节**覆盖** §2 里与之冲突的条目（R1 的「isolate 隐含 targets」保留；R3、U3、U4、U6、U7 按下表改；R7 推广）。

| # | 决定 | 理由 |
|---|---|---|
| V1 | core 把「强制重算」从 `isolate` 里拆出来，成为独立的 **`force: string[]`**（id 语义同 `targets`，子图按前缀展开）：`force` 里的节点跳过缓存查找、强制执行、结果覆盖写回（原 R3 的行为）。`force` 可与 `targets`、`isolate` 组合，也可与 preview 组合。**`isolate` 不再隐含 force**：只保留 R2 的「上游只许取缓存，不齐就 `upstream_not_ready`」，本节点命中缓存就是命中 | 强制重算只对非确定、读外部文件的算子有意义，不该每次点击都付代价；两个语义正交，拆开才能自由组合 |
| V2 | **R7 推广到所有带 `targets` 的运行**（包括既有的「运行到此」、智能运行、isolate）：计划外、结果仓里有当前 cacheKey 结果的节点挂进这次 run，`run_finished.attached` 列出；编辑器对这类运行保留节点表，被挂上的保持原状态，没挂上的退回 idle。全图运行（无 targets）行为不变 | 部分运行后，下游显示「完成」就必须取得到输出 —— 这条对「运行到此」同样成立 |
| V3 | 按钮**单击 = 智能运行**：`startRun(doc, path, { targets: [id] })`，即「运行到此」的语义：本节点 + 缺结果或过时的上游，有缓存的复用，下游不进计划。**按钮不再因为上游不齐而置灰**；只在本节点算子缺失（本来就没有按钮）或本节点有编辑期校验 error 时置灰 | 用户不必自己做依赖分析；改完上游参数直接点下游就能看结果 |
| V4 | **Shift+单击 = 强制重算此节点**：`{ targets: [id], force: [id] }`，上游仍走智能判断 | 给非确定、读外部文件的算子一个显式入口 |
| V5 | hover 提示（`title`）按预判写三种之一：「运行此节点」+ 需要一起跑的上游时「（将一并运行上游 A、B）」；全部就绪且本节点命中缓存时「已是最新（命中缓存）—— Shift+点击强制重算」；其余「运行此节点」。预判复用编辑器已有的精确 stale / 执行状态（ADR-0007 的那套），是提示不是权威。`data-run-state` 去掉因上游产生的 `disabled`，新增 `data-run-upstream`（将一并运行的上游 id，逗号分隔） | 点之前就知道会跑多少 |
| V6 | 右键菜单三项，同一组放在一起：「运行到此」（既有项，与单击同一动作）、「强制重算此节点」（= Shift+单击）、「仅此节点（用现有上游）」（= `{ isolate: [id] }`，上游不齐时置灰并写明缺谁，core 回 `upstream_not_ready` 时照 U5 提示）。原 U7 的「只运行此节点」由后两项取代 | 严格模式留给要精确控制的人，不占主操作 |
| V7 | 停止语义照 U3：由按钮（单击或 Shift+单击）发起的运行，运行中点它自己 = 停止；别的运行进行中点它 = 抢占 | 不变 |

**不做**（修订一）：KNIME 式的「改参数后下游显示待运行」另立名目 —— 现有 stale 虚线框已覆盖，只在文案里统一叫「已过时」。

### 修订一的验收（覆盖 §4 中冲突的条目，其余照旧）

core / bridge：
14. `a → b → c` 全跑后 `targets:[b]`：b 命中缓存、没有算子执行；`targets:[b], force:[b]`：只有 b 执行（计数 +1），a 命中缓存；改 a 的参数后 `targets:[b]`：a、b 执行，c 不执行。
15. `isolate:[b]`（不带 force）在 b 已缓存时命中缓存、计数不变；`isolate:[b], force:[b]` 执行。原验收 4 按此改写。上游不齐时 `isolate:[b]` 仍 `upstream_not_ready`（原验收 2、3 照旧）。
16. 「运行到此」`targets:[b]` 之后，c、兄弟支路 d 在 `attached` 里、按新 runId 取得到输出；`force` + preview 可组合并进预览命名空间。

编辑器（改写 noderun.mjs，并检查既有分组里依赖「运行到此清空节点表」的断言，按 V2 改并在验收记录里逐条说明）：
17. 改上游 a 的参数后，b 的按钮可点，`title` 含 a、`data-run-upstream` 含 a；单击后 a、b 依次 running → done，c 不进计划且仍 done、取得到输出。
18. 全部就绪时单击 b：b 显示「已缓存」、没有真执行（stats.cached 为 true）；`title` 为「已是最新」那条。真鼠标 Shift+单击 b：b 真执行（cached 为 false），a 不执行。
19. 右键三项：「运行到此」与单击一致；「强制重算此节点」与 Shift+单击一致；「仅此节点」在上游过时时置灰且写明缺谁，绕过预判调用得到 `upstream_not_ready` toast、没有节点 running。
20. 本节点有编辑期校验 error 时按钮置灰；运行中点自己停止、全图运行中点它抢占（原验收 10 照旧）。
21. 不回归：`pnpm check`、带 `LYFLOW_PACKS=gap;dts` 的 `pnpm e2e`（落盘、grep 未验/跳过）、`pnpm e2e:http` headless 全绿。
