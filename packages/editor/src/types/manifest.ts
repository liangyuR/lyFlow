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

/** Bundle 的一个字段（m8-plan L2）。type 是 types 表里的具体类型，不嵌套 Bundle。 */
export interface BundleField {
  name: string;
  type: string;
  doc?: string;
}

/** 一种 Bundle 的声明（m8-plan L1/L2）。端口类型写作 `Bundle<kind>`，类型检查只认 kind 相等；
 *  结果仓、事件、summary 与图输出按 `<port>.<field>` 寻址。 */
export interface BundleDesc {
  kind: string;
  label?: string;
  doc?: string;
  pack?: string;
  fields: BundleField[];
}

/** `Bundle<k>` → k；不是这种写法返回 null。 */
export function bundleKindOf(type: string): string | null {
  const m = /^Bundle<([^<> ]+)>$/.exec(type);
  return m?.[1] ?? null;
}

/** 端口契约（ADR-0024）。刻意**只有四种键**，不是表达式引擎 —— 与 Condition 同一条
 *  原则：需要更复杂的判断通常说明这个算子该拆了。执行器在输入绑定时检查，
 *  违反报 contract_violation，前端这里只负责把它显示出来。 */
export interface PortContract {
  /** 元素数：点云 = 点数，Indices = 下标个数，Tensor = 元素总数，其余 = 1。eq 与 min/max 二选一。 */
  elementCount?: { eq?: number; min?: number; max?: number };
  /** 只可能是 true —— false 读起来像「允许 NaN」，而那是没有契约。 */
  finite?: true;
  /** 张量形状，-1 = 任意。只对 Tensor 端口有意义。 */
  shape?: number[];
  /** Record 的 type 字串。只对 Record 端口有意义。 */
  recordType?: string;
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
  contract?: PortContract;
  /** 一份样例值（m6-plan H8），任意 JSON。不参与任何校验，只回答「这里长什么样」。 */
  example?: unknown;
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

  /** 语义标记（m8-plan L15），只有编辑器读。roi = vec4f 的 [xMin, yMin, xMax, yMax]，
   *  XY 平面上的一个框，单位按 unit；2D 剖面视图里画成可拖、可拉伸的框。 */
  semantic?: "roi";
  /** 只对 semantic=roi 有意义：框画在 `<dir 参数>/<files 参数的每个文件名>` 拼起来的云上
   *  （那片云自己的坐标系，例如模板坐标系）。不给 = 画在节点显示的那片数据云上。 */
  roiBackdrop?: RoiBackdrop;
}

export interface RoiBackdrop {
  /** 本算子一个 path 参数的名字。 */
  dir: string;
  /** 本算子 string / path 参数的名字。 */
  files?: string[];
}

/** 片段里的一个节点（m8-plan L14）。写法与 GraphDoc 的节点相同，id 只在片段内唯一。 */
export interface SnippetNode {
  id: string;
  op: string;
  params?: Record<string, unknown>;
  ui?: { position?: { x: number; y: number }; title?: string };
}

export interface SnippetPortHint {
  node: string;
  port: string;
  hint?: string;
}

/** 片段（`*.lyflow-snippet.json`，schema/snippet.schema.json）。插入就是带自动连线的粘贴，
 *  插完是普通节点。来源两处：manifest 的 snippets（算子包随附）与宿主扫描的用户目录。 */
export interface SnippetDesc {
  id: string;
  label: string;
  category?: string;
  doc?: string;
  pack?: string;
  nodes: SnippetNode[];
  edges?: { from: { node: string; port: string }; to: { node: string; port: string } }[];
  ports?: { inputs?: SnippetPortHint[]; outputs?: SnippetPortHint[] };
  /** 用户目录里的那些：从哪个文件读来的。 */
  source?: string;
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
  /** Bundle 声明（m8-plan L2）。没有包声明时这一项不出现。 */
  bundles?: BundleDesc[];
  operators: OperatorDesc[];
  /** 注册了的「文本 → 图」导入器（ADR-0017）。没有任何导入器时这一项不出现。 */
  importers?: Importer[];
  /** 算子包随附的片段（m8-plan L14）。没有任何片段时这一项不出现。 */
  snippets?: SnippetDesc[];
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
