// 参数面板（param-recipe P2）：画布右侧与画布并排的大面板，一张图的全部参数在这里看得全、改得动。
// 开着时替代 Inspector（D4：Inspector 的功能是它的子集）。三个页签：按节点（P2）、配方矩阵与配方管理（P3 填）。
// 行的全集、搜索、过滤与折叠的判据都在 lib/paramPanel.ts；这里只管画、事件与定位。
// 控件与 Inspector 是同一份（ParamControls 的映射表），行右键菜单也是同一个。

import { useReactFlow } from "@xyflow/react";
import { memo, useCallback, useEffect, useMemo, useRef, useState, useSyncExternalStore } from "react";

import { graphParamNameProblem } from "../lib/graphParams";
import { useMotionEnabled, viewportMs } from "../lib/motion";
import {
  buildPanelModel,
  flattenPanel,
  sectionKey,
  type GraphParamRow,
  type NodeParamRow,
  type NodeSection,
  type PanelChip,
  type PanelDiagnostics,
  type PanelItem,
  type RowDiag,
} from "../lib/paramPanel";
import { valueEquals } from "../lib/params";
import { roiBoundsVersion, roiThumb, subscribeRoiBounds } from "../lib/roiThumbs";
import { augmentOperators } from "../lib/subgraph";
import { useExecutionStore } from "../store/execution";
import { useGraphStore } from "../store/graph";
import { useManifestStore } from "../store/manifest";
import { useGraphParamOverrides } from "../store/recipe";
import { useUiStore, type ParamPanelTab } from "../store/ui";
import { useValidationStore } from "../store/validation";
import type { ParamSpec } from "../types/graph";
import type { ParamType } from "../types/manifest";

import { ParamControl } from "./ParamControls";
import { VirtualList, type VirtualListHandle } from "./VirtualList";

import "../styles.panel.css";

const TABS: { id: ParamPanelTab; label: string }[] = [
  { id: "nodes", label: "按节点" },
  { id: "matrix", label: "配方矩阵" },
  { id: "recipes", label: "配方管理" },
];

const CHIPS: { id: PanelChip; label: string; title: string }[] = [
  { id: "all", label: "全部", title: "所有参数（visibleWhen 不满足的不算）" },
  { id: "modified", label: "已改动", title: "与算子默认值不同的（行首有蓝点）" },
  { id: "recipe", label: "配方", title: "已纳入配方：图参数本身，以及值由图参数提供的那些" },
  { id: "diag", label: "诊断", title: "带诊断的（编辑期校验或上次运行的错误）" },
];

const TYPES: ParamType[] = [
  "bool", "int", "float", "vec2f", "vec3f", "vec4f", "enum", "flags",
  "string", "text", "path", "color", "transform", "curve",
];

export function ParamPanel() {
  const panel = useUiStore((s) => s.paramPanel);
  const ui = useUiStore.getState();
  return (
    <section
      className={`ppanel${panel.maximized ? " is-maximized" : ""}`}
      data-testid="param-panel"
      data-tab={panel.tab}
      data-maximized={panel.maximized ? "1" : "0"}
    >
      <header className="ppanel__head">
        <div className="ppanel__tabs" role="tablist">
          {TABS.map((t) => (
            <button
              key={t.id}
              type="button"
              role="tab"
              aria-selected={panel.tab === t.id}
              className={`ppanel__tab${panel.tab === t.id ? " is-active" : ""}`}
              data-testid={`pp-tab-${t.id}`}
              onClick={() => ui.setParamPanelTab(t.id)}
            >
              {t.label}
            </button>
          ))}
        </div>
        <span className="ppanel__spacer" />
        <button
          type="button"
          className="ppanel__icon"
          data-testid="pp-maximize"
          aria-pressed={panel.maximized}
          title={panel.maximized ? "还原：画布回来" : "最大化：画布收起"}
          onClick={() => ui.setParamPanelMaximized(!panel.maximized)}
        >
          {panel.maximized ? "❐" : "□"}
        </button>
        <button
          type="button"
          className="ppanel__icon"
          data-testid="pp-close"
          title="关闭参数面板（Ctrl+Shift+P）"
          onClick={() => ui.toggleParamPanel(false)}
        >
          ✕
        </button>
      </header>
      {panel.tab === "nodes" ? (
        <NodesTab />
      ) : (
        <div className="ppanel__placeholder" data-testid={`pp-placeholder-${panel.tab}`}>
          <p>{panel.tab === "matrix" ? "配方矩阵" : "配方管理"}在 P3 提供。</p>
          <p className="insp__hint">
            {panel.tab === "matrix"
              ? "行是图参数、列是「基础」与各个配方，单元格直接编辑、只看差异、多选复制。"
              : "新建、复制、重命名、删除、设为默认、导入导出配方文件，以及失配报告。"}
          </p>
        </div>
      )}
    </section>
  );
}

