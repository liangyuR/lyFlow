# M5 实施计划 —— 能被 Agent 用（草稿，待确认）

目标：**一个只拿得到 CLI 或 MCP、拿不到仓库源码的 Agent，能独立完成「给一组测点设计图并调稳参数」，
过程中不需要自己写任何解析器、批跑器或评估脚本。** 顺带让人做批量评估也不用再写脚本。

前提：M4 已落地（CLI、sweep、图级命名输出、注入）。M5 **不改执行器、不改缓存、不在 core 里放任何 Agent 逻辑**。
新能力全部是 CLI 子命令、算子、manifest 字段与一个薄的 MCP 适配层，与桌面走同一条路径（ADR-0012）。

---

## 来源：一次真实任务的证据

范围不是拍脑袋定的。2026-09-15 一个 Agent 在 xyz-gap-inspector 上从零设计并调稳了 KUN10 的 19 个测点
（51 帧同 VIN 重复采集，无测量基线，只有客户公差带），约 4 小时，产出 `gap.notch_width` 与 `gap.flush` 的
`signed`/`scale`。事后按 21 题问卷复盘，与仓库现状核对后的结论：

| 观察 | 数字 | 对 M5 的含义 |
|---|---|---|
| 只用了 `run` 与 `manifest` 两个子命令；`validate`/`plan`/`sweep`/`dump`/`diff` 一次没用 | 2 / 9 | 现有子命令的形态不是 Agent 的工作单元 |
| 自己写了 12 个 Python 脚本，其中 JSON Lines 解析器、批跑器、统计各一份 | 至少一半代码是重复劳动 | **缺「批量 × 任意指标 × 统计」原语** |
| 合成位移验证是判断「测的是不是那条缝」的唯一手段；三版脚本里两版给出错误结论且不报错 | 900 个合成 PCD | **缺「扰动 → 灵敏度」原语**，且它必须由平台做对一次 |
| 单次 run 墙钟 0.05 s，图本身 29 ms；等计算的时间≈0 | 上万次 run 累计几分钟 | 瓶颈是判断不是算力，**不做自动优化器** |
| 读了 7 个 `ops/*.cpp` 反推语义；manifest 的参数 doc 评价「相当好」 | 26 个算子 0 个写了适用前提 | 缺的是**「什么时候它不成立」**，不是参数说明 |
| 缓存命中时 `state=skipped` 但输出照样在，业务侧 harvest 只认 `done`，产线交互页复现「几何消失」 | 1 个线上 bug | `skipped` 语义要显式化 |
| 「声明过的输出端口每个都必须写」没有文档，靠 `internal` 错误发现 | 1 次编译往返 | 错误码与文档各补一处 |
| 51 帧全部参与调参，无留出；零点建立在「这批件处于标称」的未验证假设上 | in-sample | 评估工具从第一天支持留出集，**策略由人定** |
| 归档 PCD 是剖面布局，算子要传感器布局，手工转了 1938 个文件 | 51×19×2 | gap 包的 load 算子加一个布局参数 |

不成立的一条：「参数单位没标」。核对 manifest，26 个 gap 算子里所有带量纲的数值参数都有 `unit`，
缺 `unit` 的 19 个全是计数、比例、增益。不做。

---

## 0. 定死的决定

