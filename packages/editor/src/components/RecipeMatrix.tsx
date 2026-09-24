// 配方矩阵（param-recipe P3.5）：行是图参数（按 group 分组），列是「基础」+ 各配方（当前配方列头标 ●）。
// 单元格三态：沿用基础（暗灰字，不存）、配方存的值（浅橙底、橙字）、越界或非法（浅红底、红字，悬停写原因）。
// 双击编辑：简单类型就地用该参数的控件改，复杂值（向量、颜色、transform、curve、多行文本、flags）弹出编辑。
// 单击选中、Ctrl 加选、Shift 连选；「复制选中 N 格 → 配方」让目标配方里这些参数的有效值等于选中格的值。
// 改值、复制都经 graph store 的动作（一次撤销，K7）；列的隐藏、选中、只看差异是纯 UI 状态。

import { useCallback, useEffect, useMemo, useRef, useState } from "react";

import { graphParamSpecOf } from "../lib/graphParams";
import { valueEquals } from "../lib/params";
import { BASE_LABEL, formatValue, KIND_LABEL } from "../lib/recipes";
import { augmentOperators } from "../lib/subgraph";
import { useGraphStore } from "../store/graph";
import { useManifestStore } from "../store/manifest";
import { selectRecipe, useRecipeReports, useRecipeStore } from "../store/recipe";
import { useUiStore } from "../store/ui";
import type { GraphParam } from "../types/graph";
import type { Param, ParamType } from "../types/manifest";

import { ParamControl } from "./ParamControls";
import { newRecipeInteractive } from "./recipeActions";

/** 基础列的列键。配方名不会是它（配方名里不能有 `:`）。 */
const BASE = ":base";

const INLINE: ReadonlySet<ParamType> = new Set(["bool", "int", "float", "enum", "string", "path"]);

type CellState = "base" | "inherit" | "override" | "invalid";

interface MatrixRow {
  name: string;
  gp: GraphParam;
  spec: Param | null;
  group: string;
}

const cellKey = (param: string, col: string) => `${param}\u0000${col}`;

