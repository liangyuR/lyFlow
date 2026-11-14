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

## 安装

编辑器以 TypeScript 源码 tarball 随 LyFlow 发布，不发布到 npm registry。使用与运行库配套的发布资产安装，例如：

```powershell
pnpm add ./lyflow-editor-vX.Y.Z.tgz
```

仓库内开发通过 workspace 依赖引用。资产名、版本与发布规则见 [发包指南](../../docs/deployment/packaging.md)。

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
能编译 TS/TSX 的打包器（Vite、Next、Rspack 都行）。宿主的 `tsc` 会连编辑器源码一起查，所以
`pnpm --filter @lyflow/editor typecheck` 用 React 18 与 19 两套类型各查一遍（`tsconfig.react19.json`）。

## Props

```ts
interface LyFlowEditorProps {
  transport: Transport;              // 必填
  dialogs?: EditorDialogs;           // 文件对话框，宿主提供
  graphPath?: string;                // 挂载或路径变化后打开这张图
  sceneId?: string | null;            // 宿主点云会话，运行时转给后端输入提供者
  onDocChange?: (doc: GraphDoc, dirty: boolean) => void;
  theme?: Record<string, string>;    // Qing Steel token 或兼容的 --lyflow-* 变量覆盖
  className?: string;
  windowChrome?: FlowWindowChrome;    // 宿主提供的标题栏窗口按钮与拖动/最大化回调
  animations?: boolean;              // 画布动效，默认 true
  topologyLocked?: boolean;          // 锁住拓扑、只许调参，默认 false
}
```

完整定义见 [LyFlowEditor.tsx](src/LyFlowEditor.tsx)。`onDocChange` 在文档或合并后的 dirty 状态变化时回调，包括复用活动标签打开新文档；宿主可据此更新自己的标题栏和保存提示。

`sceneId` 经运行选项交给宿主的输入提供者，不修改图中的路径参数。`windowChrome` 的类型由包导出，包含 `controls`、`focused`、`onDragStart()` 和 `onToggleMaximize()`；不给时不接管原生窗口操作。宿主装配示例见 [嵌入指南](../../docs/embedding.md)。

当前编辑器的 store、Transport 与对话框配置是模块级单例，同一页面只支持一个 `<LyFlowEditor>`。工作流标签是该实例内的多个文档，不等于多个独立编辑器组件。

`animations={false}` 关掉画布动效：节点进出场、完成/出错的闪光、连线生长、自动布局的过渡
全部直接落到终态，CSS 的循环与过渡一并停掉。系统设了「减少动态效果」
（`prefers-reduced-motion: reduce`）时等同于 false，不用宿主自己判断。信息不会跟着丢：
正在流数据的边关了动画仍是一条静态高亮。

`topologyLocked` 给「流程调试页」这类只许调参的宿主页面：图的结构冻结，参数照常调。拦在 graph store 里，
快捷键、粘贴、右键菜单、拖线都过同一道关 —— 被拦的动作什么都不做、返回各自的「没做」（null / false / 空数组 /
不通过的 `ConnectVerdict`），`lastRejection` 写成 `拓扑已锁定`，编辑器把它弹成提示。

- 拦下的：加 / 删 / 粘贴 / 复制节点（`addNode`、`addNodeAuto`、`insertSnippet`、`deleteNodes`、`deleteNodesHealing`、
  `pasteNodes`、`duplicateNodes`，含剪切与 Ctrl+D）；连线（`connect`、`disconnect`、`reconnectEdge`、`insertOnEdge`，
  含拖线松在空白处建节点）；`replaceNodeOp`、`inlineLibrary`、`composeSubgraph`、`dissolveSubgraph`、保存到库；
  `setBypass`（静音改的是数据流）；图参数与图级输出的结构（`promoteToGraphParam`、`bindToGraphParam`、
  `unbindFromGraphParam`、`removeGraphParam`、`renameGraphParam`、`setGraphParamSpec`、`moveBaseEditToRecipe`、
  `promoteParam`、`unpromoteParam`、`markGraphOutput`、`removeGraphOutput`）。`applyMigrations` 只拦带连线改写
  （edits）的那几条，整条不写回；只改版本与参数的照写。`restoreParams` 照常恢复参数，但不恢复静音。
