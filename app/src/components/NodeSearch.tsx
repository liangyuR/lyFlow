// 画布上的算子搜索弹层（交互清单 P0 #9）。双击空白处唤起，在光标处落节点。
// 键盘全程可用（输入过滤、↑↓、Enter、Esc）—— 鼠标点选是退路不是主路。

import { useEffect, useMemo, useRef, useState } from "react";

import { searchOperators, FIELD_LABELS } from "../lib/search";
import { useGraphStore } from "../store/graph";
import { useManifestStore } from "../store/manifest";
import { useUiStore } from "../store/ui";

const MAX_ROWS = 40;
const POPUP_WIDTH = 340;
const POPUP_HEIGHT = 360;

export function NodeSearch() {
  const popup = useUiStore((s) => s.searchPopup);
  const closeSearch = useUiStore((s) => s.closeSearch);
  const operators = useManifestStore((s) => s.bundle?.operators);

  const [query, setQuery] = useState("");
  const [cursor, setCursor] = useState(0);
  const inputRef = useRef<HTMLInputElement>(null);
  const listRef = useRef<HTMLDivElement>(null);

  // 每次打开都从空查询开始 —— 保留上次的输入会让人不知道自己在看什么
  useEffect(() => {
    if (popup) {
      setQuery("");
      setCursor(0);
      inputRef.current?.focus();
    }
  }, [popup]);

  const rows = useMemo(() => {
    const all = operators ?? [];
    if (!query.trim()) {
      // 空查询列全部，让人知道有哪些可用，而不是面对一个空白框
      return all.slice(0, MAX_ROWS).map((op) => ({ op, fieldIndex: 0, indices: [] as number[] }));
    }
    return searchOperators(all, query).slice(0, MAX_ROWS);
  }, [operators, query]);

  useEffect(() => setCursor(0), [query]);

  useEffect(() => {
    if (rows.length === 0) return;
    const id = rows[Math.min(cursor, rows.length - 1)]?.op.id;
    if (!id) return;
    listRef.current
      ?.querySelector(`[data-op-id="${CSS.escape(id)}"]`)
      ?.scrollIntoView({ block: "nearest" });
  }, [cursor, rows]);

  if (!popup) return null;

  const pick = (opId: string) => {
    const graph = useGraphStore.getState();
    const nodeId = graph.addNode(opId, popup.flow);
    if (nodeId) {
      useUiStore.getState().setSelection([nodeId], []);
      // 从端口拖出、中途松手弹出的搜索面板：选中后自动接上。
      // 接不上（类型不匹配）不算错误 —— 节点已经落下了，用户可以自己改。
      if (popup.pendingFrom) {
        const op = useManifestStore.getState().operatorsById.get(opId);
        const firstInput = op?.inputs[0];
        if (firstInput) {
          graph.connect(popup.pendingFrom, { node: nodeId, port: firstInput.name });
        }
      }
    }
    closeSearch();
  };

  // 贴着光标放，但不能溢出窗口
  const left = Math.min(popup.screen.x, window.innerWidth - POPUP_WIDTH - 12);
  const top = Math.min(popup.screen.y, window.innerHeight - POPUP_HEIGHT - 12);

  const onKeyDown = (e: React.KeyboardEvent) => {
    if (e.key === "ArrowDown") {
      e.preventDefault();
      setCursor((c) => Math.min(c + 1, rows.length - 1));
    } else if (e.key === "ArrowUp") {
      e.preventDefault();
      setCursor((c) => Math.max(c - 1, 0));
    } else if (e.key === "Enter") {
      e.preventDefault();
      const hit = rows[Math.min(cursor, rows.length - 1)];
      if (hit) pick(hit.op.id);
    } else if (e.key === "Escape") {
      e.preventDefault();
      closeSearch();
    }
    // 阻止冒泡：否则 Delete/方向键会被画布的快捷键抢走
    e.stopPropagation();
  };

  return (
    <>
      <div className="search-backdrop" onClick={closeSearch} />
      <div
        className="search-popup"
        style={{ left, top, width: POPUP_WIDTH, maxHeight: POPUP_HEIGHT }}
        onKeyDown={onKeyDown}
      >
        <input
          ref={inputRef}
          className="search-popup__input"
          type="text"
          value={query}
          placeholder="搜索算子…"
          spellCheck={false}
          onChange={(e) => setQuery(e.target.value)}
        />
        <div className="search-popup__list" ref={listRef}>
          {rows.length === 0 ? (
            <p className="search-popup__empty">没有匹配的算子</p>
          ) : (
            rows.map((hit, i) => (
              <button
                key={hit.op.id}
                type="button"
                data-op-id={hit.op.id}
                className={`search-popup__row${i === Math.min(cursor, rows.length - 1) ? " is-active" : ""}`}
                onMouseEnter={() => setCursor(i)}
                onClick={() => pick(hit.op.id)}
              >
                <span className="search-popup__label">{hit.op.label}</span>
                <span className="search-popup__cat">{hit.op.category}</span>
                {hit.fieldIndex > 0 && (
                  <span className="search-popup__why">{FIELD_LABELS[hit.fieldIndex]}</span>
                )}
              </button>
            ))
          )}
        </div>
      </div>
    </>
  );
}
