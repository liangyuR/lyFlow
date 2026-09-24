// 配方的界面动作（工具栏下拉框、配方管理页共用）：问名字、确认删除、选文件，然后调 graph store 的
// 语义化动作（撤销栈在那里，K7）或 recipeFiles 的导入导出。名字的规矩在 lib/recipes.ts。

import { dialogs } from "../lib/dialogs";
import { askChoice, askText } from "../lib/modal";
import { recipeFileName, recipeNameProblem, uniqueRecipeName } from "../lib/recipes";
import { useGraphStore } from "../store/graph";
import { recipeSet, selectRecipe, useRecipeStore } from "../store/recipe";
import { exportRecipeTo, importRecipeFrom } from "../store/recipeFiles";
import { useUiStore } from "../store/ui";

const toast = (text: string, kind: "info" | "warn" = "info") => useUiStore.getState().showToast(text, kind);

/** 没存过盘的图不能建配方（P3.2）：配方文件放在图文件旁边。 */
function needDir(): boolean {
  if (useRecipeStore.getState().dir) return true;
  toast("图还没存过盘：先保存图（Ctrl+S），配方存在图文件旁边的 <图名>.recipes/ 里", "warn");
  return false;
}

/** 新建配方：空的，或（勾上）复制 copyFrom 的值。建完切到它。 */
export async function newRecipeInteractive(copyFrom: string | null = useRecipeStore.getState().current): Promise<string | null> {
  if (!needDir()) return null;
  const answer = await askText({
    title: "新建配方",
    message: "名字也是文件名（<名字>.lyflow-recipe.json）。新配方是空的：所有值沿用基础。",
    value: uniqueRecipeName(recipeSet(), "新配方"),
    okLabel: "新建",
    validate: (v) => recipeNameProblem(recipeSet(), v),
    checkbox: copyFrom ? { label: `复制配方「${copyFrom}」的值`, checked: false } : undefined,
  });
  if (!answer) return null;
  const ok = useGraphStore.getState().createRecipe(answer.value, answer.checked ? copyFrom : null);
  if (!ok) return null;
  selectRecipe(answer.value);
  toast(`已新建配方 ${answer.value}（Ctrl+S 写进文件）`);
  return answer.value;
}

export async function duplicateRecipeInteractive(name: string): Promise<string | null> {
  if (!needDir()) return null;
  const answer = await askText({
    title: `复制配方「${name}」`,
    value: uniqueRecipeName(recipeSet(), `${name} 副本`),
    okLabel: "复制",
    validate: (v) => recipeNameProblem(recipeSet(), v),
  });
  if (!answer) return null;
  if (!useGraphStore.getState().createRecipe(answer.value, name)) return null;
  selectRecipe(answer.value);
  return answer.value;
}

export async function renameRecipeInteractive(name: string): Promise<string | null> {
  const answer = await askText({
    title: `重命名配方「${name}」`,
    message: "存盘时文件跟着改名。",
    value: name,
    okLabel: "重命名",
    validate: (v) => (v === name ? null : recipeNameProblem(recipeSet(), v, name)),
  });
  if (!answer || answer.value === name) return null;
  return useGraphStore.getState().renameRecipe(name, answer.value) ? answer.value : null;
}

export async function deleteRecipeInteractive(name: string): Promise<boolean> {
  const answer = await askChoice({
    title: `删除配方「${name}」？`,
    message: `存盘时删掉 ${recipeFileName(name)}。在那之前可以 Ctrl+Z 撤销。`,
    choices: [
      { id: "delete", label: "删除", tone: "danger" },
      { id: "cancel", label: "取消" },
    ],
  });
  if (answer !== "delete") return false;
  useGraphStore.getState().deleteRecipe(name);
  return true;
}

/** 选一个文件：宿主给了配方专用的就用它，其次通用的 pickPath，都没有就在编辑器里输路径
 *  （HTTP 宿主：工作区里的相对路径）。 */
async function pickFile(mode: "open" | "save", suggested: string): Promise<string | null> {
  const d = dialogs();
  try {
    if (d.pickRecipePath) return await d.pickRecipePath(mode, suggested);
    if (d.pickPath) {
      return await d.pickPath({
        mode,
        defaultPath: suggested,
        filters: [{ name: "LyFlow 配方", extensions: ["lyflow-recipe.json", "json"] }],
      });
    }
  } catch {
    /* 宿主的对话框用不了（比如不在 Tauri 里）：退回输路径 */
  }
  const answer = await askText({
    title: mode === "open" ? "导入配方：文件路径" : "导出配方：存到哪里",
    message: "宿主没有提供文件对话框。填文件路径（HTTP 后端是工作区里的相对路径），文件名要以 .lyflow-recipe.json 结尾。",
    value: mode === "save" ? suggested : "",
    validate: (v) => (v.trim().toLowerCase().endsWith(".lyflow-recipe.json") ? null : "文件名要以 .lyflow-recipe.json 结尾"),
  });
  return answer?.value.trim() ?? null;
}

export async function importRecipeInteractive(): Promise<string | null> {
  if (!needDir()) return null;
  const path = await pickFile("open", "");
  if (!path) return null;
  try {
    const name = await importRecipeFrom(path);
    if (name) {
      selectRecipe(name);
      toast(`已导入配方 ${name}（Ctrl+S 写进配方目录）`);
    }
    return name;
  } catch (e) {
    toast(e instanceof Error ? e.message : String(e), "warn");
    return null;
  }
}

export async function exportRecipeInteractive(name: string): Promise<boolean> {
  const path = await pickFile("save", recipeFileName(name));
  if (!path) return false;
  try {
    await exportRecipeTo(name, path);
    toast(`已导出 ${path}`);
    return true;
  } catch (e) {
    toast(e instanceof Error ? e.message : String(e), "warn");
    return false;
  }
}
