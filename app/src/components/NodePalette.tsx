//
// 节点面板 —— 交互清单 P0 第 9 项。
//
// 两种模式：
//   空查询   -> 按 category 的树形结构分组展示
//   有查询   -> 扁平的排序结果，命中字符高亮
//
// 分类树完全由 manifest 的 category 字段推导（用 / 分层），前端不硬编码
// 任何分类名。加一个新分类的算子，面板里自动长出新分支。
//

import { useEffect, useMemo, useRef, useState } from "react";

import { fuzzyMatchAny } from "../lib/fuzzy";
import { useManifestStore } from "../store/manifest";
import type { OperatorDesc } from "../types/manifest";

interface Props {
  selectedId: string | null;
  onSelect: (id: string) => void;
}

interface Hit {
  op: OperatorDesc;
  score: number;
  fieldIndex: number;
  indices: number[];
}

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

/** 搜索时把匹配到的字符包成 <mark>。 */
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
  onSelect,
}: {
  op: OperatorDesc;
  active: boolean;
  hit?: Hit;
  onSelect: (id: string) => void;
}) {
  // fieldIndex 对应 searchFields 的顺序：0=label 1=id 2=keywords 3=doc
  const highlightLabel = hit?.fieldIndex === 0 ? hit.indices : [];
  const matchedElsewhere = hit && hit.fieldIndex > 0;

  return (
    <button
      type="button"
      className={`op-row${active ? " is-active" : ""}`}
      data-op-id={op.id}
      onClick={() => onSelect(op.id)}
    >
      <span className="op-row__label">
        <Highlight text={op.label} indices={highlightLabel} />
      </span>
      <span className="op-row__id">{op.id}</span>
      {matchedElsewhere && (
        <span className="op-row__why">
          {hit.fieldIndex === 1 ? "id" : hit.fieldIndex === 2 ? "关键词" : "说明"}
        </span>
      )}
    </button>
  );
}

function TreeBranch({
  node,
  depth,
  selectedId,
  onSelect,
}: {
  node: TreeNode;
  depth: number;
  selectedId: string | null;
  onSelect: (id: string) => void;
}) {
  const [open, setOpen] = useState(true);
  const childBranches = [...node.children.values()];

  return (
    <div className="tree-branch" style={{ ["--depth" as string]: depth }}>
      <button type="button" className="tree-branch__head" onClick={() => setOpen((v) => !v)}>
        <span className={`tree-branch__caret${open ? " is-open" : ""}`} aria-hidden>
          ▸
        </span>
        {node.name}
        <span className="tree-branch__count">
          {node.operators.length + childBranches.length}
        </span>
      </button>
      {open && (
        <div className="tree-branch__body">
          {childBranches.map((child) => (
            <TreeBranch
              key={child.path}
              node={child}
              depth={depth + 1}
              selectedId={selectedId}
              onSelect={onSelect}
            />
          ))}
          {node.operators.map((op) => (
            <OperatorRow
              key={op.id}
              op={op}
              active={op.id === selectedId}
              onSelect={onSelect}
            />
          ))}
        </div>
      )}
    </div>
  );
}

export function NodePalette({ selectedId, onSelect }: Props) {
  const bundle = useManifestStore((s) => s.bundle);
  const [query, setQuery] = useState("");
  const [cursor, setCursor] = useState(0);
  const inputRef = useRef<HTMLInputElement>(null);
  const listRef = useRef<HTMLDivElement>(null);

  const operators = useMemo(() => bundle?.operators ?? [], [bundle]);

  const hits = useMemo<Hit[]>(() => {
    const q = query.trim();
    if (!q) return [];
    const result: Hit[] = [];
    for (const op of operators) {
      // 顺序即优先级：label 命中比 keyword 命中更值钱（fuzzyMatchAny 按下标降权）
      const match = fuzzyMatchAny(q, [
        op.label,
        op.id,
        (op.keywords ?? []).join(" "),
        op.doc ?? "",
      ]);
      if (match) result.push({ op, ...match });
    }
    result.sort((a, b) => b.score - a.score);
    return result;
  }, [query, operators]);

  const tree = useMemo(() => buildTree(operators), [operators]);

  useEffect(() => setCursor(0), [query]);

  // 键盘选中的项滚进视野。搜索面板不支持键盘会立刻显得很笨重。
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
      if (hit) onSelect(hit.op.id);
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
          ref={inputRef}
          type="search"
          value={query}
          placeholder="搜索算子…  ↑↓ 选择  Enter 确认"
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
                onSelect={onSelect}
              />
            ))
          )
        ) : (
          [...tree.children.values()].map((child) => (
            <TreeBranch
              key={child.path}
              node={child}
              depth={0}
              selectedId={selectedId}
              onSelect={onSelect}
            />
          ))
        )}
      </div>
    </div>
  );
}
