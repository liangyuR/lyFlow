import type { Diagnostic, ExecutionEvent, RunOutputs } from "./types.js";

export interface NodeSummary {
  id: string;
  state: string;
  outputsAvailable: boolean | null;
  durationMs: number | null;
  elementCount: number | null;
  errors: Diagnostic[];
}

export interface RunSummary {
  status: string;
  durationMs: number | null;
  nodes: NodeSummary[];
  diagnostics: Diagnostic[];
  eventCount: number;
}

export interface GraphOutputSummary {
  node: string;
  port: string;
  type: string;
  elementCount?: number;
  value?: unknown;
  missing?: boolean;
}

export function summarizeRun(events: ExecutionEvent[]): RunSummary {
  const nodes = new Map<string, NodeSummary>();
  let status = "unknown";
  let durationMs: number | null = null;
  const diagnostics: Diagnostic[] = [];

  for (const e of events) {
    if (e.kind === "node_state" && typeof e.nodeId === "string") {
      const errors: Diagnostic[] = [];
      if (Array.isArray(e.errors)) errors.push(...e.errors);
      else if (e.error) errors.push(e.error);
      nodes.set(e.nodeId, {
        id: e.nodeId,
        state: e.state ?? "unknown",
        outputsAvailable: e.stats?.outputsAvailable ?? null,
        durationMs: e.durationMs ?? null,
        elementCount: e.stats?.elementCount ?? null,
        errors,
      });
      continue;
    }
    if (e.kind === "run_finished") {
      status = e.status ?? "unknown";
      durationMs = e.durationMs ?? null;
      if (e.error) diagnostics.push(e.error);
    }
  }

  return {
    status,
    durationMs,
    nodes: [...nodes.values()],
    diagnostics,
    eventCount: events.length,
  };
}

export function summarizeOutputs(outputs: RunOutputs): Record<string, GraphOutputSummary> {
  const out: Record<string, GraphOutputSummary> = {};
  for (const [name, o] of Object.entries(outputs ?? {})) {
    const item: GraphOutputSummary = { node: o.node, port: o.port, type: o.type };
    if (typeof o.elementCount === "number") item.elementCount = o.elementCount;
    if (o.value !== undefined) item.value = o.value;
    if (o.missing) item.missing = true;
    out[name] = item;
  }
  return out;
}

export function parseDiagnostics(text: string): Diagnostic[] | null {
  try {
    const value = JSON.parse(text) as unknown;
    if (Array.isArray(value)) return value as Diagnostic[];
    if (value && typeof value === "object") {
      const inner = (value as { error?: unknown }).error;
      if (typeof inner === "string") return parseDiagnostics(inner);
    }
  } catch {
    return null;
  }
  return null;
}