// ------------------------------------------------------------ 按节点

/** 上次运行里带 paramPath 的错误，拍成一个字符串键：执行 store 每 16 ms 一批事件，
 *  只有这一段真的变了才重建面板的模型（zustand 按 Object.is 比，字符串相等就不重渲）。 */
function runErrorsKey(nodes: ReadonlyMap<string, { errors: { paramPath?: string | undefined; message: string; code: string }[] }>): string {
  const out: [string, string, string, string][] = [];
  for (const [id, n] of nodes) {
    for (const e of n.errors) if (e.paramPath) out.push([id, e.paramPath, e.message, e.code]);
  }
  return out.length === 0 ? "" : JSON.stringify(out);
}

function usePanelDiagnostics(): PanelDiagnostics {
  const byNode = useValidationStore((s) => s.byNode);
  const graphLevel = useValidationStore((s) => s.graphLevel);
  const runKey = useExecutionStore((s) => runErrorsKey(s.nodes));
  return useMemo(() => {
    const nodes = new Map<string, Map<string, RowDiag[]>>();
    const add = (node: string, param: string, d: RowDiag) => {
      let m = nodes.get(node);
      if (!m) nodes.set(node, (m = new Map()));
      const list = m.get(param);
      // 同一参数两边都有同一句话时只留一条（校验的那条在前：它是对着当前的值说的）
      if (list) {
        if (!list.some((x) => x.message === d.message)) list.push(d);
      } else {
        m.set(param, [d]);
      }
    };
    for (const [id, diags] of byNode) {
      for (const d of diags) {
        if (d.paramPath && (d.severity === "error" || d.severity === "warning")) {
          add(id, d.paramPath, { severity: d.severity, message: d.message, code: d.code });
        }
      }
    }
    if (runKey) {
      for (const [id, param, message, code] of JSON.parse(runKey) as [string, string, string, string][]) {
        add(id, param, { severity: "error", message, code });
      }
    }
    const byGraphParam = new Map<string, RowDiag[]>();
    for (const d of graphLevel) {
      if (!d.paramPath || (d.severity !== "error" && d.severity !== "warning")) continue;
      const list = byGraphParam.get(d.paramPath) ?? [];
      list.push({ severity: d.severity, message: d.message, code: d.code });
      byGraphParam.set(d.paramPath, list);
    }
    return { byNode: nodes, byGraphParam };
  }, [byNode, graphLevel, runKey]);
}

/** 估一个行高（挂上之后按真值修正）。估得越准，滚动条越不抖。 */
function estimate(item: PanelItem): number {
  switch (item.kind) {
    case "gp-head":
      return 34;
    case "node-head":
      return 44;
    case "group-head":
      return 28;
    case "def-head":
      return 32;
    case "empty":
      return 40;
    case "gp-row":
      return 84 + item.row.diags.length * 18;
    case "row": {
      const extra = item.row.diags.length * 18;
      switch (item.row.param.type) {
        case "text":
          return 80 + extra;
        case "vec2f":
        case "vec3f":
          return 38 + extra;
        case "vec4f":
          return (item.row.param.semantic === "roi" ? 92 : 38) + extra;
        case "transform":
          return 96 + extra;
        case "curve":
          return 250 + extra;
        default:
          return 38 + extra;
      }
    }
  }
}

