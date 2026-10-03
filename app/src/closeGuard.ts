// 关窗口前问一句。M3 计划里的「关闭 / 打开时未保存提示」只做了打开、新建那一半（编辑器里的
// dialogs.confirmDiscard）：点了 × 改动直接就没了，没存过盘的图连 `<file>~` 备份都没有。
// 关窗口是壳的事（A2-1），和窗口标题一样装在这里。
//
// 装上之后关窗口由这里收尾：JS 里有 close-requested 的监听，tauri 就不自己关了，没被拦下时
// onCloseRequested 替我们 destroy —— 所以 capabilities 里要有 core:window:allow-destroy，
// 少了它点 × 就再也关不掉（e2e m3 的 suitePanels 查着）。

//
// 问的是编辑器画的「保存 / 不保存 / 取消」（@lyflow/editor 的 resolveUnsaved）：以前是原生的「关闭 / 取消」，
// 想保住改动只能先取消、存盘、再点一次 ×。

import { recipesDirty, resolveUnsaved, useGraphStore } from "@lyflow/editor";

/** 装上了没有（验收脚本经 devbridge 读）。 */
export const closeGuard = { installed: false };

/** 要不要关：没有没存的改动（图或配方）直接关；有就问，存成了或选了不保存才关。
 *  decide 单独拿出来，验收脚本经 devbridge 知道问没问。 */
export async function shouldClose(
  decide: () => Promise<boolean> = () => resolveUnsaved("关闭窗口"),
): Promise<boolean> {
  if (!useGraphStore.getState().dirty && !recipesDirty()) return true;
  return decide();
}

export async function installCloseGuard(): Promise<void> {
  if (!("__TAURI_INTERNALS__" in window)) return;
  const { getCurrentWindow } = await import("@tauri-apps/api/window");
  let asking = false;
  await getCurrentWindow().onCloseRequested(async (event) => {
    // 对话框还开着又点了一次 ×：不叠第二个框
    if (asking) {
      event.preventDefault();
      return;
    }
    asking = true;
    try {
      if (!(await shouldClose())) event.preventDefault();
    } finally {
      asking = false;
    }
  });
  closeGuard.installed = true;
}