| # | 决定 | 一句话理由 |
|---|---|---|
| G1 | **评估、扰动都是 CLI 子命令**，与 `run` 共用 `execute()` 与结果仓；MCP 只是它们的另一张皮 | 同一条路径（ADR-0012）；CLI 先被人用一轮，再决定 MCP 的工具面 |
| G2 | `lyflow eval` 取代并包含 `sweep`：**样本集 × 参数组 → 任意标量指标 → 内建统计**；`sweep` 保留为轴展开的语法糖 | 问卷第 20 题第一名；`--metric` 只认三个字段是 Agent 重写它的唯一原因 |
| G3 | 指标是**值路径**：`outputs.gap`、`outputs.bundle.point_counts.left`、`nodes.n_groove.quality.cameras.primary.cornerDepthMm`、`nodes.n_fit.durationMs` | 图级命名输出与 `stats.outputs[].value` 里已经有全部数据，缺的只是「按路径取」 |
| G4 | 样本集是一份 JSON Lines：每行 `{ id, set: { "<node>.<param>": value }, tags? }`；另给 `--samples-glob <pattern> --bind <node>.<param>` 一步生成 | 复用 `--set` 的语义，与注入（ADR-0017）正交；业务服务那条路以后走 sceneId 注入，接口不变 |
| G5 | 统计内建：n、成功数、失败码直方图、mean、std、min、max、p2p、按 `tags` 分组；**`--holdout <tag>` 把带该 tag 的样本排除在参数选择之外但照样报数** | 留出是方法论底线；切法由人定，工具只负责把两组数分开报 |
| G6 | 扰动 = **图手术 + eval**：新标准算子 `edit.translate_region`（cloud, 半空间或盒选区, 平移量）插在源节点之后，`lyflow perturb` 对平移量做轴扫描并报 `d(指标)/d(位移)` 与预期对照 | 不写 900 个 PCD；算子本身在编辑器里可单独用来「模拟缝张开」 |
| G7 | 选区只做**几何选区**（半空间 `{point, normal}` 与盒）；「最深点 + 偏置」这类领域规则不进平台，由调用方或 gap 包算出选区后传入 | 问卷里两版切错都出在领域判断，平台把「切在哪」显式化、可审计，而不是替人猜 |
| G8 | manifest 加可选 `preconditions: string[]`；gap 包 26 个算子**全部填**，Inspector 在 doc 下方列出（**M7 J4 已撤销**：preconditions 字段删除，约束改为加载期 validate 或运行期信号） | 「什么时候它不成立」比参数列表值钱；结构化字段让 Agent 能过滤，而不是在 doc 里找关键词 |
| G9 | `stats` 加 `outputsAvailable: bool`；`skipped` 不改名 | cached/bypassed/provided 为 true，not_demanded 为 false；改名是破坏性变更，收益不够 |
| G10 | 算子没写声明过的输出端口：错误码从 `internal` 改为 `output_not_written`，消息带端口名与「`Port.required` 只对输入有效」；写进 `docs/op-packs.md` | 一次编译往返换十行 |
| G11 | `gap.load_profile_pair` 加 `layout: sensor \| profile` 参数，默认 `sensor` | 1938 个文件的手工转换是产品历史债，在读入口解决一次 |
| G12 | MCP 服务是 `packages/mcp`，Node，stdio，**对着 `/lyflow/*` HTTP 契约说话**，外加本地起 CLI 的 eval/perturb | 同时能接 test-server 与阶段 B 业务服务，Agent 可以对着现场数据调参；不发明第四种语义 |
| G13 | **不做**：自动优化器、编辑器内 AI 面板、core 内任何 Agent 逻辑、标签与基线管理 | 瓶颈是判断；Transport 已是边界；标签是业务仓库的事 |

G1–G7 合写 ADR-0020（eval-and-perturb-as-cli）；G12 一份 ADR-0021（mcp-as-transport-consumer）。G8–G11 是小改，不单开 ADR。

---

## 1. `lyflow eval`

```
lyflow eval <graph> --samples <samples.jsonl> | --samples-glob <pattern> --bind <node>.<param>
                    [--params <paramsets.json>] [--param <node>.<param>=<start>:<end>:<steps>]...
                    --metric <path> [--metric <path>]...
                    [--holdout <tag>] [--group-by <tag-key>]
                    [--csv <out.csv>] [--base-dir <dir>] [--parallel <n>]
```