const keyOf = (item: PanelItem) => item.key;

function NodesTab() {
  const doc = useGraphStore((s) => s.doc);
  const path = useUiStore((s) => s.path);
  const base = useManifestStore((s) => s.operatorsById);
  const overrides = useGraphParamOverrides();
  const diagnostics = usePanelDiagnostics();
  const ops = useMemo(() => augmentOperators(base, doc.subgraphs), [base, doc.subgraphs]);
  const model = useMemo(
    () => buildPanelModel({ doc, ops, path, overrides, diagnostics }),
    [doc, ops, path, overrides, diagnostics],
  );

  const [query, setQuery] = useState("");
  const [chip, setChip] = useState<PanelChip>("all");
  const [type, setType] = useState<ParamType | null>(null);
  const [toggled, setToggled] = useState<Record<string, boolean>>({});
  const view = useMemo(
    () => flattenPanel(model, { query, chip, type, toggled }),
    [model, query, chip, type, toggled],
  );
  const toggle = useCallback((key: string) => setToggled((t) => ({ ...t, [key]: !t[key] })), []);

  const list = useRef<VirtualListHandle>(null);
  const [highlight, setHighlight] = useState<string | null>(null);
  const fromPanel = useRef(false);

  // 画布上选中节点 → 面板滚到那一节并高亮（P2.1）。面板里点标题引起的选中不再滚（它已经在眼前）
  const selected = useUiStore((s) => s.selectedNodes);
  useEffect(() => {
    if (selected.size !== 1) return;
    const key = sectionKey(path, [...selected][0]!);
    setHighlight(key);
    if (fromPanel.current) {
      fromPanel.current = false;
      list.current?.scrollToKey(key, "nearest");
      return;
    }
    // 等这一帧的 items 落定（刚换层、刚展开时 key 还不在列表里）
    requestAnimationFrame(() => list.current?.scrollToKey(key, "start"));
  }, [selected, path]);

  // 面板里点节点标题 → 画布选中并居中（P2.1）。定义里的节点先进到那一层
  const rf = useReactFlow();
  const motionOn = useMotionEnabled();
  const focusNode = useCallback(
    (s: NodeSection) => {
      const ui = useUiStore.getState();
      fromPanel.current = true;
      if (s.path.length !== ui.path.length) ui.setPath(s.path);
      ui.setSelection([s.node.id], []);
      let tries = 0;
      const center = () => {
        const n = rf.getInternalNode(s.node.id);
        if (!n) {
          if ((tries += 1) < 20) requestAnimationFrame(center);
          return;
        }
        const w = n.measured?.width ?? 0;
        const h = n.measured?.height ?? 0;
        void rf.setCenter(n.internals.positionAbsolute.x + w / 2, n.internals.positionAbsolute.y + h / 2, {
          zoom: rf.getZoom(),
          duration: viewportMs(motionOn),
        });
      };
      requestAnimationFrame(center);
    },
    [rf, motionOn],
  );

  const locate = useCallback((key: string) => {
    setHighlight(key);
    list.current?.scrollToKey(key, "start");
  }, []);

  const render = useCallback(
    (item: PanelItem) => {
      switch (item.kind) {
        case "gp-head":
          return (
            <GroupTitle
              testId="pp-gp-head"
              open={item.open}
              onToggle={() => toggle("gp")}
              className="pp-gp-head"
              invalid={item.errors > 0}
            >
              图参数 <span className="pp-count">{item.count}</span>
              {item.errors > 0 && <span className="pp-gp-head__err"> · {item.errors} 处有错</span>}
            </GroupTitle>
          );
        case "gp-row":
          return <GraphParamPanelRow row={item.row} focused={highlight === item.key} />;
        case "node-head":
          return (
            <NodeHead
              item={item}
              focused={highlight === item.key}
              onToggle={() => toggle(item.key)}
              onFocus={focusNode}
            />
          );
        case "group-head":
          return (
            <GroupTitle
              testId={`pp-group-${item.key}`}
              open={item.open}
              onToggle={() => toggle(item.key)}
              className="pp-group"
              depth={item.depth}
            >
              {item.name || (item.advanced ? "高级" : "")}
              {item.advanced && item.name !== "高级" && <span className="pp-group__adv">高级</span>}
              <span className="pp-count">{item.count}</span>
            </GroupTitle>
          );
        case "def-head":
          return (
            <GroupTitle
              testId={`pp-def-${item.section.fullNodeId}`}
              open={item.open}
              onToggle={() => toggle(item.key)}
              className="pp-def"
              depth={item.section.depth}
            >
              子图定义 · {item.shared} 个实例共享
              <span className="pp-count">{item.nodes} 个节点</span>
            </GroupTitle>
          );
        case "row":
          return (
            <PanelParamRow
              row={item.row}
              focused={highlight !== null && highlight === `n:${item.row.fullNodeId}`}
              onLocate={locate}
            />
          );
        case "empty":
          return <p className="pp-empty">{item.text}</p>;
      }
    },
    [highlight, toggle, focusNode, locate],
  );

  const typeOptions = TYPES.filter((t) => (view.counts.byType[t] ?? 0) > 0);

  return (
    <div className="ppanel__body">
      <div className="pp-filter">
        <input
          className="ctl pp-search"
          data-testid="pp-search"
          type="search"
          placeholder="搜索参数名、label、节点、值…"
          value={query}
          spellCheck={false}
          onChange={(e) => setQuery(e.target.value)}
          onKeyDown={(e) => {
            if (e.key === "Escape") {
              e.stopPropagation();
              setQuery("");
            }
          }}
        />
        <div className="pp-chips">
          {CHIPS.map((c) => (
            <button
              key={c.id}
              type="button"
              className={`ctl-chip pp-chip${chip === c.id ? " is-on" : ""}`}
              data-testid={`pp-chip-${c.id}`}
              data-count={view.counts[c.id]}
              aria-pressed={chip === c.id}
              title={c.title}
              onClick={() => setChip(c.id)}
            >
              {c.label} <span className="pp-count">{view.counts[c.id]}</span>
            </button>
          ))}
          <select
            className={`ctl pp-type${type ? " is-on" : ""}`}
            data-testid="pp-type"
            value={type ?? ""}
            title="按参数类型过滤"
            onChange={(e) => setType((e.target.value || null) as ParamType | null)}
          >
            <option value="">类型 ▾</option>
            {typeOptions.map((t) => (
              <option key={t} value={t} data-count={view.counts.byType[t]}>
                {t} ({view.counts.byType[t]})
              </option>
            ))}
            {type && !typeOptions.includes(type) && <option value={type}>{type} (0)</option>}
          </select>
        </div>
      </div>
      <VirtualList
        items={view.items}
        keyOf={keyOf}
        estimate={estimate}
        render={render}
        className="pp-list"
        testId="pp-list"
        handle={list}
      />
    </div>
  );
}

