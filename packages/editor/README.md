# `@lyflow/editor`

LyFlow 的节点图编辑器，一个 React 组件。画布、参数表单、Inspector、3D 视图、
连线内容查看器、撤销重做、快捷键、类型校验都在里面；**怎么和 C++ 说话不在里面** ——
那是宿主给的 `Transport`。

```tsx
import { LyFlowEditor, HttpTransport } from "@lyflow/editor";

const transport = new HttpTransport("http://127.0.0.1:8787", token);

export function App() {
  return <LyFlowEditor transport={transport} />;
}
```

编辑器会撑满父容器的高度，所以父容器得有高度（`flex: 1` 或 `height: 100%`）。

## 装它

```
pnpm add @lyflow/editor
```

peerDependencies —— **必须与宿主共用同一份实例**，否则包里的 hooks 会在宿主的
渲染树里炸掉（"Invalid hook call"）：

| 包 | 版本范围 |
|---|---|
| `react` | `^18.3.1 \|\| ^19.0.0` |
| `react-dom` | `^18.3.1 \|\| ^19.0.0` |
| `@xyflow/react` | `^12.11.6` |
| `three` | `>=0.160.0 <1` |
| `zustand` | `^5.0.2` |
| `@tauri-apps/api` | `^2`，**可选** —— 只有用 `TauriTransport` 才需要 |

Vite 宿主把它们写进 `resolve.dedupe`：

```ts
resolve: { dedupe: ["react", "react-dom", "@xyflow/react", "three", "zustand"] }
```

`examples/host-react/` 是一份可以照抄的最小宿主，它的 `vite.config.ts` 里还带一个
构建期插件：打包结果里任何一个 peer 出现两份就直接让构建失败。

包按 **TypeScript 源码**发布（`exports` 指向 `src/index.ts`），所以宿主要用一个
能编译 TS/TSX 的打包器（Vite、Next、Rspack 都行）。

## Props

```ts
interface LyFlowEditorProps {
  transport: Transport;              // 必填
  dialogs?: EditorDialogs;           // 文件对话框，宿主提供
  graphPath?: string;                // 挂载后自动打开这张图
  onDocChange?: (doc: GraphDoc, dirty: boolean) => void;
  theme?: Record<string, string>;    // --lyflow-* 变量覆盖
  className?: string;
  animations?: boolean;              // 画布动效，默认 true
}
```

`onDocChange` 在文档或 dirty 状态变化时回调，宿主拿它做自己的标题栏、保存提示。

`animations={false}` 关掉画布动效：节点进出场、完成/出错的闪光、连线生长、自动布局的过渡
全部直接落到终态，CSS 的循环与过渡一并停掉。系统设了「减少动态效果」
（`prefers-reduced-motion: reduce`）时等同于 false，不用宿主自己判断。信息不会跟着丢：
正在流数据的边关了动画仍是一条静态高亮。

## Transport

三个现成实现：

| 实现 | 用途 |
|---|---|
| `new TauriTransport()` | 跑在 Tauri 壳里，每个方法对一条 `#[tauri::command]` |
| `new HttpTransport(baseUrl, token?)` | 跑在浏览器里，对着 [`docs/http-transport.md`](../../docs/http-transport.md) 的后端 |
| `new StaticTransport(manifestUrl?)` | 只读：读一份 dump 出来的 manifest，什么都跑不了。没有后端时也能把界面渲染出来 |

