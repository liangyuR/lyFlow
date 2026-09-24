// 配方文件的读写（param-recipe P3.1 / P3.2 / K6 ③）：打开图时读配方目录、Ctrl+S 时把有改动的配方写回去
// （改名、删除、index.json 一起）、存盘前检测文件是否被外部修改、30 s 自动备份、导入导出。
// 格式与判定都在 lib/recipes.ts（纯函数）；这里只有 I/O 与簿记（recipe store 的 saved / savedFiles / diskText）。

import { askChoice, askText } from "../lib/modal";
import {
  baseNameOf,
  EMPTY_SET,
  entryFromParsed,
  joinPath,
  newRecipeId,
  orderEntries,
  parseIndex,
  parseRecipeText,
  recipeDirOf,
  recipeFileName,
  recipeNameProblem,
  recipeStem,
  RECIPE_AUTOSAVE,
  RECIPE_INDEX,
  serializeIndex,
  serializeRecipe,
  type RecipeEntry,
  type RecipeSet,
} from "../lib/recipes";
import { transport } from "../transport";
import type { GraphDoc } from "../types/graph";
import { rebaseHistoryRecipes, useGraphStore } from "./graph";
import { applyRecipeSet, recipeSet, resetRecipes, selectRecipe, useRecipeStore } from "./recipe";

let loadTicket = 0;
let loading: Promise<void> = Promise.resolve();

/** 当前这一次载入（没有在载入就是一个已完成的 Promise）。验收脚本与「从备份恢复」等它。 */
export function recipesLoaded(): Promise<void> {
  return loading;
}

async function readOrNull(path: string): Promise<string | null> {
  try {
    return await transport.readRecipeFile(path);
  } catch {
    return null;
  }
}

/** 打开一张图（或换了一张图）：清掉上一张图的配方，读这张图旁边的配方目录。没有路径（未保存的新图）
 *  或静态模式就只清不读。读完后有 index.json 的 default 就选它（P3.3），否则停在「基础」。 */
export function loadRecipesFor(graphPath: string | null): Promise<void> {
  const mine = ++loadTicket;
  const dir = graphPath && transport.kind !== "static" ? recipeDirOf(graphPath) : null;
  resetRecipes(dir, dir ? "loading" : "none");
  const before = recipeSet();
  if (!dir) {
    loading = Promise.resolve();
    return loading;
  }
  loading = (async () => {
    try {
      const listing = await transport.listRecipeDir(dir);
      if (mine !== loadTicket) return;
      const diskText = new Map<string, string>();
      const problems: { file: string; message: string }[] = [];
      const savedFiles = new Map<string, string>();
      const entries: RecipeEntry[] = [];
      const seen = new Set<string>();
      let index = { defaultName: null as string | null, order: [] as string[] };
      for (const f of listing.files) {
        if (f.name.toLowerCase() === RECIPE_INDEX) {
          const text = await readOrNull(joinPath(dir, f.name));
          if (text !== null) {
            diskText.set(RECIPE_INDEX, text);
            index = parseIndex(text);
          }
          continue;
        }
        const stem = recipeStem(f.name);
        if (!stem) continue; // autosave~.json 不是配方
        const text = await readOrNull(joinPath(dir, f.name));
        if (text === null) {
          problems.push({ file: f.name, message: "读不出来" });
          continue;
        }
        diskText.set(f.name, text);
        const parsed = parseRecipeText(text, stem);
        if ("error" in parsed) {
          problems.push({ file: f.name, message: parsed.error });
          continue;
        }
        if (seen.has(stem.toLowerCase())) {
          problems.push({ file: f.name, message: "与另一个配方的名字只差大小写，跳过" });
          continue;
        }
        seen.add(stem.toLowerCase());
        // 身份以文件名为准：手工复制出来、文件里的 name 还是旧名字的，按文件名认（下次写这个文件时改正 name）
        const entry = entryFromParsed(parsed, stem);
        entries.push(entry);
        savedFiles.set(entry.id, f.name);
      }
      if (mine !== loadTicket) return;
      const recipes = orderEntries(entries, index.order);
      const set: RecipeSet = {
        recipes,
        defaultName: index.defaultName && recipes.some((r) => r.name === index.defaultName) ? index.defaultName : null,
      };
      useRecipeStore.setState({ set, saved: set, savedFiles, diskText, problems, status: "ready" });
      rebaseHistoryRecipes(before, set);
      if (set.defaultName) selectRecipe(set.defaultName, "auto");
    } catch (e) {
      if (mine !== loadTicket) return;
      useRecipeStore.setState({
        status: "error",
        problems: [{ file: baseNameOf(dir), message: e instanceof Error ? e.message : String(e) }],
      });
    }
  })();
  return loading;
}