export function RecipeMatrix() {
  const doc = useGraphStore((s) => s.doc);
  const baseOps = useManifestStore((s) => s.operatorsById);
  const set = useRecipeStore((s) => s.set);
  const current = useRecipeStore((s) => s.current);
  const dir = useRecipeStore((s) => s.dir);
  const reports = useRecipeReports(doc);
  const ops = useMemo(() => augmentOperators(baseOps, doc.subgraphs), [baseOps, doc.subgraphs]);

  const [onlyDiff, setOnlyDiff] = useState(false);
  const [hidden, setHidden] = useState<ReadonlySet<string>>(new Set());
  const [selected, setSelected] = useState<ReadonlySet<string>>(new Set());
  const [anchor, setAnchor] = useState<{ row: number; col: number } | null>(null);
  const [editing, setEditing] = useState<{ param: string; col: string } | null>(null);
  const [copyOpen, setCopyOpen] = useState(false);

  const rows: MatrixRow[] = useMemo(
    () =>
      Object.entries(doc.params ?? {}).map(([name, gp]) => {
        const spec = graphParamSpecOf(doc, name, gp, ops);
        return { name, gp, spec, group: spec?.group ?? gp.group ?? "" };
      }),
    [doc, ops],
  );
  const columns = useMemo(
    () => [BASE, ...set.recipes.map((r) => r.name).filter((n) => !hidden.has(n))],
    [set, hidden],
  );
  const entries = useMemo(() => new Map(set.recipes.map((r) => [r.name, r])), [set]);

  const stateOf = useCallback(
    (row: MatrixRow, col: string): { state: CellState; value: unknown; reason?: string } => {
      if (col === BASE) return { state: "base", value: row.gp.default };
      const entry = entries.get(col);
      const bad = reports.get(col)?.byParam.get(row.name);
      if (bad) return { state: "invalid", value: entry?.values[row.name], reason: `${KIND_LABEL[bad.kind]}：${bad.message}` };
      if (entry && Object.prototype.hasOwnProperty.call(entry.values, row.name)) {
        return { state: "override", value: entry.values[row.name] };
      }
      return { state: "inherit", value: row.gp.default };
    },
    [entries, reports],
  );

  // 只显示差异：至少一个（没隐藏的）配方在这一行存了值或有问题
  const shown = useMemo(
    () =>
      onlyDiff
        ? rows.filter((r) => columns.some((c) => c !== BASE && stateOf(r, c).state !== "inherit"))
        : rows,
    [rows, columns, onlyDiff, stateOf],
  );
  const diffCount = useMemo(
    () => rows.filter((r) => set.recipes.some((e) => stateOf(r, e.name).state !== "inherit")).length,
    [rows, set, stateOf],
  );

  // 行列变了（删了配方、隐藏了列、只看差异）就把选不到的格子从选区里拿掉
  useEffect(() => {
    const live = new Set<string>();
    for (const r of shown) for (const c of columns) live.add(cellKey(r.name, c));
    setSelected((s) => {
      const next = new Set([...s].filter((k) => live.has(k)));
      return next.size === s.size ? s : next;
    });
  }, [shown, columns]);

  const clickCell = (e: React.MouseEvent, ri: number, ci: number) => {
    const key = cellKey(shown[ri]!.name, columns[ci]!);
    if (e.shiftKey && anchor) {
      const next = new Set<string>();
      const [r0, r1] = [Math.min(anchor.row, ri), Math.max(anchor.row, ri)];
      const [c0, c1] = [Math.min(anchor.col, ci), Math.max(anchor.col, ci)];
      for (let r = r0; r <= r1; r += 1) for (let c = c0; c <= c1; c += 1) next.add(cellKey(shown[r]!.name, columns[c]!));
      setSelected(next);
      return;
    }
    if (e.ctrlKey || e.metaKey) {
      // 函数式更新：连着两次点击赶在一次重渲之前时，第二次也要看到第一次的结果
      setSelected((s) => {
        const next = new Set(s);
        if (next.has(key)) next.delete(key);
        else next.add(key);
        return next;
      });
    } else {
      setSelected(new Set([key]));
    }
    setAnchor({ row: ri, col: ci });
  };

  const copyTo = (target: string) => {
    setCopyOpen(false);
    const byParam = new Map<string, unknown>();
    for (const key of selected) {
      const [param, col] = key.split("\u0000") as [string, string];
      const row = rows.find((r) => r.name === param);
      if (!row) continue;
      if (byParam.has(param)) {
        useUiStore.getState().showToast(`参数 ${param} 选了不止一格：一个参数只能复制一个值`, "warn");
        return;
      }
      byParam.set(param, stateOf(row, col).value);
    }
    const n = useGraphStore.getState().copyRecipeCells(
      [...byParam].map(([param, value]) => ({ param, value })),
      target,
    );
    useUiStore.getState().showToast(n > 0 ? `已复制 ${n} 格到配方 ${target}` : `配方 ${target} 里这些值本来就一样`);
  };

  const commit = (row: MatrixRow, col: string, v: unknown) => {
    const g = useGraphStore.getState();
    if (col === BASE) g.setGraphParamDefault(row.name, v);
    else if (!valueEquals(v, stateOf(row, col).value)) g.setRecipeValue(col, row.name, v);
  };

  if (rows.length === 0) {
    return (
      <div className="ppanel__body mx" data-testid="recipe-matrix">
        <p className="pp-empty">还没有图参数：在「按节点」页点参数行右端的书签「纳入配方」，它们才会出现在这里。</p>
      </div>
    );
  }

  const hiddenCount = hidden.size;
  let lastGroup: string | null = null;
  return (
    <div className="ppanel__body mx" data-testid="recipe-matrix">
      <div className="mx-bar">
        <button
          type="button"
          className={`ctl-chip pp-chip${onlyDiff ? " is-on" : ""}`}
          data-testid="mx-diff"
          aria-pressed={onlyDiff}
          onClick={() => setOnlyDiff(true)}
        >
          只显示差异 <span className="pp-count">{diffCount}</span>
        </button>
        <button
          type="button"
          className={`ctl-chip pp-chip${!onlyDiff ? " is-on" : ""}`}
          data-testid="mx-all"
          aria-pressed={!onlyDiff}
          onClick={() => setOnlyDiff(false)}
        >
          全部 <span className="pp-count">{rows.length}</span>
        </button>
        {hiddenCount > 0 && (
          <button type="button" className="ctl-chip pp-chip" data-testid="mx-unhide" onClick={() => setHidden(new Set())}>
            显示隐藏的 {hiddenCount} 列
          </button>
        )}
        <span className="ppanel__spacer" />
        {!dir && <span className="insp__hint">图还没存过盘：先保存图才能建配方</span>}
        {dir && set.recipes.length === 0 && (
          <button type="button" className="lf-btn" data-testid="mx-new" onClick={() => void newRecipeInteractive(null)}>
            + 新建配方
          </button>
        )}
        <div className="mx-copy">
          <button
            type="button"
            className="lf-btn lf-btn--primary"
            data-testid="mx-copy"
            data-count={selected.size}
            disabled={selected.size === 0 || set.recipes.length === 0}
            onClick={() => setCopyOpen((v) => !v)}
          >
            复制选中 {selected.size} 格 → 配方 ▾
          </button>
          {copyOpen && (
            <div className="recipe-menu__list mx-copy__menu" data-testid="mx-copy-menu">
              {set.recipes.map((r) => (
                <button
                  key={r.id}
                  type="button"
                  className="recipe-menu__item"
                  data-testid={`mx-copy-to-${r.name}`}
                  onClick={() => copyTo(r.name)}
                >
                  {r.name}
                </button>
              ))}
            </div>
          )}
        </div>
      </div>
      <div className="mx-scroll">
        <table className="mx-table" data-testid="mx-table">
          <thead>
            <tr>
              <th className="mx-th mx-th--param">参数</th>
              {columns.map((c) => {
                const entry = entries.get(c);
                const bad = entry ? (reports.get(c)?.blocking ?? 0) : 0;
                return (
                  <th
                    key={c}
                    className={`mx-th${c === current || (c === BASE && current === null) ? " is-current" : ""}`}
                    data-testid={`mx-col-${c === BASE ? "base" : c}`}
                    data-col={c}
                  >
                    <button
                      type="button"
                      className="mx-th__name"
                      title={c === BASE ? "基础：图参数的默认值（点一下切到基础）" : `点一下切到配方「${c}」`}
                      onClick={() => selectRecipe(c === BASE ? null : c)}
                    >
                      {(c === current || (c === BASE && current === null)) && <span className="mx-th__dot">●</span>}
                      {c === BASE ? BASE_LABEL : c}
                      {entry && set.defaultName === c && <span className="recipe-menu__star">★</span>}
                    </button>
                    {bad > 0 && (
                      <span className="recipe-badge recipe-badge--bad" title="失配数：这个配方不能运行">
                        {bad}
                      </span>
                    )}
                    {c !== BASE && (
                      <button
                        type="button"
                        className="mx-th__hide"
                        data-testid={`mx-hide-${c}`}
                        title="隐藏这一列"
                        onClick={() => setHidden(new Set([...hidden, c]))}
                      >
                        ×
                      </button>
                    )}
                  </th>
                );
              })}
            </tr>
          </thead>
          <tbody>
            {shown.map((row, ri) => {
              const head =
                row.group !== lastGroup ? (
                  <tr key={`g:${row.group}:${ri}`} className="mx-group">
                    <td colSpan={columns.length + 1}>{row.group || "未分组"}</td>
                  </tr>
                ) : null;
              lastGroup = row.group;
              return [
                head,
                <tr key={row.name} className="mx-row" data-param={row.name}>
                  <th className="mx-td mx-td--param" scope="row" title={row.gp.doc ?? row.name}>
                    <span className="mx-param__label">{row.gp.label || row.name}</span>
                    <code className="mx-param__name">{row.name}</code>
                  </th>
                  {columns.map((c, ci) => {
                    const cell = stateOf(row, c);
                    const key = cellKey(row.name, c);
                    const isEditing = editing?.param === row.name && editing.col === c;
                    return (
                      <td
                        key={c}
                        className={`mx-td mx-cell is-${cell.state}${selected.has(key) ? " is-selected" : ""}`}
                        data-testid={`mx-cell-${row.name}-${c === BASE ? "base" : c}`}
                        data-param={row.name}
                        data-col={c === BASE ? "" : c}
                        data-state={cell.state}
                        data-selected={selected.has(key) ? "1" : undefined}
                        title={
                          cell.reason ??
                          (cell.state === "inherit"
                            ? "沿用基础（这个配方没存这个值）。双击编辑"
                            : cell.state === "override"
                              ? "这个配方存的值。双击编辑"
                              : "基础（default）。双击编辑")
                        }
                        onClick={(e) => clickCell(e, ri, ci)}
                        onDoubleClick={() => row.spec && setEditing({ param: row.name, col: c })}
                      >
                        {isEditing && row.spec ? (
                          <CellEditor
                            spec={row.spec}
                            value={cell.value}
                            inline={INLINE.has(row.spec.type)}
                            onCommit={(v) => commit(row, c, v)}
                            onReset={
                              c !== BASE && cell.state !== "inherit"
                                ? () => useGraphStore.getState().clearRecipeValue(c, row.name)
                                : undefined
                            }
                            onClose={() => setEditing(null)}
                          />
                        ) : (
                          <span className="mx-cell__text">{formatValue(cell.value, row.spec ?? undefined)}</span>
                        )}
                      </td>
                    );
                  })}
                </tr>,
              ];
            })}
          </tbody>
        </table>
      </div>
      <p className="mx-legend">
        <span className="mx-legend__item is-inherit">灰字</span> 沿用基础（不存）
        <span className="mx-legend__item is-override">橙</span> 配方存的值
        <span className="mx-legend__item is-invalid">红</span> 越界或非法（悬停看原因；这个配方不能运行）
      </p>
    </div>
  );
}

