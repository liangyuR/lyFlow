# M4 实施计划 —— 能扩展

目标：**图本身成为可复用、可脚本化、可交互探索的资产。** 三件大事：子图、headless CLI、live preview。
外加两件 P2 里靠 CLI 顺手就能做的：参数扫描、图 diff。

前提：M3 已落地。M4 不改 M2/M3 的接口形态；子图靠**编译期展开**接进现有执行器，
live preview 靠 M2 就定下的抢占式 run 与 M3 的缓存。

---

## 0. 定死的决定

| # | 决定 | 一句话理由 |
|---|---|---|
| F1 | 子图在 C++ **compile 阶段展开成平图**，执行器、缓存、事件对子图一无所知 | 嵌套执行器是全项目最贵的一块；展开后 cacheKey、并行、bypass 全部自动成立 |
| F2 | 展开后的节点 id 用 `/` 拼路径：`outer/inner/leaf`；事件里的 `nodeId` 就是这个路径 | 前端按路径前缀聚合到当前层级的节点上，进入子图后按下一段精确匹配；`localId` 的字符集本来就允许 `/` 以外的所有字符，`/` 只作分隔 |
| F3 | 子图定义存在 GraphDoc 的 `subgraphs`，节点用 `op: "sub:<subgraphId>"` 引用；**库算子**是存成独立文件的子图，由 C++ 扫描目录注册成普通算子 | 一份 GraphDoc 自包含、可移植；库算子走 manifest，前端零改动就出现在面板里（ADR-0003 再次兑现） |
| F4 | 子图的对外参数是**提升**出来的：`{ name, ...Param 字段, binds: [{node, param}] }`，一个外参可绑多个内参 | 这是 Blender GN 的模型，用户已有心智；比「子图自己声明参数再在内部引用」少一层间接 |
| F5 | live preview 是一次 `mode=preview` 的普通 run：源算子输出**先抽稀**，其余算子照常跑；预览结果进缓存的独立命名空间 | 不给算子加第二套 compute；抽稀在源头一次生效，全链路自然变快；不污染正式缓存 |
| F6 | CLI 是 bridge crate 的第二个 bin，**不依赖 Tauri**，与 app 共用 `core_ffi` 和 GraphDoc 校验 | 同一份加载、校验、迁移代码；CI 里跑的和桌面里跑的是同一条路径 |
| F7 | CLI 输出 **JSON Lines 事件流**，就是 ExecutionEvent 原样一行一条 | 前端和 CI 消费同一格式；不发明第二种日志格式 |

F1/F2/F3/F4 合写一份 ADR（0010 subgraph-by-expansion），F5 一份（0011 preview-as-decimated-run），F6/F7 一份（0012 headless-cli）。

---

## 1. 子图 / 复合算子

### 1.1 GraphDoc schema

```jsonc
"subgraphs": {
  "<subgraphId>": {
    "name": "去平面",
    "doc": "...",
    "nodes": [...], "edges": [...],                 // 与顶层同构，可嵌套引用其他 sub:
    "inputs":  [{ "name": "cloud", "type": "PointCloud", "label": "...", "to":   [{ "node": "n1", "port": "cloud" }] }],
    "outputs": [{ "name": "rest",  "type": "PointCloud", "label": "...", "from": { "node": "n5", "port": "rest" } }],
    "params":  [{ "name": "dist", "type": "float", "default": 0.01, "min": 0, "label": "...", "binds": [{ "node": "n3", "param": "distanceThreshold" }] }]
  }
}
```
- 一个输入可扇出到多个内部端口（`to` 是数组）；一个输出只来自一个内部端口。
- 子图不能直接或间接引用自己（C++ validate 报 `recursive_subgraph`）。
- `groups` 同时启用（P2 #32）：纯 UI 的框，schema 早已预留。

### 1.2 C++

- **manifest 合成**：`sub:` 节点的 OperatorDesc 在 validate 时由子图定义临时合成（端口、参数来自 `inputs/outputs/params`），走同一套参数校验和默认值合并，所以 `paramPath` 对子图参数同样成立。
- **展开**（compile 前）：递归把 `sub:` 节点替换为内部节点，id 加前缀；外部连到子图输入的边改接到 `to` 列表；子图输出的边改从 `from` 引出；`binds` 把外参值写进内参（覆盖内参自己的值）；`bypass` 的子图节点整体透传。展开有深度上限 32。
- 展开后进入既有的 validate → compile → execute，**无任何改动**。cacheKey 自动按内部节点算，子图里改一个参数只重算受影响的内部节点。
- **库目录**：`lyflow_set_library_dirs(const char* const*, size_t)`；每个 `*.lyflow-op.json` 是单个子图定义加 `{ id, version, category, keywords }` 头；注册成 `lib.<id>` 算子，category 前缀 `Library/`。运行时展开方式与 `sub:` 相同。库文件变化 → 与热重载同一条 `manifest-updated` 通路。
- 事件：`nodeId` 为路径；`run_started.nodes` 列展开后的全部节点。

### 1.3 Rust

- `GraphDoc` 结构体加 `subgraphs`；结构校验递归。
- `library_dirs` 来自 app data 下 `library/` 和设置里的额外目录；`save_as_library(subgraphId, meta)` command 写文件。

### 1.4 前端

