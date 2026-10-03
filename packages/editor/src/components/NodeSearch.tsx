// 画布上的算子搜索弹层（交互清单 P0 #9）。双击空白处唤起，在光标处落节点。
// 键盘全程可用（输入过滤、↑↓、Enter、Esc）—— 鼠标点选是退路不是主路。
// 从端口拖线松在空白处唤起时（#18）：接得上拖出那一头的算子排前面、行尾写接到它的哪个端口，接不上的置灰排在后面。
// 要插到一条连线中间时（连线右键「插入算子…」、只选中一条连线按 Tab）同理：插得进的排前面、写着进出用哪两个端口。

import { useEffect, useMemo, useRef, useState } from "react";

import { addNodeWithAutoConnect, insertIntoEdge, replaceOperator } from "../lib/insert";
import { planReplace } from "../lib/replace";
import { searchOperators, FIELD_LABELS } from "../lib/search";
import { augmentOperators, levelOf } from "../lib/subgraph";
import { findPort, inferAnyTypes, insertPortsFor, pendingPort, pendingType } from "../lib/typecheck";
import { estimateNodeHeight } from "../lib/placement";
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

/** 搜索里怎么引导：哪些算子合适（值是行尾的标注，null = 不合适、置灰排后面）、分隔行写什么、占位字。 */
interface Guide {
  fit: ReadonlyMap<string, string | null>;
  /** 不合适的那几行行尾写什么（换算子时「断 2 条线」）。 */
  miss?: ReadonlyMap<string, string>;
  sep: (n: number) => string;
  placeholder: string;
}

