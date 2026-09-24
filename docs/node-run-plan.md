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
