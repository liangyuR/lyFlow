//
// 文件操作（交互清单 P0 #13）。
//
// 存盘走 Rust 侧的 save_graph：结构校验在那里做，前端拒绝自己写文件。
// 理由是 headless 执行、脚本生成的图走的也是同一条校验路径，
// 校验逻辑放前端就会有一条绕过它的路（docs/graph-doc.md）。
//

import { transport } from "../transport";
import type { GraphDoc } from "../types/graph";

const FILTERS = [{ name: "LyFlow Graph", extensions: ["lyflow.json", "json"] }];

function inTauri(): boolean {
  return "__TAURI_INTERNALS__" in window;
}

export class NoDialogError extends Error {
  constructor() {
    super("浏览器模式没有文件对话框，请在 Tauri 里运行");
  }
}

/** 返回 null 表示用户取消。 */
export async function pickOpenPath(): Promise<string | null> {
  if (!inTauri()) throw new NoDialogError();
  const { open } = await import("@tauri-apps/plugin-dialog");
  const picked = await open({ multiple: false, filters: FILTERS });
  return typeof picked === "string" ? picked : null;
}

export async function pickSavePath(suggested: string): Promise<string | null> {
  if (!inTauri()) throw new NoDialogError();
  const { save } = await import("@tauri-apps/plugin-dialog");
  const picked = await save({ defaultPath: suggested, filters: FILTERS });
  return typeof picked === "string" ? picked : null;
}

/** 有未保存改动时问一句。返回 true 表示可以继续。 */
export async function confirmDiscard(dirty: boolean): Promise<boolean> {
  if (!dirty) return true;
  if (!inTauri()) return window.confirm("当前图有未保存的改动，确定放弃吗？");
  const { ask } = await import("@tauri-apps/plugin-dialog");
  return ask("当前图有未保存的改动，确定放弃吗？", {
    title: "LyFlow",
    kind: "warning",
  });
}

export async function saveDocTo(path: string, doc: GraphDoc): Promise<void> {
  await transport.saveGraph(path, doc);
}

export async function loadDocFrom(path: string): Promise<GraphDoc> {
  return transport.loadGraph(path);
}

/** 从图名推一个默认文件名。 */
export function suggestFileName(doc: GraphDoc): string {
  const base = (doc.name ?? "untitled").trim() || "untitled";
  return `${base}.lyflow.json`;
}
