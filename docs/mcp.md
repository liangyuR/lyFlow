# MCP 服务

`@lyflow/mcp`（源码在 [`packages/mcp/`](../packages/mcp/)）把 LyFlow 包成一个 stdio 的
[MCP](https://modelcontextprotocol.io) 服务，让只拿得到工具、拿不到仓库源码的 Agent
能读算子、改图、跑图、批量评估。

一句话：**它是 [HTTP 传输契约](http-transport.md) 的又一个消费方，不是第四种传输。**
描述、校验、执行这些走 `/lyflow/*`（test-server 或阶段 B 的业务服务都行）；
`eval` / `perturb` / `diff_graphs` / `get_params` / `patch_graph` / `list_recipes` 起本地 `lyflow` 可执行文件
（ADR-0020：它们是 CLI 子命令，MCP 只是另一张皮）。
理由见 [ADR-0021](adr/0021-mcp-as-transport-consumer.md)。

```
Agent ── stdio ── @lyflow/mcp ──┬── HTTP + WS ── /lyflow/*（test-server 或业务服务）── core
                                └── spawn ────── lyflow.exe eval / perturb / diff /
                                                            params / patch / recipes ─ core
```

---

## 起它

### 1. 后端

开发期用仓库自带的桩服务器（每个请求起一次 CLI，零依赖）：

```bash
node packages/editor/test-server/server.mjs --port 8787 \
     --root <工作区根> --cli bridge/target/debug/lyflow.exe
```

`--root` 是工作区根：`baseDir` 这类相对路径都从它开始解析，逃出去的路径服务端会拒。
阶段 B 的业务服务实现同一份契约，把 `LYFLOW_HTTP_BASE` 指过去即可，MCP 这边不用改。

### 2. 构建 MCP

```bash
pnpm install
pnpm --filter @lyflow/mcp build     # 产物 packages/mcp/dist/index.js
```

### 3. 环境变量

| 变量 | 必填 | 含义 |
|---|---|---|
| `LYFLOW_HTTP_BASE` | 是 | `/lyflow/*` 的基址，例如 `http://127.0.0.1:8787`。缺了进程直接退出并说明 |
| `LYFLOW_HTTP_TOKEN` | 否 | 有它就每个请求带 `Authorization: Bearer`，WebSocket 走 `lyflow-token.<token>` 子协议 |
| `LYFLOW_CLI` | 否 | 本地 `lyflow.exe` 路径。`eval` / `perturb` / `diff_graphs` / `get_params` / `patch_graph` 用它；缺了这五个工具返回一句说得清的错，其余工具不受影响 |
| `LYFLOW_PACKS` | 否 | 透传给 CLI 子进程。注意它在当前实现里是**构建期**变量（`scripts/build-core.ps1` 用它选算子包），运行期的 exe 已经带着自己那份算子表 |
| `LYFLOW_WORK_DIR` | 否 | `eval` / `perturb` 的逐行结果落盘目录，默认 `os.tmpdir()/lyflow-mcp` |

### 4. 在 Claude Code 里配

仓库根的 `.mcp.json`：

```json
{
  "mcpServers": {
    "lyflow": {
      "command": "node",
      "args": ["D:/project/LyFlow/packages/mcp/dist/index.js"],
      "env": {
        "LYFLOW_HTTP_BASE": "http://127.0.0.1:8787",
        "LYFLOW_CLI": "D:/project/LyFlow/bridge/target/debug/lyflow.exe",
        "LYFLOW_PACKS": "gap"
      }
    }
  }
}
```

也可以走包里的 `bin`：`pnpm --filter @lyflow/mcp exec lyflow-mcp`，等价于 `node dist/index.js`。

---

## 工具

输出**一律是压紧的 JSON 文本**，且刻意裁过：Agent 每一次调用都在花上下文，
点云、逐行结果这些大东西要么变成统计量，要么落盘给路径。

| 工具 | 输入 | 底下是什么 | 返回 |
|---|---|---|---|
| `list_operators` | `pack?` `category?` `query?` | `GET /lyflow/manifest`（进程内缓存，`core-info.generation` 变了才重拉） | `{count, total, operators:[{id,label,category,pack?,doc}]}`，`doc` 只有第一句 |
| `get_operator` | `id` | 同上 | 全量 `OperatorDesc`（含每个参数的 `doc` 与端口契约）；找不到时 `{error, nearest:[三个最接近的 id]}` |
| `list_port_types` | — | 同上 | `{types:[…]}` |
| `validate_graph` | `graph \| graphPath` `baseDir?` | `POST /lyflow/validate` | `{diagnostics:[…], ok}` |
| `plan_graph` | 同上 + `targets?` | `POST /lyflow/plan` | `{plan:[{nodeId,cacheKey,cached,level,upstreamMissing,bypass,lazy,demandedBy}]}`，`lazy` = 只被惰性端口依赖、主路径成功时不跑 |
| `run_graph` | 同上 + `targets?` `set?` `recipe?` `mode?` `timeoutMs?` | `POST /lyflow/run` + WS 等该 `runId` 的 `run_finished`；给了 `recipe` 先 `LYFLOW_CLI recipes --recipe` | core 的 run summary + `runId` / `runStatus` / `diagnostics`（见下），带 `recipe` 时另有 `recipe` |
| `get_node_outputs` | `runId` `nodeId` | `GET /lyflow/runs/:id/nodes/:node/outputs` | `{outputs:[OutputInfo]}` 原样 |
| `summarize_output` | `runId` `nodeId` `port` `maxPoints?` `head?` | 点云走 `GET …/clouds/:node/:port`，其余用 `OutputInfo.value` | 见下 |
| `eval` | `graphPath` 样本 参数 `metric[]` `recipe?` … | `LYFLOW_CLI eval …` | 压紧的统计（`compact`，默认开）+ 失败样本清单 + `rowsPath` |
| `perturb` | `graphPath` `after` `region` `axis` `metric[]` … | `LYFLOW_CLI perturb …` | `perturb_summary` 数组 + 不通过样本 + `samplesPath` + `rowsPath` |
| `diff_graphs` | `a` `b` | `LYFLOW_CLI diff a b --json` | `{exitCode, diff}` 原样 |
| `get_params` | `graphPath` `node[]?` `only?` `set[]?` `recipe?` `baseDir?` | `LYFLOW_CLI params … --json` | `{exitCode, argv, count, params:[{node,op,param,value,source,graphParam?,unit?,min?,max?}], stderrTail}` |
| `list_recipes` | `graphPath` | `LYFLOW_CLI recipes <graph> --json` | `{dir, exists, default, count, recipes:[{name,file,default,values,runnable,blocking,mismatches,items}], problems}`（见下） |
| `patch_graph` | `graphPath` `removeNode[]?` `addNode[]?` `rewire[]?` `set[]?` `dryRun?` `out?` | `LYFLOW_CLI patch … --json` | `patch_result` 摊平：`{exitCode, argv, applied, noops, wrote, diff, stderrTail}` |

### 图怎么给

`graph`（内联 GraphDoc）与 `graphPath`（本地图文件，MCP 进程读它）二选一。
**MCP 不写图文件** —— 改完的图由 Agent 自己用文件系统存，
唯一的例外是 `patch_graph` 且显式给了 `dryRun: false`（见下）。

### `patch_graph`：`dryRun` 默认 **true**

改图结构的四个动作（[ADR-0023](adr/0023-patch-as-idempotent-structural-edit.md)），
顺序定死 remove → add → rewire → set，与传参顺序无关。默认只算差异不写文件；
要真写就给 `dryRun: false`，写到别的路径再加 `out`（`out` 必须配 `dryRun: false`，
否则当场报错 ——「我以为它写了」是这套工具里最贵的误解）。

幂等：删不存在的 id、左端口没有出边的 rewire、同值的 set 都进 `noops[]`
（`{action, spec, reason, message}`），这时 `applied` 全空、`diff.empty` 为 true、`wrote` 是 `null`。
改完不合法时整体不写，返回 `{error, exitCode: 1, diagnostics: […]}`，诊断是 core 的原话。

`graphPath` 是给 MCP 进程读的本地路径；`baseDir` 是给**后端**解析相对路径参数的目录，
相对后端工作区根。给了 `baseDir` 就以它为准；没给而 `graphPath` 是相对路径时拿它当基准；
`graphPath` 是绝对路径又没给 `baseDir` 时，基准是后端的工作区根。

送出去之前 MCP 会查一遍 GraphDoc 的必填字段（`schemaVersion` / `id` / `nodes` / `edges`，
每个节点的 `id` 与 `op`）。少了当场报，不然桩服务器会把「图连结构都不合法」压成一个空诊断数组。

### `run_graph` 的返回

返回值**就是 core 的 run summary**（[ADR-0022](adr/0022-run-summary-as-core-output.md)），
MCP 只在外面套了 `runId` / `runStatus` / `diagnostics`。
`status` / `nodes` / `outputs` / `decisions` / `contractViolations` 原样来自 core ——
这一层一个字都不重建。

```jsonc
{"runId":"01M2K43FJE4C9NJ7QEYHXP89WW",
 "status":"degraded", "runStatus":"ok", "durationMs":2.03,
 "outputs":{
   "gap":   {"state":"value","node":"n_gap","port":"gap","type":"Measurement",
             "elementCount":1,"value":{"kind":"Measurement","value":3.52,"ok":true,"unit":"mm"}},
   "flush": {"state":"inactive","node":"b_n_flush","port":"flush","reason":"not_demanded"},
   "bundle":{"state":"failed","node":"n_bundle","port":"bundle",
             "from":"n_fb_line","code":"insufficient_points",
             "root":"n_fit_datum","rootCode":"insufficient_points"}},
 "nodes":{"n_fit_datum":{"state":"error","code":"insufficient_points",
                         "durationMs":2.1,"outputsAvailable":false},
          "n_gap":{"state":"done","durationMs":0.46,"cached":true,"outputsAvailable":true}},
 "decisions":{"n_fb_line":{"choice":"b","reason":"io: 主路径没有产出",
                           "port":"choice","type":"FallbackChoice"}},
 "contractViolations":[],
 "diagnostics":[]}
```

- **`status` 三态**：`ok`（零个节点出错）/ `degraded`（有节点坏了，但每一维声明输出
  要么拿到了值、要么本来就不要）/ `failed`（有一维本该有却崩了）。
  另有两个只属于这一层的值：校验没过（后端 400）时是 `invalid`，`runId` 为 `null`、
  `diagnostics` 里是诊断；等不到 `run_finished` 时是 `timeout`（默认等 300 s，`timeoutMs` 可改）。
- **`runStatus`** 是 `run_finished` 那个 `ok` / `error` / `cancelled`，与 `status` 不是一回事，
  所以两个都留着：带 fallback 的图「主路径炸了、备用接住了」是
  `runStatus: ok` + `status: degraded`。**判成败读 `status`。**
- **`outputs` 每一维三态**：`value` / `inactive`（这一维本来就没有，`reason` 说明为什么）/
  `failed`（本该有、崩了，`from` 是沿边回溯到的**最近**的出错节点，`root` 是沿失败链继续
  往上追到的、拓扑序最早的那个）。带 fallback 的图里 `from` 常常是 fallback 自己，
  要查的根因看 `root`。别把 `inactive` 当失败 —— 它就是「这个点位不量这一维」。
  `node` / `port` 照旧给着，直接拿去喂 `summarize_output`。
- **`decisions`** 是全图每一个 `FallbackChoice`，条数等于图里 `flow.fallback` + `flow.select`
  的节点数。按 Record 的类型收，不按算子 id。
- `nodes` 是 `id → 收尾状态` 的**对象**（M5 那一版是数组）。`state=skipped` 不代表没有输出，
  看 `outputsAvailable`；`reason=not_demanded` 才是真的什么都没有。
  节点的完整诊断列表仍在事件流里，summary 每个节点只给一个 `code`。
- 图级输出是点云时不带 `value`，**这里永远不返回点云本身**。
- 接的是老 core（ABI < v9）时没有 summary，返回退回 M5 那套形状：`nodes` 是数组、
  没有 `decisions` / `contractViolations`、`status` 等于 `runStatus`。
- `set` 的语义与 CLI `--set` 完全一样：键是 `<节点>.<参数>`，在发给后端之前改 doc。
  与 `--set` 的差别只有一处：值是已经解析好的 JSON 值，不用再写成字符串。

### 配方：`recipe` 与 `list_recipes`

配方是顶层图参数的一组取值，存在图旁的 `<图名>.recipes/<名字>.lyflow-recipe.json`（[recipe.md](recipe.md)）。
MCP 只读配方，**不提供写配方的工具**（param-recipe P4.2）—— 配方由工程师在编辑器里维护。

- **`list_recipes { graphPath }`**：图旁目录里的每个配方。`runnable: false` 表示有 ①–③ 失配、不能跑；`mismatches` 是四类
  各几条（`extra` 多出 / `type` 类型不符 / `range` 越界 / `spec` 规格变了，最后一类只提示）；`items` 每条
  `{kind, param, message, fixLabel}`，与编辑器「配方管理」页、CLI 的 stderr 同一套用语；`default` 是 `index.json` 的默认配方。

  ```jsonc
  {"dir":"D:/work/车门缝隙.recipes","exists":true,"default":"车型A","count":2,
   "recipes":[
     {"name":"车型A","file":"D:/work/车门缝隙.recipes/车型A.lyflow-recipe.json","default":true,"values":2,
      "runnable":true,"blocking":0,"mismatches":{"extra":0,"type":0,"range":0,"spec":0},"items":[]},
     {"name":"坏","file":"…/坏.lyflow-recipe.json","default":false,"values":2,"runnable":false,"blocking":2,
      "mismatches":{"extra":1,"type":0,"range":1,"spec":0},
      "items":[{"kind":"range","param":"count","message":"不能小于 1","fixLabel":"夹到限位：1"},
               {"kind":"extra","param":"nope","message":"图里没有图参数 nope（改名或删掉了？）","fixLabel":"删除这个值"}]}],
   "problems":[], "graphId":"…", "specDigest":"sha256:…"}
  ```

- **`run_graph { …, recipe }`**：按这个配方跑，与 `lyflow run --recipe` 是同一个结果（验收 28 在冒烟测试里对过点数）。
  MCP 先起 `lyflow recipes <图> --recipe <文件> --json` 查失配、取合成好的图参数取值，再经 HTTP 信封的 `params` 交给后端
  （判定只有 `bridge/src/recipe.rs` 一份）。内联的 `graph` 也行，MCP 把它落成工作目录里的临时文件给 CLI。
  有 ①–③ 时**不碰后端**，返回错误 `{error: "配方「坏」有 2 处失配，不能运行", recipe: {name, file, blocking, items}}`；
  跑成了返回里多一个 `recipe: {name, file, values, warnings?}`，`warnings` 是 ④ 的提示。需要 `LYFLOW_CLI`。
- **`get_params { …, recipe }`**：「按这个配方跑时每个参数的生效值」，被图参数写入的行 `source: "graph"`、另带 `graphParam`；
  失配时 CLI 退出码 4，工具报错并带回 stderr（条目在里面）。`only` 多了 `"graph"`。
- **`eval { …, recipe }`**：透传 `--recipe`，作用于所有样本；叠加顺序 基础 → 配方 → `params` 里的参数组 → `param`。

### `summarize_output` 的返回

点云（`maxPoints` 是抽稀上限，`0` 表示不抽稀；`bbox` 用的是**全量**点算的包围盒）：

```jsonc
{"runId":"…","nodeId":"voxel","port":"cloud","type":"PointCloud","elementCount":5779,
 "maxPoints":200000,"kind":"cloud","pointCount":5779,"totalPoints":5779,
 "bbox":{"min":[-1.4917,-1.4766,-0.3494],"max":[1.4885,1.4646,1.1342]},
 "channels":{"x":{"min":-1.4917,"max":1.4885,"mean":-0.0038},
             "y":{…},"z":{…},"intensity":{"min":0.0040,"max":0.9987,"mean":0.4149}},
 "head":[{"x":-0.8474,"y":-0.5459,"z":0.0029,"intensity":0.2725}]}
```

带法线时 `channels` 多 `nx` / `ny` / `nz`，`head` 的每个点多一个 `normal`。
统计全在 MCP 进程里算，core 一行没改。

其余类型：`Tensor` 给 `{kind:"tensor", shape, count, min, max, mean}`（张量数据本来就不进 IPC，
只有形状与统计量）；`Indices` 给 `{kind:"indices", count}`（这一版契约只有点云有二进制端点，
取不到逐个下标）；`Measurement` 这类小值给 `{kind:"value", value}` 原样。

### `eval` / `perturb` 的样本集

样本集三选一，与 CLI 一一对应（[agent-tuning.md](agent-tuning.md) §3）：
`samplesPath`（手写的 JSON Lines）、`samplesGlob` + `bind`（单文件一步生成）、
`samplesDir` + `bindPair`/`bind` + `pattern`（一帧一目录，工具自己配对）。
后者还认 `sampleSubdir`、`sortBy`（`name` / `mtime`）、`splitHalf`：

```jsonc
{"graphPath":"…/4.lyflow.json",
 "samplesDir":"…/sensor","sampleSubdir":"4",
 "bindPair":"n_load.primaryFile,n_load.secondaryFile",
 "pattern":"*Master*.pcd,*Slave*.pcd","splitHalf":"half",
 "set":["n_load.source=files"],"metric":["outputs.gap"],"holdout":"half=b"}
```

每个 glob 在一帧里要**恰好匹配到一个**文件，否则 `exitCode` 4 并在 `stderr` 里说是哪一帧。

CLI 的 `--samples-jsonl-out` 与 `--parallel` **MCP 不提供**，逐条对照见
[agent-tuning.md](agent-tuning.md) §7。

### `eval` 的返回

默认 `compact: true`，每个（参数组 × 指标 × 分组）压成一行：

```jsonc
{"exitCode":0,"timedOut":false,
 "argv":["eval","…/4.lyflow.json","--samples-dir","…","--metric","outputs.gap","--holdout","half=b"],
 "compact":true,
 "summaries":[{"paramSet":0,"params":{},"metric":"outputs.gap","group":"holdout",
               "n":25,"ok":25,"mean":1.04935,"std":0.05318},
              {"paramSet":0,"params":{},"metric":"outputs.gap","group":"train",
               "n":26,"ok":26,"mean":1.08380,"std":0.07507}],
 "rowCount":51,"rowsPath":"…/lyflow-mcp/eval-…/rows.jsonl",
 "csvPath":"…/p4.csv",
 "failureCount":0,"failures":[],"failuresTruncated":false,
 "stderrTail":"1 组参数 × 51 个样本 = 51 次运行，51 次 ok"}
```

- `compact` 的字段是 `{paramSet, params, metric, group, n, ok, failCodes（非空才带）, mean, std}`。
  `compact: false` 回原样的 `eval_summary`（`groups` 嵌套，带 `min` / `max` / `p2p`）。
  8 组参数 × 5 个指标 × 2 组在盲测里是 31 KB，压紧之后是它的几分之一。
- 逐行的 `eval_row` **不进返回值**，整份写在 `rowsPath` 指的 JSON Lines 文件里。
  51 帧 × 8 组参数就是 408 行，那是给 `jq` 看的，不是给上下文窗口看的。
- `csv` 给了路径就原样透给 CLI 的 `--csv`，返回里回一个 `csvPath`。
- `failures` 是状态不是 `ok` 的样本，最多 `failuresLimit` 条（默认 20，`0` 表示一条都不回、
  只给 `rowsPath`），超了 `failuresTruncated` 为 `true`。每条带
  `{sample, paramSet, status, errors, summaryStatus, outputs}` ——
  后两个来自那次运行的 run summary（[ADR-0022](adr/0022-run-summary-as-core-output.md)），
  `outputs` 是每一维的三态。**没有它就得回头翻 `rowsPath` 才分得清
  「这一维本来就没有」和「本该有、崩了」。** `rowsPath` 里每行也带完整的 `summary`
  （CLI 的 `--no-summary` 可以关掉，MCP 这边不提供这个开关 —— 落盘不占上下文）。
- **读 summary 的顺序是先 `ok/n` 与 `failCodes`，再 `std`**，理由见
  [agent-tuning.md](agent-tuning.md) §3。
- 退出码 4（用法错，比如指标路径拼错）或者根本起不来时，额外带一个 `stderr` 字段放**全文** ——
  那里面有「这张图上可用的标量路径」这样必须看全的东西。其余情况只给 `stderrTail`。

### `perturb` 的返回

同构，另有一个 `samplesPath`：

```jsonc
{"exitCode":2,"timedOut":false,"argv":[…],
 "summaries":[{"kind":"perturb_summary","metric":"outputs.gap","axis":"x=-0.0003:0.0003:5",
               "expect":1000,"tolerance":100,"samples":51,"pass":49,
               "slopeMean":983.06,"slopeStd":39.44,"slopeMin":795.93,"slopeMax":1060.17,
               "nonResponsive":0,"signFold":0}],
 "sampleCount":51,
 "samplesPath":"…/lyflow-mcp/perturb-…/samples.jsonl",
 "rowsPath":"…/lyflow-mcp/perturb-…/rows.jsonl",
 "failureCount":2,"failures":[{"kind":"perturb_sample","sample":"…","n":5,"slope":795.93,
                               "slopeNeg":990.91,"slopePos":990.91,"pass":false}],
 "failuresTruncated":false,
 "stderrTail":"在 n_frame_s:cloud 之后插入 __perturb；5 个位移 × 51 个样本 = 255 次运行"}
```

- **`samplesPath` 里是全量的 `perturb_sample`**，每样本每指标一行，不受 `failuresLimit` 影响。
  「不响应的帧是不是缝位置偏得最远的那些」这类诊断要读它 —— 只看被截断的 `failures` 做不完。
- `failures` 是 `pass:false` 的那些，最多 `failuresLimit` 条（默认 20，`0` 表示只给路径）。
- `rowsPath` 里是全量的 `perturb_row` + `perturb_sample` + summary。
- `region` 的 `point` / `min` / `max` 与 `axis` 的位移都是**米**；`outputs.*` 是**毫米**，
  所以 `expect` 常写 ±1000。`after` 要插在被图里 `camera` 参数选中的那一路相机之后
  （[agent-tuning.md](agent-tuning.md) §4）。

---

## resources

| URI | 是什么 |
|---|---|
| `lyflow://manifest` | 后端当前这一份完整 `OperatorManifestBundle` |
| `lyflow://schema/operator-manifest` | `schema/operator-manifest.schema.json` |
| `lyflow://schema/graph-doc` | `schema/graph-doc.schema.json` —— 手写图之前先看它 |
| `lyflow://schema/execution-event` | `schema/execution-event.schema.json` |
| `lyflow://examples/graph` | `schema/examples/graph.example.lyflow.json` |
| `lyflow://docs/agent-tuning` | [agent-tuning.md](agent-tuning.md) |
| `lyflow://docs/http-transport` | [http-transport.md](http-transport.md) |
| `lyflow://packs/<name>/readme` | `packs/<name>/README.md`，可枚举 |

除 `lyflow://manifest` 以外都读仓库里的文件（沿着模块路径往上找 `schema/operator-manifest.schema.json`
定位仓库根）。把 `dist/` 单独拷到仓库外跑时这些 resource 会返回一句说清楚的错，工具不受影响。

---

## 不做的事

- **读写图文件。** Agent 有文件系统，`graphPath` 只用来读一份给后端，写回是 Agent 自己的事。
- **点云可视化。** 只给统计量；要图片用编辑器的 PNG 导出。
- **任何自动调参。** 真实任务里的瓶颈是判断不是算力（单次 run 0.05 s，上万次也就几分钟），
  平台把「测的是不是那条缝」变成可审计的一步，不替人拍板（m5-plan G13）。
- **编辑器 UI。** MCP 与编辑器是同一份契约的两个消费方，互不依赖 ——
  `@lyflow/mcp` 不依赖 `@lyflow/editor`（那边带 React）。
- **第四种传输语义。** 没有新的端点、新的事件、新的错误码。契约变了，改的是
  [http-transport.md](http-transport.md)，不是这里。

## 测试

```bash
pnpm --filter @lyflow/mcp test
```

`node:test`。不需要后端的那些（输入 schema、`set` 应用到 doc、eval/perturb 的 argv 映射、
点云统计、JSON Lines 解析容错）总是跑；集成冒烟自己起 test-server 与 MCP 子进程，跑
`list_operators → get_operator → validate_graph → run_graph → get_node_outputs → summarize_output`，
另一条走配方：`list_recipes → run_graph recipe`（点数与 `lyflow run --recipe --outputs` 相同）→ 失配的配方被拦 → 内联图带 recipe →
`get_params recipe`。没有 `bridge/target/debug/lyflow.exe` 时它们 skip 并打出原因。`pnpm check` 里带着这一步。
