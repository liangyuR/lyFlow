// 画布上的算子搜索弹层（交互清单 P0 #9）。双击空白处唤起，在光标处落节点。
// 键盘全程可用（输入过滤、↑↓、Enter、Esc）—— 鼠标点选是退路不是主路。
// 从端口拖线松在空白处唤起时（#18）：接得上拖出那一头的算子排前面、行尾写接到它的哪个端口，接不上的置灰排在后面。

import { useEffect, useMemo, useRef, useState } from "react";

import { addNodeWithAutoConnect } from "../lib/insert";
import { searchOperators, FIELD_LABELS } from "../lib/search";
import { augmentOperators, levelOf } from "../lib/subgraph";
import { findPort, inferAnyTypes, pendingPort, pendingType } from "../lib/typecheck";
import { useGraphStore } from "../store/graph";
import { useManifestStore } from "../store/manifest";
import { useUiStore } from "../store/ui";
import type { OperatorDesc } from "../types/manifest";

/** 拖线唤起时，每个算子接拖出那一头用哪个端口（null = 接不上），以及占位字里写的「接到谁」。 */
interface PendingInfo {
  side: "input" | "output";
  portOf: ReadonlyMap<string, string | null>;
  /** 拖出那一头的实际类型 */
  type: string | null;
  /** 「RANSAC 平面.Inliers」 */
  from: string;
}

function portLabel(op: OperatorDesc, name: string, side: "input" | "output"): string {
  return findPort(op, name, side)?.label || name;
}

const MAX_ROWS = 40;
const POPUP_WIDTH = 340;
const POPUP_HEIGHT = 360;

