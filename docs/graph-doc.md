# GraphDoc — 图数据模型

GraphDoc 是 LyFlow 的**唯一真实数据源**。前端状态、磁盘文件、传给 C++ 的载荷都是它。
React Flow 的 `Node` / `Edge` 只是渲染时的派生产物。

## 设计约束

1. **不含运行时状态。** 没有 `selected`、`dragging`、`measured`、`status`。这些活在内存里的独立 store 中。
2. **UI 与语义分离。** 坐标、折叠状态等放在每个节点的 `ui` 子对象里。后端可以整体忽略 `ui`。
3. **可稳定序列化。** 字段顺序固定、无浮点噪声、数组有序，保证同一个图两次保存产生相同字节，git diff 才有意义。
4. **可前向演进。** 顶层有 `schemaVersion`，节点记录 `opVersion`，未知字段保留不丢。

## 结构

```jsonc
{
  "schemaVersion": 1,
  "id": "01J8X...",              // ULID，文件重命名后仍可追踪
  "name": "bin picking preprocess",
  "meta": {
    "createdAt": "2026-09-05T10:00:00Z",
    "modifiedAt": "2026-09-05T11:30:00Z",
    "app": "lyflow/0.1.0"
  },

  "nodes": [
    {
      "id": "n_load",
      "op": "io.load_pcd",         // 对应 OperatorManifest.id
      "opVersion": "1.0.0",        // 保存时的算子版本，用于迁移
      "params": {
        "path": "samples/bin.pcd"
      },
      "ui": {
        "position": { "x": 0, "y": 0 },
        "collapsed": false,
        "title": null,             // null = 用 manifest 的 label
        "color": null,
        "width": null              // null = 自适应
      }
    },
    {
      "id": "n_voxel",
      "op": "filter.voxel_grid",
      "opVersion": "1.2.0",
      "params": { "leafSize": [0.005, 0.005, 0.005] },
      "ui": { "position": { "x": 260, "y": 0 } }
    }
  ],

  "edges": [
    {
      "id": "e_1",
      "from": { "node": "n_load",  "port": "cloud" },
      "to":   { "node": "n_voxel", "port": "cloud" }
    }
  ],

  "outputs": {         // 图级命名输出，见下面「图级输出」
    "cloud": { "node": "n_voxel", "port": "cloud" }
  },

  "params": {},        // 顶层图参数，见下面「顶层图参数」
  "groups": [],        // 预留：节点分组框
  "subgraphs": {},     // 复合算子定义，见下面「子图」
  "x": {}              // 扩展位：未知字段容器，保证向前兼容
}
```

JSON Schema： [`schema/graph-doc.schema.json`](../schema/graph-doc.schema.json)

## 关键决策

### 端口用名字，不用序号

`{ "node": "n_load", "port": "cloud" }` 而不是 `"outputIndex": 0`。
算子演进时增删端口不会让老图错位；`git diff` 也读得懂。

### 一个输入端口最多一条边

输入端口是单连接，输出端口可以一对多。多输入合并（比如拼接两片点云）由**算子显式声明多个输入端口**
或声明可变长端口来表达，而不是靠往一个端口上连多条边——后者的求值顺序是隐式的，是 bug 温床。

### 参数是稀疏的

`params` 只存**与 manifest 默认值不同**的项。好处：默认值改动能自动传播到老图；文件更小；diff 更干净。
读取时用 `manifest.defaults` 与 `params` 合并得到有效值。

代价：manifest 改默认值会静默改变老图的行为。缓解办法是节点上记了 `opVersion`，
可以在打开时提示「此图保存于 v1.1.0，当前 v1.2.0，`leafSize` 默认值已变更」。

### 图级输出（阶段 A，[ADR-0017](adr/0017-graph-outputs-injection-importers.md)）

`outputs` 是 `{ 名字: { node, port, label? } }`。嵌入 LyFlow 的宿主只认名字，
不认节点 id —— 用户在编辑器里重命名一个节点不该让产线停摆。

`lyflow_run_outputs(run_id)` 按名字返回 `{ node, port, type, elementCount, byteSize, value? }`；
点云只给元信息，二进制仍走 `lyflow_output_cloud`。该端口这次没有结果时多一个
`"missing": true`，而不是把这一项省掉：宿主拿到的键集合应当只由图决定。

指子图内部的端口时写**展开后**的路径 id（`outer/inner`），与 `run_started.plan` 同一套 id。

