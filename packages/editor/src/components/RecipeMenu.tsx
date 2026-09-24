// 工具栏右侧的配方下拉框（param-recipe P3.3）：显示当前配方名，配方有没存的改动时旁边一个橙色小圆点。
// 菜单：「基础」+ 各配方（★ = 默认，红徽标 = 失配数）+「新建配方…」「管理配方…」。
// 切换不弹窗、不进撤销：未保存的改动留在内存里（K6 ③）。

import { useEffect, useRef, useState } from "react";

import { BASE_LABEL } from "../lib/recipes";
import { useGraphStore } from "../store/graph";
import { selectRecipe, useRecipeReports, useRecipeStore, useRecipesDirty } from "../store/recipe";
import { useUiStore } from "../store/ui";

import { newRecipeInteractive } from "./recipeActions";

import "../styles.recipe.css";

export function RecipeMenu() {
  const set = useRecipeStore((s) => s.set);
  const saved = useRecipeStore((s) => s.saved);
  const current = useRecipeStore((s) => s.current);
  const dir = useRecipeStore((s) => s.dir);
  const status = useRecipeStore((s) => s.status);
  const dirty = useRecipesDirty();
  const doc = useGraphStore((s) => s.doc);
  const reports = useRecipeReports(doc);
  const [open, setOpen] = useState(false);
  const box = useRef<HTMLDivElement>(null);

  // 点菜单外面收起
  useEffect(() => {
    if (!open) return;
    const away = (e: PointerEvent) => {
      if (box.current && !box.current.contains(e.target as Node)) setOpen(false);
    };
    window.addEventListener("pointerdown", away, true);
    return () => window.removeEventListener("pointerdown", away, true);
  }, [open]);

  const blocking = current ? (reports.get(current)?.blocking ?? 0) : 0;
  const pick = (name: string | null) => {
    setOpen(false);
    selectRecipe(name);
  };

  return (
    <div className="recipe-menu" ref={box}>
      <button
        type="button"
        className={`recipe-menu__toggle${current ? " is-recipe" : ""}`}
        data-testid="recipe-toggle"
        data-current={current ?? ""}
        data-dirty={dirty ? "1" : "0"}
        aria-expanded={open}
        title={
          dir
            ? `当前配方：${current ?? `${BASE_LABEL}（只用默认值）`}。切换不弹窗，没存的改动留在内存里，Ctrl+S 一起保存`
            : "图还没存过盘：先保存图，才能建配方"
        }
        onClick={() => setOpen((v) => !v)}
      >
        <span className="recipe-menu__label">配方</span>
        <span className="recipe-menu__name" data-testid="recipe-current">
          {current ?? BASE_LABEL}
        </span>
        {blocking > 0 && (
          <span className="recipe-badge recipe-badge--bad" title={`${blocking} 处失配：这个配方不能运行`}>
            {blocking}
          </span>
        )}
        {dirty && <span className="recipe-menu__dirty" data-testid="recipe-dirty" title="配方有没存的改动" />}
        <span className="recipe-menu__chev">▾</span>
      </button>
      {open && (
        <div className="recipe-menu__list" data-testid="recipe-menu" role="menu">
          <button
            type="button"
            role="menuitemradio"
            aria-checked={current === null}
            className={`recipe-menu__item${current === null ? " is-current" : ""}`}
            data-testid="recipe-option"
            data-name=""
            onClick={() => pick(null)}
          >
            <span className="recipe-menu__check">{current === null ? "●" : ""}</span>
            <span className="recipe-menu__itemname">{BASE_LABEL}</span>
            <span className="recipe-menu__hint">只用默认值</span>
          </button>
          {set.recipes.map((r) => {
            const report = reports.get(r.name);
            const unsaved = !saved.recipes.includes(r);
            return (
              <button
                key={r.id}
                type="button"
                role="menuitemradio"
                aria-checked={current === r.name}
                className={`recipe-menu__item${current === r.name ? " is-current" : ""}`}
                data-testid="recipe-option"
                data-name={r.name}
                onClick={() => pick(r.name)}
              >
                <span className="recipe-menu__check">{current === r.name ? "●" : ""}</span>
                <span className="recipe-menu__itemname">{r.name}</span>
                {set.defaultName === r.name && (
                  <span className="recipe-menu__star" title="默认配方：打开图时选它">★</span>
                )}
                {unsaved && <span className="recipe-menu__dirty" title="没存的改动" />}
                <span className="recipe-menu__hint">{Object.keys(r.values).length} 个值</span>
                {(report?.blocking ?? 0) > 0 && (
                  <span className="recipe-badge recipe-badge--bad" title="失配数：这个配方不能运行">
                    {report!.blocking}
                  </span>
                )}
              </button>
            );
          })}
          {status === "loading" && <p className="recipe-menu__note">正在读配方目录…</p>}
          <div className="recipe-menu__sep" />
          <button
            type="button"
            className="recipe-menu__item recipe-menu__action"
            data-testid="recipe-new"
            disabled={!dir}
            title={dir ? "新建一个配方（空的，或复制当前配方）" : "先保存图"}
            onClick={() => {
              setOpen(false);
              void newRecipeInteractive();
            }}
          >
            新建配方…
          </button>
          <button
            type="button"
            className="recipe-menu__item recipe-menu__action"
            data-testid="recipe-manage"
            onClick={() => {
              setOpen(false);
              const ui = useUiStore.getState();
              ui.toggleParamPanel(true);
              ui.setParamPanelTab("recipes");
            }}
          >
            管理配方…
          </button>
        </div>
      )}
    </div>
  );
}
