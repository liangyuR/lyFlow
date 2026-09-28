# 同一页面多个编辑器实例 —— 调研记录

> 状态：**调研记录，设计待专门讨论**（2026-09-29）。C 档「同页多编辑器实例」。
> 路径相对 `packages/editor/src`；行号是写这份记录时的位置，以符号名为准。

## 结论

editor 包现在只能单实例。ADR-0018（「stores 与 transport 都是模块级单例，两个实例会共用一份文档……等有需求再说」）
与 `phase-a2-acceptance.md`（「后挂载的那个的 transport 会赢」）写了这一点，`docs/embedding.md` 没写。
所有文档里都找不到宿主要多实例的记录（`gap-inspector-integration-design.md` 讲的「双 React 实例」是 peer 依赖问题，无关）。

## 1. 模块级单例

**zustand store 10 个**，都在模块作用域 `create()`：`store/graph.ts`、`ui.ts`、`execution.ts`、`manifest.ts`、`recipe.ts`、
`cache.ts`、`compare.ts`、`peek.ts`、`validation.ts`，以及 `lib/modal.ts` 的 `useModalStore`（旁边还有模块级 `let seq`）。
store 之间直接 import：graph 引 manifest / recipe / compare / ui，execution 引 cache / recipe / ui，cache、validation、peek、
compare、recipeFiles 也各自引别的 store。

**transport 与 dialogs**：`transport/index.ts` 用 `let current` 存当前实现，导出的 `transport` 是转发给它的 Proxy；
`lib/dialogs.ts` 同样模式。`LyFlowEditor.tsx` 在 render 期间调 `setTransport` / `setDialogs` / `setRunSceneId`，后挂载的覆盖前一个。
直接调 `transport.` 的文件 17 个（不含 transport 目录）。

**模块级缓存与状态**：
- `lib/cloudCache.ts`：`cloudCache`（按字节 LRU）、`pinnedRuns`
- `store/execution.ts`：notReady / served / touched / staged / flushTimer / latestSeq、transitionListeners、aggCache（按路径的 WeakMap）、subscription、runTicket、sceneId
- `store/cache.ts`：staleCache、timer、ticket；`store/validation.ts`：timer、ticket、cache；`store/manifest.ts`：snippetCache；`store/recipeFiles.ts`：loadTicket、loading
- `lib/roiThumbs.ts`：bounds、version、listeners；`lib/motion.ts`：entering、layoutIntent、flashListeners；`lib/peekCanvas.ts`：getters；`lib/preview.ts`：timer、lastNode
- 纯 WeakMap / 常量的缓存（如 `lib/subgraph.ts`、`lib/recipes.ts` 里的）不会串。

**全局副作用**：
- `document.body` 的 class：`is-resizing`（`LyFlowEditor.tsx`）、`param-dragging`（`NumberInput.tsx`）；CSS 写成 `body.is-resizing .app__canvas`（`styles.css`），一个实例拖动影响所有实例；类名也没有 `--lyflow-` 前缀，可能与宿主样式撞。
- 快捷键：`hooks/useShortcuts.ts` 挂在编辑器根元素上没问题，但另在 `ownerDocument` 上挂了「无主按键」监听，焦点在 body 时每个实例都响应。
- `NodePalette.tsx` 用 `document.querySelector(".react-flow__pane")`，多实例时拿到第一个的画布。
- localStorage 里记面板宽度用同一个键，影响不大。

## 2. 宿主怎么用编辑器

- `index.ts` 除了组件，还直接导出 9 个 store hook（没导出 modal）与 `startRun`、`requestPlan`、`loadRecipesFor` 等依赖单例的命令式函数。
- `LyFlowEditorProps`：`transport`（必填）、`dialogs`、`sceneId`、`animations`、`graphPath`、`onDocChange`、`className`、`theme`。
- `examples/host-react/src/main.tsx` 只挂一个；它的 `bridge.ts` 把全局 store 挂到 window 给 e2e 用，`ReactInstanceProbe` 直接调 `useUiStore`。`app/src/devbridge.ts` 同理。

## 3. 直接调 store 的次数（写记录时）

| store | 文件数 | `.getState()` | hook 调用 |
|---|---|---|---|
| ui | 33 | 127 | 58 |
| graph | 29 | 88 | 40 |
| recipe | 11 | 24 | 21 |
| execution | 16 | 17 | 30 |
| cache | 8 | 13 | 8 |
| compare | 7 | 12 | 3 |
| manifest | 21 | 11 | 31 |
| peek | 8 | 6 | 9 |
| modal | 3 | 3 | 3 |
| validation | 4 | 2 | 5 |

`getState()` 共 303 处；components + LyFlowEditor 29 个文件、lib / hooks 10 个用到 store，test 另有 7 个。

## 4. 方案（待讨论）

**A. React context + 每实例一套 store**（约 2–3 人周，约 45 个源文件 + 7 个测试文件，另补 e2e）
- `createEditorInstance(transport, dialogs)` 用 `createStore` 建 10 个 store，transport、dialogs、各类缓存 / ticket / timer 都进实例对象，经 `<EditorContext>` 下传。
- hook 改成 `useGraph(sel)`（内部 `useStore(ctx.graph, sel)`）；303 处 `getState()` 与 lib 里的命令式函数改成收实例参数；store 之间的 import 改成经实例互相引用。
- body class 改挂实例根元素、`querySelector` 限在根元素内、无主按键只派发给最近获得焦点的实例。
- `index.ts` 的导出方式会变，破坏性，要给旧导出留一层兼容。

**B. iframe / 独立 JS realm 隔离**（约 3–5 人日，主要在宿主侧与桥接层）
- 每个实例一个 iframe，各自加载一份包，编辑器代码基本不动。
- 代价：transport 与宿主桥接改走 postMessage；主题与尺寸要同步；宿主不能再直接调 store hook；每个实例各加载一份 three、React Flow 等依赖，点云缓存也各一份（每份上限 256 MB）。

**更便宜的过渡**
- 同一时刻只挂一个：宿主做成 tab，切换时卸载当前实例并 reset（`LyFlowEditor.tsx` 已有 `useCacheStore.getState().reset()` 先例），再 `setTransport`。要补一个统一的 `resetEditor()`，约 1–2 人日。
- 先打地基：只把 transport、dialogs、sceneId 放进 context，并修掉 body class、document 查询、无主按键这几处全局副作用；A 方案以后逐个 store 渐进迁移。
- 挂第二个实例时直接报错：dev 模式下检测到第二个 `LyFlowEditor` 挂载就 `console.error`，别再静默共用一份文档。

调研时的倾向：大改暂缓（没有宿主提需求），先做约 1 天的防呆（第二个实例报错、body class / DOM 查询收回实例根、文档写明一页一个）。
