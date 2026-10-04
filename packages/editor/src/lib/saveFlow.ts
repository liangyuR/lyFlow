// 保存当前的图与有改动的配方（K6 ③）。从 LyFlowEditor 里挪出来，是因为「保存 / 不保存 / 取消」那一问（lib/unsaved）
// 选了保存之后要知道到底存没存成：另存为对话框取消了、配方的外部修改那一问选了取消、写盘出错，都算没存成，
// 后面的打开 / 新建 / 关窗口就不做。

import { discardUntitledBackup } from "./autosave";
import { discardBackup, pickSavePath, rememberFile, saveDocTo, suggestFileName } from "./files";
import { useGraphStore } from "../store/graph";
import { commitRecipeSave, prepareRecipeSave } from "../store/recipeFiles";
import { useUiStore } from "../store/ui";

/** 存成了返回 true。forcePicker = 另存为。 */
export async function saveCurrent(forcePicker: boolean): Promise<boolean> {
  const graph = useGraphStore.getState();
  const ui = useUiStore.getState();
  try {
    let path = graph.filePath;
    const wasUntitled = !path;
    if (!path || forcePicker) {
      path = await pickSavePath(path ?? suggestFileName(graph.doc));
      if (!path) return false; // 用户取消
    }
    // 一次保存图与所有有改动的配方文件（K6 ③）。配方先查外部修改：用户选了取消，图也不存
    const doc = graph.doc;
    const recipes = await prepareRecipeSave(doc, path);
    if (recipes === "cancelled") {
      ui.showToast("已取消保存", "warn");
      return false;
    }
    await saveDocTo(path, doc);
    // 记下的是真正写下去的那一份：撤销回到它时 dirty 复原（P1.6）
    useGraphStore.getState().markSaved(path, doc);
    if (recipes) await commitRecipeSave(recipes);
    await rememberFile(path);
    // 存过盘就没有「未保存的改动」了，备份留着只会在下次开图时误报
    await discardBackup(path);
    if (wasUntitled) await discardUntitledBackup();
    ui.showToast("已保存");
    return true;
  } catch (e) {
    ui.showToast(e instanceof Error ? e.message : String(e), "warn");
    return false;
  }
}