- stdout JSON Lines：每个（参数组 × 样本）一行 `eval_row`，末尾每个参数组一行 `eval_summary`（按 holdout 与 group 分别给统计）。
- `--params` 是显式参数组列表 `[{ "n_fit.distThresh": 0.1 }, { ... }]`；`--param` 轴展开成参数组后与之合并。两者都没给就是单组「图原样」。
- 同进程，缓存**默认开**：参数变了 cacheKey 就变，不会拿到错误答案（ADR-0007）；`--no-cache` 仍可用。
- 指标路径解析失败是 `EXIT_USAGE`，并列出这张图上所有可用的标量路径 —— Agent 第一次总会拼错。
- `sweep` 的 `--metric nodeId:port.field` 旧写法继续接受，内部翻译成路径。
- 退出码沿用 ADR-0012。

## 2. `lyflow perturb` 与 `edit.translate_region`

算子（`packs/std-pointcloud`）：输入 cloud；参数 `region: { kind: halfspace, point, normal } | { kind: box, min, max }`、`translation: vec3f`；
输出 cloud。选区内的点加平移，其余原样。确定性、可缓存。

```
lyflow perturb <graph> --after <node>:<port> --region <json> --axis <x|y|z>=<start>:<end>:<steps>
                       --samples ... --metric <path> [--expect <slope>] [--tolerance <v>]
```

- 在 `--after` 指定的端口后插入 `edit.translate_region`，对 `translation` 的一个分量做轴扫描，其余走 eval。
- 报每个样本的斜率 `d(metric)/d(位移)`（最小二乘）与残差，给了 `--expect` 就逐样本判定通过与否。
  Audio_1 那类「读数不响应」是斜率≈0，点 7 那类「取绝对值折叠」是正负两侧斜率符号相反且残差大 —— 两个真实失效模式都要能在报告里直接看出来。
- 只做几何选区（G7）。文档里写明：切分线穿过近竖直壁、或压在被选中的锚点上，会得出错误结论，选区必须在剖面上核对。

## 3. manifest 与事件的小改

- schema：`OperatorDesc.preconditions?: string[]`（M7 J4 已撤销）；`stats.outputsAvailable: boolean`。两份 example 与 `scripts/validate_schema.py` 跟着改。
- core：填 `outputsAvailable`；`output_not_written` 错误码。
- gap 包：26 个算子填 `preconditions`（`groove_joint`：「假定缝底有一条比两侧面都深的槽；缝闭合、两圆边直接相碰时不适用」；
  `selected_point`：「在整片输入云上选点，不裁 ROI」；`fit_line`：「`innerEnd` 取靠缝一端」……以问卷第 7、10 题为清单起点）；
  `load_profile_pair` 加 `layout`。
- 编辑器：Inspector 在 doc 下方列 `preconditions`；`skipped` 节点的 tooltip 按 `outputsAvailable` 区分「已缓存」与「未被需要」。
- 文档：`docs/op-packs.md` 加「输出端口契约」一节；`docs/architecture.md` 的 NodeState 段落补 `outputsAvailable`。

## 4. `packages/mcp`

Node + `@modelcontextprotocol/sdk`，stdio。配置：`LYFLOW_HTTP_BASE`（业务服务或 test-server）与 `LYFLOW_CLI`（本地 exe，eval/perturb 走它）。

| 工具 | 底下是什么 | 输出裁剪 |
|---|---|---|
| `list_operators` / `get_operator` | `GET /lyflow/manifest` | 列表只给 id/label/category/一句 doc；详情给全量含 `preconditions` |
| `validate_graph` / `plan_graph` | `POST /lyflow/validate` / `plan` | 原样 |
| `run_graph` | `POST /lyflow/run` + WS 收齐 | 返回 outputs、每节点 state/durationMs/stats、诊断；不返回云 |
| `summarize_output` | `GET clouds/tensors/indices` | 点数、包围盒、各通道 min/max/mean、前 N 个值；Node 侧算，core 不动 |
| `eval` / `perturb` | 本地 CLI | 只返回 `eval_summary` 与失败样本清单，`eval_row` 落文件给路径 |
| `diff_graphs` | `lyflow diff` | 原样 |

