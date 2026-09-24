// 画布。React Flow 的交互事件在这里翻译成 graph store 的语义化动作。
// M4 起只渲染 ui.path 指的那一层，面包屑负责进出（ADR-0010）。

import {
  Background,
  BackgroundVariant,
  Controls,
  MiniMap,
  ReactFlow,
  useReactFlow,
  ViewportPortal,
  type Connection,
  type Edge,
  type EdgeChange,
  type FinalConnectionState,
  type NodeChange,
  type OnNodeDrag,
  type OnSelectionChangeParams,
} from "@xyflow/react";
import { useCallback, useEffect, useMemo, useRef, useState } from "react";

import { layoutGraph } from "../lib/layout";
import { useMotionEnabled, withLayoutTransition } from "../lib/motion";
import {
  createMappingCache,
  distanceToSegment,
  extractMoves,
  extractSizes,
  segmentHitsRect,
  toReactFlow,
  type LyNode,
} from "../lib/mapping";
import { defaultViewFor, peekSourceOf } from "../lib/peekSource";
import { augmentOperators, fullId, levelOf, pathIsValid } from "../lib/subgraph";
import { canConnect, compatibleSources, compatibleTargets, inferAnyTypes } from "../lib/typecheck";
import { keyHint } from "../lib/keymap";
import { useExecutionStore } from "../store/execution";
import { useGraphStore } from "../store/graph";
import { useManifestStore } from "../store/manifest";
import {
  clampPeekScreen,
  findWindowForEdge,
  usePeekStore,
  PEEK_DEFAULT_SIZE,
} from "../store/peek";
import { useUiStore } from "../store/ui";
import { transport } from "../transport";
import { subgraphIdOf, type GraphDoc } from "../types/graph";

import { addNodeWithAutoConnect, insertSnippetById } from "../lib/insert";
import { EdgePeekLayer } from "./EdgePeekLayer";
import { FlowEdge } from "./FlowEdge";
import { OPERATOR_DND_MIME, SNIPPET_DND_MIME } from "./NodePalette";
import { OperatorNode } from "./OperatorNode";
import { useCanvasMotion } from "./useCanvasMotion";

import "@xyflow/react/dist/style.css";

const nodeTypes = { operator: OperatorNode };

/** 所有连线都走同一个组件（docs/motion-plan.md E1）：惰性边的虚线与 tooltip、hover、
 *  流动、生长都在里面。替换的是 default 类型，所以 mapping 不用给边写 type。 */
const edgeTypes = { default: FlowEdge };

/** 吸附半径。24 px 是「靠近就吸上」和「误吸到隔壁端口」之间的平衡点（P1 #17）。 */
const CONNECTION_RADIUS = 24;
/** 网格步长。8 是常见节点宽高的公约数，吸上去不会看着歪。 */
const GRID: [number, number] = [8, 8];
/** 对齐参考线的容差，画布坐标。 */
const GUIDE_TOLERANCE = 4;
/** 拖节点到连线上的命中容差（P1 #21）。主判据是连线穿过节点矩形，
 *  这个半径只是给「擦着边过去」留一点余量。 */
const EDGE_HIT_RADIUS = 12;
/** 插入用的透传算子。它是普通算子不是特殊节点类型（E5）。 */
const REROUTE_OP = "util.reroute";
/** 超过这个节点数就只渲染可见的那些（§4）。小图下全量渲染的手感更好。 */
const VIRTUALIZE_ABOVE = 80;

// 下面这几个都必须是模块常量，不能写成 JSX 里的字面量。React Flow 的 StoreUpdater
// 按**引用**比较它跟踪的那批 props（`defaultEdgeOptions` 就在里面），内联对象每次渲染
// 都是新引用，effect 于是每帧跑满一遍并往 store 里写一次、通知一遍所有订阅者。
// 剩下几个虽然不在跟踪表里，但一样会白白透传给内层组件。
const DELETE_KEYS = ["Delete"];
const MULTI_SELECTION_KEYS = ["Shift", "Control"];
/** 中键与右键平移。左键留给框选。 */
const PAN_BUTTONS = [1, 2];
const PRO_OPTIONS = { hideAttribution: false };
const CONNECTION_LINE_STYLE = { stroke: "#4a9eff", strokeWidth: 2 };
const DEFAULT_EDGE_OPTIONS = { type: "default" };

export interface CanvasActions {
  /** 只跑到某个节点（交互清单 P1 #27）。 */
  onRunToNode: (nodeId: string) => void;
}

interface ContextMenuState {
  nodeId: string;
  x: number;
  y: number;
}

interface EdgeMenuState {
  edgeId: string;
  x: number;
  y: number;
}

const PEEK_FLASH_MS = 200;

function flashPeek(root: HTMLElement | null, id: string): void {
  const el = root?.querySelector(`[data-peek-id="${id}"]`);
  if (!el) return;
  el.classList.add("is-flash");
  setTimeout(() => el.classList.remove("is-flash"), PEEK_FLASH_MS);
}

interface Guide {
  axis: "x" | "y";
  at: number;
}

/** 节点在画布坐标下的矩形。参考线和边命中都要用。 */
function rectOf(
  doc: GraphDoc,
  id: string,
  measured: ReadonlyMap<string, { width: number; height: number }>,
) {
  const node = doc.nodes.find((n) => n.id === id);
  const p = node?.ui?.position ?? { x: 0, y: 0 };
  const size = measured.get(id) ?? { width: 220, height: 90 };
  return { x: p.x, y: p.y, w: size.width, h: size.height };
}