`port` 也可以写成 `<port>.<field>`，指向一个 Bundle 端口（`Bundle<kind>`）里的字段，例如
`{ "node": "n_locate", "port": "rois.datum" }`（M8a，m8-plan L3）。校验期查那个端口确实是
Bundle、kind 里确实声明了这个字段，否则报 `unknown_port`；`lyflow_run_outputs` 与 summary
给的是那个字段自己的类型和值。

### 子图（M4，[ADR-0010](adr/0010-subgraph-by-expansion.md)）

`subgraphs` 的键是 subgraphId，节点用 `op: "sub:<subgraphId>"` 引用它：

```jsonc
"subgraphs": {
  "sg_clean": {
    "name": "去噪",
    "nodes": [...], "edges": [...],           // 与顶层同构，可以再引用别的 sub:
    "inputs":  [{ "name": "cloud", "type": "PointCloud",
                  "to": [{ "node": "s_voxel", "port": "cloud" }] }],
    "outputs": [{ "name": "cloud", "type": "PointCloud",
                  "from": { "node": "s_sor", "port": "cloud" } }],
    "params":  [{ "name": "leafSize", "type": "vec3f", "default": [0.005, 0.005, 0.005],
                  "binds": [{ "node": "s_voxel", "param": "leafSize" }] }]
  }
}
```

四条规则：

- **一个输入可以扇出到多个内部端口**（`to` 是数组），一个输出只能来自一个内部端口。
- **对外参数是提升出来的**：`params[]` 就是一份 manifest 的 param 声明加一组 `binds`。
  外层节点的 `params` 里写的是外参名；展开时把值写进每一个绑定的内参（覆盖内参自己的值）。
- **子图不能直接或间接引用自己**，C++ 展开时报 `recursive_subgraph`；嵌套深度上限 32。
- **库算子**（`op: "lib.<id>"`）是同一份结构存成独立文件 `*.lyflow-op.json`，
  由 C++ 扫描库目录注册成普通算子。库文件必须自包含 —— 里面不能再有 `sub:` 引用。

展开发生在 C++ 的 compile **之前**，展开后的节点 id 是路径 `outer/inner/leaf`，
事件里的 `nodeId` 就是它。执行器、结果仓、缓存、事件流对子图一无所知。
前端按当前所在层级（`ui.path`）的前缀把事件聚合到那一层的节点上。

在界面里：选中若干节点按 **Ctrl+G** 合成子图，**Ctrl+Shift+G** 解散；双击子图节点进去，
**Esc** 或面包屑出来；内部节点的参数右键可以「提升为子图参数」；
子图节点右键可以「保存到库」。

### 顶层图参数（M7，P1 起是完整参数规格）

「本来就是全局」的值（间隙补偿量、模型路径）要有唯一的定义处，宿主按名字传值，而不是改 JSON 里某几个节点的参数。
它也是**参数配方的结构**（[param-recipe-plan.md](param-recipe-plan.md) K1）：配方 = `{ 图参数名: 值 }` 的稀疏覆盖，
没写的用 `default`（称为「基础」）。GraphDoc 顶层可选 `params`：

```jsonc
"params": {
  "gapOffset": {
    "type": "float",                               // 可选，manifest 参数类型名；给了就是一份完整规格
    "label": "间隙测点 · 补偿量",                    // 以下都是可选的规格字段，与 manifest 的 param 同一套
    "min": -2, "max": 2, "softMin": -0.5, "softMax": 0.5, "step": 0.01, "unit": "mm",
    "group": "补偿",
    "default": 0.0,                                // 必填，不传值时用它（配方的「基础」值）
    "binds": ["n_gap.offset", "n_circles.offset"], // 必填，<节点id>.<参数名>
    "doc": "间隙读数补偿，mm"                        // 可选
  },
  "modelPath": { "default": "models/v12s0.onnx", "binds": ["n_infer.modelPath"] }   // 老格式照样合法
}
```

**规格（P1.1）。** 每一项在 `{type?, default, binds, doc?}` 之外允许 manifest 参数声明的全部字段：
`label`、`min`/`max`（硬限位）、`softMin`/`softMax`（滑条范围）、`step`、`unit`、`componentLabels`、`options`、
`group`、`advanced`、`placeholder`、`pattern`、`rows`、`filters`/`mode`、`alpha`、`visibleWhen`/`enabledWhen`、
`semantic`/`roiBackdrop`。schema 里是 `$ref` 到 `operator-manifest.schema.json` 的 `paramSpec`（manifest 的
`param` = `name` + 同一份 `paramSpec`），不抄第二份；多出来的字段（包括拼错的）一律拒绝。老文件完全兼容。
样例见 [`schema/examples/graph-params.example.lyflow.json`](../schema/examples/graph-params.example.lyflow.json)。

