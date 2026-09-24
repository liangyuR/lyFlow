// 配方管理（param-recipe P3.6 / P3.7）：左边是列表（★ 默认、值个数、失配数红徽标），顶上新建 / 复制 / 导入，
// 底下是配方目录；右边是选中配方的详情（文件名、值个数、设为默认 / 重命名 / 删除 / 导出）与失配报告：
// 每条一个彩色类别徽标 + 描述 + 建议修复按钮，底部「全部按建议修复」（一次撤销）。
// 管理动作都改内存里的配方集合、进撤销栈，Ctrl+S 时才落盘（导出除外：它立即写）。

import { useEffect, useState } from "react";

import { baseNameOf, KIND_LABEL, recipeFileName, type Mismatch } from "../lib/recipes";
import { useGraphStore } from "../store/graph";
import { selectRecipe, useRecipeReports, useRecipeStore } from "../store/recipe";

import {
  deleteRecipeInteractive,
  duplicateRecipeInteractive,
  exportRecipeInteractive,
  importRecipeInteractive,
  newRecipeInteractive,
  renameRecipeInteractive,
} from "./recipeActions";

export function RecipeManager() {
  const doc = useGraphStore((s) => s.doc);
  const set = useRecipeStore((s) => s.set);
  const saved = useRecipeStore((s) => s.saved);
  const current = useRecipeStore((s) => s.current);
  const dir = useRecipeStore((s) => s.dir);
  const status = useRecipeStore((s) => s.status);
  const problems = useRecipeStore((s) => s.problems);
  const reports = useRecipeReports(doc);
  const [picked, setPicked] = useState<string | null>(current);

  // 选中的配方没了（删掉、改名）就退回当前配方或第一个
  const exists = picked !== null && set.recipes.some((r) => r.name === picked);
  useEffect(() => {
    if (!exists) setPicked(current ?? set.recipes[0]?.name ?? null);
  }, [exists, current, set]);
  const entry = set.recipes.find((r) => r.name === picked) ?? null;
  const report = entry ? reports.get(entry.name) : undefined;

  const choose = async (p: Promise<string | null>) => {
    const name = await p;
    if (name) setPicked(name);
  };

  return (
    <div className="ppanel__body mg" data-testid="recipe-manager">
      <aside className="mg-side">
        <div className="mg-tools">
          <button type="button" className="lf-btn" data-testid="mg-new" disabled={!dir} onClick={() => void choose(newRecipeInteractive(null))}>
            + 新建
          </button>
          <button
            type="button"
            className="lf-btn"
            data-testid="mg-dup"
            disabled={!entry}
            onClick={() => entry && void choose(duplicateRecipeInteractive(entry.name))}
          >
            复制
          </button>
          <button type="button" className="lf-btn" data-testid="mg-import" disabled={!dir} onClick={() => void choose(importRecipeInteractive())}>
            导入…
          </button>
        </div>
        <ul className="mg-list" data-testid="mg-list">
          {set.recipes.length === 0 && (
            <li className="mg-empty">
              {dir ? "还没有配方。「+ 新建」建一个空配方，所有值先沿用基础。" : "图还没存过盘：先保存图（Ctrl+S），配方存在图文件旁边。"}
            </li>
          )}
          {set.recipes.map((r) => {
            const rep = reports.get(r.name);
            const unsaved = !saved.recipes.includes(r);
            return (
              <li key={r.id}>
                <button
                  type="button"
                  className={`mg-item${r.name === picked ? " is-picked" : ""}${r.name === current ? " is-current" : ""}`}
                  data-testid="mg-item"
                  data-name={r.name}
                  onClick={() => setPicked(r.name)}
                  onDoubleClick={() => selectRecipe(r.name)}
                  title="单击看详情；双击切到这个配方"
                >
                  <span className={`mg-item__star${set.defaultName === r.name ? " is-on" : ""}`} title="默认配方">
                    ★
                  </span>
                  <span className="mg-item__name">{r.name}</span>
                  {unsaved && <span className="recipe-menu__dirty" title="没存的改动" />}
                  <span className="mg-item__count">{Object.keys(r.values).length} 个值</span>
                  {(rep?.blocking ?? 0) > 0 && (
                    <span className="recipe-badge recipe-badge--bad" data-testid="mg-item-bad" title="失配数：这个配方不能运行">
                      {rep!.blocking}
                    </span>
                  )}
                  {rep?.items.some((m) => m.kind === "spec") && (
                    <span className="recipe-badge recipe-badge--spec" title="规格变了（只提示，不阻止运行）">④</span>
                  )}
                </button>
              </li>
            );
          })}
        </ul>
        <footer className="mg-dir" data-testid="mg-dir" title={dir ?? ""}>
          {dir ? (
            <>
              配方目录 <code>{baseNameOf(dir)}/</code>
              {status === "loading" && " · 正在读…"}
            </>
          ) : (
            "图还没存过盘，没有配方目录"
          )}
          {problems.length > 0 && (
            <ul className="mg-problems" data-testid="mg-problems">
              {problems.map((p) => (
                <li key={p.file}>
                  <code>{p.file}</code>：{p.message}
                </li>
              ))}
            </ul>
          )}
        </footer>
      </aside>

      <section className="mg-main">
        {!entry ? (
          <p className="pp-empty">选一个配方看它的详情与失配报告。</p>
        ) : (
          <>
            <header className="mg-head">
              <div className="mg-head__title">
                <h3 data-testid="mg-title">
                  {entry.name}
                  {set.defaultName === entry.name && <span className="recipe-menu__star"> ★ 默认</span>}
                  {entry.name === current && <span className="mg-tag">当前</span>}
                  {!saved.recipes.includes(entry) && <span className="mg-tag mg-tag--dirty">未保存</span>}
                </h3>
                <p className="mg-head__meta">
                  <code>{recipeFileName(entry.name)}</code> · {Object.keys(entry.values).length} 个值
                  {entry.updatedAt && ` · 改于 ${entry.updatedAt.replace("T", " ").slice(0, 19)}`}
                </p>
              </div>
              <div className="mg-head__actions">
                {entry.name !== current && (
                  <button type="button" className="lf-btn" data-testid="mg-use" onClick={() => selectRecipe(entry.name)}>
                    切到它
                  </button>
                )}
                <button
                  type="button"
                  className="lf-btn"
                  data-testid="mg-default"
                  onClick={() =>
                    useGraphStore.getState().setDefaultRecipe(set.defaultName === entry.name ? null : entry.name)
                  }
                >
                  {set.defaultName === entry.name ? "取消默认" : "设为默认"}
                </button>
                <button type="button" className="lf-btn" data-testid="mg-rename" onClick={() => void choose(renameRecipeInteractive(entry.name))}>
                  重命名
                </button>
                <button type="button" className="lf-btn" data-testid="mg-export" onClick={() => void exportRecipeInteractive(entry.name)}>
                  导出…
                </button>
                <button type="button" className="lf-btn lf-btn--danger" data-testid="mg-delete" onClick={() => void deleteRecipeInteractive(entry.name)}>
                  删除
                </button>
              </div>
            </header>

            <h4 className="mg-section">失配报告</h4>
            {!report || report.items.length === 0 ? (
              <p className="mg-ok" data-testid="mg-no-mismatch">没有失配：配方里每个值都对得上当前图的图参数规格。</p>
            ) : (
              <ul className="mg-report" data-testid="mg-report">
                {report.items.map((m, i) => (
                  <MismatchRow key={`${m.kind}:${m.param ?? ""}:${i}`} recipe={entry.name} m={m} />
                ))}
              </ul>
            )}
            {report && report.items.length > 0 && (
              <footer className="mg-foot">
                <span className="mg-foot__note">
                  {report.blocking > 0
                    ? "有失配时这个配方不能运行；其余配方不受影响。"
                    : "只有「规格变了」的提示：不阻止运行。"}
                </span>
                <button
                  type="button"
                  className="lf-btn lf-btn--primary"
                  data-testid="mg-fix-all"
                  onClick={() => useGraphStore.getState().fixRecipe(entry.name, report.items)}
                >
                  全部按建议修复
                </button>
              </footer>
            )}
          </>
        )}
      </section>
    </div>
  );
}

function MismatchRow({ recipe, m }: { recipe: string; m: Mismatch }) {
  return (
    <li className="mg-mm" data-testid="mg-mismatch" data-kind={m.kind} data-param={m.param ?? ""}>
      <span className={`mg-kind mg-kind--${m.kind}`}>{KIND_LABEL[m.kind]}</span>
      <div className="mg-mm__body">
        {m.param && <code className="mg-mm__param">{m.param}</code>}
        <span className="mg-mm__msg">{m.message}</span>
      </div>
      <button
        type="button"
        className="lf-btn mg-mm__fix"
        data-testid="mg-fix"
        onClick={() => useGraphStore.getState().fixRecipe(recipe, [m])}
      >
        {m.fixLabel}
      </button>
    </li>
  );
}
