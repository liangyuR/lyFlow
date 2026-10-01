// 桌面壳里的 WebView2 还是一个浏览器：浏览器快捷键默认开着 —— F5 / Ctrl+R 刷新整个页面（没存的图、撤销栈、运行结果
// 全没了，关窗口前那一问也拦不住），Ctrl+S 弹「网页另存为」，Ctrl+P 打印，Ctrl+F / Ctrl+G / F3 开浏览器的查找条；
// 右键弹的是浏览器自己的菜单（「刷新」「另存为」「打印」）。Tauri 2 没把 WebView2 的 AreBrowserAcceleratorKeysEnabled /
// AreDefaultContextMenusEnabled 开放出来（wry 有，tauri-runtime-wry 没接），只能在页面里挡。
//
// 这些键在捕获阶段 preventDefault、不拦传播：编辑器照样收到 F5（运行）、Ctrl+S（保存）、Ctrl+F（查找节点）、Ctrl+G（合成）。
// 以前编辑器自己处理时会 preventDefault，可搜索面板、对话框开着时、焦点在输入框里时它不接这些键，就落到了浏览器上；
// Ctrl+R 编辑器根本不用 —— 一按整个 app 重新载入。只装在桌面壳里：浏览器宿主（HTTP 那条路）里这些是用户自己想要的。

/** WebView2 的浏览器快捷键里 app 用不着、按了有害的那些：刷新、另存为、打印、页内查找。缩放（Ctrl+= / Ctrl+-）是另一个设置，不挡。 */
export function isBrowserShortcut(e: KeyboardEvent): boolean {
  if (e.key === "F5" || e.key === "BrowserRefresh" || e.key === "F3") return true;
  if (!(e.ctrlKey || e.metaKey) || e.altKey) return false;
  return ["r", "s", "p", "f", "g"].includes(e.key.toLowerCase());
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
    if (isBrowserShortcut(e)) e.preventDefault();
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
