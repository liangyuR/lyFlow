// 定时备份（交互清单 2.5）。存过盘的图写 `<file>~`；没存过盘的（2026-10-02 起）写到传输层给的一个固定位置
// 旁边的 `~`（Tauri：app data 下的 untitled.lyflow.json），下次开 app 时问要不要恢复 —— 以前没存过盘的图
// 什么都不备份，崩溃、断电、被强杀就全没了。传输层没有 untitledBackupPath（HTTP、静态快照）时照旧不备份。

import { activeTransport, transport, type BackupStatus, type LoadedGraph, type Transport } from "../transport";
import { useGraphStore } from "../store/graph";
import { recipesDirty } from "../store/recipe";
import { discardRecipeAutosave, writeRecipeAutosave } from "../store/recipeFiles";
import { isMigration } from "../types/execution";
import { dialogs } from "./dialogs";
import { backupStatus, baseName, confirmRestore, discardBackup, readBackup, writeBackup } from "./files";
import { askChoice, modalHostReady } from "./modal";

let cached: { owner: Transport; path: Promise<string | null> } | null = null;

/** 没存过盘的图的备份「正文」路径（备份在它旁边的 `~`）；传输层不支持时是 null。每个传输只问一次。 */
export function untitledBackupPath(): Promise<string | null> {
  const owner = activeTransport();
  if (cached?.owner !== owner) {
    cached = {
      owner,
      path: (async () => {
        try {
          return (await transport.untitledBackupPath?.()) ?? null;
        } catch {
          return null;
        }
      })(),
    };
  }
  return cached.path;
}

/** 写不进就算了：备份不该打断正事，下一拍再试（也免得一个没接住的 Promise 在控制台里报红）。 */
async function quietly(work: Promise<unknown>): Promise<void> {
  try {
    await work;
  } catch {
    /* 忽略 */
  }
}

/** 30 秒一拍（LyFlowEditor 的定时器；验收脚本经 devbridge 直接调）。 */
export async function autosaveTick(): Promise<void> {
  const graph = useGraphStore.getState();
  if (graph.filePath) {
    // 配方有没存的改动时同一拍写 `<配方目录>/autosave~.json`（整个内存里的配方集合），并照样写一份
    // `<file>~` —— 下次开图时「恢复备份」只问一次，图与配方一起回来（docs/recipe.md「自动备份」）
    const recipes = recipesDirty();
    if (!graph.dirty && !recipes) return;
    await quietly(writeBackup(graph.filePath, graph.doc));
    if (recipes) await quietly(writeRecipeAutosave());
    return;
  }
  // 没存过盘：有改动、不是空图才写（新开 app 的那张空白图不值得下次再问一句）
  if (!graph.dirty || graph.doc.nodes.length === 0) return;
  const path = await untitledBackupPath();
  if (path) await quietly(writeBackup(path, graph.doc));
}

export interface UntitledBackup {
  path: string;
  loaded: LoadedGraph;
  /** 备份写下的时刻（毫秒时间戳），拿不到是 null。 */
  savedAt: number | null;
}

/** 上次没存过盘的那张图的备份（有节点才算）。读不出来的顺手删掉。 */
export async function findUntitledBackup(): Promise<UntitledBackup | null> {
  const path = await untitledBackupPath();
  if (!path) return null;
  const status = await backupStatus(path);
  if (!status.exists) return null;
  let loaded: LoadedGraph;
  try {
    loaded = await readBackup(path);
  } catch {
    await discardBackup(path);
    return null;
  }
  if (loaded.doc.nodes.length === 0) {
    await discardBackup(path);
    return null;
  }
  return { path, loaded, savedAt: status.backupModified };
}

/** 换上备份里的那张图：没有路径、算没保存（标题带 *、关窗口会问、定时备份接着往同一处写）。
 *  备份留着，直到存了盘或者被新建 / 打开换掉。 */
