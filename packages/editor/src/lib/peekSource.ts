import { useMemo } from "react";

import { augmentOperators, levelOf, resolveOutput, type SubPath } from "./subgraph";
import { useExecutionStore, type RunPhase } from "../store/execution";
import { useGraphStore } from "../store/graph";
import { useManifestStore } from "../store/manifest";
import type { PeekView } from "../store/peek";
import type { NodeState, OutputStat } from "../types/execution";
import type { OperatorDesc } from "../types/manifest";
import type { GraphDoc, PortRef } from "../types/graph";

export interface PeekSource {
  resolved: { nodeId: string; port: string } | null;
  runId: string | null;
  type: string | null;
  stat: OutputStat | undefined;
  status: string | null;
  label: string;
}

export function defaultViewFor(type: string | null): PeekView {
  switch (type) {
    case "PointCloud":
      return "cloud3d";
    case "Box2D":
    case "Line2D":
    case "Circle2D":
    case "Point2D":
      return "cloud2d";
    case "Tensor":
      return "tensor";
    case "Indices":
      return "indices";
    default:
      return "value";
  }
}

const VIEWS_POINT_CLOUD: PeekView[] = ["cloud3d", "cloud2d", "value"];
const VIEWS_SHAPE_2D: PeekView[] = ["cloud2d", "value"];
const VIEWS_TENSOR: PeekView[] = ["tensor", "value"];
const VIEWS_INDICES: PeekView[] = ["indices"];
const VIEWS_VALUE: PeekView[] = ["value"];

export function viewsFor(type: string | null): PeekView[] {
  switch (type) {
    case "PointCloud":
      return VIEWS_POINT_CLOUD;
    case "Box2D":
    case "Line2D":
    case "Circle2D":
    case "Point2D":
      return VIEWS_SHAPE_2D;
    case "Tensor":
      return VIEWS_TENSOR;
    case "Indices":
      return VIEWS_INDICES;
    default:
      return VIEWS_VALUE;
  }
}

function build(
  doc: GraphDoc,
  path: SubPath,
  from: PortRef,
  operatorsById: ReadonlyMap<string, OperatorDesc>,
  resolved: { nodeId: string; port: string } | null,
  runId: string | null,
  runStatus: RunPhase,
  leafState: NodeState | undefined,
  leafOutputs: readonly OutputStat[] | undefined,
): PeekSource {
  const node = levelOf(doc, path).nodes.find((n) => n.id === from.node);
  const label = node?.ui?.title || from.node;
  const ops = augmentOperators(operatorsById, doc.subgraphs);
  const declared = node
    ? (ops.get(node.op)?.outputs.find((o) => o.name === from.port)?.type ?? null)
    : null;
  const stat = resolved ? leafOutputs?.find((o) => o.port === resolved.port) : undefined;
  const type = stat?.type ?? declared;

  let status: string | null = null;
  if (!resolved) status = "这个算子的内部结果查不到（库算子的定义在库文件里）";
  else if (!runId || runStatus === "idle") status = "未运行";
  else if (leafState === "error") status = "该节点运行出错";
  else if (leafState !== "done" && leafState !== "skipped")
    status = leafState === "running" ? "正在计算…" : "该节点尚未产出结果";
  else if (!stat) status = "该节点尚未产出结果";
  else if (type === null || type === "Any") status = "未运行";

  return { resolved, runId, type, stat, status, label };
}

export function peekSourceOf(doc: GraphDoc, path: SubPath, from: PortRef): PeekSource {
  const exec = useExecutionStore.getState();
  const resolved = resolveOutput(doc, path, from.node, from.port);
  const leaf = resolved ? exec.nodes.get(resolved.nodeId) : undefined;
  return build(
    doc,
    path,
    from,
    useManifestStore.getState().operatorsById,
    resolved,
    exec.runId,
    exec.runStatus,
    leaf?.state,
    leaf?.stats?.outputs,
  );
}

export function usePeekSource(path: SubPath, from: PortRef): PeekSource {
  const doc = useGraphStore((s) => s.doc);
  const operatorsById = useManifestStore((s) => s.operatorsById);
  const runId = useExecutionStore((s) => s.runId);
  const runStatus = useExecutionStore((s) => s.runStatus);

  const resolved = useMemo(
    () => resolveOutput(doc, path, from.node, from.port),
    [doc, path, from],
  );

  const leafId = resolved?.nodeId;
  const leafState = useExecutionStore((s) => (leafId ? s.nodes.get(leafId)?.state : undefined));
  const leafOutputs = useExecutionStore((s) =>
    leafId ? s.nodes.get(leafId)?.stats?.outputs : undefined,
  );

  return useMemo(
    () =>
      build(
        doc,
        path,
        from,
        operatorsById,
        resolved,
        runId,
        runStatus,
        leafState,
        leafOutputs,
      ),
    [doc, path, from, operatorsById, resolved, runId, runStatus, leafState, leafOutputs],
  );
}