export function NodeSearch() {
  const popup = useUiStore((s) => s.searchPopup);
  const closeSearch = useUiStore((s) => s.closeSearch);
  const operators = useManifestStore((s) => s.bundle?.operators);
  const recentOps = useUiStore((s) => s.recentOps);
  const baseOps = useManifestStore((s) => s.operatorsById);
  const typesByName = useManifestStore((s) => s.typesByName);
  const doc = useGraphStore((s) => s.doc);
  const path = useUiStore((s) => s.path);

  // 拖线唤起的：先把每个算子接哪个端口算出来（算子几十个、端口几个，弹层开着时算一次）
  const pending = useMemo<PendingInfo | null>(() => {
    const from = popup?.pendingFrom;
    if (!from) return null;
    const side = popup.pendingSide === "input" ? "input" : "output";
    const lvl = levelOf(doc, path);
    const view = { ...doc, nodes: lvl.nodes, edges: lvl.edges };
    const ctx = { operatorsById: augmentOperators(baseOps, doc.subgraphs), typesByName };
    const types = inferAnyTypes(ctx, view);
    const portOf = new Map<string, string | null>();
    for (const op of operators ?? []) portOf.set(op.id, pendingPort(ctx, view, from, side, op, types));
    const node = view.nodes.find((n) => n.id === from.node);
    const nodeOp = node ? ctx.operatorsById.get(node.op) : undefined;
    const who = node?.ui?.title ?? nodeOp?.label ?? from.node;
    const port = nodeOp ? portLabel(nodeOp, from.port, side) : from.port;
    return { side, portOf, type: pendingType(ctx, view, from, side), from: `${who}.${port}` };
  }, [popup, doc, path, baseOps, typesByName, operators]);

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

  const { rows, incompatible } = useMemo(() => {
    const all = operators ?? [];
    let hits: { op: OperatorDesc; fieldIndex: number; indices: readonly number[]; recent: boolean }[];
    if (!query.trim()) {
      // 空查询列全部，让人知道有哪些可用，而不是面对一个空白框；最近用过的排在最前
      const recent = new Set(recentOps);
      const first = recentOps.flatMap((id) => all.filter((op) => op.id === id));
      const rest = all.filter((op) => !recent.has(op.id));
      hits = [...first, ...rest].map((op) => ({ op, fieldIndex: 0, indices: [] as number[], recent: recent.has(op.id) }));
    } else {
      hits = searchOperators(all, query).map((hit) => ({ ...hit, recent: false }));
    }
    if (!pending) return { rows: hits.slice(0, MAX_ROWS), incompatible: 0 };
    // 接得上的排前面（各自保持原来的先后），接不上的置灰排在后面、不藏起来：也许就是想放一个不接线的。
    // 先分再截：不然算子一多，接得上的那几个可能被截在 MAX_ROWS 外面
    const ok = hits.filter((h) => pending.portOf.get(h.op.id) != null);
    const no = hits.filter((h) => pending.portOf.get(h.op.id) == null);
    return { rows: [...ok, ...no].slice(0, MAX_ROWS), incompatible: no.length };
  }, [operators, query, recentOps, pending]);

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
    // 双击空白处唤起的：与拖入同一条路，按类型自动连线（m8-plan L13）
    if (!popup.pendingFrom) {
      addNodeWithAutoConnect(opId, popup.flow);
      closeSearch();
      return;
    }
    const graph = useGraphStore.getState();
    const pendingFrom = popup.pendingFrom;
    const op = useManifestStore.getState().operatorsById.get(opId);
    // 接到新算子的哪个端口：按类型挑（pendingPort），不是固定取第一个
    const port = pending?.portOf.get(opId) ?? null;
    // 加节点与接线一条撤销（以前两条：Ctrl+Z 一次，线没了、节点还在）
    const nodeId = graph.batch(op ? `添加 ${op.label}` : "添加节点", () => {
      const id = graph.addNode(opId, popup.flow);
      // 拖的是输入端时新节点在上游，端口方向要反过来（P1 #18/#19）
      if (id && port) {
        if (popup.pendingSide === "input") graph.connect({ node: id, port }, pendingFrom);
        else graph.connect(pendingFrom, { node: id, port });
      }
      return id;
    });
    if (nodeId) {
      useUiStore.getState().setSelection([nodeId], []);
      useUiStore.getState().noteOperatorUsed(opId);
      // 选的是置灰的那种：节点照样放下，说一声没接线
      if (!port && op) {
        useUiStore.getState().showToast(`${op.label} 没有能接 ${pending?.type ?? "这一头"} 的端口：放下了，没接线`, "warn");
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
          placeholder={pending ? `接到 ${pending.from} 的算子…` : "搜索算子…"}
          spellCheck={false}
          onChange={(e) => setQuery(e.target.value)}
        />
        <div className="search-popup__list" ref={listRef}>
          {rows.length === 0 ? (
            <p className="search-popup__empty">没有匹配的算子</p>
          ) : (
            rows.map((hit, i) => {
              const port = pending ? (pending.portOf.get(hit.op.id) ?? null) : null;
              const greyed = pending !== null && port === null;
              // 接得上与接不上之间一条分隔：写明后面这些接不上什么
              const prev = i > 0 ? rows[i - 1] : undefined;
              const firstGreyed = greyed && (!prev || pending.portOf.get(prev.op.id) != null);
              return (
                <div key={hit.op.id} className="search-popup__item">
                  {firstGreyed && (
                    <p className="search-popup__sep" data-testid="search-incompatible-sep">
                      {incompatible} 个算子接不上 {pending.type ?? "这一头"}
                    </p>
                  )}
                  <button
                    type="button"
                    data-op-id={hit.op.id}
                    data-port={port ?? undefined}
                    data-incompatible={greyed ? "1" : undefined}
                    className={`search-popup__row${i === Math.min(cursor, rows.length - 1) ? " is-active" : ""}${
                      greyed ? " is-incompatible" : ""
                    }`}
                    onMouseEnter={() => setCursor(i)}
                    onClick={() => pick(hit.op.id)}
                  >
                    <span className="search-popup__label">{hit.op.label}</span>
                    {port && pending && (
                      <span className="search-popup__port">
                        {pending.side === "input" ? "← " : "→ "}
                        {portLabel(hit.op, port, pending.side === "input" ? "output" : "input")}
                      </span>
                    )}
                    <span className="search-popup__cat">{hit.op.category}</span>
                    {hit.recent && <span className="search-popup__why">最近</span>}
                    {hit.fieldIndex > 0 && (
                      <span className="search-popup__why">{FIELD_LABELS[hit.fieldIndex]}</span>
                    )}
                  </button>
                </div>
              );
            })
          )}
        </div>
      </div>
    </>
  );
}
