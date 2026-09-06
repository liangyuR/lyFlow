# ADR-0018：编辑器是一个包，传输层由宿主注入

日期：2026-09-07　状态：已采纳

## 背景

`xyz-gap-inspector` 要把 LyFlow 的图编辑界面嵌进自己的 Web 前端
（[gap-inspector-integration-design.md](../gap-inspector-integration-design.md) §2.7）：
在浏览器里编图、点运行，计算在**业务自己的服务进程**里发生 —— 那个进程加载 core DLL，
前端与它之间隔着一条 HTTP + WebSocket。

M1–M4 的前端全在 `app/src` 里，与 Tauri 长在一起：
`transport/index.ts` 是一个模块级单例，靠 `"__TAURI_INTERNALS__" in window` 二选一；
文件对话框直接 `import("@tauri-apps/plugin-dialog")`；快捷键挂在 `window` 上；
设计变量定义在 `:root`。这四件事都让它没法被别的页面装进去。

## 决定

### 一、`packages/editor`（`@lyflow/editor`），`app/` 只剩壳

画布、参数表单、Inspector、3D 视图、stores、类型校验、keymap、样式全部搬进包，
导出一个 `<LyFlowEditor>` 组件。`app/` 剩下的是：入口（`main.tsx`）、
窗口标题（`title.ts`）、Tauri 文件对话框（`dialogs.ts`）、验收窗口桥（`devbridge.ts`）、
以及把 `TauriTransport` 装配起来。

包按 **TypeScript 源码**发布（`exports` 指向 `src/index.ts`），不出 `dist`。
理由：两个消费方（`app/`、`examples/host-react/`）都是 Vite + TS，
一个构建步骤只会带来「dist 过期了」这一类新故障，换不来任何东西。
代价是宿主必须用一个能编译 TS/TSX 的打包器 —— 写在 README 里。

### 二、传输层由宿主注入，包内用转发代理消费

`<LyFlowEditor transport={t}>` 在 **render 里**（不是 effect 里）调 `setTransport(t)`：
子树的 store 一挂载就会去调传输层，等到 effect 就晚了。

包内二十来个调用点仍然写 `transport.getManifest()` —— `transport` 是一个
转发到「当前那一个」实现的 `Proxy`。这样迁移不用改二十个文件，
以后要支持「一个页面里两个编辑器各连各的后端」时再改也不迟（现在不需要）。

三个实现：`TauriTransport`（每个方法对一条 `#[tauri::command]`）、
`HttpTransport`（对着 [http-transport.md](../http-transport.md)）、
`StaticTransport`（只读快照，没有后端时也能把界面渲染出来）。
`TransportKind` 从两个值变成三个：`tauri | http | static`。
**能力判断一律写 `kind === "static"` 而不是 `kind !== "tauri"`** ——
live preview 那三处原来就是后者，在 HTTP 下会静默失效。

### 三、React / React Flow / three / zustand 是 peerDependencies

包里创建的 zustand store，它的 hook 会在**宿主的**渲染树里被调用
（宿主可以自己 `useUiStore(...)`）。两份 React 的话 dispatcher 对不上，直接
"Invalid hook call"。所以这四个 + `react-dom` 都是 peer，宿主负责提供唯一一份。

光声明 peer 不够，`examples/host-react/vite.config.ts` 里还有一个构建期插件：
扫打包结果的 module id，任何一个 peer 出现两个 `node_modules/<name>/` 根就让构建失败。
运行期还有一条：宿主渲染一个调 `useUiStore` 的探针组件，两份 React 时它会抛。
两条都在 `pnpm check` / `pnpm e2e:http` 里。

`@tauri-apps/api` 是**可选** peer：只有 `TauriTransport` 用得到它，
浏览器宿主不装也能跑（那三处 `import()` 永远不会执行）。

### 四、样式：`--lyflow-*`，定义在编辑器根元素上

变量从 `--bg-0` 之类改名成 `--lyflow-bg-0`，并且**定义在 `.app` 上而不是 `:root`** ——
宿主页面的其余部分不该被我们改色。`html/body/#root` 的高度与底色也从包里挪走了
（`app/src/shell.css`、`examples/host-react/src/host.css` 各自一份）：
那是页面的事，不是组件的事。

宿主换皮走 `theme` prop（写成根元素的 inline style，优先级最高）或者在更外层重定义。

### 五、快捷键挂在编辑器根元素上，不挂 window

`useShortcuts(handlers, rootRef)`。宿主页面里的其它输入框不会被我们拦下。
代价是根元素得拿得到焦点，于是三件事：`tabIndex={-1}`、挂载时 focus、
点进编辑器时如果焦点掉回 body 就收回来。

**还有一条兜底**：根元素的 `ownerDocument` 上也挂一个 keydown，
但**只处理 `event.target === document.body` 的**。焦点所在的元素被卸载时
（改完参数、那一行重渲染了）浏览器会把焦点丢回 body，此时按 F5 谁都收不到 ——
这条兜底接住的正是这种「无主」按键，宿主自己的控件仍然不受影响。
这是对 A2-3 字面的一点扩张，写在这里备案。

### 六、文件对话框由宿主注入

`EditorDialogs`：`pickOpenPath` / `pickSavePath` / `confirmDiscard` / `confirmRestore`
四件必需，外加可选的 `pickPath`（参数表单里的路径选择、3D 视图导出 PNG）。
不给就退回 `window.confirm` 与浏览器下载。包里因此一行 `@tauri-apps/plugin-dialog` 都没有。

## 影响

- `app/` 的行为一个字没变：`pnpm e2e` 从 323 涨到 332（多的九条是 A2-5 的图级输出 UI）。
- 新增 `pnpm e2e:http`：Node 桩服务器 + 系统 Chrome + `examples/host-react`，29 条。
- `pnpm check` 多两步：包的 strict typecheck、宿主示例的构建（连带跑重复依赖检查）。
- ADR-0002 提到的 `app/src/lib/mapping.ts` 现在在 `packages/editor/src/lib/mapping.ts`，
  「映射层单独一个模块、别处不许直接引用 React Flow」这条规则不变。

## 没做

- **一个页面里多个编辑器实例**。stores 与 transport 都是模块级单例，两个实例会共用一份文档。
  真要做就得把 store 建进 React context，那是一次不小的改造，等有需求再说。
- **`dist` 构建与 npm 发布**。见上面「一」。
- **编辑器自己的多文档 / 多标签**（阶段 A 的「不做」里就写了）。