/** 图换了路径但还是同一张图（第一次存盘、脚本直接 markSaved）：内存里没有配方就只换目录，
 *  有配方的情况由 saveRecipes 自己处理（另存为：全部写进新目录）。 */
export function followGraphPath(graphPath: string | null): void {
  const rs = useRecipeStore.getState();
  const dir = graphPath && transport.kind !== "static" ? recipeDirOf(graphPath) : null;
  if (rs.dir === dir) return;
  if (rs.set.recipes.length === 0 && rs.saved.recipes.length === 0) {
    useRecipeStore.setState({ dir, status: dir ? "ready" : "none", savedFiles: new Map(), diskText: new Map() });
  }
}

// ------------------------------------------------------------ 存盘

type WriteOp = { entry: RecipeEntry; file: string; fresh: boolean };

interface SavePlan {
  dir: string;
  set: RecipeSet;
  doc: GraphDoc;
  deletes: { id: string; file: string }[];
  renames: { id: string; from: string; to: string }[];
  writes: WriteOp[];
  indexText: string | null;
  relocated: boolean;
}

function planSave(doc: GraphDoc, dir: string): SavePlan | null {
  const rs = useRecipeStore.getState();
  const relocated = rs.dir !== dir;
  const set = rs.set;
  const saved = relocated ? EMPTY_SET : rs.saved;
  const savedFiles = relocated ? new Map<string, string>() : rs.savedFiles;
  if (set === saved) return null;
  const savedById = new Map(saved.recipes.map((e) => [e.id, e]));
  const plan: SavePlan = { dir, set, doc, deletes: [], renames: [], writes: [], indexText: null, relocated };
  for (const entry of set.recipes) {
    const file = recipeFileName(entry.name);
    const old = savedFiles.get(entry.id);
    if (old === undefined) {
      plan.writes.push({ entry, file, fresh: true });
      continue;
    }
    if (old !== file) plan.renames.push({ id: entry.id, from: old, to: file });
    if (entry !== savedById.get(entry.id) || old !== file) plan.writes.push({ entry, file, fresh: false });
  }
  const alive = new Set(set.recipes.map((e) => e.id));
  for (const [id, file] of savedFiles) if (!alive.has(id)) plan.deletes.push({ id, file });
  const indexText = serializeIndex(set);
  const known = relocated ? undefined : rs.diskText.get(RECIPE_INDEX);
  if (known !== indexText && (set.recipes.length > 0 || known !== undefined)) plan.indexText = indexText;
  return plan;
}

interface Conflict {
  file: string;
  why: "modified" | "deleted" | "exists";
}

/** 存盘前看一眼：要动的文件是不是在上次读 / 写之后被别人改过（不做合并，只检测，计划「不做」一节）。 */
async function findConflicts(plan: SavePlan): Promise<Conflict[]> {
  const rs = useRecipeStore.getState();
  const known = plan.relocated ? new Map<string, string>() : rs.diskText;
  const listing = await transport.listRecipeDir(plan.dir);
  const present = new Set(listing.files.map((f) => f.name));
  const out: Conflict[] = [];
  const check = async (file: string) => {
    const was = known.get(file);
    if (was === undefined) {
      if (present.has(file)) out.push({ file, why: "exists" });
      return;
    }
    if (!present.has(file)) {
      out.push({ file, why: "deleted" });
      return;
    }
    const now = await readOrNull(joinPath(plan.dir, file));
    if (now !== was) out.push({ file, why: "modified" });
  };
  const leaving = new Set([...plan.deletes.map((d) => d.file), ...plan.renames.map((r) => r.from)]);
  for (const d of plan.deletes) await check(d.file);
  for (const r of plan.renames) await check(r.from);
  for (const w of plan.writes) {
    const rename = plan.renames.find((r) => r.id === w.entry.id);
    if (rename) continue; // 改名的源已经查过；目标名在下面按「新文件」查
    if (w.fresh && leaving.has(w.file)) continue; // 同名的旧文件这次会先删 / 先挪走
    await check(w.file);
  }
  for (const r of plan.renames) {
    if (!leaving.has(r.to) && present.has(r.to) && known.get(r.to) === undefined) out.push({ file: r.to, why: "exists" });
  }
  if (plan.indexText !== null) await check(RECIPE_INDEX);
  return out;
}