function GroupTitle({
  testId,
  open,
  onToggle,
  className,
  depth = 0,
  invalid = false,
  children,
}: {
  testId: string;
  open: boolean;
  onToggle: () => void;
  className: string;
  depth?: number;
  invalid?: boolean;
  children: React.ReactNode;
}) {
  return (
    <button
      type="button"
      className={`pp-title ${className}${invalid ? " is-invalid" : ""}`}
      data-testid={testId}
      data-open={open ? "1" : "0"}
      style={{ ["--pp-depth" as string]: depth }}
      onClick={onToggle}
    >
      <span className="pp-title__chev">{open ? "▾" : "▸"}</span>
      {children}
    </button>
  );
}

const NodeHead = memo(function NodeHead({
  item,
  focused,
  onToggle,
  onFocus,
}: {
  item: Extract<PanelItem, { kind: "node-head" }>;
  focused: boolean;
  onToggle: () => void;
  onFocus: (s: NodeSection) => void;
}) {
  const s = item.section;
  return (
    <div
      className={`pp-node${focused ? " is-focused" : ""}`}
      data-testid={`pp-node-${s.fullNodeId}`}
      data-node={s.node.id}
      data-focused={focused ? "1" : undefined}
      style={{ ["--pp-depth" as string]: s.depth }}
    >
      <button type="button" className="pp-title__chev pp-node__chev" onClick={onToggle} title="收起 / 展开这一节">
        {item.open ? "▾" : "▸"}
      </button>
      <button
        type="button"
        className="pp-node__title"
        data-testid={`pp-node-title-${s.fullNodeId}`}
        title="在画布上选中并居中这个节点"
        onClick={() => onFocus(s)}
      >
        {s.title}
      </button>
      <code className="pp-node__op">{s.op?.id ?? s.node.op}</code>
      {s.shared > 0 && (
        <span
          className="pp-badge pp-badge--shared"
          data-testid={`pp-shared-${s.fullNodeId}`}
          title="这是子图定义里的节点：改它，用这个定义的每个实例都跟着变"
        >
          子图定义 · {s.shared} 个实例共享
        </span>
      )}
      {s.library && (
        <span
          className="pp-badge"
          data-testid={`pp-library-${s.fullNodeId}`}
          title="库算子的定义在库文件里：这里只改它对外的参数，内部只读"
        >
          库算子 · 内部只读
        </span>
      )}
      {s.subgraphId && <span className="pp-badge">子图</span>}
      <span className="pp-node__count" data-testid={`pp-count-${s.fullNodeId}`}>
        {item.total === 0 ? "无参数" : `${item.total} 参数 · ${item.modified} 已改`}
        {item.matched !== null && ` · 命中 ${item.matched}`}
      </span>
    </div>
  );
});

