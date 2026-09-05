//
// 左侧算子面板。
//
// 分类树完全由 manifest 的 category 字段推导（用 / 分层），前端不硬编码任何
// 分类名。加一个新分类的算子，面板里自动长出新分支 —— 这是 M0 验收过的。
//
// 三种加节点的方式，因为三种人都会试：拖到画布上、双击、以及画布上双击搜索。
//

import { useReactFlow } from "@xyflow/react";
import { useEffect, useMemo, useRef, useState } from "react";

import { FIELD_LABELS, searchOperators, type OperatorHit } from "../lib/search";
import { useGraphStore } from "../store/graph";
import { useManifestStore } from "../store/manifest";
import { useUiStore } from "../store/ui";
import type { OperatorDesc } from "../types/manifest";

/** 拖到画布时用的 MIME。GraphCanvas 的 onDrop 读它。 */
export const OPERATOR_DND_MIME = "application/lyflow-operator";

interface TreeNode {
  name: string;
  path: string;
  children: Map<string, TreeNode>;
  operators: OperatorDesc[];
}

function emptyNode(name: string, path: string): TreeNode {
  return { name, path, children: new Map(), operators: [] };
}

function buildTree(operators: readonly OperatorDesc[]): TreeNode {
  const root = emptyNode("", "");
  for (const op of operators) {
    const segments = op.category.split("/").filter(Boolean);
    let cursor = root;
    let path = "";
    for (const segment of segments) {
      path = path ? `${path}/${segment}` : segment;
      let child = cursor.children.get(segment);
      if (!child) {
        child = emptyNode(segment, path);
        cursor.children.set(segment, child);
      }
      cursor = child;
    }
    cursor.operators.push(op);
  }
  return root;
}

function Highlight({ text, indices }: { text: string; indices: readonly number[] }) {
  if (indices.length === 0) return <>{text}</>;
  const marked = new Set(indices);
  const parts: React.ReactNode[] = [];
  let buffer = "";
  let bufferMarked = false;

  const flush = (key: number) => {
    if (!buffer) return;
    parts.push(bufferMarked ? <mark key={key}>{buffer}</mark> : <span key={key}>{buffer}</span>);
    buffer = "";
  };

  for (let i = 0; i < text.length; i++) {
    const isMarked = marked.has(i);
    if (isMarked !== bufferMarked) {
      flush(i);
      bufferMarked = isMarked;
    }
    buffer += text[i];
  }
  flush(text.length);
  return <>{parts}</>;
}

function OperatorRow({
  op,
  active,
  hit,
}: {
  op: OperatorDesc;
  active: boolean;
  hit?: OperatorHit;
}) {
  const setInspected = useUiStore((s) => s.setInspectedOperator);
  const { screenToFlowPosition } = useReactFlow();
  const highlightLabel = hit?.fieldIndex === 0 ? hit.indices : [];

  /** 画布可视区中心的画布坐标。双击添加时落在这里，比写死原点合理。 */
  const viewportCenter = () => {
    const pane = document.querySelector(".react-flow__pane");
    const rect = pane?.getBoundingClientRect();
    if (!rect) return { x: 0, y: 0 };
    return screenToFlowPosition({ x: rect.left + rect.width / 2, y: rect.top + rect.height / 2 });
  };

  return (
    <button
      type="button"
      className={`op-row${active ? " is-active" : ""}`}
      data-op-id={op.id}
      draggable
      onDragStart={(e) => {
        e.dataTransfer.setData(OPERATOR_DND_MIME, op.id);
        e.dataTransfer.effectAllowed = "copy";
      }}
      onClick={() => setInspected(op.id)}
      onDoubleClick={() => {
        // 拖拽是精确落点的方式，双击是图省事的方式，都得有。
        const id = useGraphStore.getState().addNode(op.id, viewportCenter());
        if (id) useUiStore.getState().setSelection([id], []);
      }}
      title={op.doc}
    >
      <span className="op-row__label">
        <Highlight text={op.label} indices={highlightLabel} />
      </span>
      <span className="op-row__id">{op.id}</span>
      {hit && hit.fieldIndex > 0 && (
        <span className="op-row__why">{FIELD_LABELS[hit.fieldIndex]}</span>
      )}
    </button>
  );
}

