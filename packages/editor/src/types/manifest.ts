// OperatorManifest 的 TypeScript 镜像，必须与 schema/operator-manifest.schema.json
// 一致。改动顺序永远是：schema → C++ → 这里。

export type ParamType =
  | "bool" | "int" | "float"
  | "vec2f" | "vec3f" | "vec4f"
  | "enum" | "flags"
  | "string" | "text" | "path"
  | "color" | "transform" | "curve";

export interface PortType {
  name: string;
  /** 端口与连线着色。让类型系统「看得见」。 */
  color: string;
  castableTo?: string[];
  doc?: string;
}

export interface Port {
  name: string;
  type: string;
  label?: string;
  doc?: string;
  /** 仅对 inputs 有意义，缺省为 true。 */
  required?: boolean;
  /** 仅对 inputs 有意义（ADR-0016）。上游失败时本节点收到一个 Error 值而不是被连坐。 */
  acceptsError?: boolean;
  /** 仅对 inputs 有意义（ADR-0016）。这条边的上游闭包只有被 demand 时才调度。 */
  lazy?: boolean;
}

/** 一种可导入的外部格式（ADR-0017）。`kind` 是传给 import 的第一个参数。 */
export interface Importer {
  kind: string;
  label: string;
  doc?: string;
  pack?: string;
}

export interface EnumOption {
  value: string | number;
  label: string;
  doc?: string;
}

/** 参数联动条件。只支持对同节点其他参数的等值/包含判断。 */
export interface Condition {
  param: string;
  eq?: unknown;
  ne?: unknown;
  in?: unknown[];
}

export interface FileFilter {
  name: string;
  extensions: string[];
}

export interface Param {
  name: string;
  type: ParamType;
  label?: string;
  doc?: string;
  default: unknown;
  group?: string;
  advanced?: boolean;

  min?: number;
  max?: number;
  softMin?: number;
  softMax?: number;
  step?: number;
  unit?: string;

  componentLabels?: string[];
  options?: EnumOption[];
  placeholder?: string;
  pattern?: string;
  rows?: number;
  filters?: FileFilter[];
  mode?: "open" | "save" | "dir";
  alpha?: boolean;

  visibleWhen?: Condition;
  enabledWhen?: Condition;
}

export interface Capabilities {
  cancellable?: boolean;
  previewable?: boolean;
  deterministic?: boolean;
}

export interface OperatorDesc {
  id: string;
  version: string;
  /** 来自哪个算子包（ADR-0014）。只用于排查，前端不据此改任何行为。 */
  pack?: string;
  aliases?: string[];
  label: string;
  /** 用 / 分层，决定搜索面板的树形结构。 */
  category: string;
  keywords?: string[];
  doc?: string;
  inputs: Port[];
  outputs: Port[];
  params: Param[];
  capabilities?: Capabilities;
}

export interface OperatorManifestBundle {
  schemaVersion: number;
  generatedBy?: string;
  types: PortType[];
  operators: OperatorDesc[];
  /** 注册了的「文本 → 图」导入器（ADR-0017）。没有任何导入器时这一项不出现。 */
  importers?: Importer[];
}

export interface CoreInfo {
  version: string;
  operatorCount: number;
  typeCount: number;
  /** 热重载换了几代。0 = 启动时加载的那一份（ADR-0009）。 */
  generation?: number;
  /** 开发期才是 true：安装包里没有可盯的 CMake 产物。 */
  hotReload?: boolean;
}
