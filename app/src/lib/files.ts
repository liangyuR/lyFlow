// 文件操作（交互清单 P0 #13）。存盘走 Rust 侧的 save_graph，前端拒绝自己写文件 ——
// 校验逻辑放前端就会有一条绕过它的路（docs/graph-doc.md）。

import { transport, type BackupStatus, type LoadedGraph, type RecentEntry } from "../transport";
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

export async function loadDocFrom(path: string): Promise<LoadedGraph> {
  return transport.loadGraph(path);
}

// ---- 最近文件与自动备份（交互清单 2.5）

/** 每 30 秒写一次 `<file>~`。间隔再短就成了每次敲键都写盘。 */
export const BACKUP_INTERVAL_MS = 30_000;

export async function recentFiles(): Promise<RecentEntry[]> {
  try {
    return await transport.getRecentFiles();
  } catch {
    return [];
  }
}

/** 打开/保存成功后记一笔。失败不上报 —— 最近文件坏了不该打断正事。 */
export async function rememberFile(path: string): Promise<void> {
  try {
    await transport.pushRecentFile(path);
  } catch {
    /* 忽略 */
  }
}

export async function writeBackup(path: string, doc: GraphDoc): Promise<void> {
  await transport.writeBackup(path, doc);
}

export async function backupStatus(path: string): Promise<BackupStatus> {
  try {
    return await transport.backupStatus(path);
  } catch {
    return { exists: false, newer: false, backupModified: null, fileModified: null };
  }
}

export async function readBackup(path: string): Promise<LoadedGraph> {
  return transport.readBackup(path);
}

export async function discardBackup(path: string): Promise<void> {
  try {
    await transport.discardBackup(path);
  } catch {
    /* 忽略：备份删不掉最多下次再问一遍 */
  }
}

/** 备份比正文新时问一句。返回 true 表示用户要恢复。 */
export async function confirmRestore(path: string): Promise<boolean> {
  const message =
    `${baseName(path)} 有一份比正文更新的自动备份，` +
    `上次可能是异常退出的。要恢复备份吗？（选否则丢弃备份）`;
  if (!inTauri()) return window.confirm(message);
  const { ask } = await import("@tauri-apps/plugin-dialog");
  return ask(message, { title: "LyFlow", kind: "warning" });
}

export function baseName(path: string): string {
  const parts = path.split(/[\/]/);
  return parts[parts.length - 1] ?? path;
}

/** 从图名推一个默认文件名。 */
export function suggestFileName(doc: GraphDoc): string {
  const base = (doc.name ?? "untitled").trim() || "untitled";
  return `${base}.lyflow.json`;
}
