// Tauri 壳的文件对话框。编辑器包不认识 @tauri-apps/plugin-dialog，
// 由这里注入进去（A2-1）。

import type { EditorDialogs } from "@lyflow/editor";

const FILTERS = [{ name: "LyFlow Graph", extensions: ["lyflow.json", "json"] }];
const RECIPE_FILTERS = [{ name: "LyFlow 配方", extensions: ["lyflow-recipe.json", "json"] }];

function inTauri(): boolean {
  return "__TAURI_INTERNALS__" in window;
}

export class NoDialogError extends Error {
  constructor() {
    super("浏览器模式没有文件对话框，请在 Tauri 里运行");
  }
}

export const tauriDialogs: EditorDialogs = {
  async pickOpenPath() {
    if (!inTauri()) throw new NoDialogError();
    const { open } = await import("@tauri-apps/plugin-dialog");
    const picked = await open({ multiple: false, filters: FILTERS });
    return typeof picked === "string" ? picked : null;
  },

  async pickSavePath(suggested: string) {
    if (!inTauri()) throw new NoDialogError();
    const { save } = await import("@tauri-apps/plugin-dialog");
    const picked = await save({ defaultPath: suggested, filters: FILTERS });
    return typeof picked === "string" ? picked : null;
  },

  async confirmDiscard(dirty: boolean) {
    if (!dirty) return true;
    const message = "当前图有未保存的改动，确定放弃吗？";
    if (!inTauri()) return window.confirm(message);
    const { ask } = await import("@tauri-apps/plugin-dialog");
    return ask(message, { title: "LyFlow", kind: "warning" });
  },

  async confirmRestore(_path: string, message: string) {
    if (!inTauri()) return window.confirm(message);
    const { ask } = await import("@tauri-apps/plugin-dialog");
    return ask(message, { title: "LyFlow", kind: "warning" });
  },

  // 配方的导入 / 导出（param-recipe P3.6）。只给配方用：通用的 pickPath 一给，参数表单与 3D 导出也会换成
  // 原生对话框，那是另一件事
  async pickRecipePath(mode: "open" | "save", suggested?: string) {
    if (!inTauri()) throw new NoDialogError();
    const { open, save } = await import("@tauri-apps/plugin-dialog");
    const picked =
      mode === "open"
        ? await open({ multiple: false, filters: RECIPE_FILTERS })
        : await save({ ...(suggested ? { defaultPath: suggested } : {}), filters: RECIPE_FILTERS });
    return typeof picked === "string" ? picked : null;
  },
};
