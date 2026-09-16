# MCP 服务

`@lyflow/mcp`（源码在 [`packages/mcp/`](../packages/mcp/)）把 LyFlow 包成一个 stdio 的
[MCP](https://modelcontextprotocol.io) 服务，让只拿得到工具、拿不到仓库源码的 Agent
能读算子、改图、跑图、批量评估。

一句话：**它是 [HTTP 传输契约](http-transport.md) 的又一个消费方，不是第四种传输。**
描述、校验、执行这些走 `/lyflow/*`（test-server 或阶段 B 的业务服务都行）；
`eval` / `perturb` / `diff_graphs` 起本地 `lyflow` 可执行文件（ADR-0020：它们是 CLI 子命令，
MCP 只是另一张皮）。理由见 [ADR-0021](adr/0021-mcp-as-transport-consumer.md)。

```
Agent ── stdio ── @lyflow/mcp ──┬── HTTP + WS ── /lyflow/*（test-server 或业务服务）── core
                                └── spawn ────── lyflow.exe eval / perturb / diff ── core
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
| `LYFLOW_CLI` | 否 | 本地 `lyflow.exe` 路径。`eval` / `perturb` / `diff_graphs` 用它；缺了这三个工具返回一句说得清的错，其余工具不受影响 |
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
| `get_operator` | `id` | 同上 | 全量 `OperatorDesc`，**含 `preconditions`**；找不到时 `{error, nearest:[三个最接近的 id]}` |
| `list_port_types` | — | 同上 | `{types:[…]}` |
| `validate_graph` | `graph \| graphPath` `baseDir?` | `POST /lyflow/validate` | `{diagnostics:[…], ok}` |
| `plan_graph` | 同上 + `targets?` | `POST /lyflow/plan` | `{plan:[{nodeId,cacheKey,cached,level,upstreamMissing,bypass}]}` |
| `run_graph` | 同上 + `targets?` `set?` `mode?` `timeoutMs?` | `POST /lyflow/run` + WS 等该 `runId` 的 `run_finished` | `{runId,status,durationMs,outputs,nodes,diagnostics}` |
| `get_node_outputs` | `runId` `nodeId` | `GET /lyflow/runs/:id/nodes/:node/outputs` | `{outputs:[OutputInfo]}` 原样 |
| `summarize_output` | `runId` `nodeId` `port` `maxPoints?` `head?` | 点云走 `GET …/clouds/:node/:port`，其余用 `OutputInfo.value` | 见下 |
| `eval` | `graphPath` 样本 参数 `metric[]` … | `LYFLOW_CLI eval …` | 压紧的统计（`compact`，默认开）+ 失败样本清单 + `rowsPath` |
| `perturb` | `graphPath` `after` `region` `axis` `metric[]` … | `LYFLOW_CLI perturb …` | `perturb_summary` 数组 + 不通过样本 + `samplesPath` + `rowsPath` |
| `diff_graphs` | `a` `b` | `LYFLOW_CLI diff a b --json` | `{exitCode, diff}` 原样 |

### 图怎么给

`graph`（内联 GraphDoc）与 `graphPath`（本地图文件，MCP 进程读它）二选一。
**MCP 不写图文件** —— 改完的图由 Agent 自己用文件系统存。

`graphPath` 是给 MCP 进程读的本地路径；`baseDir` 是给**后端**解析相对路径参数的目录，
相对后端工作区根。给了 `baseDir` 就以它为准；没给而 `graphPath` 是相对路径时拿它当基准；
`graphPath` 是绝对路径又没给 `baseDir` 时，基准是后端的工作区根。

送出去之前 MCP 会查一遍 GraphDoc 的必填字段（`schemaVersion` / `id` / `nodes` / `edges`，
每个节点的 `id` 与 `op`）。少了当场报，不然桩服务器会把「图连结构都不合法」压成一个空诊断数组。

### `run_graph` 的返回

```jsonc
{"runId":"01M2K43FJE4C9NJ7QEYHXP89WW","status":"ok","durationMs":2.03,
 "outputs":{"thinned":{"node":"voxel","port":"cloud","type":"PointCloud","elementCount":5779}},
 "nodes":[{"id":"gen","state":"done","outputsAvailable":true,
           "durationMs":0.46,"elementCount":8000,"errors":[]}],
 "diagnostics":[]}
```

- `status` 是 `ok` / `error` / `cancelled`，另有两个只属于这一层的值：
  校验没过（后端 400）时是 `invalid`，`runId` 为 `null`、`diagnostics` 里是诊断；
  等不到 `run_finished` 时是 `timeout`（默认等 300 s，`timeoutMs` 可改）。
- `nodes[].state=skipped` **不代表没有输出**，看 `outputsAvailable`。
- `outputs` 里给了 `node` / `port`，就是为了能直接拿去喂 `summarize_output`。
- 图级输出是点云时不带 `value`，**这里永远不返回点云本身**。
- `set` 的语义与 CLI `--set` 完全一样：键是 `<节点>.<参数>`，在发给后端之前改 doc。
  与 `--set` 的差别只有一处：值是已经解析好的 JSON 值，不用再写成字符串。

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
  只给 `rowsPath`），超了 `failuresTruncated` 为 `true`。
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
没有 `bridge/target/debug/lyflow.exe` 时它 skip 并打出原因。`pnpm check` 里带着这一步。
