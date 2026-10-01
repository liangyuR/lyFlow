// 查找节点（Ctrl+F）。大图里找一个节点原来只能缩放着看、或者一层层进子图翻；
// 这里把整张图连子图里面的节点一起列出来模糊找，回车打开到它所在的那一层、选中并移进视野。
// 键盘全程可用（输入过滤、↑↓、Enter、Esc），与算子搜索弹层同一套手感。

import { useEffect, useMemo, useRef, useState } from "react";

import { listGraphNodes, NODE_FIELD_LABELS, searchGraphNodes } from "../lib/findNodes";
import { useExecutionStore } from "../store/execution";
import { useGraphStore } from "../store/graph";
import { useManifestStore } from "../store/manifest";
import { useUiStore } from "../store/ui";

const MAX_ROWS = 50;
const POPUP_WIDTH = 460;
const POPUP_HEIGHT = 400;

export function NodeFinder() {
  const open = useUiStore((s) => s.finderOpen);
  const setOpen = useUiStore((s) => s.setFinderOpen);
  const doc = useGraphStore((s) => s.doc);
  const operatorsById = useManifestStore((s) => s.operatorsById);
  const exec = useExecutionStore((s) => s.nodes);

  const [query, setQuery] = useState("");
  const [cursor, setCursor] = useState(0);
  const inputRef = useRef<HTMLInputElement>(null);
  const listRef = useRef<HTMLDivElement>(null);

  // 每次打开都从空查询开始，与算子搜索一样
  useEffect(() => {
    if (open) {
      setQuery("");
      setCursor(0);
      inputRef.current?.focus();
    }
  }, [open]);

  const entries = useMemo(() => (open ? listGraphNodes(doc, operatorsById) : []), [open, doc, operatorsById]);
  const hits = useMemo(() => searchGraphNodes(entries, query), [entries, query]);
  const rows = hits.slice(0, MAX_ROWS);

  useEffect(() => setCursor(0), [query]);

  useEffect(() => {
    const id = rows[Math.min(cursor, rows.length - 1)]?.entry.id;
    if (!id) return;
    listRef.current?.querySelector(`[data-id="${CSS.escape(id)}"]`)?.scrollIntoView({ block: "nearest" });
  }, [cursor, rows]);

  if (!open) return null;

  const close = () => setOpen(false);
  const pick = (i: number) => {
    const hit = rows[i];
    if (!hit) return;
    close();
    useUiStore.getState().revealNode(hit.entry.path, hit.entry.localId);
  };

  const onKeyDown = (e: React.KeyboardEvent) => {
    if (e.key === "ArrowDown") {
      e.preventDefault();
      setCursor((c) => Math.min(c + 1, rows.length - 1));
    } else if (e.key === "ArrowUp") {
      e.preventDefault();
      setCursor((c) => Math.max(c - 1, 0));
    } else if (e.key === "Enter") {
      e.preventDefault();
      pick(Math.min(cursor, rows.length - 1));
    } else if (e.key === "Escape" || ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === "f")) {
      // 再按一次 Ctrl+F 也是关掉（顺手挡住 WebView 自己的页内查找）
      e.preventDefault();
      close();
    }
    e.stopPropagation();
  };

  const left = Math.max(12, (window.innerWidth - POPUP_WIDTH) / 2);
  const active = Math.min(cursor, rows.length - 1);

  return (
    <>
      <div className="search-backdrop" onClick={close} />
      <div
        className="search-popup finder"
        data-testid="node-finder"
        style={{ left, top: 64, width: POPUP_WIDTH, maxHeight: POPUP_HEIGHT }}
        onKeyDown={onKeyDown}
      >
        <input
          ref={inputRef}
          className="search-popup__input"
          data-testid="node-finder-input"
          type="text"
          value={query}
          placeholder="查找节点：名字、id、算子、所在子图…"
          spellCheck={false}
          onChange={(e) => setQuery(e.target.value)}
        />
        <div className="search-popup__list" ref={listRef}>
          {rows.length === 0 ? (
            <p className="search-popup__empty">{entries.length === 0 ? "图里还没有节点" : "没有匹配的节点"}</p>
          ) : (
            rows.map((hit, i) => {
              const { entry } = hit;
              const failed = exec.get(entry.id)?.state === "error";
              return (
                <button
                  key={entry.id}
                  type="button"
                  data-id={entry.id}
                  data-testid="node-finder-row"
                  className={`search-popup__row${i === active ? " is-active" : ""}`}
                  onMouseEnter={() => setCursor(i)}
                  onClick={() => pick(i)}
                >
                  <span className="search-popup__label">{entry.title}</span>
                  {entry.parents.length > 0 && <span className="finder__where">在 {entry.parents.join(" › ")}</span>}
                  {failed && <span className="finder__err">出错</span>}
                  {hit.fieldIndex > 0 && <span className="search-popup__why">{NODE_FIELD_LABELS[hit.fieldIndex]}</span>}
                  {entry.opLabel !== entry.title && <span className="search-popup__cat">{entry.opLabel}</span>}
                </button>
              );
            })
          )}
          {hits.length > rows.length && (
            <p className="search-popup__empty">还有 {hits.length - rows.length} 个，继续输入缩小范围</p>
          )}
        </div>
      </div>
    </>
  );
}
