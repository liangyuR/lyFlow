// Tauri 壳的文件对话框。编辑器包不认识 @tauri-apps/plugin-dialog，
// 由这里注入进去（A2-1）。

import type { EditorDialogs, PathPickRequest } from "@lyflow/editor";

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

/** 验收脚本的桩（devbridge 的 shell.stubPickPath 装上，scripts/e2e 起 app 时就装）：原生对话框会挡住自动化，
 *  验收时换成脚本给的答案。正常使用时它是 null。 */
let pickOverride: ((request: PathPickRequest) => Promise<string | null>) | null = null;

export function overridePickPath(fn: ((request: PathPickRequest) => Promise<string | null>) | null): void {
  pickOverride = fn;
}

/** 参数表单的「浏览…」、3D / 查看器导出 PNG、库目录的「浏览…」（2026-10-04 拍板给原生对话框：推翻 param-recipe P3
 *  决定 13 的后半句 —— 那时怕它卡住导出的 e2e，现在验收经 overridePickPath 打桩）。 */
async function pickPath(request: PathPickRequest): Promise<string | null> {
  if (pickOverride) return pickOverride(request);
  if (!inTauri()) throw new NoDialogError();
  const { open, save } = await import("@tauri-apps/plugin-dialog");
  const filters = request.filters && request.filters.length > 0 ? request.filters : undefined;
  const start = request.defaultPath ? { defaultPath: request.defaultPath } : {};
  const picked =
    request.mode === "save"
      ? await save({ ...start, ...(filters ? { filters } : {}) })
      : await open({
          ...start,
          multiple: false,
          directory: request.mode === "dir",
          ...(filters && request.mode !== "dir" ? { filters } : {}),
        });
  return typeof picked === "string" ? picked : null;
}

export const tauriDialogs: EditorDialogs = {
  // 浏览器模式（pnpm app:dev，不在 Tauri 里）没有对话框：不给，「浏览…」就不摆
  ...(inTauri() ? { pickPath } : {}),

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

  async confirmRestore(_path: string, message: string) {
    if (!inTauri()) return window.confirm(message);
    const { ask } = await import("@tauri-apps/plugin-dialog");
    return ask(message, { title: "LyFlow", kind: "warning" });
  },

  // 配方的导入 / 导出（param-recipe P3.6）：带配方的文件类型过滤
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