const WHY: Record<Conflict["why"], string> = {
  modified: "在编辑器外被修改过",
  deleted: "在编辑器外被删除了",
  exists: "目录里已经有同名文件（不是这个编辑器写的）",
};

/** 「重新载入」：这几个文件以磁盘为准，替换内存里对应的配方（一步撤销，撤销就回到编辑器里的版本）。 */
async function reloadConflicts(plan: SavePlan, conflicts: readonly Conflict[]): Promise<void> {
  const rs = useRecipeStore.getState();
  const savedFiles = new Map(rs.savedFiles);
  const diskText = new Map(rs.diskText);
  let recipes = [...rs.set.recipes];
  let savedRecipes = [...rs.saved.recipes];
  let defaultName = rs.set.defaultName;
  let savedDefault = rs.saved.defaultName;
  const byFile = new Map<string, string>();
  for (const [id, f] of savedFiles) byFile.set(f, id);
  for (const c of conflicts) {
    const text = await readOrNull(joinPath(plan.dir, c.file));
    if (c.file === RECIPE_INDEX) {
      if (text !== null) {
        const idx = parseIndex(text);
        diskText.set(RECIPE_INDEX, text);
        defaultName = idx.defaultName;
        savedDefault = idx.defaultName;
        recipes = orderEntries(recipes, idx.order);
      } else {
        diskText.delete(RECIPE_INDEX);
      }
      continue;
    }
    const stem = recipeStem(c.file) ?? c.file;
    // 这个文件对应的内存配方：按簿记认，新建的按名字认
    const id = byFile.get(c.file) ?? recipes.find((e) => recipeFileName(e.name) === c.file)?.id;
    recipes = recipes.filter((e) => e.id !== id);
    savedRecipes = savedRecipes.filter((e) => e.id !== id);
    if (id) savedFiles.delete(id);
    const parsed = text === null ? null : parseRecipeText(text, stem);
    if (text === null || !parsed || "error" in parsed) {
      diskText.delete(c.file);
      continue;
    }
    const entry = { ...entryFromParsed(parsed, stem), id: id ?? newRecipeId() };
    recipes.push(entry);
    savedRecipes.push(entry);
    savedFiles.set(entry.id, c.file);
    diskText.set(c.file, text);
  }
  if (defaultName && !recipes.some((e) => e.name === defaultName)) defaultName = null;
  const next: RecipeSet = { recipes, defaultName };
  useGraphStore.getState().replaceRecipes(`重新载入配方文件（${conflicts.length} 个）`, next);
  useRecipeStore.setState({
    saved: { recipes: savedRecipes, defaultName: savedDefault },
    savedFiles,
    diskText,
  });
}

/** 存盘的第一步：算要做什么、查外部修改、问用户。返回 null = 没有要写的；"cancelled" = 用户取消了
 *  （调用方连图一起不存）。第二步 commitRecipeSave 在图写完之后做。 */
