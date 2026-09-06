# app —— Tauri 壳

编辑器本身在 [`packages/editor`](../packages/editor)（`@lyflow/editor`）。
这里只剩把它装进一个桌面窗口所需要的东西（[ADR-0018](../docs/adr/0018-editor-as-package.md)）：

```
src/main.tsx      入口：挑一个 Transport，装窗口桥与标题，渲染 <LyFlowEditor>
src/dialogs.ts    Tauri 的文件对话框，注入给编辑器
src/title.ts      窗口标题 `文件名 *`
src/devbridge.ts  验收窗口桥（scripts/e2e 用）
src/shell.css     页面级重置：html/body/#root 的高度与底色
index.html        #root
vite.config.ts    dev server 端口（与 bridge/tauri.conf.json 对上）+ peer 去重
```

壳里没有任何图的语义。加算子、改画布、改参数表单一律去包里。

## Transport 的选择

```ts
const transport = inTauri() ? new TauriTransport() : new StaticTransport();
```

`__TAURI_INTERNALS__` 在就是 Tauri，否则是浏览器模式（见下）。
`TauriTransport` 的每个方法对着一条 `#[tauri::command]`，清单在
[`bridge/src/commands.rs`](../bridge/src/commands.rs)。

## 对话框归壳

打开、另存、放弃改动、恢复备份、参数里的路径选择、3D 导出 PNG ——
这六件事要弹系统对话框，编辑器包不认识 `@tauri-apps/plugin-dialog`，
由 `src/dialogs.ts` 实现 `EditorDialogs` 注入进去。

## 验收

不写 UI 单元测试（CLAUDE.md）。验收方式是 CDP 驱动真实运行的 Tauri app：
`pnpm e2e`，脚本在 [`scripts/e2e/`](../scripts/e2e/)。
`src/devbridge.ts` 把包里那五个 store 挂到 `window.__lyflow` 上供脚本读状态 ——
它只读+转发，不放任何业务逻辑，应用代码一律不许 import 它。

浏览器宿主那条线是 `pnpm e2e:http`（`examples/host-react` + Node 桩服务器）。

## 浏览器模式

`pnpm app:dev` 不启动 Tauri，manifest 读 `public/manifest.dev.json`
（用 `pnpm core:dump` 刷新）。界面迭代的反馈循环因此是秒级而不是分钟级。
状态栏会把这种模式明确标成「静态快照」，避免有人对着三天前的数据调半天。
要连真后端就用 `pnpm host:dev` + `pnpm test-server`。