function TreeBranch({ node, depth }: { node: TreeNode; depth: number }) {
  const [open, setOpen] = useState(true);
  const inspected = useUiStore((s) => s.inspectedOperator);
  const childBranches = [...node.children.values()];

  return (
    <div className="tree-branch" style={{ ["--depth" as string]: depth }}>
      <button type="button" className="tree-branch__head" onClick={() => setOpen((v) => !v)}>
        <span className={`tree-branch__caret${open ? " is-open" : ""}`} aria-hidden>
          ▸
        </span>
        {node.name}
        <span className="tree-branch__count">{node.operators.length + childBranches.length}</span>
      </button>
      {open && (
        <div className="tree-branch__body">
          {childBranches.map((child) => (
            <TreeBranch key={child.path} node={child} depth={depth + 1} />
          ))}
          {node.operators.map((op) => (
            <OperatorRow key={op.id} op={op} active={op.id === inspected} />
          ))}
        </div>
      )}
    </div>
  );
}

export function NodePalette() {
  const operators = useManifestStore((s) => s.bundle?.operators);
  const [query, setQuery] = useState("");
  const [cursor, setCursor] = useState(0);
  const listRef = useRef<HTMLDivElement>(null);

  const all = useMemo(() => operators ?? [], [operators]);
  const hits = useMemo(() => searchOperators(all, query), [all, query]);
  const tree = useMemo(() => buildTree(all), [all]);

  useEffect(() => setCursor(0), [query]);

  useEffect(() => {
    if (!query.trim() || hits.length === 0) return;
    const id = hits[Math.min(cursor, hits.length - 1)]?.op.id;
    if (!id) return;
    listRef.current
      ?.querySelector(`[data-op-id="${CSS.escape(id)}"]`)
      ?.scrollIntoView({ block: "nearest" });
  }, [cursor, hits, query]);

  const onKeyDown = (e: React.KeyboardEvent) => {
    if (!query.trim() || hits.length === 0) return;
    if (e.key === "ArrowDown") {
      e.preventDefault();
      setCursor((c) => Math.min(c + 1, hits.length - 1));
    } else if (e.key === "ArrowUp") {
      e.preventDefault();
      setCursor((c) => Math.max(c - 1, 0));
    } else if (e.key === "Enter") {
      e.preventDefault();
      const hit = hits[Math.min(cursor, hits.length - 1)];
      if (hit) useUiStore.getState().setInspectedOperator(hit.op.id);
    } else if (e.key === "Escape") {
      e.preventDefault();
      setQuery("");
    }
  };

  const searching = query.trim().length > 0;

  return (
    <div className="palette">
      <div className="palette__search">
        <input
          type="search"
          value={query}
          placeholder="搜索算子…"
          onChange={(e) => setQuery(e.target.value)}
          onKeyDown={onKeyDown}
          spellCheck={false}
        />
      </div>

      <div className="palette__list" ref={listRef}>
        {searching ? (
          hits.length === 0 ? (
            <p className="palette__empty">没有匹配的算子</p>
          ) : (
            hits.map((hit, i) => (
              <OperatorRow
                key={hit.op.id}
                op={hit.op}
                hit={hit}
                active={i === Math.min(cursor, hits.length - 1)}
              />
            ))
          )
        ) : (
          [...tree.children.values()].map((child) => (
            <TreeBranch key={child.path} node={child} depth={0} />
          ))
        )}
      </div>

      <p className="palette__tip">拖到画布，或双击添加；单击查看说明。</p>
    </div>
  );
}