function Diags({ diags }: { diags: readonly RowDiag[] }) {
  if (diags.length === 0) return null;
  return (
    <ul className="pp-diags">
      {diags.map((d, i) => (
        <li key={i} className={`pp-diag is-${d.severity}`} data-severity={d.severity}>
          {d.code && <code>{d.code}</code>} {d.message}
        </li>
      ))}
    </ul>
  );
}

// ------------------------------------------------------------ 一行节点参数

/** 行的结构（P2.4）：左 label（改过的有蓝点），中控件，右图标（恢复、纳入配方书签）。
 *  最左一条 stripe 与 label 后的 tags 是给 P3 留的位置：被当前配方覆盖的行挂橙色竖条与
 *  「配方 · 基础 X」标签。 */
const PanelParamRow = memo(function PanelParamRow({
  row,
  focused,
  onLocate,
}: {
  row: NodeParamRow;
  focused: boolean;
  onLocate: (key: string) => void;
}) {
  const { param, node, path, binding } = row;
  const disabled = !row.enabled || row.readOnly !== null;
  const error = row.diags.find((d) => d.severity === "error");
  const onChange = useCallback(
    (v: unknown) => {
      if (!valueEquals(v, row.value)) useGraphStore.getState().setParam(node.id, param.name, v, path);
    },
    [row.value, node.id, param.name, path],
  );
  return (
    <div
      className={`prow${disabled ? " is-disabled" : ""}${error ? " has-error" : ""}${focused ? " is-focused" : ""}`}
      data-testid={`prow-${row.key}`}
      data-node={row.fullNodeId}
      data-param={param.name}
      data-type={param.type}
      data-modified={row.modified ? "1" : undefined}
      data-graph-param={binding?.graphParam}
      data-readonly={row.readOnly ? "1" : undefined}
      data-enabled={row.enabled ? "1" : "0"}
      data-diag={row.diags.length > 0 ? row.diags.length : undefined}
      data-shared={row.shared > 0 ? row.shared : undefined}
      style={{ ["--pp-depth" as string]: row.depth }}
    >
      <span className="prow__stripe" aria-hidden />
      <div className="prow__label">
        <span className={`prow__dot${row.modified ? " is-on" : ""}`} aria-hidden title="与算子默认值不同" />
        <span className="prow__name" title={param.doc ? `${param.name}：${param.doc}` : param.name}>
          {param.label || param.name}
        </span>
        <span className="prow__tags">
          {binding && (
            <span className="prow__tag prow__tag--graph" title="值来自顶层图参数；在这一行改的是它">
              图参数 {binding.graphParam}
            </span>
          )}
          {row.promoted && !binding && (
            <span className="prow__tag" title={row.readOnly ?? ""}>
              ↑{row.promoted}
            </span>
          )}
        </span>
      </div>
      <div className="prow__control">
        {param.semantic === "roi" && param.type === "vec4f" && <RoiThumb row={row} />}
        <ParamControl
          param={param}
          value={row.value}
          disabled={disabled}
          nodeId={node.id}
          path={path}
          promotedAs={row.promoted ?? undefined}
          graphBinding={binding}
          onChange={onChange}
        />
        <Diags diags={row.diags} />
      </div>
      <div className="prow__icons">
        {row.modified && row.readOnly === null && row.enabled && (
          <button
            type="button"
            className="prow__icon"
            data-testid={`pp-reset-${row.key}`}
            title={`恢复算子默认值 ${JSON.stringify(param.default)}`}
            onClick={() => useGraphStore.getState().setParam(node.id, param.name, param.default, path)}
          >
            ↺
          </button>
        )}
        {binding ? (
          <button
            type="button"
            className="prow__icon prow__bookmark is-on"
            data-testid={`pp-bookmark-${row.key}`}
            data-on="1"
            title={`已纳入配方：由图参数 ${binding.graphParam} 提供（点一下定位到它）`}
            onClick={() => onLocate(`gp:${binding.graphParam}`)}
          >
            <Bookmark on />
          </button>
        ) : (
          <button
            type="button"
            className="prow__icon prow__bookmark"
            data-testid={`pp-bookmark-${row.key}`}
            data-on="0"
            title={
              path.length > 0
                ? "纳入配方：逐层提升成图参数（内参 → 子图参数 → 这个实例上的图参数）"
                : "纳入配方：提升成顶层图参数，当前值成为它的默认值"
            }
            onClick={() => {
              const name = useGraphStore.getState().promoteToGraphParam(node.id, param.name, path);
              if (name) useUiStore.getState().showToast(`已纳入配方：图参数 ${name}`);
            }}
          >
            <Bookmark on={false} />
          </button>
        )}
      </div>
    </div>
  );
});

