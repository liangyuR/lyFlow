// 传输层抽象：让 UI 代码不知道自己跑在 Tauri 里还是浏览器里。
// 浏览器模式用 dump 的静态 manifest，状态栏必须显眼标出当前是哪种 transport。

import type { CoreInfo, OperatorManifestBundle } from "../types/manifest";
import type { GraphDoc } from "../types/graph";
import type {
  CacheStats,
  ExecutionEvent,
  GraphDiagnostic,
  OutputInfo,
  PlanNode,
} from "../types/execution";

export type TransportKind = "tauri" | "static";

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
  runGraph(doc: GraphDoc, graphPath: string | null, targets?: string[]): Promise<string>;
  cancelRun(runId: string): Promise<void>;
  getOutputInfo(runId: string, nodeId: string): Promise<OutputInfo[]>;
  /** 点云走二进制，绝不 JSON（ADR-0006）。 */
  getOutputCloud(
    runId: string,
    nodeId: string,
    port: string,
    maxPoints: number,
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
}

/** 浏览器模式下所有执行相关的入口都走这里。 */
function browserOnly(what: string): never {
  throw new Error(`浏览器模式不能${what} —— 计算在 C++ 里，请在 Tauri 里运行（pnpm dev）`);
}

function inTauri(): boolean {
  return typeof window !== "undefined" && "__TAURI_INTERNALS__" in window;
}

const tauriTransport: Transport = {
  kind: "tauri",
  async getManifest() {
    const { invoke } = await import("@tauri-apps/api/core");
    return invoke<OperatorManifestBundle>("get_manifest");
  },
  async getCoreInfo() {
    const { invoke } = await import("@tauri-apps/api/core");
    return invoke<CoreInfo>("get_core_info");
  },
  async saveGraph(path, doc) {
    const { invoke } = await import("@tauri-apps/api/core");
    return invoke<void>("save_graph", { path, doc });
  },
  async loadGraph(path) {
    const { invoke } = await import("@tauri-apps/api/core");
    return invoke<LoadedGraph>("load_graph", { path });
  },
  async validateGraph(doc, graphPath) {
    const { invoke } = await import("@tauri-apps/api/core");
    return invoke<GraphDiagnostic[]>("validate_graph", { doc, graphPath });
  },
  async planGraph(doc, graphPath, targets) {
    const { invoke } = await import("@tauri-apps/api/core");
    const out = await invoke<PlanNode[] | GraphDiagnostic[]>("plan_graph", {
      doc,
      graphPath,
      targets: targets ?? null,
    });
    // 校验没过时 core 返回的是诊断数组而不是计划数组（ADR-0007）。
    // 两者靠 cacheKey 字段区分：诊断里永远没有它。
    const items = out as { cacheKey?: unknown }[];
    if (items.some((n) => typeof n.cacheKey !== "string")) return [];
    return out as PlanNode[];
  },
  async clearCache() {
    const { invoke } = await import("@tauri-apps/api/core");
    return invoke<void>("clear_cache");
  },
  async cacheStats() {
    const { invoke } = await import("@tauri-apps/api/core");
    return invoke<CacheStats>("cache_stats");
  },
  async runGraph(doc, graphPath, targets) {
    const { invoke } = await import("@tauri-apps/api/core");
    return invoke<string>("run_graph", { doc, graphPath, targets: targets ?? null });
  },
  async cancelRun(runId) {
    const { invoke } = await import("@tauri-apps/api/core");
    return invoke<void>("cancel_run", { runId });
  },
  async getOutputInfo(runId, nodeId) {
    const { invoke } = await import("@tauri-apps/api/core");
    return invoke<OutputInfo[]>("get_output_info", { runId, nodeId });
  },
  async getOutputCloud(runId, nodeId, port, maxPoints) {
    const { invoke } = await import("@tauri-apps/api/core");
    // Tauri 的 ipc::Response 在 JS 侧到手是 ArrayBuffer；某些版本给的是
    // Uint8Array，两种都接住 —— 差别只在一次 .buffer。
    const raw = await invoke<ArrayBuffer | Uint8Array>("get_output_cloud", {
      runId,
      nodeId,
      port,
      maxPoints,
    });
    if (raw instanceof ArrayBuffer) return raw;
    const view = raw as Uint8Array;
    return view.buffer.slice(view.byteOffset, view.byteOffset + view.byteLength) as ArrayBuffer;
  },
  async onExecutionEvent(cb) {
    const { listen } = await import("@tauri-apps/api/event");
    return listen<ExecutionEvent>("execution-event", (e) => cb(e.payload));
  },
  async onManifestUpdated(cb) {
    const { listen } = await import("@tauri-apps/api/event");
    return listen<ManifestUpdated>("manifest-updated", (e) => cb(e.payload));
  },
  async onCoreReloadFailed(cb) {
    const { listen } = await import("@tauri-apps/api/event");
    return listen<CoreReloadFailed>("core-reload-failed", (e) => cb(e.payload));
  },
  async getRecentFiles() {
    const { invoke } = await import("@tauri-apps/api/core");
    return invoke<RecentEntry[]>("get_recent_files");
  },
  async pushRecentFile(path) {
    const { invoke } = await import("@tauri-apps/api/core");
    return invoke<RecentEntry[]>("push_recent_file", { path });
  },
  async writeBackup(path, doc) {
    const { invoke } = await import("@tauri-apps/api/core");
    return invoke<void>("write_backup", { path, doc });
  },
  async backupStatus(path) {
    const { invoke } = await import("@tauri-apps/api/core");
    return invoke<BackupStatus>("backup_status", { path });
  },
  async readBackup(path) {
    const { invoke } = await import("@tauri-apps/api/core");
    return invoke<LoadedGraph>("read_backup", { path });
  },
  async discardBackup(path) {
    const { invoke } = await import("@tauri-apps/api/core");
    return invoke<void>("discard_backup", { path });
  },
};

