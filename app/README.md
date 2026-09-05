# app —— 前端（React + React Flow + three.js）

图编辑、类型校验（体验层）、参数表单、状态展示、本地撤销重做。
**不做执行调度、算子语义、结果计算**（[docs/architecture.md](../docs/architecture.md)）。

```
src/store/        graph（GraphDoc + 撤销栈）、ui（选中/剪贴板/抽屉）、
                  manifest（算子描述）、execution（运行状态）、cache（plan/缓存统计）
src/lib/          typecheck（连线合法性 + Any 推导）、params（稀疏存储与联动）、
                  keymap（唯一那张快捷键表）、layout（dagre）、mapping（↔ React Flow）
src/components/   画布、检查器、工具栏、3D 视图、底部抽屉、快捷键面板
src/transport/    Tauri / 静态快照两种 transport
```

## 三条纪律

1. **GraphDoc 是唯一真实数据源**（ADR-0002）。React Flow 的 Node/Edge 是单向派生，
   交互结果一律翻译成 graph store 的语义化动作再写回。
2. **改图只能走 graph store 的语义化动作**，动作集合是封闭的。这样撤销栈的每一步
   都对应一个用户能理解的操作，而不是「节点数组第 3 项的 x 变了」。
3. **纯 UI 状态不进 GraphDoc、不进撤销栈**。选中、视口、抽屉开合、量测尺寸都在
   `store/ui.ts` 或组件的旁路缓存里。按 Ctrl+Z 只取消了一次选中，用户会认为撤销坏了。

历史用整份 doc 快照而不是 patch：immer 的结构共享让未改动的节点在新旧快照之间
共用同一份对象；而 patch 的路径基于数组下标，删一个节点会让之前所有 patch 失效。

撤销的**粒度**由控件决定，处理散落在 `ParamControls.tsx` 全文：输入框用本地 state、
失焦或回车才提交（一次编辑一条撤销），滑块按下时 `begin()`、松开时 `commit()`
（一次拖动一条撤销）。少了这层，拖一次滑块会往撤销栈里塞几十条，Ctrl+Z 就没意义了。

## 算子知识只有一处

前端不硬编码任何算子。唯一的「算子知识」是 `ParamControls.tsx` 里那张
`param.type → React 控件` 映射表。新增一种参数类型的成本是加一个 case（有界）；
新增一个算子的成本是零（ADR-0003）。

算子描述存在 manifest store，节点只记 `op` id、渲染时现查 —— 这是热重载的前提。
反过来在创建节点时把 manifest 快照进节点里，热重载就永久失效了。

## 参数的稀疏存储

`GraphDoc` 的 `params` 只记与 manifest 默认值**不同**的项（docs/graph-doc.md）。
这样默认值一改就能自动传播到所有老图，文件也更小、diff 更干净。

代价是读写都得走 `lib/params.ts`：读一个参数永远是「manifest 默认值 ← 节点覆盖值」
的合并，写一个参数在值回到默认时要把这一项**删掉**，而不是存一个等于默认的副本。
漏掉删键那一半，稀疏存储就静默退化成全量存储 —— 图照样能用，只是上面那些好处全没了。

## 节点字段的穿透

`bypass`（静音：不调 compute，输出从类型兼容的输入透传）是**执行语义**不是 UI 状态，
所以它在 `node.bypass` 而不是 `node.ui` 里，而且**进撤销栈**。前端必须一路带着它 ——
复制粘贴、迁移写回这类「重建节点对象」而不是就地改的路径漏掉它，用户存盘时静音就没了。
`GraphDoc.x` 同理，是老客户端打开新版本写的图时不丢数据的未知字段容器。

## stale 是读来的，不是算出来的

「哪几个节点已经过时」的判定**只在 C++**（[ADR-0007](../docs/adr/0007-cache-authority.md)）。
`store/cache.ts` 只干两件事：doc 变了就 debounce 150 ms 调一次 `plan_graph`，
把结果和上一次 `run_started.nodes[].cacheKey` 对比。

前端算不出 IO 算子的 `externalKey`（路径没变但文件被覆盖了，缓存也得失效），
自己推一定会在那儿错 —— 而且错得很安静。`isNodeStale` 要求
「键变了 **且** 现在没有缓存」两个条件同时成立，只看键变化的话改回原值也会一直标着红。

## 迁移是一条可撤销的动作

打开老图时 `load_graph` 回的是 `{ doc, migrations }`，前端把那批动作交给
`applyMigrations`：一条撤销记录、置 dirty、toast 提示
（[ADR-0008](../docs/adr/0008-migration-as-diagnostic.md)）。

不静默改写，是因为那是**用户的文档被改了**。撤销栈里必须有这一步，标题栏必须有那个 `*`。

## 踩过的坑

这些全部来自 CDP 验收（`scripts/e2e`）抓到的真 bug，没有一个是单元测试能发现的。

- **选中状态渲染死循环。** 选中会流回画布（映射层写进 `node.selected`），画布又
  回调 `onSelectionChange`。无条件 `set` 新 `Set` 会让引用每次都变 → useMemo 重算
  → 节点数组换新 → 再次回调。`ui.setSelection` 必须先比对再写。