**取值的校验（P1.2）。** 声明了 `type` 的图参数，core 在展开期先按它自己的规格查 `default` 与宿主传进来的值
（类型、硬限位、`options`；`enum` 没给 `options` 时只查是字符串），不合法报 `bad_param`，`paramPath` 是图参数名、
`nodeId` 为空，消息里说清是「default」还是「传入的值」；然后照旧写进被绑定的节点、走节点自己的参数规整
（节点那一层的错照常报，`nodeId` 是节点）。两层**都要**满足：图参数的规格决定控件与第一道校验，被绑定算子的
声明是它自己的硬契约，core 不会因为图参数放宽了限位就放行一个算子不认的值。有这一类错时 `validate` 照样收齐
全部诊断，但 `plan` / `run` 不给可运行的计划（运行整次失败、一个节点都不跑）。没有 `type` 的老图参数跳过第一步。
`visibleWhen`/`enabledWhen`/`roiBackdrop` 只给编辑器看，core 不据此判定。

- `binds` 的每一项是 `节点id.参数名`，按**最后一个** `.` 切（节点 id 本身可以含 `.`）。
  目标是顶层节点；可以是子图实例节点，这时参数名是该子图声明的提升参数。
- 语义与子图的 `params[].binds` 相同：**展开期**把取值写进被绑定的节点参数。
  所以 cacheKey、`lyflow plan`、`lyflow params` 自动反映它 —— 改一个顶层参数，
  只有被绑定的节点及其下游的 cacheKey 会变。
- 运行期取值：CLI `run` / `validate` / `plan` / `params` / `eval` / `patch` 用
  `--param <名字>=<json>`（值先按 JSON 解析，解析不了当字符串，与 `--set` 同一条规则）；
  C ABI 用 `lyflow_run_options.params_json`（见 [embedding.md](embedding.md)）。不传就用 `default`。
- `lyflow params` 里被顶层参数写入的那一行 `source` 是 `graph`，另带 `graphParam: "<名字>"`。

规则（前三条是 validate 诊断，图跑不起来）：

| 情形 | 报什么 |
|---|---|
| bind 指向不存在的节点或参数 | `unknown_bind` |
| 被绑定的参数在节点的 `params` 里又显式写了值 | `param_conflict`（一处定义） |
| 同一个目标被两个顶层参数绑定 | `param_conflict` |
| `--param` 给了未声明的名字 | `unknown_param`，CLI 退出码 4 |
| `--set` 命中被绑定的参数 | 报错，CLI 退出码 4，提示改用 `--param` |

`eval` 原本就有 `--param <节点>.<参数>=<start>:<end>:<steps>` 扫描轴，两者按 `=` 左边**是否含 `.`**
区分：含 `.` 是扫描轴，不含是顶层参数。

#### 编辑器里的图参数（param-recipe P1）

- **纳入配方 = 提升为图参数**（K2）。Inspector 里参数行右键「纳入配方（提升为图参数）」：
  这个参数当前的有效值成为 `default`，节点上的显式值删掉（一处定义，不会有 `param_conflict`），加一条 bind；
  规格从被绑定目标的声明复制，`label` 写成「节点标题 · 参数 label」。同一算子内的 `visibleWhen`/`enabledWhen`、
  `roiBackdrop` 不复制（它们指的是那个算子自己的其它参数名，到图这一层对不上），`semantic` 照抄。
  名字取参数名，重了加 `_2`、`_3`（与子图提升参数同一条规则）；名字不能空、不能含 `.`（CLI 靠它区分
  图参数与 `<节点>.<参数>`）、不能含 `=`。整个动作一条撤销。
- **子图里纳入 = 逐层提升链**。在子图内部的参数上做同一个动作：从这一层往外，哪一层还没提升就在那个子图定义里
  提升成子图参数（默认值 = 这一层的当前值，所以**同一子图的其它实例行为不变**），一直到顶层，再在「当前路径上
  的那个实例」上绑成图参数。嵌套多层时每一层各起各的名字（参数名，重了加后缀），label 带上路径上的实例标题
  （「实例 / 内部节点 · 参数」）。已经提升过的层直接沿用，往外一层的当前值取那个实例上外参的值。
  一个动作、一次撤销。库算子（`lib.`）进不去，内部参数没有这个动作。