/** 当前层级的「像一份 doc」的视图。只认 GraphDoc 的函数都吃它。 */
function levelView(): GraphDoc {
  const doc = useGraphStore.getState().doc;
  const lvl = levelOf(doc, useUiStore.getState().path);
  return lvl === doc ? doc : { ...doc, nodes: lvl.nodes, edges: lvl.edges };
}

/** 面包屑。点任意一段回到那一层（F2）。 */
function Breadcrumb() {
  const path = useUiStore((s) => s.path);
  const doc = useGraphStore((s) => s.doc);
  const exitTo = useUiStore((s) => s.exitTo);
  if (path.length === 0) return null;
  return (
    <nav className="breadcrumb" data-testid="breadcrumb" data-depth={path.length}>
      <button type="button" data-testid="breadcrumb-root" onClick={() => exitTo(0)}>
        顶层
      </button>
      {path.map((seg, i) => (
        <span key={`${seg.nodeId}-${i}`} className="breadcrumb__seg">
          <span className="breadcrumb__sep">/</span>
          <button
            type="button"
            data-testid={`breadcrumb-${i}`}
            onClick={() => exitTo(i + 1)}
            disabled={i === path.length - 1}
          >
            {doc.subgraphs?.[seg.subgraphId]?.name || seg.subgraphId}
          </button>
        </span>
      ))}
      <span className="breadcrumb__hint">Esc 退出上一层</span>
    </nav>
  );
}

