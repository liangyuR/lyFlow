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
import type {
  BackupStatus,
  CoreReloadFailed,
  LibraryMeta,
  LibraryRefresh,
  LibraryStatus,
  LoadedGraph,
  ManifestUpdated,
  RecentEntry,
  RunOptions,
  Transport,
  TransportKind,
  Unlisten,
} from "./types";

/** Tauri 壳里的传输：每个方法对应一条 `#[tauri::command]`。 */
export class TauriTransport implements Transport {
  readonly kind: TransportKind = "tauri";

  async getManifest(): Promise<OperatorManifestBundle> {
    const { invoke } = await import("@tauri-apps/api/core");
    return invoke<OperatorManifestBundle>("get_manifest");
  }
  async getCoreInfo(): Promise<CoreInfo> {
    const { invoke } = await import("@tauri-apps/api/core");
    return invoke<CoreInfo>("get_core_info");
  }
  async saveGraph(path: string, doc: GraphDoc): Promise<void> {
    const { invoke } = await import("@tauri-apps/api/core");
    return invoke<void>("save_graph", { path, doc });
  }
  async loadGraph(path: string): Promise<LoadedGraph> {
    const { invoke } = await import("@tauri-apps/api/core");
    return invoke<LoadedGraph>("load_graph", { path });
  }
  async validateGraph(doc: GraphDoc, graphPath: string | null): Promise<GraphDiagnostic[]> {
    const { invoke } = await import("@tauri-apps/api/core");
    return invoke<GraphDiagnostic[]>("validate_graph", { doc, graphPath });
  }
  async planGraph(
    doc: GraphDoc,
    graphPath: string | null,
    targets?: string[],
  ): Promise<PlanNode[]> {
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
  }
  async clearCache(): Promise<void> {
    const { invoke } = await import("@tauri-apps/api/core");
    return invoke<void>("clear_cache");
  }
  async cacheStats(): Promise<CacheStats> {
    const { invoke } = await import("@tauri-apps/api/core");
    return invoke<CacheStats>("cache_stats");
  }
  async runGraph(
    doc: GraphDoc,
    graphPath: string | null,
    options?: RunOptions,
  ): Promise<string> {
    const { invoke } = await import("@tauri-apps/api/core");
    return invoke<string>("run_graph", {
      doc,
      graphPath,
      targets: options?.targets ?? null,
      mode: options?.mode ?? "full",
      previewMaxPoints: options?.previewMaxPoints ?? null,
      previewBudgetMs: options?.previewBudgetMs ?? null,
      sceneId: options?.sceneId ?? null,
    });
  }
  async cancelRun(runId: string): Promise<void> {
    const { invoke } = await import("@tauri-apps/api/core");
    return invoke<void>("cancel_run", { runId });
  }
  async getOutputInfo(runId: string, nodeId: string): Promise<OutputInfo[]> {
    const { invoke } = await import("@tauri-apps/api/core");
    return invoke<OutputInfo[]>("get_output_info", { runId, nodeId });
  }
  async getRunOutputs(runId: string): Promise<RunOutputs> {
    const { invoke } = await import("@tauri-apps/api/core");
    return invoke<RunOutputs>("get_run_outputs", { runId });
  }
  async getOutputCloud(
    runId: string,
    nodeId: string,
    port: string,
    maxPoints: number,
  ): Promise<ArrayBuffer> {
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
  }
  async getOutputTensor(
    runId: string,
    nodeId: string,
    port: string,
    offset: number,
    count: number,
  ): Promise<ArrayBuffer> {
    const { invoke } = await import("@tauri-apps/api/core");
    const raw = await invoke<ArrayBuffer | Uint8Array>("get_output_tensor", {
      runId,
      nodeId,
      port,
      offset,
      count,
    });
    if (raw instanceof ArrayBuffer) return raw;
    const view = raw as Uint8Array;
    return view.buffer.slice(view.byteOffset, view.byteOffset + view.byteLength) as ArrayBuffer;
  }
  async getOutputIndices(
    runId: string,
    nodeId: string,
    port: string,
    offset: number,
    count: number,
  ): Promise<ArrayBuffer> {
    const { invoke } = await import("@tauri-apps/api/core");
    const raw = await invoke<ArrayBuffer | Uint8Array>("get_output_indices", {
      runId,
      nodeId,
      port,
      offset,
      count,
    });
    if (raw instanceof ArrayBuffer) return raw;
    const view = raw as Uint8Array;
    return view.buffer.slice(view.byteOffset, view.byteOffset + view.byteLength) as ArrayBuffer;
  }
  async onExecutionEvent(cb: (e: ExecutionEvent) => void): Promise<Unlisten> {
    const { listen } = await import("@tauri-apps/api/event");
    return listen<ExecutionEvent>("execution-event", (e) => cb(e.payload));
  }
  async onManifestUpdated(cb: (e: ManifestUpdated) => void): Promise<Unlisten> {
    const { listen } = await import("@tauri-apps/api/event");
    return listen<ManifestUpdated>("manifest-updated", (e) => cb(e.payload));
  }
  async onCoreReloadFailed(cb: (e: CoreReloadFailed) => void): Promise<Unlisten> {
    const { listen } = await import("@tauri-apps/api/event");
    return listen<CoreReloadFailed>("core-reload-failed", (e) => cb(e.payload));
  }
  async getRecentFiles(): Promise<RecentEntry[]> {
    const { invoke } = await import("@tauri-apps/api/core");
    return invoke<RecentEntry[]>("get_recent_files");
  }
  async pushRecentFile(path: string): Promise<RecentEntry[]> {
    const { invoke } = await import("@tauri-apps/api/core");
    return invoke<RecentEntry[]>("push_recent_file", { path });
  }
  async writeBackup(path: string, doc: GraphDoc): Promise<void> {
    const { invoke } = await import("@tauri-apps/api/core");
    return invoke<void>("write_backup", { path, doc });
  }
  async backupStatus(path: string): Promise<BackupStatus> {
    const { invoke } = await import("@tauri-apps/api/core");
    return invoke<BackupStatus>("backup_status", { path });
  }
  async readBackup(path: string): Promise<LoadedGraph> {
    const { invoke } = await import("@tauri-apps/api/core");
    return invoke<LoadedGraph>("read_backup", { path });
  }
  async discardBackup(path: string): Promise<void> {
    const { invoke } = await import("@tauri-apps/api/core");
    return invoke<void>("discard_backup", { path });
  }
  async writeFileBytes(path: string, contents: Uint8Array): Promise<void> {
    const { invoke } = await import("@tauri-apps/api/core");
    // Tauri 的 command 参数走 JSON，Uint8Array 要拍成普通数组
    return invoke<void>("write_file_bytes", { path, contents: Array.from(contents) });
  }
  async getLibraryStatus(): Promise<LibraryStatus> {
    const { invoke } = await import("@tauri-apps/api/core");
    return invoke<LibraryStatus>("get_library_status");
  }
  async refreshLibrary(): Promise<LibraryRefresh> {
    const { invoke } = await import("@tauri-apps/api/core");
    return invoke<LibraryRefresh>("refresh_library");
  }
  async saveAsLibrary(
    doc: GraphDoc,
    subgraphId: string,
    meta: LibraryMeta,
  ): Promise<LibraryStatus> {
    const { invoke } = await import("@tauri-apps/api/core");
    return invoke<LibraryStatus>("save_as_library", { doc, subgraphId, meta });
  }
  async importGraph(kind: string, text: string, baseDir: string | null): Promise<GraphDoc> {
    const { invoke } = await import("@tauri-apps/api/core");
    return invoke<GraphDoc>("import_graph", { kind, text, baseDir });
  }
}