- **导航**：`ui store` 加 `path: string[]`（当前所在子图栈），画布只渲染当前层级的 nodes/edges；面包屑可点；双击 `sub:` 节点进入，Esc/面包屑退出。撤销栈不分层，仍是整份 doc 快照。
- **状态聚合**：事件 `nodeId` 按当前 `path` 前缀匹配。`sub:` 节点的状态 = 其内部节点状态的归约（任一 error → error，任一 running → running，全 done/skipped → done）；进度 = 已完成内部节点比例。
- **创建**：选中若干节点 → Ctrl+G「合成子图」：跨边界的边自动变成输入/输出端口，名字取内部端口名，冲突加后缀；逆操作「解散子图」。两者都是 change 动作。
- **提升参数**：子图内参数右键「提升为子图参数」→ 出现在外层节点的表单；已提升的内参在内部显示为只读并标注来源。
- **库**：子图右键「保存到库」（填 id/category）；库算子在面板 `Library/` 分类下，与内置算子无差别；右键库节点「展开为内联子图」可再编辑。
- **3D 视图**：进入子图后选中内部节点同样能看输出（路径 id 直接查结果仓）。

**验收**：把 M2 演示 pipeline 的中间四个节点合成子图，运行结果点数与合成前完全一致；改一个提升参数只重算子图内受影响节点；保存到库后新建图从面板拖出即用；嵌套两层子图的 cacheKey 稳定（重跑全 skipped）；递归引用被拒。

---

## 2. Live preview

- run 选项加 `mode: "full" | "preview"`、`previewMaxPoints`（默认 200 000）、`previewBudgetMs`（默认 300）。
- 执行器 preview 模式：无输入的源算子（load/synthetic/库源）输出先做等步长抽稀到 `previewMaxPoints`；其余算子原样跑。cacheKey 混入 `preview:<maxPoints>` 前缀，与正式缓存互不命中。超过 `previewBudgetMs` 的 run 记 `log warn`，前端据此提示「预览过慢，降低预览点数」。
- 抢占：M2 已是「新 run 取消旧 run」。preview run 结束后前端按需自动发起 full run（「自动运行」开关）。
- 前端：参数控件 `onDragStart` 进入预览态；每次值变化 debounce 30 ms 发 preview run，目标 = 选中节点；`onDragEnd` 若开了自动运行则发 full run。3D 视图角标「预览 20 万点」。
- PCL 算子的 `cancellable=false` 在预览下无碍：输入已被抽稀，单节点耗时可控。

**验收**：拖动体素 leafSize 滑块时 3D 视图跟手（事件到渲染 < 100 ms，CDP 量），松手后正式结果替换预览；预览期间正式缓存无新增条目。

---

## 3. Headless CLI

`bridge/src/bin/lyflow.rs`，`cargo build --bin lyflow`，DLL 与 exe 同目录。

```
lyflow run      graph.lyflow.json [--to nodeId]... [--set nodeId.param=<json>]... [--base-dir d] [--parallel n] [--no-cache]
lyflow validate graph.lyflow.json
lyflow plan     graph.lyflow.json
lyflow migrate  graph.lyflow.json [--write]
lyflow manifest [--check]
lyflow dump     graph.lyflow.json nodeId:port out.pcd          # 跑到该节点并把输出写盘
lyflow sweep    graph.lyflow.json --param nodeId.param=start:end:steps [--param ...] --metric nodeId:port.elementCount [--csv out.csv]
lyflow diff     a.lyflow.json b.lyflow.json [--json]
```

- stdout 是 JSON Lines（`run` 输出 ExecutionEvent 原样；`validate/plan/migrate` 输出各自 JSON），stderr 给人看。
- 退出码：0 ok，1 校验失败，2 执行失败，3 取消（Ctrl+C 走 `lyflow_run_cancel`），4 参数错。
- `--set` 的路径与前端「复制路径名」一致，值为 JSON 字面量。
- `sweep` 是笛卡尔积循环调 run，靠缓存让上游只算一次；输出每组参数与指标的表。
- `diff` 是语义 diff：新增/删除节点、参数变化（合并默认值后比较）、边变化，忽略 `ui`。

**验收**：CI 脚本用 `lyflow run` 跑演示 pipeline 并断言退出码与最终点数；`sweep` 5 组 leafSize 只加载文件一次（事件里 load 节点 4 次 skipped）；`diff` 对只移动节点的两份图输出为空。

---

## 4. 大图性能（P2 #37）

- React Flow `onlyRenderVisibleElements`；参数面板只渲染选中节点。
- 事件合并：execution store 把 16 ms 内到达的 `node_state` 批量应用一次。
- 基准 fixture：300 节点、400 边的合成图；CDP 量拖动帧率 ≥ 30 fps、打开 < 1 s。

---

## 5. 实现顺序

1. 子图 schema + Rust 结构 + C++ 展开 + 测试（1.1–1.2、1.3 的结构部分）
2. 子图前端（1.4）
3. 库目录（1.2 库、1.3 save_as_library、1.4 库）
4. CLI（3），先 `run/validate/plan/migrate/manifest`
5. live preview（2）
6. `sweep` / `diff` / 大图性能（3 的后两条、4）

每步 `pnpm check` 绿；CLI 进 `check.ps1` 作为 Rust 之后的一步。

---

## 6. 验收（M4 完成的定义）

- [ ] 1–4 各自验收自动化
- [ ] 一条真实任务用「库算子 + CLI」跑在无 GUI 的机器上
- [ ] 交互清单 P2 #31 #32 #33 #34 #36 #37 标完成

---

## 7. 明确不做（留给 M5）

- 第二种数据域（Image）。数据模型的 `Data::Kind` 与端口类型表已为此留位，验证「模型是否通用」是 M5 的事
- 第三方算子插件 DLL（`lyflow_plugin_init`）。热重载 + 库算子已覆盖自己人的扩展需求
- 缓存落盘；两节点并排对比（P2 #35）；协作（P2 #38）