- **被绑定参数的显示与编辑**（P1.4）。被图参数提供的行（顶层节点上直接被绑定的，或子图里整条提升链通到图参数的）
  显示图参数的**有效值**，标「由图参数 X 提供」；在这一行编辑（包括 2D 拖框、粘贴、重置）路由到图参数：
  选着「基础」时改 `default`，P3 起选着配方时写进当前配方（K6）。判定只在 graph store 的 `setParam` 一处。
- **动作**（graph store，每个一条撤销）：`promoteToGraphParam`、`bindToGraphParam`（类型不同的拒绝）、
  `unbindFromGraphParam`（把当前有效值写回节点，仍按稀疏存储，行为不变）、`removeGraphParam`（同上，写回所有
  绑定目标；最后一个删掉时 `params` 键也不留）、`renameGraphParam`（键的位置不变）、`setGraphParamDefault`、
  `setGraphParamSpec`（改不了 `binds`/`default`/`type`）、`editGraphParamValue`（按 K6 路由）。
  「当前有效值」= `default` ← 当前配方覆盖，P1 里就是 `default`。删节点会摘掉指着它的 bind；
  合成子图时被绑定的参数自动提升成外参、bind 改指到新实例；解散时 bind 改指回内联出来的内参；取消子图提升会摘掉
  指着那个外参的 bind —— 这几处都不留 `unknown_bind`。
- **运行传参**（K3）。每次运行、实时预览、实时校验、编计划，编辑器都合成 `default ← 当前配方覆盖` 得到
  `{名字: 值}`，经 `RunOptions.params`（validate / plan 的同名参数）交给后端：Tauri 走 C ABI 的 `params_json`
  （validate / plan 用 `lyflow_validate_params` / `lyflow_plan_params`），HTTP 走信封的 `params`，桩服务器转成
  CLI 的 `--param`。切配方不改 GraphDoc；结果缓存按内容寻址，值换回来就命中缓存。
- 图参数自己的诊断（`nodeId` 为空、`paramPath` 是名字）在 Inspector 顶上「图参数」简表的对应行下标红；
  完整的参数面板（改规格、搜索过滤、全类型控件）是 P2。

### `ui` 可以整体丢弃

后端处理时直接忽略 `ui`。反过来，一个没有 `ui` 的 GraphDoc（比如脚本生成的）也必须能被前端打开——
缺失坐标时自动布局。M3 用 `@dagrejs/dagre` 做 LR 分层（E8），只在**两种**时机触发：
打开时发现有节点缺 `ui.position`，以及用户主动点「整理」/ 按 Ctrl+L
（M4 起 Ctrl+G 让给了「合成子图」）。
**永远不自动覆盖用户摆好的位置**，而且整段布局是一条撤销记录。

## 前端映射层

```
GraphDoc ──(toReactFlow)──► { nodes: Node[], edges: Edge[] }   仅渲染用
   ▲                                    │
   └──────(applyChange)◄────────────────┘   用户交互 → 语义化 change → 写回 GraphDoc
```

写回时走**语义化的 change 动作**（`moveNode`、`setParam`、`connect`、`disconnect`、`addNode`、`deleteNodes`、
M4 起还有 `composeSubgraph`、`dissolveSubgraph`、`promoteParam`），
它们一律作用于**当前层级**（`ui.path` 指的那一层），
不要直接 diff 节点数组。原因有二：撤销重做需要的是有语义的 patch；批量操作（多选移动）应该合成一条 undo 记录。

撤销重做用 immer patch 或 `zustand` + `zundo`，逆向 patch 直接由 immer 生成。
**注意把纯 UI 操作排除在 undo 栈之外**（画布平移缩放、选中变化），否则用户按 Ctrl+Z 会觉得没反应。

## 校验分层

| 层级 | 时机 | 目的 |
|---|---|---|
| 前端 | 连线时 / 输入时 | 手感。挡掉明显错误，即时反馈 |
| Rust | 反序列化时 | 结构完整性。字段类型、引用存在性、无悬空边 |
| C++ | 执行前 | 权威。类型系统、参数范围、环检测、资源可行性、顶层参数绑定、算子的 `validate` 钩子 |

三层都要做。前端那层可以被绕过，后两层不能省。