export async function prepareRecipeSave(doc: GraphDoc, graphPath: string): Promise<SavePlan | null | "cancelled"> {
  if (transport.kind === "static") return null;
  await loading;
  const dir = recipeDirOf(graphPath);
  let plan = planSave(doc, dir);
  if (!plan) return null;
  for (let round = 0; round < 3; round += 1) {
    const conflicts = await findConflicts(plan);
    if (conflicts.length === 0) return plan;
    const items = conflicts.map((c) => `${c.file}：${WHY[c.why]}`);
    const choices = plan.relocated
      ? [
          { id: "overwrite", label: "覆盖", tone: "danger" as const },
          { id: "cancel", label: "取消保存" },
        ]
      : [
          { id: "overwrite", label: "覆盖（写编辑器里的版本）", tone: "danger" as const },
          { id: "reload", label: "重新载入这些文件", tone: "primary" as const },
          { id: "cancel", label: "取消保存" },
        ];
    const answer = await askChoice({
      title: "配方文件已被外部修改",
      message: "保存会覆盖下面这些文件。重新载入 = 这几个以磁盘上的为准（可以 Ctrl+Z 回到编辑器里的版本），其余照常保存。",
      items,
      choices,
    });
    if (answer === "overwrite") return plan;
    if (answer !== "reload") return "cancelled";
    await reloadConflicts(plan, conflicts);
    plan = planSave(useGraphStore.getState().doc, dir);
    if (!plan) return null;
  }
  return plan;
}

/** 存盘的第二步：删、改名（两段式，换名互换也不撞）、写、index.json。簿记逐步更新：中途失败时
 *  已经做完的那几步记得住，下次存盘不会重复或误报外部修改。 */
export async function commitRecipeSave(plan: SavePlan): Promise<void> {
  const rs = useRecipeStore.getState();
  const savedFiles = new Map(plan.relocated ? [] : rs.savedFiles);
  const diskText = new Map(plan.relocated ? [] : rs.diskText);
  const at = (f: string) => joinPath(plan.dir, f);
  const flush = () => useRecipeStore.setState({ savedFiles: new Map(savedFiles), diskText: new Map(diskText) });
  try {
    for (const d of plan.deletes) {
      await transport.deleteRecipeFile(at(d.file));
      savedFiles.delete(d.id);
      diskText.delete(d.file);
    }
    const temps = plan.renames.map((r, i) => ({ ...r, tmp: `~${i}~${r.to}` }));
    for (const r of temps) {
      await transport.renameRecipeFile(at(r.from), at(r.tmp));
      diskText.delete(r.from);
      savedFiles.set(r.id, r.tmp);
    }
    for (const r of temps) {
      await transport.renameRecipeFile(at(r.tmp), at(r.to));
      savedFiles.set(r.id, r.to);
    }
    for (const w of plan.writes) {
      const text = serializeRecipe(w.entry, plan.doc);
      await transport.writeRecipeFile(at(w.file), text);
      savedFiles.set(w.entry.id, w.file);
      diskText.set(w.file, text);
    }
    if (plan.indexText !== null) {
      await transport.writeRecipeFile(at(RECIPE_INDEX), plan.indexText);
      diskText.set(RECIPE_INDEX, plan.indexText);
    }
  } catch (e) {
    flush();
    throw e;
  }
  useRecipeStore.setState({
    dir: plan.dir,
    status: "ready",
    saved: plan.set,
    savedFiles,
    diskText,
  });
  await discardRecipeAutosave();
}

// ------------------------------------------------------------ 自动备份（30 s，与 `<图>~` 同一个节拍）

/** 把内存里整个配方集合写进 `<配方目录>/autosave~.json`（只在有没存的改动时）。崩溃后打开图、选「恢复备份」
 *  时连它一起恢复。每个配方记着它对应的磁盘文件，恢复后改名 / 删除照样认得出来。 */
export async function writeRecipeAutosave(): Promise<void> {
  const rs = useRecipeStore.getState();
  if (!rs.dir || rs.set === rs.saved || transport.kind === "static") return;
  const payload = {
    schemaVersion: 1,
    savedAt: new Date().toISOString(),
    defaultName: rs.set.defaultName,
    recipes: rs.set.recipes.map((e) => ({
      file: rs.savedFiles.get(e.id) ?? null,
      name: e.name,
      values: e.values,
      note: e.note,
      graph: e.graph,
      updatedAt: e.updatedAt,
    })),
  };
  try {
    await transport.writeRecipeFile(joinPath(rs.dir, RECIPE_AUTOSAVE), JSON.stringify(payload));
  } catch {
    /* 备份失败不打断编辑，下一个 30 s 再试 */
  }
}