/** 保存到库的小表单。id 决定文件名与算子 id，所以必须让人自己填。 */
function LibraryDialog({
  subgraphId,
  onClose,
}: {
  subgraphId: string;
  onClose: () => void;
}) {
  const def = useGraphStore((s) => s.doc.subgraphs?.[subgraphId]);
  const [id, setId] = useState((def?.name || subgraphId).replace(/[^\w.-]+/g, "_"));
  const [category, setCategory] = useState(def?.category?.replace(/^Library\//, "") || "General");
  const [busy, setBusy] = useState(false);

  const save = async () => {
    setBusy(true);
    try {
      const status = await transport.saveAsLibrary(useGraphStore.getState().doc, subgraphId, {
        id,
        category,
      });
      const refreshed = await transport.refreshLibrary();
      useManifestStore.getState().replaceBundle(refreshed.manifest, 0);
      useUiStore
        .getState()
        .showToast(`已保存到库：lib.${id}（库里现在有 ${status.count} 个算子）`);
      onClose();
    } catch (e) {
      useUiStore.getState().showToast(e instanceof Error ? e.message : String(e), "warn");
      setBusy(false);
    }
  };

  return (
    <div className="modal" data-testid="library-dialog" onClick={(e) => e.stopPropagation()}>
      <h4>保存到库</h4>
      <label>
        算子 id
        <input
          data-testid="library-id"
          value={id}
          spellCheck={false}
          onChange={(e) => setId(e.target.value)}
        />
      </label>
      <label>
        分类
        <input
          data-testid="library-category"
          value={category}
          spellCheck={false}
          onChange={(e) => setCategory(e.target.value)}
        />
      </label>
      <p className="modal__hint">
        会写成 <code>{id}.lyflow-op.json</code>，在面板的 <code>Library/{category}</code> 下出现。
      </p>
      <div className="modal__row">
        <button type="button" onClick={onClose}>
          取消
        </button>
        <button
          type="button"
          data-testid="library-save"
          disabled={busy || !id.trim()}
          onClick={() => void save()}
        >
          保存
        </button>
      </div>
    </div>
  );
}

export function GraphCanvas({ onRunToNode }: CanvasActions) {
  const doc = useGraphStore((s) => s.doc);
  const path = useUiStore((s) => s.path);
  const baseOperators = useManifestStore((s) => s.operatorsById);
  const typesByName = useManifestStore((s) => s.typesByName);
  const selectedNodes = useUiStore((s) => s.selectedNodes);
  const selectedEdges = useUiStore((s) => s.selectedEdges);

  const { screenToFlowPosition, fitView } = useReactFlow();
  const wrapper = useRef<HTMLDivElement>(null);
  // 删除残影挂在这一层（N3）。它在 ViewportPortal 里，坐标就是画布坐标
  const ghostLayer = useRef<HTMLDivElement>(null);
  const motionOn = useMotionEnabled();
  const { override, cancelLayout } = useCanvasMotion(wrapper, ghostLayer, motionOn);

  /**
   * 节点量测尺寸的旁路缓存。不进 GraphDoc，但 MiniMap 靠它才肯画节点：React Flow 的
   * `NodeComponentWrapperInner` 走的是 `getNodeDimensions(node.internals.userNode)` 加
   * `nodeHasDimensions(userNode)`，读的是**这里传进 props 的那份 measured**，而不是它自己
   * 量完存在 internals 上的那份。所以量测结果必须回流到节点对象上，这条边不能简单砍掉。
   *
   * 回流意味着存在一条 量测 -> setState -> 新节点对象 -> setNodes -> 重新量测 的回路，
   * 下面 onNodesChange 里的容差与熔断是给它加的阻尼；lib/mapping.ts 的引用归一让没变的
   * 节点保持同一个对象，把每轮的重建面从"整张图"缩到"真的变了的那个节点"。
   */
  const measured = useRef(new Map<string, { width: number; height: number }>());
  const [measuredTick, setMeasuredTick] = useState(0);
  // 映射结果的引用归一：没变的节点保持同一个对象，React Flow 的 adoptUserNodes 才会走
  // checkEquality 快路径，不重建内部节点、不丢 measured。详见 lib/mapping.ts 的 MappingCache。
  const mapping = useRef(createMappingCache());
  // 同一次挂载/布局抖动里，允许连续触发重渲染的次数上限，见下面 onNodesChange 里的说明。
  const sizeBurst = useRef({ count: 0, resetHandle: null as number | null });
  const [menu, setMenu] = useState<ContextMenuState | null>(null);
  const [edgeMenu, setEdgeMenu] = useState<EdgeMenuState | null>(null);
  const [guides, setGuides] = useState<Guide[]>([]);
  const [snapping, setSnapping] = useState(true);
  const [libraryFor, setLibraryFor] = useState<string | null>(null);
  const running = useExecutionStore((s) => s.runStatus === "running");
  const dragged = useRef<string | null>(null);
  /** onReconnect 有没有接住这次拖动。onReconnectEnd 的第四个参数各版本形态不一，
   *  与其猜它，不如自己记一笔 —— 猜错的后果是把一条好边直接删掉。 */
  const reconnected = useRef(false);

  const operatorsById = useMemo(
    () => augmentOperators(baseOperators, doc.subgraphs),
    [baseOperators, doc.subgraphs],
  );
  const ctx = useMemo(() => ({ operatorsById, typesByName }), [operatorsById, typesByName]);

  // 当前层级。子图被删掉之后路径就失效了，弹回顶层比画一张空图诚实。
  const view = useMemo<GraphDoc>(() => {
    const lvl = levelOf(doc, path);
    return lvl === doc ? doc : { ...doc, nodes: lvl.nodes, edges: lvl.edges };
  }, [doc, path]);

  useEffect(() => {
    if (path.length > 0 && !pathIsValid(doc, path)) useUiStore.getState().setPath([]);
  }, [doc, path]);

  useEffect(() => {
    usePeekStore.getState().prune(doc, path);
  }, [doc, path]);

  const anyTypes = useMemo(() => inferAnyTypes(ctx, view), [ctx, view]);

  const { nodes: docNodes, edges } = useMemo(
    () =>
      toReactFlow(
        view,
        ctx,
        { nodes: selectedNodes, edges: selectedEdges },
        measured.current,
        anyTypes,
        mapping.current,
      ),
    // measuredTick 是 measured.current 的变更信号，故意作为依赖
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [view, ctx, selectedNodes, selectedEdges, measuredTick, anyTypes],
  );

  // 自动布局过渡（N4）：只把这一帧的临时位置叠在映射结果上，映射缓存里仍是 doc 的位置 ——
  // 过渡一结束 override 变回 null，节点对象原样复用，不会白重建一轮。
  const nodes = useMemo(
    () =>
      override
        ? docNodes.map((n) => {
            const p = override.get(n.id);
            return p ? { ...n, position: p } : n;
          })
        : docNodes,
    [docNodes, override],
  );

  // -- 节点变更 ------------------------------------------------------------
  const onNodesChange = useCallback((changes: NodeChange<LyNode>[]) => {
    const graph = useGraphStore.getState();

    const moves = extractMoves(changes as { type: string; id: string; position?: { x: number; y: number } }[]);
    if (moves.length > 0) graph.moveNodes(moves);

    const removed = changes.filter((c) => c.type === "remove").map((c) => c.id);
    if (removed.length > 0) graph.deleteNodes(removed);

    // 量测尺寸：不进 GraphDoc，但要回流到节点对象上供 MiniMap 使用（见上面 measured 的注释）。
    // 只在尺寸真的变了时才 bump，否则会和 React Flow 的重新量测互相触发。
    // 严格 !== 曾经假设 ResizeObserver 两次量出的同一个稳定尺寸会是完全相等的浮点数，
    // 但 Windows 下的分数 DPI 缩放会让同一个节点连续两次量出例如 142.3999939 / 142.4000015
    // 这种差 0.001px 的"抖动"，永远不会严格相等 -> sizeChanged 永远是 true -> 计数器无限自增
    // -> 触发 React Flow 自己的 setNodes 重新量测 -> 再抖一次，构成"Maximum update depth
    // exceeded" 的死循环。改成容差比较：小于半个像素的差异视为同一个尺寸，不再触发。
    const SIZE_EPSILON_PX = 0.5;
    let sizeChanged = false;
    for (const s of extractSizes(
      changes as { type: string; id: string; dimensions?: { width: number; height: number } }[],
    )) {
      const prev = measured.current.get(s.id);
      if (
        !prev ||
        Math.abs(prev.width - s.width) > SIZE_EPSILON_PX ||
        Math.abs(prev.height - s.height) > SIZE_EPSILON_PX
      ) {
        measured.current.set(s.id, { width: s.width, height: s.height });
        sizeChanged = true;
      }
    }
    // 容差挡不住的情况依然存在（比如画布在挂载瞬间跟着 Splitter/Segmented 一起被重新
    // 布局，量出的宽高每次都真的差好几像素，不是浮点抖动）。这里再加一道熔断：同一阵
    // "还在变"的连续触发超过 MAX_SIZE_BUMPS_PER_BURST 次就不再 setState 了 —— 尺寸缓存
    // 仍然照量到的最新值更新（下次真正需要用到时不会是陈旧值），只是不再拿它去触发
    // React Flow 重新渲染，从而不管抖动多大都不可能撞上 React 的嵌套更新上限。等一帧
    // 没有新的尺寸变化，计数器复位，不影响之后用户手动拖拽调整节点大小之类的正常场景。
    if (sizeChanged) {
      const burst = sizeBurst.current;
      burst.count += 1;
      if (burst.resetHandle !== null) cancelAnimationFrame(burst.resetHandle);
      burst.resetHandle = requestAnimationFrame(() => {
        sizeBurst.current.count = 0;
        sizeBurst.current.resetHandle = null;
      });
      const MAX_SIZE_BUMPS_PER_BURST = 8;
      if (burst.count <= MAX_SIZE_BUMPS_PER_BURST) {
        setMeasuredTick((t) => t + 1);
      } else if (burst.count === MAX_SIZE_BUMPS_PER_BURST + 1) {
        console.warn(
          "[LyFlow] 节点尺寸连续多次变化未收敛，已停止跟随重渲染以避免死循环（Maximum update depth exceeded）。",
        );
      }
    }

    // select / dragging 是 UI 运行时状态，不进 GraphDoc。
    // 选中由 onSelectionChange 统一处理，这里忽略。
  }, []);

  const onEdgesChange = useCallback((changes: EdgeChange<Edge>[]) => {
    const removed = changes.filter((c) => c.type === "remove").map((c) => c.id);
    if (removed.length > 0) useGraphStore.getState().disconnect(removed);
  }, []);

  // -- 拖动：整段拖动只记一条撤销 + 对齐参考线（交互清单 P1 #22）-------------
  const onNodeDragStart: OnNodeDrag<LyNode> = useCallback(
    (_e, node) => {
      dragged.current = node.id;
      // 拖动以 doc 的位置为准，布局过渡还没走完就直接落到终点
      cancelLayout();
      useUiStore.getState().setHoverPaused(true);
      useGraphStore.getState().begin();
    },
    [cancelLayout],
  );

  const onNodeDrag: OnNodeDrag<LyNode> = useCallback((e, node) => {
    // Shift 临时关掉吸附与参考线：需要摆一个「差一点」的位置时总得有出路
    const shift = "shiftKey" in e && e.shiftKey;
    setSnapping(!shift);
    if (shift) {
      setGuides([]);
      return;
    }
    const current = levelView();
    const me = rectOf(current, node.id, measured.current);
    const found: Guide[] = [];
    const pairs = (axis: "x" | "y", mine: number[], theirs: number[]) => {
      for (let i = 0; i < mine.length; i += 1) {
        const a = mine[i] as number;
        const b = theirs[i] as number;
        if (Math.abs(a - b) < GUIDE_TOLERANCE) found.push({ axis, at: b });
      }
    };
    for (const other of current.nodes) {
      if (other.id === node.id) continue;
      const r = rectOf(current, other.id, measured.current);
      pairs("x", [me.x, me.x + me.w / 2, me.x + me.w], [r.x, r.x + r.w / 2, r.x + r.w]);
      pairs("y", [me.y, me.y + me.h / 2, me.y + me.h], [r.y, r.y + r.h / 2, r.y + r.h]);
    }
    setGuides(found.slice(0, 4));
  }, []);

  /** 拖动结束时对**单个**选中节点做边命中：多选时插入谁到中间是没有答案的。 */
  const insertOnHoveredEdge = useCallback(
    (nodeId: string) => {
      const graph = useGraphStore.getState();
      const current = levelView();
      const me = rectOf(current, nodeId, measured.current);
      const center = { x: me.x + me.w / 2, y: me.y + me.h / 2 };
      const op = ctx.operatorsById.get(current.nodes.find((n) => n.id === nodeId)?.op ?? "");
      if (!op) return false;
      // 已经有连线的节点不参与插入：它多半只是被挪到了别的连线附近
      if (current.edges.some((e) => e.from.node === nodeId || e.to.node === nodeId)) return false;

      for (const edge of current.edges) {
        const a = rectOf(current, edge.from.node, measured.current);
        const b = rectOf(current, edge.to.node, measured.current);
        const from = { x: a.x + a.w, y: a.y + a.h / 2 };
        const to = { x: b.x, y: b.y + b.h / 2 };
        const hit =
          segmentHitsRect(from, to, me) || distanceToSegment(center, from, to) <= EDGE_HIT_RADIUS;
        if (!hit) continue;

        // 有且仅有一对兼容端口时才插入。多于一对就没有唯一解，宁可不动。
        const without: GraphDoc = { ...current, edges: current.edges.filter((e) => e.id !== edge.id) };
        const pairs: { inPort: string; outPort: string }[] = [];
        for (const inPort of op.inputs) {
          for (const outPort of op.outputs) {
            const upstreamOk = canConnect(ctx, without, edge.from, {
              node: nodeId,
              port: inPort.name,
            }).ok;
            const downstreamOk = canConnect(
              ctx,
              without,
              { node: nodeId, port: outPort.name },
              edge.to,
            ).ok;
            if (upstreamOk && downstreamOk) pairs.push({ inPort: inPort.name, outPort: outPort.name });
          }
        }
        if (pairs.length !== 1) continue;
        graph.insertOnEdge(edge.id, nodeId, pairs[0]!.inPort, pairs[0]!.outPort);
        return true;
      }
      return false;
    },
    [ctx],
  );

  const onNodeDragStop: OnNodeDrag<LyNode> = useCallback(
    (_e, node) => {
      setGuides([]);
      setSnapping(true);
      useUiStore.getState().setHoverPaused(false);
      const graph = useGraphStore.getState();
      graph.commit("移动节点");
      const selection = useUiStore.getState().selectedNodes;
      if (selection.size <= 1 && dragged.current === node.id) {
        if (insertOnHoveredEdge(node.id)) {
          useUiStore.getState().showToast("已插入到连线中间");
        }
      }
      dragged.current = null;
    },
    [insertOnHoveredEdge],
  );

  // -- 连线 ----------------------------------------------------------------
  const onConnect = useCallback((c: Connection) => {
    if (!c.source || !c.target || !c.sourceHandle || !c.targetHandle) return;
    const verdict = useGraphStore.getState().connect(
      { node: c.source, port: c.sourceHandle },
      { node: c.target, port: c.targetHandle },
    );
    if (!verdict.ok) {
      useUiStore.getState().showToast(verdict.reason, "warn");
    } else {
      // 人自己接了一条：自动连线留下的候选高亮就完成了使命
      useUiStore.getState().clearAutoHint();
    }
  }, []);

  /** 拖线开始：把「哪些端口能落」算一次存进 ui store，端口自己去读（P1 #20）。 */
  const onConnectStart = useCallback(
    (
      _e: unknown,
      params: { nodeId: string | null; handleId: string | null; handleType: string | null },
    ) => {
      if (!params.nodeId || !params.handleId) return;
      const current = levelView();
      const fromOutput = params.handleType === "source";
      const ref = { node: params.nodeId, port: params.handleId };
      const compatible = fromOutput
        ? compatibleTargets(ctx, current, ref)
        : compatibleSources(ctx, current, ref);
      useUiStore.getState().beginConnection(
        { ...ref, side: fromOutput ? "output" : "input" },
        compatible,
      );
    },
    [ctx],
  );

  /** 拖到空白处松手 → 弹搜索面板，选中后自动接上（交互清单 P1 #18）。 */
  const onConnectEnd = useCallback(
    (event: MouseEvent | TouchEvent, state: FinalConnectionState) => {
      const ui = useUiStore.getState();
      const pending = ui.pendingFrom;
      ui.endConnection();
      if (state.isValid || !pending) return;
      const point =
        "clientX" in event
          ? { x: event.clientX, y: event.clientY }
          : { x: event.changedTouches[0]?.clientX ?? 0, y: event.changedTouches[0]?.clientY ?? 0 };
      ui.openSearch({
        screen: point,
        flow: screenToFlowPosition(point),
        pendingFrom: { node: pending.node, port: pending.port },
        pendingSide: pending.side,
      });
    },
    [screenToFlowPosition],
  );

  const onReconnectStart = useCallback(() => {
    reconnected.current = false;
  }, []);

  /** 拖离输入端后落到别的端口上（交互清单 P1 #19）。 */
  const onReconnect = useCallback((oldEdge: Edge, c: Connection) => {
    if (!c.target || !c.targetHandle) return;
    const verdict = useGraphStore
      .getState()
      .reconnectEdge(oldEdge.id, { node: c.target, port: c.targetHandle });
    if (verdict.ok) reconnected.current = true;
    else useUiStore.getState().showToast(verdict.reason, "warn");
  }, []);

  /** 拖离之后落在空白处：断开并弹搜索面板，复用 #18 的通路。 */
  const onReconnectEnd = useCallback(
    (event: MouseEvent | TouchEvent, edge: Edge) => {
      if (reconnected.current) {
        reconnected.current = false;
        return;
      }
      const graph = useGraphStore.getState();
      const from = levelView().edges.find((e) => e.id === edge.id)?.from;
      graph.disconnect([edge.id]);
      if (!from || !("clientX" in event)) return;
      const point = { x: event.clientX, y: event.clientY };
      useUiStore.getState().openSearch({
        screen: point,
        flow: screenToFlowPosition(point),
        pendingFrom: from,
        pendingSide: "output",
      });
    },
    [screenToFlowPosition],
  );

  /** 拖线过程中实时判定能不能落。这是「手感」那一层，
   *  权威校验仍然在 C++ 侧（docs/architecture.md）。 */
  const isValidConnection = useCallback(
    (c: Connection | Edge) => {
      const source = "source" in c ? c.source : null;
      const target = "target" in c ? c.target : null;
      const sourceHandle = "sourceHandle" in c ? c.sourceHandle : null;
      const targetHandle = "targetHandle" in c ? c.targetHandle : null;
      if (!source || !target || !sourceHandle || !targetHandle) return false;
      return canConnect(
        ctx,
        levelView(),
        { node: source, port: sourceHandle },
        { node: target, port: targetHandle },
      ).ok;
    },
    [ctx],
  );

  // -- 选中 ----------------------------------------------------------------
  const onSelectionChange = useCallback((params: OnSelectionChangeParams) => {
    useUiStore.getState().setSelection(
      params.nodes.map((n) => n.id),
      params.edges.map((e) => e.id),
    );
  }, []);

  // -- hover（docs/motion-plan.md H2 / H3）。纯 UI 状态，进 ui store 不进 doc -------
  const onNodeMouseEnter = useCallback((_e: React.MouseEvent, node: { id: string }) => {
    useUiStore.getState().setHoverNode(node.id);
  }, []);
  const onNodeMouseLeave = useCallback(() => {
    useUiStore.getState().setHoverNode(null);
  }, []);
  const onEdgeMouseEnter = useCallback((_e: React.MouseEvent, edge: Edge) => {
    if (!edge.sourceHandle || !edge.targetHandle) return;
    useUiStore.getState().setHoverEdge({
      id: edge.id,
      from: { node: edge.source, port: edge.sourceHandle },
      to: { node: edge.target, port: edge.targetHandle },
    });
  }, []);
  const onEdgeMouseLeave = useCallback(() => {
    useUiStore.getState().setHoverEdge(null);
  }, []);
  // 框选期间不淡化（H2）：拖出来的框会扫过一大片节点
  const onSelectionStart = useCallback(() => {
    useUiStore.getState().setHoverPaused(true);
  }, []);
  const onSelectionEnd = useCallback(() => {
    useUiStore.getState().setHoverPaused(false);
  }, []);

  // -- 双击：空白处开搜索面板，连线中点插一个 reroute，子图节点进去 ----------
  const onDoubleClick = useCallback(
    (e: React.MouseEvent) => {
      const target = e.target as HTMLElement;
      if (!target.classList.contains("react-flow__pane")) return;
      useUiStore.getState().openSearch({
        screen: { x: e.clientX, y: e.clientY },
        flow: screenToFlowPosition({ x: e.clientX, y: e.clientY }),
      });
    },
    [screenToFlowPosition],
  );

  const enterSubgraph = useCallback((nodeId: string) => {
    const current = levelView();
    const node = current.nodes.find((n) => n.id === nodeId);
    const subgraphId = node ? subgraphIdOf(node.op) : null;
    if (!subgraphId) return false;
    useUiStore.getState().enterSubgraph({ nodeId, subgraphId });
    setTimeout(() => void fitView({ duration: 200 }), 60);
    return true;
    // fitView 的引用是稳定的（useReactFlow 返回的都是），列进依赖只是为了 lint
  }, [fitView]);

  const onNodeDoubleClick = useCallback(
    (e: React.MouseEvent, node: { id: string }) => {
      // 双击标题改名的事件先冒泡到这里，标题那一片已经 stopPropagation 过了
      if (enterSubgraph(node.id)) e.stopPropagation();
    },
    [enterSubgraph],
  );

  const insertReroute = useCallback(
    (edgeId: string, screen: { x: number; y: number }) => {
      const graph = useGraphStore.getState();
      const at = screenToFlowPosition(screen);
      const id = graph.addNode(REROUTE_OP, { x: at.x - 40, y: at.y - 20 });
      if (!id) return;
      if (!graph.insertOnEdge(edgeId, id, "in", "out")) graph.deleteNodes([id]);
      else useUiStore.getState().setSelection([id], []);
    },
    [screenToFlowPosition],
  );

  const openPeek = useCallback((edgeId: string, screen: { x: number; y: number }) => {
    const ui = useUiStore.getState();
    const edge = levelView().edges.find((e) => e.id === edgeId);
    if (!edge) return;
    const peek = usePeekStore.getState();
    const existing = findWindowForEdge(peek.windows, edgeId, ui.path);
    if (existing) {
      peek.focus(existing.id);
      flashPeek(wrapper.current, existing.id);
      return;
    }
    const rect = wrapper.current?.getBoundingClientRect();
    const at = { x: screen.x + 12, y: screen.y + 12 };
    const doc = useGraphStore.getState().doc;
    peek.open({
      edgeId,
      path: ui.path,
      from: edge.from,
      screen: rect ? clampPeekScreen(rect, at, PEEK_DEFAULT_SIZE) : at,
      view: defaultViewFor(peekSourceOf(doc, ui.path, edge.from).type),
    });
  }, []);

  const onEdgeDoubleClick = useCallback(
    (e: React.MouseEvent, edge: Edge) => {
      e.stopPropagation();
      openPeek(edge.id, { x: e.clientX, y: e.clientY });
    },
    [openPeek],
  );

  // -- 右键菜单（交互清单 P1 #24 #25 #27 + P2 #31）--------------------------
  const onNodeContextMenu = useCallback((e: React.MouseEvent, node: { id: string }) => {
    e.preventDefault();
    // 右键的节点如果不在选区里，就先把它选上 —— 否则菜单里的动作作用于谁很含糊
    const ui = useUiStore.getState();
    if (!ui.selectedNodes.has(node.id)) ui.setSelection([node.id], []);
    setEdgeMenu(null);
    setMenu({ nodeId: node.id, x: e.clientX, y: e.clientY });
  }, []);

  const onEdgeContextMenu = useCallback((e: React.MouseEvent, edge: Edge) => {
    e.preventDefault();
    setMenu(null);
    setEdgeMenu({ edgeId: edge.id, x: e.clientX, y: e.clientY });
  }, []);

  const closeMenu = useCallback(() => {
    setMenu(null);
    setEdgeMenu(null);
  }, []);

  const onPaneClick = useCallback(() => {
    closeMenu();
    useUiStore.getState().clearAutoHint();
  }, [closeMenu]);

  const menuTargets = useCallback((): string[] => {
    const ui = useUiStore.getState();
    return ui.selectedNodes.size > 0 ? [...ui.selectedNodes] : menu ? [menu.nodeId] : [];
  }, [menu]);

  const menuNode = menu ? view.nodes.find((n) => n.id === menu.nodeId) : undefined;
  const menuSubgraphId = menuNode ? subgraphIdOf(menuNode.op) : null;
  const menuIsLibrary = menuNode?.op.startsWith("lib.") === true;

  // 图级输出（ADR-0017）：outputs 里存的是**展开后**的路径 id，
  // 所以在子图里标输出也说得清是哪一个端口。
  const menuOutputs = useMemo(() => {
    if (!menuNode) return [];
    const op = operatorsById.get(menuNode.op);
    if (!op) return [];
    const declared = doc.outputs ?? {};
    const full = fullId(path, menuNode.id);
    return op.outputs.map((port) => {
      const hit = Object.entries(declared).find(
        ([, o]) => o.node === full && o.port === port.name,
      );
      return { port: port.name, name: hit?.[0] };
    });
  }, [menuNode, operatorsById, doc.outputs, path]);

  const doCompose = useCallback(() => {
    const ids = menuTargets();
    const result = useGraphStore.getState().composeSubgraph(ids);
    if (result) {
      useUiStore.getState().setSelection([result.nodeId], []);
      useUiStore.getState().showToast(`已合成子图（${ids.length} 个节点）`);
    }
    setMenu(null);
  }, [menuTargets]);

  const doDissolve = useCallback(() => {
    if (!menu) return;
    const inlined = useGraphStore.getState().dissolveSubgraph(menu.nodeId);
    if (inlined.length > 0) {
      useUiStore.getState().setSelection(inlined, []);
      useUiStore.getState().showToast(`已解散，内联了 ${inlined.length} 个节点`);
    } else {
      useUiStore.getState().showToast("这个节点不是子图", "warn");
    }
    setMenu(null);
  }, [menu]);

  // -- 从面板拖算子 / 片段进来 ------------------------------------------------
  const onDragOver = useCallback((e: React.DragEvent) => {
    const types = e.dataTransfer.types;
    if (!types.includes(OPERATOR_DND_MIME) && !types.includes(SNIPPET_DND_MIME)) return;
    e.preventDefault();
    e.dataTransfer.dropEffect = "copy";
  }, []);

  const onDrop = useCallback(
    (e: React.DragEvent) => {
      const position = screenToFlowPosition({ x: e.clientX, y: e.clientY });
      const snippetId = e.dataTransfer.getData(SNIPPET_DND_MIME);
      if (snippetId) {
        e.preventDefault();
        insertSnippetById(snippetId, position);
        return;
      }
      const opId = e.dataTransfer.getData(OPERATOR_DND_MIME);
      if (!opId) return;
      e.preventDefault();
      addNodeWithAutoConnect(opId, position);
    },
    [screenToFlowPosition],
  );

  return (
    <div
      className="canvas"
      ref={wrapper}
      onDoubleClick={onDoubleClick}
      onDragOver={onDragOver}
      onDrop={onDrop}
      onClick={() => {
        closeMenu();
        setLibraryFor(null);
      }}
      data-snapping={snapping ? "1" : "0"}
      data-depth={path.length}
      data-layout-moving={override ? "1" : undefined}
    >
      <Breadcrumb />
      <ReactFlow
        nodes={nodes}
        edges={edges}
        nodeTypes={nodeTypes}
        edgeTypes={edgeTypes}
        onNodesChange={onNodesChange}
        onEdgesChange={onEdgesChange}
        onNodeDragStart={onNodeDragStart}
        onNodeDrag={onNodeDrag}
        onNodeDragStop={onNodeDragStop}
        onConnect={onConnect}
        onConnectStart={onConnectStart}
        onConnectEnd={onConnectEnd}
        onReconnectStart={onReconnectStart}
        onReconnect={onReconnect}
        onReconnectEnd={onReconnectEnd}
        edgesReconnectable
        reconnectRadius={CONNECTION_RADIUS}
        connectionRadius={CONNECTION_RADIUS}
        isValidConnection={isValidConnection}
        onSelectionChange={onSelectionChange}
        onNodeContextMenu={onNodeContextMenu}
        onNodeDoubleClick={onNodeDoubleClick}
        onEdgeDoubleClick={onEdgeDoubleClick}
        onEdgeContextMenu={onEdgeContextMenu}
        onPaneClick={onPaneClick}
        onNodeMouseEnter={onNodeMouseEnter}
        onNodeMouseLeave={onNodeMouseLeave}
        onEdgeMouseEnter={onEdgeMouseEnter}
        onEdgeMouseLeave={onEdgeMouseLeave}
        onSelectionStart={onSelectionStart}
        onSelectionEnd={onSelectionEnd}
        // 大图只画视野里的节点（§4）。小图不开：开了之后平移会有一帧空窗。
        onlyRenderVisibleElements={nodes.length > VIRTUALIZE_ABOVE}
        // zoomOnDoubleClick 必须关：d3-zoom 会 stopImmediatePropagation 把双击拦死。
        // deleteKeyCode 不含 Backspace：输入框里退格却删掉节点是经典事故（app/README.md）。
        zoomOnDoubleClick={false}
        deleteKeyCode={DELETE_KEYS}
        multiSelectionKeyCode={MULTI_SELECTION_KEYS}
        selectionKeyCode={null}
        panOnDrag={PAN_BUTTONS}
        selectionOnDrag
        snapToGrid={snapping}
        snapGrid={GRID}
        proOptions={PRO_OPTIONS}
        fitView
        minZoom={0.2}
        maxZoom={2.5}
        connectionLineStyle={CONNECTION_LINE_STYLE}
        defaultEdgeOptions={DEFAULT_EDGE_OPTIONS}
      >
        <Background variant={BackgroundVariant.Dots} gap={16} size={1} color="#2a2f39" />
        <Controls showInteractive={false} />
        <MiniMap
          pannable
          zoomable
          nodeColor="#55607a"
          nodeStrokeColor="#8a94a8"
          maskColor="rgba(20, 22, 26, 0.75)"
        />
        {/* 参考线自绘：React Flow 没有这个能力，而它是「摆得整齐」的全部来源。
            ViewportPortal 让这几条线活在画布坐标系里，跟着平移缩放走。 */}
        <ViewportPortal>
          <div
            className="canvas__guides"
            data-testid="align-guides"
            data-count={guides.length}
          >
            {guides.map((g, i) => (
              <div
                key={`${g.axis}${i}`}
                className={`canvas__guide canvas__guide--${g.axis}`}
                data-axis={g.axis}
                data-at={g.at}
                style={
                  g.axis === "x"
                    ? { left: g.at, top: -10000, height: 20000 }
                    : { top: g.at, left: -10000, width: 20000 }
                }
              />
            ))}
          </div>
          {/* 删除残影（N3）。React 不往里放任何子节点，全由 useCanvasMotion 命令式地挂/摘 */}
          <div className="canvas__ghosts" ref={ghostLayer} />
        </ViewportPortal>
      </ReactFlow>

      <EdgePeekLayer />

      {edgeMenu && (
        <div
          className="ctxmenu"
          style={{ left: edgeMenu.x, top: edgeMenu.y }}
          data-testid="edge-context-menu"
          onClick={(e) => e.stopPropagation()}
        >
          <button
            type="button"
            data-testid="edge-ctx-peek"
            onClick={() => {
              openPeek(edgeMenu.edgeId, { x: edgeMenu.x, y: edgeMenu.y });
              setEdgeMenu(null);
            }}
          >
            查看内容
          </button>
          <button
            type="button"
            data-testid="edge-ctx-reroute"
            onClick={() => {
              insertReroute(edgeMenu.edgeId, { x: edgeMenu.x, y: edgeMenu.y });
              setEdgeMenu(null);
            }}
          >
            在此插入 Reroute
          </button>
          <button
            type="button"
            data-testid="edge-ctx-delete"
            onClick={() => {
              useGraphStore.getState().disconnect([edgeMenu.edgeId]);
              setEdgeMenu(null);
            }}
          >
            删除连线
          </button>
        </div>
      )}

      {menu && (
        <div
          className="ctxmenu"
          style={{ left: menu.x, top: menu.y }}
          data-testid="node-context-menu"
          onClick={(e) => e.stopPropagation()}
        >
          <button
            type="button"
            data-testid="run-to-node"
            disabled={running}
            onClick={() => {
              onRunToNode(menu.nodeId);
              setMenu(null);
            }}
          >
            运行到此节点 <kbd>{keyHint("runToNode")}</kbd>
          </button>
          <button
            type="button"
            data-testid="ctx-compose"
            onClick={doCompose}
          >
            合成子图 <kbd>{keyHint("compose")}</kbd>
          </button>
          {menuSubgraphId && (
            <>
              <button
                type="button"
                data-testid="ctx-enter"
                onClick={() => {
                  enterSubgraph(menu.nodeId);
                  setMenu(null);
                }}
              >
                进入子图
              </button>
              <button type="button" data-testid="ctx-dissolve" onClick={doDissolve}>
                解散子图 <kbd>{keyHint("dissolve")}</kbd>
              </button>
              <button
                type="button"
                data-testid="ctx-save-library"
                onClick={() => {
                  setLibraryFor(menuSubgraphId);
                  setMenu(null);
                }}
              >
                保存到库…
              </button>
            </>
          )}
          {menuIsLibrary && (
            <button
              type="button"
              data-testid="ctx-inline-library"
              onClick={() => {
                useUiStore
                  .getState()
                  .showToast("库算子要先在库目录里编辑，或从原图重新合成", "warn");
                setMenu(null);
              }}
            >
              展开为内联子图
            </button>
          )}
          {menuOutputs.map((o) => (
            <button
              key={o.port}
              type="button"
              data-testid={`ctx-mark-output-${o.port}`}
              data-marked={o.name ? "1" : "0"}
              onClick={() => {
                const graph = useGraphStore.getState();
                if (o.name) {
                  graph.removeGraphOutput(o.name);
                  useUiStore.getState().showToast(`已取消图级输出 ${o.name}`);
                } else {
                  const name = graph.markGraphOutput({
                    node: fullId(path, menu.nodeId),
                    port: o.port,
                  });
                  useUiStore.getState().showToast(`已标为图级输出 ${name}`);
                }
                setMenu(null);
              }}
            >
              {o.name ? `取消输出 ${o.name}` : `标为输出：${o.port}`}
            </button>
          ))}
          <button
            type="button"
            data-testid="ctx-mute"
            onClick={() => {
              const ids = menuTargets();
              useGraphStore.getState().setBypass(ids, !(menuNode?.bypass === true));
              setMenu(null);
            }}
          >
            {menuNode?.bypass ? "取消静音" : "静音"} <kbd>{keyHint("mute")}</kbd>
          </button>
          <button
            type="button"
            data-testid="ctx-collapse"
            onClick={() => {
              const ids = menuTargets();
              useGraphStore.getState().setCollapsed(ids, !(menuNode?.ui?.collapsed === true));
              setMenu(null);
            }}
          >
            {menuNode?.ui?.collapsed ? "展开" : "折叠"} <kbd>{keyHint("collapse")}</kbd>
          </button>
          <button
            type="button"
            data-testid="ctx-layout"
            onClick={() => {
              const ids = new Set(menuTargets());
              const moves = layoutGraph(levelView(), {
                only: ids,
                measured: measured.current,
              });
              // 用户触发的整理才过渡（N4），见 lib/motion.ts 的 withLayoutTransition
              withLayoutTransition(() => useGraphStore.getState().applyLayout(moves));
              setMenu(null);
            }}
          >
            整理选中的布局 <kbd>{keyHint("layout")}</kbd>
          </button>
          <button
            type="button"
            data-testid="ctx-fit"
            onClick={() => {
              void fitView({ duration: 200 });
              setMenu(null);
            }}
          >
            适配视图 <kbd>{keyHint("fitView")}</kbd>
          </button>
        </div>
      )}

      {libraryFor && (
        <LibraryDialog subgraphId={libraryFor} onClose={() => setLibraryFor(null)} />
      )}
    </div>
  );
}