- 照常的：`setParam` / `setParamMany` / `restoreParams`、`editGraphParamValue` / `setGraphParamDefault`、配方动作、
  挪位置与布局、折叠、改名、运行（整图、到某节点、仅此节点、强制重算、取消）、预览、检查器、日志、进出子图、查找节点、
  适配视图、打开与保存。
- 界面：算子面板（连同片段）与算子搜索弹层不渲染，画布 `nodesConnectable` / `edgesReconnectable` 关掉，
  节点与连线右键菜单里改结构的项置灰，空白处右键没有「添加算子…」。
- 锁上的那一刻清空撤销 / 重做栈：Ctrl+Z 只走得到锁上之后的调参。

## Transport

三个现成实现：

| 实现 | 用途 |
|---|---|
| `new TauriTransport()` | 跑在 Tauri 壳里，每个方法对一条 `#[tauri::command]` |
| `new HttpTransport(baseUrl, token?)` | 跑在浏览器里，对着 [`docs/http-transport.md`](../../docs/http-transport.md) 的后端 |
| `new StaticTransport(manifestUrl?)` | 只读：读一份 dump 出来的 manifest，什么都跑不了。没有后端时也能把界面渲染出来 |

Rust/Tauri 宿主通过 `lyflow_lib::host::attach`（`host` feature）装配 managed state 与启动逻辑，并用 `lyflow_handler!` 注册 LyFlow 命令及宿主自己的命令。命令名、事件名与现成 `TauriTransport` 配套；接入方式见 [嵌入指南](../../docs/embedding.md)，以 [host.rs](../../bridge/src/host.rs) 的实际接口为准。

自定义后端须实现 [Transport](src/transport/types.ts)。Transport 同时包含引擎执行与宿主文件服务，并非每个方法都直接对应一个 C ABI 函数。

| 能力 | 必须保持的契约 |
|---|---|
| 描述与校验 | `getManifest`、`getCoreInfo`、`validateGraph` 和 `planGraph` 返回权威描述、诊断与计划。校验和计划的 `params` 必须与实际运行使用的顶层图参数一致。 |
| 执行与结果 | `runGraph` 返回 runId，状态通过 `onExecutionEvent` 到达；`cancelRun` 取消请求。`getOutputInfo`、`getRunOutputs` 和二进制取数方法按 runId 与展开后的节点路径读取结果。 |
| 运行选项 | 原样传递 `targets`、`isolate`、`force`、`mode`、`previewMaxPoints`、`previewBudgetMs`、`sceneId`、`params`。`isolate` 与 preview 不组合；部分运行和强制重算语义见 [嵌入指南](../../docs/embedding.md)。 |
| 文件与备份 | 图文件、最近文件、备份、配方文件、二进制写入由宿主实现。配方 I/O 文本进出，格式和失配由编辑器校验；路径限制见 [配方契约](../../docs/recipe.md)。 |
| 库与更新 | 库目录、库算子定义、导入器与刷新状态由宿主提供；`onManifestUpdated` 和 `onCoreReloadFailed` 传递重新加载结果。 |
| 可选能力 | `getOutputImage`、`loadCloudFile`、`listSnippets`、`untitledBackupPath` 及 Python 管理方法允许不实现；编辑器按可用能力降级。图像预览缺失时只显示已有统计信息，模板底图缺失时只画 ROI 框，Python 未支持时给出能力提示。 |

Python 管理方法为 `getPythonSettings`、`setPythonSettings`、`refreshPythonNodes`。宿主通过 `supported` / `editable` 表达支持状态和只读配置，详见 [Python 节点指南](../../docs/python-nodes.md)。

C ABI 签名以 [c_api.h](../../core/include/lyflow/c_api.h) 为准。产品版本与 ABI 使用同一个完整 `X.Y.Z` 字符串，C++/Rust 客户端按主、次版本系列判定运行时兼容：同系列补丁兼容，跨次版本拒绝加载。旧数字 ABI 不能作为当前接口契约。资产中的版本和源码修订仍须满足 [发包契约](../../docs/deployment/packaging.md)。

点云走二进制，绝不 JSON（ADR-0006）：`getOutputCloud` 返回的 `ArrayBuffer`
布局见 [http 契约](../../docs/http-transport.md)的「结果」一节，
解码用包里导出的 `decodeCloud`。

## 对话框