/** 浏览器模式：读 public/manifest.dev.json，
 *  用 `pnpm --dir app run dump-manifest` 之类的方式刷新它（见 scripts/）。 */
const staticTransport: Transport = {
  kind: "static",
  async getManifest() {
    const res = await fetch("/manifest.dev.json");
    if (!res.ok) {
      throw new Error(
        `读不到 /manifest.dev.json (${res.status})。` +
          `浏览器模式需要先 dump 一份：scripts/dump-manifest.ps1`,
      );
    }
    return (await res.json()) as OperatorManifestBundle;
  },
  async getCoreInfo() {
    const m = await this.getManifest();
    return {
      version: (m.generatedBy ?? "unknown").split("/")[1] ?? "unknown",
      operatorCount: m.operators.length,
      typeCount: m.types.length,
      generation: 0,
      hotReload: false,
    };
  },
  async saveGraph() {
    throw new Error("浏览器模式不能存盘，请在 Tauri 里运行");
  },
  async loadGraph() {
    throw new Error("浏览器模式不能读盘，请在 Tauri 里运行");
  },
  async validateGraph() {
    return browserOnly("做权威校验");
  },
  async planGraph() {
    return browserOnly("编译计划");
  },
  async clearCache() {
    return browserOnly("清缓存");
  },
  async cacheStats() {
    return browserOnly("看缓存统计");
  },
  async runGraph() {
    return browserOnly("运行");
  },
  async cancelRun() {
    return browserOnly("运行");
  },
  async getOutputInfo() {
    return browserOnly("取运行结果");
  },
  async getOutputCloud() {
    return browserOnly("取运行结果");
  },
  async onExecutionEvent() {
    // 这几个不抛：界面启动时无条件订阅，浏览器模式下静默给个空的取消函数
    // 比让整个 App 挂在一个 useEffect 里强。真去点运行才会拿到上面那条错误。
    return () => {};
  },
  async onManifestUpdated() {
    return () => {};
  },
  async onCoreReloadFailed() {
    return () => {};
  },
  async getRecentFiles() {
    return [];
  },
  async pushRecentFile() {
    return [];
  },
  async writeBackup() {
    /* 浏览器模式没有盘可写，静默跳过 —— 定时器不该每 30 秒弹一次错 */
  },
  async backupStatus() {
    return { exists: false, newer: false, backupModified: null, fileModified: null };
  },
  async readBackup() {
    throw new Error("浏览器模式不能读盘，请在 Tauri 里运行");
  },
  async discardBackup() {
    /* 同上 */
  },
};

export const transport: Transport = inTauri() ? tauriTransport : staticTransport;