function Bookmark({ on }: { on: boolean }) {
  return (
    <svg width="12" height="14" viewBox="0 0 12 14" aria-hidden>
      <path d="M2 1.5h8v11L6 9.6 2 12.5z" fill={on ? "currentColor" : "none"} stroke="currentColor" strokeWidth="1.3" strokeLinejoin="round" />
    </svg>
  );
}

/** ROI 行的缩略图（P2.7）：同组的框都画在底图范围里，这一个高亮；点它或「拖框」进 2D 拖框视图。 */
function RoiThumb({ row }: { row: NodeParamRow }) {
  // 视图取到这个节点的云之后换成真底图的范围（行本身没变，不订阅就一直是框的并集）
  useSyncExternalStore(subscribeRoiBounds, roiBoundsVersion);
  const thumb = roiThumb(row.op, row.effective, row.param.name, row.fullNodeId);
  const W = 72;
  const H = 44;
  const [x0, y0, x1, y1] = thumb.extent;
  const sx = (x: number) => ((x - x0) / (x1 - x0)) * W;
  // 世界 y 向上、屏幕 y 向下
  const sy = (y: number) => H - ((y - y0) / (y1 - y0)) * H;
  const enter = () => enterRoiEdit(row, thumb.frame);
  return (
    <div className="prow__roi">
      <svg
        className="prow__thumb"
        data-testid={`pp-roi-thumb-${row.key}`}
        data-from-cloud={thumb.fromCloud ? "1" : "0"}
        width={W}
        height={H}
        viewBox={`0 0 ${W} ${H}`}
        onClick={enter}
      >
        <title>{thumb.fromCloud ? "框在底图上的位置（点开进 2D 拖框）" : "框之间的相对位置（还没有底图；点开进 2D 拖框）"}</title>
        <rect className="prow__thumb-bg" x={0} y={0} width={W} height={H} />
        {thumb.boxes.map((b) => (
          <rect
            key={b.param}
            className={`prow__thumb-box${b.current ? " is-current" : ""}`}
            x={sx(b.rect[0])}
            y={sy(b.rect[3])}
            width={Math.max(1, sx(b.rect[2]) - sx(b.rect[0]))}
            height={Math.max(1, sy(b.rect[1]) - sy(b.rect[3]))}
          />
        ))}
      </svg>
      <button type="button" className="ctl-btn" data-testid={`pp-roi-edit-${row.key}`} onClick={enter}>
        拖框
      </button>
    </div>
  );
}

