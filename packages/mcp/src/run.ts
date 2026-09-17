import type { Diagnostic, ExecutionEvent, OutputValue, RunOutputs } from "./types.js";

/** core 产出的 run summary（ADR-0022）。这一份不是 MCP 算出来的 ——
 *  它整个来自 run_finished 事件，与 `lyflow_run_summary(runId)` 是同一个对象。
 *  消费方不得从 node_state 重建它。 */
export interface CoreNodeSummary {
  state: string;
  /** state=error/cancelled 时的错误码。 */
  code?: string;
  /** state=skipped 的机器可读原因，目前只有 not_demanded。 */
  reason?: string;
  durationMs?: number;
  cached?: boolean;
  bypassed?: boolean;
  provided?: boolean;
  outputsAvailable?: boolean;
}

/** 图级输出的三态（ADR-0022 H3）：有值 / 本来就没有 / 本该有但崩了。 */
export interface CoreOutputSummary {
  state: string;
  node: string;
  port: string;
  type?: string;
  elementCount?: number;
  value?: OutputValue;
  /** state=inactive 的原因。 */
  reason?: string;
  /** state=failed 时回溯到的最近的出错节点。 */
  from?: string;
  code?: string;
}

export interface CoreRunSummary {
  runId?: string;
  /** ok | degraded | failed（H2）。与 run_finished.status 不是一回事。 */
  status: string;
  durationMs?: number;
  nodes: Record<string, CoreNodeSummary>;
  outputs: Record<string, CoreOutputSummary>;
  /** 全图每一个 FallbackChoice（H4）。 */
  decisions: Record<string, Record<string, unknown>>;
  contractViolations: unknown[];
}

/** 从事件流里把 core 那份 summary 取出来。老 core（ABI < v9）没有它，返回 null。 */
export function coreSummary(events: ExecutionEvent[]): CoreRunSummary | null {
  for (let i = events.length - 1; i >= 0; i -= 1) {
    if (events[i]?.kind !== "run_finished") continue;
    const s = events[i]?.["summary"];
    return s && typeof s === "object" ? (s as CoreRunSummary) : null;
  }
  return null;
}

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
