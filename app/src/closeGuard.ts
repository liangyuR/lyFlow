// 关窗口前问一句。M3 计划里的「关闭 / 打开时未保存提示」只做了打开、新建那一半（编辑器里的
// dialogs.confirmDiscard）：点了 × 改动直接就没了，没存过盘的图连 `<file>~` 备份都没有。
// 关窗口是壳的事（A2-1），和窗口标题一样装在这里。
//
// 装上之后关窗口由这里收尾：JS 里有 close-requested 的监听，tauri 就不自己关了，没被拦下时
// onCloseRequested 替我们 destroy —— 所以 capabilities 里要有 core:window:allow-destroy，
// 少了它点 × 就再也关不掉（e2e m3 的 suitePanels 查着）。

import { recipesDirty, useGraphStore } from "@lyflow/editor";

const MESSAGE = "当前图有未保存的改动，关闭窗口会丢掉它们。确定关闭吗？";

/** 装上了没有（验收脚本经 devbridge 读）。 */
export const closeGuard = { installed: false };

/** 要不要关：没有没存的改动（图或配方）直接关；有就问，选「关闭」才关。
 *  单独拿出来，验收脚本经 devbridge 换个 ask 直接调 —— 原生对话框脚本点不了。 */
export async function shouldClose(ask: (message: string) => Promise<boolean>): Promise<boolean> {
  if (!useGraphStore.getState().dirty && !recipesDirty()) return true;
  return ask(MESSAGE);
}

export async function installCloseGuard(): Promise<void> {
  if (!("__TAURI_INTERNALS__" in window)) return;
  const [{ getCurrentWindow }, { ask }] = await Promise.all([
    import("@tauri-apps/api/window"),
    import("@tauri-apps/plugin-dialog"),
  ]);
  const askNative = (message: string) =>
    ask(message, { title: "LyFlow", kind: "warning", okLabel: "关闭", cancelLabel: "取消" });
  let asking = false;
  await getCurrentWindow().onCloseRequested(async (event) => {
    // 对话框还开着又点了一次 ×：不叠第二个框
    if (asking) {
      event.preventDefault();
      return;
    }
    asking = true;
    try {
      if (!(await shouldClose(askNative))) event.preventDefault();
    } finally {
      asking = false;
    }
  });
  closeGuard.installed = true;
}
