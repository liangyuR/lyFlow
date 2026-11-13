import { getFlowSnapshots, restoreFlowDocuments, useFlowTabsStore } from '../store/flowTabs';
import {
  clearFlowBackup,
  readFlowBackup,
  writeFlowBackup,
  type FlowWorkspaceBackup,
} from './flowBackup';

let suspended = false;
let keepPreviousBackup = false;

export function resumeWorkspaceBackups(): void {
  suspended = false;
  keepPreviousBackup = false;
}
export function deferWorkspaceBackups(): void {
  suspended = true;
  keepPreviousBackup = true;
}

export async function backupWorkspace(): Promise<boolean> {
  if (suspended || useFlowTabsStore.getState().switching) return true;
  const workspace = getFlowSnapshots();
  const tabs = workspace.tabs.map(({ id, title, snapshot }) => ({
    id,
    title,
    doc: snapshot.graph.doc,
    filePath: snapshot.graph.filePath,
    dirty: snapshot.graph.dirty,
    savedDoc: snapshot.graph.savedDoc,
    recipeDirty: snapshot.recipe.set !== snapshot.recipe.saved,
    recipe: {
      set: snapshot.recipe.set,
      saved: snapshot.recipe.saved,
      current: snapshot.recipe.current,
      currentId: snapshot.recipe.currentId,
      dir: snapshot.recipe.dir,
      savedFiles: [...snapshot.recipe.savedFiles],
      diskText: [...snapshot.recipe.diskText],
      baseEdits: snapshot.recipe.baseEdits,
    },
  }));
  if (!tabs.some((tab) => tab.dirty || tab.recipeDirty)) return clearFlowBackup();
  return writeFlowBackup({ version: 1, activeId: workspace.activeId, tabs });
}

export { readFlowBackup as readWorkspaceBackup };

export function restoreWorkspaceBackup(backup: FlowWorkspaceBackup): boolean {
  return restoreFlowDocuments({
    activeId: backup.activeId,
    tabs: backup.tabs.map((tab) => ({
      ...tab,
      recipe: {
        ...tab.recipe,
        savedFiles: new Map(tab.recipe.savedFiles),
        diskText: new Map(tab.recipe.diskText),
      },
    })),
  });
}

export async function clearWorkspaceBackup(): Promise<boolean> {
  suspended = true;
  if (keepPreviousBackup) return true;
  return clearFlowBackup();
}