打开、另存这几件事要弹系统对话框，编辑器不知道怎么弹，
由宿主注入（有没存的改动时那一问「保存 / 不保存 / 取消」、恢复备份那一问「恢复 / 丢弃 / 取消」是编辑器自己画的，`lib/unsaved.ts`、`lib/autosave.ts`，不经宿主 —— `confirmRestore` 只在编辑器整个卸掉、画不出对话框时才用；
宿主关窗口前应调导出的 `resolveAllUnsaved("关闭窗口")` 检查全部工作流，`resolveUnsaved` 只处理当前工作流：

```ts
import type { EditorDialogs } from "@lyflow/editor";

const dialogs: EditorDialogs = {
  pickOpenPath: () => …,          // 返回 null 表示用户取消
  pickSavePath: (suggested) => …,
  confirmRestore: (path, message) => …,
  pickPath: (req) => …,           // 可选：参数表单的「浏览…」、3D / 查看器导出 PNG、库目录（不给：浏览不摆、导出退回浏览器下载）
  pickRecipePath: (mode, name) => …, // 可选：配方的导入（open）/ 导出（save）
};
```

配方的导入导出先找 `pickRecipePath`，其次 `pickPath`，都没有就在编辑器里让人输一个路径（HTTP 宿主填工作区里的相对路径）。
起配方名、确认删除、「文件已被外部修改」这几个对话框是编辑器自己画的（`lib/modal.ts`），不经宿主。

不给 `dialogs` 时，默认宿主不能选择打开或保存路径，会给出不支持文件对话框的提示；恢复确认的卸载兜底使用 `window.confirm`。3D 导出可退回浏览器下载。桌面实现见 [app/src/dialogs.ts](../../app/src/dialogs.ts)。

## 主题

界面依赖 Qing Steel **v0.11.2** 发布包，统一使用上游颜色、字体、圆角、控件和动效刻度。默认深色，根容器使用 `.qs-scheme-dark`；常驻面板按 surface 色阶分层，临时菜单和对话框使用上游浮层角色。

通过 `theme` prop 覆盖 Qing Steel token。原有 `--lyflow-*` prop 会映射到对应的上游角色；两者同时传入时，上游 token 优先。CSS 定制新界面时请在编辑器根选择器上覆盖上游 token。

| 兼容变量 | Qing Steel 角色 | 用处 |
|---|---|---|
| `--lyflow-bg-0` … `--lyflow-bg-3` | `--surface` / `--surface-low` / `--surface-mid` / `--surface-high` | 四层表面 |
| `--lyflow-border` / `--lyflow-border-strong` | `--outline-variant` / `--outline` | 分隔线、边框 |
| `--lyflow-fg-0` / `--lyflow-fg-1` | `--on-surface` / `--on-surface-variant` | 正文、次要文字 |
| `--lyflow-accent` / `--lyflow-accent-dim` | `--primary` / `--primary-container` | 主操作、选中、焦点 |
| `--lyflow-ok` / `--lyflow-warn` / `--lyflow-danger` | `--ok` / `--warn` / `--ng` | 成功、警告、错误 |
| `--lyflow-recipe` | `--tertiary` | 配方覆盖 |
| `--lyflow-radius` / `--lyflow-mono` | `--r-md` / `--font-mono` | 输入圆角、等宽字体 |

```tsx
<LyFlowEditor transport={t} theme={{ "--primary": "#7FD4F0", "--primary-container": "#004E63" }} />
```

CSS 由组件自己 `import`，宿主不用单独引样式文件。

## 工作流标签与导航

顶部标签对应独立工作流；新建或打开会保留其它标签的未保存内容，重复打开同一路径只激活已有标签。切换保留节点、配方、撤销/重做、选择、视口与已完成的运行结果；当前工作流运行中或配方读取中，先完成或停止操作再切换。

标签支持方向键/Home/End 切换、Delete 关闭，双击或标签有焦点时按 F2 重命名。关闭未保存标签与窗口都会询问保存、不保存或取消；窗口关闭检查全部标签。工具栏提供「保存 ▾」，下拉包含另存为、重命名；新建、打开与最近文件集中在左侧工作流面板。

左侧导航展开节点库、工作流列表，并进入参数、日志和帮助。`topologyLocked` 下节点/工作流管理入口隐藏，参数、日志、帮助仍可访问。完整手势见 [操作指南](../../docs/interaction-checklist.md)。

