// ExecutionEvent 的 TypeScript 镜像，契约在 schema/execution-event.schema.json。
// 与 manifest 一样，这里只是给编辑器用的视图；改动顺序永远是 schema → C++ → 这里。

export type NodeState =
  | "idle"
  | "pending"
  | "running"
  | "done"
  | "error"
  | "cancelled"
  | "skipped";

export type RunStatus = "ok" | "error" | "cancelled";

export type Phase = "validate" | "compile" | "execute";

export interface Diagnostic {
  phase: Phase;
  /** 机器可读短码。**不要穷举它做逻辑分支** —— core 随时可能加新的 code，
   *  前端把未知 code 当通用错误显示就行（schema 里也是这么写的）。 */
  code: string;
  message: string;
  /** 出错的参数名。红框直接标到那个输入框上（交互清单 P0 #15）。 */
  paramPath?: string;
  portName?: string;
}

/** `lyflow_validate` 返回的诊断，比事件里的多一个 nodeId 和 severity。 */
export interface GraphDiagnostic extends Diagnostic {
  nodeId: string;
  severity: "error" | "warning";
}

export interface OutputStat {
  port: string;
  type: string;
  elementCount: number;
}

export interface NodeStats {
  elementCount?: number;
  byteSize?: number;
  outputs?: OutputStat[];
}

interface EventBase {
  schemaVersion: number;
  runId: string;
  seq: number;
  at?: string;
}

export interface RunStartedEvent extends EventBase {
  kind: "run_started";
  nodeCount?: number;
  plan?: string[];
  targets?: string[];
  /** 编译结果。M3 的精确 stale 与「将重算 N 个节点」提示靠它。 */
  nodes?: { id: string; cacheKey: string; level: number }[];
}

export interface NodeStateEvent extends EventBase {
  kind: "node_state";
  nodeId: string;
  state: NodeState;
  durationMs?: number;
  stats?: NodeStats;
  error?: Diagnostic;
  errors?: Diagnostic[];
}

export interface NodeProgressEvent extends EventBase {
  kind: "node_progress";
  nodeId: string;
  progress: number;
  message?: string;
}

export interface RunFinishedEvent extends EventBase {
  kind: "run_finished";
  status: RunStatus;
  durationMs?: number;
  error?: Diagnostic;
}

export interface LogEvent extends EventBase {
  kind: "log";
  level: "debug" | "info" | "warn" | "error";
  nodeId?: string;
  message: string;
}

export type ExecutionEvent =
  | RunStartedEvent
  | NodeStateEvent
  | NodeProgressEvent
  | RunFinishedEvent
  | LogEvent;

/** `get_output_info` 的返回。 */
export interface OutputInfo {
  port: string;
  type: string;
  elementCount: number;
  byteSize: number;
}

// 二进制点云（ADR-0006） ------------------------------------------------------

/** 'LYPC' 小端。 */
export const CLOUD_MAGIC = 0x4350594c;
export const CLOUD_HAS_INTENSITY = 1;

export interface CloudPayload {
  pointCount: number;
  totalPoints: number;
  /** [minX, minY, minZ, maxX, maxY, maxZ]，用**全量**点云算的。 */
  bounds: Float32Array;
  /** 3n 个 float，可以直接喂给 BufferAttribute。 */
  xyz: Float32Array;
  intensity: Float32Array | null;
}

/** 解析二进制点云。用视图而不是拷贝：一百万点是 12MB，多拷一次就是多 12MB
 *  和一次明显的卡顿。`buffer` 本身来自 IPC，之后不会有人改它。 */
export function decodeCloud(buffer: ArrayBuffer): CloudPayload {
  if (buffer.byteLength < 40) {
    throw new Error(`点云载荷太短（${buffer.byteLength} 字节），多半不是点云数据`);
  }
  const header = new DataView(buffer);
  const magic = header.getUint32(0, true);
  if (magic !== CLOUD_MAGIC) {
    // magic 不对通常意味着上游返回的是一段错误文本。没有这个检查的话，
    // 那段文本会被当成坐标画出来，然后没人想得到去怀疑 IPC。
    throw new Error(`点云载荷的 magic 不对（0x${magic.toString(16)}），响应不是点云`);
  }
  const pointCount = header.getUint32(4, true);
  const totalPoints = header.getUint32(8, true);
  const flags = header.getUint32(12, true);

  const bounds = new Float32Array(buffer, 16, 6);
  const xyzOffset = 40;
  const xyz = new Float32Array(buffer, xyzOffset, pointCount * 3);
  const intensity =
    flags & CLOUD_HAS_INTENSITY
      ? new Float32Array(buffer, xyzOffset + pointCount * 12, pointCount)
      : null;

  return { pointCount, totalPoints, bounds, xyz, intensity };
}
