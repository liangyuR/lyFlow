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
  /** "diagnostic" 或 "migration"。老 core 不带这个字段，缺省按普通诊断。 */
  kind?: "diagnostic" | "migration";
}

/** 迁移动作（ADR-0008）：C++ 只说「该改成什么」，写回 doc 是 applyMigrations 的事。
 *  params 是**完整**的参数对象而不是补丁 —— 改名参数没法用补丁表达。 */
export interface MigrationAction extends GraphDiagnostic {
  kind: "migration";
  op: string;
  opVersion: string;
  params: Record<string, unknown>;
  notes?: string[];
}

export function isMigration(d: GraphDiagnostic): d is MigrationAction {
  return d.kind === "migration";
}

/** `plan_graph` 的每节点结果。缓存判定的唯一权威在 C++（ADR-0007）。 */
export interface PlanNode {
  nodeId: string;
  cacheKey: string;
  cached: boolean;
  level: number;
  /** 至少一个直接上游没有缓存 —— 这个节点这次一定得重算。 */
  upstreamMissing: boolean;
  bypass: boolean;
}

/** `cache_stats` 的返回。 */
export interface CacheStats {
  entries: number;
  bytes: number;
  budgetBytes: number;
  hits: number;
  misses: number;
  evictions: number;
}

/** 非点云输出的值。`kind` 就是端口类型名，其余字段随类型而定（core 的 Data::valueJson）。
 *  坐标一律是米；Measurement 的 value 带自己的 unit。 */
export interface OutputValue {
  kind: string;
  /** Box2D（两角）。Tensor 复用这两个键，但那里是标量统计量。 */
  min?: [number, number] | number | null;
  max?: [number, number] | number | null;
  /** Line2D */
  point?: [number, number];
  dir?: [number, number];
  hasSegment?: boolean;
  start?: [number, number];
  end?: [number, number];
  /** Circle2D */
  center?: [number, number];
  radius?: number;
  /** Point2D */
  p?: [number, number];
  /** Measurement。value 为 null 表示没测出来（C++ 侧的非有限值）。 */
  value?: number | null;
  ok?: boolean;
  unit?: string;
  message?: string;
  verdict?: string;
  nominal?: number;
  upper?: number;
  lower?: number;
  /** Record */
  type?: string;
  data?: Record<string, unknown>;
  /** Plane / Transform */
  normal?: number[];
  d?: number;
  m?: number[];
  /** Tensor。张量本身不进 IPC，只有形状与统计量。 */
  shape?: number[];
  count?: number;
  mean?: number | null;
}

export interface OutputStat {
  port: string;
  type: string;
  elementCount: number;
  /** 非点云输出才有。点云与 Indices 走二进制通道（ADR-0006）。 */
  value?: OutputValue;
}

export interface NodeStats {
  elementCount?: number;
  byteSize?: number;
  /** state=skipped 是因为命中缓存。与 bypassed 互斥。 */
  cached?: boolean;
  /** state=skipped 是因为节点被静音，输出由输入透传而来。 */
  bypassed?: boolean;
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
  /** 本次实际用了几个 worker（E2）。 */
  maxParallel?: number;
  plan?: string[];
  targets?: string[];
  /** 编译结果。精确 stale 与「将重算 N 个节点」提示靠它（ADR-0007）。 */
  nodes?: { id: string; cacheKey: string; level: number; bypass?: boolean }[];
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
  value?: OutputValue;
}

// 二进制点云（ADR-0006） ------------------------------------------------------

/** 'LYPC' 小端。 */
export const CLOUD_MAGIC = 0x4350594c;
export const CLOUD_HAS_INTENSITY = 1;
export const CLOUD_HAS_NORMALS = 2;

export interface CloudPayload {
  pointCount: number;
  totalPoints: number;
  /** [minX, minY, minZ, maxX, maxY, maxZ]，用**全量**点云算的。 */
  bounds: Float32Array;
  /** 3n 个 float，可以直接喂给 BufferAttribute。 */
  xyz: Float32Array;
  intensity: Float32Array | null;
  /** 3n 个 float，或 null。法线着色靠它（M3 尾巴 c）。 */
  normals: Float32Array | null;
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
  // 通道按 flags 的位序依次排在坐标后面：先 intensity，再 normals
  let offset = xyzOffset + pointCount * 12;
  let intensity: Float32Array | null = null;
  if (flags & CLOUD_HAS_INTENSITY) {
    intensity = new Float32Array(buffer, offset, pointCount);
    offset += pointCount * 4;
  }
  let normals: Float32Array | null = null;
  if (flags & CLOUD_HAS_NORMALS) {
    normals = new Float32Array(buffer, offset, pointCount * 3);
    offset += pointCount * 12;
  }

  return { pointCount, totalPoints, bounds, xyz, intensity, normals };
}