多文档未保存内容另外备份到当前浏览器/WebView配置的 IndexedDB，重新打开时询问恢复。它与原有图文件旁的 `~` 备份并存；不是自动保存到原文件，备份恢复不包含撤销历史和运行结果。选择稍后会保留原工作区备份；本次会话仍有原来的活动图文件备份。

## 快捷键

画布使用 Qing Steel 深色背景与点阵。空白处左键拖动平移，节点上左键拖动移动节点，右键打开菜单。按住 Ctrl 拖动框选（与框相交的节点即可选中），Ctrl+Shift 拖动追加选区；Ctrl/Shift 单击仍可加选或取消单个节点。

选中多个节点后，选区上方显示操作条，可点击「运行选中」或按 Shift+F5。选区运行只计算这些节点及其必要上游，复用缓存，不运行未选中的下游，也不合并待自动运行的其它目标；F5 仍运行整图。

键盘监听挂在**编辑器根元素**上，不挂 `window`：宿主页面里的其它输入框不会被拦。
代价是根元素得拿得到焦点 —— 组件自己 `tabIndex={-1}` 并在挂载时 focus，
点进编辑器时也会收回焦点。焦点掉回 `document.body`（比如刚才那个输入框被卸载了）
时还有一条兜底：只接管「无主」的按键，宿主自己的控件仍然不受影响。

按 `?` 看完整键表（`src/lib/keymap.ts` 是唯一的那张表，处理器和面板都从它生成）。

## 还导出了什么

宿主要自己搭工具栏、状态栏或验收桥时用得上：

- stores：`useGraphStore`、`useUiStore`、`useManifestStore`、`useExecutionStore`、`useCacheStore`、`useRecipeStore`
- 配方：`selectRecipe`、`recipesDirty`、`loadRecipesFor`、`recipesLoaded`、`importRecipeFrom`、`exportRecipeTo`，
  纯函数 `specDigest`、`recipeReport`、`recipeDirOf`、`serializeRecipe`、`parseRecipeText`（改配方仍走 graph store 的动作）
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
  styles*.css        Qing Steel 主题角色；兼容 --lyflow-* prop；动效 CSS 在 styles.motion.css
