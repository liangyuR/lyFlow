// 画布。把 React Flow 的交互事件翻译成 graph store 的语义化动作 ——
// 「节点数组第 3 项的 position 变了」没法做撤销，「移动了这几个节点」可以。

import {
  Background,
  BackgroundVariant,
  Controls,
  MiniMap,
  ReactFlow,
  useReactFlow,
  type Connection,
  type Edge,
  type EdgeChange,
  type NodeChange,
  type OnSelectionChangeParams,
} from "@xyflow/react";
import { useCallback, useMemo, useRef, useState } from "react";

import { extractMoves, extractSizes, toReactFlow, type LyNode } from "../lib/mapping";
import { canConnect } from "../lib/typecheck";
import { useExecutionStore } from "../store/execution";
import { useGraphStore } from "../store/graph";
import { useManifestStore } from "../store/manifest";
import { useUiStore } from "../store/ui";

import { OPERATOR_DND_MIME } from "./NodePalette";
import { OperatorNode } from "./OperatorNode";

import "@xyflow/react/dist/style.css";

const nodeTypes = { operator: OperatorNode };

export interface CanvasActions {
  /** 只跑到某个节点（交互清单 P1 #27）。数据通路 M2 已经全部就绪，UI 就这一行。 */
  onRunToNode: (nodeId: string) => void;
}

interface ContextMenuState {
  nodeId: string;
  x: number;
  y: number;
}

export function GraphCanvas({ onRunToNode }: CanvasActions) {
  const doc = useGraphStore((s) => s.doc);
  const operatorsById = useManifestStore((s) => s.operatorsById);
  const typesByName = useManifestStore((s) => s.typesByName);
  const selectedNodes = useUiStore((s) => s.selectedNodes);
  const selectedEdges = useUiStore((s) => s.selectedEdges);

  const { screenToFlowPosition } = useReactFlow();
  const wrapper = useRef<HTMLDivElement>(null);

  // 节点量测尺寸的旁路缓存：不进 GraphDoc，但 MiniMap 需要它才肯画节点。
  // ref 存数据 + 计数器触发重渲染，尺寸稳定后计数器不再变，不会自激。
  const measured = useRef(new Map<string, { width: number; height: number }>());
  const [measuredTick, setMeasuredTick] = useState(0);
  const [menu, setMenu] = useState<ContextMenuState | null>(null);
  const running = useExecutionStore((s) => s.runStatus === "running");

  const ctx = useMemo(() => ({ operatorsById, typesByName }), [operatorsById, typesByName]);

  const { nodes, edges } = useMemo(
    () => toReactFlow(doc, ctx, { nodes: selectedNodes, edges: selectedEdges }, measured.current),
    // measuredTick 是 measured.current 的变更信号，故意作为依赖
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [doc, ctx, selectedNodes, selectedEdges, measuredTick],
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

  // -- 拖动：整段拖动只记一条撤销 -------------------------------------------
  const onNodeDragStart = useCallback(() => {
    useGraphStore.getState().begin();
  }, []);

  const onNodeDragStop = useCallback(() => {
    useGraphStore.getState().commit("移动节点");
  }, []);

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

  // -- 双击空白处开搜索面板（交互清单 P0 #9）--------------------------------
  const onDoubleClick = useCallback(
    (e: React.MouseEvent) => {
      const target = e.target as HTMLElement;
      // 只在画布空白处响应，双击节点标题是重命名（M1 暂未做）
      if (!target.classList.contains("react-flow__pane")) return;
      useUiStore.getState().openSearch({
        screen: { x: e.clientX, y: e.clientY },
        flow: screenToFlowPosition({ x: e.clientX, y: e.clientY }),
      });
    },
    [screenToFlowPosition],
  );

  // -- 右键菜单（Run to node）----------------------------------------------
  const onNodeContextMenu = useCallback((e: React.MouseEvent, node: { id: string }) => {
    e.preventDefault();
    setMenu({ nodeId: node.id, x: e.clientX, y: e.clientY });
  }, []);

  const closeMenu = useCallback(() => setMenu(null), []);

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
    >
      <ReactFlow
        nodes={nodes}
        edges={edges}
        nodeTypes={nodeTypes}
        onNodesChange={onNodesChange}
        onEdgesChange={onEdgesChange}
        onNodeDragStart={onNodeDragStart}
        onNodeDragStop={onNodeDragStop}
        onConnect={onConnect}
        isValidConnection={isValidConnection}
        onSelectionChange={onSelectionChange}
        onNodeContextMenu={onNodeContextMenu}
        onPaneClick={closeMenu}
        // zoomOnDoubleClick 必须关：d3-zoom 会 stopImmediatePropagation 把双击拦死。
        // deleteKeyCode 不含 Backspace：输入框里退格却删掉节点是经典事故（app/README.md）。
        zoomOnDoubleClick={false}
        deleteKeyCode={["Delete"]}
        multiSelectionKeyCode={["Shift", "Control"]}
        selectionKeyCode={null}
        panOnDrag={[1, 2]}
        selectionOnDrag
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
            运行到此节点
          </button>
        </div>
      )}
    </div>
  );
}
