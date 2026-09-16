// 传输层抽象：让 UI 代码不知道自己跑在 Tauri 里、浏览器里还是 HTTP 后端上。
// 三个实现（tauri / http / static）各自一个文件，这里只有契约。

import type { CoreInfo, OperatorManifestBundle } from "../types/manifest";
import type { GraphDoc } from "../types/graph";
import type {
  CacheStats,
  ExecutionEvent,
  GraphDiagnostic,
  OutputInfo,
  RunOutputs,
  PlanNode,
} from "../types/execution";

export type TransportKind = "tauri" | "http" | "static";

export type Unlisten = () => void;

/** `load_graph` 的回包。migrations 由前端以 applyMigrations 写回 doc（ADR-0008）。 */
export interface LoadedGraph {
  doc: GraphDoc;
  migrations: GraphDiagnostic[];
}

/** 热重载换代（ADR-0009）。当前 doc 不动，manifest store 整份替换。 */
export interface ManifestUpdated {
  generation: number;
  manifest: OperatorManifestBundle;
  operatorCount: number;
}

export interface CoreReloadFailed {
  problems: string[];
  generation: number;
}

export interface RecentEntry {
  path: string;
  openedAt: number;
}

/** 一次运行的选项。preview 会让源算子先抽稀（ADR-0011）。 */
export interface RunOptions {
  targets?: string[] | undefined;
  mode?: "full" | "preview" | undefined;
  previewMaxPoints?: number | undefined;
  previewBudgetMs?: number | undefined;
  /** 宿主已经加载好的点云会话 id。给了它，后端就把那对云注入
   *  `gap.load_profile_pair` 的端口，图里的路径参数一个字不用改（见
   *  `LyFlowEditorProps.sceneId`）。 */
  sceneId?: string | null | undefined;
}

/** 库算子目录的状态（ADR-0010）。 */
export interface LibraryStatus {
  dirs: string[];
  count: number;
  problems: string[];
}

export interface LibraryRefresh {
  status: LibraryStatus;
  manifest: OperatorManifestBundle;
}

export interface LibraryMeta {
  id: string;
  category?: string;
  keywords?: string[];
  version?: string;
}

/** `<file>~` 备份与正文的比较结果。newer = 上次多半是崩溃退出的。 */
export interface BackupStatus {
  exists: boolean;
  newer: boolean;
  backupModified: number | null;
  fileModified: number | null;
}

export interface Transport {
  readonly kind: TransportKind;
  getManifest(): Promise<OperatorManifestBundle>;
  getCoreInfo(): Promise<CoreInfo>;
  saveGraph(path: string, doc: GraphDoc): Promise<void>;
  loadGraph(path: string): Promise<LoadedGraph>;

  /** 权威校验（C++ 侧），返回全部诊断。 */
  validateGraph(doc: GraphDoc, graphPath: string | null): Promise<GraphDiagnostic[]>;
  /** 编译一次但不执行。stale 标记的唯一权威（ADR-0007）。 */
  planGraph(doc: GraphDoc, graphPath: string | null, targets?: string[]): Promise<PlanNode[]>;
  clearCache(): Promise<void>;
  cacheStats(): Promise<CacheStats>;
  /** 启动一次运行，返回 runId。状态走 `onExecutionEvent`。 */
  runGraph(doc: GraphDoc, graphPath: string | null, options?: RunOptions): Promise<string>;
  cancelRun(runId: string): Promise<void>;
  getOutputInfo(runId: string, nodeId: string): Promise<OutputInfo[]>;
  /** 图级命名输出（ADR-0017）。宿主与验收脚本按名字取值，不认节点 id。 */
  getRunOutputs(runId: string): Promise<RunOutputs>;
  /** 点云走二进制，绝不 JSON（ADR-0006）。 */
  getOutputCloud(
    runId: string,
    nodeId: string,
    port: string,
    maxPoints: number,
  ): Promise<ArrayBuffer>;
  getOutputTensor(
    runId: string,
    nodeId: string,
    port: string,
    offset: number,
    count: number,
  ): Promise<ArrayBuffer>;
  getOutputIndices(
    runId: string,
    nodeId: string,
    port: string,
    offset: number,
    count: number,
  ): Promise<ArrayBuffer>;
  onExecutionEvent(cb: (e: ExecutionEvent) => void): Promise<Unlisten>;
  onManifestUpdated(cb: (e: ManifestUpdated) => void): Promise<Unlisten>;
  onCoreReloadFailed(cb: (e: CoreReloadFailed) => void): Promise<Unlisten>;

  getRecentFiles(): Promise<RecentEntry[]>;
  pushRecentFile(path: string): Promise<RecentEntry[]>;
  writeBackup(path: string, doc: GraphDoc): Promise<void>;
  backupStatus(path: string): Promise<BackupStatus>;
  readBackup(path: string): Promise<LoadedGraph>;
  discardBackup(path: string): Promise<void>;

  /** 写一段二进制到磁盘。3D 视图导出 PNG 用它（M3 尾巴 b）。 */
  writeFileBytes(path: string, contents: Uint8Array): Promise<void>;
  getLibraryStatus(): Promise<LibraryStatus>;
  /** 重扫库目录并拿回新 manifest。 */
  refreshLibrary(): Promise<LibraryRefresh>;
  /** 把 doc 里的一个子图存成库文件。 */
  saveAsLibrary(doc: GraphDoc, subgraphId: string, meta: LibraryMeta): Promise<LibraryStatus>;
  /** 把一份外部配置导入成 GraphDoc（ADR-0017 的 `lyflow_import`）。 */
  importGraph(kind: string, text: string, baseDir: string | null): Promise<GraphDoc>;
}
