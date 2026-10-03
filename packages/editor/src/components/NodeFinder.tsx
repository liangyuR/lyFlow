// 查找节点（Ctrl+F）。大图里找一个节点原来只能缩放着看、或者一层层进子图翻；
// 这里把整张图连子图里面的节点一起列出来模糊找，回车打开到它所在的那一层、选中并移进视野。
// 键盘全程可用（输入过滤、↑↓、Enter、Esc），与算子搜索弹层同一套手感。Alt+Enter 把这一层里所有命中的一起选上
// （接着在检查器里一起改、一起静音、合成子图）；查询里能夹 is:muted / is:error / op:… 筛选。

import { useEffect, useMemo, useRef, useState } from "react";

import { docNodeKey, listGraphNodes, NODE_FIELD_LABELS, searchGraphNodes } from "../lib/findNodes";
import { pathPrefix } from "../lib/subgraph";
import { worstTone } from "../lib/outputs";
import type { OutputStat } from "../types/execution";
import { useExecutionStore, useJudgedNodes } from "../store/execution";
import { useGraphStore } from "../store/graph";
import { useManifestStore } from "../store/manifest";
import { useUiStore } from "../store/ui";

const MAX_ROWS = 50;
const POPUP_WIDTH = 460;
const POPUP_HEIGHT = 400;

/** 这次的判定（只认这一次跑完的：出错 / 运行中的节点表里留着的是上一次的）。 */
function toneOf(n: { state: string; stats?: { outputs?: OutputStat[] } | undefined } | undefined) {
  return n && (n.state === "done" || n.state === "skipped") ? worstTone(n.stats?.outputs) : null;
}

export function NodeFinder() {
  const open = useUiStore((s) => s.finderOpen);
  const seed = useUiStore((s) => s.finderSeed);
  const setOpen = useUiStore((s) => s.setFinderOpen);
  const doc = useGraphStore((s) => s.doc);
  const operatorsById = useManifestStore((s) => s.operatorsById);
  const exec = useExecutionStore((s) => s.nodes);
  // NG / 边界看的是与工具栏计数同一份节点表（拖参数的预览不算）
  const judged = useJudgedNodes();

  const [query, setQuery] = useState("");
  const [cursor, setCursor] = useState(0);
  const inputRef = useRef<HTMLInputElement>(null);
  const listRef = useRef<HTMLDivElement>(null);

  // 每次打开都从空查询开始，与算子搜索一样
  useEffect(() => {
    if (open) {
      setQuery(seed);
      setCursor(0);
      inputRef.current?.focus();
    }
  }, [open, seed]);

  const entries = useMemo(() => (open ? listGraphNodes(doc, operatorsById) : []), [open, doc, operatorsById]);
  const hits = useMemo(
    () =>
      searchGraphNodes(
        entries,
        query,
        (id) => exec.get(id)?.state === "error",
        (id) => toneOf(judged.get(id)),
      ),
    [entries, query, exec, judged],
  );
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
  // Alt+Enter：这一层里命中的全部选上（别的层的选不上 —— 选中只在一层里，说一声有几个）
  const pickAll = () => {
    const ui = useUiStore.getState();
    const here = pathPrefix(ui.path);
    const hereHits = hits.filter((h) => pathPrefix(h.entry.path) === here);
    const ids = hereHits.map((h) => h.entry.localId);
    // 同一个子图定义的别的实例里列出来的是同一个节点：选上了就不算「别的层没选」的
    const picked = new Set(hereHits.map((h) => docNodeKey(h.entry)));
    const elsewhere = hits.filter((h) => pathPrefix(h.entry.path) !== here && !picked.has(docNodeKey(h.entry))).length;
    if (ids.length === 0) {
      ui.showToast(hits.length === 0 ? "没有匹配的节点" : `这一层没有匹配的；${elsewhere} 个在别的层，回车逐个打开`, "warn");
      return;
    }
    close();
    // 选上是为了接着编辑：预览最大化着（画布、检查器都看不见）就先还原，与回车打开到节点（revealNode）一样
    ui.setViewerMaximized(false);
    ui.setSelection(ids, []);
    ui.showToast(`选中了这一层的 ${ids.length} 个节点${elsewhere > 0 ? `（另有 ${elsewhere} 个在别的层，没选）` : ""}`);
  };

  const onKeyDown = (e: React.KeyboardEvent) => {
    if (e.key === "ArrowDown") {
      e.preventDefault();
      setCursor((c) => Math.min(c + 1, rows.length - 1));
    } else if (e.key === "ArrowUp") {
      e.preventDefault();
      setCursor((c) => Math.max(c - 1, 0));
    } else if (e.key === "Enter" && e.altKey) {
      e.preventDefault();
      pickAll();
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
          placeholder="查找节点（is:muted / is:ng / op:… 筛选，Alt+Enter 选这一层的）"
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
                  {entry.muted && <span className="finder__muted">静音</span>}
                  {(() => {
                    const tone = toneOf(judged.get(entry.id));
                    return tone === "ng" || tone === "margin" ? (
                      <span className={`finder__tone finder__tone--${tone}`} data-tone={tone}>
                        {tone === "ng" ? "NG" : "边界"}
                      </span>
                    ) : null;
                  })()}
                  {hit.fieldIndex > 0 && <span className="search-popup__why">{NODE_FIELD_LABELS[hit.fieldIndex]}</span>}
                  {entry.opLabel !== entry.title && <span className="search-popup__cat">{entry.opLabel}</span>}
                </button>
              );
            })
          )}
          {hits.length > rows.length && (
            <p className="search-popup__empty">还有 {hits.length - rows.length} 个，继续输入缩小范围（Alt+Enter 选上这一层命中的全部）</p>
          )}
        </div>
      </div>
    </>
  );
}