要接自己的后端就实现 `Transport` 接口（`src/transport/types.ts`）。
它的每个方法都对着 C ABI v9 的一个入口。v7 那一版的完整清单见
[`docs/phase-a1-acceptance.md`](../../docs/phase-a1-acceptance.md#c-abi-v7-的最终签名清单)，
v8 增补的张量与下标两个入口见 [ADR-0019](../../docs/adr/0019-output-tensor-and-indices-over-abi.md)，
v9 增补的 `lyflow_run_summary` 见 [ADR-0022](../../docs/adr/0022-run-summary-as-core-output.md)
（编辑器不额外调它 —— 那份 summary 就挂在 `run_finished` 事件上）。

点云走二进制，绝不 JSON（ADR-0006）：`getOutputCloud` 返回的 `ArrayBuffer`
布局见 [http 契约](../../docs/http-transport.md)的「结果」一节，
解码用包里导出的 `decodeCloud`。

## 对话框

打开、另存、放弃改动、恢复备份这四件事要弹系统对话框，编辑器不知道怎么弹，
由宿主注入：

```ts
import type { EditorDialogs } from "@lyflow/editor";

const dialogs: EditorDialogs = {
  pickOpenPath: () => …,          // 返回 null 表示用户取消
  pickSavePath: (suggested) => …,
  confirmDiscard: (dirty) => …,
  confirmRestore: (path, message) => …,
  pickPath: (req) => …,           // 可选：参数表单的路径选择、3D 导出 PNG
};
```

不给 `dialogs` 时：确认类退回 `window.confirm`，路径选择类给一条提示，
3D 导出退回浏览器自己的下载。`app/src/dialogs.ts` 是 Tauri 版的实现。

## 主题

样式全部走 `--lyflow-*` 变量，默认值定义在编辑器根元素 `.app` 上（不是 `:root`，
宿主页面的其余部分不会被改色）。覆盖方式二选一：`theme` prop，或者在更外层的
选择器里重定义。

| 变量 | 默认 | 用处 |
|---|---|---|
| `--lyflow-bg-0` … `--lyflow-bg-3` | `#14161a` … `#2a2f39` | 由深到浅的四层底色 |
| `--lyflow-border` / `--lyflow-border-strong` | `#333a46` / `#454d5c` | 分隔线、边框 |
| `--lyflow-fg-0` … `--lyflow-fg-2` | `#e6e9ef` … `#6f7887` | 由亮到暗的三级文字 |
| `--lyflow-accent` / `--lyflow-accent-dim` | `#4a9eff` / `#2d6099` | 选中、连线、焦点 |
| `--lyflow-warn` | `#f0a020` | 警告 |
| `--lyflow-radius` | `6px` | 圆角 |
| `--lyflow-mono` | 一串等宽字体 | 代码与 id |

```tsx
<LyFlowEditor transport={t} theme={{ "--lyflow-accent": "#ff8a3d" }} />
```

CSS 由组件自己 `import`，宿主不用单独引样式文件。

## 快捷键

键盘监听挂在**编辑器根元素**上，不挂 `window`：宿主页面里的其它输入框不会被拦。
代价是根元素得拿得到焦点 —— 组件自己 `tabIndex={-1}` 并在挂载时 focus，
点进编辑器时也会收回焦点。焦点掉回 `document.body`（比如刚才那个输入框被卸载了）
时还有一条兜底：只接管「无主」的按键，宿主自己的控件仍然不受影响。

按 `?` 看完整键表（`src/lib/keymap.ts` 是唯一的那张表，处理器和面板都从它生成）。

## 还导出了什么

宿主要自己搭工具栏、状态栏或验收桥时用得上：

- stores：`useGraphStore`、`useUiStore`、`useManifestStore`、`useExecutionStore`、`useCacheStore`
- 动作：`startRun`、`cancelCurrentRun`、`requestPlan`、`schedulePlan`、`onNodeTransition`
- 工具：`levelOf`、`fullId`、`pathPrefix`、`layoutGraph`、`needsInitialLayout`、`decodeCloud`
- 类型：`GraphDoc`、`GraphNode`、`ExecutionEvent`、`OperatorDesc`、`RunOutputs` 等

**改图只能走 store 的语义化动作**（ADR-0002：GraphDoc 是唯一真实数据源）。
直接 `setState` 一份新 doc 会绕过撤销栈与校验。

## 目录

```
src/
  LyFlowEditor.tsx   组件本体：布局、文件流程、执行事件订阅、动效开关
  components/        画布、节点、参数控件、Inspector、3D 视图、工具栏、连线查看器（peek/）
  store/             graph / ui / manifest / execution / cache / peek
  lib/               类型校验、布局、映射、子图、参数、keymap、对话框注入、动效（motion.ts）
  transport/         Transport 契约 + tauri / http / static 三个实现
  types/             GraphDoc、manifest、ExecutionEvent 的 TS 镜像
  styles*.css        全部走 --lyflow-* 变量；动效的全部 CSS 在 styles.motion.css
test-server/         docs/http-transport.md 的最小 Node 实现，给 e2e:http 用
```

## 开发

```
pnpm --filter @lyflow/editor typecheck    # strict，check.ps1 里跑
pnpm host:dev                             # 起 examples/host-react
pnpm test-server -- --root <工作区> --cli <lyflow.exe>
pnpm e2e:http                             # 桩 + Chrome + 宿主，一条龙
```

不写 UI 单元测试（CLAUDE.md）：这一层的正确性由 `pnpm e2e` 与 `pnpm e2e:http` 端到端保证。

---

## 三条纪律

1. **GraphDoc 是唯一真实数据源**（ADR-0002）。React Flow 的 Node/Edge 是单向派生，
   交互结果一律翻译成 graph store 的语义化动作再写回。
2. **改图只能走 graph store 的语义化动作**，动作集合是封闭的。这样撤销栈的每一步
   都对应一个用户能理解的操作，而不是「节点数组第 3 项的 x 变了」。
3. **纯 UI 状态不进 GraphDoc、不进撤销栈**。选中、视口、抽屉开合、量测尺寸都在
   `src/store/ui.ts` 或组件的旁路缓存里。按 Ctrl+Z 只取消了一次选中，用户会认为撤销坏了。

历史用整份 doc 快照而不是 patch：immer 的结构共享让未改动的节点在新旧快照之间
共用同一份对象；而 patch 的路径基于数组下标，删一个节点会让之前所有 patch 失效。

撤销的**粒度**由控件决定，处理散落在 `src/components/ParamControls.tsx` 全文：输入框用本地 state、
失焦或回车才提交（一次编辑一条撤销），滑块按下时 `begin()`、松开时 `commit()`
（一次拖动一条撤销）。少了这层，拖一次滑块会往撤销栈里塞几十条，Ctrl+Z 就没意义了。

## 算子知识只有一处

前端不硬编码任何算子。唯一的「算子知识」是 `src/components/ParamControls.tsx` 里那张
`param.type → React 控件` 映射表。新增一种参数类型的成本是加一个 case（有界）；
新增一个算子的成本是零（ADR-0003）。

算子描述存在 manifest store，节点只记 `op` id、渲染时现查 —— 这是热重载的前提。
反过来在创建节点时把 manifest 快照进节点里，热重载就永久失效了。

## 参数的稀疏存储

`GraphDoc` 的 `params` 只记与 manifest 默认值**不同**的项（docs/graph-doc.md）。
这样默认值一改就能自动传播到所有老图，文件也更小、diff 更干净。

代价是读写都得走 `src/lib/params.ts`：读一个参数永远是「manifest 默认值 ← 节点覆盖值」
的合并，写一个参数在值回到默认时要把这一项**删掉**，而不是存一个等于默认的副本。
漏掉删键那一半，稀疏存储就静默退化成全量存储 —— 图照样能用，只是上面那些好处全没了。

## 节点字段的穿透

`bypass`（静音：不调 compute，输出从类型兼容的输入透传）是**执行语义**不是 UI 状态，
所以它在 `node.bypass` 而不是 `node.ui` 里，而且**进撤销栈**。前端必须一路带着它 ——
复制粘贴、迁移写回这类「重建节点对象」而不是就地改的路径漏掉它，用户存盘时静音就没了。
`GraphDoc.x` 同理，是老客户端打开新版本写的图时不丢数据的未知字段容器。

## stale 是读来的，不是算出来的

「哪几个节点已经过时」的判定**只在 C++**（[ADR-0007](../../docs/adr/0007-cache-authority.md)）。
`src/store/cache.ts` 只干两件事：doc 变了就 debounce 150 ms 调一次 `plan_graph`，
把结果和上一次 `run_started.nodes[].cacheKey` 对比。

前端算不出 IO 算子的 `externalKey`（路径没变但文件被覆盖了，缓存也得失效），
自己推一定会在那儿错 —— 而且错得很安静。`isNodeStale` 要求
「键变了 **且** 现在没有缓存」两个条件同时成立，只看键变化的话改回原值也会一直标着红。

## 迁移是一条可撤销的动作

打开老图时 `load_graph` 回的是 `{ doc, migrations }`，前端把那批动作交给
`applyMigrations`：一条撤销记录、置 dirty、toast 提示
（[ADR-0008](../../docs/adr/0008-migration-as-diagnostic.md)）。

不静默改写，是因为那是**用户的文档被改了**。撤销栈里必须有这一步，标题栏必须有那个 `*`。

## 动效

[docs/motion-plan.md](../../docs/motion-plan.md) 是设计，验收记录在 [docs/motion-acceptance.md](../../docs/motion-acceptance.md)。
动画库是 `motion`（只从 `motion/react` 导入），和 dagre、immer 一样是编辑器自带的依赖，宿主不用管。

**分工。** 一次性、有进出场的动效用 motion：节点进场、删除残影、连线生长、完成/出错闪光、
错误抖动、自动布局过渡。持续循环与 hover 用 CSS：running 的呼吸光、边上的数据流动、
节点/边/端口的 hover。几百条边的 hover 判定和流动循环放在 JS 里每帧跑不划算。

**只改两处。** 时长（fast 120 / base 200 / slow 320 ms）、缓动、motion 的预设和「现在要不要动」
都在 `src/lib/motion.ts`；CSS 变量 `--lyflow-motion-*`、全部 keyframes 与 hover 规则都在
`src/styles.motion.css`。别处不写时长、不写 `@keyframes`（`git grep @keyframes` 只该命中那一个文件）。
要不要动由根组件算好经 context 往下传（`useMotionEnabled()`）；关掉时根上挂 `lyflow-motion-off`。

**谁算「新出现」由 doc 的差分决定，不由挂载决定。** 画布同步订阅 graph store，同一张图
（`epoch`，`loadDoc` / `newDoc` 各加一）、同一层里比较前后的 id，新 id 记进一个短命集合
（`markEntering`，按到期时刻记），节点与连线组件挂载时问一句 `isEntering`。按挂载判定的话，
大图虚拟化下节点平移进视口就会重新挂载，满屏乱闪；打开文件也不该闪。

**动效状态不进 GraphDoc、不进 `data`。** 流动看目标节点的状态（`useNodeState`，只订阅状态串），
hover 在 ui store（`hoverNodeId` / `hoverEdge` / `hoverPaused`），都从 store 现查。塞进节点或边的
`data` 就得同步进 `lib/mapping.ts` 的 `sameNode` / `sameEdge`，而且每条事件都要重建整批对象。

**删除不等动画。** doc 立即改（撤销栈、e2e、store 都是即时的），画布在订阅回调里 —— 那时 React
还没重渲、DOM 还在 —— 克隆被删节点的 DOM 放进残影层淡出。克隆体剥掉全部 `id` 与 `data-*`，
`aria-hidden`、不接鼠标，免得被 e2e 或无障碍树当成真节点。

**自动布局只在用户触发时过渡。** 工具栏、Ctrl+L、右键「整理」经 `withLayoutTransition()` 调
`applyLayout`；doc 一次提交（一个撤销步），画布把临时位置逐帧喂给 React Flow，连线跟着走。
打开文件时的初始布局、脚本直调 `applyLayout` 都一步到位。

**元素动画用 `animateMini`，不用完整版的 `animate(element)`。** 后者给元素建一个 VisualElement，
终值在下一帧的渲染批次里才写回：`then` 里刚擦掉的 inline `opacity` 会被它重新写成 1，
静音、未被需要这些类的半透明就永久失效了（e2e 抓到过）。

**实时预览不闪绿。** 拖参数触发的 preview run 一秒能跑好几轮，结束时不播 done 闪光（看的是
`execution.preview`）；error 的闪光与抖动照播。

**关动效时视口也不动。** 编辑器自己调的 `fitView`（适配视图、整理之后、进子图、打开缺坐标的文件）
时长经 `viewportMs()` 取，关掉时是 0。

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
- **含端口的元素不许位移、缩放，hover 也不行**（docs/motion-plan.md A5）。React Flow 在节点挂载、
  尺寸变化、`updateNodeInternals` 时用 `getBoundingClientRect` 量端口相对节点的位置；作用在
  `.node`、`.node__body`、`.node-port` 上的 transform 只要赶上一次量测，连线端点就**永久**错位，
  直到下一次量测。所以节点进场只动 `opacity`、hover「抬起」只加阴影；允许 transform 的只有
  不含端口的 `.node__head`（错误抖动）、端口圆点的 `::before`、删除残影。
- **圆点本身也不能缩放 —— 「以圆心缩放不改变量测」是错的**（A6，验收后修正）。React Flow 取端口的
  `x/y` 用 `getBoundingClientRect`（含 transform），宽高却用 `offsetWidth/Height`（不含），连线端点在圆点
  **外缘**（源在右缘、目标在左缘）而不在圆心。圆点放大 1.35 倍的那一刻要是赶上量测（比如 hover 着端口时
  运行结束、节点多出一行状态条），端点会偏 1.75 px 左右，直到下一次量测。所以端口 hover 的放大与光晕画在
  圆点的 `::before` 上：伪元素不进 `getBoundingClientRect`，圆点的盒子一动不动。e2e 验收 8 专门在 hover
  端口期间把节点撑宽、逼 React Flow 重量一次，再断言端点。
- **拖线期间兼容端口的放大是老行为**，仍然缩放圆点本身，一律经 `--lyflow-handle-scale` 乘在
  `translateY(-50%)` 后面 —— 直接写 `transform: scale()` 会把居中的那半个身位顶掉（兼容端口、自动连线的
  候选端口原来就是这么往下掉的）。
- **inline 的 `strokeWidth` 压过一切 CSS。** 连线的线宽原来写在映射层的 `style` 里，于是
  `.selected` 的加粗从来没生效过。现在 `style` 只放颜色和虚线，线宽在 `styles.motion.css`。
- **自定义边要自己画命中路径。** React Flow 的 `BaseEdge` 在 `interactionWidth` 缺省时按 20 画
  `.react-flow__edge-interaction`；自定义边拿到的 prop 是 undefined，照抄 `interactionWidth ? … : null`
  就一条命中路径都没有了 —— 双击开 Peek、右键菜单、e2e 找线全靠它。
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

`sub:` 节点的 `OperatorDesc` 是**合成**出来的（`src/lib/subgraph.ts` 的
`augmentOperators`），因为子图定义随文档走而 manifest 是进程级的。
合成结果按 `doc.subgraphs` 的对象身份缓存，doc 不变就不重建。

## 大图性能

- 节点数超过 80 才开 React Flow 的 `onlyRenderVisibleElements`：
  小图下全量渲染的手感更好，开了之后平移会有一帧空窗。
- `node_state` / `node_progress` 按 **16 ms** 合并成一次 store 更新；
  `run_started` / `run_finished` 立刻 flush，所以「等运行结束再读状态」仍然准。
- 状态流水账（`window.__lyflow.transitions`）来自**事件**而不是 store 快照 ——
  合并窗口会把中间态吃掉，从快照推就断言不了「节点依次变色」。