test-server/         docs/http-transport.md 的最小 Node 实现，给 e2e:http 用
```

## 开发

```
pnpm --filter @lyflow/editor typecheck    # strict，check.ps1 里跑
pnpm host:dev                             # 起 examples/host-react
pnpm test-server -- --root <工作区> --cli <lyflow.exe>
pnpm e2e:http                             # 桩 + Chrome + 宿主，一条龙
```

纯逻辑和 store 契约使用 `pnpm --filter @lyflow/editor test`；真实鼠标、键盘、焦点及宿主链路使用 `pnpm e2e`、`pnpm e2e:http`。这些是验证入口，不代表任意修订已经通过。选择范围与跳过项要求见 [测试地图](../../docs/testing.md)、[E2E 说明](../../scripts/e2e/README.md)。

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
自己推一定会在那儿错 —— 而且错得很安静。`staleLocalIds` 要求
「键变了 **且** 现在没有缓存」两个条件同时成立，只看键变化的话改回原值也会一直标着红。

读它的有：节点上的虚线框（`OperatorNode` 的 `is-stale`）、运行按钮的预判（`NodeRunButton`，以及 `lib/nodeRun.ts` 里
「仅此节点」的 `nodeRunAvailability`），和预览左上角的「参数改过了 · 画面是上一次的结果」
（`ViewerStatus.tsx` 的 `StaleBadge`，跑着的时候不显示）。拖参数的 preview run 除了改的节点，还带上正看着的、
在它下游的节点（预览上的那个 —— 钉住的，没钉就是选中的 —— 与对比里没冻结的 B，`lib/nodeRun.ts` 的 `previewTargets`）—— 不然钉在下游时拖上游的参数，
画面一动不动（ADR-0011 修订）。

## 迁移是一条可撤销的动作

打开老图时 `load_graph` 回的是 `{ doc, migrations }`，前端把那批动作交给
`applyMigrations`：一条撤销记录、置 dirty、toast 提示
（[ADR-0008](../../docs/adr/0008-migration-as-diagnostic.md)）。

不静默改写，是因为那是**用户的文档被改了**。撤销栈里必须有这一步，标题栏必须显示未保存标记。

## 动效

动画库是 `motion`（只从 `motion/react` 导入），和 dagre、immer 一样是编辑器自带的依赖，宿主不用管。

**分工。** 一次性、有进出场的动效用 motion：节点进场、删除残影、连线生长、完成/出错闪光、
错误抖动、自动布局过渡。持续循环与 hover 用 CSS：running 的呼吸光、边上的数据流动、
节点/边/端口的 hover。几百条边的 hover 判定和流动循环放在 JS 里每帧跑不划算。

**集中维护。** JS 动效的时长、缓动、transition 预设和启用判定在 [motion.ts](src/lib/motion.ts)；CSS 动效在 [styles.motion.css](src/styles.motion.css)，通过 `--lyflow-motion-fast/base/slow` 引用 Qing Steel 的 `--dur-short/medium/long`，缓动使用上游主题角色。修改动效时同步核对 JS 与 CSS，不在各组件散写时长或 keyframes。
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

**自动布局只在用户触发时过渡。** 选区操作条、Ctrl+L、右键「整理」经 `withLayoutTransition()` 调
`applyLayout`；doc 一次提交（一个撤销步），画布把临时位置逐帧喂给 React Flow，连线跟着走。
打开文件时的初始布局、脚本直调 `applyLayout` 都一步到位。

**元素动画用 `animateMini`，不用完整版的 `animate(element)`。** 后者给元素建一个 VisualElement，
终值在下一帧的渲染批次里才写回：`then` 里刚擦掉的 inline `opacity` 会被它重新写成 1，
静音、未被需要这些类的半透明可能失效。

**实时预览不闪绿。** 拖参数触发的 preview run 一秒能跑好几轮，结束时不播 done 闪光（看的是
`execution.preview`）；error 的闪光与抖动照播。

**关动效时视口也不动。** 编辑器自己调的 `fitView`（适配视图、整理之后、进子图、打开缺坐标的文件）
时长经 `viewportMs()` 取，关掉时是 0。

## 节点运行按钮

用户操作见 [操作指南](../../docs/interaction-checklist.md)，回归入口见 [测试地图](../../docs/testing.md)。每个节点标题栏右端一个运行按钮
（`components/NodeRunButton.tsx`），右键菜单里有同一组的三项；两处的判据、状态与动作都在
`src/lib/nodeRun.ts`，不许各写一份。

- **单击 = 智能运行**：`startRun(doc, path, { targets: [展开后的 id] })`，即「运行到此」—— 本节点 + 缺结果或
  过时的上游，有缓存的复用，下游不进计划。**Shift+单击** 再加 `force: [id]`：本节点跳过缓存真跑一遍。
  右键三项：运行到此节点（= 单击）/ 强制重算此节点（= Shift+单击）/ 仅此节点（用现有上游）（= `isolate`，
  上游不齐时置灰写明缺谁）。按钮只在本节点有编辑期校验 error 时置灰。
- **hover 预告**（`title` 与 `data-run-upstream`）：将一并运行的上游 = 当前层里的祖先中「下次会真算」的那些；
  判据优先用 `plan_graph` 的 `cached`（ADR-0007），拿不到时退回执行状态。全部就绪且本节点
  命中缓存时写「已是最新（命中缓存）—— Shift+点击强制重算」。每次正式运行收场 `LyFlowEditor` 立刻重编一次计划
  （不等 debounce），否则刚跑完的节点仍被当成「没缓存」。
- **带 targets 的运行都不清空节点表**：计划里的节点照常先亮成排队中、跟着事件更新；计划外的留着，
  收场时按 `run_finished.attached` 决定留不留。
- **单节点运行（isolate）另有一条**：`beginRun` 带着 isolate 时留着上一次的 `nodes`，`run_started` 只追加
  cacheKey（`extendRanWith`，计划外的下游还要按它们自己上次的键判 stale），不在 isolate 里的节点的
  `node_state` 一律不落库 —— 否则「只跑 b」会把 a 刷成「已缓存」、耗时归零。流水账
  （`onNodeTransition`）同样只记 isolate 里的节点。
- **计划外节点的输出靠 core 挂上**。`run_finished.attached` 列出按新 runId 还取得到输出的节点；
  收场时节点表里这次没有事件（isolate 下是没命中缓存，`served`）、又不在 attached 里的，一律退回 idle ——
  否则它们显示「完成」，点开 3D 视图或 Edge Peek 却是空的。stale 判定不变。老 core 不带 attached，就全部照旧。
- **上游不齐：不弹框。** `run_finished.diagnostics` 里的 `upstream_not_ready`（外加执行期才撞上的惰性上游）
  拼成一条 warn toast，缺结果的上游在当前层各闪一下红光（`lib/motion.ts` 的 `flashNodesLocate`，
  复用 S2 的 error 光晕但不抖，节点上挂 `data-flash="locate"`）。它们没有失败，不标红。
- **停与抢占。** 只有这次运行是这个节点自己的按钮（或同义的「运行到此」）发起的（`execution.targets` 恰好是它）才显示 ■、点了是
  `cancelCurrentRun`；别的运行在跑时它照常显示状态（包括 running 的进度环），点了是发起新运行抢占旧的。
- **标题栏改成了 flex。** 标题 `flex: 1; min-width: 0` 并在自己身上省略，徽标与按钮排在右端；按钮的命中区
  20×20 用负外边距收回，标题栏不因此变高。按钮不含端口，hover 的放大画在它自己的 SVG 上，避免影响端口量测。

## 实现约束

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
- **快捷键精确匹配修饰键。** Shift 必须精确匹配，避免 `Shift+F5` 被 `F5` 抢先处理；例外是 `?` 这类本来就需要 Shift 才能输入的符号。
- **`onReconnectEnd` 的第四个参数各版本形态不一。** 猜错的后果是把一条刚重连好的边
  直接删掉。改成在 `onReconnect` 里自己记一笔，`onReconnectEnd` 只看那一笔。
- **拖节点到连线上：容差不能只看中心距离。** 12 px 是**画布坐标**，画布缩到 50%
  时只剩 6 个屏幕像素，用户根本够不着。判据改成「连线穿过节点矩形」，
  中心距离只作为擦边时的余量。
- **含端口的元素不许位移、缩放，hover 也不行**。React Flow 在节点挂载、
  尺寸变化、`updateNodeInternals` 时用 `getBoundingClientRect` 量端口相对节点的位置；作用在
  `.node`、`.node__body`、`.node-port` 上的 transform 只要赶上一次量测，连线端点就**永久**错位，
  直到下一次量测。所以节点进场只动 `opacity`、hover「抬起」只加阴影；允许 transform 的只有
  不含端口的 `.node__head`（错误抖动）、端口圆点的 `::before`、删除残影。
- **圆点本身也不能缩放 —— 「以圆心缩放不改变量测」是错的**。React Flow 取端口的
  `x/y` 用 `getBoundingClientRect`（含 transform），宽高却用 `offsetWidth/Height`（不含），连线端点在圆点
  **外缘**（源在右缘、目标在左缘）而不在圆心。圆点放大 1.35 倍的那一刻要是赶上量测（比如 hover 着端口时
  运行结束、节点多出一行状态条），端点会偏 1.75 px 左右，直到下一次量测。所以端口 hover 的放大与光晕画在
  圆点的 `::before` 上：伪元素不进 `getBoundingClientRect`，圆点的盒子一动不动。相关交互回归应在 hover 端口时触发尺寸变化，验证连线端点仍与端口相接。
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
- **3D 视图的画布要在 CSS 里显式铺满宿主。** `renderer.setSize(w, h, false)` 只设绘图缓冲（w·DPR 像素）、
  不写 style；画布没定宽高时按缓冲的像素数当 CSS 尺寸摊开。100% 缩放下两者相等看不出来，150% 下画布比宿主
  大一半、右下三分之一被裁掉：画面中心跑到可见区右下角，2D 拖框层按画布尺寸摆的框有的落在可见区外、拖不到。
  `.viewer__canvas canvas` 现在与 peek 的 CloudView 一样绝对定位、宽高 100%。
- **ResizeObserver 会给刚卸掉的元素报一次 0×0。** 虚拟列表的行滚出视口被卸掉后，摘观察的 effect 跑之前
  观察回调可能先到；把这个 0 当行高记下，会让偏移、`scrollToKey` 与视口补偿失效。回调里跳过 `offsetParent === null`（不在排版里）的行，不依赖卸载与观察回调的先后顺序。

## 图参数

数据格式和绑定规则见 [GraphDoc](../../docs/graph-doc.md) 与 [配方契约](../../docs/recipe.md)。

- **纳入配方 = 提升为图参数**：参数行右键「纳入配方（提升为图参数）」→ `promoteToGraphParam`。在子图里做就是整条
  逐层提升链，一个动作一次撤销。其余动作（绑定、解除、删除、改名、改 default、改规格）都在 graph store。
- **被绑定的参数只有一个写入处**：`setParam` 命中被图参数提供的参数（`lib/graphParams.ts` 的 `resolveGraphBinding`）
  时转给 `editGraphParamValue`，按当前配方写进配方或 default。Inspector、2D 拖框、粘贴、重置都经 `setParam`，
  所以不必各判一遍；显示走 `withBoundValues`（节点的浅拷贝，被绑定的键换成图参数的有效值）。
- **有效值 = default ← 当前配方覆盖**：读值一律经 `store/recipe.ts`（`runParamsOf`、`useGraphParamOverrides`）。
  当前配方提供稀疏覆盖；运行、实时预览、实时校验与计划均把合成取值经 `RunOptions.params`（validate / plan 的同名参数）交给后端。
- **dirty 是「doc 不是存盘时那一份」**：graph store 记着 `savedDoc`（对象身份），撤销回到保存点时 dirty 复原。
  存盘是异步的，`markSaved(path, doc)` 记下真正写下去的那一份。

## 参数面板

使用方式见 [操作指南](../../docs/interaction-checklist.md)；参数规格见 [OperatorManifest](../../docs/operator-manifest.md)。

- **开关与布局**：左侧导航「参数」或 `Ctrl+Shift+P`（键表里的 `paramPanel`）。状态在 ui store 的 `paramPanel`
  （`open / maximized / tab / viewerOpen`），纯 UI、不进 doc 不进撤销。开着时右侧那一列换成面板、**Inspector 不渲染**；
  3D 视图还在那一列顶上，收成一条「预览」标题栏（ROI 行「拖框」会展开它）—— Viewer3D 始终是同一个实例，切换不重建
  WebGL。面板宽度与 Inspector 宽度各记各的，面板那份记在 `localStorage["lyflow.paramPanel.width"]`。最大化把画布压成
  0 宽（不卸载：节点尺寸与端口量测都还在）。面板关着时 Inspector 顶上的图参数简表照旧。
  预览也能最大化（预览栏的 □ 或 `Shift+Space`，Esc 还原；ui store 的 `viewerMaximized`）：画布、算子面板、检查器 / 参数面板
  同样只是藏起来；画布看不见时动画布的快捷键（删除、复制、搜索……）不响，定位到节点、打开参数面板、换图、整理布局先还原。
- **数据模型在 `lib/paramPanel.ts`**（纯函数、有单测）：行的全集 = 图参数 + 当前层每个节点的**可见**参数 + 子图实例展开进
  定义的节点（按实例各一份，因为绑定链按实例不同）。chip 的判据：已改动 = 与算子默认不同（图参数行比第一个绑定目标的
  默认）；配方 = 图参数本身与被它提供的行；诊断 = 带 paramPath 的校验诊断或上次运行的错误；类型 = param.type。
  搜索按空白切词、每个词都要出现在「参数名 label 节点标题 展开后的节点 id 值的文本」里。过滤生效时折叠一概不算。
- **虚拟化**：`components/VirtualList.tsx`，按类型估行高、ResizeObserver 量真值、视口上方的行变高时补 scrollTop。
  行组件 `memo`；诊断里「上次运行的错误」拍成一个字符串键再订阅，运行时每 16 ms 一批的事件不会重建模型。
- **编辑走的还是 store 的动作**：`setParam(nodeId, name, value, at?)` 多了一个可选的层级 `at`（展开进子图定义的那几行在
  更深一层）；`promoteToGraphParam` / `promoteParam` / `unpromoteParam` 同样。`ParamControl` 的 `path` prop 把层级带给
  右键菜单与 live preview（预览目标按「相对当前层」的 id 拼）。
- **配方行标记**：最左一条 `.prow__stripe`（`.prow.is-recipe-override` 时画橙色竖条）、label 后的 `.prow__tags`。
- **transform / curve**：`components/TransformControl.tsx`、`components/CurveControl.tsx`，纯逻辑在 `lib/transform.ts`、
  `lib/curve.ts`（与 core 的 `checkCurveValue` / `evaluateCurve` 同一套判据与插值）。数字框抽成了 `components/NumberInput.tsx`。
  `valueEquals` 现在也比普通对象（curve 的值），与键顺序无关 —— 稀疏存储删不删键靠它。

## 配方

文件格式、覆盖规则与失配处理见 [配方契约](../../docs/recipe.md)。

- **三层**：`lib/recipes.ts` 是纯函数（文件格式、目录约定、配方名、specDigest、四类失配与修复建议；有单测，并对着
  `schema/fixtures/recipes/` 的共享夹具，与 Rust 实现共用）；`store/recipe.ts` 存配方集合、当前配方与存盘簿记；
  `store/recipeFiles.ts` 是 I/O（打开图时读 `<图名>.recipes/`、Ctrl+S 写回、外部修改检测、自动备份、导入导出）。
- **撤销**：改配方集合的动作都在 **graph store** 里（它握着撤销栈），`HistoryEntry` 同时快照 `doc` 与 `recipes`；
  `transact(label, docRecipe, recipesStep)` 一步里可以同时改两样（「写回基础」「改为只在本配方生效」）。`begin/commit` 也快照配方，
  拖滑块改配方里的值合成一条。**当前配方不进撤销栈**，按配方的内存 id 跟随（改名、撤销改名时它还是当前配方）。
- **dirty 与 graph store 同一个判法**：`set !== saved`（对象身份），撤销回到保存点就不脏。工作流标签的未保存小圆点与宿主的 `onDocChange`
  是图与配方合并的；存盘簿记 `savedFiles`（配方 id → 文件名）让「改名」存成 rename、「删掉」存成 delete；`diskText`（文件名 →
  上次读 / 写的文本）是外部修改检测的依据。
- **配方跟着图走**：LyFlowEditor 订阅 graph store，`epoch` 变了（打开、新建）就 `loadRecipesFor(filePath)`，同一张图只是换了路径
  （第一次存盘）就 `followGraphPath`。所以宿主与脚本直接 `loadDoc(doc, path)` 也会读配方目录。异步读完时把历史里还指着
  「载入前那份空集合」的快照换成读到的（`rebaseHistoryRecipes`）。
- **写值的唯一入口**仍是 `editGraphParamValue`：选着配方写配方、选着基础写 default；`setParam` 命中被绑定的参数时
  转给它。选着配方改没纳入的参数时，`setParam` 顺手记一笔 `baseEdits`（为「改为只在本配方生效」保留改动前的值）。
- **失配阻止运行**在 `startRun` 里：当前配方有 ①–③ 就 failRun 并写明哪个配方的哪几处；显式给了 `params` 的运行不受约束。
- **界面**：`components/RecipeMenu.tsx`（工具栏下拉框）、`RecipeMatrix.tsx`（矩阵）、`RecipeManager.tsx`（管理与失配报告）、
  `recipeActions.ts`（问名字、确认、选文件，然后调 store 动作）、`Modal.tsx`；样式在 `styles.recipe.css`。
  按节点页的行（`ParamPanel.tsx`）用 `.prow__stripe` 与 `.prow__tags` 画覆盖标记。
- **窗口桥**：`window.__lyflow.recipes`（`loaded` / `importFrom` / `exportTo` / `specDigest` / `autosave` / `restoreAutosave`）
  与 `snapshot().recipe`（当前配方、取值、配方集合、合并后的脏标记）。

## 配方的 CLI / MCP / 宿主一侧

CLI `--recipe`、MCP 的 `recipe` / `list_recipes` 与宿主按名字切换，使用 [配方契约](../../docs/recipe.md) 和 [嵌入接口](../../docs/embedding.md)。

`lib/recipes.ts` 与 Rust 的 `bridge/src/recipe.rs` 实现同一套规则，使用 `schema/fixtures/recipes/expected.json` 共享夹具；失配报告的字段、文案与修复建议需同步修改。成员检查只认 `doc.params` 自身的键，不能从原型链读取同名属性。

窄窗口通过响应式样式收敛工具栏内边距和次要状态文字；工作流名称在顶部标签中显示，文件操作集中在保存菜单与工作流面板。交互及宽度回归使用 `scripts/e2e/m3.mjs`、`params_p4.mjs`，不依赖已删除的旧图名输入框。

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