/** 探针节点的 id：判「这个算子插不插得进那条线」时临时放进图里，不进 store。 */
const PROBE_ID = "__lyflow_probe__";

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

  // 插到一条线中间：每个算子放一个探针节点进去，看有没有唯一的一对端口两头都接得上（与拖节点到线上同一个判法）
  const insert = useMemo(() => {
    const edgeId = popup?.insertEdge;
    if (!edgeId) return null;
    const lvl = levelOf(doc, path);
    const view = { ...doc, nodes: lvl.nodes, edges: lvl.edges };
    const edge = view.edges.find((e) => e.id === edgeId);
    if (!edge) return null;
    const ctx = { operatorsById: augmentOperators(baseOps, doc.subgraphs), typesByName };
    const types = inferAnyTypes(ctx, { ...view, edges: view.edges.filter((e) => e.id !== edgeId) });
    const fit = new Map<string, string | null>();
    for (const op of operators ?? []) {
      const probe = { ...view, nodes: [...view.nodes, { id: PROBE_ID, op: op.id, params: {} }] };
      const ports = insertPortsFor(ctx, probe, edge, PROBE_ID, types);
      fit.set(op.id, ports ? `${portLabel(op, ports.inPort, "input")} → ${portLabel(op, ports.outPort, "output")}` : null);
    }
    const name = (id: string) => {
      const n = view.nodes.find((x) => x.id === id);
      return n?.ui?.title ?? (n ? ctx.operatorsById.get(n.op)?.label : undefined) ?? id;
    };
    return { fit, between: `${name(edge.from.node)} → ${name(edge.to.node)}` };
  }, [popup, doc, path, baseOps, typesByName, operators]);

  // 换算子：每个算子算一遍换上去会断几条线。连线全留着的排前面；会断线的置灰排后面、照样能选（换完说清楚丢了什么）；
  // 子图出口会断的选了也不换
  const replace = useMemo(() => {
    const nodeId = popup?.replaceNode;
    if (!nodeId) return null;
    const lvl = levelOf(doc, path);
    const node = lvl.nodes.find((n) => n.id === nodeId);
    if (!node) return null;
    const ctx = { operatorsById: augmentOperators(baseOps, doc.subgraphs), typesByName };
    const fit = new Map<string, string | null>();
    const miss = new Map<string, string>();
    for (const op of operators ?? []) {
      if (op.id === node.op) continue;
      const plan = planReplace(ctx, doc, path, nodeId, op);
      // 子图入口进来的那几条也是线（在子图里它们不是边）
      const cut = plan ? plan.droppedEdges.length + plan.droppedInputs.length : 0;
      if (plan && !plan.blocked && cut === 0) {
        fit.set(op.id, "连线全留着");
        continue;
      }
      fit.set(op.id, null);
      miss.set(op.id, !plan ? "换不了" : plan.blocked ? "子图输出会断，换不了" : `断 ${cut} 条线`);
    }
    const name = node.ui?.title ?? ctx.operatorsById.get(node.op)?.label ?? node.id;
    return { fit, miss, self: node.op, name };
  }, [popup, doc, path, baseOps, typesByName, operators]);

  const guide = useMemo<Guide | null>(() => {
    if (replace) {
      return {
        fit: replace.fit,
        miss: replace.miss,
        sep: (n) => `${n} 个算子换上去会断线`,
        placeholder: `把「${replace.name}」换成…`,
      };
    }
    if (pending) {
      const arrow = pending.side === "input" ? "← " : "→ ";
      const fit = new Map<string, string | null>();
      for (const op of operators ?? []) {
        const port = pending.portOf.get(op.id) ?? null;
        fit.set(op.id, port ? arrow + portLabel(op, port, pending.side === "input" ? "output" : "input") : null);
      }
      return { fit, sep: (n) => `${n} 个算子接不上 ${pending.type ?? "这一头"}`, placeholder: `接到 ${pending.from} 的算子…` };
    }
    if (insert) {
      return { fit: insert.fit, sep: (n) => `${n} 个算子插不进这条线`, placeholder: `插到 ${insert.between} 之间的算子…` };
    }
    return null;
  }, [replace, pending, insert, operators]);

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

  const { rows, incompatible, total } = useMemo(() => {
    // 换算子时不列它自己
    const all = (operators ?? []).filter((op) => op.id !== replace?.self);
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
    if (!guide) return { rows: hits.slice(0, MAX_ROWS), incompatible: 0, total: hits.length };
    // 合适的排前面（各自保持原来的先后），不合适的置灰排在后面、不藏起来：也许就是想放一个不接线的。
    // 先分再截：不然算子一多，合适的那几个可能被截在 MAX_ROWS 外面
    const ok = hits.filter((h) => guide.fit.get(h.op.id) != null);
    const no = hits.filter((h) => guide.fit.get(h.op.id) == null);
    return { rows: [...ok, ...no].slice(0, MAX_ROWS), incompatible: no.length, total: hits.length };
  }, [operators, query, recentOps, guide, replace]);

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
    // 换算子：换不了（子图出口会断）弹层留着，换一个
    if (popup.replaceNode) {
      if (replaceOperator(opId, popup.replaceNode)) closeSearch();
      return;
    }
    // 插到一条线中间：加节点与插入一条撤销；插不进就照旧放下（按类型自动连线）、说一声
    if (popup.insertEdge) {
      if (!insertIntoEdge(opId, popup.insertEdge)) {
        addNodeWithAutoConnect(opId, popup.flow);
        const op = useManifestStore.getState().operatorsById.get(opId);
        if (op) useUiStore.getState().showToast(`${op.label} 插不进这条线：放在了一边`, "warn");
      }
      closeSearch();
      return;
    }
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
      // 从选中的节点接出：这时才知道新节点多高（端口多的比选中的那个高），按它重新找一次不压着别人的位置
      const at = popup.place && op ? (popup.place({ h: estimateNodeHeight(op.inputs.length, op.outputs.length) }) ?? popup.flow) : popup.flow;
      const id = graph.addNode(opId, at);
      // 拖的是输入端时新节点在上游，端口方向要反过来（P1 #18/#19）
      if (id && port) {
        if (popup.pendingSide === "input") graph.connect({ node: id, port }, pendingFrom);
        else graph.connect(pendingFrom, { node: id, port });
      }
      return id;
    });
    if (nodeId) {
      if (popup.follow) useUiStore.getState().followNode(nodeId);
      else useUiStore.getState().setSelection([nodeId], []);
      useUiStore.getState().noteOperatorUsed(opId);
      // 选的是置灰的那种：节点照样放下，说一声没接线
      if (!port && op) {
        useUiStore.getState().showToast(`${op.label} 没有能接 ${pending?.type ?? "这一头"} 的端口：放下了，没接线`, "warn");
      }
    }
    closeSearch();
  };

  // 当前那一行的算子说明与进出端口：以前只有名字和分类，选之前不知道它干什么、接什么（面板里单击才看得到说明）
  const activeOp = rows[Math.min(cursor, rows.length - 1)]?.op;

  // 贴着光标放，但不能溢出窗口
  // 两头都夹：选中一个节点按 Tab 时弹层摆在新节点要落的地方，那个节点在视野外的话坐标是负的
  const left = Math.max(12, Math.min(popup.screen.x, window.innerWidth - POPUP_WIDTH - 12));
  const top = Math.max(12, Math.min(popup.screen.y, window.innerHeight - POPUP_HEIGHT - 12));

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
          placeholder={guide ? guide.placeholder : "搜索算子…"}
          spellCheck={false}
          onChange={(e) => setQuery(e.target.value)}
        />
        <div className="search-popup__list" ref={listRef}>
          {rows.length === 0 ? (
            <p className="search-popup__empty">没有匹配的算子</p>
          ) : (
            rows.map((hit, i) => {
              const port = pending ? (pending.portOf.get(hit.op.id) ?? null) : null;
              const fitNote = guide ? (guide.fit.get(hit.op.id) ?? null) : null;
              const note = fitNote ?? guide?.miss?.get(hit.op.id) ?? null;
              const greyed = guide !== null && fitNote === null;
              // 合适与不合适之间一条分隔：写明后面这些为什么不合适
              const prev = i > 0 ? rows[i - 1] : undefined;
              const firstGreyed = greyed && (!prev || guide.fit.get(prev.op.id) != null);
              return (
                <div key={hit.op.id} className="search-popup__item">
                  {firstGreyed && (
                    <p className="search-popup__sep" data-testid="search-incompatible-sep">
                      {guide.sep(incompatible)}
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
                    {note && <span className="search-popup__port">{note}</span>}
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
        {/* 不放在滚动的列表里：键盘走到第 40 行时它还在下面看不见 */}
        {total > rows.length && (
          <p className="search-popup__more" data-testid="search-more" onMouseDown={(e) => e.preventDefault()}>
            还有 {total - rows.length} 个，继续输入缩小范围
          </p>
        )}
        {activeOp && (
          // 点这一块不把焦点从输入框拿走：不然 ↑↓、回车、打字都没反应了
          <div
            className="search-popup__detail"
            data-testid="search-detail"
            data-op-id={activeOp.id}
            onMouseDown={(e) => e.preventDefault()}
          >
            {activeOp.doc && <p className="search-popup__doc">{activeOp.doc}</p>}
            <p className="search-popup__io">
              <span>输入 {portsText(activeOp.inputs)}</span>
              <span className="search-popup__arrow">→</span>
              <span>输出 {portsText(activeOp.outputs)}</span>
            </p>
          </div>
        )}
      </div>
    </>
  );
}

/** 「cloud（PointCloud）、pose（Transform，可选）」；没有写「无」。可选只出现在输入口上。 */
function portsText(
  ports: readonly { name: string; type: string; label?: string | undefined; required?: boolean | undefined }[],
): string {
  return ports.length === 0
    ? "无"
    : ports.map((p) => `${p.label || p.name}（${p.type}${p.required === false ? "，可选" : ""}）`).join("、");
}
