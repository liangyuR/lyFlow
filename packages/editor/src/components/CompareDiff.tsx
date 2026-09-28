// 对比的差异表（docs/compare-plan.md §2.4）：列 字段 | A | B | Δ，不同的行有 accent 左边框，
// 仅一侧有的端口灰字。规则全在 lib/compareDiff，这里只管画。

import { useState } from "react";

import type { DiffResult } from "../lib/compareDiff";

export function CompareDiff({ diff }: { diff: DiffResult }) {
  const [open, setOpen] = useState(true);
  const [onlyChanged, setOnlyChanged] = useState(false);
  const rows = onlyChanged ? diff.rows.filter((r) => r.changed) : diff.rows;
  return (
    <section
      className={`compare-diff${diff.modeMismatch ? " is-mismatch" : ""}`}
      data-testid="compare-diff"
      data-changed-count={diff.changed}
      data-mode-mismatch={diff.modeMismatch ? "1" : "0"}
    >
      <header className="compare-diff__head">
        <button
          type="button"
          className="compare-diff__toggle"
          onClick={() => setOpen(!open)}
          aria-expanded={open}
          title={open ? "收起差异表" : "展开差异表"}
        >
          {open ? "▾" : "▸"} 差异
        </button>
        <span className="compare-diff__summary">
          <b className={diff.changed > 0 ? "is-changed" : ""}>{diff.changed} 项不同</b>
          {` · ${diff.same} 项相同`}
          {diff.only > 0 && ` · ${diff.only} 项仅一侧有`}
        </span>
        {diff.modeMismatch && (
          <span className="compare-diff__badge" title="一侧来自预览运行（源头抽稀），点数与量测都变了">
            预览中，数值仅供参考
          </span>
        )}
        <span className="viewer__spacer" />
        <label className="compare-diff__filter">
          <input
            type="checkbox"
            data-testid="compare-diff-only-changed"
            checked={onlyChanged}
            onChange={(e) => setOnlyChanged(e.target.checked)}
          />
          只看不同
        </label>
      </header>
      {open && (
        <div className="compare-diff__scroll">
          {rows.length === 0 ? (
            <div className="compare-diff__empty">{diff.rows.length === 0 ? "两侧都还没有输出" : "没有不同"}</div>
          ) : (
            <table className="compare-diff__table">
              <thead>
                <tr>
                  <th>字段</th>
                  <th>A</th>
                  <th>B</th>
                  <th>Δ（A − B）</th>
                </tr>
              </thead>
              <tbody>
                {rows.map((r) => (
                  <tr
                    key={r.key}
                    className={r.kind === "only" ? "is-only" : r.changed ? "is-changed" : undefined}
                    data-key={r.key}
                    data-changed={r.changed ? "1" : "0"}
                    data-kind={r.kind}
                    title={r.kind === "only" ? `只有 ${r.side} 侧有这个端口（${r.type}）` : r.type}
                  >
                    <td className="compare-diff__key">{r.key}</td>
                    <td>{r.a}</td>
                    <td>{r.b}</td>
                    <td className="compare-diff__delta" data-testid="compare-diff-delta">
                      {r.delta ?? ""}
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          )}
        </div>
      )}
    </section>
  );
}