resources：manifest、三份 schema、`schema/examples/*`、各包 README、`docs/agent-tuning.md`。

**不在 MCP 里做的**：读写图文件（Agent 直接用文件系统）、点云可视化（给 `exportPng` 的路径即可）。

## 5. 实现顺序（每步 `pnpm check` 绿）

1. §3 全部（schema、core 两处、gap 包 preconditions 与 layout、编辑器两处、文档）
2. `lyflow eval`，`sweep` 改为语法糖；`cargo test` 覆盖路径解析、holdout 分组、旧 `--metric` 写法
3. `edit.translate_region` 算子 + doctest；`lyflow perturb`
4. `docs/agent-tuning.md`：评估 → 扰动 → 留出的工作法，含问卷里两个切错案例
5. `packages/mcp`
6. §6 验收

## 6. 验收（M5 完成的定义）

数据用问卷的那一套：`luoshi/cloud/KUN10` 51 帧、19 张最终图（`scratchpad/graphs/`）。

- [ ] `lyflow eval` 对 19 张图 × 51 帧、指标 `outputs.gap`，**零自定义脚本**复现问卷里那张 std 表（同数据同图，逐值一致到 1e-6）
- [ ] `--holdout` 把 51 帧按时间前后切两半，两组分别报数
- [ ] `lyflow perturb` 在 Audio_1 上报斜率≈0 并判定失败；在点 1 上报斜率≈1；对 `gap.flush` 的 `signed=false` 报出正负两侧斜率符号相反
- [ ] manifest 26/26 gap 算子有 `preconditions`，schema 校验过，Inspector 可见
- [ ] `outputsAvailable` 进 schema 与事件；xyz-gap-inspector 的 harvest 改为认它（那边一处）
- [ ] 一个 **Opus 子代理，只给 MCP 服务、`docs/agent-tuning.md` 与数据路径，不给仓库源码**，重做点 4/4_4 的 `distThresh` 调参，
      得出「放宽到 0.8 更稳」的同一结论；记录工具调用次数与它写了几个脚本（目标：0 个）
- [ ] 同一子代理对 Audio_1 用 `perturb` 独立发现「读数不响应」
- [ ] `pnpm check`、`pnpm e2e`、`pnpm check:gap` 全绿；`cargo build --bin lyflow --no-default-features` 仍不碰 Tauri

## 7. 风险

- **eval 的样本绑定只有 `--set` 一种**，业务服务的注入路径（sceneId）这一轮不接。留到阶段 B 切换后按需加，接口按 G4 不变。
- **perturb 的选区要人给**。工具把「切在哪」变成显式输入，但不保证切得对；`agent-tuning.md` 必须把两个反例写清楚。
- **51 帧同 VIN 只能证明重复性**。eval 报的 std 是干净的，零点仍然 in-sample。这是数据问题不是工具问题，文档里明说。
- MCP 工具面在子代理盲测前不锁定；盲测暴露出来的工具再加，没暴露的不加。

## 8. 已确认的决定（2026-09-16）

1. G4：样本集只用 `--set` 覆盖 JSONL，这一轮不做 sceneId 注入。行格式预留 `scene` 字段，将来接注入不改已有行。
2. G7：扰动选区只做几何选区。gap 包将来若要「按缝角自动定选区」，做成一个输出 region 的 gap 算子再喂给 `edit.translate_region`，不进平台。
3. G12：MCP 放本里程碑最后做，以子代理盲测验收；盲测拿到的是工具调用次数与脚本数这类硬数字，人用一轮只有感受。
4. 留出集按时间前后各半切。时间是这 51 帧唯一有结构的维度，前后半段之间夹着一天多的环境与漂移；随机切会抹掉它。切法只影响 std 的可信度，不修零点 in-sample 的问题。

实现在独立 worktree `feat/m5-agent-tooling` 上进行，与 `feat/gap-notch-width` 上未提交的 Edge Peek 工作隔离。
