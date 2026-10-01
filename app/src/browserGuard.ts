// 桌面壳里的 WebView2 还是一个浏览器：F5 / Ctrl+R 刷新整个页面 —— 没存的图、撤销栈、运行结果全没了，关窗口前
// 那一问（closeGuard）也拦不住；右键弹的是浏览器自己的菜单（「刷新」「另存为」「打印」）。Tauri 2 没把 WebView2 的
// AreBrowserAcceleratorKeysEnabled / AreDefaultContextMenusEnabled 开放出来，只能在页面里挡。
//
// 刷新键在捕获阶段 preventDefault、不拦传播：编辑器照样收到 F5 去运行。以前编辑器自己处理 F5 时会 preventDefault，
// 可对话框、搜索面板开着时它不接键，F5 就落到了刷新上；Ctrl+R 编辑器根本不用 —— 一按整个 app 重新载入。
// 只装在桌面壳里：浏览器宿主（HTTP 那条路）里 F5 刷新是用户自己想要的。

export function isReloadKey(e: KeyboardEvent): boolean {
  if (e.key === "F5" || e.key === "BrowserRefresh") return true;
  return (e.ctrlKey || e.metaKey) && !e.altKey && e.key.toLowerCase() === "r";
}

/** 输入框里、选着文字时留着浏览器的菜单（剪切 / 复制 / 粘贴）；开发构建里按住 Ctrl 右键也留着（要「检查」）。 */
function wantsNativeMenu(e: MouseEvent, win: Window, dev: boolean): boolean {
  if (dev && e.ctrlKey) return true;
  const target = e.target instanceof Element ? e.target : null;
  if (target?.closest('input, textarea, [contenteditable=""], [contenteditable="true"]')) return true;
  return (win.getSelection()?.toString() ?? "") !== "";
}

export function installBrowserGuard(win: Window = window, { dev = false } = {}): () => void {
  const onKey = (e: KeyboardEvent) => {
    if (isReloadKey(e)) e.preventDefault();
  };
  const onMenu = (e: MouseEvent) => {
    if (!e.defaultPrevented && !wantsNativeMenu(e, win, dev)) e.preventDefault();
  };
  win.addEventListener("keydown", onKey, true);
  win.addEventListener("contextmenu", onMenu);
  return () => {
    win.removeEventListener("keydown", onKey, true);
    win.removeEventListener("contextmenu", onMenu);
  };
}
