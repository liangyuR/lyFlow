// 有没存的改动时，打开 / 最近文件 / 新建 / 关窗口之前问「保存 / 不保存 / 取消」。以前只有「确定放弃吗？」
// （关窗口是「关闭 / 取消」）：想保住改动只能先取消、按 Ctrl+S、再把刚才的操作做一遍，顺手点了「是」改动就没了。
// 对话框画在编辑器里（lib/modal，与「文件已被外部修改」那一问一样），保存默认拿焦点（回车即保存），Esc = 取消。

import { askChoice } from "./modal";
import { saveCurrent } from "./saveFlow";
import { useGraphStore } from "../store/graph";
import { recipesDirty } from "../store/recipe";

/** action 是接下来要做的事（「打开别的图」「新建」「关闭窗口」），写进问句。
 *  返回 true = 可以接着做：没有改动、存成了、或选了不保存。save 给单测换桩用。 */
export async function resolveUnsaved(
  action: string,
  save: (forcePicker: boolean) => Promise<boolean> = saveCurrent,
): Promise<boolean> {
  const graphDirty = useGraphStore.getState().dirty;
  const recipes = recipesDirty();
  if (!graphDirty && !recipes) return true;
  const what = graphDirty && recipes ? "这张图和配方" : graphDirty ? "这张图" : "配方";
  const choice = await askChoice({
    title: "保存改动？",
    message: `${what}有改动还没保存。${action}之前要先保存吗？`,
    choices: [
      { id: "save", label: "保存", tone: "primary" },
      { id: "discard", label: "不保存", tone: "danger" },
      { id: "cancel", label: "取消" },
    ],
  });
  if (choice === "discard") return true;
  // 没存过盘的图会弹另存为：在那里取消、配方那一问选了取消、写盘出错，都等于这里选了取消
  if (choice === "save") return save(false);
  return false;
}