/** 从自动备份恢复内存里的配方集合（打开图时用户选了「恢复备份」）。恢复出来的是未保存状态。 */
export async function restoreRecipeAutosave(): Promise<boolean> {
  await loading;
  const rs = useRecipeStore.getState();
  if (!rs.dir) return false;
  const text = await readOrNull(joinPath(rs.dir, RECIPE_AUTOSAVE));
  if (text === null) return false;
  let raw: { defaultName?: unknown; recipes?: unknown };
  try {
    raw = JSON.parse(text) as typeof raw;
  } catch {
    return false;
  }
  if (!Array.isArray(raw.recipes)) return false;
  const idOfFile = new Map<string, string>();
  for (const [id, f] of rs.savedFiles) idOfFile.set(f, id);
  const recipes: RecipeEntry[] = [];
  for (const item of raw.recipes as Record<string, unknown>[]) {
    if (typeof item?.["name"] !== "string") continue;
    const file = typeof item["file"] === "string" ? item["file"] : null;
    const g = item["graph"] as { id?: unknown; specDigest?: unknown } | null | undefined;
    recipes.push({
      id: (file && idOfFile.get(file)) || newRecipeId(),
      name: item["name"],
      values: (item["values"] as Record<string, unknown>) ?? {},
      note: typeof item["note"] === "string" ? item["note"] : undefined,
      graph: g && typeof g.id === "string" && typeof g.specDigest === "string" ? { id: g.id, specDigest: g.specDigest } : null,
      updatedAt: typeof item["updatedAt"] === "string" ? item["updatedAt"] : null,
    });
  }
  const set: RecipeSet = {
    recipes,
    defaultName: typeof raw.defaultName === "string" && recipes.some((r) => r.name === raw.defaultName) ? raw.defaultName : null,
  };
  const before = recipeSet();
  applyRecipeSet(set);
  rebaseHistoryRecipes(before, set);
  return true;
}

export async function discardRecipeAutosave(graphPath?: string): Promise<void> {
  const dir = graphPath ? recipeDirOf(graphPath) : useRecipeStore.getState().dir;
  if (!dir || transport.kind === "static") return;
  try {
    await transport.deleteRecipeFile(joinPath(dir, RECIPE_AUTOSAVE));
  } catch {
    /* 删不掉最多下次开图再问一遍 */
  }
}

// ------------------------------------------------------------ 导入、导出（P3.6）

/** 读一个外部配方文件，准备导入。名字取文件里的 name（没有就用文件名）。 */
export async function readRecipeForImport(path: string): Promise<RecipeEntry> {
  const text = await transport.readRecipeFile(path);
  const stem = recipeStem(baseNameOf(path)) ?? baseNameOf(path);
  const parsed = parseRecipeText(text, stem);
  if ("error" in parsed) throw new Error(`${baseNameOf(path)}：${parsed.error}`);
  return entryFromParsed(parsed);
}

/** 导入：复制进配方目录（存盘时写），重名时让用户改名（P3.6）。返回最终的名字；取消返回 null。 */
export async function importRecipeFrom(path: string, name?: string): Promise<string | null> {
  const entry = await readRecipeForImport(path);
  let final = name ?? entry.name;
  const problem = recipeNameProblem(recipeSet(), final);
  if (problem) {
    const answer = await askText({
      title: "导入配方：换个名字",
      message: `${problem}。给导入的这一份起个名字：`,
      value: final,
      okLabel: "导入",
      validate: (v) => recipeNameProblem(recipeSet(), v),
    });
    if (!answer) return null;
    final = answer.value;
  }
  const ok = useGraphStore.getState().addImportedRecipe({ ...entry, name: final });
  return ok ? final : null;
}

/** 导出：把配方按当前格式另存到任意位置（立即写，不进撤销、不改内存里的配方）。 */
export async function exportRecipeTo(name: string, path: string): Promise<void> {
  const entry = recipeSet().recipes.find((r) => r.name === name);
  if (!entry) throw new Error(`没有配方 ${name}`);
  await transport.writeRecipeFile(path, serializeRecipe(entry, useGraphStore.getState().doc));
}
