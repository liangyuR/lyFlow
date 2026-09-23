# HTTP 传输契约

`@lyflow/editor` 的 `HttpTransport` 与后端之间的协议。**阶段 B 的业务服务照这一份实现。**

一句话：`Transport` 接口的每个方法对着 C ABI v10 的一个入口，这里再对着一个 REST 端点。
三方一一对应，见下面的对照表。v7 的 ABI 清单在 [phase-a1-acceptance.md](phase-a1-acceptance.md#c-abi-v7-的最终签名清单)，
v8 加的两个取数入口见 [ADR-0019](adr/0019-output-tensor-and-indices-over-abi.md)，
v9 加的 `lyflow_run_summary` 见 [ADR-0022](adr/0022-run-summary-as-core-output.md)。
v10 在 `lyflow_run_options` 末尾加的 `params_json`（顶层图参数取值）见 [embedding.md](embedding.md#顶层图参数)。

- **基址**：构造时给的 `baseUrl`，例如 `http://127.0.0.1:8787`。所有路径都挂在 `/lyflow/` 下。
- **编码**：请求体与响应体都是 `application/json; charset=utf-8`，
  **点云、张量、下标三个端点除外**（`application/octet-stream`）。
- **鉴权**：给了 `token` 时每个 HTTP 请求带 `Authorization: Bearer <token>`；
  WebSocket 走子协议（见 [事件流](#事件流websocket)）。没给 token 时两边都不带。
- **错误**：非 2xx 的响应体是 `{"error": "给人看的一句话"}`；`HttpTransport` 把它抛成 `Error`。
- **不做的事**：这份契约里没有分页、没有游标、没有 ETag。图和事件都不大，
  真正大的那一样（点云）走二进制。

参考实现：[`packages/editor/test-server/`](../packages/editor/test-server/)（Node，零依赖，起 `lyflow` CLI）。
客户端这一侧除了 `@lyflow/editor`，还有 [`packages/mcp`](../packages/mcp/) —— 给 Agent 用的 MCP 服务，
按这一份独立实现了一遍（[mcp.md](mcp.md)、[ADR-0021](adr/0021-mcp-as-transport-consumer.md)）。
所以这份契约有两个互不依赖的消费方盯着，写歪了会有人先撞上。

---

## 端点清单

| 方法 | 路径 | Transport 方法 | C ABI |
|---|---|---|---|
| GET | `/lyflow/manifest` | `getManifest` | `lyflow_manifest_json` |
| GET | `/lyflow/core-info` | `getCoreInfo` | `lyflow_version` + manifest 统计 |
| POST | `/lyflow/validate` | `validateGraph` | `lyflow_validate` |
| POST | `/lyflow/plan` | `planGraph` | `lyflow_plan` |
| POST | `/lyflow/run` | `runGraph` | `lyflow_run_start` |
| POST | `/lyflow/cancel` | `cancelRun` | `lyflow_run_cancel` |
| GET | `/lyflow/runs/:runId/outputs` | `getRunOutputs` | `lyflow_run_outputs` |
| GET | `/lyflow/runs/:runId/summary` | —（编辑器从 `run_finished` 事件里读） | `lyflow_run_summary` |
| GET | `/lyflow/runs/:runId/nodes/:nodeId/outputs` | `getOutputInfo` | `lyflow_output_info` |
| GET | `/lyflow/runs/:runId/clouds/:nodeId/:port` | `getOutputCloud` | `lyflow_output_cloud` |
| GET | `/lyflow/runs/:runId/tensors/:nodeId/:port?offset=&count=` | `getOutputTensor` | `lyflow_output_tensor` |
| GET | `/lyflow/runs/:runId/indices/:nodeId/:port?offset=&count=` | `getOutputIndices` | `lyflow_output_indices` |
| GET | `/lyflow/cache` | `cacheStats` | `lyflow_cache_stats` |
| DELETE | `/lyflow/cache` | `clearCache` | `lyflow_cache_clear` |
| GET | `/lyflow/library` | `getLibraryStatus` | `lyflow_library_count` |
| POST | `/lyflow/library/refresh` | `refreshLibrary` | `lyflow_set_library_dirs` |
| POST | `/lyflow/library/save` | `saveAsLibrary` | 桥接层的事，core 不参与 |
| POST | `/lyflow/import` | `importGraph` | `lyflow_import` |
| GET / PUT | `/lyflow/files/graph?path=` | `loadGraph` / `saveGraph` | 桥接层 |
| GET / PUT / DELETE | `/lyflow/files/backup?path=` | `readBackup` / `writeBackup` / `discardBackup` | 桥接层 |
| GET | `/lyflow/files/backup/status?path=` | `backupStatus` | 桥接层 |
| PUT | `/lyflow/files/bytes?path=` | `writeFileBytes` | 桥接层 |
| GET / POST | `/lyflow/recent` | `getRecentFiles` / `pushRecentFile` | 桥接层 |
| WS | `/lyflow/events` | `onExecutionEvent` 等三个 | `lyflow_event_cb` |

`path` 查询参数是**相对工作区根**的路径。服务端必须拒绝逃出工作区的路径（返回 403）。

---

## 描述

### `GET /lyflow/manifest`

返回一整份 `OperatorManifestBundle`，schema 见 [`schema/operator-manifest.schema.json`](../schema/operator-manifest.schema.json)。
包含 v7 新增的顶层 `importers` 段。

### `GET /lyflow/core-info`

```json
{ "version": "0.1.0", "operatorCount": 45, "typeCount": 13,
  "generation": 0, "hotReload": false }
```

`generation` 是热重载换代计数（ADR-0009）。后端不支持热重载就恒为 0、`hotReload: false`。

---

## 校验、计划、执行

三个都收同一个信封：

```json
{ "doc": { /* GraphDoc */ }, "graphPath": "sub/dir/graph.lyflow.json" }
```

`graphPath` 可以是 `null`。它**只**决定相对路径参数的基准目录，不表示后端要去读那个文件。

### `POST /lyflow/validate` → `GraphDiagnostic[]`

图合法时返回 `[]`。诊断的字段见 [`schema/execution-event.schema.json`](../schema/execution-event.schema.json) 的 `diagnostic`，
多带 `nodeId`、`severity`、可选 `kind`（`"diagnostic"` / `"migration"`）。

### `POST /lyflow/plan` → `PlanNode[]`

信封多一个 `targets: string[] | null`（只编译到这些节点的上游闭包）。

```json
[{ "nodeId": "n1", "cacheKey": "976b7c…", "cached": false,
   "level": 0, "upstreamMissing": false, "bypass": false,
   "lazy": false, "demandedBy": [] }]
```

`lazy` 为 true 表示这个节点只被惰性端口依赖（ADR-0016），主路径成功时它一次都不跑；
`demandedBy` 是管着它的那些惰性端口，写成 `<节点>:<端口>`。闭包深处的节点沿着惰性节点
往下继承，所以一条链上的每一个都指得回那个真正的闸门。

与 `lyflow_plan` 一样：**校验没过时返回的是诊断数组而不是计划数组**，
靠有没有 `cacheKey` 区分（ADR-0007）。`HttpTransport` 认出诊断数组时给 UI 返回 `[]`。

### `POST /lyflow/run` → `{ "runId": "01M1VR…" }`

信封：

```json
{ "doc": {}, "graphPath": null, "targets": ["n1"] ,
  "mode": "full", "previewMaxPoints": null, "previewBudgetMs": null,
  "sceneId": null }
```

`mode` 是 `"full"` 或 `"preview"`（ADR-0011：预览模式下源算子先抽稀）。

`sceneId` 是**可选**的宿主扩展：宿主页面上已经加载好一对点云时，把它的会话 id 带上，
后端就把那对云注入到起始算子的输入端口，而不是让图自己按参数去读盘。不带（或 `null`）
就是原来的行为。会话不存在时返回 404，图里没有可注入的节点时返回 400 —— 两者都要能和
「运行失败」区分开，因为用户能做的动作完全不同（重新读一次点云 / 改图）。宿主自己决定
「一对点云」对应哪个算子的哪个端口；编辑器只负责把 id 透传下去。

**响应必须在第一条事件之后才发出**，因为 runId 是后端分配的，前端要拿它认领事件。
事件可以比响应先到：编辑器会把认不出的事件先攒着，`runId` 一回来就补上。

校验没过（没有产生任何事件）时返回 400，`error` 里放诊断数组的 JSON 文本。

### `POST /lyflow/cancel`

```json
{ "runId": "01M1VR…" }
```

返回 `{}`。取消是**尽力**的：后端必须保证这次运行最终发出一条
`run_finished`（`status: "cancelled"` 或已经跑完了的真实状态）。

---

## 结果

### `GET /lyflow/runs/:runId/outputs` → `RunOutputs`

`lyflow_run_outputs` 的原样转发：名字 → 该端口的元信息。

```json
{ "thinned": { "node": "voxel", "port": "cloud", "type": "PointCloud",
               "elementCount": 4321, "byteSize": 69136 },
  "gap": { "node": "n_gap", "port": "gap", "type": "Measurement",
           "elementCount": 1, "byteSize": 0,
           "value": { "kind": "Measurement", "value": 5.7031, "unit": "mm" } } }
```

图没声明 `outputs` 时是 `{}`；端口没有结果时那一项带 `"missing": true`。
**`missing` 分不开「本来就没有」与「本该有、崩了」** —— 要分开读下面那个端点。

### `GET /lyflow/runs/:runId/summary` → `RunSummary`

`lyflow_run_summary` 的原样转发（[ADR-0022](adr/0022-run-summary-as-core-output.md)）。
与 `run_finished` 事件里那个 `summary` 字段是同一个对象。

```json
{ "runId": "01M2…", "status": "degraded", "durationMs": 31.2,
  "nodes": { "n_fit_datum": { "state": "error", "code": "insufficient_points",
                              "durationMs": 2.1, "outputsAvailable": false } },
  "outputs": { "gap": { "state": "value", "node": "n_gap", "port": "gap",
                        "type": "Measurement", "elementCount": 1,
                        "value": { "kind": "Measurement", "value": 3.52, "unit": "mm" } },
               "flush": { "state": "inactive", "node": "b_n_flush", "port": "flush",
                          "reason": "not_demanded" },
               "bundle": { "state": "failed", "node": "n_bundle", "port": "bundle",
                           "from": "n_fb_line", "code": "insufficient_points",
                           "root": "n_fit_datum", "rootCode": "insufficient_points" } },
  "decisions": { "n_fb_line": { "choice": "b", "reason": "io: 主路径没有产出",
                                "port": "choice", "type": "FallbackChoice" } },
  "contractViolations": [ { "node": "n_tensor", "port": "primary",
                            "expected": { "elementCount": { "eq": 1280 } },
                            "actual": { "elementCount": 1230 } } ] }
```

`status` 三态 `ok | degraded | failed`，`outputs` 每一维三态 `value | inactive | failed`。
**run 还没结束（或者已经被 free）时返回 404** —— `lyflow_run_summary` 那一层返回 `NULL`，
「还没有」与「跑完了、什么都没有」是两件事，不能都压成 `{}`。

编辑器不走这个端点：它从 WebSocket 上的 `run_finished` 事件里直接读，少一次往返。
端点留着是给不订事件流的消费方（脚本、业务服务的健康检查）用的。

### `GET /lyflow/runs/:runId/nodes/:nodeId/outputs` → `OutputInfo[]`

```json
[{ "port": "cloud", "type": "PointCloud", "elementCount": 4321, "byteSize": 69136 }]
```

非点云的项多一个 `value`（与 `RunOutputs` 里的同形）。

### `GET /lyflow/runs/:runId/clouds/:nodeId/:port?maxPoints=N`

`Content-Type: application/octet-stream`，载荷就是 `lyflow_output_cloud` 那一套
（ADR-0006；编码在 `bridge/src/execution.rs` 的 `encode_cloud`，解码在
`packages/editor/src/types/execution.ts` 的 `decodeCloud`）。**小端**：

| 偏移 | 类型 | 含义 |
|---|---|---|
| 0 | `u32` | magic `0x4350594C`（'LYPC'） |
| 4 | `u32` | `pointCount` —— 这一份里有几个点（抽稀之后） |
| 8 | `u32` | `totalPoints` —— 抽稀之前有几个点 |
| 12 | `u32` | `flags`：bit0 = 带 intensity，bit1 = 带 normals |
| 16 | `f32[6]` | `bounds`，`[minX,minY,minZ,maxX,maxY,maxZ]`，用**全量**点算 |
| 40 | `f32[3n]` | `xyz` |
| 40+12n | `f32[n]` | `intensity`，仅当 bit0 置位 |
| … | `f32[3n]` | `normals`，仅当 bit1 置位，排在 intensity 之后 |

`maxPoints` 是上限，不是要求；`0` 表示不抽稀。响应必须带 `Content-Length`。
magic 对不上时编辑器会当成「响应不是点云」直接报错，所以**错误一定要走非 2xx + JSON**，
不要往这个端点里塞错误文本。

### `GET /lyflow/runs/:runId/tensors/:nodeId/:port?offset=&count=`

`Content-Type: application/octet-stream`，载荷是 `lyflow_output_tensor` 的视图
（ADR-0019；编码在 `bridge/src/execution.rs` 的 `encode_tensor`）。**小端**：

| 偏移 | 类型 | 含义 |
|---|---|---|
| 0 | `u32` | magic `0x4E54594C`（'LYTN'） |
| 4 | `u32` | `rank` —— 形状有几维 |
| 8 | `u32` | `flags`，保留，这一版恒为 0 |
| 12 | `u32` | `count` —— 这一片里有几个元素 |
| 16 | `u64` | `offset` —— 这一片从第几个元素开始 |
| 24 | `u64` | `total` —— 整个张量有几个元素 |
| 32 | `i64[rank]` | `shape`，**永远是完整形状**，不随 `offset/count` 变 |
| 32+8·rank | `f32[count]` | 数据 |

### `GET /lyflow/runs/:runId/indices/:nodeId/:port?offset=&count=`

`Content-Type: application/octet-stream`，载荷是 `lyflow_output_indices` 的视图
（ADR-0019；编码在 `bridge/src/execution.rs` 的 `encode_indices`）。**小端**：

| 偏移 | 类型 | 含义 |
|---|---|---|
| 0 | `u32` | magic `0x5849594C`（'LYIX'） |
| 4 | `u32` | `count` —— 这一片里有几个下标 |
| 8 | `u32` | `total` —— 一共有几个下标 |
| 12 | `u32` | `flags`，保留，这一版恒为 0 |
| 16 | `u64` | `sourceCloudId` —— 这些下标指向哪片云；前端只显示，不校验 |
| 24 | `i32[count]` | 下标 |

两份布局都让 `shape`（8 字节）与数值数组（4 字节）落在自然对齐上，前端可以直接开
`BigInt64Array` / `Float32Array` / `Int32Array` 视图，零拷贝。

共同的约定（与 C ABI 一字不差）：

- `offset` 缺省 0；`count` 缺省 0 表示「从 `offset` 取到末尾」。
- **`offset` 越界是成功 + 空切片**（`count = 0`、没有数据段），不是错误 ——
  翻页翻到最后一片是正常操作，不该逼出一个「这个错要不要弹提示」的判断。
- 张量**不抽稀**：抽稀过的图像不是同一张图，数据量只靠切片控制（ADR-0019）。
- 桥接层把 `count` clamp 到 **4 194 304**（16 MB 的 f32 / i32）。**`count = 0` 同样被
  clamp**，不是原样透传 —— 否则一个一亿元素的张量会一次性 400 MB 过 IPC。
  上限是「一次请求该多大」的产品判断，属于传输层；core 自己不设限。
- 端口上没有结果、或那个端口不是张量 / 下标集合时返回非 2xx + JSON 错误体。
  与点云同理：magic 对不上编辑器就当成「响应不是张量」，**不要往这两个端点里塞错误文本**。

**参考实现（`packages/editor/test-server/`）不覆盖这两个端点，返回 501。**
那个桩服务器每次请求起一次 `lyflow` CLI，没有常驻的结果仓；点云能work 是因为 CLI 的
`dump` 会写出 PCD 文件，而张量与下标没有对应的落盘格式。理由见
[ADR-0019](adr/0019-output-tensor-and-indices-over-abi.md) 的「代价」一节。
真正的业务服务常驻 core，按 ABI 直接实现，不受此限。

---

## 缓存与库算子

`GET /lyflow/cache`：

```json
{ "entries": 12, "bytes": 8388608, "budgetBytes": 268435456,
  "hits": 30, "misses": 4, "evictions": 0 }
```

`DELETE /lyflow/cache` 返回 `{}`。

`GET /lyflow/library`：`{ "dirs": [], "count": 0, "problems": [] }`。
`POST /lyflow/library/refresh`：`{ "status": {…}, "manifest": {…} }`（重扫之后连新 manifest 一起给）。
`POST /lyflow/library/save`：`{ "doc": {}, "subgraphId": "s1", "meta": { "id": "lib.foo" } }` → `LibraryStatus`。
不支持的后端返回 501。

## 导入

`POST /lyflow/import`：

```json
{ "kind": "StandardGap.yml", "text": "…", "baseDir": "configs/R1" }
```

成功返回一个 GraphDoc 对象；失败返回 400 + `{"error": "<诊断数组的 JSON>"}`。
可用的 `kind` 在 manifest 的 `importers` 段里。

## 工作区文件

`GET /lyflow/files/graph?path=a/b.lyflow.json`：

```json
{ "doc": { /* GraphDoc */ }, "migrations": [ /* MigrationAction[] */ ] }
```

`migrations` 是 C++ 给出的迁移动作（ADR-0008），前端自己写回 doc；没有就给 `[]`。

`PUT /lyflow/files/graph?path=…`：请求体 `{ "doc": {…} }`，返回 `{}`。

备份三件套（`/lyflow/files/backup`）语义与 Tauri 侧一致：写在正文旁边的 `<file>~`，
`status` 返回 `{ exists, newer, backupModified, fileModified }`，时间戳是毫秒。
`newer: true` 表示上次多半是异常退出的，编辑器会问用户要不要恢复。

`PUT /lyflow/files/bytes?path=…`：请求体是**裸二进制**（`application/octet-stream`），
3D 视图导出 PNG 用它。

`GET /lyflow/recent` → `[{ "path": "…", "openedAt": 1730000000000 }]`；
`POST /lyflow/recent` 收 `{ "path": "…" }`，返回更新后的整张表。

---

## 事件流（WebSocket）

`ws(s)://<baseUrl>/lyflow/events`，服务端推**文本帧**，每帧一个 JSON 对象。

- **执行事件的帧体就是 ExecutionEvent 本身**，没有信封 ——
  schema 见 [`schema/execution-event.schema.json`](../schema/execution-event.schema.json)，
  `kind` 是 `run_started` / `plan_extended` / `node_state` / `node_progress` / `run_finished` / `log`。
- 另有两种**控制帧**，靠 `kind` 与执行事件区分开：

```json
{ "kind": "manifest_updated", "generation": 3,
  "operatorCount": 46, "manifest": { /* 整份 bundle */ } }
```

```json
{ "kind": "core_reload_failed", "generation": 2, "problems": ["…"] }
```

不支持热重载的后端一条都不发。

**顺序**：同一个 `runId` 的事件必须按 `seq` 递增、不跳号地推。
编辑器在 seq 不连续时会打一条 `console.warn` 并继续，但界面可能漏状态。

**鉴权**：token 走子协议，不进查询串（查询串会进访问日志和 Referer）。
客户端握手时带 `Sec-WebSocket-Protocol: lyflow.v1, lyflow-token.<token>`；
服务端校验第二项，并回 `Sec-WebSocket-Protocol: lyflow.v1`。校验不过回 401，不要升级。

**事件流不在 HTTP 端口上时**：`new HttpTransport(baseUrl, token, { eventsUrl })` 可以把这一条连接指到别处
（业务服务的 WebSocket 是独立端口）。REST 仍走 `baseUrl`，只有事件流改地址。

**历史兼容：后端的 WebSocket 库不会回子协议时**。浏览器的规矩是「请求了子协议而响应里没有」
就直接判握手失败，所以这样的后端连不上。`{ eventsProtocols: [] }` 让编辑器不请求任何子协议，
鉴权改由宿主放进 `eventsUrl`（例如 `ws://127.0.0.1:<port>/lyflow/events?token=…`）——
这是**降级**：查询串会进访问日志，只在服务只监听回环地址、且 WebSocket 库确实改不动时用。
服务端仍应当在子协议存在时校验它。**新后端不要走这一条**：换一个会回
`Sec-WebSocket-Protocol` 的库即可。xyz-gap-inspector 曾经因为 IXWebSocket 12.0.1 的
`serverHandshake` 不回子协议而走过这条路，现在它的 WebSocket 是 Boost.Beast，已经回到正路。

**重连**：`HttpTransport` 在连接断开后按 200/500/1000/2000/4000 ms 退避重连，
只要还有订阅者就一直重连。断线期间的事件**会丢** —— 后端不需要缓冲重放，
编辑器会在下一次运行时重新拿到完整的事件序列。

---

## 桩服务器与真实服务的差别

`packages/editor/test-server/` 每个请求起一次 `lyflow` CLI 进程，所以：

- **缓存统计恒为 0**：进程之间没有共享的结果仓。
- **取点云会重跑一遍图**：`lyflow dump` 自己跑一次再写 PCD，服务端把 ASCII PCD 转成上面的二进制布局。
  真实服务应该常驻 core（`core/include/lyflow/client.hpp`），直接把 `lyflow_output_cloud`
  的视图写进响应体。
- **取张量与取下标返回 501**：这两样没有像 PCD 那样的落盘格式，一个没有常驻结果仓的
  桩服务器拿不到它们（ADR-0019 的「代价」）。
- **`loadGraph` 的 `migrations` 恒为 `[]`**：桩不跑 `lyflow migrate`。
- **`saveAsLibrary` 返回 501**。
- **不发热重载帧**。

这些都是桩的省略，不是契约的一部分。