/** 单元格里的编辑器。简单类型就地、复杂的弹一个小浮层；都是该参数自己的控件（ParamControls 那一份）。
 *  点外面或 Esc 收起：数字框失焦才提交，所以收起放到下一拍，让失焦先把值交上去。 */
function CellEditor({
  spec,
  value,
  inline,
  onCommit,
  onReset,
  onClose,
}: {
  spec: Param;
  value: unknown;
  inline: boolean;
  onCommit: (v: unknown) => void;
  onReset?: (() => void) | undefined;
  onClose: () => void;
}) {
  const box = useRef<HTMLDivElement>(null);
  useEffect(() => {
    const away = (e: PointerEvent) => {
      if (box.current && !box.current.contains(e.target as Node)) setTimeout(onClose, 0);
    };
    window.addEventListener("pointerdown", away, true);
    const first = box.current?.querySelector<HTMLElement>("input, select, textarea");
    first?.focus();
    return () => window.removeEventListener("pointerdown", away, true);
  }, [onClose]);
  // 就地编辑不画滑块（格子太窄）：限位不在控件上夹，越界的值照样写进去、单元格标红，由失配报告说原因
  let param = spec;
  if (inline) {
    const { softMin: _a, softMax: _b, min: _c, max: _d, ...rest } = spec;
    param = rest as Param;
  }
  return (
    <div
      ref={box}
      className={inline ? "mx-edit mx-edit--inline" : "mx-edit mx-edit--pop"}
      data-testid="mx-editor"
      onClick={(e) => e.stopPropagation()}
      onDoubleClick={(e) => e.stopPropagation()}
      onKeyDown={(e) => {
        if (e.key === "Escape") {
          e.stopPropagation();
          (document.activeElement as HTMLElement | null)?.blur();
          setTimeout(onClose, 0);
        }
        if (e.key === "Enter" && inline) setTimeout(onClose, 0);
      }}
    >
      <ParamControl param={param} value={value} disabled={false} onChange={onCommit} />
      {!inline && (
        <div className="mx-edit__buttons">
          {onReset && (
            <button type="button" className="prow__textbtn" data-testid="mx-edit-reset" onClick={() => { onReset(); onClose(); }}>
              恢复基础
            </button>
          )}
          <button type="button" className="lf-btn" data-testid="mx-edit-done" onClick={() => setTimeout(onClose, 0)}>
            完成
          </button>
        </div>
      )}
    </div>
  );
}