export function restoreUntitled(backup: UntitledBackup): void {
  useGraphStore.getState().loadDoc(backup.loaded.doc, null);
  const migrations = backup.loaded.migrations.filter(isMigration);
  if (migrations.length > 0) useGraphStore.getState().applyMigrations(migrations);
  useGraphStore.getState().markUnsaved();
}

/** 没存过盘的图存了盘、或者被新建 / 打开换掉之后：它的备份没用了。 */
export async function discardUntitledBackup(): Promise<void> {
  const path = await untitledBackupPath();
  if (path) await discardBackup(path);
}

/** 问用户的那句话。 */
export function untitledRestoreMessage(backup: UntitledBackup): string {
  const when = backup.savedAt ? `，${new Date(backup.savedAt).toLocaleString()} 备份的` : "";
  return (
    `上次有一张还没存过盘的图（${backup.loaded.doc.nodes.length} 个节点${when}）没有保存就退出了。` +
    `要恢复吗？（选否则丢弃它）`
  );
}

// ---- 恢复备份那一问（交互清单 2.5）
// 以前是原生的「是 / 否」：选否（浏览器宿主里按 Esc 也是否）两份备份就删了，点错了找不回来；也没写备份与正文各是
// 什么时候的。现在画在编辑器里（lib/modal），三选一：恢复 / 丢弃 / 取消 —— 取消什么都不动，备份留着、下次再问。
// 没挂对话框的宿主（编辑器整个卸掉了）退回原生的是 / 否，与以前一样。

export type RestoreAnswer = "restore" | "discard" | "cancel";

const whenText = (ms: number | null): string => (ms ? new Date(ms).toLocaleString() : "不详");

async function askThree(title: string, message: string, discardLabel: string, fallback: () => Promise<boolean>): Promise<RestoreAnswer> {
  if (!modalHostReady()) return (await fallback()) ? "restore" : "discard";
  const choice = await askChoice({
    title,
    message,
    choices: [
      { id: "restore", label: "恢复备份", tone: "primary" },
      { id: "discard", label: discardLabel, tone: "danger" },
      { id: "cancel", label: "取消（备份留着）" },
    ],
  });
  return choice === "restore" || choice === "discard" ? choice : "cancel";
}

/** 打开 path 之前看一眼它的备份：比正文新就问一句。返回从哪开 —— "backup"、"file"；null = 取消了，不打开、备份都留着。
 *  选了丢弃、或者备份不比正文新时，图与配方的两份备份都删掉（与以前一样）。 */
export async function openSourceFor(path: string): Promise<"backup" | "file" | null> {
  const status: BackupStatus = await backupStatus(path);
  if (status.newer) {
    const answer = await askThree(
      "恢复自动备份？",
      `${baseName(path)} 有一份比上次保存更新的自动备份（备份于 ${whenText(status.backupModified)}，` +
        `文件上次保存于 ${whenText(status.fileModified)}）。上次可能是异常退出的，也可能是关的时候选了「不保存」。`,
      "丢弃备份，打开上次保存的",
      () => confirmRestore(path),
    );
    if (answer === "restore") return "backup";
    if (answer === "cancel") return null;
  }
  if (status.exists) {
    await discardBackup(path);
    await discardRecipeAutosave(path);
  }
  return "file";
}

/** 开 app 时那张没存过盘的图：恢复 / 丢弃 / 取消。取消 = 这回先不管、备份留着；不过新开的这张图有了改动，
 *  定时备份就会写到同一处、换成新图的（只有一个位置）—— 问句里写明。 */
export function askRestoreUntitled(backup: UntitledBackup): Promise<RestoreAnswer> {
  return askThree(
    "恢复上次没存的图？",
    `${untitledRestoreMessage(backup).replace("要恢复吗？（选否则丢弃它）", "要恢复吗？")}` +
      "选「取消」备份先留着，不过在新图里改了东西之后，它会被新图的备份替换。",
    "丢弃它",
    () => dialogs().confirmRestore(backup.path, untitledRestoreMessage(backup)),
  );
}
