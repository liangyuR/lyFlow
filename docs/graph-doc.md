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
| C++ | 执行前 | 权威。类型系统、参数范围、环检测、资源可行性 |

三层都要做。前端那层可以被绕过，后两层不能省。