- **d3-zoom 吞掉双击。** React Flow 底层给 pane 装了 dblclick 缩放，它会
  `stopImmediatePropagation`。要在空白处双击开搜索面板就必须 `zoomOnDoubleClick={false}`。
- **MiniMap 一个节点都不画。** 它靠传进去的对象判断节点有没有尺寸，而尺寸只通过
  `onNodesChange` 的 `dimensions` 事件回传。尺寸是 UI 运行时状态不能进 GraphDoc，
  所以存在画布组件的旁路缓存里，映射时合并进来。
- **Backspace 不参与删除。** 在参数输入框里退格却删掉了节点是经典事故。
- **WebGL 上下文泄漏。** `renderer.dispose()` 不释放上下文（那是 `forceContextLoss`）。
  少了它，每次挂载/卸载漏一个；浏览器攒够十几个后开始逐出最老的，表现是视图突然全黑。
- **点大小不能进重建几何体的 effect。** 否则拖一下滑块就要重新分配几十兆颜色数组、
  重扫两百万点、再传一次 GPU，每个 input 事件一遍。
- **取点云的 effect 不能依赖整张 nodes Map。** 那张 Map 每来一条事件就是新引用
  （包括每 50ms 一条的进度），会排起一队几十兆的 IPC 往返。只订阅那一个节点的状态串。
- **`run_graph` 的回复顺序不等于运行开始顺序。** 两次调用走不同的 Tauri 工作线程，
  先发的可能后到。用一个本地自增 ticket 让后发的那次赢，与 C++ 侧的抢占方向一致。
- **事件可能比 `run_graph` 的返回值先到。** C++ 是先起线程再返回句柄，所以认不出
  runId 的事件先进 `orphans`，`beginRun` 时按 runId 认领。直接丢的话小图会整场跑完
  而界面什么都没发生。
- **订阅守卫必须守 Promise 本身。** 守 `unlisten !== null` 不够：它在 await 之后
  才赋值，而 StrictMode 的挂载→卸载→再挂载会让两次都看到 null，结果注册两个监听器。
- **Shift+F5 被 F5 抢先匹配。** 键表原来只在写明 `Shift+` 时才要求 shift 按下，
  于是 `F5` 这一条先命中。Shift 必须**精确**匹配，唯一的例外是 `?` 这种
  本来就要按 Shift 才打得出的符号。
- **`onReconnectEnd` 的第四个参数各版本形态不一。** 猜错的后果是把一条刚重连好的边
  直接删掉。改成在 `onReconnect` 里自己记一笔，`onReconnectEnd` 只看那一笔。
- **拖节点到连线上：容差不能只看中心距离。** 12 px 是**画布坐标**，画布缩到 50%
  时只剩 6 个屏幕像素，用户根本够不着。判据改成「连线穿过节点矩形」，
  中心距离只作为擦边时的余量。
- **React Flow 的 `nodeDragThreshold` 会吞掉第一段位移。** 拖拽类的 CDP 断言必须先
  发一个 2 px 的「唤醒」移动，否则落点永远差第一步那么多，位移越大差得越多。

## 层级（子图）

`ui.path` 是当前所在的子图栈（`{ nodeId, subgraphId }[]`），**纯导航状态**：
不进 doc、不进撤销栈。画布只渲染 `levelOf(doc, path)` 那一层，
graph store 里所有改图的动作也都作用在那一层。

事件里的 `nodeId` 是**展开后的路径**（`outer/inner/leaf`）。
`aggregatedNodes(path, nodes)` 把它按当前层级的前缀聚合成「本地 id → 状态」：
叶子节点直接复用原对象（引用不变，组件不白重渲），子图节点按
「任一 error → error，任一 running → running，全 done/skipped → done」归约。

`sub:` 节点的 `OperatorDesc` 是**合成**出来的（`lib/subgraph.ts` 的
`augmentOperators`），因为子图定义随文档走而 manifest 是进程级的。
合成结果按 `doc.subgraphs` 的对象身份缓存，doc 不变就不重建。

## 大图性能

- 节点数超过 80 才开 React Flow 的 `onlyRenderVisibleElements`：
  小图下全量渲染的手感更好，开了之后平移会有一帧空窗。
- `node_state` / `node_progress` 按 **16 ms** 合并成一次 store 更新；
  `run_started` / `run_finished` 立刻 flush，所以「等运行结束再读状态」仍然准。
- 状态流水账（`window.__lyflow.transitions`）来自**事件**而不是 store 快照 ——
  合并窗口会把中间态吃掉，从快照推就断言不了「节点依次变色」。

## 验收

不写 UI 单元测试（CLAUDE.md）。验收方式是 CDP 驱动真实运行的 Tauri app：
`pnpm e2e`，脚本在 [`scripts/e2e/`](../scripts/e2e/)。
`src/lib/devbridge.ts` 把 store 挂到 `window.__lyflow` 上供脚本读状态 ——
它只读+转发，不放任何业务逻辑，应用代码一律不许 import 它。

## 浏览器模式

`pnpm app:dev` 不启动 Tauri，manifest 读 `public/manifest.dev.json`
（用 `pnpm core:dump` 刷新）。界面迭代的反馈循环因此是秒级而不是分钟级。
状态栏会把这种模式明确标成「静态快照」，避免有人对着三天前的数据调半天。