/** 进现有的 2D 拖框视图（Viewer3D 的 RoiLayer）：选中节点、切到这一组框、相机切 2D、视图展开。
 *  定义里的节点先进到那一层 —— 视图只画当前层的节点。拖动写回走 setParam，与 Inspector 同一条路。 */
function enterRoiEdit(row: NodeParamRow, frame: string | null) {
  const ui = useUiStore.getState();
  if (row.path.length !== ui.path.length) ui.setPath(row.path);
  ui.setPinnedNode(null);
  ui.setSelection([row.node.id], []);
  if (frame) ui.setRoiFrame(row.node.id, frame);
  ui.setViewerMode("2d");
  ui.setPanelViewerOpen(true);
}

// ------------------------------------------------------------ 一行图参数

const SPEC_FIELDS: { key: keyof ParamSpec; label: string; numeric: boolean }[] = [
  { key: "label", label: "Label", numeric: false },
  { key: "min", label: "Min", numeric: true },
  { key: "max", label: "Max", numeric: true },
  { key: "softMin", label: "Soft Min", numeric: true },
  { key: "softMax", label: "Soft Max", numeric: true },
  { key: "step", label: "Step", numeric: true },
  { key: "unit", label: "单位", numeric: false },
  { key: "group", label: "Group", numeric: false },
];

/** 「图参数」分组的一行（P2.3）：值可改（按 K6 写进配方或 default），规格可改（label、限位、单位、group），
 *  名字可改，绑定可以一条条解除，整个可以删（当前值写回每个绑定目标，行为不变）。 */
