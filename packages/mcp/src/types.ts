export interface PortType {
  name: string;
  color?: string;
  castableTo?: string[];
  doc?: string;
}

export interface OperatorDesc {
  id: string;
  version?: string;
  pack?: string;
  aliases?: string[];
  label: string;
  category: string;
  keywords?: string[];
  doc?: string;
  inputs?: unknown[];
  outputs?: unknown[];
  params?: unknown[];
  capabilities?: Record<string, unknown>;
}

export interface BundleDesc {
  kind: string;
  label?: string;
  doc?: string;
  pack?: string;
  fields: { name: string; type: string; doc?: string }[];
}

export interface ManifestBundle {
  schemaVersion: number;
  generatedBy?: string;
  types: PortType[];
  /** Bundle 声明（m8-plan L2）：端口类型 `Bundle<kind>` 的字段表。 */
  bundles?: BundleDesc[];
  operators: OperatorDesc[];
  importers?: unknown[];
}

export interface CoreInfo {
  version?: string;
  operatorCount?: number;
  typeCount?: number;
  generation?: number;
  hotReload?: boolean;
}

export interface GraphNode {
  id: string;
  op: string;
  opVersion?: string;
  params?: Record<string, unknown>;
  [key: string]: unknown;
}

export interface GraphDoc {
  schemaVersion?: number;
  id?: string;
  name?: string;
  nodes: GraphNode[];
  edges?: unknown[];
  outputs?: Record<string, unknown>;
  [key: string]: unknown;
}

export interface Diagnostic {
  phase?: string;
  code?: string;
  message?: string;
  nodeId?: string;
  severity?: string;
  paramPath?: string;
  portName?: string;
  [key: string]: unknown;
}

export interface OutputValue {
  kind?: string;
  value?: number | null;
  unit?: string;
  shape?: number[];
  count?: number;
  min?: unknown;
  max?: unknown;
  mean?: number | null;
  [key: string]: unknown;
}

export interface OutputInfo {
  port: string;
  type: string;
  elementCount?: number;
  byteSize?: number;
  value?: OutputValue;
}

export interface RunOutput {
  node: string;
  port: string;
  type: string;
  elementCount?: number;
  byteSize?: number;
  value?: OutputValue;
  missing?: boolean;
}

export type RunOutputs = Record<string, RunOutput>;

export interface NodeStats {
  elementCount?: number;
  byteSize?: number;
  cached?: boolean;
  bypassed?: boolean;
  provided?: boolean;
  outputsAvailable?: boolean;
  reason?: string;
  outputs?: OutputInfo[];
}

export interface ExecutionEvent {
  kind: string;
  runId?: string;
  seq?: number;
  nodeId?: string;
  state?: string;
  status?: string;
  durationMs?: number;
  stats?: NodeStats;
  error?: Diagnostic;
  errors?: Diagnostic[];
  level?: string;
  message?: string;
  [key: string]: unknown;
}

export interface RunEnvelope {
  doc: GraphDoc;
  graphPath: string | null;
  targets?: string[] | null;
  mode?: string;
  previewMaxPoints?: number | null;
  previewBudgetMs?: number | null;
  sceneId?: string | null;
  /** 顶层图参数的取值 `{名字: 值}`（http-transport.md「params」）。带 recipe 的 run_graph 由配方合成。 */
  params?: Record<string, unknown> | null;
}
