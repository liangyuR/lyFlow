// 画布。把 React Flow 的交互事件翻译成 graph store 的语义化动作 ——
// 「节点数组第 3 项的 position 变了」没法做撤销，「移动了这几个节点」可以。

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
import { useCallback, useMemo, useRef, useState } from "react";

import { layoutGraph } from "../lib/layout";
import {
  distanceToSegment,
  extractMoves,
  extractSizes,
  segmentHitsRect,
  toReactFlow,
  type LyNode,
} from "../lib/mapping";
import { canConnect, compatibleSources, compatibleTargets, inferAnyTypes } from "../lib/typecheck";
import { keyHint } from "../lib/keymap";
import { useExecutionStore } from "../store/execution";
import { useGraphStore } from "../store/graph";
import { useManifestStore } from "../store/manifest";
import { useUiStore } from "../store/ui";
import type { GraphDoc } from "../types/graph";

import { OPERATOR_DND_MIME } from "./NodePalette";
import { OperatorNode } from "./OperatorNode";

import "@xyflow/react/dist/style.css";

const nodeTypes = { operator: OperatorNode };

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

export interface CanvasActions {
  /** 只跑到某个节点（交互清单 P1 #27）。 */
  onRunToNode: (nodeId: string) => void;
}

interface ContextMenuState {
  nodeId: string;
  x: number;
  y: number;
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

export function GraphCanvas({ onRunToNode }: CanvasActions) {
  const doc = useGraphStore((s) => s.doc);
  const operatorsById = useManifestStore((s) => s.operatorsById);
  const typesByName = useManifestStore((s) => s.typesByName);
  const selectedNodes = useUiStore((s) => s.selectedNodes);
  const selectedEdges = useUiStore((s) => s.selectedEdges);

  const { screenToFlowPosition, fitView } = useReactFlow();
  const wrapper = useRef<HTMLDivElement>(null);

  // 节点量测尺寸的旁路缓存：不进 GraphDoc，但 MiniMap 需要它才肯画节点。
  // ref 存数据 + 计数器触发重渲染，尺寸稳定后计数器不再变，不会自激。
  const measured = useRef(new Map<string, { width: number; height: number }>());
  const [measuredTick, setMeasuredTick] = useState(0);
  const [menu, setMenu] = useState<ContextMenuState | null>(null);
  const [guides, setGuides] = useState<Guide[]>([]);
  const [snapping, setSnapping] = useState(true);
  const running = useExecutionStore((s) => s.runStatus === "running");
  const dragged = useRef<string | null>(null);
  /** onReconnect 有没有接住这次拖动。onReconnectEnd 的第四个参数各版本形态不一，
   *  与其猜它，不如自己记一笔 —— 猜错的后果是把一条好边直接删掉。 */
  const reconnected = useRef(false);

  const ctx = useMemo(() => ({ operatorsById, typesByName }), [operatorsById, typesByName]);
  const anyTypes = useMemo(() => inferAnyTypes(ctx, doc), [ctx, doc]);

  const { nodes, edges } = useMemo(
    () =>
      toReactFlow(doc, ctx, { nodes: selectedNodes, edges: selectedEdges }, measured.current, anyTypes),
    // measuredTick 是 measured.current 的变更信号，故意作为依赖
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [doc, ctx, selectedNodes, selectedEdges, measuredTick, anyTypes],
  );

  // -- 节点变更 ------------------------------------------------------------
  const onNodesChange = useCallback((changes: NodeChange<LyNode>[]) => {
    const graph = useGraphStore.getState();

    const moves = extractMoves(changes as { type: string; id: string; position?: { x: number; y: number } }[]);
    if (moves.length > 0) graph.moveNodes(moves);

    const removed = changes.filter((c) => c.type === "remove").map((c) => c.id);
    if (removed.length > 0) graph.deleteNodes(removed);

    // 量测尺寸：不进 GraphDoc，但要留在旁路缓存里供 MiniMap 使用。
    // 只在尺寸真的变了时才 bump，否则会和 React Flow 的重新量测互相触发。
    let sizeChanged = false;
    for (const s of extractSizes(
      changes as { type: string; id: string; dimensions?: { width: number; height: number } }[],
    )) {
      const prev = measured.current.get(s.id);
      if (!prev || prev.width !== s.width || prev.height !== s.height) {
        measured.current.set(s.id, { width: s.width, height: s.height });
        sizeChanged = true;
      }
    }
    if (sizeChanged) setMeasuredTick((t) => t + 1);

    // select / dragging 是 UI 运行时状态，不进 GraphDoc。
    // 选中由 onSelectionChange 统一处理，这里忽略。
  }, []);

  const onEdgesChange = useCallback((changes: EdgeChange<Edge>[]) => {
    const removed = changes.filter((c) => c.type === "remove").map((c) => c.id);
    if (removed.length > 0) useGraphStore.getState().disconnect(removed);
  }, []);

  // -- 拖动：整段拖动只记一条撤销 + 对齐参考线（交互清单 P1 #22）-------------
  const onNodeDragStart: OnNodeDrag<LyNode> = useCallback((_e, node) => {
    dragged.current = node.id;
    useGraphStore.getState().begin();
  }, []);

  const onNodeDrag: OnNodeDrag<LyNode> = useCallback((e, node) => {
    // Shift 临时关掉吸附与参考线：需要摆一个「差一点」的位置时总得有出路
    const shift = "shiftKey" in e && e.shiftKey;
    setSnapping(!shift);
    if (shift) {
      setGuides([]);
      return;
    }
    const doc = useGraphStore.getState().doc;
    const me = rectOf(doc, node.id, measured.current);
    const found: Guide[] = [];
    const pairs = (axis: "x" | "y", mine: number[], theirs: number[]) => {
      for (let i = 0; i < mine.length; i += 1) {
        const a = mine[i] as number;
        const b = theirs[i] as number;
        if (Math.abs(a - b) < GUIDE_TOLERANCE) found.push({ axis, at: b });
      }
    };
    for (const other of doc.nodes) {
      if (other.id === node.id) continue;
      const r = rectOf(doc, other.id, measured.current);
      pairs("x", [me.x, me.x + me.w / 2, me.x + me.w], [r.x, r.x + r.w / 2, r.x + r.w]);
      pairs("y", [me.y, me.y + me.h / 2, me.y + me.h], [r.y, r.y + r.h / 2, r.y + r.h]);
    }
    setGuides(found.slice(0, 4));
  }, []);

  /** 拖动结束时对**单个**选中节点做边命中：多选时插入谁到中间是没有答案的。 */
  const insertOnHoveredEdge = useCallback(
    (nodeId: string) => {
      const graph = useGraphStore.getState();
      const doc = graph.doc;
      const me = rectOf(doc, nodeId, measured.current);
      const center = { x: me.x + me.w / 2, y: me.y + me.h / 2 };
      const op = useManifestStore.getState().operatorsById.get(
        doc.nodes.find((n) => n.id === nodeId)?.op ?? "",
      );
      if (!op) return false;
      // 已经有连线的节点不参与插入：它多半只是被挪到了别的连线附近
      if (doc.edges.some((e) => e.from.node === nodeId || e.to.node === nodeId)) return false;

      for (const edge of doc.edges) {
        const a = rectOf(doc, edge.from.node, measured.current);
        const b = rectOf(doc, edge.to.node, measured.current);
        const from = { x: a.x + a.w, y: a.y + a.h / 2 };
        const to = { x: b.x, y: b.y + b.h / 2 };
        const hit =
          segmentHitsRect(from, to, me) || distanceToSegment(center, from, to) <= EDGE_HIT_RADIUS;
        if (!hit) continue;

        // 有且仅有一对兼容端口时才插入。多于一对就没有唯一解，宁可不动。
        const without: GraphDoc = { ...doc, edges: doc.edges.filter((e) => e.id !== edge.id) };
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
    }
  }, []);

  /** 拖线开始：把「哪些端口能落」算一次存进 ui store，端口自己去读（P1 #20）。 */
  const onConnectStart = useCallback(
    (
      _e: unknown,
      params: { nodeId: string | null; handleId: string | null; handleType: string | null },
    ) => {
      if (!params.nodeId || !params.handleId) return;
      const doc = useGraphStore.getState().doc;
      const fromOutput = params.handleType === "source";
      const ref = { node: params.nodeId, port: params.handleId };
      const compatible = fromOutput
        ? compatibleTargets(ctx, doc, ref)
        : compatibleSources(ctx, doc, ref);
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
      const from = graph.doc.edges.find((e) => e.id === edge.id)?.from;
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
        useGraphStore.getState().doc,
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

  // -- 双击：空白处开搜索面板，连线中点插一个 reroute -----------------------
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

  const onEdgeDoubleClick = useCallback(
    (e: React.MouseEvent, edge: Edge) => {
      e.stopPropagation();
      const graph = useGraphStore.getState();
      const at = screenToFlowPosition({ x: e.clientX, y: e.clientY });
      const id = graph.addNode(REROUTE_OP, { x: at.x - 40, y: at.y - 20 });
      if (!id) return;
      if (!graph.insertOnEdge(edge.id, id, "in", "out")) graph.deleteNodes([id]);
      else useUiStore.getState().setSelection([id], []);
    },
    [screenToFlowPosition],
  );

  // -- 右键菜单（交互清单 P1 #24 #25 #27）----------------------------------
  const onNodeContextMenu = useCallback((e: React.MouseEvent, node: { id: string }) => {
    e.preventDefault();
    // 右键的节点如果不在选区里，就先把它选上 —— 否则菜单里的动作作用于谁很含糊
    const ui = useUiStore.getState();
    if (!ui.selectedNodes.has(node.id)) ui.setSelection([node.id], []);
    setMenu({ nodeId: node.id, x: e.clientX, y: e.clientY });
  }, []);

  const closeMenu = useCallback(() => setMenu(null), []);

  const menuTargets = useCallback((): string[] => {
    const ui = useUiStore.getState();
    return ui.selectedNodes.size > 0 ? [...ui.selectedNodes] : menu ? [menu.nodeId] : [];
  }, [menu]);

  const menuNode = menu ? doc.nodes.find((n) => n.id === menu.nodeId) : undefined;

  // -- 从面板拖算子进来 -----------------------------------------------------
  const onDragOver = useCallback((e: React.DragEvent) => {
    if (!e.dataTransfer.types.includes(OPERATOR_DND_MIME)) return;
    e.preventDefault();
    e.dataTransfer.dropEffect = "copy";
  }, []);

  const onDrop = useCallback(
    (e: React.DragEvent) => {
      const opId = e.dataTransfer.getData(OPERATOR_DND_MIME);
      if (!opId) return;
      e.preventDefault();
      const position = screenToFlowPosition({ x: e.clientX, y: e.clientY });
      const id = useGraphStore.getState().addNode(opId, position);
      if (id) useUiStore.getState().setSelection([id], []);
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
      onClick={closeMenu}
      data-snapping={snapping ? "1" : "0"}
    >
      <ReactFlow
        nodes={nodes}
        edges={edges}
        nodeTypes={nodeTypes}
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
        onEdgeDoubleClick={onEdgeDoubleClick}
        onPaneClick={closeMenu}
        // zoomOnDoubleClick 必须关：d3-zoom 会 stopImmediatePropagation 把双击拦死。
        // deleteKeyCode 不含 Backspace：输入框里退格却删掉节点是经典事故（app/README.md）。
        zoomOnDoubleClick={false}
        deleteKeyCode={["Delete"]}
        multiSelectionKeyCode={["Shift", "Control"]}
        selectionKeyCode={null}
        panOnDrag={[1, 2]}
        selectionOnDrag
        snapToGrid={snapping}
        snapGrid={GRID}
        proOptions={{ hideAttribution: false }}
        fitView
        minZoom={0.2}
        maxZoom={2.5}
        connectionLineStyle={{ stroke: "#4a9eff", strokeWidth: 2 }}
        defaultEdgeOptions={{ type: "default" }}
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
        </ViewportPortal>
      </ReactFlow>

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
              const moves = layoutGraph(useGraphStore.getState().doc, {
                only: ids,
                measured: measured.current,
              });
              useGraphStore.getState().applyLayout(moves);
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
    </div>
  );
}