const GraphParamPanelRow = memo(function GraphParamPanelRow({
  row,
  focused,
}: {
  row: GraphParamRow;
  focused: boolean;
}) {
  const [specOpen, setSpecOpen] = useState(false);
  const { name, gp, spec } = row;
  const error = row.diags.find((d) => d.severity === "error");
  const g = useGraphStore.getState;
  return (
    <div
      className={`prow prow--gp${error ? " has-error" : ""}${focused ? " is-focused" : ""}`}
      data-testid={`pp-gp-${name}`}
      data-graph-param={name}
      data-type={row.type ?? undefined}
      data-modified={row.modified ? "1" : undefined}
      data-diag={row.diags.length > 0 ? row.diags.length : undefined}
    >
      <span className="prow__stripe" aria-hidden />
      <div className="prow__label">
        <span className={`prow__dot${row.modified ? " is-on" : ""}`} aria-hidden />
        <span className="prow__name" title={gp.doc ?? name}>
          {gp.label || name}
        </span>
        <code className="prow__gpname">{name}</code>
      </div>
      <div className="prow__control">
        {spec ? (
          <ParamControl
            param={spec}
            value={row.value}
            disabled={false}
            onChange={(v) => {
              if (!valueEquals(v, row.value)) g().editGraphParamValue(name, v);
            }}
          />
        ) : (
          <code>{JSON.stringify(row.value)}</code>
        )}
        <Diags diags={row.diags} />
        <div className="prow__binds">
          {gp.binds.length === 0 && <span className="insp__hint">（没有绑定任何参数）</span>}
          {gp.binds.map((b) => (
            <span className="insp-gparam__bind" key={b} data-bind={b}>
              {b}
              <button
                type="button"
                className="ctl-btn"
                data-testid={`pp-unbind-${name}-${b}`}
                title="解除这一条绑定：当前值写回这个参数，行为不变"
                onClick={() => g().unbindFromGraphParam(name, b)}
              >
                ✕
              </button>
            </span>
          ))}
        </div>
        {specOpen && <SpecEditor name={name} spec={gp} />}
      </div>
      <div className="prow__icons">
        <button
          type="button"
          className={`prow__icon${specOpen ? " is-on" : ""}`}
          data-testid={`pp-spec-${name}`}
          aria-pressed={specOpen}
          title="编辑规格：名字、label、限位、单位、group"
          onClick={() => setSpecOpen((v) => !v)}
        >
          ⚙
        </button>
        <button
          type="button"
          className="prow__icon"
          data-testid={`pp-remove-${name}`}
          title="删除这个图参数：当前值写回它绑定的每一个参数，行为不变"
          onClick={() => g().removeGraphParam(name)}
        >
          ✕
        </button>
      </div>
    </div>
  );
});

function SpecEditor({ name, spec }: { name: string; spec: ParamSpec }) {
  return (
    <div className="pp-spec" data-testid={`pp-spec-editor-${name}`}>
      <label className="pp-spec__field">
        <span>名字</span>
        <SpecText
          testId={`pp-spec-${name}-name`}
          value={name}
          onCommit={(next) => {
            const g = useGraphStore.getState();
            const problem = graphParamNameProblem(g.doc, next, name);
            if (problem) {
              useUiStore.getState().showToast(problem, "warn");
              return false;
            }
            return g.renameGraphParam(name, next);
          }}
        />
      </label>
      {SPEC_FIELDS.map((f) => (
        <label className="pp-spec__field" key={f.key}>
          <span>{f.label}</span>
          <SpecText
            testId={`pp-spec-${name}-${f.key}`}
            value={spec[f.key] === undefined ? "" : String(spec[f.key])}
            onCommit={(text) => {
              let v: unknown = text.trim() === "" ? undefined : text;
              if (f.numeric && v !== undefined) {
                const n = Number(text);
                if (!Number.isFinite(n)) {
                  useUiStore.getState().showToast(`${f.label} 要一个数字`, "warn");
                  return false;
                }
                v = n;
              }
              useGraphStore.getState().setGraphParamSpec(name, { [f.key]: v } as Partial<ParamSpec>);
              return true;
            }}
          />
        </label>
      ))}
    </div>
  );
}

/** 规格的一格：失焦或回车才提交（一次一条撤销），提交被拒就退回原值。 */
function SpecText({
  testId,
  value,
  onCommit,
}: {
  testId: string;
  value: string;
  onCommit: (text: string) => boolean;
}) {
  const [text, setText] = useState(value);
  useEffect(() => setText(value), [value]);
  return (
    <input
      className="ctl ctl--str"
      data-testid={testId}
      value={text}
      spellCheck={false}
      onChange={(e) => setText(e.target.value)}
      onBlur={() => {
        if (text === value) return;
        if (!onCommit(text)) setText(value);
      }}
      onKeyDown={(e) => {
        if (e.key === "Enter") e.currentTarget.blur();
        if (e.key === "Escape") {
          setText(value);
          e.currentTarget.blur();
        }
      }}
    />
  );
}
